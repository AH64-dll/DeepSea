# ModernGekko benchmark environment (this machine).
# Source this file before using mg_bench.sh / mg_symbolize.py.
# Paths are absolute; keep them stable across worktrees so every
# agent's benchmark numbers are directly comparable.

MG_ROOT="/run/media/amr/New Volume/zelda"
MG_GAME_ROOT="$MG_ROOT/ZeldaDecompile/port/disc/GZLE01-full"
MG_MODULE_INLINE="$MG_ROOT/ZeldaDecompile/port/game/gGZLE01_recomp.so.inline"
MG_MODULE_OPT="$MG_ROOT/ZeldaDecompile/port/game/gGZLE01_recomp.so.opt"
MG_MODULE_BASELINE="$MG_ROOT/ZeldaDecompile/port/game/gGZLE01_recomp.so"
MG_STATE_DIR="$MG_ROOT/ZeldaDecompile/port/state"
MG_USERDIR_TEMPLATE="$MG_ROOT/ZeldaDecompile/port/run/benchharness-user-dir"
MG_BENCH_OUT="$MG_ROOT/bench-out"
MG_STATE_HEAVY="$MG_STATE_DIR/seademo-entry.state"   # verified loads on .inline (2026-09-11)
MG_STATE_MED="$MG_STATE_DIR/outset-day-interim.state"
MG_STATE_LIGHT="$MG_STATE_DIR/fileselect-entry.state"
