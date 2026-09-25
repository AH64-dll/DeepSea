# Independent Review — Optimization Branch Audit (a1 / a2 / a3)

Date: 2026-09-11. Reviewer: independent audit agent (read-only; no fixes applied).

Scope: three optimization worktrees against the `perf-baseline` tree
(runner `fd6ed54`, vendor `6fe991a`), benchmarked on GZLE01 headless
(`--capped`, `--headless`, mods loaded, 60 s measured windows via
`bench/mg_bench.sh`). Baseline: sea 25.75 VI Hz, outset 16.41, filesel
26.39, `fallback=0`, ~45% generated code / ~29% runner chassis / ~26%
libraries / 0.4% JIT.

Method: every patch was read as a diff (`git show -p`), key claims were
traced to running code in each worktree, and the bench artifacts under
`bench-out/` were re-checked (`summary.txt` + `metadata.env` — runner and
module sha256 confirm clean A/B pairs: same runner/state per pair, only the
named variable differs). No new long benchmarks were needed; two cheap
sanity checks were run (see below).

---

## 1. Per-patch verdict table

| # | Patch (commit) | What it does | Verdict |
|---|---|---|---|
| A1-1 | `85971da` — `bench/a1_mem_direct.sh` + `-DMODERNGEKKO_INLINE_XLAT_FULL` | Rewrites generated `mem_(read|write)(8|16|32|64)` call sites to the constant-bound `*_direct` accessors | **Safe to merge** |
| A1-2 | `1262f06` + vendor `8c2fcd6` — in-module dispatch batching (`ppc_set_native_check` / `g_mg_native_ok` / `dolrecomp_run_budget`, budget 256) | Keeps dispatch inside the module for a bounded cycle budget; chassis predicate is queried per block | **Needs changes** (two items, §3.1) |
| A1-3 | `51b43ba` — `bench/a1_chain.py` post-pass; `emit_cross_chunk_call` emitter path | Cross-chunk call chaining experiments | **Experimental only** — correctly rejected by the agent; the emitter variant must never ship (§3.2) |
| A1-4 | `f5675fd` — `TextureCacheBase.cpp` zero-height EFB guard | `num_blocks_y != 0` before dividing by it | **Safe to merge** |
| A2-1 | `d0b2f6c` — flat sorted hook/patch membership (`handled_sorted`) in `src/runtime/mod_loader.cpp` | Binary-search membership instead of two hash-map probes | **Safe to merge** |
| A2-2 | `907002b` — SMC guard envelope, sorted REL-section indexes, chunk-index reuse | Vendor `StaticRecompCore_SMC.cpp` fast paths | **Safe to merge** |
| A2-3 | `39721af` — `setenv(overwrite=0)` default-arm `JOURNAL_FILTER` / `INLINE_XLAT` / `REL_LAZY_ARM` | Turns on three pre-existing module/chassis fast paths by default | **Safe to merge, with one co-gating requirement** (§3.1 — `REL_LAZY_ARM` must not stay armed when the A1 batch channel is armed) |
| A2-4 | `1e53e6e` — tier/transition/fallback-reason counters | Always-on u64 accounting + shutdown summary | **Safe to merge** |
| A3-1 | `46fc3c7` — `MODERNGEKKO_HASH_THREADS` parallel + streaming hashing | Indexed digest slots from N workers, serial manifest | **Safe to merge** (one Windows-only nit, §4.4) |
| A3-2 | `067378d` + `477fc5b` — `RuntimeConfig::preinspected_metadata` reuse | Skips the second ~11 s full-tree hash per launch / per lobby boot | **Safe to merge** |
| A3-3 | `2eed4da` — `threads.csv` stat field fix | `$12+$13` = utime+stime | **Safe** — already integrated into the main bench (`154bb3a`) |

---

## 2. Correctness review (what was verified in the code)

### A1 — direct memory helpers (`85971da`)

Verified in `vendor/dolphin/GXRuntime/include/core/cpu.h` and
`src/core/cpu.c`:

- Hit set: `offset = (addr & ~0x40000000u) - 0x80000000u; offset <=
  GC_MAIN_RAM_SIZE - size` — identical to `get_ram_ptr`'s MEM1 window when
  `ram_size == GC_MAIN_RAM_SIZE` (unsigned wrap rejects everything else,
  including the uncached mirror, EXRAM, and MMIO). The GC-size precondition
  is enforced twice: `cpu_init` hardcodes `ram_size = GC_MAIN_RAM_SIZE`
  (cpu.c:86) and `module_on_state_loaded` FATALs otherwise.
- EXRAM: second-window probe preserved (`mem_direct_exram_ptr`, same
  `masked - GC_EXRAM_BASE` bounded test as `get_ram_ptr`), stores skip the
  journal exactly like the legacy `(u32)-1` offset encoding.
- Stores: `clear_matching_reservation` + BE write +
  `mem_journal_push(offset, size)` — same order and same offset/size pairs
  as the legacy helpers, so the write journal (incl. A2's page filter)
  sees identical traffic.
- Reads keep `read_be16/32/64`; the `*le` variants exist but are not
  referenced by any generated chunk (verified by grep over both the pinned
  tree and `wt-a1-codegen/dol-gen-a1`).
- Slow tails `mem_*_slow` are the legacy `get_ram_ptr` + external-callback
  sequence verbatim — MMIO and misses unchanged.
- `GX_MEM_READ32/WRITE32` route through the direct helpers only under
  `-DMODERNGEKKO_INLINE_XLAT_FULL` (cpu.h:743–749), which the A1 build
  script sets (`bench/rebuild_module_a1.sh`).

PPC semantics / BE-LE / MMIO / reservation / journal: unchanged. This is a
callee-name substitution onto accessors that already existed in-tree.

### A1 — dispatch batching (`1262f06` + `8c2fcd6`)

The mechanism: the chassis installs a callback (`ppc_set_native_check` →
`ModuleNativeOk`) that answers *exactly* the predicate the chassis
evaluates before re-entering the module —
`m_module_active && FastDispatchableRunPath(pc) && !IsHostCallAddress(pc)`.
`FastDispatchableRunPath` is a verbatim extraction of the old boundary
lambda (verified line-by-line against the parent). Inside a batch,
`dolrecomp_call` still runs replacement-first, then `native_ok`, then
`find_original`; a `false` answer returns `0` with `ctx->pc` set, and the
chassis handles that pc exactly as it would have at a block boundary.
Verified:

- `IsHostCallAddress` covers hooks ∪ patches ∪ pending-returns via
  `ModManager::HandlesAddress` (and the `|0x80000000` physical-alias leg),
  so hooked/demoted/pending-return pcs all escape the batch.
- `ctx->exception` is checked per block, identical ordering to
  `dolrecomp_run_blocks`.
- Charge accounting: `ctx->downcount` accumulates negative; the batch exits
  at `<= -256`; the chassis flushes `charge = -m_guest.downcount` /
  `max(charge,1)` in `finish_segment` — partial-batch charges flush
  correctly on a `return 0` mid-batch too.
- Lockstep: when the verifier is enabled the runner clears the callback to
  `nullptr`, and the module falls back to `run_blocks(state,1)` +
  legacy `ppc_host_call` path — per-block granularity preserved.
- ABI: `ppc_set_native_check` is an optional export; older modules stay
  single-block, newer modules on older runners keep the
  `ctx->host_call` path. Both directions inert.
- `module_run` engages the batch only when `g_mg_native_ok` is non-null;
  `MODERNGEKKO_BATCH_BUDGET` clamps 0/negative to 1 (≈ single-block).
- OSLink-family replacement cases (`0x80305424/0x80305448/0x803056BC/
  0x80305890`) still run in-batch — REL link/unlink handling is preserved.

What batching actually changes (bounded, and correctly identified by the
agent): `finish_segment`/`prepare_segment` run once per ≤256 guest cycles
instead of once per block. Concretely that coarsens: downcount→CoreTiming
delivery, `AdvanceGuestTimebase`/`ctx->timebase` freshness (mftb reads
inside a batch see a frozen-but-monotone value — the same staleness bound
already existed inside a chunk's backward loop), idle-pc/idle-jump streak
counting, and the dispatch diagnostics (per-batch sampling). Worst-case
event granularity equals the existing in-chunk loop bound
(`DOLRECOMP_C_LOOP_CYCLE_BUDGET = 256`); the *typical* boundary interval
grows ~4–5× for call-dense code, which is exactly why their
`BATCH_BUDGET=1024` experiment lost VI rate despite fewer dispatches. None
of this alters guest-visible state ordering — exceptions, hooks, SMC,
save/load (SyncIn/Out + `NotifyStateRestored`) are all boundary- or
store-level, unchanged.

Two real issues — see §3.1.

### A1 — SIGFPE guard (`f5675fd`)

`TextureCacheBase.cpp`: `if (hash_sample_size != 0 && num_blocks_y != 0)`.
For zero-height EFB copies `NumBlocksY()` is 0 and the row loop contributes
nothing, so the hash legitimately reduces to `size_in_bytes`; dividing by
`num_blocks_y` was the only thing left doing `x/0`. Skipping the division
preserves the degenerate result and doesn't alter any nonzero-height hash.
Not masking a deeper bug — the guard is the fix.

### A2 — flat membership (`d0b2f6c`)

`handled_sorted` = sorted unique union of `hooks` keys + `patches` keys,
built after a successful `Load()` and cleared in `Unload()`. Verified:
hooks/patches have no post-load mutator (only the `Load()` loop writes
them; `Unload()` clears everything incl. `handled_sorted` at :573).
`Dispatch` ordering preserved: `runtime_start` event → pending-return
check **first** (return-observer addresses are dynamic and correctly
excluded from the static set) → flat reject → hooks → patch.
`HandlesAddress` = membership ∨ pending-scan; `HandlesRange` =
`lower_bound(start) < end` ∨ pending range-scan — both exact.

### A2 — SMC envelope + REL index (`907002b`)

- `m_dol_guard_lo/hi` computed over exactly the `end <= 0x80400000` ranges
  the old loop used; the envelope `address < hi && address+length-1 >= lo`
  is a strict superset, keeps the old `u32` wrap arithmetic bit-for-bit,
  and the authoritative chunk binary search below is untouched — envelope
  false-positives only cost a lookup. Degenerate `[0,0)` ranges make `hi=0`
  → guard disabled, same as "no qualifying range".
- `FindActiveSection`: binary-searches sorted starts, walks backward,
  early-out `start + max_section_size <= address` is sound (u64 math; a
  covering section must satisfy `start > address - size >= address -
  max`), keeps min-array-index tiebreak = first-in-array-order under
  overlap, and `check_header` reproduces the stale-header skip. The
  reverse direction (`ResolveRuntimeAddress`) uses `check_header=false`,
  matching the old no-check scan (verified against `907002b~1`).
- Mutation coverage: `m_active_rel_sections` is only ever assigned by
  `RefreshRelSections()` (which calls `RebuildRelSectionPageMap()`) and
  cleared in `LoadModule()` alongside the index vectors. Both memos are
  invalidated on set change. Sound.
- `chunk_index_out` reuses the same lookup-table result — no second probe
  inconsistency possible.

### A2 — default-arm env (`39721af`)

`setenv(name, "1", overwrite=0)` behind a `getenv` check at the top of
`Init()` (StaticRecompCore.cpp:211–213), before the module `dlopen`
(`LoadModule()` at :277) and before `m_rel_lazy_arm` is read (:253). An
explicit user value — including `"0"` — always wins; verified.

The three flags themselves:
- `INLINE_XLAT`: hit-set identical to `get_ram_ptr` (verified above); GC
  size enforced at `cpu_init`.
- `JOURNAL_FILTER`: the mask builder (`rel_loader.c:3768`+) covers every
  offset the journal body can act on — SMC-guarded DOL chunk pages, the
  page containing the watched globals `0x3F7650/0x3F758C/0x3F75FC`, the
  mt-watch region when armed, and the *non-executable* R/L data sections
  of every active module, plus one predecessor page each for straddles.
  Stores into R-exec sections only produce the `TRAP` diagnostic (no
  guest-visible effect) — correctly identified as the only suppressed
  output. Rebuilt on every link/unlink (same dispatch thread). One narrow
  caveat in §4.2.
- `REL_LAZY_ARM`: correct in isolation — see §3.1 for the interaction.

### A2 — tier counters (`1e53e6e`)

`run_fallback_jit`/`interp_step` lambdas wrap the exact old sequences;
fb-reason classification adds one `ChunkIndexOf` probe per else-entry
(a cold path) — slightly more than "zero cost" but trivial. Counters are
diagnostic-only.

### A3 — hashing + dedup

- `HashDirectorySha256`: file list walked+sorted before workers spawn;
  workers write disjoint `digests[i]` slots pulled from a `fetch_add`
  counter; `jthread` join before serial manifest assembly → bit-identical
  for any worker count. `DirectoryHashThreads` parses safely (bad input →
  serial; capped at hw/64/filecount). `HashFileSha256` streams a 4 MiB
  buffer; EOF/`bad()` handled; manifest's `file_size` path unchanged.
- Determinism test exists and passes locally (serial vs 8 workers; sizes
  crossing the 64-byte SHA block and 4 MiB streaming boundaries). I ran
  `./build/moderngekko_game_inspect_test` and the same under
  `MODERNGEKKO_HASH_THREADS=8`: both pass.
- `preinspected_metadata`: `InspectGame` stores `weakly_canonical(root)`
  in `metadata.root` (game.cpp:458,513), and `Runtime::Create` compares
  `weakly_canonical(game_root) == metadata->root` — same-tree reuse only,
  else it re-inspects. The reused `GameInspectResult{metadata,{}}` carries
  an empty `error`, identical to a fresh success. Netplay lobby reuse is
  the same pattern.
- `threads.csv` fix: `/proc/<tid>/stat` fields after the `)` split start
  at field 3, so `$12`=utime, `$13`=stime — fix correct; already merged
  into the main bench.

A3's "nothing safely parallelizable in steady state" holds up: `--headless`
forces `MAIN_GFX_BACKEND="Null"` and `BACKEND_NULLSOUND`
(dolphin_runtime.cpp:342,384), so there is no GPU/audio pipeline to feed;
Emuthread is ~87% of one core and the journal
(`rel_loader_write_journal`) performs synchronous guest-RAM mutations
(`rel_wr32` repoints, L-mirror `memcpy`) that must stay ordered with guest
execution — offloading it would change semantics. Claim verified.

---

## 3. Blocking / required-before-merge items

### 3.1 A1 batch channel + A2 `REL_LAZY_ARM=1` — real interaction defect

`m_rel_discovery_armed` has exactly two setters: `NotifyStateRestored()`
and the `ResolveNativeAddress` fast path checking
`pc ∈ {OSLink 0x80305424, OSLinkFixed 0x80305448, OSUnlink 0x803056BC,
__OSModuleInit 0x80305890}` (StaticRecompCore_SMC.cpp:216–221).
`RefreshRelSections()` has exactly one caller — the arena-miss retry at
:409, gated by `!armed → return false` at :370.

Under the A1 batch channel, those four PCs are consumed by the module's
`dolrecomp_dispatch_replacement` **before** `g_mg_native_ok` is ever
queried (`dolrecomp_call` runs replacement-first and returns on it). So
the arm check inside `ResolveNativeAddress` never sees a link PC for calls
initiated from native code — i.e., all steady-state links. Net effect
with `MODERNGEKKO_REL_LAZY_ARM=1` armed (A2's new default):

- `m_active_rel_sections` is never repopulated after a mid-run link
  (stays at whatever the state-restore arm discovered);
- every runtime-window REL dispatch miss short-circuits to `false` →
  `dolrecomp_call` returns 0 → the pc permanently routes to the fallback
  JIT/interpreter (correct output, interpreter speed — a silent, large
  regression on REL-heavy scenes like outset, and nondeterministic
  because it depends on whether a link PC ever lands on a chassis
  boundary);
- the linked-window (≥0x80500000) and retail-twin REL paths are
  unaffected — they live inside the module's own dispatcher — which is
  why neither agent's solo benchmarks could see this.

This is invisible in both agents' numbers: A1's runner lacks the A2
setenv defaults (lazy arm off → legacy refresh still works under
batching), and A2's module doesn't batch (the arm fires at every chassis
boundary).

**Required resolution** (pick one — listed in order of preference):
1. Co-gate: when `ppc_set_native_check` is installed (channel armed) do
   not arm `m_rel_lazy_arm` — i.e., in `LoadModule`/Init, `m_rel_lazy_arm
   &= !batch_channel_armed`. Keeps legacy refresh-on-miss; costs the
   stale-binding-churn win under batching only.
2. Re-trigger: have the module notify the chassis on link (e.g., a
   `ppc_notify_rel_linked` export called from `rel_loader_link`/
   `unlink`, or the module-side `dolrecomp_call` poking `g_mg_native_ok`
   for the four link PCs even when replaced — cheap, preserves lazy arm).
3. Keep `REL_LAZY_ARM` off by default entirely (drops that piece of the
   A2 win).

Until one lands, do not ship A1-batch and A2's env defaults together.

### 3.2 `dolrecomp_run_budget` has no non-charge escape — recommended guard

If `find_original` hits a verified chunk at a pc that isn't a block entry
(mid-chunk data; reachable via a wild computed jump), the func's `default:`
returns with zero charge and unchanged `ctx->pc` → the batch loop spins
forever without consulting `*state_ptr`. The old path had the same
livelock but stayed responsive to quit (the chassis re-checked
`CPU::State` every dispatch); under batching it becomes a hard hang
requiring SIGKILL. Low probability, real severity delta. One-instruction
fix per block — cap iterations (`++i < 4096` → return 1) — recommended
before merge.

### 3.3 `emit_cross_chunk_call` (emitter path) — reject

`DolRecomp/src/backend/emitter.c`'s version checks only call depth — no
native-eligibility, no replacement-first ordering, no hook/pending-return
handling. It is dormant (shipped `dol-gen-a1` chunks emit plain `return`
for cross-chunk escapes — verified), and `a1_chain.py`'s post-pass *is*
properly guarded (`g_mg_native_ok` per site, the 73 replacement cases
excluded, depth-capped) yet still measured slower than batching alone
(+25.8% avg vs +32.5% — 34.46/33.97/33.36 vs 35.54/35.90/35.80) while
adding ~19 MB. Both correctly rejected by the agent; keep them out of the
integrated tree permanently. If the emitter path is ever revived it needs
the full predicate, not just the depth cap.

---

## 4. Non-blocking findings / caveats

1. **A1 diagnostics drift**: `runtime_start` now fires at the first hooked
   dispatch rather than the first block; `m_native_dispatches`, dispatch
   histograms and search samples count per batch, not per block;
   `m_guest.timebase` freezes inside a batch. Diagnostics-only; guest
   state unaffected.
2. **A2 journal-filter snapshot caveat**: the mask is built from
   section-table entries read out of live guest RAM at link/unlink time,
   while the journal body re-reads them live. Under exactly the heap-
   clobber scenario the journal exists to catch, a mutated table could let
   the filter skip a store the (unfiltered) body would have mirrored/
   marked dirty. Narrow and theoretical — the unfiltered body is itself
   undefined-behavior-adjacent under a clobbered table — but the filter is
   strictly less conservative in that window. Acceptable; noted for the
   record.
3. **A3 Windows nit**: if `Sha256FileBcrypt` fails mid-file, the portable
   fallback in `HashFileSha256` hashes only the unconsumed tail (the
   `ifstream` position is not rewound) — yields a wrong-but-deterministic
   digest → spurious pin-check rejection, never silent acceptance.
   `file.clear(); file.seekg(0)` (or return `nullopt`) would fix it.
   Windows-only path; Linux unaffected.
4. **`ppc_set_native_check` signature**: module declares `(void* fn,
   void* user)`; the chassis passes a function pointer through `void*` —
   POSIX-legal, fine on all targets here.
5. **`m_chunk_host_call_state` memoization**: a chunk once classified
   "contains host-call" (including via a transient pending-return inside
   `HandlesRange`) stays demoted — pre-existing behavior, unchanged by
   A2.
6. **Perf-flag footnote**: every arm in the A1/A2 A/B runs used
   `MODERNGEKKO_PERF_COUNTERS=1` (`--perf`). Batching amortizes the
   per-dispatch `steady_clock` calls (~1% CPU at 5.7M dispatches/s), so
   the +32.5% sea figure likely overstates the uninstrumented delta by
   ~1 point. Direction and magnitude are unaffected.
7. **Double `resolve_active()` call** in `ResolveNativeAddress`
   (SMC.cpp:280,323) — redundant on the miss path, pre-existing in
   `907002b~1`, not an A2 bug.

---

## 5. Benchmark methodology assessment

- **A1**: interleaved A/B per round (`a1_ab.sh`), same runner binary
  (sha `abee58f…`) and state (`4b7b99…`), module as sole variable —
  verified via `metadata.env`. Rep-level data confirms every claim:
  pinned 26.20/26.96/27.75 → direct 29.26/30.56/28.61 → batch
  35.54/35.90/35.80 (sea); 17.12/16.95 → 23.36/22.89 (outset).
  `shutdown_native` drops 344.8M→85.7M ≈ 4× (matches "4–10×" claim).
  `fallback=0`, `exit_code=0` everywhere. Methodology sound; the only
  caveat is the `--perf` instrumentation asymmetry (§4.6).
- **A2**: quiet-machine A/B pairs, same recipe; the sea spread (+5.9% vs
  +15.0%) is explained by **baseline drift between reps** — rep1 base was
  27.92, rep2 base was 31.04 (machine quieter later in the afternoon);
  within-pair deltas are consistent in direction. Outset +11.2%
  (21.97→24.43), filesel +13.0% (31.50→35.59). Honest claim: +6–15%,
  point estimate ≈ +10%. The contended `a2r2` series is noise-tier, as
  the agent itself states.
- **A3**: startup-only claims (dedup ~13–14 s/launch + ~3 s with 4
  workers on this host's fuse-mounted tree), no steady-state claims made
  or needed; `threads.csv` fix verified against `/proc` field layout.
- All runs used the same harness (`mg_bench.sh` flock-serialized, same
  user-dir template, `--capped` with values comfortably below the 60 Hz
  cap). Nothing in the measurement protocol invalidates the headline
  claims; percentages just shouldn't be summed (§6).

## 6. Integration order & conflicts

Benefit overlap: A1's batch eliminates the per-block chassis
finish/prepare + host-call-probe round trip; A2 makes parts of that same
work cheaper (flat membership still runs per block inside `native_ok`,
journal filter is per-store, envelope is per-invalidation). So A2's
dispatch-probe wins are partially subsumed by batching, while its
per-store/per-invalidation wins are orthogonal. **Do not add the
percentages** — realistic combined estimate on sea is ~+35–40%, not +45%+
(and only after §3.1 is resolved).

Textual conflicts (both vendor branches touch the same files; all
semantically disjoint):
- `StaticRecompCore_Run.cpp`: A1 rewrites the `fast_dispatchable_at`
  lambda (~:80–97); A2 inserts tier lambdas at ~:111+ and counters in the
  else-branch — adjacent hunks, likely one trivial merge conflict.
- `StaticRecompCore_SMC.cpp`: A1 appends `FastDispatchableRunPath`/
  `ModuleNativeOk` after `FastDispatchableAt` (~:582); A2 rewrote
  `ChunkIndexOf` just above (:536–561) — small context overlap possible.
- `StaticRecompCore.cpp`: A1 installs the callback early in `LoadModule`
  (~:494); A2's clears/envelope are later in the same function (:577–599)
  plus `Init`/`Shutdown` elsewhere — same function, disjoint regions.
- `StaticRecompCore.h`: A1 adds method decls (public, ~:61); A2 adds
  member fields (private, ~:146–265) — disjoint.
- Non-vendor overlap: none — A2's `src/runtime/mod_loader.cpp` is
  untouched by A1; A3's files (`game.cpp`, `dolphin_runtime.cpp`,
  `runtime.hpp`, tools) are disjoint from both. `bench/mg_bench.sh`
  conflict is moot — A3's fix is already in the main bench.

Recommended order:
1. **A1 `f5675fd`** — independent crash fix, zero risk.
2. **A2 `d0b2f6c` + `907002b` + `1e53e6e`** — chassis data-structure and
   instrumentation wins; self-contained, measured.
3. **A3 `46fc3c7` + `067378d` + `477fc5b`** — startup-only; independent.
4. **A1 `85971da`** (direct-mem transform + `INLINE_XLAT_FULL` module
   build) — orthogonal; needs the module rebuild recipe, not a code merge.
5. **A2 `39721af` + A1 `1262f06`/`8c2fcd6`** — **together**, with the
   §3.1 co-gate (`REL_LAZY_ARM` off when the batch channel is armed, or a
   link-notify path) and the §3.2 iteration cap. Treating these as one
   integration unit avoids the silent REL-discovery regression.
6. **Never**: `emit_cross_chunk_call`; `a1_chain.py` remains reference
   only.

Verification already done: `verify-inline-fixed-runner` reproduces the
pinned module at 25.81 VI Hz (baseline 25.75) — the A1 rebuild recipe is
faithful; A3's determinism test passes serial and 8-thread.

## 7. Residual risk summary

- **Guest-visible behavior**: no patch alters instruction semantics,
  hook/patch semantics, REL loading order, save/load, or exception
  handling. The only timing-adjacent change is batching's ≤256-cycle
  event granularity (bounded, same nominal bound as the existing
  in-chunk loop).
- **Determinism**: batch boundaries are cycle-deterministic; A3's digest
  is order-independent by construction; A2's structures are exact
  equivalents. No new nondeterminism.
- **30 Hz logic / input timing**: untouched paths.
- **Crash/hang surface**: the §3.2 zero-charge spin is the only new
  hard-hang mode; `f5675fd` removes a real SIGFPE.
- **Silent-fallback surface**: §3.1 is the only path where a combination
  silently converts native dispatch into permanent interpreter/JIT
  execution — blocked pending the co-gate.
