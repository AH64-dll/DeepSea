# PLAN-60 — consolidated execution plan to reach 59.9 VI/s (real-time)

Date: 2026-09-17. Synthesizes P1-MODULE (pending), P2-RUNTIME.md, P3-AUDIT.md
plus orchestrator findings (Windows-side artifacts: dcall/icall transforms,
PERF-RESCUE-LEDGER F1-F55, RESULTS.md).

## Ground truth

- Reference (quiet, Sep-11 gates): sea 38.18, outset 24.79, filesel 38.94 VI Hz.
- Honest same-content deltas (P3): +39% sea / +48% outset / +38% filesel.
- Target: **59.9 VI/s ≈ 485.5 Mcyc/s guest**. Sea needs +57%; outset needs +140%
  (outset is the true worst scene — 24.79).
- Attribution (P3 sampler, sea, .opt): guest code 53.7% (func_803056E0 OS-chunk
  10.7–17.5%, SelectThread spin dominant), module glue 19.8% (FP/PS helpers ~13%),
  chassis 17.8% (dispatch/REL-resolve ~11%), libs 8.1% (vdso clocks ~3.6%),
  jit 0.5%, journal 0.4%.
- Environment: Ryzen 5 8645HS, powersave governor (unwritable), load 14–25
  typical → **all gates interleaved-A/B or quiet-window; pin taskset -c 0-5.**
- 60 VI/s ≠ 60 fps: capped mode presents VI/2. True 60fps needs --uncapped +
  frame60-accum at ~100% RT. Uncapped costs ~20% when starved — gate it
  separately after capped reaches ~55 VI/s.

## Phase A — module-side (Executor M)

| # | Lever | Expected | Notes |
|---|---|---|---|
| M1 | **Build Linux .so from `dol-inline-opt-dcall` tree** (206/206 chunks: `s_okgen_dc` + `s_ict_dc`; REL tree `rel-out-p14` IS relcall-transformed — 540 `s_ict_rc` sites keyed on `g_rel_dispatch_gen`) | **+40–60%, possibly cap** | **Windows evidence VERIFIED from raw vi.csv: `gGZLE01_recomp-dcall.dll` = 59.90 mean/59.9 min on ALL SIX scene-reps + 90-sample soak** (dcall-final/, dcall-soak/). This is THE lever — if it ports cleanly the target is hit. BUILD RUNNING: `/tmp/mg-bench/obj-dcall` → `game/gGZLE01_recomp.so.dcall`. |
| M2 | `-fvisibility=hidden`/`-Bsymbolic-functions` on module | +2–5% | P1 census: only ~5 symbols dlsym'd by chassis; ~875 exports + ~1.015M PLT-bound calls + 3,818 GOT loads (in func_803056E0 alone) are pure tax. Assert export presence post-build — hiding `ppc_dispatch_epoch` silently disarms epoch = wrong-code risk |
| M3 | PGO (gcc or llvm profdata) on module | +5% | F53 measured +5.1% on Windows; reproduce on Linux with sea+outset profile |
| M4 | `MODERNGEKKO_CYCLE_BUDGET` sweep 256→1024–4096 | +1–3% | env knob live in new glue; 1024 starved CoreTiming pre-fix but batch-exit shape changed since |
| M5 | FP/PS helper slimming (lazy fprf, SSE forms) | +5–7% | ~13% share; needs semantics-level change (P1 detail) |
| M6 | module-local native-ok bitmap | +4–8% | agent-4 ranked (ledger tail) |
| M7 | deferred flag materialization in emitter | +3–8% | agent-4 ranked; emitter.c change → regen |
| M8 | `ppc_fp_available_inline` hoist | +2–5% | agent-4 ranked |
| M9 | pcelide tree evaluation | ? | `dol-inline-opt-pcelide` exists; F54 dead-store elision was neutral — check what differs before spending a build |

## Phase B — chassis/runtime (Executor R)

| # | Lever | Expected | Notes |
|---|---|---|---|
| R0 | **L1+L2 landmine fixes FIRST** | unblocks | L1: unconditional `[idle]` fprintf Run.cpp:361-366 — gate behind at_idle_pc/rate-limit. L2: glue rdtsc/phist ungated — **DONE by orchestrator** (`s_diag_on` in module_glue.c) |
| R1 | Idle-jump v2: ratio trigger over `STATICRECOMP_IDLE_PCS=80307ef4` (consecutive-streak trigger provably never engages) | +5–15% | Med risk; quantum ≤486000cyc; keep AdvanceGuestTimebase exact |
| R2 | Dispatch-eligibility collapse (memoize TranslateRelAddress/ResolveNativeAddress per burst; REL last-section memo; single-probe IsHostCallAddress) | +4–6% | key memos on g_rel_dispatch_gen + m_rel_mapping_generation |
| R3 | `dolrecomp_dispatch_replacement` prefilter bitmap (16K-entry, generated from case list) | +2–4% | must list EVERY case incl link-family; miss = skipped hook |
| R4 | Cheap clock (rdtsc cache / sample every Nth slice) | +1–3% | keep Throttle ms-resolution |
| R5 | Journal gate fold + sorted active-scan | +1–2% | preserve first-match order |
| R6 | CoreTiming Advance trim (dirty-flag ext-exc check, no re-sort when unchanged) | +1–3% | keep 20000-cyc slice cap |
| R7 | Sync trim (FPSCR memo, resync signature memo) | +0.5–1% | invalidate FPSCR memo on guest write |
| R8 | `STATICRECOMP_GUARDED_BATCH=N` multi-segment dispatch | +0.5–1.5% | force N=1 under lockstep |
| R9 | Bench hygiene: quiesce `MODERNGEKKO_HASH_THREADS` during measured window (7.9% steal), perf=0 A/B, same-field-range compare | quality | |
| R10 | REL per-entry residual trims | +0.5–1% | keep g_rel_in_chunk journal leg |

## Phase C — verification & loop

1. Serialized quiet-machine gate per landing: sea+outset+filesel, capped,
   `fallback=0`/`smc_failed=0`/exit 0/`[vi]` monotonic required.
2. Cross-module save/load spot-check after module changes.
3. Uncapped+frame60 run once capped approaches ~55 VI/s (the 60fps-present check).
4. Outset stays sub-60 on known levers (best observed ~35–37) — report verified
   ceiling honestly if it binds.
5. Iterate M/R levers until sea+filesel ≥59.9 or list exhausted.

## Hard rules (binding)

- Never ship `emit_cross_chunk_call`; never demote hook chunks; batch ≤256
  default (env sweep allowed); journal stays synchronous; REL first-match order
  preserved; lockstep disarms batch+guarded modes; benchmarks serialize on
  /tmp/mg-bench.lock.
- module_glue.c has TWO copies (F52): `port/game/src/module_glue.c` (Linux
  rebuild_module.sh default) vs `port-tools/wt-a1-codegen/modsrc-a1/` (Windows
  build.ninja). Keep in sync or reconcile.

## dcall build recipe (verified)

```
bash bench/rebuild_module.sh \
  --out  $MG_ROOT/ZeldaDecompile/port/game/gGZLE01_recomp.so.dcall \
  --objdir /tmp/mg-bench/obj-dcall \
  --dol-gen $MG_ROOT/ZeldaDecompile/port/build/dol-inline-opt-dcall/generated \
  --rel-gen $MG_ROOT/ZeldaDecompile/port/build/rel-out-p14/generated/rels \
  --glue   $MG_ROOT/ZeldaDecompile/port/game/src/module_glue.c \
  --extra-flags "-include /tmp/cycle_budget.h -DMODERNGEKKO_INLINE_XLAT_FULL" -j8
```

(`-include` needs a space-free path — copy cycle_budget.h to /tmp first;
`--extra-flags` word-splits on spaces.)
