#!/usr/bin/env bash
# rebuild_module.sh — portable gGZLE01_recomp.so builder (this machine).
# Reconstructs the .inline recipe verified by object comparison 2026-09-11:
#   DOL chunks : ZeldaDecompile/port/build/dol-inline/generated  (plain mem_*)
#   REL chunks : ZeldaDecompile/port/build/rel-out-p14/generated/rels
#   GXRuntime  : <repo>/vendor/dolphin/GXRuntime/src  (per-worktree)
#
# Usage:
#   rebuild_module.sh --out PATH --objdir DIR [--gxruntime DIR]
#                     [--dol-gen DIR] [--rel-gen DIR] [-j N]
#                     [--extra-flags "..."] [--mg-root DIR]
#
# Agents MUST pass --gxruntime pointing at THEIR worktree's vendor/dolphin
# and a private --objdir. Never write to port/game/gGZLE01_recomp.so* pins.
set -u
cd "$(dirname "$0")/.." || exit 1
MG_REPO="$(pwd)"
ZELDA="/run/media/amr/New Volume/zelda"
PORT="$ZELDA/ZeldaDecompile/port"

OUT=""; OBJDIR=""
GXRUNTIME_SRC="$MG_REPO/vendor/dolphin/GXRuntime/src"
DOL_GEN="$PORT/build/dol-inline/generated"
REL_GEN="$PORT/build/rel-out-p14/generated/rels"
GLUE="$PORT/game/src/module_glue.c"
LOADER="$PORT/loader/rel_loader.c"
EXTRA_FLAGS=""
JOBS=8

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2;;
    --objdir) OBJDIR="$2"; shift 2;;
    --gxruntime) GXRUNTIME_SRC="$2"; shift 2;;
    --dol-gen) DOL_GEN="$2"; shift 2;;
    --rel-gen) REL_GEN="$2"; shift 2;;
    --glue) GLUE="$2"; shift 2;;
    --loader) LOADER="$2"; shift 2;;
    --mg-repo) MG_REPO="$2"; GXRUNTIME_SRC="$MG_REPO/vendor/dolphin/GXRuntime/src"; shift 2;;
    -j) JOBS="$2"; shift 2;;
    -j*) JOBS="${1#-j}"; shift;;
    --extra-flags) EXTRA_FLAGS="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[ -n "$OUT" ] || { echo "--out required" >&2; exit 2; }
[ -n "$OBJDIR" ] || { echo "--objdir required" >&2; exit 2; }
case "$JOBS" in ''|*[!0-9]*) echo "-j must be a positive integer" >&2; exit 2;; esac
[ "$JOBS" -ge 1 ] || { echo "-j must be >= 1" >&2; exit 2; }
[ -d "$DOL_GEN/chunks" ] || { echo "DOL_GEN missing chunks: $DOL_GEN" >&2; exit 2; }
[ -d "$REL_GEN" ] || { echo "REL_GEN missing: $REL_GEN" >&2; exit 2; }
[ -d "$GXRUNTIME_SRC/core" ] || { echo "GXRUNTIME_SRC missing core/: $GXRUNTIME_SRC" >&2; exit 2; }

case "$OUT" in
  */gGZLE01_recomp.so|*/gGZLE01_recomp.so.inline)
    echo "REFUSED: --out may not target a pinned module name ($OUT)" >&2; exit 3;;
esac

mkdir -p "$OBJDIR"

FLAGS=(
  -I "$MG_REPO/include"
  -I "$MG_REPO/vendor/dolphin/GXRuntime/include"
  -I "$MG_REPO/vendor/dolphin/DolRecomp/src"
  '-DDOLRECOMP_CPU_HEADER="core/cpu.h"'
  -I "$DOL_GEN"
  # module_tables.inc is generated only into the canonical dol tree; DOL
  # section/chunk ranges are identical across codegen variants.
  -I "$PORT/build/dol/generated"
  -I "$PORT/descriptor"
  -I "$PORT/loader"
  -DDOLRECOMP_ENABLE_REPLACEMENTS
  -fPIC -ffp-contract=off -fno-fast-math -O2
)
if [ -n "$EXTRA_FLAGS" ]; then
  read -r -a EF <<< "$EXTRA_FLAGS"
  FLAGS+=("${EF[@]}")
fi

LOG="$OBJDIR/build.log"; FAIL="$OBJDIR/fail.log"
rm -f "$LOG" "$FAIL"
find "$DOL_GEN" -name '*.c' | sort > "$OBJDIR/dol.list"
find "$REL_GEN" -name '*.c' | sort > "$OBJDIR/rel.list"

compile_one() {
  src="$1"; out="$2"
  cc "${FLAGS[@]}" -c "$src" -o "$out" 2>>"$LOG" || echo "FAIL $src" >>"$FAIL"
}
export -f compile_one 2>/dev/null || true

count=0
# DOL sources
{
  while read -r src; do
    base=$(basename "$src" .c)
    printf '%s\t%s\n' "$src" "$OBJDIR/$base.o"
  done < "$OBJDIR/dol.list"
  # GXRuntime CPU core
  for src in "$GXRUNTIME_SRC"/core/cpu.c "$GXRUNTIME_SRC"/core/cpu_exception.c \
             "$GXRUNTIME_SRC"/core/cpu_interpreter.c "$GXRUNTIME_SRC"/core/cpu_interpreter_float.c \
             "$GXRUNTIME_SRC"/core/cpu_interpreter_integer.c "$GXRUNTIME_SRC"/core/cpu_interpreter_table.c; do
    base=$(basename "$src" .c)
    printf '%s\t%s\n' "$src" "$OBJDIR/gx_$base.o"
  done
  # glue + loader
  printf '%s\t%s\n' "$GLUE" "$OBJDIR/module_glue.o"
  printf '%s\t%s\n' "$LOADER" "$OBJDIR/rel_loader.o"
  # REL sources (namespaced by module dir)
  while read -r src; do
    d="$(dirname "$src")"
    # Walk up to the module dir: the dir whose parent is named "rels". Stop
    # at "/" (absolute path) and "." (relative path — dirname's fixpoint, so
    # continuing would spin forever).
    while [ "$(basename "$(dirname "$d")")" != "rels" ] && [ "$d" != "/" ] && [ "$d" != "." ]; do d="$(dirname "$d")"; done
    mod="$(basename "$d")"
    # A source outside any <...>/rels/<mod>/ tree collapses every module to
    # the same "__<base>.o" name — fail loudly instead of colliding. Check
    # the postcondition itself: basename "/" returns "/", so an empty-mod
    # test would not catch the root-collapse case.
    [ "$(basename "$(dirname "$d")")" = "rels" ] || { echo "REL source not under a rels/<mod>/ dir: $src" >&2; exit 2; }
    base="$(basename "$src" .c)"
    printf '%s\t%s\n' "$src" "$OBJDIR/${mod}__${base}.o"
  done < "$OBJDIR/rel.list"
} > "$OBJDIR/jobs.tsv"

total=$(wc -l < "$OBJDIR/jobs.tsv")
# Zero jobs means a --dol-gen/--rel-gen dir exists but holds no sources —
# without this gate the link glob below picks up STALE $OBJDIR/*.o and
# silently ships an incomplete module.
[ "$total" -gt 0 ] || { echo "no sources found under $DOL_GEN / $REL_GEN" >&2; exit 2; }
echo "[rebuild] $total sources, -j$JOBS -> $OUT"
# Without this check a failed split (bad -j, unsupported -n r/N) left no
# slices and the link below silently reused stale objects from $OBJDIR.
split -n "r/$JOBS" "$OBJDIR/jobs.tsv" "$OBJDIR/slice-" || { echo "split failed" >&2; exit 1; }
[ -e "$OBJDIR"/slice-aa ] || { echo "split produced no slices" >&2; exit 1; }
for f in "$OBJDIR"/slice-*; do
  ( while IFS=$'\t' read -r src out; do
      # incremental: skip objects newer than their source
      [ -f "$out" ] && [ "$out" -nt "$src" ] && continue
      cc "${FLAGS[@]}" -c "$src" -o "$out" 2>>"$LOG" || echo "FAIL $src" >>"$FAIL"
    done < "$f" ) &
done
wait

if [ -f "$FAIL" ]; then
  echo "[rebuild] FAILURES: $(wc -l < "$FAIL")"; head -15 "$FAIL"; exit 1
fi
cc -shared -o "$OUT.new" "$OBJDIR"/*.o -lm || { tail -20 "$LOG"; exit 1; }
mv "$OUT.new" "$OUT"
echo "[rebuild] linked ok: $OUT ($(stat -c %s "$OUT") bytes, $total objects)"
