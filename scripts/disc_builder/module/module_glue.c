// gGZLE01_recomp module export glue for the Wind Waker (GZLE01) DOL recompile.
//
// Wraps the DolRecomp-generated chunk dispatcher behind the ModernGekko
// module ABI v3. All environment access goes through the CPUState hook
// pointers the chassis installs; the module itself has no host dependencies
// beyond libc/libm and the bundled GXRuntime CPU core.
//
// ABI trap note: generated.h defaults DOLRECOMP_CPU_HEADER to "cpu/cpu.h"
// (DolRecomp's own copy, whose CPUState carries a spr[1024] tail with a
// different layout than the runtime's). The build compiles with
// -DDOLRECOMP_CPU_HEADER="core/cpu.h" and -I GXRuntime/include so the
// generated code, this glue, and the ModernGekko loader all agree on the
// CPUState layout.

// REL loader integration (G6): the generated dispatch calls
// dolrecomp_dispatch_replacement (defined in port/loader/rel_loader.c) on
// every module entry. Must be defined before including generated.h.
// #define DOLRECOMP_ENABLE_REPLACEMENTS (BISECT: loader hooks disabled)
#include "generated.h"
#include "moderngekko/module_abi.h"
#include "core/guarded_dispatch.h"
#include <stdlib.h>

// dolrecomp_call_depth (extern in generated.h, cross-chunk guest-call depth
// guard) is defined by the bundled GXRuntime cpu.c — no storage here.

// A1 batch-dispatch channel (externs in generated.h): the chassis installs
// the block-boundary query through ppc_set_native_check; dolrecomp_call
// consults it in place of the ppc_host_call probe. Storage lives here.
int (*g_mg_native_ok)(u32 address, void* user);
void* g_mg_native_ok_user;

static int (*s_native_ok_real)(u32, void*);
static unsigned long long s_nok_calls, s_nok_fail;
static int native_ok_shim(u32 a, void* u) {
    int r = s_native_ok_real(a, u);
    ++s_nok_calls;
    if (!r) {
        ++s_nok_fail;
        if (s_nok_fail <= 40)
            fprintf(stderr, "[nok] site-native_ok=0 pc=%08x\n", a);
    }
    return r;
}
MODERNGEKKO_MODULE_EXPORT void ppc_set_native_check(int (*fn)(u32, void*), void* user)
{
    s_native_ok_real = fn;
    g_mg_native_ok = native_ok_shim;
    g_mg_native_ok_user = user;
}

/* In-burst host-call trampoline (optional, chassis-installed). Where a
 * pc's verdict says it cannot run in-module today we previously always
 * took return 0 -> burst exit -> chassis SyncIn -> host_call Dispatch ->
 * SyncOut -> classification. With the trampoline the chassis runs the
 * host-call funnel IN PLACE (HookHostCall: dispatch + alias retry +
 * verdict-pc drain + fallback-JIT lr invalidate) while ctx is still the
 * live m_guest, then reports how to proceed. The passthrough pair the
 * callback arms doubles as the "dispatch already ran" signal — the
 * Run-loop gate skips its own host_call block when armed for this pc,
 * so dispatch still happens exactly once per event.
 * Return codes (mirror StaticRecompCore::HostCallBurst):
 *   EXIT     — burst-exit now; chassis bookkeeping handles the rest.
 *   CONTINUE — hook ran in place; re-dispatch ctx->pc (redirect/handled).
 *   RUN_BODY — declined clean; run the original body ourselves. */
#define MG_HC_EXIT     0
#define MG_HC_CONTINUE 1
#define MG_HC_RUN_BODY 2
static int (*g_mg_burst_hc)(CPUState*, u32, void*);
static void* g_mg_burst_hc_user;
MODERNGEKKO_MODULE_EXPORT void
ppc_set_burst_host_call(int (*fn)(CPUState*, u32, void*), void* user)
{
    g_mg_burst_hc = fn;
    g_mg_burst_hc_user = user;
}
static int glue_burst_exit(CPUState* ctx, u32 address)
{
    if (g_mg_burst_hc) {
        const int r = g_mg_burst_hc(ctx, address, g_mg_burst_hc_user);
        if (r == MG_HC_CONTINUE)
            return 1;            /* loop re-dispatches ctx->pc */
        if (r == MG_HC_RUN_BODY) {
            DolRecompFunction fn = dolrecomp_find_original(address);
            if (fn) { ctx->pc = address; fn(ctx); return 1; }
        }
    }
    return 0;                    /* EXIT or unarmed: today's path */
}

/* Runtime guest-cycle budget (MODERNGEKKO_CYCLE_BUDGET env, parsed in
 * merge_rel_tables). When the module is built with -include cycle_budget.h
 * this is ALSO the value DOLRECOMP_C_LOOP_CYCLE_BUDGET expands to in every
 * generated chunk, so in-chunk loop back-edges and the batched block driver
 * in module_run_segment share one bound. It bounds only the accumulated
 * charge between chassis checks — never which instructions run — so
 * changing it cannot alter guest semantics; it trades dispatch overhead
 * against CoreTiming/interrupt latency (256 cycles = ~0.5us at 486MHz,
 * trivially inside Dolphin's 20000-cycle slice either way). The default
 * reproduces generated.h's literal 256. */
s64 dolrecomp_cycle_budget = 256;

/* Batch-scoped native-dispatch cache (pc -> resolved generated function).
 *
 * dolrecomp_call pays the full gauntlet on every block: extern
 * replacement-dispatcher call, the extern g_mg_native_ok query (which in
 * the chassis runs forced-fallback scan + chunk lookup + host-call chunk
 * state + HandlesAddress's handled_sorted binary_search + pending_returns
 * scan), then find_original, then the callee's own entry switch. For the
 * hot DOL blocks that dominates per-block cost.
 *
 * Entries are tagged with a generation. When the chassis exports an epoch
 * signal (ppc_dispatch_epoch below), it bumps the generation at every
 * verdict-mutating event — chunk verify/demote, SMC state transitions,
 * forced-fallback installs, and every host_call dispatch that could have
 * armed hooks or pending returns — so cached pcs persist across batches
 * until the underlying verdict could have changed. When no epoch signal
 * was ever installed (older runners), the generation falls back to
 * per-batch bumping: every piece of state the gauntlet consults is
 * chassis-owned and can only change at the segment boundary, never while
 * run_budget is inside a batch (mid-batch the only non-native code that
 * runs is dolrecomp_dispatch_replacement — kept on the hit path below so
 * runtime-gated DOL hook cases still fire — plus the generated functions
 * themselves, which cannot mutate chassis dispatch state), so a pc cached
 * in batch N revalidates unconditionally in batch N+1.
 *
 * Only DOL pcs get cached here. The populate paths are: decline-then-
 * verified (native_ok + find_original), negative (decline + native_ok==0),
 * and claim (dispatcher claimed a DOL case-list pc — see the miss path).
 * REL pcs populate only through the loader's published chunk fn
 * (g_rel_dispatched_fn) as REL-pinned entries. */
/* Misses measured as ~94% live-entry collisions at direct-mapped 16K —
 * the active call-site set overflows single slots. 2-way set-associative:
 * 8192 sets x 2 ways = same 16384-entry / 512KB footprint, L2-resident
 * on the Zen4 target (1MB/core), and a pairwise conflict now needs a
 * THIRD live pc in the set to force an eviction. */
#define GLUE_DCACHE_SETS_LOG2 13
#define GLUE_DCACHE_SETS (1u << GLUE_DCACHE_SETS_LOG2)
#define GLUE_DCACHE_SIZE (GLUE_DCACHE_SETS * 2)
typedef struct {
    u32 pc;
    u32 gen;
    u32 rgen;              /* 0 = DOL entry; nonzero = REL entry, valid while
                            * it equals rel_loader's g_rel_dispatch_gen */
    u32 call_pc;           /* REL entries: pc the callee must see (twin for
                            * retail-band arrivals, address otherwise) */
    u32 may_repl;          /* DOL entries: dolrecomp_replacement_may_claim(pc);
                            * skips the dispatcher call on the hit path for
                            * pcs outside the static case list */
    u32 neg;               /* 1 = negative verdict: replacement declined AND
                            * native_ok==0 at populate. A hit returns 0
                            * immediately (identical burst-exit to the exe)
                            * without re-running the miss gauntlet. */
    DolRecompFunction fn;
} GlueDcacheEntry;
static GlueDcacheEntry s_dcache[GLUE_DCACHE_SIZE];
/* Second-seen admission tags (per set, tag-only): most misses are
 * one-shot pcs whose populate would only evict a live entry. First
 * sighting records the tag and runs the call uncached; the second call
 * populates. Verdict staleness is unaffected — populate always re-runs
 * the full gauntlet, so tags need no epoch handling. */
static u32 s_seen[GLUE_DCACHE_SETS];
/* Round-robin eviction bit per set: when both ways hold live entries,
 * populate evicts way s_way[set] and flips it. */
static u8 s_way[GLUE_DCACHE_SETS];
u32 g_mg_dcache_gen = 1;
#define s_dcache_gen g_mg_dcache_gen
static int s_dcache_epoch_armed;
/* rel_loader.c exports — REL dispatch validity counter and the resolved
 * chunk fn from the last replacement dispatch (0 when the dispatch is not
 * memoizable: E4 gate, retail->twin redirect, or a non-REL path). */
extern u32 g_rel_dispatch_gen;
extern int g_rel_in_chunk;
extern void (*g_rel_dispatched_fn)(CPUState* ctx);
extern u32 g_rel_dispatched_pc;
/* rel_loader.c: static membership prefilter for the replacement dispatcher
 * (1 = pc may claim, 0 = provably absent from the case list). */
extern int dolrecomp_replacement_may_claim(u32 address);
/* rel_loader.c OPTIONAL export (weak — NULL until it lands): per-pc claim
 * thunk for DOL case-list pcs. The thunk runs the pc's dispatcher case
 * body exactly as the switch would (ctx mutations incl. ctx->pc=lr on
 * claim) and, on a decline, runs the original body itself — orig is the
 * find_original result handed over at populate (rel_loader.c keeps it in
 * a per-pc static the thunk calls on decline). Cached as a positive
 * entry's fn with may_repl=0, so hits skip the dispatcher preamble
 * entirely while declines still replay the original. Must return NULL for
 * every pc while mt-watch is armed (the dispatcher preamble's shadow sync
 * is required on every call). */
extern DolRecompFunction dolrecomp_claim_thunk(u32 pc, DolRecompFunction orig)
    __attribute__((weak));

/* Chassis epoch signal: called once at native-check install time to arm
 * persistent caching, then once per verdict-mutating event thereafter
 * (chunk state transitions, forced-fallback installs, host_call dispatch
 * returns — hook/patch registration and pending-return pushes all funnel
 * through Dispatch, which only runs inside host_call). Every call
 * invalidates all cached verdicts cheaply via the generation tag. */
/* TEMP diag: attribute epoch-bump callers (return address inside the
 * runner). MG_GLUE_DIAG-gated; printed at each glue-tick. */
static unsigned long long s_epoch_caller_pc[8];
static unsigned s_epoch_caller_n[8];
static int s_epoch_diag = -1;
MODERNGEKKO_MODULE_EXPORT void ppc_dispatch_epoch(void)
{
    ++s_dcache_gen;
    s_dcache_epoch_armed = 1;
    if (s_epoch_diag < 0) s_epoch_diag = getenv("MG_GLUE_DIAG") ? 1 : 0;
    if (s_epoch_diag) {
        const unsigned long long ra =
            (unsigned long long)(uintptr_t)__builtin_return_address(0);
        unsigned i;
        for (i = 0; i < 8; ++i) {
            if (s_epoch_caller_pc[i] == ra || s_epoch_caller_n[i] == 0) {
                s_epoch_caller_pc[i] = ra;
                ++s_epoch_caller_n[i];
                break;
            }
        }
    }
}

/* Per-pc sibling of ppc_dispatch_epoch: the chassis calls this for verdict
 * mutations it can attribute to a single pc (mod pending-return push/pop).
 * e->pc is KEPT so the next call takes the cheap same-pc stale-gen
 * revalidation (dispatch probe + native_ok + gen refresh) rather than a
 * full miss; gen=0 is the never-valid sentinel (s_dcache_gen starts at 1
 * and only increments), so the hit test, dcache_peek, and the admission
 * path all see a lapsed generation — identical effect to a global bump,
 * scoped to this one slot. s_seen is untouched (the tag belongs to this
 * pc; keeping it preserves second-seen credit). A slot naming a different
 * pc is not ours to kill. */
MODERNGEKKO_MODULE_EXPORT void ppc_dispatch_epoch_pc(u32 pc)
{
    const u32 set = ((pc >> 2) * 2654435761u) >> (32 - GLUE_DCACHE_SETS_LOG2);
    GlueDcacheEntry* e = &s_dcache[set << 1];
    if (e->pc != pc) ++e;
    if (e->pc == pc)
        e->gen = 0;
    s_dcache_epoch_armed = 1;
}

/* Mirrors dolrecomp_call (generated.h) exactly; the only difference is the
 * cached-ok hit path, which skips the native query + find_original after
 * replacement declines — the two verdicts the batch-scoping argument above
 * proves cannot change mid-batch for a pc that populated the entry. */
static unsigned long long s_diag_segs, s_diag_calls, s_diag_ret0, s_diag_exc,
                          s_diag_dhi, s_diag_dnz,
                          s_diag_hit, s_diag_miss, s_diag_repl, s_diag_relhit,
                          s_diag_relgen, s_diag_relpop, s_diag_relpc, s_diag_genmove,
                          s_diag_neghit, s_diag_negpop, s_diag_clpop,
                          s_diag_clthunk, s_diag_coll;
static int s_negcache_off = -1; /* MG_NEGCACHE_OFF, latched like s_diag_on */
static int s_claimcache_on = -1; /* MG_CLAIMCACHE: default on, =0 off */
static int claimcache_enabled(void) {
    if (s_claimcache_on < 0) {
        const char* ccv = getenv("MG_CLAIMCACHE");
        s_claimcache_on = (ccv && ccv[0] == '0') ? 0 : 1;
    }
    return s_claimcache_on;
}
/* MG_GLUE_DIAG master gate: when unset, the rdtsc/phist instrumentation and
 * diagnostic printfs below cost nothing (single predictable branch). */
static int s_diag_on = -1;
static unsigned long long s_t_fn, s_t_loop;
static inline unsigned long long mg_rdtsc(void) {
    unsigned hi, lo;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
static u32 s_last_gen;
static u32 s_diag_lastpc;

static u32 e_dbg_rgen, e_dbg_callpc;
static unsigned s_resume_n;
static uint32_t s_resume_lr[16];
#define PHIST_N 8192
static struct { u32 pc; unsigned long long cyc; } s_phist[PHIST_N];
static inline void phist_add(u32 pc, unsigned long long cyc) {
    u32 k = ((pc >> 2) * 2654435761u) >> 20;
    unsigned i;
    for (i = 0; i < 4; i++) {
        u32 sl = (k + i) & (PHIST_N - 1);
        if (s_phist[sl].pc == pc || s_phist[sl].cyc == 0) { s_phist[sl].pc = pc; s_phist[sl].cyc += cyc; return; }
    }
    { unsigned best = k; for (i = 1; i < 4; i++) { u32 sl = (k+i)&(PHIST_N-1); if (s_phist[sl].cyc < s_phist[best].cyc) best = sl; }
      s_phist[best].pc = pc; s_phist[best].cyc = cyc; }
}
static void phist_dump(void) {
    unsigned idx[PHIST_N]; unsigned n = 0, i, j;
    for (i = 0; i < PHIST_N; i++) if (s_phist[i].cyc) idx[n++] = i;
    for (i = 1; i < n; i++) { unsigned v = idx[i]; j = i; while (j > 0 && s_phist[idx[j-1]].cyc < s_phist[v].cyc) { idx[j] = idx[j-1]; j--; } idx[j] = v; }
    fprintf(stderr, "[phist] top12:\n");
    for (i = 0; i < n && i < 12; i++) fprintf(stderr, "  %12llu 0x%08X\n", s_phist[idx[i]].cyc, s_phist[idx[i]].pc);
    memset(s_phist, 0, sizeof(s_phist));
}

static int glue_call(CPUState* ctx, u32 address)
{
    const u32 set = ((address >> 2) * 2654435761u) >> (32 - GLUE_DCACHE_SETS_LOG2);
    GlueDcacheEntry* e0 = &s_dcache[set << 1];
    GlueDcacheEntry* e;
    u32 alias;
    DolRecompFunction fn;

    ctx->pc = address;
    if (s_diag_on > 0 && address >= 0x80500000u) ++s_diag_relpc;
    if (e0->pc == address && e0->gen == s_dcache_gen) e = e0;
    else if (e0[1].pc == address && e0[1].gen == s_dcache_gen) e = e0 + 1;
    else goto miss;
    {
        if (e->rgen) {
            /* REL entry: skip the replacement dispatcher entirely — REL pcs
             * cannot match the DOL switch cases, and the loader-side verdict
             * is pinned by rgen (active-table / L-image / heap-generation
             * changes all bump g_rel_dispatch_gen). The s_in_rel_chunk journal
             * guard the dispatcher normally sets is replicated here. */
            if (e->rgen != g_rel_dispatch_gen) { if (s_diag_on > 0) ++s_diag_relgen; goto miss; }
            if (e->neg) {
                /* Negative verdict on a REL pc: dispatcher decline and
                 * native_ok==0 are both pinned by (gen,rgen). may_repl is
                 * still honored so the mt-watch "dispatcher preamble runs
                 * on every call" invariant holds (prefilter claims all).
                 * call_pc is dead on neg entries — reused as a staleness
                 * counter: every 65536th hit falls through to the miss
                 * path and re-queries native_ok. The chassis now bumps the
                 * epoch on every UNVERIFIED->{VERIFIED,FAILED} transition
                 * (VerifyChunk), so a pinned neg dies with its epoch — the
                 * periodic requery is residual insurance for any verdict
                 * flip that escapes the epoch contract (an unarmed epoch
                 * on an older runner, or a future chassis path that mutates
                 * dispatch state without bumping). */
                if ((++e->call_pc & 0xFFFFu) == 0) goto miss;
                if (e->may_repl &&
                    dolrecomp_dispatch_replacement(ctx, address)) { if (s_diag_on > 0) ++s_diag_repl; return 1; }
                if (s_diag_on > 0) { ++s_diag_hit; ++s_diag_neghit; }
                return 0;
            }
            if (s_diag_on > 0) { ++s_diag_hit; ++s_diag_relhit; }
            ctx->pc = e->call_pc;
            const int saved_in_rel = g_rel_in_chunk;

            g_rel_in_chunk = 1;
            if (s_diag_on > 0) { const unsigned long long _t = mg_rdtsc(); e->fn(ctx); unsigned long long _d = mg_rdtsc() - _t; s_t_fn += _d; phist_add(address, _d); }
            else { e->fn(ctx); }
            g_rel_in_chunk = saved_in_rel;
            return 1;
        }
        if (e->neg) {
            /* Negative verdict on a DOL pc (mod hook sites, forced fallbacks,
             * unverified chunks): native_ok==0 is pinned by the dispatch
             * epoch — the same contract positive entries rely on — and the
             * replacement decline is static for pcs outside the case list.
             * may_repl keeps runtime-gated DOL cases re-checked per hit,
             * exactly like the positive path below. Returns 0, so the
             * exe-side burst-exit + host_call fallback is identical.
             * call_pc (dead on neg entries) counts hits: every 65536th
             * falls through to the miss path for a fresh native_ok query —
             * cheap insurance against verdict flips the epoch missed. */
            if ((++e->call_pc & 0xFFFFu) == 0) goto miss;
            if (e->may_repl &&
                dolrecomp_dispatch_replacement(ctx, address)) { if (s_diag_on > 0) ++s_diag_repl; return 1; }
            if (s_diag_on > 0) { ++s_diag_hit; ++s_diag_neghit; }
            return glue_burst_exit(ctx, address);
        }
        if (s_diag_on > 0) ++s_diag_hit;
        if (e->may_repl &&
            dolrecomp_dispatch_replacement(ctx, address)) { if (s_diag_on > 0) ++s_diag_repl; return 1; }
        if (s_diag_on > 0) { const unsigned long long _t = mg_rdtsc(); e->fn(ctx); unsigned long long _d = mg_rdtsc() - _t; s_t_fn += _d; phist_add(address, _d); }
        else { e->fn(ctx); }
        return 1;
    }
miss:
    /* Populate target (2-way): a way already naming this pc > a dead way
     * (empty or stale-gen — populating evicts nothing) > round-robin
     * eviction of a live entry. The same-pc-first preference also keeps
     * goto-miss paths (neg self-heal, relgen lapse) repairing their own
     * entry rather than spilling into the sibling way. */
    if (e0->pc == address) e = e0;
    else if (e0[1].pc == address) e = e0 + 1;
    else if (e0->gen != s_dcache_gen) e = e0;
    else if (e0[1].gen != s_dcache_gen) e = e0 + 1;
    else { e = &e0[s_way[set]]; s_way[set] ^= 1u; }
    if (s_diag_on > 0) {
        ++s_diag_miss;
        /* coll = this populate WILL evict a live entry (both ways held
         * live different pcs) — the real thrash metric under 2-way. */
        if (e->pc && e->pc != address && e->gen == s_dcache_gen) ++s_diag_coll;
    }
    /* Second-seen admission (see s_seen decl): first sighting runs the
     * full gauntlet uncached so one-shot pcs cannot evict live entries.
     * Exempt: same-pc overwrites (the entry already belongs to this pc —
     * evicts nothing) and dead ways (empty or stale-gen — populating
     * evicts nothing either). */
    const int admit = (s_seen[set] == address) || (e->pc == address) ||
                      (e->gen != s_dcache_gen);
    if (!admit) s_seen[set] = address;
    /* Same-pc stale-gen fast revalidation (~55-60% of misses): the entry
     * still names this pc, only the epoch lapsed. fn is immutable per pc
     * (generated bodies never move; thunk install is deterministic), so
     * revalidation is dispatch-probe + native_ok + gen refresh —
     * find_original and the entry rewrite are skipped. may_repl is NOT
     * trusted: may_repl==0 covers both "not a case pc" and "thunked case
     * pc", and the thunked kind must keep dispatching per call (claim
     * still wins over a native_ok==0 verdict, matching the miss path's
     * dispatch-before-native_ok order). Gen refresh is NOT admission-
     * gated: the entry already belongs to this pc, refreshing evicts
     * nothing. REL entries (rgen!=0) keep the full path — call_pc twin /
     * rgen semantics differ. `dispatched` suppresses a second dispatch
     * on the full_miss/past_dispatch edges (decline arms are ctx-pure
     * today but the invariant shouldn't be relied on). */
    if (e->pc == address && !e->rgen) {
        int dispatched = 0;
        if (e->may_repl || dolrecomp_replacement_may_claim(address)) {
            dispatched = 1;
            if (dolrecomp_dispatch_replacement(ctx, address)) {
                e->gen = s_dcache_gen;
                return 1;
            }
        }
        if (!g_mg_native_ok) {
            if (dispatched) goto past_dispatch;
            goto full_miss;
        }
        if (!g_mg_native_ok(address, g_mg_native_ok_user)) {
            /* verdict still (or freshly) 0: keep/refresh the neg entry */
            e->gen = s_dcache_gen;
            e->neg = 1u;
            e->fn = 0;
            e->call_pc = 0;
            e->may_repl = (u32)dolrecomp_replacement_may_claim(address);
            return glue_burst_exit(ctx, address);
        }
        if (e->neg) {
            if (dispatched) goto past_dispatch;
            goto full_miss;  /* verdict flipped 0->1: full positive populate */
        }
        e->gen = s_dcache_gen;
        e->fn(ctx);          /* orig or thunk — a thunk re-runs its arm and
                              * falls back to orig on decline, matching the
                              * dispatcher path's semantics */
        return 1;
    }
full_miss:
    if (dolrecomp_dispatch_replacement(ctx, address)) {
        /* Memoize REL chunk dispatches: the loader published the fn it just
         * ran. E4-gate dispatches leave it NULL; retail->twin redirects
         * publish the twin (pc,fn) pair, so the entry replays the loader's
         * ctx->pc rewrite via call_pc on every hit. */
        if (g_rel_dispatched_fn) {
            if (admit) {
            if (s_diag_on > 0) ++s_diag_relpop;
            e->pc = address;
            e->gen = s_dcache_gen;
            e->rgen = g_rel_dispatch_gen;
            e->call_pc = g_rel_dispatched_pc;
            e->neg = 0;   /* clear a stale negative verdict in this slot */
            e->fn = (DolRecompFunction)g_rel_dispatched_fn;
            }
        } else if (address < 0x80500000u && g_mg_native_ok) {
            /* Claim-path caching (MG_CLAIMCACHE, default on): a DOL case
             * claim publishes no fn, so it used to return here on every
             * call — full miss + dispatcher preamble + publish check per
             * hit, and no dcache_peek verdict for the return-2 contract.
             * "Claimed once" is NOT a stable per-pc verdict (mtxrepl
             * declines on non-finite inputs, judges decline unknown leaf
             * judges, the translate cases always decline), so the cached
             * entry must still consult the dispatcher on every hit —
             * which the existing hit paths already do under may_repl.
             * What needs caching is only the DECLINE replay, selected by
             * one populate-time native_ok query:
             *
             *   native_ok==0 (hooked/unverified claim pc): a decline
             *     burst-exits — a neg entry replays return 0 exactly,
             *     while may_repl=1 keeps the dispatcher ahead of it on
             *     every hit (claim still wins over the hook, matching the
             *     miss path's dispatch-before-native_ok order). Honors
             *     MG_NEGCACHE_OFF.
             *   native_ok==1 + find_original hit: a positive entry with
             *     may_repl=1 replays dispatch-then-original per hit.
             *     When rel_loader exports dolrecomp_claim_thunk (weak,
             *     NULL until it lands) the thunk — case body with
             *     dolrecomp_call_original on decline — becomes e->fn and
             *     may_repl drops to 0, skipping the dispatcher preamble
             *     on hits entirely.
             *   find_original miss: a decline would continue into the
             *     physical-alias tail below, which no entry kind replays —
             *     leave uncached.
             *
             * REL-band claims that publish nothing (the E4 gate) are
             * excluded by the band test: its post-call fixup must re-run
             * per dispatch. may_repl is forced rather than queried: a DOL
             * claim implies the pc is in the case list, so may_claim is
             * provably 1 here (and 1-for-all under mt-watch). */
            if (claimcache_enabled() && admit) {
                if (!g_mg_native_ok(address, g_mg_native_ok_user)) {
                    if (s_negcache_off < 0)
                        s_negcache_off = getenv("MG_NEGCACHE_OFF") ? 1 : 0;
                    if (!s_negcache_off) {
                        if (s_diag_on > 0) ++s_diag_clpop;
                        e->pc = address;
                        e->gen = s_dcache_gen;
                        e->rgen = 0;
                        e->call_pc = 0;
                        e->may_repl = 1u;
                        e->neg = 1u;
                        e->fn = 0;
                    }
                } else {
                    fn = dolrecomp_find_original(address);
                    if (fn) {
                        DolRecompFunction thunk =
                            dolrecomp_claim_thunk
                                ? dolrecomp_claim_thunk(address, fn)
                                : (DolRecompFunction)0;
                        if (s_diag_on > 0) ++s_diag_clpop;
                        e->pc = address;
                        e->gen = s_dcache_gen;
                        e->rgen = 0;
                        e->call_pc = 0;
                        e->may_repl = thunk ? 0u : 1u;
                        e->neg = 0;
                        e->fn = thunk ? thunk : fn;
                        if (thunk && s_diag_on > 0) ++s_diag_clthunk;
                    }
                }
            }
        }
        return 1;
    }
past_dispatch:
    /* Reached from revalidation edges where the dispatcher already ran
     * this call — skips the duplicate dispatch above. */
    if (g_mg_native_ok) {
        int okv = g_mg_native_ok(address, g_mg_native_ok_user);
        if (!okv) {
            /* Negative verdict: this pc cannot dispatch to generated code
             * under the current epoch (~40 unique pcs — mod hook call
             * addresses — at ~100K+/s aggregate). Caching it lets repeat
             * calls replay the return-0 burst-exit WITHOUT the dispatcher
             * call + extern native_ok gauntlet (chassis forced-fallback
             * scan + chunk lookup + host-call state + HandlesAddress
             * binsearch + pending_returns scan) + find_original.
             * Invalidation mirrors positive entries exactly: e->gen pins
             * the chassis-side verdict (ppc_dispatch_epoch bumps on every
             * verdict-mutating event; unarmed => per-batch bump => the
             * entry dies with its batch), e->rgen pins REL-side state for
             * REL pcs (g_rel_dispatch_gen), and may_repl re-runs the
             * dispatcher per hit for runtime-gated DOL cases. Worst case
             * for a stale entry is replaying return-0 -> burst exit ->
             * host_call/interpreter fallback — never a wrong fn — so the
             * failure mode is benign even if a verdict flip went
             * un-signalled. */
            if (s_diag_on > 0) ++s_diag_negpop;
            /* Latch the A/B knob once: env is fixed per process, and a
             * per-populate getenv would add a libc env scan to the
             * collision-thrash worst case (a slot alternating between two
             * hot pcs repopulates on every call). */
            if (s_negcache_off < 0)
                s_negcache_off = getenv("MG_NEGCACHE_OFF") ? 1 : 0;
            if (s_negcache_off || !admit) { /* A/B + second-seen: no populate */ }
            else {
            e->pc = address;
            e->gen = s_dcache_gen;
            e->rgen = (address >= 0x80500000u) ? g_rel_dispatch_gen : 0u;
            e->call_pc = 0;
            e->may_repl = (u32)dolrecomp_replacement_may_claim(address);
            e->neg = 1u;
            e->fn = 0;
            }
            if (s_diag_on > 0 && s_diag_ret0 < 40) fprintf(stderr, "[nok] miss pc=%08x native_ok=0\n", address);
            return glue_burst_exit(ctx, address);
        }
        if (s_diag_on > 0 && s_diag_ret0 < 40) fprintf(stderr, "[nok] miss pc=%08x native_ok=1\n", address);
    } else if (ctx->host_call && ppc_host_call(ctx, address)) {
        return 1;
    }
    fn = dolrecomp_find_original(address);
    if (fn) {
        if (admit) {
        e->pc = address;
        e->gen = s_dcache_gen;
        e->rgen = 0;      /* clear stale REL fields: a slot that previously
                           * held a REL entry would otherwise take the REL-hit
                           * branch and replay a dead call_pc into a DOL fn */
        e->call_pc = 0;
        e->may_repl = (u32)dolrecomp_replacement_may_claim(address);
        e->neg = 0;     /* clear a stale negative verdict in this slot */
        e->fn = fn;
        /* Same upgrade as the claim populate: for a case-list pc, a
         * rel_loader claim thunk (weak export, NULL until it lands) runs
         * the case body + original-on-decline without the dispatcher
         * preamble — hits then skip the per-call may_repl dispatch. This
         * call still runs the declined original (fn) regardless. Gated on
         * MG_CLAIMCACHE so =0 A/Bs the whole claim-caching feature. */
        if (e->may_repl && dolrecomp_claim_thunk && claimcache_enabled()) {
            DolRecompFunction thunk = dolrecomp_claim_thunk(address, fn);
            if (thunk) { e->fn = thunk; e->may_repl = 0; if (s_diag_on > 0) ++s_diag_clthunk; }
        }
        }
        ctx->pc = address;
        if (s_diag_on > 0) { const unsigned long long _t = mg_rdtsc(); fn(ctx); unsigned long long _d = mg_rdtsc() - _t; s_t_fn += _d; phist_add(address, _d); }
        else { fn(ctx); }
        return 1;
    }
    if (dolrecomp_physical_pc_alias(ctx, address, &alias)) {
        ctx->pc = alias;
        if (dolrecomp_dispatch_replacement(ctx, alias)) return 1;
        if (g_mg_native_ok) {
            if (!g_mg_native_ok(alias, g_mg_native_ok_user)) return 0;
        } else if (ctx->host_call && ppc_host_call(ctx, alias)) {
            return 1;
        }
        if (dolrecomp_call_original(ctx, alias)) return 1;
    }
    e_dbg_rgen = e->rgen; e_dbg_callpc = e->call_pc;
    return 0;
}

/* Cheap "is this pc's verdict cached" probe for the return-2 contract:
 * true iff the next glue_call would take a cache hit under the current
 * epoch (and, for REL entries, the current rel dispatch generation). */
static int dcache_peek(u32 address)
{
    const u32 set = ((address >> 2) * 2654435761u) >> (32 - GLUE_DCACHE_SETS_LOG2);
    GlueDcacheEntry* e = &s_dcache[set << 1];
    if (e->pc != address) ++e;
    if (e->pc != address || e->gen != s_dcache_gen)
        return 0;
    /* Negative entries are hits (they skip the gauntlet) but still return
     * 0 — the burst must exit for the chassis host_call to fire. Report
     * unverified so the chassis keeps its normal re-probe + host_call path
     * rather than being told (via return 2) the pc is dispatch-ready. */
    if (e->neg)
        return 0;
    if (e->rgen && e->rgen != g_rel_dispatch_gen)
        return 0;
    return 1;
}

/* Same loop contract as dolrecomp_run_budget (generated.h): bail on
 * uncovered pc / exception, stop when the accumulated guest charge crosses
 * the budget, 4096-iteration cap against zero-charge livelock. */
static int glue_run_budget_impl(CPUState* ctx, s64 budget)
{
    int i;
    if (!s_dcache_epoch_armed)
        ++s_dcache_gen;
    if (s_dcache_gen != s_last_gen) { if (s_diag_on > 0) ++s_diag_genmove; s_last_gen = s_dcache_gen; }
    if (s_diag_on > 0) ++s_diag_segs;
    for (i = 0; i < 4096; ++i) {
        if (s_diag_on > 0) {
            ++s_diag_calls;
            if (dolrecomp_call_depth) { ++s_diag_dnz; if (dolrecomp_call_depth > s_diag_dhi) s_diag_dhi = dolrecomp_call_depth; }
            if (ctx->pc == 0x80307EF4u && s_resume_n < 16) s_resume_lr[s_resume_n++] = ctx->lr;
        }
        if (!glue_call(ctx, ctx->pc)) { if (s_diag_on > 0) { ++s_diag_ret0; s_diag_lastpc = ctx->pc; if (ctx->pc >= 0x80500000u && ctx->pc < 0x81800000u && s_diag_ret0 <= 200) fprintf(stderr, "[ret0] pc=%08x lr=%08x\n", ctx->pc, ctx->lr); } return 0; }
        if (ctx->exception) { if (s_diag_on > 0) { if (s_diag_exc < 12) fprintf(stderr, "[exc] pc=%08x srr0=%08x lr=%08x\n", ctx->pc, ctx->srr0, ctx->lr); ++s_diag_exc; } return 0; }
        if (ctx->downcount <= -budget) {
            /* Return 2 when the NEXT pc is already cache-verified: the
             * chassis may skip its per-segment re-probe (the verdict the
             * hit path would apply is pinned by the epoch, so re-running
             * FastDispatchableRunPath + IsHostCallAddress buys nothing). */
            return dcache_peek(ctx->pc) ? 2 : 1;
        }
    }
    return 1;
}

static int glue_run_budget(CPUState* ctx, s64 budget)
{
    if (s_diag_on <= 0)
        return glue_run_budget_impl(ctx, budget);
    const unsigned long long _t0 = mg_rdtsc();
    const int _rc = glue_run_budget_impl(ctx, budget);
    s_t_loop += mg_rdtsc() - _t0;
    { static int _d = -1; static unsigned long long _nt;
      if (_d < 0) { _d = getenv("MG_GLUE_DIAG") ? 1 : 0; _nt = mg_rdtsc() + 8000000000ull; }
      if (_d && mg_rdtsc() >= _nt) {
        _nt = mg_rdtsc() + 8000000000ull;
        fprintf(stderr, "[glue-tick] calls=%llu segs=%llu ret0=%llu exc=%llu hit=%llu miss=%llu coll=%llu relhit=%llu relpc=%llu genmove=%llu gen=%u neghit=%llu negpop=%llu clpop=%llu clthunk=%llu tfn=%llu tloop=%llu ovh=%.1f%%\n",
            s_diag_calls, s_diag_segs, s_diag_ret0, s_diag_exc, s_diag_hit, s_diag_miss, s_diag_coll, s_diag_relhit, s_diag_relpc, s_diag_genmove, s_dcache_gen,
            s_diag_neghit, s_diag_negpop, s_diag_clpop, s_diag_clthunk,
            s_t_fn, s_t_loop, s_t_loop ? 100.0 * (double)(s_t_loop - s_t_fn) / (double)s_t_loop : 0.0);
        phist_dump();
        { unsigned _i; fprintf(stderr, "[epoch-callers]"); for (_i = 0; _i < 8; ++_i) if (s_epoch_caller_n[_i]) fprintf(stderr, " %llx=%u", s_epoch_caller_pc[_i], s_epoch_caller_n[_i]); fprintf(stderr, "\n"); memset(s_epoch_caller_n, 0, sizeof(s_epoch_caller_n)); memset(s_epoch_caller_pc, 0, sizeof(s_epoch_caller_pc)); }
        if (s_resume_n) { unsigned _i; fprintf(stderr, "[resume-lr]"); for (_i = 0; _i < s_resume_n; ++_i) fprintf(stderr, " %08x", s_resume_lr[_i]); fprintf(stderr, "\n"); s_resume_n = 0; }
      } }
    return _rc;
}

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Bounded dispatch trace (MODERNGEKKO_DISPATCH_TRACE=1): logs the first
// s_trace_limit dispatched addresses, then a per-second heartbeat with the
// latest pc + total count. Used to locate boot stalls in the REL loader
// bring-up (G6). Zero cost when the env var is unset.
static int s_trace_on;
static unsigned s_trace_count;
static unsigned s_trace_limit;
static unsigned s_trace_last_pc;
static long s_trace_last_sec;

// Rolling PC histogram (MODERNGEKKO_DISPATCH_TRACE=1): bucketed by PC>>4,
// dumped top-N at each heartbeat. Zero cost when the env var is unset.
#define HIST_BUCKETS 8192
static struct { uint32_t pc; uint32_t count; } s_hist[HIST_BUCKETS];

static void hist_add(uint32_t address)
{
    uint32_t key = (address >> 4) & (HIST_BUCKETS - 1u);
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        uint32_t k = (key + i) & (HIST_BUCKETS - 1u);
        if (s_hist[k].pc == address || s_hist[k].count == 0u) {
            s_hist[k].pc = address;
            s_hist[k].count++;
            return;
        }
    }
    /* 4-way full: replace the least-hot slot */
    uint32_t best = key, j;
    for (j = 1; j < 4u; j++) {
        uint32_t k = (key + j) & (HIST_BUCKETS - 1u);
        if (s_hist[k].count < s_hist[best].count)
            best = k;
    }
    s_hist[best].pc = address;
    s_hist[best].count = 1;
}

static void hist_dump(long now_ms)
{
    uint32_t idx[HIST_BUCKETS];
    uint32_t n = 0, i, j;
    for (i = 0; i < HIST_BUCKETS; i++) {
        if (s_hist[i].count)
            idx[n++] = i;
    }
    /* insertion sort by count desc (n small after dedup by bucket) */
    for (i = 1; i < n; i++) {
        uint32_t v = idx[i];
        j = i;
        while (j > 0 && s_hist[idx[j - 1]].count < s_hist[v].count) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
    fprintf(stderr, "[hist] t=%ldms top12:\n", now_ms);
    for (i = 0; i < n && i < 12u; i++) {
        fprintf(stderr, "  %8u 0x%08X\n", s_hist[idx[i]].count, s_hist[idx[i]].pc);
    }
    memset(s_hist, 0, sizeof(s_hist));
}

// Dump the guest OS thread queue: head of __OSActiveThreadQueue at
// 0x800000DC, OSThread.active_threads_link at +0x2FC (next at +0x2FC).
// Each thread's saved context: srr0 at +0x198 (where it resumes), lr at
// +0x84, stack_base at +0x304. Reads go through the same RAM window the
// dispatch uses (journal base).
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline int32_t be32s(const uint8_t* p) { return (int32_t)be32(p); }

static void thread_dump(CPUState* state, long now_ms)
{
    uint8_t* ram = state->ram;
    uint32_t head;
    uint32_t t;
    int n = 0;
    /* The fixed lowmem reads below reach ram+0xEB — require the window, not
     * just the pointer (the OSThread walk itself is already span-checked). */
    if (!ram || state->ram_size < 0x100u)
        return;
    head = be32(ram + 0xDC); /* __OSActiveThreadQueue.head */
    fprintf(stderr, "[thr] t=%ldms active_head=0x%08X current=0x%08X\n", now_ms, head,
            be32(ram + 0xE4));
    fprintf(stderr, "[thr]   lowmem C0=0x%08X C4=0x%08X C8=0x%08X D0=0x%08X D4=0x%08X D8=0x%08X DC=0x%08X E0=0x%08X E4=0x%08X E8=0x%08X\n",
            be32(ram + 0xC0), be32(ram + 0xC4), be32(ram + 0xC8),
            be32(ram + 0xD0), be32(ram + 0xD4), be32(ram + 0xD8),
            be32(ram + 0xDC), be32(ram + 0xE0), be32(ram + 0xE4),
            be32(ram + 0xE8));
    for (t = head; t >= 0x80000000u && t < 0x81800000u && n < 16; n++) {
        uint32_t base = t - 0x80000000u;
        /* Highest field read is stack_base at +0x304 (4 bytes): the whole
         * 0x308-byte OSThread window must fit inside ram_size. */
        if (base + 0x308u > state->ram_size)
            break;
        fprintf(stderr,
                "[thr]   0x%08X state=%u prio=%d susp=%d srr0=0x%08X lr=0x%08X "
                "q=0x%08X stack=0x%08X\n",
                t, be16(ram + base + 0x2C8), be32s(ram + base + 0x2D0),
                be32s(ram + base + 0x2CC), be32(ram + base + 0x198),
                be32(ram + base + 0x84), be32(ram + base + 0x2DC),
                be32(ram + base + 0x304));
        t = be32(ram + base + 0x2FC); /* active_threads_link.next */
        if (t == head)
            break; /* full cycle */
    }
}

// Walk the guest call stack from the current SP (the dispatch boundary syncs
// GPRs, so state->gpr[1] is the guest SP at block entry).
static void stack_dump(CPUState* state)
{
    uint8_t* ram = state->ram;
    uint32_t sp = state->gpr[1];
    int depth;
    if (!ram)
        return;
    fprintf(stderr, "[stk] sp=0x%08X pc=0x%08X lr=0x%08X\n", sp, state->pc, state->lr);
    for (depth = 0; depth < 24; depth++) {
        uint32_t base;
        uint32_t back, lrsave;
        if (sp < 0x80000000u || sp >= 0x81800000u)
            break;
        base = sp - 0x80000000u;
        if (base + 8 >= state->ram_size)
            break;
        back = be32(ram + base);
        lrsave = be32(ram + base + 4);
        if (lrsave >= 0x80000000u && lrsave < 0x81800000u)
            fprintf(stderr, "[stk]   +%d ret=0x%08X\n", depth, lrsave);
        if (back <= sp || back >= 0x81800000u)
            break;
        sp = back;
    }
}

static void module_dispatch_trace(uint32_t address)
{
    struct timespec ts;
    long now_ms;
    static long prev_ms;

    s_trace_count++;
    s_trace_last_pc = address;
    hist_add(address);
    if (s_trace_count <= s_trace_limit) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        fprintf(stderr, "[dispatch] #%u pc=0x%08X t=%ldms gap=%ldms\n", s_trace_count,
                address, now_ms, now_ms - prev_ms);
        prev_ms = now_ms;
    }
}

// The chassis (Dolphin StaticRecomp core) calls dispatch(state, address) with
// state->pc == address and expects one guest segment per call: it flushes the
// module's accumulated cycle charges into downcount after each dispatch, which
// is its back-edge timing check for CoreTiming to regain control (external-
// interrupt latency parity). When the A1 batch channel is armed, a "segment"
// is a whole cycle-budgeted batch of blocks — the same worst-case granularity
// an in-chunk loop back-edge already keeps — so the chassis deadline is still
// observed at the identical cadence. Returns 1 when the segment was covered,
// 0 when pc fell outside module coverage (interpreter fallback) or the guest
// raised an exception (pc already redirected to the exception vector by
// DolRecomp's runtime).
static int module_run_segment(CPUState* state)
{
    /* Batched block driver (the second half of the A1 channel): every block
     * still passes through dolrecomp_call — replacement dispatcher first,
     * then the g_mg_native_ok query (the exact predicate the chassis applies
     * at block boundaries: fast-dispatchable chunk && !IsHostCallAddress),
     * then the generated original — so hooked, demoted, unverified and
     * uncovered pcs surface to the chassis with ctx->pc set exactly as a
     * one-block dispatch would leave them. The batch ends once the
     * accumulated charge crosses dolrecomp_cycle_budget, matching the bound
     * in-chunk loop back-edges already impose; guest-visible timing/event
     * latency is unchanged by construction. ctx->timebase freshness is also
     * bounded by that same charge (finish_segment refills it per segment).
     * With the query unarmed (lockstep verification, older chassis) keep the
     * strict one-block contract so per-block Verify and the legacy
     * ppc_host_call probe hold. */
    if (g_mg_native_ok)
        return glue_run_budget(state, dolrecomp_cycle_budget);
    return dolrecomp_run_blocks(state, 1);
}

static int module_dispatch(CPUState* state, uint32_t address)
{
    struct timespec ts;
    long now_ms;
    static long prev_ms;

    if (s_trace_on) {
        module_dispatch_trace(address);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (now_ms - s_trace_last_sec > 3000L) {
            s_trace_last_sec = now_ms;
            fprintf(stderr, "[dispatch] heartbeat t=%ldms total=%u last_pc=0x%08X msr=0x%08X gap=%ldms\n",
                    now_ms, s_trace_count, s_trace_last_pc, state->msr, now_ms - prev_ms);
            hist_dump(now_ms);
            thread_dump(state, now_ms);
            stack_dump(state);
        }
        prev_ms = now_ms;
    }
    /* LRELOC P1 finding-4 + P2: first-class relocated-bank execution
     * evidence. Two signals here:
     *   l_dispatch / hi_water -- chassis-visible dispatches into EXRAM pcs.
     *     Rare BY DESIGN: cross-chunk guest calls are host recursion inside
     *     dolrecomp_run_blocks, so REL blocks mostly never resurface at this
     *     boundary (confirmed: 36 M dispatches over 45 s, zero EXRAM pcs).
     *   g_exram_direct_hits -- shared counter bumped by EVERY inline
     *     second-window hit in cpu.h (direct accessors + xlat leg); chunks
     *     reach their relocated L-bank ONLY through those probes, so its
     *     delta is unambiguous proof of active L-bank traffic. */
    {
        static unsigned long long l_dispatch_n;
        static uint32_t l_dispatch_hi_water;
        static unsigned long long l_xhits_last;
        int want_print = 0;
        struct timespec lts;
        long now_l;
        static long l_dispatch_last_ms;
        static uint32_t l_clock_probe;

        if (address >= GC_EXRAM_BASE) {
            l_dispatch_n++;
            if (address > l_dispatch_hi_water)
                l_dispatch_hi_water = address;
            want_print = (l_dispatch_n == 1);
        } else if (l_dispatch_n == 1) {
            want_print = 1; /* follow-up line closes out a lone entry */
        }
        if (g_exram_direct_hits - l_xhits_last >= (1ull << 26))
            want_print = 1;
        /* A lone relocated-bank entry can keep want_print set forever.
         * Bound diagnostic clock reads without changing guest block timing. */
        if (!want_print || ((++l_clock_probe & 4095u) != 1u))
            return module_run_segment(state); /* hot exit: no clock call */
        clock_gettime(CLOCK_MONOTONIC, &lts);
        now_l = lts.tv_sec * 1000L + lts.tv_nsec / 1000000L;
        if (!(l_dispatch_n || g_exram_direct_hits))
            goto l_dispatch_done; /* nothing to report yet */
        if (now_l - l_dispatch_last_ms < 5000L)
            goto l_dispatch_done;
        fprintf(stderr,
                "[loader] l_dispatch=%llu hi_water=0x%08X xhits=%llu\n",
                l_dispatch_n, l_dispatch_hi_water, g_exram_direct_hits);
        l_dispatch_last_ms = now_l;
        l_xhits_last = g_exram_direct_hits;
    l_dispatch_done:;
    }
    return module_run_segment(state);
}

#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void staticrecomp_dispatch_guarded_v1(CPUState* state, u32 address,
                                      u32 max_segments, void* opaque,
                                      StaticRecompContinueV1 continuation)
{
    if (!continuation || max_segments == 0)
        return;
    for (u32 i = 0; i < max_segments; ++i) {
        module_dispatch(state, address);
        /* The host must see even the final segment's charges and exceptions. */
        if (!continuation(opaque, state, &address, max_segments - i - 1))
            break;
    }
}

// Defined in port/loader/rel_loader.c: rebuilds the s_active[] REL table from
// the restored guest OSModuleInfo queue when its signature diverges (state
// restore). Guarded to a couple of guest loads per call — on_state_loaded
// fires on every SyncIn, not just on restore.
extern int rel_loader_resync_active(CPUState* state);

// Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
static void module_on_state_loaded(CPUState* state)
{
#ifdef MODERNGEKKO_INLINE_XLAT_FULL
    /* W-c O3 full-form guard (Advisor P4): the mem_*_direct accessors compile
     * the GC main-RAM window in as constants, so they are only valid against
     * a GC-sized main RAM. This hook is the chassis->module handoff — the one
     * point where the host's configured geometry is visible before any
     * generated code runs. Mirror of the chassis requirement-check in
     * LegacyRuntime::LoadModule: fail LOUDLY instead of corrupting silently
     * if future host wiring changes the RAM size. */
    if (state->ram == NULL || state->ram_size != GC_MAIN_RAM_SIZE) {
        fprintf(stderr,
                "FATAL: full-form direct memory accessors require a %u-byte "
                "main RAM; chassis provided %u bytes (%s pointer). Aborting.\n",
                GC_MAIN_RAM_SIZE, state->ram_size,
                state->ram ? "non-NULL" : "NULL");
        abort();
    }
#endif
    ppc_fpscr_updated(state);
    /* REL link state (and thereby replacement verdicts) may have changed;
     * expire all cached dispatch verdicts -- but only when the queue
     * signature actually moved. This runs on every SyncIn (per burst), so
     * an unconditional bump flushed the whole cache tens of thousands of
     * times per second. resync_active is signature-gated and reports
     * whether it rebuilt. */
    if (rel_loader_resync_active(state))
        ++s_dcache_gen;
}

#include "module_tables.inc"

// Phase 2 (REL bring-up): 415 retail RELs linked at 0x80500000..0x80C85F60
// by the E1 folder-mode batch. rel-modules-data.inc (generated by
// scripts/port/gen_rel_descriptor.py from batch output + REL headers —
// never hand-written) carries the REL code/chunk/hash tables and the
// per-module registration arrays. The ABI exposes ONE sorted code-range
// set, so the DOL and REL tables are concatenated: DOL occupies
// 0x80003100..0x80338680, REL starts at 0x805000D4, so the merge is the
// concatenation in address order. Built at load via a constructor (runs
// before staticrecomp_get_module is resolved through dlopen).
#include "rel-modules-data.inc"

/* Generated-table integrity: the merge below trusts each count macro to
 * equal its array's true length — a regen that desynced a MODULE_*_COUNT or
 * REL_MODULE_*_COUNT from its table would under- or over-run the merged
 * destination. Pin the agreement at compile time. */
#define GLUE_CASSERT(name, expr) \
    typedef char glue_cassert_##name[(expr) ? 1 : -1]
#define GLUE_CASSERT_COUNT(name, array, count) \
    GLUE_CASSERT(name, sizeof(array) == (count) * sizeof((array)[0]))

GLUE_CASSERT_COUNT(dol_code_ranges, s_code_ranges, MODULE_CODE_RANGE_COUNT);
GLUE_CASSERT_COUNT(dol_smc_ranges, s_smc_ranges, MODULE_SMC_RANGE_COUNT);
GLUE_CASSERT_COUNT(dol_chunk_ranges, s_chunk_ranges, MODULE_CHUNK_RANGE_COUNT);
GLUE_CASSERT_COUNT(dol_chunk_hashes, s_chunk_hashes, MODULE_CHUNK_RANGE_COUNT);
GLUE_CASSERT_COUNT(rel_code_ranges, s_rel_code_ranges, REL_MODULE_CODE_RANGE_COUNT);
GLUE_CASSERT_COUNT(rel_chunk_ranges, s_rel_chunk_ranges, REL_MODULE_CHUNK_RANGE_COUNT);
GLUE_CASSERT_COUNT(rel_chunk_hashes, s_rel_chunk_hashes, REL_MODULE_CHUNK_RANGE_COUNT);
GLUE_CASSERT_COUNT(rel_modules, s_rel_modules, MODULE_REL_MODULE_COUNT);

#undef GLUE_CASSERT_COUNT
#undef GLUE_CASSERT

static StaticRecompRange s_code_ranges_merged[MODULE_CODE_RANGE_COUNT +
                                              REL_MODULE_CODE_RANGE_COUNT];
static StaticRecompRange s_chunk_ranges_merged[MODULE_CHUNK_RANGE_COUNT +
                                               REL_MODULE_CHUNK_RANGE_COUNT];
static uint64_t s_chunk_hashes_merged[MODULE_CHUNK_RANGE_COUNT +
                                      REL_MODULE_CHUNK_RANGE_COUNT];

__attribute__((constructor)) static void merge_rel_tables(void)
{
    const char* tr = getenv("MODERNGEKKO_DISPATCH_TRACE");
    const char* cb = getenv("MODERNGEKKO_CYCLE_BUDGET");
    s_trace_on = tr && *tr && tr[0] != '0';
    s_diag_on = getenv("MG_GLUE_DIAG") ? 1 : 0;
    s_trace_limit = 4000;
    if (cb && *cb) {
        /* Sweepable dispatch-overhead knob (bench agent). Clamp keeps the
         * bound meaningful: below 64 the batch degenerates toward one block
         * and can under-shoot the charge of a single loop burst; the ceiling
         * (16384 guest cycles ~= 0.34ms at 48.6MHz) keeps worst-case
         * CoreTiming/async-interrupt latency far inside a VI frame even
         * though the mid-batch window skips finish_segment's exception
         * re-check. The value only gates WHEN control returns to the
         * chassis, never what executes — every guest instruction still
         * runs in order. */
        long v = strtol(cb, NULL, 10);
        if (v < 64) v = 64;
        if (v > (1L << 14)) v = (1L << 14);
        dolrecomp_cycle_budget = (s64)v;
        fprintf(stderr, "[module] cycle budget = %lld\n",
                (long long)dolrecomp_cycle_budget);
    }
    memcpy(s_code_ranges_merged, s_code_ranges, sizeof(s_code_ranges));
    memcpy(s_code_ranges_merged + MODULE_CODE_RANGE_COUNT, s_rel_code_ranges,
           sizeof(s_rel_code_ranges));
    memcpy(s_chunk_ranges_merged, s_chunk_ranges, sizeof(s_chunk_ranges));
    memcpy(s_chunk_ranges_merged + MODULE_CHUNK_RANGE_COUNT, s_rel_chunk_ranges,
           sizeof(s_rel_chunk_ranges));
    memcpy(s_chunk_hashes_merged, s_chunk_hashes, sizeof(s_chunk_hashes));
    memcpy(s_chunk_hashes_merged + MODULE_CHUNK_RANGE_COUNT, s_rel_chunk_hashes,
           sizeof(s_rel_chunk_hashes));
}

static const ModernGekkoModuleDesc s_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION, /* 3u */
    GXRUNTIME_CPU_ABI_VERSION, /* 3u; == MODERNGEKKO_CPU_ABI_VERSION */
    (uint32_t)sizeof(CPUState),
    "GZLE01",
    DOLRECOMP_ENTRY_POINT, /* 0x80003140u */
    module_dispatch,
    module_on_state_loaded,
    s_code_ranges_merged,
    MODULE_CODE_RANGE_COUNT + REL_MODULE_CODE_RANGE_COUNT, /* 2 + 415 */
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges_merged,
    MODULE_CHUNK_RANGE_COUNT + REL_MODULE_CHUNK_RANGE_COUNT, /* 206 + 542 */
    s_chunk_hashes_merged,
    s_rel_modules,
    MODULE_REL_MODULE_COUNT, /* 415u */
};

MODERNGEKKO_MODULE_EXPORT const ModernGekkoModuleDesc* staticrecomp_get_module(void)
{
    return &s_descriptor;
}

// Dol-watch filter (phase2-dol-watch-spec.md §1.1): the SMC-guarded DOL
// chunk ranges (module_tables.inc, address-ordered) exported for the
// rel_loader write-journal ring — only stores landing inside these ranges
// are journaled (legit DOL-text stores are ~0-1 per run; the ring therefore
// holds the whole session history of corruption-class writes).
MODERNGEKKO_MODULE_EXPORT uint32_t ppc_smc_dol_chunk_count(void)
{
    return MODULE_CHUNK_RANGE_COUNT;
}
MODERNGEKKO_MODULE_EXPORT const uint32_t* ppc_smc_dol_chunk_ranges(void)
{
    return (const uint32_t*)s_chunk_ranges; /* pairs: start,end, start,end, ... */
}

__attribute__((destructor)) static void glue_diag_dump(void)
{
    if (!getenv("MG_GLUE_DIAG")) return;
    fprintf(stderr,
            "[glue-diag] segs=%llu calls=%llu calls/seg=%.2f ret0=%llu exc=%llu "
            "hit=%llu miss=%llu repl=%llu relhit=%llu relpop=%llu relgen=%llu "
            "relpc=%llu lastpc=%08x dnz=%llu dhi=%llu genmove=%llu gen=%u nok=%llu nokf=%llu "
            "neghit=%llu negpop=%llu "
            "tfn=%llu tloop=%llu ovh=%.1f%%\n",
            s_diag_segs, s_diag_calls,
            s_diag_segs ? (double)s_diag_calls / (double)s_diag_segs : 0.0,
            s_diag_ret0, s_diag_exc, s_diag_hit, s_diag_miss, s_diag_repl,
            s_diag_relhit, s_diag_relpop, s_diag_relgen, s_diag_relpc,
            s_diag_lastpc, s_diag_dnz, s_diag_dhi, s_diag_genmove, s_dcache_gen, s_nok_calls, s_nok_fail,
            s_diag_neghit, s_diag_negpop,
            s_t_fn, s_t_loop, s_t_loop ? 100.0 * (double)(s_t_loop - s_t_fn) / (double)s_t_loop : 0.0);
}

/* mg_lookup_fn — cross-image resolver for generated-code site inline caches
 * (declared in cycle_budget.h, force-included into every chunk TU). The DOL
 * half uses the shared generated.h find_original; the REL half lives in
 * rel_loader.c where the chunk table + s_active[] owner check exist. */
extern void (*mg_rel_lookup(u32 pc, u32* resolved_pc))(CPUState*);
void (*mg_lookup_fn(u32 pc, u32* resolved_pc))(CPUState*)
{
    *resolved_pc = pc;
    if (pc < 0x80500000u)
        return dolrecomp_find_original(pc);
    return mg_rel_lookup(pc, resolved_pc);
}
