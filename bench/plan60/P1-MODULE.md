# P1-MODULE — module/codegen path to ≥59.9 VI/s (sea, capped)

Date: 2026-09-17. Scope: `gGZLE01_recomp.so` build recipe, generated DOL/REL
trees, GXRuntime helpers, `module_glue.c`, `rel_loader.c`. Sibling docs:
`P3-AUDIT.md` (attribution/host), `P2-RUNTIME.md` (chassis). Read P3 first.

## 0. Where we stand (measured)

- `.opt` (sea 38.18 VI quiet / ~22–25 loaded) = `dol-inline-opt` chunks +
  `rel-out-p14` + **`wt-a1` modsrc glue+loader** + `-DMODERNGEKKO_INLINE_XLAT_FULL`,
  GCC `-O2 -fPIC`, `cc -shared *.o -lm`. Confirmed by symbols: `.opt` exports
  `dolrecomp_call_depth/g_mg_native_ok` but **not** `g_mg_dcache_gen`,
  `g_rel_dispatch_gen`, `g_rel_in_chunk`, `ppc_dispatch_epoch` — the
  dcache/dispatch-memo machinery (HEAD copies) is absent.
- Two live glue/loader pairs exist and differ materially:
  `ZeldaDecompile/port/game/src/module_glue.c` + `port/loader/rel_loader.c`
  (HEAD: epoch, `s_dcache`, seg_ret==2, guarded continuation, 90 repl cases,
  MG_GLUE_DIAG-gated rdtsc/phist) vs `port-tools/wt-a1-codegen/modsrc-a1/*`
  (what `.opt` shipped: 73 repl cases, no dcache). `rebuild_module.sh`
  defaults point at HEAD — any rebuild silently changes both files.
- **dcall port is already staged**: `build/dol-inline-opt-dcall/generated/`
  has all 206 chunks transformed (206× `s_okgen_dc`, 193× `s_ict_dc`;
  `generated.h` byte-identical to `.opt`'s), and a sibling build of
  `gGZLE01_recomp.so.dcall` was in flight at `/tmp/mg-bench/obj-dcall`
  (1172 TUs, HEAD glue+loader, rel-out-p14 **untransformed**, plus
  `-include cycle_budget.h`). REL-side `apply-relcall.pl` exists but no
  transformed REL tree exists yet.
- Windows evidence for the same transform family (`bench-out/`):
  `gGZLE01_recomp-dcall.dll` = **59.90 VI cap on all three states**
  (outset min 59.8, 90-sample soak at cap), vs `pgo3` = 42.0/19.7/42.2 and
  `-segret.dll` = 40.7 outset. `dcache.dll` (glue verdict cache only,
  old lineage) ≈ baseline+4~19% — the **in-chunk direct calls carry the
  win**, not the cache.
- Module-side cost census (`.opt` disasm, `func_803056E0` = OS chunk,
  10.7–17.5% of CPU samples): 76,871 host instrs for ~4K guest instrs
  (~19:1); **59% of its 13,120 jumps are >64 KB** (GCC -O2 split the
  cold `*_slow`/journal/reservation tails out-of-line); 1,326 PLT calls;
  3,818 GOT loads; store→load misses like `mov %eax,(%rbx);mov (%rbx),%esi`.
- Chassis dlsym surface into the module is only 5 symbols:
  `staticrecomp_get_module`, `staticrecomp_dispatch_guarded_v1`,
  `ppc_set_native_check`, `ppc_set_mem_write_journal`, `ppc_dispatch_epoch`
  (StaticRecompCore.cpp:497/571/599, _Run.cpp:82/95, Lockstep.cpp:58).
  Everything else rides the module descriptor → ~875 exported symbols are
  pure GOT/PLT tax.

## 1. Ranked levers

Expected Δ = sea VI Hz vs 38.18 quiet reference; cap-bounded at 59.9.

| # | Lever | Expected Δ | Files / functions | Correctness risk | Gate | Revert if |
|---|-------|-----------|-------------------|------------------|------|-----------|
| M1 | **dcall/icall in-chunk direct calls (DOL) + relcall (REL)** — constant `bl`/`b`/`bc` exits become epoch-pinned guarded `func_T(ctx)` calls; `bctrl/blrl` get monomorphic inline caches; module returns seg_ret==2 continuation. Windows-measured: all states pegged at cap (≥+43% sea; outset ≥+200%) | **+40–60%** (cap) | `build/dol-inline-opt-dcall/` (done), `apply-*.pl` for REL tree, HEAD `module_glue.c` (`g_mg_dcache_gen`, `s_dcache`, `ppc_dispatch_epoch`, `glue_run_budget` ret-2), `rel_loader.c` (`g_rel_dispatch_gen`, `g_rel_dispatched_*`), `cycle_budget.h` (-include) | Med: epoch must cover hooks/demote/SMC/restore (chassis bumps at StaticRecompCore.cpp:680, SMC:203/1217, Run:505 — verified); transform excludes replacement-case PCs and same-chunk targets; depth cap 24 keeps host stack safe; dcache safely degrades if chassis never arms epoch | separate `.so` build + A/B sea/outset/filesel; `fallback=0`, `smc_failed=0`, `[resl]`/hook logs present, savestate load+restore | any `fb_*`/`smc_failed` delta, missed hook (mod feature dead), state-restore divergence, outset regression vs .opt |
| M2 | **ELF visibility/export cut + `-Bsymbolic-functions`** — hide all but the 5 dlsym'd names (+descriptor); `-fvisibility=hidden` where `MODERNGEKKO_MODULE_EXPORT` marks the ABI, or a `local:*` version script. Kills ~1M PLT call edges and the GOT loads behind every extern global (`g_ppc_lazy_fp_enabled`, `g_mem_write_journal`, `g_mg_native_ok`, `g_rel_dispatch_gen`, masks) | **+2–5%** | `rebuild_module.sh` FLAGS/link line; audit `MODERNGEKKO_MODULE_EXPORT` coverage in glue/loader | Low-med: over-hiding an optional channel (esp. `ppc_dispatch_epoch`) silently disarms it → stale dcache verdicts = wrong-code risk; assert export presence post-build | `nm -D` diff (expect ~6 exports), 3-state run, `fallback=0`, epoch bump observed | missing-symbol warnings, `[nok]`/stale-verdict behavior, any VI drop |
| M3 | **Module PGO (two-build)** — GCC `-fprofile-generate/-fprofile-use` (private objdir) or Clang `-fprofile-instr-generate` + `llvm-profdata`. Fixes the 59%-far-jump block layout; Windows F53 = +5.1% (49.37→51.91, ABBA×8), pgo3 vs o3native ≈ +4% | **+3–8%** | `rebuild_module.sh` (+flags), `/tmp` profile dir | Low: profiles are toolchain/build-exact; `.gcda`/`.profraw` need clean exit (runner handles SIGTERM → atexit — verified by exit.state). Train on same lineage + state | fallback=0, smc_failed=0, VI A/B on all 3 states | gain <+1.5% on sea after ABBA×2, or any correctness counter moves |
| M4 | **Flag matrix on top of M1–M3**: `-O3`, `-march=znver4` (movbe for ~300K bswaps, BMI2), `-fno-stack-protector` (1,061 canary sites), `-fno-semantic-interposition`, `-fno-plt` (belt) | **+1–4%** | `rebuild_module.sh` FLAGS | Low: znver4 is bench-only anyway; keep `-ffp-contract=off -fno-fast-math` (Gekko FP semantics) | same A/B protocol | ≤+0.5% (noise) → keep simpler recipe |
| M5 | **Mem fast-path slimming** — every generated mem op already uses `_direct` (119,696 r32 / 93,855 w32 / etc.; the `*_slow` calls in disasm are cold tails inside inlined helpers, ~260K static sites ≠ runtime rate). Residual fat: `ctx->ram` reloaded per op (×2 on rmw), MEM1+EXRAM two-branch probe, journal gate ~6–9 insns/store. Hoist `u8 *ram = ctx->ram` per function (generator) or fold gate globals | **+1–3%** | `cpu.h` `mem_*_direct` (~188–580), `DolRecomp` generator emit, `rel_loader.c` journal mask | Med: must preserve reservation-clear → write → journal ordering, `(u32)-1` EXRAM journal no-op, MMIO slow tail | counter gate first (M6): if slow-path entries <1% of mem ops, bound gain accordingly | any R→L mirror/SMC/journal miss, JUTEX delta |
| M6 | **Batch-exit / path counters (dev-only, never promoted)** — count run_budget exits by cause (ret0/exc/budget/iter-cap) + slow-helper entries by width + `g_mg_native_ok` returns. HEAD glue already has `s_diag_ret0/exc/hit/miss` behind `MG_GLUE_DIAG`; port minimal counters to wt-a1 or use HEAD glue | 0 (instrument) | `module_glue.c`, `cpu.c` slow helpers | None if dev-gated; must not ship in promoted `.so` | stderr dump only | n/a |
| M7 | **`func_803056E0` (OS chunk) semantic specialization** — it IS guest scheduler code (SelectThread/OSThread/OSMessage, 0x803056E0–0x803096E0); the spin 0x80307EF4 is an in-chunk `goto` loop (agent-b verified), exits only on the 256 budget. **Do not hand-specialize**: guest aliasing/interrupt semantics; the fix is M1 (call overhead) + P2-R1 (idle time-jump). Re-sample after M1; only revisit if still >8% | defer / ≤+2% | `chunk_0193_text1_803056E0.c` (~39.9K lines) | High if touched | — | — |
| M8 | **Whole-module GCC LTO** — LLVM ThinLTO+v3 measured +0.8% (noise); GCC LTO at 1,172 TUs/238 MB is heavy; expected ≤+1% | **≤+1%** | build recipe | Low | A/B | always (cost>gain) |
| M9 | **BOLT post-link** — needs `-Wl,--emit-relocs` relink; instrument→run→update; after PGO the residual layout win is small | **+1–3%** | link line + llvm-bolt | Low-med: 238 MB .so memory/time; verify dlopen + symbol integrity | A/B | build fragility or <+1% |

## 2. Per-area findings

1. **PLT/visibility**: `nm -D` shows ~879 dynamic symbols (~748 `func_*` + ~120
   runtime); the simplistic `objdump -R` PLT-count was a red herring — the
   real census is the disassembly: ~1.42M `call`s, ~1.015M targeting
   extern/PLT-bound names (top: `ppc_fp_raise_unavailable` 286K,
   `mem_*_slow` ~630K combined, `ppc_fmul*`/`fadds`/`fsubs`). These are
   mostly *cold tails inside inlined helpers* + cross-TU calls — all become
   direct once intra-`.so` binding is local. Expected = M2.
2. **PGO**: already measured on Windows (+5.1%, F53; pgo3 42.0 sea). Linux
   needs the two-build flow; nothing in the way (runner exits cleanly on
   SIGTERM → gcov/profraw flush). Toolchain-locked.
3. **Mem-direct coverage**: source-level coverage is 100% (`_direct` only);
   runtime slow-path rate unknown — instrument (M6) before optimizing (M5).
   `g_exram_direct_hits` ≈ 646K/run exists; add slow-entry counters.
4. **Batch exits**: ~260 guest-cyc/segment ≈ budget-256 dominated
   (`jit_entries` ~100K/run ≈ 0.2% of segments; exceptions ~0.15%). Exits
   are *cheap and correct*; don't chase exit frequency — chase per-exit
   cost (P2) and remove exits via M1's in-chunk chaining.
5. **`func_803056E0`**: whole OS chunk; SelectThread spin is internal; the
   10.7–17.5% is genuine guest work + its own call overhead — M1 + P2-R1
   address both. No safe module-side specialization.
6. **Asm quality**: `func_803056E0` quantified above (~19:1 host:guest,
   59% far jumps, store→load misses, 5-push prologues on
   `dolrecomp_dispatch_replacement`). Levers: M3 layout, M2 call/global
   tax, M4 micro-flags. Next two hottest: `func_803256E0`, `func_802D56E0`
   (2.5–2.6% each) — same pattern expected; audit after M1 rebuilds.
7. **Flag matrix**: current = `-O2 -fPIC -ffp-contract=off -fno-fast-math`,
   separate compile, `cc -shared *.o -lm` (no visibility/LTO/relocs). ThinLTO+
   v3 (+0.8%) and O3-full (≈shipped) were already noise on Windows — but that
   predates the batch/dcall lineage; retest cheap variants once, don't assume.

## 3. What previous work got wrong / already disproven

- Cross-chunk chaining (A1 Exp D): slower, +19 MB — banned; dcall is its
  corrected descendant (epoch-pinned per-site verdicts, not blanket chaining).
- Batch budget 1024: slower, starves CoreTiming (b1024 runs confirm); keep 256.
- `ctx->pc` dead-store elision: 1.29M stores removed → **−0.7%**. Dead stores
  were store-buffer-free. Don't redo (`dol-inline-opt-pcelide` tree is it).
- Native coverage: `fallback=0`, tier share 99.98% — coverage is not the wall.
- Windows `-fvisibility=hidden` N/A verdict does **not** transfer: ELF got the
  PLT/GOT tax PE never had → M2 is Linux-only upside.
- ThinLTO + `-march=x86-64-v3`: +0.8% noise → don't expect flags alone.
- Glue-side dcache alone ≠ the win (Windows `dcache.dll` ~baseline+4–19%);
  the in-chunk calls are the mechanism.
- PGO is *measured*, not speculative (F53 +5.1%); don't re-describe as untried.
- `.opt`'s glue is **wt-a1**, not HEAD — editing `port/game/src/module_glue.c`
  expectations onto `.opt` misattributes; and `rebuild_module.sh` defaults
  silently pull HEAD (which is what the in-flight dcall build wants anyway).

## 4. Landmines

- **L-A**: sibling build `/tmp/mg-bench/obj-dcall` → `gGZLE01_recomp.so.dcall`
  uses HEAD glue — fine (rdtsc/phist are behind `MG_GLUE_DIAG`, s_diag_on;
  unconditional parts are only `++s_diag_*` counters + the dcache itself),
  but note `.opt` never carried them → A/B is vs a *different glue*, and the
  diag counters cost ~1–2% if the arm-check isn't optimized away.
- **L-B**: `-include /tmp/cycle_budget.h` is required for the dcall chunks
  (they read `g_mg_dcache_gen`/`g_rel_dispatch_gen`/`mg_lookup_fn` and the
  budget becomes the `dolrecomp_cycle_budget` global). Missing it = link fail.
- **L-C**: REL tree is untransformed in the in-flight build — outset/filesel
  will lag sea; `apply-relcall.pl` on a rel-out-p14 copy is the follow-up.
- **L-D**: never `--out` a pinned name; keep objdirs private; state files are
  lineage-locked — all rebuilds here keep `dol-inline-opt`+`rel-out-p14`
  coverage so existing states remain loadable.
- **L-E**: `MODERNGEKKO_JOURNAL_FILTER=1` is default-armed via chassis
  `default_env` (StaticRecompCore.cpp:216-219) — journal already 0.4%;
  don't re-litigate, only slim residual (P2-R5).

## 5. Ordered execution sequence

1. Let the in-flight `gGZLE01_recomp.so.dcall` finish; bench it via
   `mg_bench.sh` (3 states, ≤60 s each) vs `.opt` — expect sea near cap.
2. If it lands: `apply-relcall.pl` → `rel-out-p14-dcall` tree, rebuild, re-bench
   (outset is the REL-heavy witness).
3. Add M2 visibility/export cut on the *dcall* module (the transform adds
   `func_T` cross-calls — all hideable); verify `nm -D` + epoch presence.
4. M3 PGO on the dcall module (train on sea; verify outset/filesel).
5. M4 flag matrix incremental (-O3/znver4/nostackprot) — keep what A/Bs ≥+1%.
6. M6 counters → M5 slimming only if slow-path rate justifies.
7. Re-sample; if `func_803056E0` still >8%, escalate the idle-spin lever (P2-R1).
8. BOLT last, only if still short.
