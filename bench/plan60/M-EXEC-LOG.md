# M-EXEC-LOG — Executor M (module/codegen) work log

Machine: Ryzen 5 8645HS, co-tenant loaded (12–26). All A/B interleaved under
load; mg_bench.sh serializes on /tmp/mg-bench.lock. Reference quiet numbers:
.opt sea 38.18 / outset 24.79 / filesel 38.94 VI Hz.

## 2026-09-17 — M1: dol-inline-opt-dcall module build

- In-flight build completed OK: objdir `/tmp/mg-bench/obj-dcall` (1172 objs),
  output `ZeldaDecompile/port/game/gGZLE01_recomp.so.dcall` (378,682,176 B).
  Recipe per PLAN-60 §dcall: dol gen = `build/dol-inline-opt-dcall/generated`,
  rel gen = `build/rel-out-p14/generated/rels` (UNTRANSFORMED), glue =
  `port/game/src/module_glue.c` (HEAD, MG_GLUE_DIAG-gated rdtsc/phist),
  `-include /tmp/cycle_budget.h -DMODERNGEKKO_INLINE_XLAT_FULL`.
- Export check (`nm -D`): 889 dynsyms incl. `staticrecomp_get_module`,
  `ppc_set_native_check`, `ppc_dispatch_epoch`,
  `staticrecomp_dispatch_guarded_v1`, `ppc_smc_*`, plus new-channel symbols
  `g_mg_dcache_gen`, `g_rel_dispatch_gen`, `mg_lookup_fn`,
  `dolrecomp_cycle_budget` (confirms force-include took).
- Transform spot-check (`chunk_0193_text1_803056E0.c:5490+`): `bl` sites now
  emit replacement-first → epoch-pinned `native_ok` → `call_enter` depth guard
  → direct `func_T(ctx)` → continuation `goto label_CONT` (fb-fallthrough);
  `s_okgen_dc*`/`s_ict_dc*` present in all 206 chunks (675 sites in the OS
  chunk alone).
- Smoke (20 s, capped, sea, load ~19-26): **mean 59.61 VI Hz, min 53.70,
  max 60.20; fallback=0 smc_failed=0 exit=0**. vs .opt quiet ref 38.18 →
  +56% and at/near cap. bench-out: `m60-smoke-dcall-sea-20260917-111925`.
- Formal gate running: ABAB ×2 reps × {sea,outset,filesel} × {opt,dcall},
  60 s capped — `/tmp/m60/ab-gate.log`.

## Pending queue
- M2 `-fvisibility=hidden` on dcall module (dlsym surface = 5 symbols, all
  `MODERNGEKKO_MODULE_EXPORT`-annotated → safe; expect ~879 dynsyms → ~6).
- REL-side relcall: `bench-out/apply-relcall.pl` on a copy of
  `rel-out-p14/generated/rels` → `--rel-gen` swap (outset is REL-heavy).
- M3 PGO two-build; M4 CYCLE_BUDGET env sweep (no rebuild); M5-M8 codegen
  levers; M9 pcelide eval.

## 2026-09-17 (cont.) — ROOT CAUSE CANDIDATE: dispatch-epoch never armed (runner predates channel)

Evidence chain:
- `[glue-diag]` on outset dcall (MG_GLUE_DIAG run `outset-exc-diag-20260917-150555`):
  `segs=9,457,386 genmove=9,457,386` — generation bumped on EVERY segment →
  `hit=234,466 miss=14,746,796` (98.4% miss), `nok=65,690,135` (≈1.6M
  `g_mg_native_ok` host callbacks/sec — every epoch-pinned site static
  re-verifies once per segment), `calls/seg=1.58`.
- `module_glue.c:276-278`: `if (!s_dcache_epoch_armed) ++s_dcache_gen;` — the
  conservative fallback for runners without the epoch channel. With a stale
  runner it fires per `glue_run_budget_impl` call = per ~256 guest cycles.
- `ppc_dispatch_epoch` is exported by the .so (nm: `T ppc_dispatch_epoch`)
  but the installed runner `build/moderngekko-run` (Sep 11 15:29) has ZERO
  `ppc_dispatch_epoch` strings — binary predates the epoch channel
  (StaticRecompCore.cpp:594-603, uncommitted submodule work, Sep-13+).
  => `s_dcache_epoch_armed` stays 0 forever.
- Windows dll verified semantically IDENTICAL codegen (dcbf→ppc_fallback_instruction,
  sc→ppc_system_call_exception, per-FP-instr lazy-FP check incl. refptr load at
  `18c482518`) — module-side parity. The 60-VI Windows runs used a post-loopfix
  runner (Sep-13+); our Sep-11 runner lacks BOTH the epoch channel AND the
  `staticrecomp_dispatch_guarded_v1` continuation loop.
- Sep-6 Windows `module_glue.c` (windows-optimization/module/clock-gate/) has
  no dcache/epoch machinery at all — Windows ran a simpler dispatch.
- Action: `cp moderngekko-run moderngekko-run-pre-epoch`; `make moderngekko-run -j4`
  rebuilding (log /tmp/mg-runner-rebuild.log). Then gate old-vs-new runner on
  outset with .dcall before the clang-vs-gcc module question — this is the
  first concrete structural defect matching "dcall doesn't help outset".

## Counter-delta resolution (Windows dcall.dll vs our .dcall, outset)
- `cache_direct`: ours counts `ppc_fallback_instruction`→`cpu->cache_control`
  (R2 direct path, cpu.c:189-205); Windows runner may not set the hook → ops
  land in instruction_fallback instead. Counter plumbing, not codegen.
- `native_exc` 82k vs 1: same module-side raise paths; likely runner-side
  counting/lineage difference OR different measured window. Unresolved but
  module is not the variable.
- `cycles/field` 5.51M vs 8.28M: Linux undercharges ~32% — VI cadence ahead
  of charged time; consistent with slow-host execution, not fake speed.

## 2026-09-17 — Generated-tree lineage discovery (decisive for outset)
- ALL Windows experiment dlls (ww-winbuild candidate, build-clang,
  judge-fastpath, journal-*, clock-gate) were compiled from
  `port/build/dol/generated` — a PURE TRAMPOLINE tree (0 dcall/icall sites):
  every `bl`/`bctrl` is `lr=X; pc=target; return` (3 instrs).
- The Windows 59.9-VI/s outset module `gGZLE01_recomp-dcall.dll` (sha
  81CBC06A) has ONLY s_okgen sites = apply-dcall.pl output BEFORE
  apply-icall.pl/apply-relcall.pl existed. .text 206MB vs our fat 306MB.
- Our `dol-inline-opt-dcall` fat tree emits ~100-line single-entry-monomorphic
  blocks at EVERY `bctrl`/`bclrl` (chunk_0144:5828 example). On POLYMORPHIC
  sites (outset's cNdIt_Judge/cTgIt_JudgeFilter function-pointer iteration —
  phist #1/#2) the cache misses ~always, so each call pays MORE than a
  trampoline: dispatch_replacement icall + native_ok icall + find_original +
  enter/leave, then still calls in. ~100MB dead code also pressures
  I-cache/BTB across all chunks.
- Puredcall tree ready: `port/build/dol-puredcall/generated` = apply-dcall.pl
  on dol-inline-opt (52,476 `bl` dcall sites; `bctrl` stays trampoline — same
  as Windows). REL side = rel-out-p14 untransformed (same as Windows).
- Hypothesis under test (build B, puredcall-clang): outset's polymorphic
  bctrl paths recover toward 40+ VI/s once the icall machinery is gone.
- Windows build flags: `-O2 -mmovbe -ffp-contract=off -fno-fast-math
  -DMODERNGEKKO_INLINE_XLAT_FULL -DDOLRECOMP_ENABLE_REPLACEMENTS`. Our gcc
  build lacks -mmovbe (0 movbe in .dcall .so); mem helpers identical
  (cpu.h same file both trees).

## 2026-09-17 16:48 — clanggate outset ABBA (fat .dcall: gcc vs clang)
Runner: moderngekko-run-pre-epoch (Sep-11 binary, abs path). taskset 0-5.
--capped --duration 60 --warmup 15 --perf. Load ~15-20 (Executor R fin1/fin2
benches + puredcall build co-tenancy; absolute VI depressed, deltas valid).

| arm | run | VI/s | guest cyc | native | native_exc | cache_direct | bursts | fb | smc |
|-----|-----|------|-----------|--------|-----------|--------------|--------|----|-----|
| gcc   | a | 14.84 | 9.27G  | 34.37M | 102,745 | 7.82M  | 1.49M | 0 | 0 |
| clang | a | 20.31 | 12.66G | 46.92M | 139,940 | 10.67M | 2.03M | 0 | 0 |
| clang | b | 18.47 | 11.56G | 42.86M | 127,901 | 9.75M  | 1.85M | 0 | 0 |
| gcc   | b | 13.55 | 8.47G  | 31.38M |  93,792 | 7.14M  | 1.36M | 0 | 0 |

- gcc mean 14.20, clang mean 19.39 → **clang +36.6% on outset**. All exits 0.
- Compiler helps materially but outset stays ~19 « 40 → compiler is NOT the
  root cause; module STRUCTURE (fat icall) hypothesis stands for puredcall.
- Clang's higher native_exc/burst counts scale with its higher throughput
  (same per-cycle rates) — exceptions are not the discriminator.
- .dcall-clang = 316.8MB (vs 378MB gcc), sha c121500d62fd9e29..., 0 movbe
  (both — generic x86-64), exports verified (dispatch_guarded_v1, epoch, etc).
