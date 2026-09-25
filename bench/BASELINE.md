# Phase-0 Performance Baseline — Linux native, headless

Date: 2026-09-11. Host: 12-core x86-64, 22 GB RAM, NTFS data volume
(no perf/valgrind/ltrace; profiling via `mg_sampler.so` SIGPROF + symbolizer).

## Harness

`bench/mg_bench.sh` — isolated user-dir, `[vi]` readiness marker, warmup then
measured window, SIGTERM stop, per-thread `/proc` CPU sampling, optional
`--sampler` (RIP samples + maps for offline attribution via
`bench/mg_symbolize.py`), `flock` serialization on `/tmp/mg-bench.lock`,
heartbeat watchdog (`MG_HEARTBEAT_S`, default 600 s).

`bench/rebuild_module.sh` — module builder; verified recipe:
`dol-inline/generated` + `rel-out-p14/generated/rels` + GXRuntime core
(object-identical reproduction of `gGZLE01_recomp.so.inline` inputs).

## Benchmark fixtures (verified to load on `gGZLE01_recomp.so.inline`)

| Name | State | Character |
|---|---|---|
| `sea`   | `port/state/seademo-entry.state`      | heavy sea_T attract scene |
| `outset`| `port/state/outset-day-interim.state` | heaviest (actor-dense village) |
| `filesel`| `port/state/fileselect-entry.state`  | medium/menu |

## Baseline results (60 s measured, warmup 15 s)

| Run | mean VI Hz | field ratio | rt_ratio (run) | notes |
|---|---|---|---|---|
| sea capped          | 25.75 | 0.43 | 0.418 | slept_ms=0 (starved) |
| sea capped no-mods  | 26.68 | 0.44 | 0.430 | mod hooks cost ~3.5% |
| sea uncapped+accum  | 10.39 | 0.17 | 0.204 | **see F-UNCAP** |
| outset capped       | 16.41 | 0.27 | 0.225 | heaviest scene |
| filesel capped      | 26.39 | 0.44 | 0.418 | menu-ish |

Shutdown counters every run: `fallback=0 smc_failed=0`, `hook_fb=0..23`,
`native` dispatches 270M–557M per run. Zero interpreter steps in measured
scenes — **native coverage is already ~100% on the DOL+REL hot path**;
`jit:anon` sampling shows ~0.4%.

## CPU attribution (sea capped, sampler 499 Hz, whole-run)

| Tier | share |
|---|---|
| recomp .so (generated code) | 44.7% |
| runner (chassis/dispatch/hooks) | 29.2% |
| libs (vdso 14.1%, libc 7.6%, gpu/misc) | 25.8% |
| jit:anon | 0.4% |

Top offenders inside those tiers:
- `[vdso]` 14.1% — clock reads (`Common::Timer::NowMs` etc.): every
  dispatch/VI probe pays a time query. Cheap-clock or caching opportunity.
- `Sha256Portable` 11.7% — startup game-integrity hash of ~2 GB assets;
  concentrated in load phase, not steady-state gameplay. (Not a per-frame
  cost, but it does inflate sample share; windowed analysis via `--from/--to`.)
- `func_803056E0` (inside OSUnlink range) 10.0% — guest OS thread-list churn.
- `rel_loader_write_journal` 3.5% — SMC write journal.
- chassis cluster `Run`+`InvalidationGuard`+`ModDispatch`+`HandlesAddress`+
  `ResolveNativeAddress`+`ChunkIndexOf`+dispatch-lambda ≈ **8–9%** —
  per-block-boundary bookkeeping; block-linking territory.
- `libc` 7.6% — memcpy/memset/strlen spread across everything.

## Findings

- **F-UNCAP**: `--uncapped` (accumulator engaged, confirmed by
  `[frame-uncap]` line) in a CPU-bound scene cuts field delivery ~2.5×
  (25.8 → 10.4 VI Hz) and raises native dispatches (360M → 536M).
  Free-running render-only iterations consume the scarce host-CPU budget;
  uncapped only pays when guest RT ≥ ~1.0. Candidate improvement: an
  accumulator that self-throttles render iterations toward VI rate when the
  guest is starved (keeps 30 Hz logic, stops burning host cycles).
- Guest emulation ~0.42× RT in sea/filesel, ~0.23× in outset — the CPU-bound
  wall is the guest CPU path itself, not pacing.
- `fallback=0` everywhere: Agent-2's "more native coverage" premise is
  largely pre-satisfied; the remaining lever is chassis/dispatch overhead
  (~29% runner share) and hook-handling cost (`hook_fb`, `Dispatch`,
  `HandlesAddress`, guards).
- `[thr] slept_ms=0` in all capped runs — the throttle never sleeps; the
  emu thread is the bottleneck.
- Warm-vs-cold cache and Wine numbers in
  `windows-optimization/PERF-RESCUE-LEDGER.md` apply to that lineage; on
  this Linux tree the same code runs ~parity in matched scenes (F17).

## Environment quirks for agents

- NTFS volume: **no symlinks**; `LD_PRELOAD` paths must be space-free
  (`mg_sampler.so` is staged into `~/.cache/mg-bench/` by mg_bench).
- Disk: ~7.8 GB free — keep per-agent obj dirs, don't duplicate generated
  trees unless needed; `--dol-gen`/`--rel-gen` default to the verified
  `.inline` inputs (shared, read-only).
- No perf/valgrind; use `--sampler` + `mg_symbolize.py`, `MODERNGEKKO_PERF_COUNTERS=1`,
  and the shutdown counters. `gprof`, `strace`, `gdb` exist.
