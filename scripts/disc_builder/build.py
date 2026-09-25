#!/usr/bin/env python3
"""Build the GZLE01 module entirely from a player's validated extracted disc.

No generated game code, disc bytes, prebuilt game objects or game DLL are inputs.
Run with --help. The same entry point is shipped in the portable builder kit.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
# The embedded interpreter runs isolated and does not add the script directory.
sys.path.insert(0, str(HERE))
from archives import rarc_files, yaz0
from patching import apply_unified_patch
import relcall
import source_manifest
from windows_job import contain_children

MODULE_NAME = "gGZLE01_recomp.dll"
INTERMEDIATES = ("dol", "rel", "tables", "inputs", "main.dol", "objects", "objects.rsp")


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


class Builder:
    def __init__(self, args):
        self.args = args
        self.started = time.monotonic()
        self.work = None
        self.spec = json.loads((HERE / "gzle01.json").read_text())
        # A packaged kit carries these source-only resources beside this script.
        self.repo = HERE.parents[1]
        self.resources = HERE / "resources" if (HERE / "resources").is_dir() else self.repo
        self.cc = self.tool(args.cc, HERE / "toolchain/bin/clang.exe", "clang")
        self.recompiler = self.tool(args.dolrecomp, HERE / "bin/dolrecomp.exe", "dolrecomp")
        profile = Path(args.profile) if args.profile else HERE / "pgo/gzle01.profdata"
        self.profile = profile.resolve() if profile.is_file() else None
        self.env = dict(os.environ)
        # A user's benchmark environment must not silently change code generation.
        for name in list(self.env):
            if name.startswith("DOLRECOMP_") or name == "MODERNGEKKO_INLINE_XLAT_FULL":
                del self.env[name]
        self.env.update({"DOLRECOMP_C_CHUNK_INSTRUCTIONS": "4096",
                         "DOLRECOMP_HOST_HOOKS": ",".join(self.spec["host_hooks"]),
                         "MODERNGEKKO_INLINE_XLAT_FULL": "1",
                         "PYTHONUTF8": "1"})
        self.env["PATH"] = str(self.cc.parent) + os.pathsep + self.env.get("PATH", "")

    @staticmethod
    def tool(option, bundled, name):
        found = option or (str(bundled) if bundled.is_file() else shutil.which(name))
        if not found or not Path(found).is_file():
            raise ValueError(f"Build tool missing: {name}. Install the complete builder kit.")
        return Path(found).resolve()

    def progress(self, stage, completed=0, total=1, **extra):
        state = dict(stage=stage, completed=completed, total=total,
                     elapsed_seconds=round(time.monotonic() - self.started, 1), **extra)
        print(json.dumps(state), flush=True)
        if self.args.status:
            write_json(self.args.status, state)

    def run(self, command, name, cwd=None, extra_env=None):
        log = self.work / (name + ".log")
        env = dict(self.env, **(extra_env or {}))
        with log.open("wb") as stream:
            result = subprocess.run([str(v) for v in command], cwd=cwd, env=env,
                                    stdout=stream, stderr=subprocess.STDOUT,
                                    creationflags=0x08000000 if os.name == "nt" else 0)
        if result.returncode:
            tail = log.read_text(encoding="utf-8", errors="replace")[-3000:]
            raise RuntimeError(f"{name} failed (exit {result.returncode}). See {log}\n{tail}")
        return log

    def validate(self):
        root = self.args.game_root.resolve()
        self.progress("Checking your game")
        for relative, expected in self.spec["inputs"].items():
            path = root / relative
            if not path.is_file() or digest(path) != expected:
                raise ValueError(f"Unsupported or damaged game: {relative} does not match "
                                 "the supported USA GZLE01 revision. Supply a clean disc dump.")
        return root

    def recipe(self):
        inputs = {}
        for path in sorted(HERE.rglob("*")):
            if path.is_file() and path.suffix in (".py", ".c", ".h", ".json", ".def", ".patch") and not any(
                    x in path.parts for x in ("resources", "toolchain", "python", "tests", "__pycache__")):
                inputs[path.relative_to(HERE).as_posix()] = digest(path)
        for relative in ("include/moderngekko", "vendor/dolphin/GXRuntime/include",
                         "vendor/dolphin/GXRuntime/src/core"):
            for path in sorted((self.resources / relative).rglob("*")):
                if path.is_file() and path.suffix in (".h", ".c"):
                    inputs[path.relative_to(self.resources).as_posix()] = digest(path)
        inputs["dolrecomp.exe"] = digest(self.recompiler)
        if self.profile:
            inputs["profile.profdata"] = digest(self.profile)
        inputs["compiler.exe"] = digest(self.cc)
        for path in sorted(self.cc.parent.glob("clang-*.exe")):
            inputs[path.name] = digest(path)
        toolchain_manifest = HERE / "toolchain-manifest.json"
        if toolchain_manifest.is_file():
            inputs["toolchain-manifest.json"] = digest(toolchain_manifest)
        inputs["native-cpu"] = self.native_cpu()
        return inputs

    def native_cpu(self):
        # The module is built with -march=native, so a user dir copied to a PC
        # with a different CPU must not reuse it (it may hold instructions that
        # CPU lacks). Key the cache by what -march=native resolves to here.
        result = subprocess.run([str(self.cc), "-march=native", "-###", "-x", "c", "-c", os.devnull,
                                 "-o", os.devnull], env=self.env, capture_output=True, text=True,
                                creationflags=0x08000000 if os.name == "nt" else 0)
        target = re.findall(r'"-target-(?:cpu|feature)" "([^"]+)"', result.stderr)
        if not target:
            raise RuntimeError("Could not identify this PC's processor with the bundled compiler.\n"
                               + result.stderr[-2000:])
        return hashlib.sha256(" ".join(target).encode()).hexdigest()

    def generate(self, root):
        self.progress("Preparing game modules")
        rels = self.work / "inputs"
        rels.mkdir()
        def checked_bytes(name):
            data = (root / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != self.spec["inputs"][name]:
                raise ValueError(f"Game changed during setup: {name}. Retry with a clean dump.")
            return data

        dol_input = self.work / "main.dol"
        dol_input.write_bytes(checked_bytes("sys/main.dol"))
        for name in self.spec["inputs"]:
            if name.endswith(".rel"):
                target = rels / "rels" / Path(name).name
                target.parent.mkdir(exist_ok=True)
                target.write_bytes(yaz0(checked_bytes(name)))
        for name, content in rarc_files(checked_bytes("files/RELS.arc")):
            if not name.endswith(".rel"):
                raise ValueError(f"Unexpected REL archive entry: {name}")
            target = rels / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                raise ValueError(f"Duplicate game module: {name}")
            target.write_bytes(content)
        if len(list(rels.rglob("*.rel"))) != 415:
            raise ValueError("Expected 415 game modules after extraction")
        self.progress("Translating game code")
        # gpr save/restore stubs live in the DOL; REL calls into them stay
        # cross-module dispatches, exactly like the tested module.
        self.run([self.recompiler, "--gamecube", f"-j{self.args.jobs}",
                  dol_input, self.work / "dol"], "generate-dol",
                 extra_env={"DOLRECOMP_GPR_STUBS": self.spec["dol_gpr_stubs"]})
        # A1 batch dispatch + its iteration cap live in the generated header.
        apply_unified_patch(self.work / "dol/generated/generated.h",
                            HERE / "patches/dol-generated-h.patch")
        self.progress("Translating game modules")
        log = self.run([self.recompiler, "--gamecube", f"-j{self.args.jobs}",
                       "--rel-base", self.spec["rel_base"], "--l-base-offset",
                       self.spec["l_base_offset"], "inputs", "rel"], "generate-rel", self.work)
        self.progress("Optimizing game module calls")
        relcall.transform_tree(self.work / "rel/generated/rels")
        rows = re.findall(r"^\s*module \d+: .+? -> base 0x[0-9A-Fa-f]+", log.read_text(), re.M)
        if len(rows) != 415:
            raise ValueError("Recompiler did not report all 415 module addresses")
        basemap = self.work / "basemap.txt"
        basemap.write_text("\n".join(row.strip() for row in rows) + "\n")
        self.progress("Building module tables")
        tables = self.work / "tables"
        tables.mkdir()
        self.run([sys.executable, HERE / "gen_module_tables.py", self.work / "dol/generated/generated.h",
                  self.work / "dol/generated/generated_smc.txt", dol_input,
                  tables / "module_tables.inc"], "dol-tables")
        self.run([sys.executable, HERE / "gen_rel_descriptor.py", "--basemap", basemap,
                  "--rels-dir", self.work / "rel/generated/rels", "--disc-root", self.work,
                  "--batch-log", log, "--out-h", tables / "rel-modules-data.h",
                  "--out-inc", tables / "rel-modules-data.inc", "--l-base-rebase", "0x91100000"], "rel-tables")
        self.run([sys.executable, HERE / "gen_rel_loader_chunks.py", "--descriptor",
                  tables / "rel-modules-data.inc", "--tree", self.work / "rel/generated/rels",
                  "--out", tables / "rel_loader_chunks.h"], "chunk-tables")
        self.run([sys.executable, HERE / "gen_rel_band_table.py", "--descriptor",
                  tables / "rel-modules-data.inc", "--disc-root", self.work,
                  "--out", tables / "rel_bands.inc"], "band-tables")
        # Refuse to compile anything but the tested code: every generated file
        # must hash-match the manifest recorded from the tested build.
        manifest = HERE / "expected-sources.json"
        if manifest.is_file():
            self.progress("Verifying translated code")
            bad = source_manifest.verify(self.work, manifest)
            if bad:
                raise RuntimeError(
                    f"Translated game code differs from the tested build in {len(bad)} file(s), "
                    f"first: {bad[0]}. The setup kit may be damaged; reinstall it.")

    def compile(self):
        runtime = self.resources / "vendor/dolphin/GXRuntime"
        sources = sorted((self.work / "dol/generated").rglob("*.c"))
        sources += sorted((self.work / "rel/generated/rels").rglob("*.c"))
        sources += [runtime / "src/core" / (name + ".c") for name in (
            "cpu", "cpu_exception", "cpu_interpreter", "cpu_interpreter_float",
            "cpu_interpreter_integer", "cpu_interpreter_table")]
        sources += [HERE / "module/module_glue.c", HERE / "module/rel_loader.c"]
        objects = self.work / "objects"
        objects.mkdir()
        # -march=native instead of a fixed x86-64-v3 baseline: the DLL never
        # leaves this PC, and a fixed AVX2 baseline would fault on older CPUs.
        # -ffp-contract=off keeps float results bit-exact with the guest
        # across -march choices.
        flags = ["-march=native", "-DNDEBUG", "-DWIN32_LEAN_AND_MEAN",
                 "-DNOMINMAX", '-DDOLRECOMP_CPU_HEADER="core/cpu.h"',
                 "-DDOLRECOMP_ENABLE_REPLACEMENTS", "-DMODERNGEKKO_INLINE_XLAT_FULL=1",
                 "-ffp-contract=off", "-fno-fast-math",
                 "-include", str(HERE / "module/cycle_budget.h")]
        if self.profile:
            # Profile from the tested module's training runs. Functions whose
            # source name differs (file-static helpers are keyed by path) just
            # compile without it.
            flags += ["-fprofile-use=" + str(self.profile), "-Wno-profile-instr-unprofiled",
                      "-Wno-profile-instr-out-of-date", "-Wno-backend-plugin"]
        for directory in (self.resources / "include", runtime / "include",
                          self.work / "dol/generated", self.work / "tables"):
            flags += ["-I", str(directory)]
        # Optimization tiers, so setup takes minutes rather than hours:
        #  - the engine (main.dol), the runtime and the actor files the profile
        #    saw run: -O2 + PGO. GVN's memory-dependence scan is off: on these
        #    one-function-per-4096-instructions files it was two thirds of all
        #    compile time for no measurable speed.
        #  - every other actor file (enemies, bosses and props of places the
        #    profiled play never visited, two thirds of the code): -O1 when it
        #    is small, -O0 when it is one of the ~330 large ones. -O1 cost
        #    grows much faster than linearly with a file's giant functions:
        #    +0.4 s on a 400 KB file, +6 s on a 1.3 MB one, where -O0 takes
        #    one to two seconds. Frame cost is the engine's: with every actor
        #    file at -O0 (the ship included) sailing measured the same
        #    emulation time per field as the all -O2 + ThinLTO module, while
        #    the engine at -O0 was four times slower.
        # No ThinLTO: its link step re-optimized the whole game for most of an
        # hour; generated code calls across files through the dispatcher, so
        # cross-file inlining bought nothing measurable.
        full = ["-O2", "-mllvm", "-enable-gvn-memdep=false"]
        small = ["-O1"]
        large = ["-O0"]
        hot = set(json.loads((HERE / "hot-sources.json").read_text())["optimize"])
        rels = self.work / "rel/generated/rels"

        def tier(source):
            if rels in source.parents and source.relative_to(self.work).as_posix() not in hot:
                return small if source.stat().st_size < 512 * 1024 else large
            return full

        def compile_one(index, source):
            output = objects / f"{index:04d}.o"
            command = [self.cc, *tier(source), *flags, "-c", source, "-o", output]
            for attempt in range(3):
                try:
                    self.run(command, f"compile-{index:04d}")
                    return output
                except RuntimeError:
                    # Real-time antivirus scanning the object clang just
                    # wrote can make its final rename fail; try again.
                    if attempt == 2:
                        raise
                    time.sleep(1 + 2 * attempt)

        # Largest -O2 files first so the end of the run stays parallel.
        order = sorted(enumerate(sources), key=lambda item: -item[1].stat().st_size *
                       (10 if tier(item[1]) is full else 1))
        with ThreadPoolExecutor(max_workers=self.args.jobs) as pool:
            futures = [pool.submit(compile_one, i, source) for i, source in order]
            try:
                for count, future in enumerate(as_completed(futures), 1):
                    future.result()
                    self.progress("Compiling game", count, len(sources))
            except Exception:
                for future in futures:
                    future.cancel()
                raise
        self.progress("Finishing game setup")
        response = self.work / "objects.rsp"
        response.write_text("\n".join('"' + p.as_posix() + '"' for p in sorted(objects.glob("*.o"))))
        module = self.work / MODULE_NAME
        self.progress("Linking the game")
        self.run([self.cc, "-shared", "-fuse-ld=lld", "-o", module, "@" + str(response),
                  "-march=native", "-Wl,--no-insert-timestamp", "-Wl,--exclude-all-symbols", "-static", "-lwinpthread"], "link")
        if not module.is_file() or module.stat().st_size < 1024 * 1024:
            raise RuntimeError("Compiler did not produce a complete game module")
        return module

    def build(self):
        root = self.validate()
        cache = self.args.output.resolve() / "GZLE01"
        cache.mkdir(parents=True, exist_ok=True)
        # OS-owned locking survives neither crashes nor cancellations, so a
        # stale lock file never blocks the next launch. Serialize publication.
        with (cache / "setup.lock").open("a+b") as lock:
            if os.name == "nt":
                import msvcrt
                lock.seek(0, 2)
                if lock.tell() == 0:
                    lock.write(b"\0")
                    lock.flush()
                waiting = False
                while True:
                    lock.seek(0)
                    try:
                        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                        break
                    except OSError:
                        if not waiting:
                            self.progress("Waiting for another game setup to finish")
                            waiting = True
                        time.sleep(0.25)
            self.build_locked(root, cache)

    def build_locked(self, root, cache):
        recipe = self.recipe()
        key = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
        artifact = cache / key
        pointer = cache / (key + ".json")
        if pointer.is_file():
            try:
                candidate = (cache / json.loads(pointer.read_text())["directory"]).resolve()
                if candidate.parent == cache:
                    artifact = candidate
            except (OSError, ValueError, KeyError):
                pass
        receipt = artifact / "build.json"
        module = artifact / MODULE_NAME
        # Never trust a leftover DLL or a partially completed directory.
        if receipt.is_file() and module.is_file() and not self.args.rebuild:
            try:
                saved = json.loads(receipt.read_text())
            except (OSError, ValueError):
                saved = {}
            if saved.get("recipe") == recipe and saved.get("module_sha256") == digest(module):
                self.publish(cache, module, True)
                return
        # A failed attempt's directory is kept for its logs until the next
        # attempt starts; a completed build (it has a receipt) may be active.
        for stale in cache.glob("build-*"):
            if stale.is_dir() and not (stale / "build.json").is_file():
                shutil.rmtree(stale, ignore_errors=True)
        self.work = Path(tempfile.mkdtemp(prefix="build-", dir=cache))
        self.progress("Starting first-time setup", log_directory=str(self.work))
        self.generate(root)
        built = self.compile()
        # Translated sources and objects are only needed to build (close to a
        # gigabyte); keep the module, the logs and the receipt.
        for name in INTERMEDIATES:
            path = self.work / name
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
        for log in self.work.glob("compile-*.log"):
            if log.stat().st_size == 0:
                log.unlink()
        write_json(self.work / "build.json", dict(recipe=recipe, module_sha256=digest(built),
                   input_sha256=self.spec["inputs"], elapsed_seconds=time.monotonic() - self.started))
        # Fresh directory per attempt. Keep diagnostics after errors; don't overwrite a
        # known working DLL while it might be loaded by a running game.
        if artifact.exists():
            artifact = self.work
        else:
            self.work.rename(artifact)
        self.work = artifact
        write_json(pointer, {"directory": artifact.name})
        self.publish(cache, artifact / MODULE_NAME, False)

    def publish(self, cache, module, hit):
        active = cache / "active-module.txt"
        temporary = active.with_suffix(".tmp")
        temporary.write_text(str(module) + "\n", encoding="utf-8")
        os.replace(temporary, active)
        self.progress("Ready to play", 1, 1, module=str(module), cache_hit=hit, finished=True)


def default_jobs():
    """Every logical CPU, at most one compiler per 1.5 GB of installed memory
    (the largest translated files peak near 450 MB in clang; most need far
    less). Installed rather than free memory: free memory swings with
    whatever else is open, and halving the jobs doubles the wait."""
    jobs = os.cpu_count() or 2
    if os.name == "nt":
        import ctypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32),
                        ("total", ctypes.c_uint64), ("available", ctypes.c_uint64),
                        ("total_page", ctypes.c_uint64), ("available_page", ctypes.c_uint64),
                        ("total_virtual", ctypes.c_uint64), ("available_virtual", ctypes.c_uint64),
                        ("available_extended", ctypes.c_uint64)]
        status = MemoryStatus(length=ctypes.sizeof(MemoryStatus))
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            jobs = min(jobs, max(2, int(status.total / (1.5 * 2**30))))
    return max(1, min(jobs, 64))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_root", type=Path)
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("LOCALAPPDATA", ".")) / "ModernGekko/modules")
    parser.add_argument("--dolrecomp", help="Path to the matching DolRecomp executable")
    parser.add_argument("--cc", help="Path to LLVM-MinGW clang.exe")
    parser.add_argument("--profile", help="PGO profile (default: pgo/gzle01.profdata in the kit)")
    parser.add_argument("--jobs", type=int, default=default_jobs())
    parser.add_argument("--status", type=Path, help="Atomic JSON progress file for the launcher")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error("--jobs must be between 1 and 64")
    try:
        contain_children()
        Builder(args).build()
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        state = dict(stage="Setup failed", error=str(exc), finished=True)
        if args.status:
            write_json(args.status, state)
        print(json.dumps(state), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
