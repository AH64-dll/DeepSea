#ifndef MODERNGEKKO_CYCLE_BUDGET_H
#define MODERNGEKKO_CYCLE_BUDGET_H

/* Force-included ahead of every generated module TU (the module CMakeLists
 * and rebuild_loader.sh pass -include with this header) so
 * DOLRECOMP_C_LOOP_CYCLE_BUDGET resolves to a runtime value instead of
 * generated.h's literal default. The variable bounds the guest-cycle
 * charge accumulated between chassis checks — in-chunk loop back-edges and
 * the module-side batched block driver share it — so it changes only how
 * OFTEN control returns to the dispatcher, never which guest instructions
 * run. The default (256) reproduces the generated.h constant exactly;
 * MODERNGEKKO_CYCLE_BUDGET overrides it at load (see module_glue.c). */
#include <stdint.h>

extern int64_t dolrecomp_cycle_budget;
#define DOLRECOMP_C_LOOP_CYCLE_BUDGET (dolrecomp_cycle_budget)

/* Dispatch-verdict epoch (module_glue.c): bumped by the chassis at every
 * event that can change a pc's dispatch verdict (hooks, REL relinks, SMC
 * invalidation, forced fallback, state restore) and once per batch on
 * runners that predate the epoch channel. Generated code reads it for
 * per-site direct-call verdict caches; the module glue uses it for the
 * pc->fn dispatch cache. */
extern uint32_t g_mg_dcache_gen;

/* REL active-set generation (rel_loader.c): bumped on every link/unlink/
 * resync that mutates s_active[]. Site caches resolving REL targets key on
 * this alongside g_mg_dcache_gen so a module unload invalidates them. */
extern uint32_t g_rel_dispatch_gen;

/* Chassis verdict callback (installed via ppc_set_native_check). Declared
 * here so REL-generated chunks (whose own generated.h predates the channel)
 * can query it identically to DOL chunks. */
extern int (*g_mg_native_ok)(uint32_t address, void* user);
extern void* g_mg_native_ok_user;

struct CPUState;
/* Cross-image dispatch lookup for generated-code inline caches. Resolves a
 * branch target to its compiled chunk fn:
 *  - DOL pc                    -> the DOL chunk table
 *  - L-band pc (0x91xxxxxx)    -> the REL chunk table, only while an active
 *                               module's L window owns it
 *  - retail-alias pc           -> the owning module's L twin (reported via
 *                               *resolved_pc so the site replays the twin)
 * Returns NULL for uncompiled / inactive / E4-gate targets; callers fall
 * back to the normal dispatch path. */
extern void (*mg_lookup_fn(uint32_t pc, uint32_t* resolved_pc))(struct CPUState*);

/* Last claimed REL dispatch's resolved (pc, fn), published post-call by
 * dolrecomp_dispatch_replacement (rel_loader.c). Site caches memoize the
 * pair keyed on both generations; 0 means "not memoizable" (E4 gate and
 * non-REL claims leave it cleared). */
extern void (*g_rel_dispatched_fn)(struct CPUState*);
extern uint32_t g_rel_dispatched_pc;

#endif /* MODERNGEKKO_CYCLE_BUDGET_H */
