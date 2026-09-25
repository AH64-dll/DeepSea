# EXECUTOR-M-LOG — module/runtime perf gates (outset focus)

Machine: Ryzen 5 8645HS (Zen4, powersave governor, ~2.0-4.2GHz mixed).
Bench: mg_bench.sh, taskset 0-5, --duration 60 --warmup 15 --capped --perf.
State: outset-day-interim.state. Runner: moderngekko-run-pre-epoch (abs path).
Clean-run gates: fallback=0 smc_failed=0 exit_code=0 on ALL arms.

## 2026-09-17 — decisive outset findings

### 1. Module lineage: puredcall-clang LINKED, byte-identical to purehybrid
- `gGZLE01_recomp.so.puredcall-clang` = 311,721,056 B, sha256 e5959896ca1dd4b6.
- Parallel "purehybrid" build (pure DOL chunks over completed objdir) produced
  the IDENTICAL sha256 — purehybrid ≡ puredcall-clang. Build recipe:
  `dol-puredcall/generated` (apply-dcall only: 52,476 s_okgen dcall sites,
  bctrl = bare trampoline) + `rel-out-p14` RELs + clang -O2.
- `_rc` relcall caches live ONLY in REL objects (rel-out-p14 was built with
  apply-relcall): 71,508 s_ict_rc/s_icf_rc/s_icg2_rc syms; DOL chunks = 0.
- DOL diff fat↔pure = exactly 193 chunks (~5MB .text delta).

### 2. ABBA outset: fat-clang vs pure — lineage delta ≈ noise, LOAD dominates
| arm | VI/s | load@start |
|-----|------|------------|
| fat r1a  | 32.07 | 6.98 |
| pure r1a | 34.64 | 7.34 |
| pure r1b | 34.93 | 6.03 |
| fat r1b  | 36.71 | 5.29 |
| fat r2a  | 38.21 | 4.78 |
| pure r2a | 39.11 | 4.01 |
| pure r2b | 39.83 | 2.37 |
| fat r2b  | 40.15 | 1.72 |
VI/s ≈ 40 − 1.4×load. Pure−fat ≈ +1-2% (noise). fallback=0/smc=0 all arms.
Evidence: bench-out/m60-hybrid-{fat,pure}-outset-r{1,2}-20260917-*
**Verdict: DOL icall machinery is NOT the outset bottleneck. Host contention
was the dominant confound in ALL prior comparisons (earlier "fat=14-19" was
measured at load 15-24; at load ~2 the same module does 38-40).**

### 3. Dispatch overhead excluded 3 ways
- MODERNGEKKO_CYCLE_BUDGET 256→4096 (16× fewer dispatches): no gain
  (14.24 vs earlier same-config — run at load 22; kept for record).
- STATICRECOMP_GUARDED_BATCH=16 (16× fewer chassis round-trips): 38.53 vs
  38.02 control — flat.
- -Wl,-Bsymbolic-functions relink (PLT relocs 289→23, exports intact):
  38.02 vs 37.89 control — flat. File: gGZLE01_recomp.so.purehybrid-bsym.

### 4. Idle-jump streak trigger: confirmed inert
STATICRECOMP_IDLE_PCS=0x80307ef4 + IDLE_JUMP=1 (threshold 4): idle_jumps=0,
VI 9.36 vs ctrl ~same (contended window). Streak trigger provably never
engages when work interleaves (matches P2-RUNTIME R1 note).

### 5. Host-side profile (mg_sampler, outset fat-clang, 20K samples)
- Tier: recomp 53.4% / runner 25.7% / lib 20.4% / jit 0.6%.
- Emu thread (78% of samples): recomp 68.7%, dispatch layer ~8%
  (ResolveNativeAddress 4.4% + ChunkIndexOf + FastDispatchable +
  RefreshRelSections + IsHostCall + TranslateRelAddress), libc 7.3%,
  vulkan+glslang 2.7%, CoreTiming/mods/fifo ~3%.
- Second thread = ~100% HashFileSha256 (LoadMetadata hashes files/ during
  the bench — external drive; ~9% of all process samples).
- Null video backend is active (--headless) yet glslang/libLLVM/RADV threads
  exist — async pipeline/drv threads; ~7% total.
- Implication: module-only pace ≈ VI/0.687 → ~40/0.687 ≈ 58 VI/s. The gap
  to 59.9 is ~69% module speed + ~31% spread-thin overhead; no single
  runner lever >10%.

### 6. movbe: the last untested Windows flag (IN PROGRESS)
- Linux modules built WITHOUT -mmovbe: 775 bswap vs 3 movbe in a 256KB
  .text slice. Every guest mem op = load+bswap (2 instrs + dep stall).
- Windows used -mmovbe (fused load+swap). Zen4 supports movbe.
- Rebuild launched: puredcall + `-march=znver4 -mmovbe -O2` -j14 →
  gGZLE01_recomp.so.puredcall-movbe (objdir /tmp/mg-objdir-movbe).
  read_be32 = memcpy+__builtin_bswap32 (canonical idiom clang fuses).

### 7. Rejected/inert this session
- Epoch-pinning runner + s_dcache_epoch_armed probe: no positive effect;
  epoch runner itself slower (~8 VI/s contended). NOT accepted.
- Idle-jump streak trigger: never fires (0 jumps).
- Cycle budget 256→4096: flat.
- Guarded batch 16: flat.
- -Bsymbolic-functions: flat.
- DOL icall→trampoline (pure): +1-2% only.

### 8. sea/filesel on purehybrid (savestate-restore verified)
- filesel: mean 56.79 (min ?, max ?) at load 15.5 — at idle reaches cap.
  native=109.4M, fallback=0 smc=0 exit=0. field_ratio=0.947.
- sea: mean 45.49, min 23.90, max 65.20 at load 5-15 (movbe build spike);
  clean counters, savestate loads. Expect 59.9 idle.
- bench-out/m60-pure-sea-20260917-180216, m60-pure-filesel-20260917-180347.

### 9. Cycle-budget re-test under contention (load ~17.5)
- b4096: 23.67 VI/s (native=3.68M) vs b256: 20.57 (native=47.4M) — +15%.
- Second data point (earlier load-22 window): 14.24 vs 8.59 — also favors 4096.
- Under host contention, bigger budget helps; idle value TBD after movbe.
  Constraint stands: keep 256 in artifacts until a clean idle re-gate.

### Pending
- movbe module ABBA vs puredcall-clang at idle.
- Idle re-gate of budget 4096 vs 256 (post-build).
- If movbe lands >45: consider -O3 sweep + hash-thread suppression
  (MODERNGEKKO_HASH_THREADS or deferred InspectGame) for the final push.
