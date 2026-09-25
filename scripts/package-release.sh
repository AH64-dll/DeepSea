#!/usr/bin/env bash
# package-release.sh — stage the Deep Sea (The Wind Waker) Windows release
# bundle with the exact layout the hand-assembled release candidate used.
#
# Runs under Git Bash / MSYS2 on Windows. This script does NOT build; it
# stages finished artifacts:
#
#   <out>/moderngekko-run.exe          runner (--build-dir)
#   <out>/DeepSea.exe                  ImGui launcher (--build-dir)
#   <out>/gGZLE01_recomp.dll           recompiled module (--module), or
#   <out>/disc-builder/                the setup kit that builds it from the
#                                    player's own disc on first launch
#                                    (--builder-kit, from
#                                    scripts/disc_builder/make_kit.py)
#   <out>/libwinpthread-1.dll          + any other non-OS runtime DLLs the
#                                    staged PE files import (objdump -p)
#   <out>/Mods/<id>.mgm/mod.dll        the shipped mods only
#   <out>/Sys/                         Dolphin data tree (from the build dir,
#                                    which POST_BUILD copies from
#                                    vendor/dolphin/Data/Sys)
#   <out>/assets/user-dir/             seed config dir — no saves, no shader
#                                    caches, no NAND dumps, no saved game path
#   <out>/licenses/                    GPLv3 (Deep Sea) + GPLv2+ and the
#                                    third-party license texts from vendored
#                                    Dolphin
#   <out>/SOURCE-OFFER.txt             written offer for complete source
#   <out>/README-PLAYERS.md, README-WINDOWS.txt, Launch-WindWaker.ps1,
#        "Play Deep Sea.cmd"         docs + launchers from packaging/
#   <out>/provenance.json              repo/vendor commits + dirty flags,
#                                    build dir, module hash, build date
#   <out>/SHA256SUMS                   hash of every staged file
#
# The script refuses to stage a bundle containing game data (disc images,
# extracted disc files, savestates, memory cards, NAND titles) or into a
# non-empty output directory without --force.
#
# Usage:
#   scripts/package-release.sh --build-dir <dir> (--module <dll> | --builder-kit <dir>)
#       --out <dir> [--version <str>] [--seed-user-dir <dir>] [--mingw-bin <dir>]
#       [--mods-dir <dir>] [--sys-dir <dir>] [--force] [--dry-run]
#
# Exactly one of --module and --builder-kit: a kit bundle carries no
# translated game code at all.
#
# Env fallbacks: BUILD_DIR MODULE_DLL BUILDER_KIT OUT_DIR VERSION SEED_USER_DIR
# MINGW_BIN MODS_DIR SYS_DIR.  --dry-run validates inputs and prints the plan
# without writing the output directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PACKAGING_DIR="$MG_ROOT/packaging"

BUILD_DIR="${BUILD_DIR:-${RUNNER_DIR:-}}"
MODULE_DLL="${MODULE_DLL:-}"
BUILDER_KIT="${BUILDER_KIT:-}"
OUT_DIR="${OUT_DIR:-}"
VERSION="${VERSION:-}"
SEED_USER_DIR="${SEED_USER_DIR:-$PACKAGING_DIR/user-dir}"
MINGW_BIN="${MINGW_BIN:-}"
MODS_DIR="${MODS_DIR:-}"
SYS_DIR="${SYS_DIR:-}"
MODS="${MODS:-frame60-accum cutscene-skip widescreen16x9}"
DRY_RUN=0
FORCE=0

die()  { echo "package-release: ERROR: $*" >&2; exit 1; }
warn() { echo "package-release: WARN: $*" >&2; }
need() { [ -f "$1" ] || die "missing required file: $1"; }
need_dir() { [ -d "$1" ] || die "missing required directory: $1"; }

usage() { sed -n '2,48p' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)     BUILD_DIR="$2"; shift 2 ;;
    --module)        MODULE_DLL="$2"; shift 2 ;;
    --builder-kit)   BUILDER_KIT="$2"; shift 2 ;;
    --out)           OUT_DIR="$2"; shift 2 ;;
    --version)       VERSION="$2"; shift 2 ;;
    --seed-user-dir) SEED_USER_DIR="$2"; shift 2 ;;
    --mingw-bin)     MINGW_BIN="$2"; shift 2 ;;
    --mods-dir)      MODS_DIR="$2"; shift 2 ;;
    --sys-dir)       SYS_DIR="$2"; shift 2 ;;
    --force)         FORCE=1; shift ;;
    --dry-run)       DRY_RUN=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    *)               die "unknown argument: $1" ;;
  esac
done

[ -n "$BUILD_DIR" ]  || die "--build-dir (or BUILD_DIR) is required"
if [ -n "$MODULE_DLL" ] && [ -n "$BUILDER_KIT" ]; then
  die "--module and --builder-kit are exclusive (a kit bundle ships no game module)"
fi
[ -n "$MODULE_DLL$BUILDER_KIT" ] || die "--module or --builder-kit is required"
[ -n "$OUT_DIR" ]    || die "--out (or OUT_DIR) is required"

# ---- required inputs -------------------------------------------------------
RUNNER_EXE="$BUILD_DIR/moderngekko-run.exe"
LAUNCHER_EXE="$BUILD_DIR/${LAUNCHER_NAME:-DeepSea}.exe"
[ -n "$MODS_DIR" ] || MODS_DIR="$BUILD_DIR/Mods"
[ -n "$SYS_DIR" ]  || SYS_DIR="$BUILD_DIR/Sys"

need "$RUNNER_EXE"
need "$LAUNCHER_EXE"
if [ -n "$MODULE_DLL" ]; then
  need "$MODULE_DLL"
else
  need_dir "$BUILDER_KIT"
  for f in build.py gzle01.json expected-sources.json toolchain-manifest.json \
           bin/dolrecomp.exe python/python.exe toolchain/bin/clang.exe \
           toolchain/bin/x86_64-w64-windows-gnu.cfg module/module_glue.c; do
    need "$BUILDER_KIT/$f"
  done
  # The kit makes game code on the player's PC; it must never carry any.
  # (toolchain/ legitimately holds the compiler's own CRT objects.)
  stray="$(find "$BUILDER_KIT" -path "$BUILDER_KIT/toolchain" -prune -o \( -name generated \
           -o -name objects -o -name '*.o' -o -name '*.obj' -o -name 'gGZLE01_recomp.dll' \
           -o -name '__pycache__' \) -print)"
  [ -z "$stray" ] || die "builder kit contains build output: $stray"
fi
need_dir "$SYS_DIR"
need_dir "$SYS_DIR/GameSettings"
need_dir "$SYS_DIR/GC"
need_dir "$MODS_DIR"
for m in $MODS; do
  need "$MODS_DIR/$m.mgm/mod.dll"
done
need_dir "$SEED_USER_DIR"
need "$SEED_USER_DIR/config.ini"
need "$SEED_USER_DIR/Config/Dolphin.ini"
need "$SEED_USER_DIR/Config/GFX.ini"
need "$SEED_USER_DIR/Config/GCPadNew.ini"
need "$SEED_USER_DIR/Config/Logger.ini"
need "$MG_ROOT/LICENSE"
need "$MG_ROOT/vendor/dolphin/COPYING"
need_dir "$MG_ROOT/vendor/dolphin/LICENSES"
for f in "Launch-WindWaker.ps1" "Play Deep Sea.cmd" "README-WINDOWS.txt" \
         "README-PLAYERS.md"; do
  need "$PACKAGING_DIR/$f"
done

MODULE_NAME="gGZLE01_recomp.dll"
[ -z "$MODULE_DLL" ] || MODULE_NAME="$(basename "$MODULE_DLL")"
if [ "$MODULE_NAME" != "gGZLE01_recomp.dll" ]; then
  warn "module file is '$MODULE_NAME'; Launch-WindWaker.ps1 expects gGZLE01_recomp.dll"
fi

# ---- non-OS runtime DLL detection ------------------------------------------
# The binaries import OS DLLs, the UCRT (api-ms-win-crt-*), and MinGW runtime
# DLLs such as libwinpthread-1.dll. OS-supplied imports must never be bundled
# (and vulkan-1.dll is loaded at runtime, never imported). Anything else is
# resolved from the build dir, then --mingw-bin, then the compiler's bin dir.
WINDOWS_SYS32=""
for w in "${SYSTEMROOT:-}" "${WINDIR:-}"; do
  if [ -n "$w" ]; then
    c="$(cygpath -u "$w" 2>/dev/null || true)"
    [ -n "$c" ] && [ -d "$c/System32" ] && { WINDOWS_SYS32="$c/System32"; break; }
  fi
done

is_os_dll() {
  local n; n="$(printf '%s' "$1" | tr 'A-Z' 'a-z')"
  case "$n" in
    api-ms-win-*.dll|ext-ms-*.dll|vulkan-1.dll) return 0 ;;
  esac
  [ -n "$WINDOWS_SYS32" ] && [ -f "$WINDOWS_SYS32/$1" ] && return 0
  return 1
}

imports_of() {
  # Unique DLL import names of a PE file; empty output if unreadable.
  objdump -p "$1" 2>/dev/null | sed -n 's/^ *DLL Name: //p' | sort -u
}

RUNTIME_DLLS=()
if command -v objdump >/dev/null 2>&1; then
  declare -A seen=()
  for pe in "$RUNNER_EXE" "$LAUNCHER_EXE" ${MODULE_DLL:+"$MODULE_DLL"}; do
    while IFS= read -r dll; do
      [ -n "$dll" ] || continue
      is_os_dll "$dll" && continue
      key="$(printf '%s' "$dll" | tr 'A-Z' 'a-z')"
      [ -n "${seen[$key]:-}" ] || { seen[$key]=1; RUNTIME_DLLS+=("$dll"); }
    done < <(imports_of "$pe")
  done
fi
if [ "${#RUNTIME_DLLS[@]}" -eq 0 ]; then
  # objdump absent or the binaries were not readable as PE (e.g. test
  # fixtures): fall back to the import set verified for the rc binaries.
  warn "objdump import scan empty; using known-required runtime DLL list"
  RUNTIME_DLLS=(libwinpthread-1.dll)
fi
echo "package-release: runtime DLLs to bundle: ${RUNTIME_DLLS[*]}"

resolve_dll() {
  local name="$1" d
  for d in "$BUILD_DIR" "$MINGW_BIN"; do
    [ -n "$d" ] && [ -f "$d/$name" ] && { printf '%s\n' "$d/$name"; return 0; }
  done
  # Compiler toolchain bin/ (llvm-mingw / mingw-w64 ships runtime DLLs there).
  for cxx in "${CXX:-}" x86_64-w64-mingw32-g++ clang++ g++; do
    [ -n "$cxx" ] || continue
    d="$(dirname "$(command -v "$cxx" 2>/dev/null || echo /nonexistent)")"
    [ -f "$d/$name" ] && { printf '%s\n' "$d/$name"; return 0; }
  done
  return 1
}

declare -a RESOLVED_DLLS=()
for dll in "${RUNTIME_DLLS[@]}"; do
  src="$(resolve_dll "$dll")" \
    || die "required runtime DLL '$dll' not found (build dir, --mingw-bin, toolchain bin)"
  RESOLVED_DLLS+=("$src")
done

# ---- dry-run ---------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  echo "package-release: DRY-RUN plan for $OUT_DIR"
  printf '  %s\n' "$RUNNER_EXE" "$LAUNCHER_EXE"
  if [ -n "$MODULE_DLL" ]; then
    printf '  %s\n' "$MODULE_DLL"
  else
    printf '  %s\n' "$BUILDER_KIT/ -> disc-builder/"
  fi
  printf '  %s\n' "${RESOLVED_DLLS[@]}"
  for m in $MODS; do printf '  %s\n' "$MODS_DIR/$m.mgm/mod.dll"; done
  printf '  %s\n' "$SYS_DIR/ -> Sys/" "$SEED_USER_DIR/ -> assets/user-dir/" \
         "packaging/{Launch-WindWaker.ps1,Play Deep Sea.cmd,README-WINDOWS.txt,README-PLAYERS.md}" \
         "licenses/ + SOURCE-OFFER.txt + provenance.json + SHA256SUMS"
  echo "package-release: DRY-RUN OK (nothing written)"
  exit 0
fi

# ---- stage -----------------------------------------------------------------
if [ -d "$OUT_DIR" ] && [ -n "$(find "$OUT_DIR" -mindepth 1 -print -quit 2>/dev/null)" ]; then
  if [ "$FORCE" = 1 ]; then
    rm -rf "$OUT_DIR"
  else
    die "output directory is not empty: $OUT_DIR (pass --force to replace it)"
  fi
fi
mkdir -p "$OUT_DIR/Mods" "$OUT_DIR/assets"

install -m0755 "$RUNNER_EXE"   "$OUT_DIR/"
install -m0755 "$LAUNCHER_EXE" "$OUT_DIR/"
if [ -n "$MODULE_DLL" ]; then
  install -m0644 "$MODULE_DLL" "$OUT_DIR/$MODULE_NAME"
else
  cp -a "$BUILDER_KIT" "$OUT_DIR/disc-builder"
fi
for src in "${RESOLVED_DLLS[@]}"; do
  install -m0644 "$src" "$OUT_DIR/$(basename "$src")"
done

# Mods: copy only mod.dll. The build tree can carry backup objects
# (mod.dll.a3new, mod.dll.o2bak) that must never ship.
for m in $MODS; do
  mkdir -p "$OUT_DIR/Mods/$m.mgm"
  install -m0644 "$MODS_DIR/$m.mgm/mod.dll" "$OUT_DIR/Mods/$m.mgm/mod.dll"
done

# Sys data tree (verbatim from the build dir's POST_BUILD copy).
rm -rf "$OUT_DIR/Sys"
cp -a "$SYS_DIR" "$OUT_DIR/Sys"

# User-dir seed + empty skeleton dirs. Deliberately NOT seeded: Cache/
# (uid/shader caches — machine/driver independence unproven; a stale
# Vulkan-Pipeline-*.cache caused a black-screen incident and the driver disk
# cache now stays off), StateSaves/, Pipes/, logs, memory cards, NAND titles,
# and default-game.txt (would leak a build-machine path).
rm -rf "$OUT_DIR/assets/user-dir"
cp -a "$SEED_USER_DIR" "$OUT_DIR/assets/user-dir"
U="$OUT_DIR/assets/user-dir"
rm -rf "$U/Cache" "$U/StateSaves" "$U/Pipes" "$U/Logs" "$U/ScreenShots" \
       "$U/games" "$U/Setup" "$U/GC" "$U/Dump" "$U/Load" "$U/GameSettings" \
       "$U/Wii/title" "$U/Wii/import" "$U/Wii/tmp"
rm -f "$U/default-game.txt" "$OUT_DIR/default-game.txt"
mkdir -p "$U/Dump/Audio" "$U/Dump/DSP" "$U/Dump/Frames" "$U/Dump/Objects" \
         "$U/Dump/SSL" "$U/Dump/Textures" \
         "$U/Dump/Debug/BranchWatch" "$U/Dump/Debug/JitBlocks" \
         "$U/GameSettings" \
         "$U/Load/DynamicInputTextures" "$U/Load/GraphicMods" \
         "$U/Load/Riivolution" "$U/Load/Textures" "$U/Load/WiiSDSync" \
         "$U/Wii/import" "$U/Wii/meta" "$U/Wii/shared1" "$U/Wii/sys" \
         "$U/Wii/ticket" "$U/Wii/title" "$U/Wii/tmp" "$U/Wii/wfs"

# Launch scripts + player docs (versioned under packaging/).
for f in "Launch-WindWaker.ps1" "Play Deep Sea.cmd" "README-WINDOWS.txt" \
         "README-PLAYERS.md"; do
  install -m0644 "$PACKAGING_DIR/$f" "$OUT_DIR/$f"
done

# ---- licenses + source offer ------------------------------------------------
mkdir -p "$OUT_DIR/licenses"
install -m0644 "$MG_ROOT/LICENSE"                "$OUT_DIR/licenses/LICENSE.DeepSea.txt"
install -m0644 "$MG_ROOT/vendor/dolphin/COPYING" "$OUT_DIR/licenses/COPYING.Dolphin.txt"
cp -a "$MG_ROOT/vendor/dolphin/LICENSES"         "$OUT_DIR/licenses/Dolphin-LICENSES"
for pair in "vendor/dolphin/Externals/licenses.md:Dolphin-third-party-licenses.md" \
            "vendor/dolphin/DolRecomp/LICENSE:LICENSE.DolRecomp.txt" \
            "vendor/dolphin/GXRuntime/LICENSE:LICENSE.GXRuntime.txt"; do
  src="$MG_ROOT/${pair%%:*}"
  if [ -f "$src" ]; then
    install -m0644 "$src" "$OUT_DIR/licenses/${pair##*:}"
  else
    warn "optional license file missing: $src"
  fi
done
# Name of the source tarball scripts/make-source-archive.sh writes for this
# commit; the offer points at it so the binaries and source travel together.
src_short="$(git -C "$MG_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
cat > "$OUT_DIR/SOURCE-OFFER.txt" <<OFFER
Source offer
============

This package contains binaries licensed under the GNU General Public
License v3 (Deep Sea and its ModernGekko runtime, DolRecomp, GXRuntime) and GPLv2-or-later
(vendored Dolphin components). License texts are in the licenses/
directory.

Complete corresponding source code — the ModernGekko runtime, the
vendored Dolphin tree under vendor/dolphin/, the DolRecomp static
recompiler, the shipped code mods, and the build scripts used to produce
these binaries — is distributed alongside this package as
DeepSea-src-${src_short}.tar.gz (written by
scripts/make-source-archive.sh from the commit in provenance.json).

If you received this package without source access, the distributor who
gave it to you will provide the complete corresponding source code on
request for at least three years, at no more than the cost of physically
performing the distribution.
OFFER
if [ -n "$BUILDER_KIT" ]; then
  cat >> "$OUT_DIR/SOURCE-OFFER.txt" <<'OFFER'

The disc-builder/ folder builds the game module on your PC from your own
disc. Its dolrecomp.exe and its scripts are part of the source above
(vendor/dolphin/DolRecomp and scripts/disc_builder/). It also carries
unmodified third-party tools under their own licenses, whose texts are in
disc-builder/licenses/: LLVM, Clang, LLD and the mingw-w64 runtime
(llvm-mingw, https://github.com/mstorsjo/llvm-mingw) and the Windows
embeddable CPython (https://www.python.org/).
OFFER
fi

# ---- legal sweep: no game data ships ---------------------------------------
offenders="$(
  find "$OUT_DIR" -type f \( \
       -iname '*.iso'  -o -iname '*.gcm'  -o -iname '*.rvz'  -o \
       -iname '*.wbfs' -o -iname '*.wbf1' -o -iname '*.wia'  -o \
       -iname '*.ciso' -o -iname '*.wad'  -o -iname '*.nkit.*' -o \
       -iname '*.dol'  -o -iname '*.rel'  -o -iname '*.arc'  -o \
       -iname '*.state' -o -iname '*.sav' -o -iname '*.gci'  -o \
       -iname '*.s[0-9][0-9]' -o -iname '*.raw' -o \
       -iname 'MemoryCard*' \) -print
  # Mutable/QA state that must never ship even when a custom seed carries it.
  find "$OUT_DIR/assets/user-dir" -mindepth 2 -type f \
       \( -path '*/games/*' -o -path '*/Pipes/*' -o -path '*/StateSaves/*' \
       -o -path '*/Cache/*' -o -path '*/Wii/title/*' \) -print 2>/dev/null
  find "$OUT_DIR" -type f -name 'default-game.txt' -print
)"
if [ -n "$offenders" ]; then
  echo "package-release: FAIL — game/save/QA data would be packaged:" >&2
  echo "$offenders" >&2
  exit 1
fi
# Hygiene sweep: logs, crash dumps, QA pipes, backup objects.
find "$OUT_DIR" -type f \( -name '*.dmp' -o -name '*.bak' -o -name '*.log' \
     -o -name '*.orig' -o -name 'crash-*.txt' \) -delete 2>/dev/null || true

# ---- provenance -------------------------------------------------------------
repo_commit="unknown"; repo_dirty=null
vendor_commit="unknown"; vendor_dirty=null
if command -v git >/dev/null 2>&1 \
   && git -C "$MG_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  repo_commit="$(git -C "$MG_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  if [ -z "$(git -C "$MG_ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    repo_dirty="false"
  else
    repo_dirty="true"
  fi
  if git -C "$MG_ROOT/vendor/dolphin" rev-parse HEAD >/dev/null 2>&1; then
    vendor_commit="$(git -C "$MG_ROOT/vendor/dolphin" rev-parse HEAD)"
    if [ -z "$(git -C "$MG_ROOT/vendor/dolphin" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
      vendor_dirty="false"
    else
      vendor_dirty="true"
    fi
  fi
fi
if [ -n "$MODULE_DLL" ]; then
  module_sha="$(sha256sum "$MODULE_DLL" | awk '{print $1}')"
  module_json="{
    \"file\": \"$MODULE_NAME\",
    \"sha256\": \"$module_sha\"
  }"
else
  kit_sha() { sha256sum "$BUILDER_KIT/$1" | awk '{print $1}'; }
  module_json="{
    \"file\": null,
    \"built_on_first_launch\": true,
    \"builder\": {
      \"dolrecomp_sha256\": \"$(kit_sha bin/dolrecomp.exe)\",
      \"toolchain_manifest_sha256\": \"$(kit_sha toolchain-manifest.json)\",
      \"expected_sources_sha256\": \"$(kit_sha expected-sources.json)\"
    }
  }"
fi
build_date="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
version_json="$(printf '%s' "${VERSION:-unversioned}" | tr -d '"\\')"
# Forward-slash path for JSON readability.
build_dir_json="$(printf '%s' "$BUILD_DIR" | tr '\\' '/')"
file_count="$(find "$OUT_DIR" -type f | wc -l | tr -d ' ')"

cat > "$OUT_DIR/provenance.json" <<EOF
{
  "package": "Deep Sea - The Wind Waker (USA) Windows release",
  "version": "$version_json",
  "created": "$build_date",
  "packaged_by": "scripts/package-release.sh",
  "repo": {
    "commit": "$repo_commit",
    "dirty": $repo_dirty
  },
  "vendor_dolphin": {
    "commit": "$vendor_commit",
    "dirty": $vendor_dirty
  },
  "build_dir": "$build_dir_json",
  "module": $module_json,
  "staged_files": $file_count
}
EOF
[ -n "$VERSION" ] && printf '%s\n' "$VERSION" > "$OUT_DIR/VERSION.txt"

# ---- hash manifest -----------------------------------------------------------
# Covers every staged file (including provenance.json), deterministic order.
( cd "$OUT_DIR" && find . -type f ! -name 'SHA256SUMS' -print0 \
    | sort -z | xargs -0 sha256sum > SHA256SUMS )

echo "package-release: staged $file_count files at $OUT_DIR"
echo "package-release: SHA256SUMS written ($(wc -l < "$OUT_DIR/SHA256SUMS" | tr -d ' ') entries)"
echo "package-release: verify with: (cd \"$OUT_DIR\" && sha256sum -c SHA256SUMS)"
