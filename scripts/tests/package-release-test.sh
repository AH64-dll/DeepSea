#!/usr/bin/env bash
# package-release-test.sh — ctest harness for scripts/package-release.sh.
# Builds a synthetic (content-free) build tree and verifies the staged bundle
# layout, manifest, provenance, legal sweep, and failure modes. Needs no game
# data and no real binaries: dummy PE-unreadable files exercise the objdump
# fallback path.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/package-release.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "package-release-test: FAIL: $*" >&2; exit 1; }
expect_file() { [ -f "$1" ] || fail "missing staged file: $1"; }

# ---- synthetic build tree ----------------------------------------------------
B="$TMP/build"
mkdir -p "$B/Mods/frame60-accum.mgm" "$B/Mods/cutscene-skip.mgm" \
         "$B/Mods/widescreen16x9.mgm" "$B/Sys/GameSettings" "$B/Sys/GC" \
         "$TMP/module"
echo "fake runner" > "$B/moderngekko-run.exe"
echo "fake launcher" > "$B/DeepSea.exe"
echo "fake pthread" > "$B/libwinpthread-1.dll"
for m in frame60-accum cutscene-skip widescreen16x9; do
  echo "fake $m" > "$B/Mods/$m.mgm/mod.dll"
done
# Stray backup object that must never be staged.
echo "stale" > "$B/Mods/frame60-accum.mgm/mod.dll.o2bak"
echo "[Video] EFBToTextureEnable = False" > "$B/Sys/GameSettings/GZLE01.ini"
echo "rom" > "$B/Sys/GC/dsp_rom.bin"
echo "ch"  > "$B/Sys/codehandler.bin"
echo "fake module" > "$TMP/module/gGZLE01_recomp.dll"

# ---- dry-run ------------------------------------------------------------------
"$SCRIPT" --build-dir "$B" --module "$TMP/module/gGZLE01_recomp.dll" \
          --out "$TMP/out" --version 0.0.0-test --dry-run > "$TMP/dry.log" \
  || fail "--dry-run exited non-zero"
grep -q "DRY-RUN OK" "$TMP/dry.log" || fail "dry-run did not report plan"
[ ! -e "$TMP/out" ] || fail "--dry-run wrote the output directory"

# ---- real staging ---------------------------------------------------------------
"$SCRIPT" --build-dir "$B" --module "$TMP/module/gGZLE01_recomp.dll" \
          --out "$TMP/out" --version 0.0.0-test > "$TMP/stage.log" \
  || fail "staging exited non-zero: $(cat "$TMP/stage.log")"

O="$TMP/out"
expect_file "$O/moderngekko-run.exe"
expect_file "$O/DeepSea.exe"
expect_file "$O/gGZLE01_recomp.dll"
expect_file "$O/libwinpthread-1.dll"
for m in frame60-accum cutscene-skip widescreen16x9; do
  expect_file "$O/Mods/$m.mgm/mod.dll"
done
[ ! -e "$O/Mods/frame60-accum.mgm/mod.dll.o2bak" ] \
  || fail "stray backup object was staged"
expect_file "$O/Sys/GameSettings/GZLE01.ini"
expect_file "$O/Sys/codehandler.bin"
expect_file "$O/assets/user-dir/config.ini"
expect_file "$O/assets/user-dir/Config/Dolphin.ini"
expect_file "$O/assets/user-dir/Config/GCPadNew.ini"
expect_file "$O/assets/user-dir/Config/Logger.ini"
expect_file "$O/assets/user-dir/Wii/fst.bin"
expect_file "$O/assets/user-dir/Wii/shared2/sys/SYSCONF"
[ -d "$O/assets/user-dir/Load/Textures" ] || fail "Load/Textures skeleton missing"
[ -d "$O/assets/user-dir/GameSettings" ] || fail "GameSettings skeleton missing"
[ ! -e "$O/assets/user-dir/Cache" ] \
  || fail "shader/uid caches must not ship in the seed"
expect_file "$O/Launch-WindWaker.ps1"
expect_file "$O/Play Deep Sea.cmd"
expect_file "$O/README-WINDOWS.txt"
expect_file "$O/README-PLAYERS.md"
expect_file "$O/licenses/LICENSE.DeepSea.txt"
expect_file "$O/licenses/COPYING.Dolphin.txt"
expect_file "$O/licenses/Dolphin-LICENSES/GPL-2.0-or-later.txt"
expect_file "$O/SOURCE-OFFER.txt"
grep -q "DeepSea-src-[0-9a-f]\{12\}\.tar\.gz" "$O/SOURCE-OFFER.txt" \
  || fail "SOURCE-OFFER.txt does not name the source tarball"
expect_file "$O/provenance.json"
expect_file "$O/SHA256SUMS"
expect_file "$O/VERSION.txt"

# Quiet release logging stance: file only, no console/window spam.
grep -q "WriteToFile = True"    "$O/assets/user-dir/Config/Logger.ini" \
  || fail "Logger.ini lost WriteToFile (crash diagnostics)"
grep -q "WriteToConsole = False" "$O/assets/user-dir/Config/Logger.ini" \
  || fail "Logger.ini still spams the console"
grep -q "OSREPORT_HLE = True"   "$O/assets/user-dir/Config/Logger.ini" \
  || fail "Logger.ini lost OSREPORT_HLE (panic text channel)"

# Manifest must verify and must cover provenance.json.
( cd "$O" && sha256sum -c SHA256SUMS >/dev/null ) \
  || fail "SHA256SUMS does not verify against the staged tree"
grep -q "provenance.json" "$O/SHA256SUMS" \
  || fail "provenance.json not covered by SHA256SUMS"

# Provenance fields.
mod_sha="$(sha256sum "$TMP/module/gGZLE01_recomp.dll" | awk '{print $1}')"
grep -q "\"$mod_sha\"" "$O/provenance.json" \
  || fail "provenance.json module sha256 mismatch"
grep -q '"commit"' "$O/provenance.json" || fail "provenance.json missing commit"
grep -q '"dirty"'  "$O/provenance.json" || fail "provenance.json missing dirty flag"

# ---- negative: game data must fail loudly --------------------------------------
mkdir -p "$B/Sys/ISO"
echo "not really a disc" > "$B/Sys/ISO/fake.iso"
if "$SCRIPT" --build-dir "$B" --module "$TMP/module/gGZLE01_recomp.dll" \
            --out "$TMP/out2" > "$TMP/bad.log" 2>&1; then
  fail "legal sweep let an .iso through"
fi
grep -q "fake.iso" "$TMP/bad.log" || fail "sweep did not name the offender"
rm -rf "$B/Sys/ISO"

# Negative: non-empty output dir without --force.
mkdir -p "$TMP/out3"
touch "$TMP/out3/stale"
if "$SCRIPT" --build-dir "$B" --module "$TMP/module/gGZLE01_recomp.dll" \
            --out "$TMP/out3" > "$TMP/nonempty.log" 2>&1; then
  fail "staging into a non-empty dir should refuse without --force"
fi
"$SCRIPT" --build-dir "$B" --module "$TMP/module/gGZLE01_recomp.dll" \
          --out "$TMP/out3" --force > /dev/null \
  || fail "--force staging failed"
[ ! -e "$TMP/out3/stale" ] || fail "--force did not replace the stale tree"

# ---- builder-kit bundle: no game module ships ------------------------------------
K="$TMP/kit"
mkdir -p "$K/bin" "$K/python" "$K/toolchain/bin" "$K/module" "$K/licenses"
for f in build.py gzle01.json expected-sources.json toolchain-manifest.json \
         bin/dolrecomp.exe python/python.exe toolchain/bin/clang.exe \
         toolchain/bin/x86_64-w64-windows-gnu.cfg module/module_glue.c \
         licenses/LICENSE.llvm-mingw.txt; do
  echo "fake $f" > "$K/$f"
done
"$SCRIPT" --build-dir "$B" --builder-kit "$K" --out "$TMP/kitout" \
          --version 0.0.0-kit > "$TMP/kit.log" \
  || fail "kit staging exited non-zero: $(cat "$TMP/kit.log")"
KO="$TMP/kitout"
expect_file "$KO/disc-builder/build.py"
expect_file "$KO/disc-builder/toolchain/bin/x86_64-w64-windows-gnu.cfg"
expect_file "$KO/moderngekko-run.exe"
[ ! -e "$KO/gGZLE01_recomp.dll" ] || fail "kit bundle staged a game module"
grep -q '"built_on_first_launch": true' "$KO/provenance.json" \
  || fail "kit provenance does not record the first-launch build"
kit_sha="$(sha256sum "$K/expected-sources.json" | awk '{print $1}')"
grep -q "\"$kit_sha\"" "$KO/provenance.json" \
  || fail "kit provenance lacks the expected-sources hash"
grep -q "llvm-mingw" "$KO/SOURCE-OFFER.txt" \
  || fail "SOURCE-OFFER.txt does not cover the kit's third-party tools"
( cd "$KO" && sha256sum -c SHA256SUMS >/dev/null ) \
  || fail "kit SHA256SUMS does not verify"

# Negative: module and kit together defeat the kit's purpose.
if "$SCRIPT" --build-dir "$B" --module "$TMP/module/gGZLE01_recomp.dll" \
            --builder-kit "$K" --out "$TMP/kitout2" > "$TMP/both.log" 2>&1; then
  fail "--module with --builder-kit should refuse"
fi
# Negative: a kit that still holds build output (translated code, objects).
mkdir -p "$K/objects"; echo "x" > "$K/objects/0000.o"
if "$SCRIPT" --build-dir "$B" --builder-kit "$K" --out "$TMP/kitout3" \
            > "$TMP/dirtykit.log" 2>&1; then
  fail "kit with build output was staged"
fi
grep -q "0000.o" "$TMP/dirtykit.log" || fail "dirty-kit check did not name the offender"

echo "package-release-test: PASS"
