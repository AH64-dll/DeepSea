# Integration log — perf/integration (parent) + perf/integration-vendor (vendor)

Baseline: stage-b-wip fd6ed54 / vendor perf-base-snapshot 6fe991a.
Baselines (quiet machine, 60 s capped): sea 25.75 / outset 16.41 / filesel 26.39 VI Hz.

## Stacked commits

### vendor (perf/integration-vendor, on 6fe991a)
1. `ebed829` — VideoCommon zero-height EFB SIGFPE guard (A1 `f5675fd`)
2. `119675a` — execution-tier + fallback-reason counters (A2 `1e53e6e`)
3. `a6df33d` — SMC guard DOL envelope + sorted REL-section index + chunk reuse (A2 `907002b`)
4. `a54ccae` — batch-dispatch channel export `ppc_set_native_check` (A1 `8c2fcd6`)
5. `fd5291b` — **orchestrator fix**: disarm `m_rel_lazy_arm` while batch channel armed
   (review §3.1 blocking interaction — batched OSLink PCs never reach the lazy-arm trigger)
6. `47d3e6b` — setenv(overwrite=0) defaults: JOURNAL_FILTER + INLINE_XLAT + REL_LAZY_ARM
   (A2 `39721af`; deliberately AFTER the co-gate so batch builds disarm lazy safely)

### parent (perf/integration, on 154bb3a)
- `3694b0f` — flat sorted hook/patch membership (A2 `d0b2f6c`)
- `db41af9` — parallel+streaming asset hashing `MODERNGEKKO_HASH_THREADS` (A3 `46fc3c7`)
- `1d733a4` — InspectGame dedup via `preinspected_metadata` (A3 `067378d`)
- `9ab4ccb` — netplay lobby dedup (A3 `477fc5b`)
- `a9a4405`+`30a53d1` — A3 report docs
- vendor gitlink → integration-vendor tip; `bench/REVIEW-REPORT.md`

### module `gGZLE01_recomp.so.opt` (not a git artifact — recipe)
- tree: `port/build/dol-inline-opt/generated` = dol-inline + `a1-generated-h-batch.patch`
  + `a1_mem_direct.sh` transform + **orchestrator iteration cap** (review §3.2, i<4096)
- glue: `wt-a1-codegen/modsrc-a1/module_glue.c` (batch wrapper, --glue override)
- flags: `-DMODERNGEKKO_INLINE_XLAT_FULL`
- rels: `rel-out-p14` (unchanged)

## Gates (each vs the committed baseline, quiet machine, 60 s capped)

| step | config | sea | outset | filesel | verdict |
|---|---|---|---|---|---|
| baseline | stage-b-wip + .inline | 25.75 | 16.41 | 26.39 | — |
| gate-1 | integ runner + .inline (batch inert) | 26.11 | 16.50 | 26.69 | ~neutral on quiet (A2 wins are contention-sensitive) |
| gate-2 | integ runner + .opt (batch armed) | **38.18** | **24.79** | **38.94** | **+48.2% / +51.1% / +47.6%** — MERGE |

All gate-2 runs: `fallback=0`, `smc_failed=0`, `exit_code=0`,
`tier-share native=99.98%`, REL discovery via legacy refresh confirmed
(`RefreshRelSections module 1: 1 valid candidate(s)`), co-gate log line
`rel-lazy-arm disarmed (batch channel armed)` present.

## Post-gate validation

- 150 s uncapped+accum .opt sea: clean exit, fallback=0 — but exposed a
  **pre-existing F-UNCAP pathology** (also present on baseline runners):
  unlimited retimed wait=1 tick made R-iterations ~free in guest time,
  ~19K bookkeeping iterations/s starved the guest (L collapsed to ~5/s,
  acc unbounded). Fixed in `12b3071` — pad unlimited wait to
  >= TICK_30FPS/8 guest ticks.
- Post-fix uncapped .opt sea: **30.43 VI Hz**, ~194 iter/s (was ~19K),
  L cadence ~17/s at 8.7% (correct vs guest time), max 48.3.
- Cross-module state compat: .opt loaded an .inline-written exit.state —
  47.81 VI Hz, fallback=0, clean.
- ctest on integration build: 46/46 pass.
- bench/threads.csv field-index bug fixed (154bb3a, from A3).

## Windows native validation (2026-09-12)

Built `perf/integration` natively with LLVM-MinGW 22.1.8, Release + ThinLTO,
and rebuilt the `.opt` module with both LLVM-MinGW and WinLibs GCC 16.1.0.
The Windows module uses `dol-inline-opt/generated`, the patched A1 glue,
`INLINE_XLAT_FULL`, `-mmovbe`, and the unchanged `rel-out-p14` tree.

A headed Vulkan, adapter-1, no-mods, perf-counter A/B/B/A run used the same
Windows Outset state (`2ADDED8E` prefix; full hash in each `metadata.json`),
8 s warmup, and 30 s measured windows. The runner was held constant; only the
module changed:

| arm | VI Hz | rendered FPS |
|---|---:|---:|
| Windows package module (`981C59E5`), rep 1 | 18.400 | 9.146 |
| Windows `.opt` GCC module (`6B3A9A85`), rep 1 | 29.530 | 14.756 |
| Windows `.opt` GCC module (`6B3A9A85`), rep 2 | 28.210 | 14.044 |
| Windows package module (`981C59E5`), rep 2 | 18.486 | 9.252 |
| mean baseline | **18.443** | **9.199** |
| mean `.opt` | **28.870** | **14.400** |
| delta | **+56.5%** | **+56.5%** |

All four runs: clean exit, `fallback=0`, `smc_failed=0`, native tier share
99.98%. Captured frames show the same scene without visible corruption.
Artifacts are under `bench-out/windows-native-ab-20260912/`.

A shorter mirrored compiler comparison measured LLVM-MinGW's 189.8 MB `.opt`
module at 28.623 VI Hz / 14.197 FPS versus GCC's 232.1 MB module at 26.743 /
13.375 (about +7% / +6%), but per-run load drift was large; treat LLVM as the
candidate, not a final compiler verdict. Artifacts are under
`bench-out/windows-compiler-ab-20260912/`.

Two Windows-only crashes were found and fixed before the clean gates:
1. `HashFileSha256` placed 1 MiB and 4 MiB streaming buffers on a PE thread
   with a 1 MiB stack (`0xC00000FD`); both buffers now use bounded heap storage.
2. A zero-height EFB copy could fail `AllocateCacheEntry`, then dereference the
   null entry while deferring its RAM copy (`0xC0000005`); a missing entry now
   takes the existing immediate-flush path.

A headed 10 s quality smoke with 16:9 projection expansion, 3x internal
resolution, and 16x anisotropy completed cleanly at 31.46 VI Hz. This is a
functional smoke only, not a performance verdict.

### Final Windows A/B — packaged LLVM module (2026-09-12, quiet machine)

**Lineage clarification:** the directory `build-module-inline-opt-gcc` is
misnamed — its CMakeCache records the LLVM-MinGW 20260616 UCRT toolchain, and
its output `0E2B7D65` was the "llvm" arm of the compiler comparison. The
`windows-native-dev` package therefore ships the LLVM-MinGW `.opt` module.
The WinLibs GCC module is `6B3A9A85` in `build-module-inline-opt-winlibs-gcc`.

An 8-run interleaved A/B (base/opt/opt/base/base/opt/opt/base) used the
packaged runner (`F6D9F03E`), the same Outset state, parity-pinned user-dir
(640x528, Vulkan, adapter 1, no mods), 8 s warmup, 30 s measured windows:

| arm | VI Hz (per run) | mean VI | mean FPS |
|---|---|---:|---:|
| baseline `981C59E5` | 17.77, 19.09, 18.68, 18.98 | **18.63** | **9.30** |
| packaged `.opt` LLVM `0E2B7D65` | 36.42, 34.47, 32.59, 33.30 | **34.20** | **17.08** |
| delta | | **+83.6%** | **+83.7%** |

All eight runs: `exit_code=0`, `forced_termination=false`, `fallback=0`,
`smc_failed=0`. Artifacts: `bench-out/windows-native-ab-final-20260912/`.
The larger delta vs the earlier +56.5% is consistent with the compiler
comparison signal (LLVM > GCC by ~7%) plus run-to-run drift.

`windows-native-dev` package smoke (packaged 1080p/16:9/16x-AF user-dir,
same state, 10 s measured): 33.8 then 32.4 VI Hz, `capture_ok=true`,
`fallback=0`, `smc_failed=0`, clean shutdown. REL lazy-arm enabled then
correctly disarmed once the batch channel armed.

120 s stability soak on the packaged build (same quality user-dir + state):
**34.32 VI Hz** sustained, `capture_ok=true`, `exit_code=0`,
`forced_termination=false`, `fallback=0`, `smc_failed=0`,
native tier share 99.98% (134.5M native executions). Artifact:
`bench-out/windows-native-package-soak/20260912-055621-521/`.

### ThinLTO + x86-64-v3 module experiment — NEUTRAL (2026-09-12)

Rationale: ~76K cross-TU float-helper call sites (`ppc_fmul`, `ppc_fsubs`,
`ppc_fcmp`, …) in generated code; ThinLTO could inline them and fold literal
register indices. `-march=x86-64-v3` adds BMI2/AVX2 (rotate/shift fusion) on
top of the already-required MOVBE baseline. Note `-fvisibility=hidden` was
considered then dropped: PE intra-DLL calls are already direct (no ELF PLT
model) and the `.def` already pins exports.

Build `build-module-inline-opt-llvm-v3`: Clang 22.1.8 (llvm-mingw 20260616),
`-O2 -mmovbe -march=x86-64-v3 -flto=thin --thinlto-jobs=6`, same
`dol-inline-opt` + `rel-out-p14` trees + A1 glue. Output 186.4 MB, sha256
`1302BC03…` (full hash in artifact metadata). Link warnings only:
pre-existing `cpu.c` comment-nesting and `rel_loader.c` format warnings.

6-run interleaved A/B vs packaged module (same runner/state/user-dir,
30 s windows; whole session drifted ~4% cooler than the morning runs):

| arm | VI Hz (per run) | mean VI |
|---|---|---:|
| LTO+v3 `1302BC03` | 33.66, 32.54, 32.05 | **32.75** |
| packaged `0E2B7D65` | 33.05, 32.46, 31.93 | **32.48** |

Delta +0.8% — within run-to-run drift. **Not promoted.** The float-helper
call overhead is not a measurable bottleneck in this scene; the per-call
sites were already cheap. Artifacts: `bench-out/windows-lto-ab-20260912/`.

### Generated-code census → ctx->pc store elision (2026-09-12)

Census of the `dol-inline-opt` + `rel-out-p14` trees found ~1.91M literal
`ctx->pc = 0x…` stores — one per guest instruction at label top — of which
~74% are provably dead: `ctx->pc` is initialized by the dispatcher at chunk
entry and explicitly re-stored on every functional exit path (branch/dispatch
returns, loop-continuations, function tails, hook probes, exceptions). The
mid-block label-top store is consumed only by MMIO diagnostics and hooks on
the *same* instruction.

Conservative transform `ZeldaDecompile/port/build/elide_pc_stores.ps1`
removes the label-top store unless the instruction body can reach
`external_read/write` diagnostics or hooks (memory ops, `ppc_host_call`,
`fallback_instruction`, exception paths keep theirs). Result: **1,290,022
stores removed** (DOL 569,287 of 773,626 label-top; REL 720,735 of
972,034), transformed trees at `dol-inline-opt-pcelide` /
`rel-out-p14-pcelide`.

**Result — NEUTRAL (2026-09-12).** Module `build-module-pcelide-llvm`
(Clang 22.1.8, same flags/glue as packaged, only the elided sources differ):
181.9 MB, sha256 `32fb4568…`. Correctness audit before building: all
`ctx->pc` reads are dispatch-entry routing (`switch`/`if..goto`, fed by the
caller) or dynamic-branch stores; loop-call returns set pc via the loop's own
exit paths; fp-guard returns pass `cia` to `ppc_take_exception`; bare
downcount/`return` bodies all carry explicit stores. Single-file
`-fsyntax-only` clean.

8-run interleaved A/B vs packaged (same runner/state/user-dir, 30 s windows):
pcelide 32.24/32.42/32.33/32.48 → **32.37**; packaged 32.50/32.77/32.73/32.37
→ **32.59**. Delta **-0.7%**, within drift. All runs `exit=0`,
`forced=false`, `fallback=0`, `smc_failed=0`. Artifacts:
`bench-out/windows-pcelide-ab-20260912/`. **Not promoted** — the dead stores
were already free (store-buffer + same L1 line absorbs one store per ~5-instr
block); the census's "dead" property held but carried no measurable cost.
vi-perf attribution from the smoke: native_ms ~84% of frame time,
core_timing ~12%, dc_hook ~2.4%.

## Notes / rejected

- A1 Exp D (cross-chunk chaining): rejected by agent (slower than batch alone, +19 MB); emitter `emit_cross_chunk_call` lacks the eligibility predicate — never ship.
- BATCH_BUDGET=1024: rejected (starves CoreTiming events).
- Threading the Emuthread: rejected by A3 on correctness/ordering grounds.
- A2 sea rep spread +5.9/+15.0: baseline drift; honest ~+10%.
- `-fvisibility=hidden` on the Windows module: N/A — PE has no ELF PLT; intra-DLL calls are already direct and the `.def` pins exports.
- ThinLTO + `-march=x86-64-v3` module: measured +0.8% (noise) — not promoted.
- `ctx->pc` dead-store elision (1.29M stores removed): measured -0.7% (noise) — correct but not faster; stores were already free in the store buffer.
