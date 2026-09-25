# P3-AUDIT — measurement validity, attribution, host/system levers, path-to-60

Date: 2026-09-17. Agent: P3 (audit + system). Host: AMD Ryzen 5 8645HS
(Zen4, 6C/12T, SMT pairs 0-5↔6-11, 16 MB L3, NUMA node0 only), amd_pstate
`powersave` governor + `balance_performance` EPP + boost, ~4.6/5.0 GHz observed.
Runs consumed: 4 (p3-sea-opt-samp, p3-sea-opt-ctl, p3-sea-opt-pin,
p3-ovlphang-opt) under `bench-out/p3-*`. Machine was co-tenant loaded
(loadavg ~12–27) for all four — noted per row.

Confidence tags: **[V]** verified this session, **[I]** inferred from
repo artifacts/code, **[S]** speculated.

---

## (a) Verdict on measurement validity

**VI Hz is the right proxy for "reaches real-time" but NOT for "60 fps".**
`[vi] fields=` increments once per emulated VI field boundary
(`Core::OnFrameEnd` ← `VideoInterfaceManager::EndField`,
`vendor/dolphin/Source/Core/Core/HW/VideoInterface.cpp:788` →
`Core.cpp:156`). 59.94 VI/s = NTSC lock = guest running 100% RT. **[V]**

But rendered FPS is a separate axis: the game presents one XFB per **two**
VI in capped mode (RESULTS.md:279 "window FPS ≈ VI/2 is the game's intended
cadence"; empirically 12.66 fps @ 25.35 VI on Windows F1). So:

| mode | 59.9 VI/s produces |
|---|---|
| `--capped` (default; frame60-accum registers **0 hooks** — `num_hooks=0` when `s_enabled=0`, mod.c:764-773) **[V]** | ~30 rendered fps (retail cadence) |
| `--uncapped`/`--open-frame-rate` (arms `MODERNGEKKO_FRAME60_ACCUM=1`, moderngekko_run.cpp:466-473) **[V]** | ~60 presents/s via 30 Hz logic + 30 Hz duplicate-present — only when RT ≈ 100% |

**The "+48%" claim re-derivation** (`bench-out/` raw vi.raw, baseline
`base-*-capped-20260911-0727*` vs `gate-opt-*-20260911-1602*`):

| scene | measured means (headline) | same-content rate (fields shared span) | honest delta |
|---|---|---|---|
| sea | 25.75 → 38.18 = **+48.3%** | 26.4 → 36.8 Hz over fields 913–2314 | **+39.4%** [V] |
| outset | 16.41 → 24.79 = **+51.1%** | 16.9 → 25.1 Hz over fields 423–1271 | **+48.3%** [V] |
| filesel | 26.39 → 38.94 = **+47.6%** | 26.7 → 36.9 Hz over fields 893–2350 | **+38.2%** [V] |

**Artifact found — demo-content drift:** the demo is deterministic per
field index (first-second field counts match to ±3 across arms: 65/65/62)
but it is *not* uniform in weight: the attract sequence has a ~60 Hz-capable
head (fields <~500) and variable-rate tail. A faster arm covers more demo
per fixed wall window and spends proportionally more of its window in
lighter content → the equal-wall-window mean is inflated ~5–9 points.
Equal-wall comparison is *approximately* fair, not exact. Same-runner
same-session control agrees: gate-inline-sea 26.11 (15:58) → gate-opt-sea
38.18 (16:02) = +46.2%, between the headline and same-content figures. **[V]**

- Warmup=15 s adequately excludes the boot/restore fast phase (first ~8
  lines ≈ 60 Hz); measured window is steady-state on all 6 runs. **[V]**
- `[vi]` lines are wall-normalized (hz = fields·1000/elapsed_ms,
  Core.cpp:904); `tail -60` ≈ 60 s ± ~2 s under starvation. **[V]**
- thr.csv sanity: `slept_ms>0` only in the capped-at-60 head phase
  (lines 1–10); zero once starved — throttle is behaving, no hidden sleep
  inside the measured window except when actually at cap. **[V]**
- **Session drift is the dominant measurement hazard**: identical binaries
  (runner 73713c4e…, module 435574c1…, state 4b7b9901…) measured
  sea = 38.18 on 09-11 (quiet) vs **22.16–24.77 today** under load ~12–27 —
  a **−35 to −45%** environment swing, larger than most lever sizes. **[V]**
  All prior headline numbers are quiet-machine numbers.

## (b) Current attribution — .opt module, sea, steady-state

mg_sampler (SIGPROF 499 Hz) on `p3-sea-opt-samp`, windowed `--from 32000`
(21,091 samples, excludes startup hash). Co-tenant loaded; **shares** are
load-insensitive, absolute VI is not.

| bucket | share | detail |
|---|---|---|
| guest game code (`recomp:func_*`) | **53.7%** | top: `func_803056E0` **10.7%** (OS-scheduler chunk incl. SelectThread spin 0x80307EF4), `func_803256E0` 2.6%, `func_802D56E0` 2.5%, `func_802456E0` 2.5%, then a long tail |
| module runtime helpers | **19.8%** | float/ps helpers ~13% (`ni_madd_msub` 1.37, `ppc_psq_*` ~1.9, `force_single`/`force_25bit_c` 1.84, `f32_from/to_bits` 1.1, `ppc_fmuls/fcmp/fadds/...` ~3, `classify_*`/`set_fprf`/`fp_*` ~1.5, `ni_*` ~2.6); dispatch machinery `dolrecomp_dispatch_replacement` 2.7 + `module_run` 1.9 + `module_dispatch` 0.2; `rel_loader_write_journal` **0.43%** (journal filter working — was 5.1% baseline) |
| runner/chassis | **17.8%** | dispatch-predicate + REL-resolve cluster ~11%: `ResolveNativeAddress` 3.47, `ChunkIndexOf` 1.36, `FastDispatchableRunPath` 1.21, `IsHostCallAddress` 0.93, `HandlesAddress` 0.83, `ModuleNativeOk` 0.72, `Run` 0.65, `TranslateRelAddress` 0.21; GPU-Null FIFO decode ~2% (`OpcodeDecoder`, `GPFifo`, `RunGpuOnCpu`, `VertexManager*`, `TextureCache*`); MMIO/timing/sync rest ~5% |
| libs | **8.1%** | `[vdso]` clock reads **3.6%**, libc 2.2%, libvulkan_radeon 0.86 + glslang 0.25 (shader-cache threads), lz4 0.25, libstdc++ 0.5 |
| jit fallback | 0.5% | `fallback=0` per shutdown counters |

Emu-thread view (`[vi-perf]`, measured window, same run):
`native_call` 903 ms/s (90.3%), `core_timing` 62.8 (6.3%), `sync` 5.8,
`dc_hook` 4.8, unaccounted ~23 (2.3%). The "native" bucket includes the
in-batch `ModuleNativeOk`→predicate callbacks (~5% by sampler). Emu thread
= `CPU-GPU thread` at **91–94% of one core**; all other threads <2%
combined. **[V]**

Where the core is saturated: ~54% genuine guest work, ~20% module runtime
glue (mostly FP/PS helper calls), ~11% dispatch predicates/REL resolution,
~6% CoreTiming, ~4% clock reads. Non-guest overhead = ~46% of host time —
eliminating it entirely would yield ~+86%; codegen quality, not pacing, is
the wall.

## (c) Host/system levers

| lever | expected Δ | status |
|---|---|---|
| `taskset -c 0-5` + `chrt -f 80` (physical cores only + RT) | **+0.1%** (24.80 vs 24.77 VI Hz, same-session adjacent runs, load ~12–18) | **[V] measured — NEUTRAL.** Emu thread already gets a dedicated core; process's other threads ~idle so SMT-exclusion doesn't hurt; RT adds nothing for a 100%-compute thread |
| SMT sibling contention | **0% controllable from our side** — today's co-tenant load cost ~35–45% absolute (same binaries: 38.18 quiet → 22–25 loaded). Pinning our process can't idle the sibling; needs `cgroup`/`isolcpus` or scheduling runs in quiet windows | **[V] measured effect, [S] mitigation** |
| governor `performance` / EPP `performance` | ~**+0–8%**: observed cur ~4.6 GHz vs 5.017 max under load; amd_pstate `balance_performance` already boosts — realistic ~+2–4% on a single hot thread | **[S]** — not writable without root (`scaling_governor`/`energy_performance_preference` Permission denied) |
| NUMA | **N/A** — single populated node | [V] |
| nice/RT priority | ~0% quiet; helps only vs co-tenant preemption (in folded into pin test) | [V] |
| Affinity already set? | **No** — `Thread.cpp:148` helper exists but no caller on the Linux path; `CACHE_AFFINITY` was Windows-only no-op (ledger A1) | [V] |

Net: **host levers ≈ 0–5%**. The real "system lever" is procedural: run
benchmarks quiet or use interleaved A/B (F47 protocol); absolute targets
like 59.9 must be gated on a quiet machine.

## (d) frame60-accum semantics at real-time

Read `mods/frame60-accum/mod.c` (775 lines) + observed runs:

- **Capped mode: mod is fully inert** (`s_enabled=0` → `num_hooks=0`);
  f60.csv empty in all capped runs. Presents = VI/2 ≈ 30 fps at 60 VI/s.
  **[V]**
- **Uncapped**: `--uncapped` sets `MODERNGEKKO_UNCAPPED/FRAME60_ACCUM=1`;
  H1 retimes `waitForTick` to `40500000/render_hz` (default 60) and the
  accumulator (`s_acc += field_0x34 delta`, fire logic when ≥ TICK_30FPS =
  1,350,000 ticks) produces ~1 L : 1 R at 100% RT → 60 presents/s.
  **[V by code + fixcheck run: L cadence ~17/s at 0.57 RT — scales to ~30/s
  at 1.0]**
- **Self-gating**: below 100% RT each slot's delta ≥ TICK_30FPS → all-L →
  retail-identical behavior; **it cannot cap throughput below VI rate**.
  The only pathology was pre-fix unlimited-wait spin (stress run 15.87 VI,
  L=0%); fixed `12b3071` pads wait to ≥TICK_30FPS/8 — post-fix 30.43 VI,
  ~194 iter/s. **[V]** Caveat: uncapped still *costs* ~20% vs capped when
  starved (R-frames burn host CPU: 30.43 vs 38.18 on sea) — only pays at
  ≥~55 VI/s sustained.
- **Scenario "60 VI but ~30 fps": YES — any capped run**, and any
  uncapped run that dips below ~95% RT in heavy scenes (accumulator
  collapses to all-L → 30 Hz presents). VI≥59.9 is necessary but not
  sufficient for a 60 fps UX.

## (e) Previous claims — confirmed vs refuted

| claim | verdict |
|---|---|
| "+48% over baseline" (INTEGRATION gate-2) | **Partially refuted**: measured +48.3% real but inflated ~5–9 pt by demo drift; same-content ≈ +39% (sea), +48% (outset), +38% (filesel). Direction and rough magnitude hold |
| env-default arming = the win? | **Refuted**: gate-1 integ-runner + `.inline` (batch inert, env defaults armed) = 26.11 = **+1.4%**; a1final decomposition: mem-direct +9.3%, +batch +21.3% — batching dominates; A2 chassis contributes the residual ~+5–10% |
| "nothing safely parallelizable on Emuthread" (A3) | **Confirmed**: headless forces `MAIN_GFX_BACKEND="Null"` + `BACKEND_NULLSOUND` (dolphin_runtime.cpp:420-421,472); GPU/FIFO CPU-side work ≈ 2% in samples (matches F23's 2.31%, Amdahl +2.4%); journal is synchronous guest-RAM mutation — must stay ordered. Max separable slice ≈ 2% |
| "sea is the heaviest scene" | **Refuted**: outset is heavier (16.41→24.79 vs sea 25.75→38.18); ovlphang-d7 ≈ sea (25.41 vs 24.77 same-session) — sea is a mid-heavy demo scene; `playsea-*`/`pulse*` states are lineage-locked (F5/F20), could not probe |
| Windows "big platform gap" | **Refuted**: F17 parity stands and post-.opt Windows runs are at/above Linux class (pgo3 sea 42.0 headed-Vulkan vs Linux 38.18 headless; outset 19.7 vs 24.79 Linux ahead). The 12.66 fps/25.35 VI figure was the *old shipped package* on a different state lineage; fps = VI/2 explains the 2× |
| equal-wall-window fairness (mg_bench.sh docstring) | **Qualified**: fair to ~±10%; faster arms drift into lighter demo content (this is why means differ per arm; use same-field-range comparisons for precise attribution) |

## (f) Path-to-60 arithmetic

Requirement: deliver 59.9 VI/s ≈ **485.5 Mcyc/s** guest (8.1 Mcyc/field
verified: 486 MHz / 60). Current quiet-machine sea: 38.18 VI = 309 Mcyc/s
(63.6% RT). Gap = **+57%** (1.70×).

Lever stack estimate (sea, all [I]/[S] except where noted):

| lever | plausible Δ | basis |
|---|---|---|
| host pinning/RT/governor | +0–5% | pin+RT measured **0** [V]; governor unwritable, +2–4% speculative |
| vdso clock reduction (cheap-clock/RDTSC cache; some is --perf instrumentation) | +2–3% | 3.6% share [V] |
| ResolveNativeAddress/REL memoization (per-segment probe, 3.5%+0.2) | +2–3% | [V share, S gain] |
| float/PS helper slimming (lazy fprf, direct SSE forms) | +5–7% | ~13% share [V]; ThinLTO+v3 already measured **+0.8%** so naive inline won't do it — needs semantics-level change [I] |
| idle-spin skip (SelectThread spin inside func_803056E0, ~10.7%) | +5–10% | [I] — previous IDLE_PCS gate measured 0-engage on this lineage (agent-a §4); needs correctness care (F8 pathology) |
| larger BATCH_BUDGET | +1–3%, risky | 1024 already rejected (starves CoreTiming) [I] |
| **in-module call linking / inline caches** (dcall+icall+dcache transforms — sibling Windows modules) | **+25–60%** | [I from bench-out windows runs: dcache.dll sea ~54–57 VI vs ~42 pgo3; outset ~30–36 vs ~19–20; icall2 outset 36.8 vs 30.3 same-pair] — not yet verified on Linux `.so` |
| HLE of hot guest functions (J3D/math/OS spin) | +10–35% if aggressive | [S] — 54% guest-code pool; correctness cost high |
| multi-core recomp / GPU offload | ~0 headless / research-scale | GPU-Null ceiling ~2% [V]; parallel guest exec breaks ordering [I] |

**Sum of non-codegen levers ≈ +15–25% → sea lands ~44–48 VI/s — does NOT
reach 59.9.** The +57% is only reachable via the codegen-structural lever
class (in-module direct/indirect call linking with epoch-pinned caches —
the `dcache`/`icall2` Windows experiments are exactly this and show the
right magnitude). Recommended sequencing:

1. Port/verify the dcall/icall transform on the Linux `.opt` module
   (siblings' `apply-*.pl` scripts + dll artifacts exist under
   `bench-out/`; verify fallback=0, SMC parity, then A/B sea+outset).
2. Add cheap-clock + REL-resolve memoization (both ~free, ~+4–6%).
3. Re-baseline on a quiet machine; treat co-tenant load as the primary
   measurement hazard (F47 interleaving mandatory under load).
4. If still short on sea, idle-spin skip + selective HLE; outset will
   remain sub-60 under any currently-known lever set (best observed
   ~34–36 VI on Windows dcache — needs ~1.7× more).
