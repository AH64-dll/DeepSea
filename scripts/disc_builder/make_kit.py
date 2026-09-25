#!/usr/bin/env python3
"""Assemble the portable game-setup kit shipped as <bundle>/disc-builder/.

The kit lets a player build gGZLE01_recomp.dll from their own disc, so the
release itself carries no translated game code. Contents:

  build.py + helpers, gzle01.json, expected-sources.json, patches/, module/
  resources/   ModernGekko + GXRuntime headers and the GXRuntime core sources
  bin/         dolrecomp.exe (this repo's vendor/dolphin/DolRecomp)
  python/      Windows embeddable CPython (runs build.py isolated)
  toolchain/   x86_64-only subset of llvm-mingw (clang, lld, headers, CRT)
  pgo/         optional profile from the tested module's training runs
  licenses/    toolchain + Python license texts

usage: make_kit.py --out DIR --dolrecomp EXE --python-embed ZIP
                   --toolchain LLVM_MINGW_DIR [--profile FILE]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

# llvm-mingw pieces a C compile + ThinLTO DLL link for x86_64 needs. Other
# architectures' sysroots, lldb, clangd/tidy, python and docs are left out.
# The .cfg files are clang's default config: without them it targets the
# host's GNU toolchain (ld, libgcc) instead of lld + compiler-rt.
TOOLCHAIN_BIN = ("clang.exe", "clang-22.exe", "ld.lld.exe", "libLLVM-22.dll",
                 "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll",
                 "x86_64-w64-windows-gnu.cfg", "x86_64-pc-windows-gnu.cfg", "mingw32-common.cfg")
TOOLCHAIN_TREES = ("include", "x86_64-w64-mingw32")


def copytree(src: Path, dst: Path, suffixes=None):
    for path in sorted(src.rglob("*")):
        if path.is_dir() or "__pycache__" in path.parts:
            continue
        if suffixes and path.suffix not in suffixes:
            continue
        target = dst / path.relative_to(src)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--dolrecomp", type=Path, required=True)
    ap.add_argument("--python-embed", type=Path, required=True)
    ap.add_argument("--toolchain", type=Path, required=True)
    ap.add_argument("--profile", type=Path)
    a = ap.parse_args()
    out = a.out
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"make_kit: {out} is not empty")
    out.mkdir(parents=True, exist_ok=True)

    # builder sources (no tests, no kit tooling)
    for path in sorted(HERE.rglob("*")):
        rel = path.relative_to(HERE)
        if path.is_dir() or rel.parts[0] in ("tests", "__pycache__") or path.name == "make_kit.py":
            continue
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out / rel)

    res = out / "resources"
    copytree(REPO / "include", res / "include", (".h", ".hpp"))
    copytree(REPO / "vendor/dolphin/GXRuntime/include", res / "vendor/dolphin/GXRuntime/include")
    copytree(REPO / "vendor/dolphin/GXRuntime/src/core", res / "vendor/dolphin/GXRuntime/src/core",
             (".c", ".h"))

    (out / "bin").mkdir()
    shutil.copy2(a.dolrecomp, out / "bin/dolrecomp.exe")

    with zipfile.ZipFile(a.python_embed) as z:
        z.extractall(out / "python")

    tc, dst = a.toolchain, out / "toolchain"
    (dst / "bin").mkdir(parents=True)
    for name in TOOLCHAIN_BIN:
        if not (tc / "bin" / name).is_file():
            raise SystemExit(f"make_kit: {tc / 'bin' / name} is missing")
        shutil.copy2(tc / "bin" / name, dst / "bin" / name)
    for tree in TOOLCHAIN_TREES:
        copytree(tc / tree, dst / tree)
    # clang resource dir: headers + x86_64 compiler-rt only
    for version in sorted((tc / "lib/clang").iterdir()):
        copytree(version / "include", dst / "lib/clang" / version.name / "include")
        for lib in sorted((version / "lib/windows").glob("*x86_64*")):
            target = dst / "lib/clang" / version.name / "lib/windows" / lib.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(lib, target)

    manifest = {p.relative_to(dst).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(dst.rglob("*")) if p.is_file()}
    (out / "toolchain-manifest.json").write_text(json.dumps(manifest, indent=0) + "\n", encoding="utf-8")

    if a.profile:
        (out / "pgo").mkdir()
        shutil.copy2(a.profile, out / "pgo/gzle01.profdata")

    lic = out / "licenses"
    lic.mkdir()
    shutil.copy2(tc / "LICENSE.TXT", lic / "LICENSE.llvm-mingw.txt")
    for name in ("LICENSE.txt", "LICENSE"):
        if (out / "python" / name).is_file():
            shutil.copy2(out / "python" / name, lic / "LICENSE.Python.txt")
    shutil.copy2(REPO / "vendor/dolphin/DolRecomp/LICENSE", lic / "LICENSE.DolRecomp.txt")
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"make_kit: {out} ({size / 2**20:.0f} MiB)")


if __name__ == "__main__":
    sys.exit(main())
