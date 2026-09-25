# Outset 60-gap analysis (2026-09-17, interim)

## Verified facts (Linux .dcall vs Windows gGZLE01_recomp-dcall.dll, SAME state sha A7E60EA)

| counter | Linux .dcall (71s) | Windows dcall.dll (41s) |
|---|---:|---:|
| fields (VI events) | 1341 (~18.9/s) | 2462 (60/s, capped) |
| native blocks | 27.3M (384K/s) | 79.6M (1.94M/s) |
| cycles charged | 7.38G (104 Mcyc/s) | 20.39G (497 Mcyc/s) |
| cycles/field | 5.51M (UNDER nominal 8.08M) | 8.28M (hardware-exact) |
| blocks/field | 20.4K | 32.3K |
| cache_direct | 6.2M | 0 |
| native_exc | 81,694 | 1 |
| reverify_events | 51 | 3 |
| verifications | 236 | 87 |
| bursts | 1.18M | 21.0M |

- Windows run was genuine: headed Vulkan, capped, no mods, state hash matches.
- Windows runner (sha 32B6AA6E) contains our perf/integration lineage (4-arg ResolveNativeAddress, guarded_v1 lookup). Same code family.
- Windows module = llvm-mingw Clang -O2; ours = GCC 16 -O2. Same -ffp-contract=off -fno-fast-math.
- Transform census: DLL has ONLY s_okgen+s_okgen2 (104K direct-call caches). Ours adds s_ict/s_icf/s_icg2 (75K icall) + s_icrg/s_icp/s_icdg (71K relcall). icall/relcall are Linux-only extras — neither helps nor hurts outset (dcall≈opt there).
- Linux pinned run (taskset -c 0-5): user=82% of a core → compute-bound, NOT host-starved. Load 17-30 is a confound for ABSOLUTE numbers but cannot explain 5x alone.
- [exc] diag: native_exc stream = FP-Unavailable (vector 0x800, lazy-FP-switch) + syscall (0xC00). ~940/s. g_ppc_lazy_fp_enabled=true on both sides (default; only StrikersRecomp disables).
- Sea on our .dcall reaches 1.7M blocks/s (59.9 VI cap) — the host CAN run at Windows-rate; the deficit is outset-specific.
- Outset per-burst wall cost ~64us vs sea ~5us, same ~20 blocks/burst → per-block cost is the gap.
- Microbench (chunk_0144 stwx loop, gcc vs clang): ~equal (500-850 Minstr/s, noise-dominated under load). Compiler NOT decisively different on leaf integer code.

## Open hypotheses (ranked)
1. Clang-vs-GCC on the giant switch/FP-heavy functions (leaf loop says equal; FP-helper and dispatch paths untested) — clang .so build IN PROGRESS (/tmp/mg-objdir-dcall-clang).
2. Per-block context traffic: our emitter writes ctx->pc multiple times per block; newer DLL-era emitter may elide. (Generated-tree version drift, not compiler.)
3. native_exc storm cost: each FP-unavailable exits segment -> Dolphin delivery -> handler redispatch. ~61/field. If Windows' module delivered these without chassis exit, real cost gap. Mechanism unverified — needs their glue/cpu.c source (not on disk).
4. Charge-accounting asymmetry: we deliver 47% MORE VIs than charged cycles warrant (5.5M/field vs 8.08M nominal). Either VI timing early OR cycles counter misses drain paths (JIT/idle). Means our 17 VI/s is OPTIMISTIC — real deficit is worse.
5. RefreshRelSections re-discovery loop (module 1 rescanned ~20x, never installed) + reverify 51x — each gen bump thrashes REL-keyed caches. Plausible few-% tax, not 5x.

## NOT the cause (ruled out)
- demo-content drift: outset VI rate uniform ~17-18 across entire window on both arms.
- REL section translation path: ResolveNativeAddress 2.9% + friends ~10.5% total.
- HashFileSha256 worker: 8.2% sample share — real contamination but bounded.
- idle_jumps: 0 on both sides.
- missing transform coverage: outset's hot chunk HAS the sites.

## 2026-09-17 17:00 — sampled profile + codegen parity (fat .dcall, pre-epoch runner)

SIGPROF @499Hz, outset, gcc .dcall (m60-prof-dcall-outset-20260917-164911,
37.9K samples): recomp 67.7% / runner 18.8% / lib 13.1% / jit 0.4%.
Emu thread = 88% of all samples (single-core bound; co-tenancy is secondary).
Runner top: HashFileSha256 1871 (worker-thread file hashing, known L3),
ResolveNativeAddress 1058, dispatch-path sum (ChunkIndexOf+FastDispatchable+
IsHostCall+HandlesAddress+ModuleNativeOk+RefreshRelSections) ≈ 5.9%, GX/fifo ≈1.2%.
Module top: func_802416E0 3.9% (contains cNdIt_Judge/cTgIt_JudgeFilter),
func_802456E0 2.7%, func_802D56E0/803256E0 ~2%; dolrecomp_dispatch_replacement
1.6% + glue_run_budget_impl 1.5% in-module dispatch tax. FP helpers
(ni_madd_msub+psq_load+f32_from_bits+force_single+psq_store) ≈ 3%.

Sea contrast (m60-prof-dcall-sea-20260917-170107, 42 VI/s under same load):
recomp 58% / runner 30.6% / lib 11.5%; func_803256E0 alone 31%; dispatch path
13.5% (yet still near cap → dispatch is NOT the wall); native_exc 4,635 vs
outset 102,745 (22x); cache_direct 0.63M vs 7.8M (12x). Outset is FP/mem/actor
dense + exception/cache-op dense; sea is a lean integer loop.

Codegen parity check (same guest fmul/mem window, func_800256E0):
- Windows dll (llvm-mingw) vs our gcc .so emit ~identical sequences:
  same ctx->pc store per instr, same lazy-FP check incl. refptr/GOT load +
  MSR bit test + raise call (EARLIER 'no lazy_fp refs in dll' was WRONG —
  dll references .refptr.g_ppc_lazy_fp_enabled per site), same MEM1/EXRAM
  bounds chain. Delta: dll uses movbe (fused load+bswap) vs our mov+bswap;
  our calls route via @plt (SysV regs). Per-instr shape ≈ equal → the 3x
  cannot be per-instruction codegen; clang measured +36.6% accounts for part.
- pc-store census identical across dol/generated, dol-puredcall,
  dol-inline-opt-dcall: 859,846 sites each (no emitter drift).

Historical recalibration (PERF-RESCUE-LEDGER): Sep-7 Windows package did
25.35 VI/s on outset (F1), Wine-on-this-box reproduced ~26 (F7), and F17
recorded near-parity (Linux 21.7–23.4). The 60-VI/s outset artifact is the
NEWER dcall.dll lineage (sha 81CBC06A, post-loopfix runner). So the local
bar for "Windows parity on outset" ≈ 26 VI/s on this host; 59.9 requires
matching the newer lineage end-to-end.

Prime suspect now: dispatch-gen churn. glue-tick on pre-epoch runner showed
genmove=2,216,280 / miss=3,453,060 vs hit=54,993 (98.4% miss) — every
per-site cache (s_okgen/s_icg) invalidates EVERY segment. If the epoch-armed
runner pins gens, the fat icall fast paths finally engage.

## 2026-09-17 18:20 — RESOLVED: epoch channel was the missing runner piece

The "prime suspect" (dispatch-gen churn) is CONFIRMED, with a twist:
- pre-epoch runner: all per-site caches (s_okgen/s_icg) expire every batch →
  98.4% miss → every `bl`/`bctrl` pays full dispatch gauntlet. ALL earlier
  module A/B data used the pre-epoch runner → pure≈fat (both always miss).
- NEW runner (moderngekko-run, epoch channel armed via ppc_dispatch_epoch):
  outset fat-clang 46.83/46.48 VI/s vs pre-epoch 39.27/40.15 — **+19%**;
  pure-dcall 44.76 — +14%. Host profile: ResolveNativeAddress 4.4%→0.6%,
  ChunkIndexOf/FastDispatchable/RefreshRelSections vanish from top-60.
- Prior "epoch runner slower (8.11)" was measured under Executor-R
  co-tenancy — RETRACTED. Correct pairing: epoch runner + fat module is the
  best-known config (~46.7 VI/s outset, still 78% of cap).

## Host-contention correction (all prior absolute numbers suspect)
- Load↔VI near-linear on outset: ~40 − 1.4×loadavg. "fat-clang 14-19" was
  load 15-24; same binary does 38-40 at load ~2. .opt quiet ref (24.79) is
  below today's fat-clang@load1.7 (40.15).
- Second-thread cost: HashFileSha256 ~9% of all samples — InspectGame hashes
  files/ from the external drive inside the measured window.
- glslang/libLLVM/RADV threads persist under --headless (Null backend):
  ~3-4% total.

## Levers re-ranked after epoch fix (outset, near-idle)
- new runner + fat-clang: 46.7 VI/s — CURRENT BEST.
- Dispatch overhead: fully excluded (budget4096 idle-pending; GB16 flat;
  bsymbolic flat; epoch arm collapsed it structurally).
- movbe: Windows had it, we don't (775 bswap vs 3 movbe per 256KB .text).
  Rebuild in flight — highest remaining module lever.
- MODERNGEKKO_PERF_COUNTERS=1 costs ~9% under contention (15.94→14.62) —
  run final gates without --perf (shutdown counters are unconditional).
- Idle-jump streak trigger: confirmed never fires (idle_jumps=0); the R1
  ratio-trigger redesign is still unimplemented.
- REL-side `_rc` caches (71,508 sites) exist in BOTH trees — untouched by
  today's DOL experiments; REL actors are hot in outset (d_a_* call paths).

## Resolution update (2026-09-17 late, quiet box after emulator kill)

### Verified on quiet box (load ~1.4-2.9), interleaved, lock-serialized
- outset: **~49.4 VI/s** — puredcall-clang (Windows recipe) + epoch runner + MODERNGEKKO_JOURNAL_FILTER=1
  - movbe arm: 49.23/49.42 — `-march=znver4 -mmovbe` NEUTRAL (rejected)
  - plain puredcall-clang: 49.65/49.20
  - earlier loaded-box numbers (~17) were ~65% host contention, not code
- sea/filesel: cap or near-cap at low load (filesel 56.79 @ load 15.5; sea 45-65 mixed during build spike)

### Levers landed by executors
- epoch-channel runner (ppc_dispatch_epoch armed): +14-19% on outset (pre-epoch runner expired every verdict cache per batch — 98.4% miss rate). THE structural fix.
- MODERNGEKKO_JOURNAL_FILTER=1: +54% outset (R-executor, env-only, semantics-preserving page-mask filter).
- FAST_ELIGIBILITY default-on: +9% (committed 5308a30 on perf/r60-vendor).

### Rejected/neutral (measured, interleaved)
- icall+relcall machinery on outset: pure-vs-fat = 1-2% noise (i-cache hypothesis dead)
- -mmovbe / -march=znver4: flat
- MODERNGEKKO_CYCLE_BUDGET 4096 vs 256: flat on quiet box (earlier +15% was contention noise)
- STATICRECOMP_GUARDED_BATCH=16: flat-to-worse
- -Bsymbolic-functions: flat (PLT 289->23)
- idle-jump streak trigger: never fires (idle_jumps=0)
- HashFileSha256 worker ~9% of process samples: harmless on quiet box (separate core); no patch needed

### ABI difference resolved (not a gap)
Windows dcall.dll uses OLDER ABI (guarded_v1 only, no epoch/run_budget) — explains counter deltas (cache_direct=0, bursts=21M, native_exc=1). Same perf lineage; not a hidden fork. WW executes ~32.3K blocks/field vs our ~20.4K — finer block granularity in old ABI accounting.

### Still pending
- PGO module build: pgo-inst compiling -> profile run -> pgo-use -> gate (last big lever)
- native_exc storm (FP-unavailable+syscall, ~61/field): measured ~0.4-1% cost — real guest work, not the gap
- Final quiet-box 3-scene verification of the full stack

## PGO results (2026-09-17 ~21:00, quiet box)

Module PGO (subset-instrumented: 215 DOL-chunk/GXRuntime/glue objects, REL objects reused;
-fprofile-generate -> outset/sea/filesel train -> -fprofile-use):
- outset: 53.94 / 53.11 vs pure 49.20 / 49.38  => **+8.6% ACCEPTED**
- all clean: fallback=0 smc_failed=0 exit=0

Runner PGO (GCC -fprofile-generate/-use -fprofile-correction, same dir rebuild):
- outset: 56.65 / 55.90 vs base-runner 55.50 / 54.96 => **+2% ACCEPTED**

Full stack 3-scene (puredcall-clang + module-PGO + runner-PGO + epoch + JF + fast-elig):
- sea 59.92 (cap), filesel 59.91 (cap), outset 56.88 mean (min 35.9 = single transient)

-O3+PGO module vs -O2+PGO: 42.4 vs 42.5 => NEUTRAL, REJECTED (but see power note)

## HOST POWER CONFOUND (critical)
Laptop (Ryzen 5 8645HS APU) went to BATTERY at ~21:15. Sustained load clocks clamp to
~3.17GHz vs ~4.7-5.0GHz boost. All VI numbers after 21:15 deflate ~25% uniformly.
The 56.9/53.5/49.4 tier numbers were AC-powered; 42.x readings = battery mode.
Absolute acceptance numbers MUST be re-verified on AC. Relative A/B unaffected.

Artifacts: bench/pgo/ = {moderngekko-run-pgo, module.profdata, runner-gcda/}

## Headless Vulkan descriptor-pool crash — ROOT CAUSE + FIX (2026-09-17)

### Symptom
Deterministic SIGSEGV in `libvulkan_radeon.so` ~160-220s into headless
outset soaks (fields ~6400-6776), reproduced on baseline runner +
non-PGO module. gdb: `vkUpdateDescriptorSets` derefs NULL at
`mov 0x40(%r8)` with r8=0; dstSet member of the write was VK_NULL_HANDLE.

### Root cause
`Presenter::Present` early-returns when `g_gfx->IsHeadless()`
(Present.cpp:999) → `VKGfx::PresentBackbuffer()` never runs →
`CommandBufferManager::SubmitCommandBuffer(..., advance_to_next_frame=true)`
never fires → `FrameResources.descriptor_pools` are never reset/destroyed
→ every dirty-binding draw allocates fresh sets → pools accumulate
(~1M sets, observed pools=1023) → `vkCreateDescriptorPool` fails →
`AllocateDescriptorSet` returns VK_NULL_HANDLE → NULL dstSet into
`vkUpdateDescriptorSets` → RADV NULL deref.

Headless-only: headed mode calls PresentBackbuffer per present and resets
pools per frame. Explains why the Windows headed run (59.9 VI/s cap,
same module) never hit it.

### Fix (vendor/dolphin)
- `VideoCommon/Present.cpp`: headless still calls
  `g_gfx->PresentBackbuffer()` — advances the GPU frame (pool reset +
  deferred destruction + fence tracking) without a swapchain.
- `VKGfx::PresentBackbuffer`: null-check `m_swap_chain` (headless has
  none; IsHeadless()==swapchain==nullptr).
- `StateTracker::Update{GX,Utility,Compute}DescriptorSet` now return
  bool; NULL set after alloc → return false → `Bind()`/`BindCompute()`
  return false → `VKGfx::Draw/DrawIndexed/DispatchComputeShader` skip
  the draw. Dirty flags preserved → retried next draw. Defense in depth.
- `CommandBufferManager::AllocateDescriptorSet`: stderr diagnostics on
  unexpected VkResult (throttled to 16) + `m_descriptor_set_count`
  ratchet capped at 16*DESCRIPTOR_SETS_PER_POOL.

### Verified
- Pre-fix: SIGSEGV at ~fields 6756/6776/6440 (3 repros incl. baseline).
- Post-fix: 600s outset soak, exit_code=0, 865M native blocks,
  0 descriptor errors, pools reset per frame.

### Files
- vendor/dolphin/Source/Core/VideoCommon/Present.cpp (headless advance)
- vendor/dolphin/Source/Core/VideoBackends/Vulkan/VKGfx.cpp (null guard)
- .../Vulkan/StateTracker.{h,cpp} (bool returns + NULL guards)
- .../Vulkan/CommandBufferManager.cpp (diagnostics + cap)
