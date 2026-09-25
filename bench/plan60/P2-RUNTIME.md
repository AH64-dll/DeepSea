# P2-RUNTIME — chassis/runtime path to ≥59.9 VI/s (sea, capped)

Date: 2026-09-17. Scope: `vendor/dolphin/.../StaticRecomp/`, GXRuntime
dispatch/memory/journal layer, `src/runtime/dolphin_runtime.cpp`, module glue
(`ZeldaDecompile/port/game/src/module_glue.c`, `port/loader/rel_loader.c`).
Sibling docs: `P3-AUDIT.md` (measured attribution + host levers — read first),
INTEGRATION.md (gate history).

## 0. Where we stand (measured, not assumed)

- Quiet-machine reference: sea **38.18 VI Hz** capped = ~309 Mcyc/s guest
  (63.6% of the 486 MHz needed for 59.94 VI/s; 8.1 Mcyc/field, verified P3).
  Gap to target: **+57%** (1.70× throughput on the CPU thread).
- Same binaries on a co-tenant-loaded box (load 12–27): **22–25 VI** — a
  −35…−45% environment swing. All absolute targets MUST be gated on a quiet
  machine or interleaved-A/B; no lever this size survives load noise.
- CPU-thread share breakdown (sampler, sea steady-state, `.opt` module):
  guest `func_*` ~54%; module runtime glue ~20% (FP/PS helpers ~13%,
  `dolrecomp_dispatch_replacement` 2.4–2.7%, `module_run` 1.8%);
  chassis ~18% (dispatch predicates + REL resolve ~10.5%, CoreTiming ~6%);
  libs ~8% (vdso clock reads ~3.6–4.6%, mostly `MODERNGEKKO_PERF_COUNTERS=1`
  tax + `steady_clock` calls in the Run loop).
- `vi-perf` emu-thread buckets (p3-sea-opt-ctl): `native_ms` 700–950 ms/s,
  `core_timing` 45–210 ms/s (spikes when VI dips), `sync` ~6 ms/s,
  `dc_hook` 4–17 ms/s. `perf-burst`: ~27–46 segments/burst,
  ~260 guest cyc/segment (budget 256 confirmed), bursts ~27–48k/s,
  SyncIn ≈ 104 ns, SyncOut ≈ 85 ns — already cheap.
- Tier counters: `native=99.98–99.99%`, `fallback=0`, `smc_failed=0`,
  `fb_nocov` ~100k/45 s (uncovered low-OS-vector pcs — required semantics,
  not a lever). Chassis overhead is NOT fallback execution.
- `rel_loader_write_journal` is at **0.4%** (was 3.5–5.3% pre-filter) —
  `MODERNGEKKO_JOURNAL_FILTER` page-mask + R/L bounds are armed by default
  (`StaticRecompCore.cpp:216-219` `default_env` sets `=1` before
  `LoadModule`; loader reads env at `rel_loader.c:4524`). The journal lever
  is mostly spent; residual is the per-store gate (~6–9 insns) + the
  `s_active` linear scan for envelope-hitting stores.
- `func_803056E0` = **10.7–17.5% of CPU-thread samples** — this generated
  "function" is the whole OS chunk 0x803056E0–0x80309xxx (OSUnlink body +
  OSMessage + OSThread incl. SelectThread 0x80307DA8–0x80307FD0). Dispatch
  histograms show `0x80307EF4` (SelectThread+0x14c) as the dominant
  re-entry site. This is guest-level scheduler churn, the single largest
  identified guest block.
- `HashFileSha256` ≈ **7.9% of ALL-thread samples** during the measured
  window (worker thread `MODERNGEKKO_HASH_THREADS`, game.cpp:333-454).
  Not on the CPU thread, but it contends for cores/L3/memory on a loaded
  box — measurement hazard, not a runtime lever per se.
- VI cadence sanity: `vi.raw` opens at ~60 Hz (light head phase) then drops
  to ~28–29 in the sea demo — the 60-cap is real and reachable; the sea
  tail is the workload. `thr.csv` shows `slept_ms=0` once starved —
  capped-mode throttle never sleeps below cap (correct).

## 1. Ranked levers

Expected Δ is on **sea VI Hz, capped, quiet machine**, relative to the
38.18 reference; ranges account for share×cut. All gates default-off or
A/B-verified.

| # | Lever | Expected Δ | Files / functions | Correctness risk | Gate | Revert if |
|---|---|---|---|---|---|---|
| R1 | **Idle-spin time-jump v2** — replace consecutive-streak trigger (never engages when spin interleaves work) with a sliding-window ratio trigger over `STATICRECOMP_IDLE_PCS` (start: `80307ef4`; measure for siblings); fix unconditional `[idle]` fprintf that fires per segment once `s_idle_arr % 4096 == 4095` during work phases (`Run.cpp:361-366`) | **+5–15%** | `StaticRecompCore_Run.cpp:345-392` (`IsIdlePc`, jump block), `StaticRecompCore.cpp:290-327` | Med: quantum (default 486000 cyc ≈1 ms) bounds event-observation latency; head re-entered per quantum; must keep `AdvanceGuestTimebase` exact and exceptions checked after jump | `STATICRECOMP_IDLE_PCS`, `STATICRECOMP_IDLE_JUMP=1`, `..._CYCLES`, `..._THRESHOLD` → new `..._WINDOW`/`..._FRACTION` | VI gain <+2% on sea AND outset; any hang/`[stall]`/state-restore divergence; VI cadence irregular (fields not ~uniform) |
| R2 | **Dispatch-eligibility collapse** — per segment the chassis still runs `DispatchableAt` (burst top) + `fast_dispatchable_at`+`IsHostCallAddress` (finish) + `ResolveNativeAddress`/`TranslateRelAddress` for ≥0x80400000 + module-side `ModuleNativeOk` per cache-miss block. Cluster ≈ 8.5–10.5%. (a) memoize `TranslateRelAddress`/`ResolveNativeAddress` per burst when pc unchanged; (b) REL-band last-section memo for segment-end lookups; (c) make `IsHostCallAddress` single-probe when `address<0x80000000` already (skip `|0x80000000` second call); (d) skip burst-top `DispatchableAt` when previous segment was seg_ret==2 AND pc unchanged | **+4–6%** | `StaticRecompCore_Run.cpp:191-192,283-296,417-421`; `StaticRecompCore_SMC.cpp:560-680` (`GetAddressLookupIndex`, `ChunkIndexOf`, `FastDispatchableRunPath`, `DispatchableAt`); `StaticRecompCore.cpp:156-170` (`IsHostCallAddress`); `mod_loader.cpp:146-152,625+` (`handled_sorted`) | Med: verdicts mutate on link/unlink/verify/demote — any memo must key on `g_rel_dispatch_gen`/dispatch epoch + `m_rel_mapping_generation`; host-call membership is static per module load | `MODERNGEKKO_FAST_ELIGIBILITY=1` | Any `fb_*` reason changes sign; REL twin mis-dispatch (outset regress); `smc_failed>0`; fallback>0 |
| R3 | **Module-side dispatch gate** — `dolrecomp_dispatch_replacement` runs per DOL block even on dcache hits (glue comment: kept so runtime-gated DOL hook cases fire). Its ~73-case switch costs a PLT call + 0x268 frame + 4 global stores + compare tree (~2.4–2.7%). Add an O(1) prefilter: 16K-entry bitmap (or sorted-u16 table) of case addresses consulted before the call; non-case pcs skip the call entirely. Semantics identical — the bitmap is a superset-free exact set | **+2–4%** | `rel_loader.c:2492` (`dolrecomp_dispatch_replacement` head), `module_glue.c:170-247` (`glue_call` hit path), `generated.h:580-598` (`dolrecomp_call`) | Low-med: bitmap must list EVERY `case` incl. link-family PCs; miss = silently skipped hook → divergent state. Generate the bitmap mechanically from the case list at build time (assert count==cases) | `MODERNGEKKO_REPL_BITSET=1` | `[resl]`/`[sync]` translation logs disappear; REL link misses; any outset/loading regression; fb deltas |
| R4 | **Cheap clock** — replace `steady_clock::now()` pairs around `Advance`/SyncIn/SyncOut and per-window flush with an rdtsc-based cached clock, or sample clocks every Nth slice. vdso+clock share ≈3.6–4.6% with perf on; ~1–2% residual with perf off (SConfig::GetGameID, stall-diag clock, throttle) | **+1–3%** (+4% for perf=1 runs) | `StaticRecompCore_Run.cpp:140-154,193-205,290-298,460-468`; `CoreTiming.cpp:446` (`Throttle`); `Common/Timer` | Low: clocks are diagnostics/throttle only; must keep ms-resolution guarantees for `Throttle` | `MODERNGEKKO_PERF_COUNTERS=0` for A/B; `MODERNGEKKO_FAST_CLOCK=1` for code path | Timer drift vs wall (throttle overshoot); identical VI ±noise is acceptable (keep anyway — it's free); revert if VI reads become non-monotonic |
| R5 | **Journal residual slimming** — per-store gate currently loads `g_mem_write_journal` (GOT), `g_mem_journal_filter_active` (GOT), then mask word. Fold: when filter inactive, fill mask all-ones and drop the flag load (one fewer global + branch per store); when journal uninstalled, keep single fn-null test. Journal body: replace `s_active[]` linear scan (≤415) for envelope hits with the existing sorted-section index | **+1–2%** | `GXRuntime/include/core/cpu.h:188-202` (`mem_journal_may_touch`/`push`), `cpu.c` filter setters; `rel_loader.c:1515-1560` (body scan), `rel_loader.c:176-184` (envelope) | Low: mask is already a semantic superset; all-ones when unfiltered = legacy semantics; sorted-index must reproduce the first-match order of the linear scan | `MODERNGEKKO_JOURNAL_FAST=1` | Any R→L mirror regression (REL data stale), L-dirty misses, DOL-write ring misses, SMC false negatives |
| R6 | **CoreTiming slice trim** — `core_timing_ms` 45–210 ms/s. `Advance()` runs per outer-while iter (~27–48k/s): event move/sort + global-timer + downcount reset + external-exception check. Candidates: skip `CheckForExternalExceptions` when `ppc.Exceptions` unchanged since last check (dirty-flag, set by any exception OR-er); avoid re-sort when event queue unchanged; hoist `MAX_SLICE_LENGTH` recompute | **+1–3%** | `CoreTiming.cpp` `Advance()`/`Idle()`/`MoveEventIntoDeque`; `StaticRecompCore_Run.cpp:140-157` | Med: delaying exception checks lengthens ext-interrupt latency; event ordering must be preserved; keep 20000-cyc slice cap | `MODERNGEKKO_FAST_ADVANCE=1` | `native_exc` count drifts; interrupt latency probes (dec/timer tests) change; VI irregular |
| R7 | **Per-burst sync trim** — SyncIn (~104 ns)+SyncOut (~85 ns) per burst are already ~1%: skip `ppc_fpscr_updated` when FPSCR unchanged (cache last value in `m_guest`); skip `rel_loader_resync_active`'s guest loads when queue signature memo says clean (module-side memo); skip `on_state_loaded` module call entirely when module reports no state-interest | **+0.5–1%** | `StaticRecompCore_Sync.cpp` `SyncIn`/`SyncOut`; `module_glue.c:601-625` (`module_on_state_loaded`), `rel_loader.c` `rel_loader_resync_active` | Low-med: FPSCR caching must invalidate on any guest FPSCR write — conservative: compare value before skipping | `MODERNGEKKO_FAST_SYNC=1` | FP rounding differences (ferr), REL resync misses after state restore |
| R8 | **Guarded multi-segment dispatch** — `STATICRECOMP_GUARDED_BATCH=N` (1–64) drives prepare/finish via the in-module continuation, removing the per-segment `m_module->dispatch` PLT round trip (~700k segs/s × ~15–25 ns) | **+0.5–1.5%** | `StaticRecompCore_Run.cpp:77-80,425-450`; module `staticrecomp_dispatch_guarded_v1` (exported by `.opt`) | Low: same prepare/finish lambdas, different driver; verify epoch/verify fences unchanged | `STATICRECOMP_GUARDED_BATCH=8` | Any fb reason appears; lockstep (must force N=1 — verify); timing drift |
| R9 | **Bench-hygiene levers (not runtime, but required for honest numbers)** — (a) quiesce `MODERNGEKKO_HASH_THREADS` workers during measured window (idle-prio or defer); (b) all comparisons `perf=0`, `sampler=0`; (c) interleaved A/B same-window; (d) same-field-range comparison (demo drifts light→heavy; equal-wall means inflate ~5–9 pt); (e) quiet-machine gating for the absolute 59.9 call | measurement quality | `game.cpp:333-454` hash workers; `mg_bench.sh` recipe | none | harness-level | — |
| R10 | **REL per-entry residual** — REL dispatches ride the dcache `rgen` path + chunk memo already; remaining: `g_rel_in_chunk` save/set/restore per REL call, `s_ram`/`s_exram` global re-stores per `dispatch_replacement` call, L-image dirty check on chunk entry. Tighten only if REL-heavy scenes (outset) still show >1% in these | **+0.5–1%** (sea); possibly more outset | `rel_loader.c` dispatch legs (~4005-4182), `module_glue.c` REL hit path | Med: `g_rel_in_chunk` guards the journal L-leg — must not be elided | `MODERNGEKKO_REL_FAST=1` | L-mirror staleness (REL data corruption), outset regress |
| R11 | **Crossing into codegen (boundary lever, coordinates with P1)** — port the `dcall`/`icall` transforms (bench-out `apply-dcall.pl`/`apply-icall.pl`, Windows artifacts `gGZLE01_recomp-dcall*.dll`, `…-icall2.dll`: sea ~54–57 vs ~42, outset ~30–36 vs ~19–20 on Windows A/B) to the Linux `.opt` tree: epoch-pinned verdict caches on constant `bl`/`b`/`bc` exits and monomorphic icache on `bctrl`/`blrl`. This is the only measured family with the magnitude to clear the residual gap | **+25–60%** | `apply-dcall.pl`, `apply-icall.pl` (bench-out/), `dol-inline-opt` chunk transform + glue epoch (`g_mg_dcache_gen`, `ppc_dispatch_epoch`) | High if eligibility wrong: hooked/patched/pending-return/demoted targets must keep the full gauntlet — the transform must exclude replacement-case pcs (already does) and re-probe on every epoch bump | separate `.so` build + `MODERNGEKKO_*` A/B | fallback>0, smc_failed>0, any hook missed (mod features dead), savestate divergence |

**Honest arithmetic:** R1+R2+R3+R4+R5+R6+R7 ≈ **+15–30%** → sea lands
~44–50 VI from 38.18 — short of 59.9. Closing fully requires R11 (the only
measured lever family with the right magnitude) or P1-side codegen gains.
P2's job is to bank the chassis ~20% safely and keep it off the codegen
critical path.

## 2. Notes on each investigation area (from the request)

1. **Journal callback + `MODERNGEKKO_JOURNAL_FILTER`**: armed by default via
   `default_env` (StaticRecompCore.cpp:216-219) → loader arms mask
   (rel_loader.c:4524-4532). Mask = SMC DOL chunk pages + watched globals +
   REL **data** sections only (exec sections unmarked — comment at
   rel_loader.c:4347+). Journal at 0.4%. Residual work = per-store gate
   (R5) + body scan. Synchronous ordering preserved; NO async variant may
   be proposed.
2. **Per-batch downcount/event accounting**: charges accumulate in
   `m_guest.downcount` inside generated code; flushed per segment in
   `finish_segment` (Run.cpp:~330-345) with clamp+timebase advance; budget
   = `dolrecomp_cycle_budget` = **256** (module_glue.c:62) — hard cap per
   constraint (1024 already rejected: starves CoreTiming). Events cannot
   be delayed past a batch boundary; further amortization is unsafe.
3. **Memory translation**: `.opt` uses `mem_*_direct` (INLINE_XLAT_FULL,
   compile-time) — constant-bound MEM1 probe + EXRAM second window +
   `*_slow` tail; journal push folded into the store. Residual per store:
   offset compute + bounds + `clear_matching_reservation` (~6 insns) +
   journal gate (~6–9 insns). R5 trims ~2–3 insns/store.
4. **Idle/OSWait-style loops**: `0x80307EF4` (SelectThread spin) dominates
   dispatch-site samples; the OS-chunk func is 10.7–17.5% of CPU time.
   Existing infra: `STATICRECOMP_IDLE_PCS` + `IDLE_JUMP` (486000 cyc
   quantum, threshold 256 consecutive). **Known engagement failure**:
   consecutive-streak resets whenever one work dispatch interleaves — the
   fix is a ratio trigger (R1). `CoreTiming::Idle()` per iteration is
   proven counterproductive — do not revisit.
5. **waitForTick / frame60 / pacing**: capped mode = frame60-accum fully
   inert (`s_enabled=0`, num_hooks=0) — nothing to retime. The guest's own
   waitForTick spin is a charged-cycle spin — partially covered by R1 if
   its PCs join the idle set. Throttle never sleeps below cap (verified
   thr.csv). Uncapped+accum has the fixed F-UNCAP pathology (pad
   ≥TICK_30FPS/8) — still ~20% slower than capped when starved; only pays
   ≥~55 VI/s. Uncapped is NOT evidence of headroom.
6. **Windows parity** (`windows-optimization/FINAL-WINDOWS-2026-09-07.md`):
   "clock gate" ✓ (perf counters env-gated off; per-block clock queries
   absent from shipped `.opt` — but see L2 landmine), "journal bounds" ✓
   (R/L envelopes rel_loader.c:152-184), "steady-state identity lookup
   skips" ✓ (`ResolveNativeAddress` <0x80400000 fast path +
   `m_chunk_lookup_table` direct map + REL memo). Windows *excluded* the
   page filter — Linux ships it on and it measures well (journal 5.3→0.4%).
   The ~3.2k-line uncommitted ModernGekko delta mixes runtime/ABI/frame60
   work with UI/UTF-8/frontend/GPU-safety/test plumbing — inventory only;
   do not fold into this plan.
7. **REL dispatch cost**: sea runs REL-band pcs often enough that
   `ResolveNativeAddress` is 3.3% — memoization is R2. Module-side REL
   path already has chunk memo + dcache rgen entries. The dcache REL hit
   correctly replicates `g_rel_in_chunk` for the journal L-leg — keep it.
8. **Residual chassis at 99.98% native**: the remaining ~46% host time is
   guest code (~54% incl. OS churn ~11–17%) + module glue (~20%) +
   eligibility (~10%) + timing (~6%) + clocks (~4%). Fallback tiers are
   ~empty; fb_nocov ~100k/run is required uncovered-PC semantics.

## 3. Already disproven / what previous work got wrong

- **Dispatch batching is done.** 4–10× fewer round-trips already landed
  (gate-2 +48%); per-segment residuals are in predicates, not call count.
- **Naive `CoreTiming::Idle()` per spin iteration is counterproductive**
  (measured: sync round-trip > ~180 ns spin cost). Only the jump variant.
- **Hook-containing chunk demotion is unsafe** (REL-space SIGILL on
  actor-create). Never propose; `m_no_hook_demote` default already pins
  it off.
- **`MODERNGEKKO_REL_LAZY_ARM=1` breaks savestate restore** when the state
  resumes inside an already-linked REL (no link-family PC → discovery
  never armed). Batch channel now force-disarms lazy-arm (`fd5291b`); any
  re-enable must ride `NotifyStateRestored`/`DolphinStaticRecompNotifyStateRestored`
  rebuild (`rel_loader_resync_active`) — state-restore gate is mandatory.
- **BATCH_BUDGET=1024 rejected** (starves CoreTiming events). Hard cap 256.
- **Uncapped throughput ≠ capped headroom**: uncapped runs the accumulator's
  R-frames and burns ~20% more host when starved; 16.00 VI uncapped vs
  24.8 capped (same binaries) proves it. Don't cite uncapped as ceiling.
- **`ctx->pc` store elision**: 1.29 M dead stores removed → −0.7% (noise).
  Dead stores were already free in the store buffer. Don't redo.
- **ThinLTO + `-march=x86-64-v3` module**: +0.8% (noise). FP-helper call
  overhead is not the wall — semantics-level changes only (P1 scope).
- **A1 Exp D cross-chunk chaining**: rejected, slower than batch alone
  (+19 MB code, eligibility-predicate gap). The dcall/icall family (R11)
  is its corrected descendant with epoch-pinned verdicts — do not confuse.
- **`taskset`/`chrt -f`/governor**: measured +0.1% — host pinning is a
  measurement-hygiene item, not a perf lever (P3 §c).
- **Per-block `Verify`/SMC re-scans**: already eliminated by verify-once +
  epoch; don't re-add per-boundary scans.
- **Journal async / deferred flush**: forbidden — synchronous R→L mirror,
  L-dirty, SMC detection and TRAP semantics are load-bearing.
- **Aggregate native-share**: 99.98% says nothing about the residual —
  the overhead concentrates in batch-exit predicates, REL resolve,
  timing, and glue.

## 4. Latent landmines found during audit (fix before/with any run)

- **L1 — `[idle]` fprintf bug** (`Run.cpp:361-366`): the print condition is
  evaluated on EVERY segment; once `s_idle_arr` crosses a ≡4095 (mod 4096)
  residue during a work phase it prints per-segment (~40+ ns × ~700k/s →
  multi-% regression *and* stderr spam) — exactly when the feature is
  armed. Gate the print behind `at_idle_pc` or a rate limiter FIRST.
- **L2 — module_glue HEAD instrumentation**: current
  `ZeldaDecompile/port/game/src/module_glue.c` runs `rdtsc`×2 +
  `phist_add` per dispatched block plus per-burst `rdtsc` in
  `glue_run_budget` (~30–60 ns/block). The shipped `.opt` predates it
  (0 rdtsc in disasm) — any module rebuild silently picks this up.
  Gate all of it behind `MG_GLUE_DIAG` (currently only the *print* is
  gated; the counters/rdtsc are unconditional).
- **L3 — hash worker during measurement**: `HashFileSha256` ≈ 7.9% of
  all-thread samples in the measured window. Bench-only fix:
  `MODERNGEKKO_HASH_THREADS=1`/idle-prio, or defer until after warmup —
  otherwise quiet-machine comparisons absorb it.
- **L4 — `guest_cycles`/`m_charged_cycles` reads ~1.7–1.9× per-VI** vs the
  verified 8.1 Mcyc/field (e.g. 387 Mcyc/s at 24.8 VI). Likely a
  double-charge or counter-scope artifact — audit before citing cycle
  rates; VI Hz stays the metric.
- **L5 — `SConfig::GetGameID()` string copy per outer-while iter**
  (Run.cpp:160): returns `std::string` by value; ~27–48k/s. Trivial, but
  free to hoist.

## 5. Ordered execution sequence

Safest/highest-evidence first; each step = isolated change + gate + full
scene sweep. `mg_bench.sh` only; ≤60 s; quiet machine or interleaved A/B;
fallback=0 / smc_failed=0 / clean-exit / `[vi]`-monotonic required on
sea+outset+filesel; savestate-restore check on anything touching REL
discovery.

1. **Re-baseline & hygiene (no code)**: confirm the current
   `build/moderngekko-run` hash vs the gate-2 binary; run sea/outset/filesel
   capped, perf=0, quiet or interleaved, with `MODERNGEKKO_HASH_THREADS`
   quiesced; record same-field-range means. Establishes the true starting
   point (expected ~38 quiet, less loaded).
2. **L1/L2 fixes**: gate the `[idle]` fprintf and the glue rdtsc/phist
   instrumentation behind their envs. Zero-risk correctness; removes two
   regressions waiting to ship. Re-run step-1's runs (should be ~neutral).
3. **R4 cheap clock** (+ ensure perf=0 A/B) → expect +1–3%.
4. **R2 eligibility collapse** → expect +4–6%. Gates: outset (REL-heavy)
   and a state-restore smoke; watch fb_* counters.
5. **R3 dispatch_replacement bitmap** → +2–4%. Gates: REL link-heavy boot,
   `[resl]`/`[sync]` translation still firing, outset.
6. **R5 journal gate fold + sorted active-scan** → +1–2%. Gate: REL data
   integrity (outset scene completion), SMC ring intact.
7. **R1 idle-jump v2 (ratio trigger)** → +5–15% if the SelectThread churn
   is skippable. Start `IDLE_PCS=80307ef4`; add measured siblings;
   quantum ≤ 486000 cyc. Gates: VI monotonicity, no `[stall]`, state
   restore, outset + filesel; watch `idle_jumps`/`jump_cycles` counters.
8. **R6 Advance trim** → +1–3%. Gate: `native_exc` parity, VI cadence.
9. **R7 + R8 micro-sync + guarded batch** → +1–2% combined. Cheap to try,
   keep only if non-neutral.
10. **Re-baseline & decision point**: if sea ≥ ~47–50 VI, the chassis stack
    did its job — hand remaining gap to R11 (dcall/icall port, coordinated
    with P1) and FP-helper semantics work; outset will still be sub-60 on
    all known levers (best observed ~34–36) — flag as follow-up.
11. **R11 port** (last; biggest risk+reward): build `apply-dcall`/
    `apply-icall` transforms against `dol-inline-opt` + `rel-out-p14`,
    epoch-wire `ppc_dispatch_epoch`, A/B sea+outset+filesel, lockstep
    spot-check, savestate restore, 120 s soak.

Every revert criterion above reduces to: measured VI regression, new
fallback/smc/exception counters, scene-visual or state-restore divergence,
or timing anomalies (`native_exc`, VI irregularity). When in doubt, the
gate env var is also the revert switch.
