// rel_loader.c — REL load/unload integration (G6 / E4).
//
// Replacement hooks for the game's OSLink-driven loader (OSLink, OSLinkFixed,
// OSUnlink, OSSetStringTable, __OSModuleInit), the guest write journal
// (R->L data mirror + code-range trap), and the REL chunk dispatcher that
// routes linked REL addresses to the compiled chunk functions.
//
// Design: port/loader/README.md + .org/port/evidence/phase2-loader.md.
// Two deliberate deviations from the design text, both grounded in the
// descriptor generator + chassis source (see phase2-loader-impl.log):
//
//  (1) The R copy's header word +0x10 (sectionInfoOffset) is NOT bumped by R.
//      StaticRecompCore::RefreshRelSections (StaticRecompCore_SMC.cpp:38)
//      matches guest RAM +0x10 against the DESCRIPTOR's raw
//      section_info_offset; a retail-style fixup would make every module
//      undiscoverable and V2 impossible. The section table itself is still
//      located at R + raw_sio, so every other consumer (game crash-dump
//      scans, dump2) that dereferences the header pointer would read the
//      table at 0x4C instead of R + 0x4C — a cosmetic degradation on
//      debug/crash paths only (RAM is mapped; reads are safe).
//
//  (2) Relocation writes to CODE sections are NOT applied on the R copy.
//      The descriptor chunk hashes are FNV-1a over the RAW retail file bytes
//      (scripts/port/gen_rel_descriptor.py:355-362) and
//      StaticRecompCore::VerifyChunk hashes guest RAM at the chunk's runtime
//      address (StaticRecompCore_SMC.cpp:282-330). Applying linked-resolved
//      fixups would make every REL chunk CHUNK_FAILED. The R copy's code
//      bytes stay raw-file-identical, which is exactly what the SMC guard
//      expects; compiled chunks (built from the relocated owned_data) are the
//      code. Data-section relocations are applied retail-exact (runtime
//      bases) so game-held/data-stored pointers stay runtime addresses.
//
// Journal ordering: the bundled GXRuntime mem_write* helpers (core/cpu.h)
// were patched to invoke the write journal AFTER the write lands, so the
// handler mirrors the exact written bytes R->L (pre-write invocation cannot
// see the final stored value).

// NOTE: core/cpu.h MUST be included before any moderngekko/*.h header:
// moderngekko/cpu_state.h shares the DOLRECOMP_CPU_H include guard and only
// forward-declares CPUState, so including it first would skip the real
// definition (and the u8/u16/u32 typedefs from core/types.h).
#include "core/cpu.h"

#include "rel_loader_chunks.h"
#include "rel-modules-data.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Declared here (not in core/cpu.h; defined in the bundled GXRuntime cpu.c).
extern void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);
/* W-c O2 journal page-filter installer (bundled GXRuntime cpu.c export). */
extern void ppc_set_mem_write_journal_filter(const uint64_t* words, uint32_t word_count);
/* W-c O3 phase 1 inline-translation armer (bundled GXRuntime cpu.c export). */
extern void ppc_set_mem_inline_xlat(int enable);
// Dol-watch filter table (module_glue.c exports): SMC-guarded DOL chunk
// ranges from module_tables.inc (start,end pairs, address-ordered).
extern uint32_t ppc_smc_dol_chunk_count(void);
extern const uint32_t* ppc_smc_dol_chunk_ranges(void);

#define REL_LOADER_QUEUE_HEAD 0x800030C8u
#define REL_LOADER_QUEUE_TAIL 0x800030CCu
#define REL_LOADER_STRING_TABLE 0x800030D0u
#define REL_LOADER_MAX_ACTIVE 415u
#define REL_LOADER_FPC_CTQ_HEAD 0x80372690u /* g_fpcCtTg_Queue.mpHead */

// B24/pulse10: the guest game heap spans the static module L windows, so an
// in-flight f_pc create request (or its unpublished process object) can be
// sitting inside the window a link is about to fill. The gen-refresh
// machinery protects MODULE data from guest writes, but nothing protected
// the guest: link's file-copy/bss-zero overwrote a live Lpalm process at
// 0x80AA3374 (module 273 relink) mid-creation, and its request then spun
// forever in fpcSCtRq_phase_SubCreateProcess on a NULL method table.
// Refuse such links up-front; retail degrades gracefully through the DMC
// link-failure path (cancel request, OSReport_Error) instead of freezing.

#define R_DOLPHIN_NOP 201u
#define R_DOLPHIN_SECTION 202u
#define R_DOLPHIN_END 203u

// Per-module active-link record for the write journal (R = runtime arena
// base, L = linked base). Section classification reads the R copy's own
// section table live (entries are absolute after the link fixups).
typedef struct {
    u32 r;            // runtime header address (arena)
    u32 l;            // linked (compiled) module base
    u32 file_size;    // descriptor file_size (raw image size)
    u32 sio;          // RAW section_info_offset (== descriptor; NOT fixed up)
    u32 num_sections; // section_count from the image header
    u32 id;           // module id (descriptor)
    u32 l_dirty;      // L data region clobbered by the game heap since last refresh
    u32 probe_off;    // R-relative offset of first non-exec data section
    u32 probe_size;   // its size
    u32 l_gen;        // heap generation at last L verification
} RelActiveModule;

static RelActiveModule s_active[REL_LOADER_MAX_ACTIVE];
static u32 s_active_count;
static u32 s_heap_gen; /* bumped on every JKRExpHeap::do_alloc */
/* Dispatch memos (dolrecomp_dispatch_replacement hot path). s_active_seq is
 * bumped at every site that mutates s_active[] (link add, unlink
 * swap-remove, queue rebuild, OSModuleInit clear); the memos below rebuild
 * lazily on the first REL dispatch after a change. */
static u32 s_active_seq;
static u32 s_memo_seq;
static const RelLoaderChunkEntry* s_last_chunk; /* last binsearch hit */
static u32 s_last_owner = 0xFFFFFFFFu;          /* s_active idx containing last addr */
static int s_l_overlap;                          /* L ranges overlap -> owner memo off */
static u32 s_e4_addr;                            /* module-1 l+0xEC, 0 = absent */
static u32 s_e4_val;                             /* its r+0x178 */
static int s_e4_dup;                             /* >1 id-1 entry -> original scan */
static u32 s_carve_rearm_seq; /* bumped on every carve re-arm (r7 re-init) */
static u8* s_ram;
static u32 s_ram_size;
static u8* s_exram;     /* GXRuntime EXRAM window base (ctx->exram) */
static u32 s_exram_size;
static int s_debug;
/* inside a REL chunk fn: L writes are the module's own. Non-static so
 * module_glue's REL dispatch cache can wrap direct chunk calls. */
int g_rel_in_chunk;
#define s_in_rel_chunk g_rel_in_chunk

/* REL dispatch-cache validity counter (module_glue s_dcache REL entries).
 * Bumped at every mutation that can change a REL dispatch outcome:
 * s_active[] link/unlink/rebuild, l_dirty journal flags (L-image clobber),
 * and s_heap_gen bumps. Entries compare a single counter, which keeps the
 * glue hit path to one extra load+cmp. */
u32 g_rel_dispatch_gen = 1;
/* Set by the REL chunk dispatch just before the chunk fn runs: the resolved
 * fn when this dispatch is safe to memoize, NULL when it is not (E4 gate
 * address needs its post-call fixup; the retail->twin redirect rewrites
 * ctx->pc so the entry could not reproduce it). Cleared/assigned only on
 * paths that actually dispatch a chunk. */
void (*g_rel_dispatched_fn)(CPUState* ctx);
u32 g_rel_dispatched_pc;

/* Queue signature as last synced into s_active[]. rel_loader_link/unlink keep
 * the table authoritative during normal play and update this after their
 * queue writes; rel_loader_resync_active rebuilds only when the live queue
 * diverges from it (i.e. a state restore changed the queue behind our back)
 * or the table is empty while the queue is not. */
static u32 s_qsig_head, s_qsig_tail;

/* Conservative envelopes avoid scanning every active module for unrelated
 * stack/heap writes. Keep R and L separate: EXRAM mirrors must not turn the
 * entire intervening address space into a candidate. No section data is
 * cached; matching writes still use the original live section-table scan. */
static u32 s_journal_r_lo, s_journal_l_lo;
static u64 s_journal_r_hi, s_journal_l_hi;
static int s_journal_bounds_valid;

static void rel_loader_rebuild_journal_bounds(void) {
    u32 i;
    s_journal_r_lo = s_journal_l_lo = UINT32_MAX;
    s_journal_r_hi = s_journal_l_hi = 0;
    s_journal_bounds_valid = 1;
    for (i = 0; i < s_active_count; ++i) {
        const RelActiveModule* m = &s_active[i];
        u64 end = (u64)m->r + m->file_size;
        if (end > UINT32_MAX) s_journal_bounds_valid = 0;
        if (m->r < s_journal_r_lo) s_journal_r_lo = m->r;
        if (end > s_journal_r_hi) s_journal_r_hi = end;
        if (m->l) {
            end = (u64)m->l + m->file_size;
            if (end > UINT32_MAX) s_journal_bounds_valid = 0;
            if (m->l < s_journal_l_lo) s_journal_l_lo = m->l;
            if (end > s_journal_l_hi) s_journal_l_hi = end;
        }
    }
}

static int rel_loader_journal_may_touch_module(u32 guest, u32 size) {
    const u64 end = (u64)guest + size;
    if (!s_journal_bounds_valid || end > UINT32_MAX)
        return 1; /* Preserve the original scan on unusual/wrapping inputs. */
    if (guest >= s_journal_r_lo && (u64)guest < s_journal_r_hi)
        return 1; /* R classification uses the write START, as before. */
    if (!s_in_rel_chunk && end > s_journal_l_lo && (u64)guest < s_journal_l_hi)
        return 1; /* L heap fills may start below the mirrored range. */
    return 0;
}

// ---------------------------------------------------------------------------
// DOL-text write watch (phase2-dol-watch-spec.md Delta B refinement): a
// bounded journal of stores into SMC-guarded DOL chunk ranges ONLY, so a
// VerifyChunk mismatch (0x800056E0 wild-write family, dig §6d) can be
// correlated to the exact writer store. Filtered rate into DOL text is
// ~0-1/run (TWW DOL code is not self-modifying), so a 256-entry ring holds
// the ENTIRE session history — at an SMC mismatch the corruption entry is
// uniquely present with offset/size/value/seq/tb. A full-MEM1 ring would
// wrap in seconds at boot store rates and lose the write (the reason for
// the filter). Env STATICRECOMP_SMC_RING_SIZE (default 256, 0 disables).
// ---------------------------------------------------------------------------

#define SMC_RING_MAX 256

typedef struct {
    u32 offset; /* RAM offset (guest - 0x80000000) */
    u32 size;   /* 1/2/4/8 */
    u32 value;  /* bytes read back post-write (BE, size-padded) */
    u64 seq;    /* monotonically increasing store sequence */
    u64 tb;     /* host monotonic ms at store time (sampled, see below) */
} SmcWriteRingEntry;

static SmcWriteRingEntry s_smc_ring[SMC_RING_MAX];
static u32 s_smc_ring_size = SMC_RING_MAX; /* 0 = disabled */
static u64 s_smc_ring_seq;
static u32 s_smc_ring_head; /* next slot to write (oldest when full) */
static u64 s_smc_ring_tb_ms; /* last sampled host ms (every 64th store) */
static u32 s_smc_dol_count; /* from ppc_smc_dol_chunk_count() */
static int s_journal_filter_enabled; /* MODERNGEKKO_JOURNAL_FILTER=1 (W-c O2) */

/* MEM1 is 24 MiB.  A 6144-entry table therefore uses one byte per 4 KiB
 * page, keeping the SMC hot path to one indexed load for pages which are
 * wholly covered by one immutable DOL range.  Boundary pages retain the
 * original binary search because a point lookup must keep its start-address
 * semantics. */
#define SMC_MEM1_BASE 0x80000000u
#define SMC_MEM1_SIZE 0x01800000u
#define SMC_PAGE_SIZE 4096u
#define SMC_PAGE_COUNT (SMC_MEM1_SIZE / SMC_PAGE_SIZE)
#define SMC_PAGE_DISJOINT 0u
#define SMC_PAGE_FULL 1u
#define SMC_PAGE_BOUNDARY 2u
static u8 s_smc_page_class[SMC_PAGE_COUNT];
static int s_smc_page_class_valid;

/* phase3-lreloc-p5b — MODERNGEKKO_SEAM_DIAG=1 (default OFF): cold-boot stream
 * wake isolation around the framework.str completion seam (p4b: DI transport
 * proven complete+acked, wedge isolated below DI/driver). Arms four default-
 * silent observability points, zero cost unset:
 *   [seam] DISPATCH MISS  — REL-domain call entries with no compiled chunk
 *           coverage (falls through to interpreter-at-guest-bytes semantics);
 *   [seam] cbForReadAsync — plus the DVDFileInfo user-callback pointer and
 *           its chunk-coverage verdict (candidate silent-no-op hop);
 *   [seam] ISR            — __DVDInterruptHandler entry pacing;
 *   [seam] SendMsg        — OSSendMessage queue/msg traffic (EXRAM flagged).
 * Logging only mutates stderr; no guest state is touched. */
static int s_seam_diag;
static unsigned s_seam_dispatch_misses;
static unsigned s_seam_isr_n;
static unsigned s_seam_sendmsg_n;
static void rel_loader_rebuild_journal_mask(void);
static const u32* s_smc_dol_ranges; /* start,end pairs (module glue export) */

static void smc_build_page_classification(void) {
    u32 i;
    u32 page;
    int valid = s_smc_dol_ranges != NULL;

    memset(s_smc_page_class, SMC_PAGE_BOUNDARY, sizeof(s_smc_page_class));
    if (valid) {
        u32 prev_end = SMC_MEM1_BASE;
        for (i = 0; i < s_smc_dol_count; ++i) {
            u32 start = s_smc_dol_ranges[i * 2u];
            u32 end = s_smc_dol_ranges[i * 2u + 1u];
            if (start < SMC_MEM1_BASE || end <= start ||
                end > SMC_MEM1_BASE + SMC_MEM1_SIZE || start < prev_end) {
                valid = 0;
                break;
            }
            prev_end = end;
        }
    }
    if (!valid) {
        s_smc_page_class_valid = 0;
        return;
    }

    /* The table is deliberately built only after the range contract has
     * passed validation.  This leaves malformed or uninitialized exports on
     * the exact pre-existing binary-search path. */
    for (page = 0; page < SMC_PAGE_COUNT; ++page) {
        u32 page_start = SMC_MEM1_BASE + page * SMC_PAGE_SIZE;
        u32 page_end = page_start + SMC_PAGE_SIZE;
        u32 overlaps = 0u;
        for (i = 0; i < s_smc_dol_count; ++i) {
            u32 start = s_smc_dol_ranges[i * 2u];
            u32 end = s_smc_dol_ranges[i * 2u + 1u];
            if (end <= page_start)
                continue;
            if (start >= page_end)
                break;
            overlaps++;
            if (overlaps > 1u)
                break;
            s_smc_page_class[page] =
                (start <= page_start && end >= page_end) ?
                    SMC_PAGE_FULL : SMC_PAGE_BOUNDARY;
        }
        if (overlaps == 0u)
            s_smc_page_class[page] = SMC_PAGE_DISJOINT;
        else if (overlaps > 1u)
            s_smc_page_class[page] = SMC_PAGE_BOUNDARY;
    }
    s_smc_page_class_valid = 1;
}

/* Binary search: is guest in [start_i, end_i) of some DOL SMC chunk? */
static int smc_guarded_dol(u32 guest) {
    u32 lo = 0u;
    u32 hi = s_smc_dol_count;
    if (s_smc_page_class_valid && guest >= SMC_MEM1_BASE &&
        guest < SMC_MEM1_BASE + SMC_MEM1_SIZE) {
        u8 cls = s_smc_page_class[(guest - SMC_MEM1_BASE) >> 12];
        if (cls == SMC_PAGE_DISJOINT)
            return 0;
        if (cls == SMC_PAGE_FULL)
            return 1;
        /* Boundary page: continue with the original point lookup below. */
    }
    /* Most journaled writes target data or the stack, outside all DOL code.
     * The same sorted, disjoint range contract is required by the search. */
    if (hi == 0u || s_smc_dol_ranges == NULL || guest < s_smc_dol_ranges[0] ||
        guest >= s_smc_dol_ranges[(hi - 1u) * 2u + 1u])
        return 0;
    while (lo < hi) {
        u32 mid = (lo + hi) >> 1;
        u32 start = s_smc_dol_ranges[mid * 2u];
        if (guest < start)
            hi = mid;
        else if (guest >= s_smc_dol_ranges[mid * 2u + 1u])
            lo = mid + 1u;
        else
            return 1;
    }
    return 0;
}

static u64 smc_ring_tb_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (u64)ts.tv_sec * 1000u + (u64)(ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------------
// p12 — MODERNGEKKO_MT_WATCH=1 (default OFF): method-table write watch.
//
/* Defined below (Guest RAM access section); the mt-watch helpers read the
 * post-write value back from guest RAM. */
static inline u8* rel_any_ptr(u32 guest);
static inline u32 rel_rd32(u32 guest);
// p7 halt evidence: the nullcall context object at 0x80AB1EE0
// (base_process_class; fpcMtd_Method nullcall, mpPcMtd method=0) lives in
// the JKR heap = MEM1, so every store into it crosses the write journal.
// p11 proved the retail pointer 0x806D2550 is NOT in any image-backed
// module data section (18/18 modules swept, zero hits) — yet between p7
// and p11 the method-table slots EVOLVED from 0/0xff filler to the retail
// twin — something WRITES retail values into the table at runtime. This
// instrument captures the writers of [0x80AB1000..0x80AB3000):
//   * ring push per intersecting u32 word: old->new (8 KB shadow of the
//     region, synced pre-guest from the dispatch replacement), size, seq,
//     heap gen, host ms. The journal push carries no pc — correlation is
//     by sequence order relative to the [rel_loader] link rows (which
//     link-generation wrote the retail value);
//   * IMMEDIATE loud line when the post-write word is a 4-aligned pointer
//     in the RETAIL band [0x80500000..0x80C873D0) — no dump needed;
//   * per-link dump of [0x80AB1E80..0x80AB1F60) (the p7 method-table
//     neighborhood) after each link, showing the table evolving;
//   * ppc_mt_dump() export: newest-first ring print, the
//     ppc_smc_dump_write_journal pattern ([mt-watch] prefix).
// Zero cost when MODERNGEKKO_MT_WATCH is unset (single flag test at the
// journal top); no guest state is touched.
// ---------------------------------------------------------------------------

#define MT_WATCH_LO 0x80AB1000u
#define MT_WATCH_HI 0x80AB3000u
#define MT_WATCH_SPAN (MT_WATCH_HI - MT_WATCH_LO)
#define MT_RING_MAX 256
#define MT_RETAIL_LO 0x80500000u
#define MT_RETAIL_HI 0x80C873D0u

typedef struct {
    u32 offset; /* RAM offset (guest - 0x80000000) */
    u32 size;   /* journal write size (1/2/4/8) */
    u32 oldv;   /* shadow u32 before this store (== newv when unsynced) */
    u32 newv;   /* u32 read back post-write */
    u64 seq;    /* monotonically increasing watched-store sequence */
    u32 gen;    /* s_heap_gen at store time (link-generation correlation) */
    u64 tb;     /* host monotonic ms at store time (sampled, 1/64) */
} MtWatchRingEntry;

static int s_mt_watch; /* MODERNGEKKO_MT_WATCH=1 */
static MtWatchRingEntry s_mt_ring[MT_RING_MAX];
static u32 s_mt_ring_head; /* next slot to write (oldest when full) */
static u64 s_mt_ring_seq;
static u64 s_mt_ring_tb_ms;
static u32 s_mt_loud_n;      /* loud-line rate cap */
static u32 s_mt_shadow_sync; /* shadow initialized from live RAM */
static u32 s_mt_shadow[MT_WATCH_SPAN / 4u];

/* Mirror the watched region into the old-value shadow. Called pre-guest
 * from the dispatch replacement (first CPUState hand-off) so the first
 * watched store has a true old value; the lazy journal-path call only
 * covers a watch hit before any dispatch (its entry loses the true old
 * value: oldv == newv). */
static void mt_watch_sync_shadow(void) {
    u32 i;
    /* The shadow copy reads the whole watched window out of guest RAM, so
     * the bound is the window's top, not just s_ram non-NULL — a chassis
     * reporting less than 0xAB3000 bytes of main RAM would otherwise take an
     * out-of-bounds diagnostic read. */
    if (!s_ram || s_mt_shadow_sync ||
        s_ram_size < MT_WATCH_HI - 0x80000000u)
        return;
    for (i = 0; i < MT_WATCH_SPAN; i += 4u)
        s_mt_shadow[i >> 2] =
            read_be32(s_ram + (MT_WATCH_LO - 0x80000000u) + i);
    s_mt_shadow_sync = 1;
}

// ---------------------------------------------------------------------------
// p13 — MODERNGEKKO_THREAD_WATCH=1 (default OFF): OSCreateThread /
// OSResumeThread watch. LRELOC boot p12-watch halts with a thread whose
// saved pc is a RETAIL module-96 twin (0x806D2550, d_a_stone2); the retail
// pc must arrive via the thread's saved OSContext — either the thread was
// CREATED with entry=retail, or its context was overwritten post-create.
// Hooking OSCreateThread (GZLE01 symbols.txt:12946, 0x8030803C) and
// OSResumeThread (GZLE01 symbols.txt:12951, 0x803086A4) names the creator
// (caller lr) and the entry value at create/resume time.
//
// Signature provenance: tww/include/dolphin/os/OSThread.h:97
//   BOOL OSCreateThread(OSThread* thread, void* func, void* param,
//                       void* stack, u32 stackSize, OSPriority priority,
//                       u16 attr);
//   s32 OSResumeThread(OSThread* thread);  (OSThread.h:102)
// OSContext layout provenance: tww/include/dolphin/os/OSContext.h:138-153 —
//   struct OSContext { u32 gpr[32]; /*0x000*/ u32 cr; u32 lr; u32 ctr;
//   u32 xer; f64 fpr[32]; u32 field_0x190; u32 fpscr; u32 srr0; u32 srr1;
//   u16 mode; u16 state; u32 gqr[8]; f64 ps[32]; }
//   => gpr[0] at +0x000, srr0 (saved pc) at +0x198. OSThread.h:57-58:
//   struct OSThread { OSContext context; ... } => context at OSThread+0.
//   A thread created with entry=E has context.gpr[0]=E and, once
//   dispatched-and-switched-out, context.srr0=last execution pc.
// Zero cost when MODERNGEKKO_THREAD_WATCH is unset (single flag test per
// hook); no guest state is touched. Independent of MODERNGEKKO_MT_WATCH
// (p11 sweep / p12 MT watch env gates unchanged).
// ---------------------------------------------------------------------------

static int s_thr_watch; /* MODERNGEKKO_THREAD_WATCH=1 */
static u32 s_thr_loud_n; /* retail-entry loud-line rate cap */

/* ---------------------------------------------------------------------------
 * p14 — MODERNGEKKO_LOADSEAM_TRACE=1 (default OFF): file-select load-seam
 * watch (load-seam-analysis.md §3). Two layers:
 *   A (host): journal watch on the memcard-control writes (g_mDoMemCd_control
 *     0x803B39A0, framework.txt:17363) — every journaled store intersecting
 *     the control struct logs pc-free [loadseam] mc-write rows so the
 *     cmd=52 mount/validity burst and any later mDoMemCd_Save-vs-absence is
 *     directly visible in the boot log.
 *   B (guest): 4 DOL decision-point hooks in the dispatch switch (P1..P4
 *     below) — log the exact decision inputs and return 0 (fall through to
 *     normal dispatch; never consume).
 * Zero cost when MODERNGEKKO_LOADSEAM_TRACE is unset (single flag test per
 * journaled write); no guest state is touched. Independent of the other
 * env gates.
 * ---------------------------------------------------------------------------
 */

static int s_loadseam_trace; /* MODERNGEKKO_LOADSEAM_TRACE=1 */
static int s_repl_libm = 1;    /* MODERNGEKKO_REPL_LIBM=0 disables the
                              * in-module sin/cos replacement cases */

/* g_mDoMemCd_control (m_Do_MemCard.cpp:18; framework.txt:17363). The
 * dFile_select decision fields (field_0x3928/392c/3917[]) are INSTANCE
 * fields on the heap dFs_c object — NOT at fixed addresses — so Layer A
 * sees the card-command stream only; the decision inputs themselves are
 * Layer B's job. Watch the whole control struct (provenance: sizeof not
 * in symbols; 0x1698 is the ctor symbol SIZE column for __ct__15mDoMemCd_Ctrl_c
 * framework.txt:460 — an upper bound is fine, the watch is intersection-based). */
#define LOADSEAM_MC_CTRL_LO 0x803B39A0u
#define LOADSEAM_MC_CTRL_HI 0x803B39A0u + 0x1698u

/* OSCreateThread(OSThread* thread, void* func, void* param, void* stack,
 * u32 stackSize, OSPriority priority, u16 attr) — args in gpr[3..9]. */
static void thr_watch_create(CPUState* ctx) {
    const u32 thread = ctx->gpr[3];
    const u32 entry = ctx->gpr[4];
    const u32 entry_arg = ctx->gpr[5];
    const u32 prio = ctx->gpr[8];
    const u32 attr = ctx->gpr[9];
    fprintf(stderr,
            "[thr-watch] create thread=0x%08X entry=0x%08X entryArg=0x%08X "
            "prio=%u attr=0x%04X from-lr=0x%08X\n",
            thread, entry, entry_arg, prio, attr, ctx->lr);
    if ((entry & 3u) == 0u && entry >= MT_RETAIL_LO && entry < MT_RETAIL_HI) {
        fprintf(stderr,
                "[thr-watch] RETAIL ENTRY thread=0x%08X entry=0x%08X "
                "entryArg=0x%08X prio=%u from-lr=0x%08X\n",
                thread, entry, entry_arg, prio, ctx->lr);
        s_thr_loud_n++;
        if (s_thr_loud_n >= 64u) {
            fprintf(stderr,
                    "[thr-watch] RETAIL ENTRY loud cap reached (%u) — "
                    "further retail-entry lines suppressed\n",
                    s_thr_loud_n);
            s_thr_loud_n = 0;
        }
    }
}

/* OSResumeThread(OSThread* thread) — thread ptr in gpr[3]. While armed,
 * dump the thread's saved-PC slot (context.srr0 @ +0x198) and entry slot
 * (context.gpr[0] @ +0x000) if the struct is readable guest RAM. */
static void thr_watch_resume(CPUState* ctx) {
    const u32 thread = ctx->gpr[3];
    u32 saved_pc = 0;
    u32 entry_gpr0 = 0;
    int have_ctx = 0;
    /* Compute the offset BEFORE the bound: thread+0x1A0 can wrap u32 for a
     * corrupt gpr[3] near 0xFFFFFFFF and would pass the range test. */
    if (s_ram && thread >= 0x80000000u && s_ram_size >= 0x1A0u &&
        thread - 0x80000000u <= s_ram_size - 0x1A0u) {
        const u32 off = thread - 0x80000000u;
        saved_pc = read_be32(s_ram + off + 0x198u);
        entry_gpr0 = read_be32(s_ram + off + 0x000u);
        have_ctx = 1;
    }
    if (have_ctx)
        fprintf(stderr,
                "[thr-watch] resume thread=0x%08X saved-pc=0x%08X entry=0x%08X\n",
                thread, saved_pc, entry_gpr0);
    else
        fprintf(stderr, "[thr-watch] resume thread=0x%08X (context unreadable)\n",
                thread);
}


/* Push one journal push's intersecting words into the ring (post-write:
 * newv is read back from RAM, oldv from the shadow). A store that starts
 * below the region and spans into it (heap memset) is clamped. */
static void mt_watch_push(u32 offset, u32 size) {
    const u32 lo = MT_WATCH_LO - 0x80000000u;
    const u32 hi = MT_WATCH_HI - 0x80000000u;
    u32 wlo = offset < lo ? lo : (offset & ~3u);
    u32 whi = offset + size > hi ? hi : offset + size;
    u32 w;
    if ((s_mt_ring_seq & 63u) == 0u)
        s_mt_ring_tb_ms = smc_ring_tb_ms();
    for (w = wlo; w < whi; w += 4u) {
        u32 guest = 0x80000000u + w;
        u32 newv = rel_rd32(guest);
        u32 idx = (w - lo) >> 2;
        u32 oldv = s_mt_shadow[idx];
        MtWatchRingEntry* e = &s_mt_ring[s_mt_ring_head];
        s_mt_shadow[idx] = newv;
        e->offset = w;
        e->size = size;
        e->oldv = oldv;
        e->newv = newv;
        e->seq = s_mt_ring_seq++;
        e->gen = s_heap_gen;
        e->tb = s_mt_ring_tb_ms;
        s_mt_ring_head++;
        if (s_mt_ring_head == MT_RING_MAX)
            s_mt_ring_head = 0; /* branch wrap, no div on the store hot path */
        if ((newv & 3u) == 0u && newv >= MT_RETAIL_LO && newv < MT_RETAIL_HI) {
            if (s_mt_loud_n < 64u)
                fprintf(stderr,
                        "[mt-watch] RETAIL-POINTER WRITE addr=0x%08X val=0x%08X "
                        "(live) old=0x%08X seq=%llu gen=%u sz=%u\n",
                        guest, newv, oldv, (unsigned long long)e->seq, e->gen,
                        size);
            s_mt_loud_n++;
        }
    }
}

// ---------------------------------------------------------------------------
// Guest RAM access (the replacement/journal run host-side with a captured
// RAM base; guest addresses are OS_BASE_CACHED | physical).
static inline u8* rel_ram_ptr(u32 guest) {
    u32 phys;
    if (guest < 0x80000000u)
        return NULL;
    phys = guest - 0x80000000u;
    if (phys >= s_ram_size)
        return NULL;
    return s_ram + phys;
}

/* phase3-lreloc-p5b root fix: the LRELOC lineages link module images at
 * descriptor-fixed EXRAM bases (0x91100000..0x9188xxxx). rel_ram_ptr above
 * only maps MEM1, so EVERY loader L-side operation on an EXRAM-linked
 * module — post-relocation data-section mirror (link step "Mirror fixed-up
 * DATA sections to L"), linked-BSS zeroing, L-verification reads in the
 * chunk dispatcher, refresh/lazy-mismatch probes — silently no-op'd or read
 * zeros. Cold boots then link id=1 with its L globals left NULL (observed:
 * "[rel_loader] link id=1 s3 ... [0x0 0x80393D0C] L[0x0 0x00000000]" while
 * the MEM1-linked control arm carries 0x80393D0C into L), and every later
 * consumer of that state wedges below the DI completion layer. Mirror the
 * chassis's own window translation ((addr & ~0x40000000) - GC_EXRAM_BASE,
 * cpu.h mem_direct_exram_ptr) so all existing rel_rd/rel_wr call sites see
 * the real linked image. No-ops back to legacy behavior when exram is not
 * allocated. */
static inline u8* rel_exram_ptr(u32 guest) {
    u32 ex_off;
    if (!s_exram || !s_exram_size)
        return NULL;
    if (guest < 0x80000000u)
        return NULL;
    ex_off = (guest & ~0x40000000u) - GC_EXRAM_BASE;
    if (ex_off >= s_exram_size)
        return NULL;
    return s_exram + ex_off;
}

/* MEM1 first, then the LRELOC EXRAM window (phase3-lreloc-p5b). */
static inline u8* rel_any_ptr(u32 guest) {
    u8* p = rel_ram_ptr(guest);
    return p ? p : rel_exram_ptr(guest);
}

/* Sized variants: rel_*_ptr only guarantees the FIRST byte is mapped, so a
 * multi-byte access starting near the end of MEM1/EXRAM would straddle the
 * host buffer edge. These require the whole [guest, guest+n) span to be
 * mapped. rel_span_ptr additionally reports how much of the span is actually
 * mapped so bulk ops (memcpy/memset) can clamp instead of overrun. */
static inline u8* rel_ram_ptr_n(u32 guest, u32 n) {
    u32 phys;
    if (guest < 0x80000000u)
        return NULL;
    phys = guest - 0x80000000u;
    if (phys >= s_ram_size || n > s_ram_size - phys)
        return NULL;
    return s_ram + phys;
}

static inline u8* rel_exram_ptr_n(u32 guest, u32 n) {
    u32 ex_off;
    if (!s_exram || !s_exram_size)
        return NULL;
    if (guest < 0x80000000u)
        return NULL;
    ex_off = (guest & ~0x40000000u) - GC_EXRAM_BASE;
    if (ex_off >= s_exram_size || n > s_exram_size - ex_off)
        return NULL;
    return s_exram + ex_off;
}

static inline u8* rel_any_ptr_n(u32 guest, u32 n) {
    u8* p = rel_ram_ptr_n(guest, n);
    return p ? p : rel_exram_ptr_n(guest, n);
}

static inline u8* rel_span_ptr(u32 guest, u32* size) {
    u32 phys;
    u32 ex_off;
    if (guest >= 0x80000000u) {
        phys = guest - 0x80000000u;
        if (phys < s_ram_size) {
            u32 room = s_ram_size - phys;
            if (*size > room)
                *size = room;
            return s_ram + phys;
        }
        if (s_exram && s_exram_size) {
            ex_off = (guest & ~0x40000000u) - GC_EXRAM_BASE;
            if (ex_off < s_exram_size) {
                u32 room = s_exram_size - ex_off;
                if (*size > room)
                    *size = room;
                return s_exram + ex_off;
            }
        }
    }
    *size = 0u;
    return NULL;
}

static inline u32 rel_rd32(u32 guest) {
    const u8* p = rel_any_ptr_n(guest, 4u);
    return p ? read_be32(p) : 0u;
}

static inline u16 rel_rd16(u32 guest) {
    const u8* p = rel_any_ptr_n(guest, 2u);
    return p ? read_be16(p) : 0u;
}

static inline u8 rel_rd8(u32 guest) {
    const u8* p = rel_any_ptr(guest);
    return p ? *p : 0u;
}

static inline void rel_wr32(u32 guest, u32 value) {
    u8* p = rel_any_ptr_n(guest, 4u);
    if (p)
        write_be32(p, value);
}

static inline void rel_wr16(u32 guest, u16 value) {
    u8* p = rel_any_ptr_n(guest, 2u);
    if (p)
        write_be16(p, value);
}

static inline void rel_wr8(u32 guest, u8 value) {
    u8* p = rel_any_ptr(guest);
    if (p)
        *p = value;
}

// ---------------------------------------------------------------------------
// Descriptor lookup (s_rel_modules is sorted ascending by module_id).
// ---------------------------------------------------------------------------

static const ModernGekkoRelModule* rel_descriptor_for(u32 id) {
    int lo = 0;
    int hi = (int)MODULE_REL_MODULE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u32 mid_id = s_rel_modules[mid].module_id;
        if (id < mid_id)
            hi = mid - 1;
        else if (id > mid_id)
            lo = mid + 1;
        else
            return &s_rel_modules[mid];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// B28/pulse12 — MODERNGEKKO_BAND_PROTECT (default OFF): allocator-level
// reservation of every module's descriptor-fixed L window.
//
// Forensics (pulse10 §2 / pulse11 §2) proved one class twice: the guest game
// heap arena spans the REL L windows, so JKR hands allocations out INSIDE a
// module's static/L image span. Whatever the guest parks there dies when the
// window's owner writes it (link data memcpy / bss memset — pulse10 id=273
// Obj_Lpalm SubCreateProcess spin), or worse when NOBODY links at all because
// an unrelated writer tramples the resting object (pulse11 id=266 cCcD_ShapeAttr
// CalcArea pc=0). B24's link-time queue-walk refusal guard covers only live
// f_pc requests AT LINK time; published-but-unguarded objects resting in any
// of the 415 windows stayed exposed to ALL writers.
//
// Fix (D1 "band carve"): walk the JKRHeap tree from sRootHeap, and for every
// JKRExpHeap split/remove free-list blocks that intersect any reserved band.
// The retail allocator then cannot double-book them BY CONSTRUCTION while all
// its own semantics (best-fit selection, alignment splits, coalescing,
// check()/dump bookkeeping via the mSize adjustment below) stay untouched.
//
// Rejected for this seat (see phase3-pulse12.md §1):
//   D2 do_alloc band-mask rejection with retry — must re-implement best-fit +
//      alignment-offset selection semantics to skip masked ranges mid-split;
//      divergence risk with fragmentation-driven retry loops.
//   D3 targeted L-band relocation (BankWave kWsBase precedent) — per-module
//      remap + consumer rewrite; closes proven instances, not the class.
//
// Layout pins (framework.map): __vt__10JKRExpHeap 0x8039CC60;
// JKRHeap mStart/mEnd/mSize @+0x30/34/38 (validated by the [heap] diag rows);
// JKRExpHeap mHeadFreeList/mTailFreeList/mHeadUsedList/mTailUsedList
// @+0x78/7C/80/84; child tree JSUPtrList head @+0x40, links are JSUPtrLink
// {obj@0, list@4, prev@8, next@c}; CMemBlock {magic@0 u16, flags@2, group@3,
// size@8, prev@0xC? no: prev@c holds mPrev} = {magic@0,flags@2,grp@3,
// size@4? — see struct below}. CMemBlock header size 0x10, content follows.

#define REL_BAND_VT_EXPHEAP 0x8039CC60u
#define REL_BAND_ROOT_HEAP 0x803F7730u /* sRootHeap__7JKRHeap */
#define REL_BAND_OFS_START 0x30u
#define REL_BAND_OFS_SIZE 0x38u
#define REL_BAND_OFS_FREEHEAD 0x78u
#define REL_BAND_OFS_FREETAIL 0x7Cu
#define REL_BAND_OFS_USEDHEAD 0x80u
#define REL_BAND_OFS_CHILDREN 0x40u
#define REL_BAND_HMAGIC 0x484Du /* 'HM' */

typedef struct {
    u32 id; /* basemap module id | 0x10000 when merged */
    u32 lo;
    u32 hi;
} RelBand;

#include "rel_bands.inc"

static int s_band_protect; /* MODERNGEKKO_BAND_PROTECT=1 */
static int s_p11_sweep; /* MODERNGEKKO_P11_SWEEP=1 (phase3-lreloc-p11) */
static u64 s_band_sweeps;
static u64 s_band_splices;      /* free-list node transformations total */
static u32 s_band_adj_pending;  /* bytes carved THIS heap THIS sweep */
static unsigned s_band_log_n;   /* rate cap for per-event rows */
static u64 s_band_reserved_total; /* carved-out bytes across all heaps */

/* Per-heap mSize bookkeeping state, keyed by guest heap pointer (see
 * band_adjust_msize). Sized for TWW's live heap population with graceful
 * degradation: overflow stops further mSize adjustments while carving stays
 * allocation-correct. */
#define REL_BAND_HEAP_ADJ_MAX 64
typedef struct {
    u32 heap;
    u32 orig_size;
    u32 adj;
} RelBandHeapAdj;
static RelBandHeapAdj s_band_heap_adj[REL_BAND_HEAP_ADJ_MAX];
static unsigned s_band_heap_adj_n;

/* Lowest-index band whose hi > lo (with sorted/disjoint bands, the first
 * possible intersection); returns -1 when that band misses [lo..hi). */
static int band_first_over(u32 lo, u32 hi) {
    int lo_i = 0;
    int hi_i = (int)REL_BAND_COUNT;
    while (lo_i < hi_i) {
        int mid = (lo_i + hi_i) >> 1;
        if (s_rel_bands[mid].hi > lo)
            hi_i = mid;
        else
            lo_i = mid + 1;
    }
    if (lo_i >= (int)REL_BAND_COUNT || s_rel_bands[lo_i].lo >= hi)
        return -1;
    return lo_i;
}

/* CMemBlock field probes/writes on guest RAM. Header layout (JKRExpHeap.h):
 * magic u16@0, flags u8@2, groupId u8@3, size u32@4, mPrev@8, mNext@c. */
static inline u32 blk_size(u32 b) { return rel_rd32(b + 4u); }
static inline u32 blk_prev(u32 b) { return rel_rd32(b + 8u); }
static inline u32 blk_next(u32 b) { return rel_rd32(b + 0xCu); }
static void blk_initiate(u32 b, u32 prev, u32 next, u32 size) {
    rel_wr16(b, (u16)REL_BAND_HMAGIC);
    rel_wr8(b + 2u, 0u);
    rel_wr8(b + 3u, 0u);
    rel_wr32(b + 4u, size);
    rel_wr32(b + 8u, prev);
    rel_wr32(b + 0xCu, next);
}

/* Sanitized exp-heap predicate: vtable match + sane span in guest RAM. */
static int band_is_exp_heap(u32 h) {
    u32 start;
    u32 end;
    if (rel_rd32(h) != REL_BAND_VT_EXPHEAP)
        return 0;
    start = rel_rd32(h + REL_BAND_OFS_START);
    end = rel_rd32(h + REL_BAND_OFS_START + 4u);
    /* The L windows this carve reserves live in EXRAM (0x91100000+), so the
     * arena must be allowed to map in either window; a MEM1-only predicate
     * (rel_ram_ptr + end < 0x82000000) rejected every heap that could
     * actually intersect a band and made the feature a silent no-op.
     * Endpoint mapping bounds the span to real memory on both windows. */
    return start != 0u && start < end &&
           rel_any_ptr(start) != NULL && rel_any_ptr(end - 1u) != NULL;
}

static void band_log(const char* fmt, ...) {
    va_list ap;
    if (s_band_log_n >= 24 && (s_band_log_n % 256u) != 0)
        return;
    s_band_log_n++;
    fprintf(stderr, "[band] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Carve ONE free-list node out around EVERY reserved band it intersects.
 * A fresh heap's single initial free node can span ALL bands, so keep pieces
 * are collected in a bounded pass: at most REL_BAND_PIECES_MAX pieces are
 * emitted per node and any beyond that (plus every < header-room sliver)
 * donate their bytes to the reservation instead of becoming unusable
 * fragments. Piece headers are written ONLY into kept (non-band) bytes.
 * Returns the replacement node to continue the walk from (or NULL = node
 * fully consumed / untouched). */
#define REL_BAND_PIECES_MAX 384u /* > band count: worst case = one node over all */
static u32 band_carve_node(u32 h, u32 node) {
    u32 nsize = blk_size(node);
    u32 content_lo = node + 0x10u;
    u32 content_hi = content_lo + nsize;
    u32 npieces = 0;
    int bi;
    u32 pos;
    /* Keep-pieces are NODE spans [start,end): the 0x10 CMemBlock header sits
     * AT start, content follows. pieces[k][0] must therefore be a keep byte
     * itself — emitting the header at (content start - 0x10) put it inside
     * the preceding band whenever a piece followed a band end, corrupting
     * the very reservation the carve creates (and handing the allocator a
     * node whose header the module image later overwrites). Node-span
     * accounting is identical: span = header + content. */
    static u32 pieces[REL_BAND_PIECES_MAX][2]; /* node [start,end) spans */

    /* From `node` (not content_lo): also catches bands covering only the
     * node's existing header, so that header slot is never reused. */
    bi = band_first_over(node, content_hi);
    if (bi < 0)
        return 0; /* this node does not touch any band */
    pos = node;
    for (; bi < (int)REL_BAND_COUNT && pos < content_hi; bi++) {
        u32 blo = s_rel_bands[bi].lo;
        u32 bhi = s_rel_bands[bi].hi;
        if (bhi <= pos)
            continue;
        if (blo > pos && blo - pos >= 0x14u) {
            /* left keep-node [pos..blo): header+content both inside the gap */
            if (npieces < REL_BAND_PIECES_MAX) {
                pieces[npieces][0] = pos;
                pieces[npieces][1] = blo;
            }
            npieces++;
            pos = blo;
        }
        if (bhi > pos)
            pos = bhi;
    }
    if (pos < content_hi && content_hi - pos >= 0x14u) {
        if (npieces < REL_BAND_PIECES_MAX) {
            pieces[npieces][0] = pos;
            pieces[npieces][1] = content_hi;
        }
        npieces++;
    }
    if (npieces > REL_BAND_PIECES_MAX) {
        band_log("WARN: node %08X spanned >%u bands; excess bytes donated to "
                 "reservation",
                 node, (unsigned)REL_BAND_PIECES_MAX);
        npieces = REL_BAND_PIECES_MAX;
    }

    {
        u32 prev = blk_prev(node);
        u32 next = blk_next(node);
        u32 accounted_lost = nsize + 0x10u; /* original check() contribution */
        u32 new_chain_head = 0u;
        u32 new_chain_tail = 0u;
        u32 k;
        for (k = 0; k < npieces; k++) {
            u32 pstart = pieces[k][0];                    /* header address */
            u32 psize = pieces[k][1] - pieces[k][0] - 0x10u; /* content size */
            accounted_lost -= psize + 0x10u;
            blk_initiate(pstart, 0u, 0u, psize);
            if (new_chain_tail == 0u) {
                new_chain_head = new_chain_tail = pstart;
            } else {
                rel_wr32(new_chain_tail + 0xCu, pstart);
                rel_wr32(pstart + 8u, new_chain_tail);
                new_chain_tail = pstart;
            }
        }
        s_band_splices++;
        s_band_adj_pending += accounted_lost;
        band_log("carve: heap %08X node %08X [%08X..%08X) -> %u piece(s), lost %u",
                 h, node, content_lo, content_hi,
                 (unsigned)npieces, accounted_lost);
        if (npieces == 0) {
            /* Node fully consumed: splice it out of the list. */
            if (prev)
                rel_wr32(prev + 0xCu, next);
            else
                rel_wr32(h + REL_BAND_OFS_FREEHEAD, next);
            if (next)
                rel_wr32(next + 8u, prev);
            else
                rel_wr32(h + REL_BAND_OFS_FREETAIL, prev);
            return next;
        }
        rel_wr32(new_chain_head + 8u, prev);
        rel_wr32(new_chain_tail + 0xCu, next);
        if (prev)
            rel_wr32(prev + 0xCu, new_chain_head);
        else
            rel_wr32(h + REL_BAND_OFS_FREEHEAD, new_chain_head);
        if (next)
            rel_wr32(next + 8u, new_chain_tail);
        else
            rel_wr32(h + REL_BAND_OFS_FREETAIL, new_chain_tail);
        return new_chain_head;
    }
}

/* Fold s_band_adj_pending into this heap's mSize bookkeeping (once per byte):
 * check() sums free-list sizes + used blocks against mSize, so the bytes we
 * removed must come off mSize. The side table stores each heap's ORIGINAL
 * mSize and the running applied adjustment so repeated sweeps never
 * double-decrement. */
static void band_adjust_msize(u32 h) {
    RelBandHeapAdj* e = NULL;
    unsigned i;
    if (s_band_adj_pending == 0u)
        return;
    for (i = 0; i < s_band_heap_adj_n; i++)
        if (s_band_heap_adj[i].heap == h) {
            e = &s_band_heap_adj[i];
            break;
        }
    if (!e) {
        if (s_band_heap_adj_n == REL_BAND_HEAP_ADJ_MAX)
            return; /* table full: stay alloc-correct, skip bookkeeping */
        e = &s_band_heap_adj[s_band_heap_adj_n++];
        e->heap = h;
        e->orig_size = rel_rd32(h + REL_BAND_OFS_SIZE);
        e->adj = 0;
    }
    e->adj += s_band_adj_pending;
    rel_wr32(h + REL_BAND_OFS_SIZE, e->orig_size - e->adj);
    s_band_reserved_total += s_band_adj_pending;
    s_band_adj_pending = 0u;
}

static void band_carve_tree(u32 h, int depth) {
    u32 link;
    if (depth > 8 || !band_is_exp_heap(h))
        return;
    band_log("tree: heap %08X span [%08X..%08X) size=%u d=%d",
             h, rel_rd32(h + REL_BAND_OFS_START),
             rel_rd32(h + REL_BAND_OFS_START + 4u),
             rel_rd32(h + REL_BAND_OFS_SIZE), depth);
    {
        /* Walk + carve the free list defensively: bounded iterations, and
         * stop trusting the chain if a node leaves the heap span. */
        u32 cur = rel_rd32(h + REL_BAND_OFS_FREEHEAD);
        u32 end = rel_rd32(h + REL_BAND_OFS_START + 4u);
        unsigned guard = 0;
        s_band_adj_pending = 0u;
        while (cur != 0u && guard++ < 200000u) {
            u32 nxt_probe = cur + 0x10u + blk_size(cur);
            u32 r;
            if (nxt_probe > end || cur < rel_rd32(h + REL_BAND_OFS_START)) {
                band_log("WARN: heap %08X free chain malformed at %08X", h, cur);
                break;
            }
            r = band_carve_node(h, cur);
            cur = (r != 0u) ? r : blk_next(cur);
            if (cur == 0u)
                break;
        }
        band_adjust_msize(h);
        /* Diagnostic only: report USED blocks already parked inside bands.
         * Carve cannot evict live objects; rows here quantify residual
         * exposure (expected ~0 on cold boots armed before first alloc). */
        {
            u32 ublk = rel_rd32(h + REL_BAND_OFS_USEDHEAD);
            unsigned uguard = 0;
            while (ublk != 0u && uguard++ < 200000u) {
                u32 cs = ublk + 0x10u;
                u32 ce = cs + blk_size(ublk);
                int bi2 = band_first_over(cs, ce);
                if (bi2 >= 0 && s_rel_bands[bi2].lo < ce) {
                    band_log("WARN: used-block overlap heap=%08X [%08X..%08X) band-id=%u "
                             "[%08X..%08X)",
                             h, cs, ce, s_rel_bands[bi2].id & 0xFFFFu,
                             s_rel_bands[bi2].lo, s_rel_bands[bi2].hi);
                    break; /* one row per heap per sweep is plenty */
                }
                ublk = blk_next(ublk);
            }
        }
    }
    /* Children (JSUPtrList head @+0x40 of JSUTree<JKRHeap>). */
    link = rel_rd32(h + REL_BAND_OFS_CHILDREN);
    {
        unsigned cguard = 0;
        while (link != 0u && cguard++ < 256u) {
            u32 child = rel_rd32(link);
            if (child != 0u)
                band_carve_tree(child, depth + 1);
            link = rel_rd32(link + 0xCu); /* JSUPtrLink.mNext */
        }
    }
}

/* Public trigger: full reserve sweep over the reachable heap tree. Cheap when
 * everything is already carved (pure list walks). `why` tags the log line. */
static void band_protect_sweep(const char* why) {
    u32 root;
    u64 splices_before;
    if (!s_band_protect || !s_ram)
        return;
    root = rel_rd32(REL_BAND_ROOT_HEAP);
    if (!band_is_exp_heap(root))
        return; /* heap world not up yet: try again on the next trigger */
    s_band_sweeps++;
    splices_before = s_band_splices;
    band_carve_tree(root, 0);
    if (s_band_splices != splices_before || s_band_sweeps == 1)
        band_log("sweep #%llu (%s): splices=%llu reserved_total=%u%s",
                 (unsigned long long)s_band_sweeps, why,
                 (unsigned long long)s_band_splices,
                 (unsigned)s_band_reserved_total,
                 s_band_splices != splices_before ? "" : " (no changes)");
}

// ---------------------------------------------------------------------------
// Write journal: mirror active-module R data writes to L; trap code writes.
// ---------------------------------------------------------------------------

static void rel_loader_write_journal(u32 offset, u32 size, void* user) {
    u32 guest;
    u32 i;
    (void)user;

    if (!s_ram)
        return;


    /* p14 loadseam (Layer A): log every journaled store intersecting
     * g_mDoMemCd_control (0x803B39A0, 0x1698 span). The journal push
     * carries no pc (GXRuntime mem_write hook contract), so rows are
     * correlated by sequence order against the [loadseam] P1-P4 decision
     * rows and the boot timeline. The cmd=52 mount/validity burst and any
     * later mDoMemCd_Save (write!) vs absence-of-read is directly visible. */
    if (s_loadseam_trace && offset < s_ram_size) {
        const u32 g = 0x80000000u + offset;
        if (g + size > LOADSEAM_MC_CTRL_LO && g < LOADSEAM_MC_CTRL_HI) {
            u32 v = 0u;
            switch (size) {
                case 1u: v = rel_rd8(g); break;
                case 2u: v = rel_rd16(g); break;
                default: v = rel_rd32(g); break;
            }
            fprintf(stderr,
                    "[loadseam] mc-write addr=0x%08X size=%u val=0x%X\n",
                    g, size, v);
        }
    }
    /* DOL-text write watch (dol-watch-spec): journal only stores landing in
     * SMC-guarded DOL chunk ranges — the corruption-class writes. Filtered
     * rate ~0-1/run so the ring holds the whole session history.
     * Fast path: one size test skips the bsearch + push entirely when
     * STATICRECOMP_SMC_RING_SIZE=0 (default ring: path below unchanged). */
    if (s_smc_ring_size != 0u) {
        if (offset < s_ram_size && smc_guarded_dol(0x80000000u + offset)) {
            SmcWriteRingEntry* e = &s_smc_ring[s_smc_ring_head];
            u32 v = 0u;
            u32 g = 0x80000000u + offset;
            switch (size) {
                case 1u: v = rel_rd8(g); break;
                case 2u: v = rel_rd16(g); break;
                default: v = rel_rd32(g); break;
            }
            e->offset = offset;
            e->size = size;
            e->value = v;
            e->seq = s_smc_ring_seq++;
            if ((e->seq & 63u) == 0u)
                s_smc_ring_tb_ms = smc_ring_tb_ms(); /* sample: 1/64 clock calls */
            e->tb = s_smc_ring_tb_ms;
            s_smc_ring_head++;
            if (s_smc_ring_head == s_smc_ring_size)
                s_smc_ring_head = 0; /* branch wrap, no div on the store hot path */
        }
    }
    /* p12 mt-watch: capture method-table-region writers (default OFF). */
    if (s_mt_watch && offset < s_ram_size &&
        offset + size > MT_WATCH_LO - 0x80000000u &&
        offset < MT_WATCH_HI - 0x80000000u)
        mt_watch_push(offset, size);

    /* Stream section relocation (JAInter aaf case 5): transInitDataFile
     * copies the stream table into the JAI solid heap, and the game's later
     * heap churn (JASDram freeTail/reuse) clobbers the copy before the
     * title scene reads StreamMgr::streamList. The streamList global
     * (0x803F7650) is stored during the aaf parse; when that store lands,
     * copy the section (0xe20 bytes at the stored pointer) into
     * loader-reserved stable guest RAM in the free DOL-bss/root-heap gap
     * (0x803FCFA0..0x8040CFE0; 0x8040A000 is loader-owned) and repoint the
     * global there. R7 RE-ARM (phase2-r7-rearm-spec.md §2.1): the audio
     * init can re-run (soft reset / demo-movie re-init — PROVEN
     * 2026-08-16: stream __start call=3 abs803F7650=0x806F6FA0 fresh
     * L-band pointer vs carve 0x8040A000 at calls 0-2), re-storing a FRESH
     * L-band pointer; drop the one-shot flag, verify-on-every-store, and
     * re-copy whenever the stored value is a fresh linked-band pointer.
     * Stores of the stable slot itself (0x8040A000) and any non-L-band
     * value are no-ops. The aaf section layout: entries with the filename
     * at entry+0x10 (inline char[]). */
    if (offset == 0x3F7650u && size == 4u) {
        u32 sec = rel_rd32(0x803F7650u);
        if (sec >= 0x80500000u && sec < 0x80C85F60u) {
            /* Fresh linked-band copy (re-parse) — relocate into the fixed
             * stable slot; re-inits overwrite in place (band never grows). */
            static const u32 kStable = 0x8040A000u;
            u32 j;
            for (j = 0; j < 0xe20u && sec - 0x80000000u + j < s_ram_size; j++)
                s_ram[kStable - 0x80000000u + j] = s_ram[sec - 0x80000000u + j];
            rel_wr32(0x803F7650u, kStable);
            s_carve_rearm_seq++;
            if (s_debug)
                fprintf(stderr,
                        "[rel_loader] streamList relocated 0x%08X -> 0x%08X (0xe20) "
                        "[re-arm #%u]\n",
                        sec, kStable, (unsigned)s_carve_rearm_seq);
        }
    }
    /* BankWave wave-control relocation (phase3-pulse8): JAInter::BankWave::init
     * allocates initOnCodeWs / wsGroupNumber / wsLoadStatus from the JASDram
     * solid heap inside the linked band (boot-observed 0x806F6C60 /
     * 0x806F8060). Late-linked REL modules carry baked L images that overlap
     * those live arrays (id=115: L=0x806EE7E0 file_size=0xC074 covers both)
     * and the link-time copy/relocation stomps them mid-boot (~field 900,
     * first title-era REL storm). Afterwards loadSceneWave's type guards
     * read garbage (dynamic waves 21/25 of sea's set 10 look like stay waves
     * and are silently skipped) and check1stDynamicWave polls garbage
     * forever -> d_s_play create phase_3 spins at the storybook->sea
     * changeGameScene on every cold interactive boot. Restore boots survive
     * only because the savestate snapshots pre-teardown heap state.
     * Fix: on the wsLoadStatus-pointer store (the LAST of init()'s three
     * static stores, issued BEFORE registWaveBankWS reads any entry; fires
     * again on audio re-init = R7-style re-arm), copy all three arrays plus
     * the per-entry name strings into loader-reserved stable RAM between
     * the SE carve and the streamList slot, and repoint the statics. Guest
     * code reaches these arrays ONLY through the statics (getWaveLoadStatus
     * / setWsLoadStatus / init), so the move is transparent; the abandoned
     * L-band originals are never read again. */
    if (offset == 0x3F758Cu && size == 4u) {
        static const u32 kWsBase = 0x80408000u;
        u32 iocws = rel_rd32(0x803F7584u);
        u32 wsgrp = rel_rd32(0x803F7588u);
        u32 wslp = rel_rd32(0x803F758Cu);
        u32 wsmax2 = rel_rd32(0x803F7590u);
        /* wsmax2 is guest RAM: the carve must fit in the 0x2000 bytes between
         * kWsBase and the streamList slot at 0x8040A000 or it tramples that
         * loader-owned copy. Entries+names use (wsmax2*44 + 31) & ~31, then
         * align32(wsmax2*4) for wsGroupNumber and wsmax2*4 for wsLoadStatus.
         * wsmax2 < 256 keeps the multiply bounded; the footprint check below
         * is the real gate (wsmax2 >= ~158 would overflow the window). */
        u32 ws_need = ((wsmax2 * 44u + 31u) & ~31u) +
                      ((wsmax2 * 4u + 31u) & ~31u) + wsmax2 * 4u;
        if (!(wslp >= kWsBase && wslp < kWsBase + 0x2000u) &&
            wsmax2 > 0u && wsmax2 < 256u && ws_need <= 0x2000u &&
            wsgrp >= 0x80000000u && wsgrp < 0x81800000u &&
            iocws >= 0x80000000u && iocws < 0x81800000u &&
            wslp >= 0x80000000u && wslp < 0x81800000u) {
            u32 cur = kWsBase;
            u32 j;
            /* initOnCodeWs entries (12 bytes each) + per-entry name strings. */
            for (j = 0; j < wsmax2 * 12u && iocws - 0x80000000u + j < s_ram_size &&
                        cur - 0x80000000u + j < s_ram_size; j++)
                s_ram[cur - 0x80000000u + j] = s_ram[iocws - 0x80000000u + j];
            for (j = 0; j < wsmax2; j++) {
                u32 np = rel_rd32(cur + j * 12u);
                if (np >= 0x80000000u && np < 0x81800000u) {
                    u32 dst = cur + wsmax2 * 12u + j * 32u;
                    u32 k;
                    for (k = 0; k < 31u && np - 0x80000000u + k < s_ram_size &&
                                dst - 0x80000000u + k < s_ram_size; k++) {
                        s_ram[dst - 0x80000000u + k] = s_ram[np - 0x80000000u + k];
                        if (s_ram[np - 0x80000000u + k] == 0u)
                            break;
                    }
                    if (k == 31u)
                        s_ram[dst - 0x80000000u + 31u] = 0u;
                    rel_wr32(cur + j * 12u, dst);
                }
            }
            rel_wr32(0x803F7584u, cur);
            cur += (wsmax2 * 12u + wsmax2 * 32u + 31u) & ~31u;
            for (j = 0; j < wsmax2 * 4u && wsgrp - 0x80000000u + j < s_ram_size &&
                        cur - 0x80000000u + j < s_ram_size; j++)
                s_ram[cur - 0x80000000u + j] = s_ram[wsgrp - 0x80000000u + j];
            rel_wr32(0x803F7588u, cur);
            cur += (wsmax2 * 4u + 31u) & ~31u;
            for (j = 0; j < wsmax2 * 4u && wslp - 0x80000000u + j < s_ram_size &&
                        cur - 0x80000000u + j < s_ram_size; j++)
                s_ram[cur - 0x80000000u + j] = s_ram[wslp - 0x80000000u + j];
            rel_wr32(0x803F758Cu, cur);
            fprintf(stderr,
                    "[rel_loader] bankwave relocated iocws 0x%08X grp 0x%08X st "
                    "0x%08X -> base 0x%08X (n=%u)\n",
                    iocws, wsgrp, wslp, kWsBase, wsmax2);
        }
    }

    /* SE-table relocation (SoundTable + SeMgr): the aaf parse copies the
     * SoundTable section (case 1, 0x94c0 at mAddress) and the sound-scene
     * table (case 6, 0x40 at SeMgr::categoryInfoTable) into the JAI solid
     * heap inside the linked band (~0x806F-0x8070). The game's later heap
     * churn (JASDram freeTail/reuse) clobbers those copies, so the per-frame
     * SE callback (checkNextFrameSe 0x80293530 / checkPlayingSe 0x80293C94
     * / storeSeBuffer 0x80294A10) walks NULL/0xFF category rows (zero-access
     * at 0x8029361c/0x80293a90/0x8029419c/0x80294ae0). SeMgr::startSeSequence
     * stores seHandle (0x803F75FC) LAST in the init chain, after every table
     * is populated; on that store, copy the pointed-to sections into
     * loader-reserved stable guest RAM BELOW the streamList copy (0x8040A000;
     * the band 0x803FCFA0..0x8040A000 is free) and repoint the globals.
     * Inner pointers (mPointerCategory[] -> mAddress copy, categoryInfoTable
     * scene ptrs -> case-6 copy) are rebased by their delta. R7 RE-ARM
     * (phase2-r7-rearm-spec.md §2.2): drop the one-shot flag — the audio
     * init re-runs on soft reset / demo-movie re-init (PROVEN 2026-08-16),
     * re-storing fresh L-band pointers; verify-on-every-store, re-copy only
     * when mAddress is a fresh linked-band pointer (in
     * 0x80500000..0x80C85F60). Re-inits overwrite in place; the band never
     * grows. */
    /* Trigger: every non-zero seHandle store. BSS-clear writes 0 early
     * (before any table exists); startSeSequence's `seHandle = NULL` is also
     * 0. The handle value (startSoundActor's *seHandle = sound) is the last
     * init step, after every SoundTable/SeMgr table is populated. */
    if (offset == 0x3F75FCu && size == 4u) {
        if (rel_rd32(0x803F75FCu) == 0u)
            goto se_table_trigger_done;
        if (rel_rd32(0x803F7640u) == 0u)
            goto se_table_trigger_done; /* SoundTable not yet parsed */
        /* R7 re-arm gate: only re-copy when mAddress is a fresh linked-band
         * allocation (re-parse). A store of the stable slot itself or any
         * non-L-band value is a no-op (no redundant re-copy). */
        {
            u32 rearm_addr = rel_rd32(0x803F7640u);
            if (rearm_addr < 0x80500000u || rearm_addr >= 0x80C85F60u)
                goto se_table_trigger_done;
        }
        static const u32 kSeBase = 0x803FD000u; /* below streamList 0x8040A000 */
        u32 cur = kSeBase;
        u32 j;
        u32 cat_max;
        u32 track_max;

        /* SoundTable::mCategotyMax (u8 at 0x803F7631) governs array sizes
         * for seRegist/sePlaySound/seRegistBuffer/seEntryCancel/seCategoryVolume. */
        cat_max = rel_rd8(0x803F7631u);
        if (cat_max == 0 || cat_max > 0x20u)
            cat_max = 8u;

        /* mAddress: 0x94c0-byte aaf case-1 section copy. */
        {
            u32 addr = rel_rd32(0x803F7640u);
            if (addr >= 0x80000000u && addr < 0x81800000u) {
                for (j = 0; j < 0x94c0u && addr - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[addr - 0x80000000u + j];
                rel_wr32(0x803F7640u, cur);
                if (s_debug)
                    fprintf(stderr, "[setbl] SoundTable.mAddress 0x%08X -> 0x%08X (0x94c0)\n",
                            addr, cur);
                cur += 0x94c0u;

                /* mSoundMax: u16[18] separate JAI-heap allocation. */
                {
                    u32 sm = rel_rd32(0x803F7634u);
                    if (sm >= 0x80000000u && sm < 0x81800000u) {
                        for (j = 0; j < 0x24u && sm - 0x80000000u + j < s_ram_size &&
                                    cur - 0x80000000u + j < s_ram_size; j++)
                            s_ram[cur - 0x80000000u + j] = s_ram[sm - 0x80000000u + j];
                        rel_wr32(0x803F7634u, cur);
                        cur += 0x24u;
                    }
                }

                /* mPointerCategory: SoundInfo*[18]; entries point INTO the
                 * mAddress copy -> rebase by the mAddress delta. */
                {
                    u32 mpc = rel_rd32(0x803F763Cu);
                    if (mpc >= 0x80000000u && mpc < 0x81800000u) {
                        for (j = 0; j < 0x48u && mpc - 0x80000000u + j < s_ram_size &&
                                    cur - 0x80000000u + j < s_ram_size; j++)
                            s_ram[cur - 0x80000000u + j] = s_ram[mpc - 0x80000000u + j];
                        for (j = 0; j < 18u; j++) {
                            u32 e = rel_rd32(mpc + j * 4u);
                            if (e >= addr && e < addr + 0x94c0u)
                                rel_wr32(cur + j * 4u, kSeBase + (e - addr));
                        }
                        rel_wr32(0x803F763Cu, cur);
                        cur += 0x48u;
                    }
                }
            }
        }

        /* categoryInfoTable: aaf case-6 section copy (0x40 bytes: sceneMax
         * u32 + sceneMax pointers + per-scene rows; pointers rebased into the
         * copy). Global points 4 bytes into the section (field_0x1c = r28+1). */
        {
            u32 ctr = rel_rd32(0x803F75E4u);
            if (ctr >= 0x80000000u && ctr < 0x81800000u) {
                u32 base = ctr - 4u; /* r28: the transInitDataFile copy base */
                u32 scene_max = rel_rd32(base);
                if (scene_max > 0x20u)
                    scene_max = 0x20u;
                for (j = 0; j < 0x40u && base - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[base - 0x80000000u + j];
                for (j = 0; j < scene_max; j++) {
                    u32 e = rel_rd32(base + 4u + j * 4u);
                    if (e >= base && e < base + 0x40u)
                        rel_wr32(cur + 4u + j * 4u, cur + (e - base));
                }
                rel_wr32(0x803F75E4u, cur + 4u);
                if (s_debug)
                    fprintf(stderr, "[setbl] categoryInfoTable 0x%08X -> 0x%08X (0x40, sceneMax=%u)\n",
                            ctr, cur + 4u, scene_max);
                cur += 0x40u;
            }
        }

        /* seRegist: LinkSound[catMax], stride 0xc (storeSeBuffer mulli r30,r0,0xc). */
        {
            u32 sr = rel_rd32(0x803F75F4u);
            u32 sz = cat_max * 0xcu;
            if (sr >= 0x80000000u && sr < 0x81800000u && sz < 0x1000u) {
                for (j = 0; j < sz && sr - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[sr - 0x80000000u + j];
                rel_wr32(0x803F75F4u, cur);
                cur += (sz + 0x1fu) & ~0x1fu;
            }
        }

        /* seRegistBuffer: JAISound*[catMax]. */
        {
            u32 rb = rel_rd32(0x803F75F8u);
            u32 sz = cat_max * 4u;
            if (rb >= 0x80000000u && rb < 0x81800000u && sz < 0x1000u) {
                for (j = 0; j < sz && rb - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[rb - 0x80000000u + j];
                rel_wr32(0x803F75F8u, cur);
                cur += (sz + 0x1fu) & ~0x1fu;
            }
        }

        /* sePlaySound: u32*[catMax] array + catMax rows of u32[0x10]. */
        {
            u32 ps = rel_rd32(0x803F75E8u);
            u32 sz = cat_max * 4u;
            if (ps >= 0x80000000u && ps < 0x81800000u && sz < 0x1000u) {
                u32 arr = cur;
                for (j = 0; j < sz && ps - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[ps - 0x80000000u + j];
                cur += sz;
                /* relocate each row (u32[0x10]) and repoint the array entry */
                for (j = 0; j < cat_max; j++) {
                    u32 row = rel_rd32(ps + j * 4u);
                    if (row >= 0x80000000u && row < 0x81800000u) {
                        u32 k;
                        for (k = 0; k < 0x40u && row - 0x80000000u + k < s_ram_size &&
                                    cur - 0x80000000u + k < s_ram_size; k++)
                            s_ram[cur - 0x80000000u + k] = s_ram[row - 0x80000000u + k];
                        rel_wr32(arr + j * 4u, cur);
                        cur += 0x40u;
                    }
                }
                rel_wr32(0x803F75E8u, arr);
            }
        }

        /* seTrackUpdate: seTrackUpdate_s[seTrackMax], stride 0x18. */
        track_max = rel_rd32(0x803F6460u);
        if (track_max == 0 || track_max > 0x40u)
            track_max = 0x20u;
        {
            u32 tu = rel_rd32(0x803F75E0u);
            u32 sz = track_max * 0x18u;
            if (tu >= 0x80000000u && tu < 0x81800000u && sz < 0x1000u) {
                for (j = 0; j < sz && tu - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[tu - 0x80000000u + j];
                rel_wr32(0x803F75E0u, cur);
                cur += (sz + 0x1fu) & ~0x1fu;
            }
        }

        /* seEntryCancel: u8[catMax]. seCategoryVolume: f32[catMax]. */
        {
            u32 ec = rel_rd32(0x803F760Cu);
            if (ec >= 0x80000000u && ec < 0x81800000u) {
                for (j = 0; j < cat_max && ec - 0x80000000u + j < s_ram_size &&
                            cur - 0x80000000u + j < s_ram_size; j++)
                    s_ram[cur - 0x80000000u + j] = s_ram[ec - 0x80000000u + j];
                rel_wr32(0x803F760Cu, cur);
                cur += (cat_max + 0x1fu) & ~0x1fu;
            }
            {
                u32 cv = rel_rd32(0x803F7608u);
                if (cv >= 0x80000000u && cv < 0x81800000u) {
                    u32 k;
                    for (k = 0; k < cat_max * 4u && cv - 0x80000000u + k < s_ram_size &&
                                cur - 0x80000000u + k < s_ram_size; k++)
                        s_ram[cur - 0x80000000u + k] = s_ram[cv - 0x80000000u + k];
                    rel_wr32(0x803F7608u, cur);
                    cur += (cat_max * 4u + 0x1fu) & ~0x1fu;
                }
            }
        }

        s_carve_rearm_seq++;
        fprintf(stderr, "[setbl] SE tables relocated (base 0x%08X, end 0x%08X, catMax=%u, trackMax=%u, seHandle=0x%08X) [re-arm #%u]\n",
                kSeBase, cur, cat_max, track_max, rel_rd32(0x803F75FCu),
                (unsigned)s_carve_rearm_seq);
    }
se_table_trigger_done:

    /* Journal-call probe (MODERNGEKKO_LOADER_DEBUG): how often the callback
     * actually runs, and with how many active modules. Tells an inert journal
     * apart from a filtered one. */
    if (s_debug) {
        static unsigned long long s_jcall_n;
        s_jcall_n++;
        if ((s_jcall_n % 100000u) == 0u)
            fprintf(stderr, "[jrnl] calls=%llu active_modules=%u\n", s_jcall_n, (unsigned)s_active_count);
    }
    if (s_active_count == 0)
        return;
    guest = 0x80000000u + offset;
    if (guest < 0x80000000u)
        return;

    /* TEMP trace: any write into the 0x8066-0x8070 window (module 60/other
     * low L bases) */
    if (guest >= 0x80679080u && guest < 0x80681220u) {
        static unsigned ntr;
        if (ntr < 20)
            fprintf(stderr, "[jrnl] M60-L write guest=0x%08X sz=%u (in_chunk=%d)\n",
                    guest, size, s_in_rel_chunk);
        ntr++;
    }

    // Detect game-heap clobbering of an active module's LINKED (L) data
    // region: the JKR arena reuses the E1 batch bases after link and fills
    // them with 0xFF, which destroys module data the compiled chunks read
    // (baked L constants: m_arc_name, bdl[] tables, ...). Mark the module
    // dirty; the REL chunk dispatcher refreshes L from the durable R copy
    // before the next chunk execution (rel_loader_refresh_l_image).
    if (!rel_loader_journal_may_touch_module(guest, size))
        return;
    for (i = 0; i < s_active_count; i++) {
        RelActiveModule* m = &s_active[i];
        u32 end = m->r + m->file_size;
        u32 si;
        int in_l_data = 0;

        if (guest >= m->r && guest < end) {
            // Classify via the R copy's fixed-up section table. Section 0 is
            // the header (never patched, offset stays 0) and is skipped.
            for (si = 1; si < m->num_sections; si++) {
                u32 entry = m->r + m->sio + si * 8u;
                u32 off = rel_rd32(entry) & ~1u;
                u32 sec_size = rel_rd32(entry + 4u);
                if (sec_size == 0u)
                    continue;
                if (off < m->r || off >= end)
                    continue; /* bss */
                if (guest >= off && guest < off + sec_size) {
                    if (rel_rd32(entry) & 1u) {
                        fprintf(stderr,
                                "[rel_loader] TRAP: guest write 0x%08X sz=%u into active REL "
                                "code range R=0x%08X L=0x%08X section=%u off=0x%08X; write "
                                "landed in RAM, SMC guard is the backstop\n",
                                guest, size, m->r, m->l, si, off);
                        return;
                    }
                    if (m->l && off >= m->r) {
                        /* size is the journal write size (1/2/4/8); use the
                         * sized resolution so a write straddling a window
                         * edge can't run past the host buffer. */
                        u32 span = size;
                        u8* src = rel_span_ptr(guest, &span);
                        u8* dst = rel_any_ptr_n(m->l + (off - m->r) + (guest - off), span);
                        if (src && dst)
                            memcpy(dst, src, span);
                    }
                    return;
                }
            }
            return;
        }

        // L-range write: the game heap reused our linked mirror. Mark dirty
        // only for DATA sections (mirror offset: L = R - r + l). Writes from
        // the module's OWN chunk execution are the module's legit data writes
        // (e.g. static init) — do NOT mark dirty (they are already in L and
        // a refresh would clobber them).
        if (s_in_rel_chunk)
            continue;
        if (m->l == 0u)
            continue;
        /* The heap fill is a large memset that may START below the L region
         * and span it; test overlap, not just the write start. */
        {
            u32 lend = m->l + m->file_size;
            u32 wend = guest + size;
            u32 si;
            if (wend <= m->l || guest >= lend)
                continue;
            for (si = 1; si < m->num_sections; si++) {
                u32 entry = m->r + m->sio + si * 8u;
                u32 roff = rel_rd32(entry) & ~1u;
                u32 rsize = rel_rd32(entry + 4u);
                if (rsize == 0u)
                    continue;
                if (roff < m->r || roff >= m->r + m->file_size)
                    continue; /* bss */
                if (rel_rd32(entry) & 1u)
                    continue; /* exec */
                {
                    u32 lstart = m->l + (roff - m->r);
                    u32 lend2 = lstart + rsize;
                    if (wend > lstart && guest < lend2) {
                        in_l_data = 1;
                        break;
                    }
                }
            }
        }
        if (in_l_data) {
            static unsigned ntr;
            if (ntr < 8)
                fprintf(stderr,
                        "[rel_loader] L-write id=%u guest=0x%08X sz=%u -> l_dirty\n",
                        m->id, guest, size);
            ntr++;
            m->l_dirty = 1u;
            ++g_rel_dispatch_gen;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Relocation pass (retail Relocate semantics; DolRecomp cursor model).
//   symbol_header: module whose section table provides symbol addresses
//   id_new:        import id to match (0 = DOL: addend-only)
//   patch_header:  module whose sections receive the patches
// Exec-section patches are SKIPPED (R copy code bytes stay raw for SMC).
// ---------------------------------------------------------------------------

static void rel_loader_relocate(u32 symbol_header, u32 id_new, u32 patch_header) {
    u32 imp_offset = rel_rd32(patch_header + 0x28u); /* fixed-up absolute */
    u32 imp_size = rel_rd32(patch_header + 0x2Cu);
    u32 imp;

    if (imp_offset < 0x80000000u || imp_size == 0u)
        return;

    for (imp = imp_offset; imp < imp_offset + imp_size; imp += 8u) {
        u32 imp_id;
        u32 rel;
        u32 cur_sec = 0;
        u32 cur_off = 0;
        u32 psio;

        /* A corrupt imp_size (or an imp_offset near the top of RAM) must not
         * let this loop wrap u32 / scan the entire address space — the table
         * lives inside the module image, i.e. inside mapped RAM. */
        if (!rel_any_ptr_n(imp, 8u))
            break;

        imp_id = rel_rd32(imp);
        if (imp_id != id_new)
            continue;

        rel = rel_rd32(imp + 4u); /* fixed-up absolute OSRel list */
        if (rel < 0x80000000u)
            continue;

        for (;;) {
            u16 roff;
            u8 type;
            u8 rsec;
            u32 addend;
            u32 p;
            u32 sym;
            u32 x;
            u32 patch_word;

            /* Unmapped rel would read back all zeros (type R_PPC_NONE) and
             * walk forward by 8 forever — off the end of RAM and around the
             * u32 wrap. Terminate the entry list at the mapped boundary. */
            if (!rel_any_ptr_n(rel, 8u))
                break;

            roff = rel_rd16(rel);
            type = rel_rd8(rel + 2u);
            rsec = rel_rd8(rel + 3u);
            addend = rel_rd32(rel + 4u);

            if (type == R_DOLPHIN_END)
                break;
            if (type == R_DOLPHIN_SECTION) {
                cur_sec = rsec;
                cur_off = 0;
                rel += 8u;
                continue;
            }
            cur_off += roff;
            if (type == R_DOLPHIN_NOP) {
                rel += 8u;
                continue;
            }

            psio = rel_rd32(patch_header + 0x10u); /* RAW sio */
            patch_word = rel_rd32(patch_header + psio + cur_sec * 8u);
            p = (patch_word & ~1u) + cur_off;

            if (id_new == 0u) {
                sym = 0u;
            } else {
                u32 ssio = rel_rd32(symbol_header + 0x10u); /* RAW sio */
                sym = rel_rd32(symbol_header + ssio + rsec * 8u) & ~1u;
            }
            x = sym + addend;

            // Exec sections: keep the raw file bytes (SMC hash ground truth).
            if (patch_word & 1u) {
                rel += 8u;
                continue;
            }

            switch (type) {
            case 0: /* R_PPC_NONE */
                break;
            case 1: /* R_PPC_ADDR32 */
                rel_wr32(p, x);
                break;
            case 2: /* R_PPC_ADDR24 */
                rel_wr32(p, (rel_rd32(p) & 0xFC000003u) | (x & 0x03FFFFFCu));
                break;
            case 3: /* R_PPC_ADDR16 */
            case 4: /* R_PPC_ADDR16_LO */
                rel_wr16(p + 2u, (u16)(x & 0xFFFFu));
                break;
            case 5: /* R_PPC_ADDR16_HI */
                rel_wr16(p + 2u, (u16)((x >> 16) & 0xFFFFu));
                break;
            case 6: /* R_PPC_ADDR16_HA */
                rel_wr16(p + 2u, (u16)(((x >> 16) + ((x & 0x8000u) ? 1u : 0u)) & 0xFFFFu));
                break;
            case 7: /* R_PPC_ADDR14 */
            case 8: /* R_PPC_ADDR14_BRTAKEN */
            case 9: /* R_PPC_ADDR14_BRNTAKEN */
                rel_wr32(p, (rel_rd32(p) & 0xFFFF0003u) | (x & 0x0000FFFCu));
                break;
            case 10: /* R_PPC_REL24 */
                rel_wr32(p, (rel_rd32(p) & 0xFC000003u) | ((x - p) & 0x03FFFFFCu));
                break;
            case 11: /* R_PPC_REL14 */
            case 12: /* R_PPC_REL14_BRTAKEN */
            case 13: /* R_PPC_REL14_BRNTAKEN */
                rel_wr32(p, (rel_rd32(p) & 0xFFFF0003u) | ((x - p) & 0x0000FFFCu));
                break;
            default:
                if (s_debug)
                    fprintf(stderr, "[rel_loader] link: unknown relocation type %u\n", type);
                break;
            }
            rel += 8u;
        }
    }
}

// ---------------------------------------------------------------------------
// rel_loader_link / rel_loader_unlink
// ---------------------------------------------------------------------------

static void rel_loader_note_queue_change(void) {
    if (!s_ram)
        return;
    s_qsig_head = rel_rd32(REL_LOADER_QUEUE_HEAD);
    s_qsig_tail = rel_rd32(REL_LOADER_QUEUE_TAIL);
}

static int rel_loader_link(u32 header, u32 bss, int fixed) {
    const ModernGekkoRelModule* desc;
    const ModernGekkoRelSection* first;
    u32 id;
    u32 num_sections;
    u32 sio;
    u32 version;
    u32 l;
    u32 qhead;
    u32 qtail;
    u32 imp_offset;
    u32 imp_size;
    u32 bss_size;
    u32 bss_align;
    u32 l_bss;
    u32 i;
    u32 bss_section = 0;
    u32 queue_next;

    if (header < 0x80000000u || bss < 0x80000000u) {
        if (s_debug)
            fprintf(stderr, "[rel_loader] link: bad args header=0x%08X bss=0x%08X\n", header, bss);
        return 0;
    }

    id = rel_rd32(header);
    num_sections = rel_rd32(header + 0x0Cu);
    sio = rel_rd32(header + 0x10u);
    version = rel_rd32(header + 0x1Cu);

    desc = rel_descriptor_for(id);
    if (!desc || num_sections == 0u || desc->section_count != num_sections ||
        desc->section_info_offset != sio || desc->version != version) {
        if (s_debug)
            fprintf(stderr,
                    "[rel_loader] link: descriptor mismatch id=%u secs=%u sio=0x%X ver=%u "
                    "(desc secs=%u sio=0x%X ver=%u) -> FALSE\n",
                    id, num_sections, sio, version, desc ? desc->section_count : 0u,
                    desc ? desc->section_info_offset : 0u, desc ? desc->version : 0u);
        return 0;
    }
    if (fixed && version < 3u)
        return 0;

    // Linked base: descriptor first (only) exec section's linked_start minus
    // that section's raw file offset (read from the R copy's section table).
    first = &desc->sections[0];
    l = first->linked_start -
        (rel_rd32(header + sio + first->section_index * 8u) & ~1u);

    // --- B24 live-object guard: refuse links whose reserved L window
    //     (file image or linked bss) would stomp a live f_pc create request
    //     or its unpublished process object. See the pulse10 rationale at
    //     REL_LOADER_FPC_CTQ_HEAD. ---
    // --- Queue emulation (retail EnqueueTail) ---
    {
        u32 win_lo[2], win_hi[2];
        u32 bsz, bal, lbss, bsz_align;
        int nw = 0;
        u32 rq_node;

        win_lo[nw] = l;
        win_hi[nw] = l + desc->file_size;
        nw++;
        bsz = rel_rd32(header + 0x20u);
        if (bsz != 0u) {
            bal = rel_rd32(header + 0x44u);
            if (bal == 0u)
                bal = 4u;
            lbss = (l + desc->file_size + bal - 1u) & ~(bal - 1u);
            win_lo[nw] = lbss;
            win_hi[nw] = lbss + bsz;
        }

        u32 rq_guard = 0;
        for (rq_node = rel_rd32(REL_LOADER_FPC_CTQ_HEAD); rq_node != 0u &&
             rq_node >= 0x80000000u && rq_node < 0x82000000u &&
             rq_guard++ < 4096u;) { /* cap: a queue cycle must not hang the link */
            u32 res = rel_rd32(rq_node + 0x40u);
            int hit = -1;
            int k;
            for (k = 0; k < nw; k++) {
                if (rq_node >= win_lo[k] && rq_node < win_hi[k])
                    hit = k;
                else if (res >= 0x80000000u && res < 0x82000000u &&
                         res >= win_lo[k] && res < win_hi[k])
                    hit = k;
                if (hit >= 0)
                    break;
            }
            if (hit >= 0) {
                fprintf(stderr,
                        "[rel_loader] link-guard: id=%u L window [%08X..%08X)+[%08X..%08X) "
                        "collides with live create req %08X res %08X -> refusing\n",
                        id, win_lo[0], win_hi[0],
                        nw > 1 ? win_lo[1] : 0u, nw > 1 ? win_hi[1] : 0u, rq_node, res);
                /* Refused links still signal churn: re-carve now so fresh
                 * heaps stop feeding the refused window's band (the sweep
                 * is idempotent — already-carved heaps are pure list
                 * walks). The old s_band_dirty "sweep soonest poll" flag
                 * was write-only: nothing ever polled it. */
                if (s_band_protect)
                    band_protect_sweep("link-refused");
                return 0;
            }
            rq_node = rel_rd32(rq_node + 8u);
        }
    }
    qhead = rel_rd32(REL_LOADER_QUEUE_HEAD);
    qtail = rel_rd32(REL_LOADER_QUEUE_TAIL);
    rel_wr32(header + 0x04u, 0u); /* link.next = NULL */
    rel_wr32(header + 0x08u, qtail); /* link.prev = tail */
    if (qtail)
        rel_wr32(qtail + 0x04u, header);
    else
        rel_wr32(REL_LOADER_QUEUE_HEAD, header);
    rel_wr32(REL_LOADER_QUEUE_TAIL, header);

    // --- Header pointer fixups (retail-exact) ---
    // NOTE: sectionInfoOffset (+0x10) stays RAW: RefreshRelSections matches
    // it against the descriptor (deviation (1) in the file header).
    rel_wr32(header + 0x24u, rel_rd32(header + 0x24u) + header); /* relOffset */
    rel_wr32(header + 0x28u, rel_rd32(header + 0x28u) + header); /* impOffset */
    if (version >= 3u)
        rel_wr32(header + 0x48u, rel_rd32(header + 0x48u) + header); /* fixSize */

    // --- Section table fixups ---
    for (i = 1; i < num_sections; i++) {
        u32 entry = header + sio + i * 8u;
        u32 off = rel_rd32(entry);
        u32 size = rel_rd32(entry + 4u);
        if (off != 0u) {
            rel_wr32(entry, off + header);
        } else if (size != 0u) {
            bss_section = i;
            rel_wr32(entry, bss);
        }
    }
    rel_wr8(header + 0x33u, (u8)bss_section);

    // --- Import table fixups ---
    imp_offset = rel_rd32(header + 0x28u);
    imp_size = rel_rd32(header + 0x2Cu);
    for (i = 0; i < imp_size; i += 8u) {
        /* Same corrupt-table bound as rel_loader_relocate: stop at the
         * mapped boundary instead of spinning imp_size/8 no-op iterations. */
        if (!rel_any_ptr_n(imp_offset + i, 8u))
            break;
        rel_wr32(imp_offset + i + 4u, rel_rd32(imp_offset + i + 4u) + header);
    }

    // --- prolog / epilog / unresolved ---
    {
        u8 psec = rel_rd8(header + 0x30u);
        u8 esec = rel_rd8(header + 0x31u);
        u8 usec = rel_rd8(header + 0x32u);
        if (psec != 0u)
            rel_wr32(header + 0x34u,
                     rel_rd32(header + 0x34u) + (rel_rd32(header + sio + psec * 8u) & ~1u));
        if (esec != 0u)
            rel_wr32(header + 0x38u,
                     rel_rd32(header + 0x38u) + (rel_rd32(header + sio + esec * 8u) & ~1u));
        if (usec != 0u)
            rel_wr32(header + 0x3Cu,
                     rel_rd32(header + 0x3Cu) + (rel_rd32(header + sio + usec * 8u) & ~1u));
    }

    // --- nameOffset += string table ---
    {
        u32 st = rel_rd32(REL_LOADER_STRING_TABLE);
        if (st)
            rel_wr32(header + 0x14u, rel_rd32(header + 0x14u) + st);
    }

    // --- Relocation pass (replacement-computed; compiled Relocate never
    //     runs). Order per retail Link(): module-0 self relocate, then
    //     two-way relocate over the queue. ---
    rel_loader_relocate(header, 0u, header);
    queue_next = rel_rd32(REL_LOADER_QUEUE_HEAD);
    {
        /* The module queue can never legitimately hold more than
         * REL_LOADER_MAX_ACTIVE nodes; a cycle in corrupt RAM would
         * otherwise loop (and re-relocate) forever. */
        u32 queue_guard = 0;
        while (queue_next && queue_guard++ <= REL_LOADER_MAX_ACTIVE) {
            u32 m = queue_next;
            u32 next = rel_rd32(m + 0x04u);
            u32 m_id = rel_rd32(m);
            rel_loader_relocate(header, id, m); /* m imports us */
            if (m != header)
                rel_loader_relocate(m, m_id, header); /* we import m */
            queue_next = next;
        }
    }

    // --- OSLinkFixed: truncate impSize at the first id==0||id==self import ---
    if (fixed) {
        for (i = 0; i < imp_size; i += 8u) {
            u32 iid = rel_rd32(imp_offset + i);
            if (iid == 0u || iid == id) {
                rel_wr32(header + 0x2Cu, i);
                break;
            }
        }
    }

    // --- Mirror fixed-up DATA sections to L (exec sections are not
    //     mirrored; BSS sections point outside the image) ---
    for (i = 1; i < num_sections; i++) {
        u32 entry = header + sio + i * 8u;
        u32 off = rel_rd32(entry) & ~1u;
        u32 size = rel_rd32(entry + 4u);
        u8* src;
        u8* dst;
        u32 span;
        if (size == 0u)
            continue;
        if (off < header || off >= header + desc->file_size)
            continue; /* bss */
        if (rel_rd32(entry) & 1u)
            continue; /* exec */
        /* size is live guest RAM — clamp it to the image tail so a corrupt
         * section entry cannot drive memcpy past the host buffers. */
        if (size > (header + desc->file_size) - off)
            size = (header + desc->file_size) - off;
        span = size;
        src = rel_span_ptr(off, &span);
        dst = rel_any_ptr_n(l + (off - header), span);
        if (src && dst)
            memcpy(dst, src, span);
    }

    /* phase3-lreloc-p11 — retail self-pointer sweep (default OFF via
     * MODERNGEKKO_P11_SWEEP=1; unset = bit-identical legacy boots).
     * TWW RELs ship with data sections PRE-RELOCATED at RETAIL addresses
     * for pointers retail OSLink does NOT carry reloc entries for. On the
     * MEM1 lineage the retail values happened to be correct as-is; under
     * LRELOC they are stale: any data->exec pointer computed at the factory
     * (method tables, thread entries, vtables) still holds the RETAIL twin
     * of this module's own code (p8: module-9 self 0x8061B7E0; p10:
     * module-96 self 0x806D2550 — both executed a slot holding the module's
     * own retail code address while the live LRELOC image sits elsewhere).
     * Sweep every image-backed non-exec data section AFTER the mirror and
     * rewrite R + L when the value is a retail-band word whose L-twin
     * (+0x10C00000, the fixed retail->LRELOC delta) lands inside THIS
     * module's L window (strictest predicate — both known halts satisfy
     * it). In-band words whose L-twin falls OUTSIDE this module's window
     * (cross-module candidates) are counted only: rewriting them needs
     * per-case review. */
    if (s_p11_sweep) {
        u32 sweep_fixed = 0;
        u32 sweep_xmod = 0;
        for (i = 1; i < num_sections; i++) {
            u32 entry = header + sio + i * 8u;
            u32 off = rel_rd32(entry) & ~1u;
            u32 size = rel_rd32(entry + 4u);
            u32 w;
            if (size == 0u)
                continue;
            if (off < header || off >= header + desc->file_size)
                continue; /* bss: zeroed, not image-backed */
            if (rel_rd32(entry) & 1u)
                continue; /* exec */
            for (w = 0u; w + 4u <= size; w += 4u) {
                u32 addr = off + w;
                u32 v = rel_rd32(addr);
                u32 twin;
                if ((v & 3u) != 0u)
                    continue;
                if (v < 0x80500000u || v >= 0x80C873D0u)
                    continue; /* not a retail-band pointer */
                twin = v + 0x10C00000u;
                if (twin < l || twin >= l + desc->file_size) {
                    /* cross-module (or non-module) candidate: log only */
                    sweep_xmod++;
                    continue;
                }
                rel_wr32(addr, twin);             /* fix the R image */
                rel_wr32(l + (off - header) + w, twin); /* keep L in sync */
                sweep_fixed++;
            }
        }
        fprintf(stderr,
                "[rel_loader] p11 sweep id=%u: %u retail self-pointers translated "
                "(%u cross-module candidates skipped)\n",
                id, sweep_fixed, sweep_xmod);
    }

    // --- Diagnostic: ctor/dtor table state right after link (R vs L) ---
    if (s_debug || id == 168u || getenv("MODERNGEKKO_MIRROR_DIAG")) {
        for (i = 1; i < num_sections && i < 4u; i++) {
            u32 entry = header + sio + i * 8u;
            u32 off = rel_rd32(entry) & ~1u;
            u32 size = rel_rd32(entry + 4u);
            if (size == 0u || off < header || off >= header + desc->file_size)
                continue;
            if (rel_rd32(entry) & 1u)
                continue;
            fprintf(stderr,
                    "[rel_loader] link id=%u s%u R=0x%08X L=0x%08X [0x%08X 0x%08X] L[0x%08X "
                    "0x%08X]\n",
                    id, i, off, l + (off - header), rel_rd32(off), rel_rd32(off + 4u),
                    rel_rd32(l + (off - header)), rel_rd32(l + (off - header) + 4u));
        }
        if (getenv("MODERNGEKKO_MIRROR_DIAG")) {
            fprintf(stderr,
                    "[mirror-diag] id=%u s_exram=%p size=0x%X dst_ok=%d\n",
                    id, (void*)s_exram, s_exram_size,
                    rel_any_ptr(l + (rel_rd32(header + sio + 8 + 8) & ~1u)) != NULL ? 1 : 0);
        }
    }

    /* p12 mt-watch: dump the method-table region right after each link so
     * the [rel_loader] link rows bracket the exact link-generation that
     * writes the retail pointer (default OFF). */
    if (s_mt_watch && s_ram) {
        u32 a;
        fprintf(stderr, "[mt-watch] region @link id=%u (gen=%u):\n", id,
                s_heap_gen);
        for (a = 0x80AB1E80u; a < 0x80AB1F60u; a += 16u) {
            fprintf(stderr,
                    "[mt-watch]   0x%08X: %08X %08X %08X %08X\n",
                    a, rel_rd32(a), rel_rd32(a + 4u), rel_rd32(a + 8u),
                    rel_rd32(a + 0xCu));
        }
    }

    // --- BSS zeroing: runtime bss + linked bss ---
    bss_size = rel_rd32(header + 0x20u);
    if (bss && bss_size) {
        /* bss_size is live guest RAM: clamp the memset to the mapped span so
         * a corrupt header cannot run the zero-fill off the end of MEM1. */
        u32 span = bss_size;
        u8* p = rel_span_ptr(bss, &span);
        if (p)
            memset(p, 0, span);
    }
    bss_align = rel_rd32(header + 0x44u);
    if (bss_align == 0u)
        bss_align = 4u;
    l_bss = (l + desc->file_size + bss_align - 1u) & ~(bss_align - 1u);
    if (bss_size) {
        u32 span = bss_size;
        u8* p = rel_span_ptr(l_bss, &span);
        if (p)
            memset(p, 0, span);
    }

    // --- Register in the active table ---
    if (s_active_count < REL_LOADER_MAX_ACTIVE) {
        RelActiveModule* m = &s_active[s_active_count];
        m->r = header;
        m->l = l;
        m->file_size = desc->file_size;
        m->sio = sio;
        m->num_sections = num_sections;
        m->id = id;
        m->l_dirty = 0u;
        m->l_gen = s_heap_gen;
        m->probe_off = 0u;
        m->probe_size = 0u;
        /* Cache the first non-exec image data section for the cheap L-vs-R
         * probe (run once per heap generation, not per dispatch). */
        for (i = 1; i < num_sections; i++) {
            u32 entry = header + sio + i * 8u;
            u32 off = rel_rd32(entry) & ~1u;
            u32 size = rel_rd32(entry + 4u);
            if (size < 8u)
                continue;
            if (off < header || off >= header + desc->file_size)
                continue; /* bss */
            if (rel_rd32(entry) & 1u)
                continue; /* exec */
            m->probe_off = off - header;
            m->probe_size = size;
            break;
        }
        s_active_count++;
        s_active_seq++;
        ++g_rel_dispatch_gen;
    }
    rel_loader_rebuild_journal_bounds();
    /* B28: the window writes above are done — reserve immediately so the
     * next allocation already honors this module's band. */
    if (s_band_protect)
        band_protect_sweep("link");
    if (s_journal_filter_enabled)
        rel_loader_rebuild_journal_mask();
    rel_loader_note_queue_change();

    if (s_debug)
        fprintf(stderr, "[rel_loader] link: id=%u R=0x%08X L=0x%08X bss=0x%08X "
                        "sections=%u file_size=0x%X fixed=%d -> TRUE\n",
                id, header, l, bss, num_sections, desc->file_size, fixed);
    return 1;
}

static int rel_loader_unlink(u32 header) {
    u32 id;
    u32 i;
    u32 found = 0;
    RelActiveModule saved;
    u32 next;
    u32 prev;
    u32 sio;

    if (header < 0x80000000u)
        return 0;
    id = rel_rd32(header);
    // Dequeue (retail DequeueItem).
    next = rel_rd32(header + 0x04u);
    prev = rel_rd32(header + 0x08u);
    if (next)
        rel_wr32(next + 0x08u, prev);
    else
        rel_wr32(REL_LOADER_QUEUE_TAIL, prev);
    if (prev)
        rel_wr32(prev + 0x04u, next);
    else
        rel_wr32(REL_LOADER_QUEUE_HEAD, next);

    // Invalidate: zero the R header id word (chassis stops matching
    // immediately) and zero the L data mirror (stale pointers fault loudly).
    sio = rel_rd32(header + 0x10u);
    memset(&saved, 0, sizeof(saved));
    for (i = 0; i < s_active_count; i++) {
        if (s_active[i].r == header) {
            saved = s_active[i];
            s_active[i] = s_active[s_active_count - 1];
            s_active_count--;
            s_active_seq++;
            ++g_rel_dispatch_gen;
            found = 1;
            break;
        }
    }
    rel_loader_rebuild_journal_bounds();
    if (s_journal_filter_enabled)
        rel_loader_rebuild_journal_mask();

    if (saved.l) {
        u32 secs = saved.num_sections ? saved.num_sections : rel_rd32(header + 0x0Cu);
        for (i = 1; i < secs; i++) {
            u32 entry = header + sio + i * 8u;
            u32 off = rel_rd32(entry) & ~1u;
            u32 size = rel_rd32(entry + 4u);
            u8* p;
            if (size == 0u)
                continue;
            if (off < header || off >= header + saved.file_size)
                continue; /* bss */
            if (rel_rd32(entry) & 1u)
                continue; /* exec */
            /* Clamp the zeroing to the image tail AND the mapped span: the
             * section table is live guest RAM. */
            if (size > (header + saved.file_size) - off)
                size = (header + saved.file_size) - off;
            p = rel_any_ptr_n(saved.l + (off - header), size);
            if (p)
                memset(p, 0, size);
        }
    }

    rel_wr32(header, 0u); /* id = 0 */
    rel_loader_note_queue_change();

    /* Unlinks also signal heap churn: re-carve now (same trigger class as
     * link) so heaps created since the last sweep stop handing out band
     * bytes. Idempotent — a fully-carved tree is a pure list walk. */
    if (s_band_protect)
        band_protect_sweep("unlink");

    if (s_debug)
        fprintf(stderr, "[rel_loader] unlink: id=%u R=0x%08X active=%d -> TRUE\n", id, header,
                found);
    return 1;
}

// ---------------------------------------------------------------------------
// Cheap L-vs-R data probe: compare the first 8 bytes of the module's first
// non-exec image data section. The JKR heap fill (0xFF/0x00) visibly differs
// from the durable R copy, so a mismatch detects clobbers that bypass both
// the write journal and the do_alloc hook. Returns 1 when L needs refresh.
// ---------------------------------------------------------------------------
static int rel_loader_l_data_mismatch(const RelActiveModule* m) {
    u32 i;
    u8* rp;
    u8* lp;
    if (!s_ram || m->l == 0u)
        return 0;
    for (i = 1; i < m->num_sections; i++) {
        u32 entry = m->r + m->sio + i * 8u;
        u32 roff = rel_rd32(entry) & ~1u;
        u32 rsize = rel_rd32(entry + 4u);
        u32 n = rsize < 8u ? rsize : 8u;
        if (n == 0u)
            continue;
        if (roff < m->r || roff >= m->r + m->file_size)
            continue; /* bss */
        if (rel_rd32(entry) & 1u)
            continue; /* exec */
        /* Sized probes: the single-byte variants only guarantee the first
         * byte is mapped; memcmp reads n (up to 8). */
        rp = rel_ram_ptr_n(roff, n);
        lp = rel_any_ptr_n(m->l + (roff - m->r), n);
        if (rp && lp && memcmp(rp, lp, n) != 0)
            return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Refresh the module's linked (L) image from the durable R copy: re-mirror
// data sections R->L and re-zero the linked bss. The L mirror is only fresh
// at link time; the game's heap arena reuses the linked region afterwards
// (clobbering the dtor table and the global destructor chain with allocator
// fill patterns). The R copy is the game's actual module allocation and stays
// correct (relocated, write-journaled). Called from the ModuleDestructorsX
// hook before the unload epilog walks the table.
// ---------------------------------------------------------------------------

static void rel_loader_refresh_l_image(const RelActiveModule* m) {
    u32 i;
    for (i = 1; i < m->num_sections; i++) {
        u32 entry = m->r + m->sio + i * 8u;
        u32 off = rel_rd32(entry) & ~1u;
        u32 size = rel_rd32(entry + 4u);
        u8* src;
        u8* dst;
        if (size == 0u)
            continue;
        if (off < m->r || off >= m->r + m->file_size)
            continue; /* bss section */
        if (rel_rd32(entry) & 1u)
            continue; /* exec */
        /* Clamp size to the image tail before resolving host pointers (the
         * section table is live guest RAM). */
        if (size > (m->r + m->file_size) - off)
            size = (m->r + m->file_size) - off;
        {
            u32 span = size;
            src = rel_span_ptr(off, &span);
            dst = rel_any_ptr_n(m->l + (off - m->r), span);
            if (src && dst)
                memcpy(dst, src, span);
        }
    }
    /* linked bss: fresh zeroed state (empty global destructor chain; the
     * chain lives in the LINKED bss, which the heap may have reused) */
    {
        u32 bss_size = rel_rd32(m->r + 0x20u);
        u32 bss_align = rel_rd32(m->r + 0x44u);
        u32 l_bss;
        u32 span;
        u8* p;
        if (bss_align == 0u)
            bss_align = 4u;
        l_bss = (m->l + m->file_size + bss_align - 1u) & ~(bss_align - 1u);
        span = bss_size;
        p = rel_span_ptr(l_bss, &span);
        if (p && span)
            memset(p, 0, span);
    }
}

// ---------------------------------------------------------------------------
// Diagnostic: dump the ctor/dtor table the game is about to walk (guest RAM
// at the argument), and the active-module state around it (R copy vs L
// mirror, linked vs runtime bss). Used to prove where stale L-copy data is
// read at unload time.
// ---------------------------------------------------------------------------

static void rel_loader_dump_ctor_dtor(const char* tag, CPUState* ctx) {
    u32 arg = ctx->gpr[3];
    u32 i;
    u32 t0 = 0, t1 = 0, t2 = 0, t3 = 0;

    fprintf(stderr, "[dtor] %s: arg=0x%08X table=[", tag, arg);
    if (arg >= 0x80000000u && arg < 0x82000000u) {
        t0 = rel_rd32(arg);
        t1 = rel_rd32(arg + 4u);
        t2 = rel_rd32(arg + 8u);
        t3 = rel_rd32(arg + 12u);
    }
    fprintf(stderr, "0x%08X 0x%08X 0x%08X 0x%08X]\n", t0, t1, t2, t3);

    for (i = 0; i < s_active_count; i++) {
        const RelActiveModule* m = &s_active[i];
        u32 bss_align;
        u32 l_bss;
        u8 bsec;
        u32 r_bss = 0;
        if (arg < m->l || arg >= m->l + m->file_size)
            continue;
        fprintf(stderr, "[dtor]   in L range id=%u R=0x%08X L=0x%08X -> R-addr=0x%08X\n", m->id,
                m->r, m->l, m->r + (arg - m->l));
        fprintf(stderr, "[dtor]   R table: [0x%08X 0x%08X]   L table: [0x%08X 0x%08X]\n",
                rel_rd32(m->r + (arg - m->l)), rel_rd32(m->r + (arg - m->l) + 4u),
                rel_rd32(arg), rel_rd32(arg + 4u));
        bss_align = rel_rd32(m->r + 0x44u);
        if (bss_align == 0u)
            bss_align = 4u;
        l_bss = (m->l + m->file_size + bss_align - 1u) & ~(bss_align - 1u);
        bsec = rel_rd8(m->r + 0x33u);
        if (bsec && bsec < m->num_sections)
            r_bss = rel_rd32(m->r + m->sio + bsec * 8u) & ~1u;
        fprintf(stderr,
                "[dtor]   bss linked=0x%08X chain0=0x%08X | runtime=0x%08X chain0=0x%08X\n",
                l_bss, rel_rd32(l_bss), r_bss, r_bss ? rel_rd32(r_bss) : 0u);
        return;
    }
    fprintf(stderr, "[dtor]   arg not in any active module L range (active=%u)\n", s_active_count);
}

// ---------------------------------------------------------------------------
// Post-state-load active-table resync
// ---------------------------------------------------------------------------
// s_active[] is host BSS — save-state serialization covers guest RAM only, so
// a --load-state boot (or in-session restore) leaves it empty/stale while the
// restored guest OSModuleInfo queue still lists every linked REL. With the
// table empty the write journal early-outs: no R->L data mirroring, no
// L-clobber dirty-marking, and no L-window owner for the chunk dispatcher —
// the game either crawls through the fallback path or dispatches an
// unmappable 0x9xxxxxxx pc (DSI -> JUTException). The queue (0x800030C8/CC) is
// restored verbatim and each module's L base is a pure function of its
// descriptor plus the restored section table, so the table is rebuildable
// from RAM alone. Rebuild only when the queue signature diverges from what
// link/unlink last synced (normal play keeps the table authoritative —
// rebuilding mid-play would wrongly clear l_dirty and trigger linked-bss
// re-zeroing through the refresh path).

static void rel_loader_rebuild_active_from_queue(void) {
    u32 node;
    u32 guard = 0;
    s_active_count = 0;
    s_active_seq++;
    ++g_rel_dispatch_gen;
    for (node = rel_rd32(REL_LOADER_QUEUE_HEAD);
         node != 0u && guard < REL_LOADER_MAX_ACTIVE;
         node = rel_rd32(node + 0x04u), ++guard) {
        const ModernGekkoRelModule* desc;
        RelActiveModule* m;
        u32 id, num_sections, sio, version, l, i;
        u32 j;
        int dup = 0;
        if (node < 0x80000000u || node - 0x80000000u >= s_ram_size)
            break;
        id = rel_rd32(node);
        num_sections = rel_rd32(node + 0x0Cu);
        sio = rel_rd32(node + 0x10u);
        version = rel_rd32(node + 0x1Cu);
        desc = rel_descriptor_for(id);
        if (!desc || num_sections == 0u || desc->section_count != num_sections ||
            desc->section_info_offset != sio || desc->version != version)
            continue; /* not a live module node; keep walking while next is sane */
        if ((u64)(node - 0x80000000u) + sio + (u64)num_sections * 8u > s_ram_size)
            continue; /* section table outside MEM1 */
        for (j = 0; j < s_active_count; ++j) {
            if (s_active[j].r == node) {
                dup = 1;
                break;
            }
        }
        if (dup)
            break; /* queue cycle */
        /* Post-link the exec-section table entry holds (raw_off + node) with
         * the exec flag in bit0 — invert the link-time base computation. */
        {
            const ModernGekkoRelSection* first = &desc->sections[0];
            l = first->linked_start -
                ((rel_rd32(node + sio + first->section_index * 8u) & ~1u) - node);
        }
        m = &s_active[s_active_count];
        m->r = node;
        m->l = l;
        m->file_size = desc->file_size;
        m->sio = sio;
        m->num_sections = num_sections;
        m->id = id;
        m->l_dirty = 0u;
        m->l_gen = s_heap_gen;
        m->probe_off = 0u;
        m->probe_size = 0u;
        /* First non-exec image data section, same rule as rel_loader_link
         * (entries are already absolute post-fixup). */
        for (i = 1; i < num_sections; i++) {
            u32 entry = node + sio + i * 8u;
            u32 off = rel_rd32(entry) & ~1u;
            u32 size = rel_rd32(entry + 4u);
            if (size < 8u)
                continue;
            if (off < node || off >= node + desc->file_size)
                continue; /* bss */
            if (rel_rd32(entry) & 1u)
                continue; /* exec */
            m->probe_off = off - node;
            m->probe_size = size;
            break;
        }
        s_active_count++;
        if (s_debug)
            fprintf(stderr, "[rel_loader] resync:   id=%u R=0x%08X L=[0x%08X..0x%08X)\n",
                    id, node, l, l + desc->file_size);
    }
    rel_loader_rebuild_journal_bounds();
    if (s_journal_filter_enabled)
        rel_loader_rebuild_journal_mask();
    fprintf(stderr, "[rel_loader] resync: rebuilt %u active module(s) from restored queue\n",
            s_active_count);
}

int rel_loader_resync_active(CPUState* ctx) {
    u32 head, tail;
    if (!ctx)
        return 0;
    s_ram = ctx->ram;
    s_ram_size = ctx->ram_size;
    s_exram = ctx->exram;
    s_exram_size = ctx->exram_size;
    if (!s_ram)
        return 0;
    head = rel_rd32(REL_LOADER_QUEUE_HEAD);
    tail = rel_rd32(REL_LOADER_QUEUE_TAIL);
    if (head == s_qsig_head && tail == s_qsig_tail &&
        (s_active_count != 0u || head == 0u))
        return 0;
    rel_loader_rebuild_active_from_queue();
    s_qsig_head = head;
    s_qsig_tail = tail;
    return 1;
}

// ---------------------------------------------------------------------------
// Replacement dispatcher (called by the generated dispatch on every module
// entry, before host-call/original). Returns 1 when covered.
// ---------------------------------------------------------------------------

/* In-module replacement for PSMTXConcat (0x8030D0FC) — the ~50-instr
 * paired-single 3x4 affine matrix multiply, ~2% of sampled guest execution.
 * Bit-exact reproduction of the guest body: paired-single arithmetic
 * evaluates as f64 fma(a, force_25bit_c(c_lane), b) with the ni_madd_msub
 * tie fix and a force_single round, matching cpu_interpreter_float.c;
 * loads/stores go through the same mem_*_direct accessors the generated
 * code uses, so journaling, reservations and window translation are
 * identical. For all-finite inputs and intermediates the only fpscr effect
 * is FPRF = classify(out[2][2]) (the last arithmetic op, ps_madds1 f0);
 * any non-finite value falls back to the guest body, keeping the NaN/Inf
 * exception paths exact. Register effects reproduced: f0..f13 result/load
 * lanes, r6 = Unit01 (0x803F66F0), the stwu link word + stfd spills in the
 * red zone, and downcount -51 (the generated entry charge). f14/f15/f31
 * are restored by the guest, so this path simply never touches them.
 * MODERNGEKKO_REPL_MTX=0 disables. */
static int s_repl_mtx = 1;

static unsigned mtxrepl_lz64(u64 v) {
    unsigned n = 0;
    while (!(v & 0x8000000000000000ull)) { v <<= 1; ++n; }
    return n;
}
static f64 mtxrepl_force_25bit_c(f64 d) {
    u64 integral = f64_bits(d);
    u64 exponent = integral & 0x7FF0000000000000ull;
    u64 fraction = integral & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0 && fraction != 0) {
        s64 keep_mask = (s64)0xFFFFFFFFF8000000ll;
        u64 round = 0x8000000u;
        unsigned shift = mtxrepl_lz64(fraction) - 11u;
        keep_mask >>= shift;
        round >>= shift;
        integral = (integral & (u64)keep_mask) + (integral & round);
    } else {
        integral = (integral & 0xFFFFFFFFF8000000ull) + (integral & 0x8000000ull);
    }
    return f64_value(integral);
}
#define MTXREPL_FPSCR_NI 0x00000004u
static f32 mtxrepl_force_single(const CPUState* cpu, f64 value) {
    if (cpu->fpscr & MTXREPL_FPSCR_NI) {
        u64 no_sign = f64_bits(value) & 0x7FFFFFFFFFFFFFFFull;
        if (no_sign < 0x3810000000000000ull) {
            u32 flushed = (u32)((f64_bits(value) & 0x8000000000000000ull) >> 32);
            return f32_value(flushed);
        }
    }
    return (f32)value;
}
static u32 mtxrepl_classify_f32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 sign = bits >> 31;
    u32 exponent = bits & 0x7F800000u;
    u32 fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}
/* c25 = pre-rounded broadcast operand. Every non-finite intermediate
 * propagates to a final output lane (Inf/NaN can never become finite again
 * through mul/fma), so checking only the 12 outputs is airtight: a bail
 * fires iff any input or intermediate was non-finite. */
static f32 mtxrepl_madd(const CPUState* cpu, f64 a, f64 c25, f64 b) {
    f64 value = fma(a, c25, b);
    u64 result_bits = f64_bits(value);
    const u64 D_MASK = 0x000000001FFFFFFFull;
    const u64 EVEN_TIE = 0x0000000010000000ull;
    if ((result_bits & D_MASK) == EVEN_TIE) {
        f64 a_prime = b - value;
        f64 b_prime = value + a_prime;
        f64 delta_a = fma(a, c25, a_prime);
        f64 delta_b = b - b_prime;
        f64 error = delta_a + delta_b;
        if (error != 0.0) {
            if ((error > 0.0) == (value > 0.0))
                value = f64_value(result_bits + 1);
            else
                value = f64_value(result_bits - 1);
        }
    }
    return mtxrepl_force_single(cpu, value);
}
static f64 mtxrepl_ld(CPUState* ctx, u32 ea) {
    return f64_value(convert_to_double(mem_read32_direct(ctx, ea)));
}
static void mtxrepl_st(CPUState* ctx, u32 ea, f64 v) {
    mem_write32_direct(ctx, ea, convert_to_single_ftz(f64_bits(v)));
}

static int mtxrepl_psmtxconcat(CPUState* ctx) {
    if (!(ctx->msr & PPC_MSR_FP)) return 0;
    const u32 gqr0 = ctx->gqr[0];
    if ((((gqr0 >> 16) & 7u) != 0u) || ((gqr0 & 7u) != 0u)) return 0;
    if (!(ctx->hid2 & PPC_HID2_LSQE)) return 0;

    const u32 sp = ctx->gpr[1];
    const u32 ea = ctx->gpr[3], eb = ctx->gpr[4], eo = ctx->gpr[5];

    const f64 a00 = mtxrepl_ld(ctx, ea + 0u),  a01 = mtxrepl_ld(ctx, ea + 4u);
    const f64 a02 = mtxrepl_ld(ctx, ea + 8u),  a03 = mtxrepl_ld(ctx, ea + 12u);
    const f64 a10 = mtxrepl_ld(ctx, ea + 16u), a11 = mtxrepl_ld(ctx, ea + 20u);
    const f64 a12 = mtxrepl_ld(ctx, ea + 24u), a13 = mtxrepl_ld(ctx, ea + 28u);
    const f64 a20 = mtxrepl_ld(ctx, ea + 32u), a21 = mtxrepl_ld(ctx, ea + 36u);
    const f64 a22 = mtxrepl_ld(ctx, ea + 40u), a23 = mtxrepl_ld(ctx, ea + 44u);
    const f64 b00 = mtxrepl_ld(ctx, eb + 0u),  b01 = mtxrepl_ld(ctx, eb + 4u);
    const f64 b02 = mtxrepl_ld(ctx, eb + 8u),  b03 = mtxrepl_ld(ctx, eb + 12u);
    const f64 b10 = mtxrepl_ld(ctx, eb + 16u), b11 = mtxrepl_ld(ctx, eb + 20u);
    const f64 b12 = mtxrepl_ld(ctx, eb + 24u), b13 = mtxrepl_ld(ctx, eb + 28u);
    const f64 b20 = mtxrepl_ld(ctx, eb + 32u), b21 = mtxrepl_ld(ctx, eb + 36u);
    const f64 b22 = mtxrepl_ld(ctx, eb + 40u), b23 = mtxrepl_ld(ctx, eb + 44u);
    const f64 q0 = mtxrepl_ld(ctx, 0x803F66F0u), q1 = mtxrepl_ld(ctx, 0x803F66F4u);
    if (ctx->exception) return 0;

    const f64 c00 = mtxrepl_force_25bit_c(a00), c01 = mtxrepl_force_25bit_c(a01);
    const f64 c02 = mtxrepl_force_25bit_c(a02), c03 = mtxrepl_force_25bit_c(a03);
    const f64 c10 = mtxrepl_force_25bit_c(a10), c11 = mtxrepl_force_25bit_c(a11);
    const f64 c12 = mtxrepl_force_25bit_c(a12), c13 = mtxrepl_force_25bit_c(a13);
    const f64 c20 = mtxrepl_force_25bit_c(a20), c21 = mtxrepl_force_25bit_c(a21);
    const f64 c22 = mtxrepl_force_25bit_c(a22), c23 = mtxrepl_force_25bit_c(a23);
    const f64 cq0 = mtxrepl_force_25bit_c(q0),  cq1 = mtxrepl_force_25bit_c(q1);

    const f64 f12_0 = b00 * c00, f12_1 = b01 * c00;
    const f64 f13_0 = b02 * c00, f13_1 = b03 * c00;
    const f64 f14_0 = b00 * c10, f14_1 = b01 * c10;
    const f64 f15_0 = b02 * c10, f15_1 = b03 * c10;
    const f64 r12_0 = mtxrepl_madd(ctx, b10, c01, f12_0);
    const f64 r12_1 = mtxrepl_madd(ctx, b11, c01, f12_1);
    const f64 r14_0 = mtxrepl_madd(ctx, b10, c11, f14_0);
    const f64 r14_1 = mtxrepl_madd(ctx, b11, c11, f14_1);
    const f64 r13_0 = mtxrepl_madd(ctx, b12, c01, f13_0);
    const f64 r13_1 = mtxrepl_madd(ctx, b13, c01, f13_1);
    const f64 r15_0 = mtxrepl_madd(ctx, b12, c11, f15_0);
    const f64 r15_1 = mtxrepl_madd(ctx, b13, c11, f15_1);
    const f64 s12_0 = mtxrepl_madd(ctx, b20, c02, r12_0);
    const f64 s12_1 = mtxrepl_madd(ctx, b21, c02, r12_1);
    const f64 s13_0 = mtxrepl_madd(ctx, b22, c02, r13_0);
    const f64 s13_1 = mtxrepl_madd(ctx, b23, c02, r13_1);
    const f64 s14_0 = mtxrepl_madd(ctx, b20, c12, r14_0);
    const f64 s14_1 = mtxrepl_madd(ctx, b21, c12, r14_1);
    const f64 s15_0 = mtxrepl_madd(ctx, b22, c12, r15_0);
    const f64 s15_1 = mtxrepl_madd(ctx, b23, c12, r15_1);
    const f64 f2_0 = b00 * c20, f2_1 = b01 * c20;
    const f64 t13_0 = mtxrepl_madd(ctx, q0, c03, s13_0);
    const f64 t13_1 = mtxrepl_madd(ctx, q1, c03, s13_1);
    const f64 f0_0 = b02 * c20, f0_1 = b03 * c20;
    const f64 t15_0 = mtxrepl_madd(ctx, q0, c13, s15_0);
    const f64 t15_1 = mtxrepl_madd(ctx, q1, c13, s15_1);
    const f64 t2_0 = mtxrepl_madd(ctx, b10, c21, f2_0);
    const f64 t2_1 = mtxrepl_madd(ctx, b11, c21, f2_1);
    const f64 t0_0 = mtxrepl_madd(ctx, b12, c21, f0_0);
    const f64 t0_1 = mtxrepl_madd(ctx, b13, c21, f0_1);
    const f64 u2_0 = mtxrepl_madd(ctx, b20, c22, t2_0);
    const f64 u2_1 = mtxrepl_madd(ctx, b21, c22, t2_1);
    const f64 u0_0 = mtxrepl_madd(ctx, b22, c22, t0_0);
    const f64 u0_1 = mtxrepl_madd(ctx, b23, c22, t0_1);
    const f64 v0_0 = mtxrepl_madd(ctx, q0, c23, u0_0);
    const f64 v0_1 = mtxrepl_madd(ctx, q1, c23, u0_1);
    if (!isfinite(s12_0) || !isfinite(s12_1) || !isfinite(t13_0) || !isfinite(t13_1) ||
        !isfinite(s14_0) || !isfinite(s14_1) || !isfinite(t15_0) || !isfinite(t15_1) ||
        !isfinite(u2_0) || !isfinite(u2_1) || !isfinite(v0_0) || !isfinite(v0_1))
        return 0;

    ctx->downcount -= 51;
    mem_write32_direct(ctx, sp - 64u, sp);
    mem_write64_direct(ctx, sp - 56u, f64_bits(ctx->fpr[14]));
    mem_write64_direct(ctx, sp - 48u, f64_bits(ctx->fpr[15]));
    mem_write64_direct(ctx, sp - 24u, f64_bits(ctx->fpr[31]));
    mtxrepl_st(ctx, eo + 0u, s12_0);  mtxrepl_st(ctx, eo + 4u, s12_1);
    mtxrepl_st(ctx, eo + 16u, s14_0); mtxrepl_st(ctx, eo + 20u, s14_1);
    mtxrepl_st(ctx, eo + 8u, t13_0);  mtxrepl_st(ctx, eo + 12u, t13_1);
    mtxrepl_st(ctx, eo + 24u, t15_0); mtxrepl_st(ctx, eo + 28u, t15_1);
    mtxrepl_st(ctx, eo + 32u, u2_0);  mtxrepl_st(ctx, eo + 36u, u2_1);
    mtxrepl_st(ctx, eo + 40u, v0_0);  mtxrepl_st(ctx, eo + 44u, v0_1);

    ctx->fpr[0] = v0_0;  ctx->ps1[0] = v0_1;
    ctx->fpr[1] = a02;   ctx->ps1[1] = a03;
    ctx->fpr[2] = u2_0;  ctx->ps1[2] = u2_1;
    ctx->fpr[3] = a12;   ctx->ps1[3] = a13;
    ctx->fpr[4] = a20;   ctx->ps1[4] = a21;
    ctx->fpr[5] = a22;   ctx->ps1[5] = a23;
    ctx->fpr[6] = b00;   ctx->ps1[6] = b01;
    ctx->fpr[7] = b02;   ctx->ps1[7] = b03;
    ctx->fpr[8] = b10;   ctx->ps1[8] = b11;
    ctx->fpr[9] = b12;   ctx->ps1[9] = b13;
    ctx->fpr[10] = b20;  ctx->ps1[10] = b21;
    ctx->fpr[11] = b22;  ctx->ps1[11] = b23;
    ctx->fpr[12] = s12_0; ctx->ps1[12] = s12_1;
    ctx->fpr[13] = t13_0; ctx->ps1[13] = t13_1;
    ctx->gpr[6] = 0x803F66F0u;
    ctx->fpscr = (ctx->fpscr & ~(0x1Fu << 12)) |
                 ((mtxrepl_classify_f32((f32)v0_0) & 0x1Fu) << 12);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Leaf replacements (MODERNGEKKO_REPL_LEAF, default on). Each is a
 * line-for-line transcription of the dol-inline-opt generated body for
 * the named entry: same accessor calls (mem_*_direct / the psq type-0
 * fast path), same helper calls (ppc_ps_* / ppc_f* / ppc_fcmp), same CR0
 * merges, same per-block downcount charges, same live-outs. Declines
 * leave ctx untouched so the guest body runs identically.
 *
 * Shared gates:
 *  - leafrepl_fp_ok mirrors the generated ppc_fp_available_inline preface
 *    WITHOUT raising: when lazy-FP is armed and MSR.FP is clear the guest
 *    body itself raises FP-unavailable at the exact faulting pc, so the
 *    repl just declines.
 *  - ctx->exception pre-set -> decline: the generated bodies bail at the
 *    first post-access check, so claiming would over-execute.
 *  - leafrepl_gqr0_ps_ok = the psq fast-path predicate from cpu.h
 *    (GQR0 load type 0 AND store type 0, HID2.LSQE for the non-indexed
 *    form). With it true the inlined sequence below is exactly what
 *    ppc_psq_load_inline/ppc_psq_store_inline do; with it false the guest
 *    takes the quantized/illegal slow path, so the repl declines.
 * ------------------------------------------------------------------ */
static int s_repl_leaf = 1; /* MODERNGEKKO_REPL_LEAF=0 disables */
static int s_repl_idle = 1; /* MODERNGEKKO_REPL_IDLE=0 disables */

static int leafrepl_fp_ok(const CPUState* ctx) {
    return !g_ppc_lazy_fp_enabled || (ctx->msr & PPC_MSR_FP) != 0;
}
static int leafrepl_gqr0_ps_ok(const CPUState* ctx) {
    const u32 gqr0 = ctx->gqr[0];
    return (((gqr0 >> 16) & 7u) == 0u) && ((gqr0 & 7u) == 0u) &&
           (ctx->hid2 & PPC_HID2_LSQE) != 0u;
}
/* cr0 nibble for cmpwi/cmplwi/and./rlwinm. — all merge XER.SO into bit0
 * exactly like the generated bodies. */
static u32 leafrepl_cr0_cmp_s32(const CPUState* ctx, s32 a, s32 b) {
    u32 bits = 0;
    if (a < b)  bits |= 0x8u;
    if (a > b)  bits |= 0x4u;
    if (a == b) bits |= 0x2u;
    return bits | ((ctx->xer >> 31) & 1u);
}
static u32 leafrepl_cr0_cmp_u32(const CPUState* ctx, u32 a, u32 b) {
    u32 bits = 0;
    if (a < b)  bits |= 0x8u;
    if (a > b)  bits |= 0x4u;
    if (a == b) bits |= 0x2u;
    return bits | ((ctx->xer >> 31) & 1u);
}
/* and./rlwinm. record: signed test of the result word. */
static u32 leafrepl_cr0_logic(const CPUState* ctx, u32 v) {
    s32 sv = (s32)v;
    u32 bits = 0;
    if (sv < 0)  bits |= 0x8u;
    if (sv > 0)  bits |= 0x4u;
    if (sv == 0) bits |= 0x2u;
    return bits | ((ctx->xer >> 31) & 1u);
}
static void leafrepl_commit_cr0(CPUState* ctx, u32 cr0) {
    ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr0 << 28);
}
/* psq_l/psq_st GQR0-type-0 fast paths — identical to the generated
 * ppc_psq_load_inline/ppc_psq_store_inline expansion with
 * GX_MEM_* == mem_*_direct under the inline-xlat module build. */
static void leafrepl_psq_l(CPUState* ctx, u8 d, u32 ea, int w) {
    ctx->fpr[d] = f64_value(convert_to_double(mem_read32_direct(ctx, ea)));
    ctx->ps1[d] = w ? 1.0
                    : f64_value(convert_to_double(mem_read32_direct(ctx, ea + 4u)));
}
static void leafrepl_psq_st(CPUState* ctx, u8 s, u32 ea, int w) {
    mem_write32_direct(ctx, ea, convert_to_single_ftz(f64_bits(ctx->fpr[s])));
    if (!w)
        mem_write32_direct(ctx, ea + 4u,
                           convert_to_single_ftz(f64_bits(ctx->ps1[s])));
}
/* lfs/stfs: lfs mirrors the value into ps1; stfs uses the
 * subnormal-preserving convert_to_single (NOT the psq ftz variant). */
static f64 leafrepl_lfs(CPUState* ctx, u32 ea) {
    return f64_value(convert_to_double(mem_read32_direct(ctx, ea)));
}
static void leafrepl_lfs_to(CPUState* ctx, u8 d, u32 ea) {
    const f64 v = leafrepl_lfs(ctx, ea);
    ctx->fpr[d] = v;
    ctx->ps1[d] = v;
}
static void leafrepl_stfs(CPUState* ctx, u8 s, u32 ea) {
    mem_write32_direct(ctx, ea, convert_to_single(f64_bits(ctx->fpr[s])));
}

/* SelectThread idle-poll collapse (0x80307EF4): generated loop
 *   lwz r0,-26288(r13); cmplwi r0,0; beq 0x80307EF4
 * spins at -3/iteration until the flag goes nonzero, then falls into the
 * bl OSDisableInterrupts at 0x80307F00 (generated chunk_0193
 * loop_80307EF4). The flag is only written by code the chassis itself
 * dispatches (interrupt handlers etc.), so the in-chunk spin can never
 * observe a change — collapsing it to one poll per dispatch keeps the
 * -3 charge cadence and the -256 budget exit identical while returning
 * control between polls. Zero flag: r0=0, CR0=EQ|SO, pc stays 0x80307EF4
 * (the loop head, same state as the guest's budget-exit). Nonzero:
 * r0=flag, CR0=GT|SO (cmplwi vs 0 can never be LT), pc=0x80307F00. */
static int idlerepl_selectthread(CPUState* ctx) {
    /* Guest order: charge, pc=head, lwz (cmplwi runs even if the load
     * raised), then the EQ back-edge. */
    ctx->downcount -= 3;
    const u32 flag = mem_read32_direct(ctx, ctx->gpr[13] - 26288u);
    ctx->gpr[0] = flag;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_u32(ctx, flag, 0u));
    ctx->pc = flag ? 0x80307F00u : 0x80307EF4u;
    return 1;
}

/* JUTResFont::getAscent (0x8022F7F8) / getDescent (0x8022F804):
 * lwz r3,76(r3); lhz r3,{10|12}(r3); blr. Charge -3 each (generated
 * chunk_0139 labels). Live-out: r3 only. */
static int leafrepl_fontmetric(CPUState* ctx, u32 field_off) {
    ctx->downcount -= 3;
    /* Commit per instruction: a fault on the lhz must leave gpr[3]
     * holding the metrics pointer, as the generated body would. */
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 76u);
    ctx->gpr[3] = mem_read16_direct(ctx, ctx->gpr[3] + field_off);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* cBgS_Chk::ChkSameActorPid (0x8024734C, generated chunk_0145): rejects
 * when the actor's pid field (r3+8) or the arg pid (r4) is 0xFFFFFFFF, or
 * the byte at r3+12 is zero; else r3 = (r4 == field8). Charges: -4 entry,
 * -3 per middle block, -2 for the li/blr tail, -4 for the compare tail.
 * Live-outs: r0 (last computed), r5 (loaded field), r3, CR0 = last
 * compare, pc=lr. */
static int leafrepl_chksameactorpid(CPUState* ctx) {
    ctx->downcount -= 4;
    ctx->gpr[5] = mem_read32_direct(ctx, ctx->gpr[3] + 8u); /* lwz r5,8(r3) */
    ctx->gpr[0] = ctx->gpr[5] + 0x10000u; /* addis r0,r5,1 */
    u32 cr0 = leafrepl_cr0_cmp_u32(ctx, ctx->gpr[0], 0xFFFFu);
    if (ctx->gpr[0] == 0xFFFFu) { /* pid field == -1 */
        leafrepl_commit_cr0(ctx, cr0);
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 3;
    ctx->gpr[0] = ctx->gpr[4] + 0x10000u; /* addis r0,r4,1 */
    cr0 = leafrepl_cr0_cmp_u32(ctx, ctx->gpr[0], 0xFFFFu);
    if (ctx->gpr[0] == 0xFFFFu) { /* arg pid == -1 */
        leafrepl_commit_cr0(ctx, cr0);
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 3;
    ctx->gpr[0] = mem_read8_direct(ctx, ctx->gpr[3] + 12u); /* lbz */
    cr0 = leafrepl_cr0_cmp_u32(ctx, ctx->gpr[0], 0u);
    if (ctx->gpr[0] == 0u) {
        leafrepl_commit_cr0(ctx, cr0);
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    /* subf r0,r5,r4; cntlzw; rlwinm r3,r0,27,24,31 — no CR write, so the
     * lbz compare's CR0 (GT|SO) survives to the blr. */
    ctx->downcount -= 4;
    ctx->gpr[0] = ctx->gpr[4] - ctx->gpr[5]; /* subf r0,r5,r4 */
    {
        u32 n = 0;
        while (n < 32 && ((ctx->gpr[0] & (0x80000000u >> n)) == 0)) n++;
        ctx->gpr[0] = n; /* cntlzw r0,r0 */
    }
    ctx->gpr[3] = ((ctx->gpr[0] << 27) | (ctx->gpr[0] >> 5)) & 0xFFu;
    leafrepl_commit_cr0(ctx, cr0);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* dBgW::ChkGrpThrough (0x800A9684, generated chunk_0041/0042): group-mask
 * walk. r6!=2 or r5==0 -> 0. Otherwise chases
 * mask = *( *(*(r3+148) + 36) + r4*52 + 48 ) and tests it against the
 * passchk flag word *(r5+4): flag bit0 is checked only when
 * (mask & 0x80700) == 0; mask bits {0x100,0x200,0x400,0x80000} gate flag
 * bits {0x2,0x4,0x8,0x10}; a set gated flag -> 0, else 1. Loads go
 * through mem_read32_direct in guest order (the conditional *(r5+4)
 * reads happen on exactly the paths that take them). Live-outs: r0
 * (last masked result — 0 on the r3=1 exit), r3 (0/1), r4 (loaded mask
 * on main paths), CR0 = last and./rlwinm./cmpli, pc=lr. Per-block
 * charges mirror the generated bodies including the -1/-2 split across
 * the 800A96E0 chunk edge. */
static int leafrepl_chkgrpthrough(CPUState* ctx) {
    ctx->downcount -= 2; /* 800A9684: cmpwi r6,2 + bc */
    u32 cr0 = leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[6], 2);
    if ((s32)ctx->gpr[6] != 2) {
        leafrepl_commit_cr0(ctx, cr0);
        ctx->downcount -= 2; /* 800A9694: li r3,0 + blr */
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 2; /* 800A968C: cmplwi r5,0 + bc */
    cr0 = leafrepl_cr0_cmp_u32(ctx, ctx->gpr[5], 0u);
    if (ctx->gpr[5] == 0u) {
        leafrepl_commit_cr0(ctx, cr0);
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    /* Commit each register as its instruction retires (guest order), so a
     * mid-sequence load fault leaves the same partial gpr[0]/gpr[3]/gpr[4]
     * state the generated body would. */
    ctx->downcount -= 11; /* 800A969C: lwz/lwz/mulli/add/lwz/lis/addi/and./bc */
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 148u);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 36u);
    ctx->gpr[0] = (u32)((s64)(s32)ctx->gpr[4] * (s64)52); /* mulli r0,r4,52 */
    ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];
    ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[3] + 48u); /* lwz r4,48 */
    ctx->gpr[3] = 0x00080000u;                  /* lis r3,8 */
    ctx->gpr[0] = ctx->gpr[3] + 1792u;          /* addi r0,r3,1792 */
    ctx->gpr[0] = ctx->gpr[4] & ctx->gpr[0];    /* and. r0,r4,r0 */
    cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
    if (ctx->gpr[0] == 0u) {
        ctx->downcount -= 3; /* 800A96C0: lwz + rlwinm. + bc */
        ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[5] + 4u);
        ctx->gpr[0] &= 0x00000001u;
        cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
        if (ctx->gpr[0] != 0u) {
            leafrepl_commit_cr0(ctx, cr0);
            ctx->downcount -= 2; /* 800A96CC: li r3,0 + blr */
            ctx->gpr[3] = 0;
            ctx->pc = ctx->lr & ~3u;
            return 1;
        }
    }
    ctx->downcount -= 2; /* 800A96D4: rlwinm. r0,r4,0,23,23 + bc */
    ctx->gpr[0] = ctx->gpr[4] & 0x00000100u;
    cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
    if (ctx->gpr[0] != 0u) {
        ctx->downcount -= 1; /* 800A96DC: lwz r0,4(r5) */
        ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[5] + 4u);
        ctx->downcount -= 2; /* 800A96E0: rlwinm. r0&2 + bc */
        ctx->gpr[0] &= 0x00000002u;
        cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
        if (ctx->gpr[0] != 0u) {
            leafrepl_commit_cr0(ctx, cr0);
            ctx->downcount -= 2; /* 800A96E8 */
            ctx->gpr[3] = 0;
            ctx->pc = ctx->lr & ~3u;
            return 1;
        }
    }
    ctx->downcount -= 2; /* 800A96F0: rlwinm. r0,r4,0,22,22 + bc */
    ctx->gpr[0] = ctx->gpr[4] & 0x00000200u;
    cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
    if (ctx->gpr[0] != 0u) {
        ctx->downcount -= 3; /* 800A96F8: lwz + rlwinm.&4 + bc */
        ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[5] + 4u);
        ctx->gpr[0] &= 0x00000004u;
        cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
        if (ctx->gpr[0] != 0u) {
            leafrepl_commit_cr0(ctx, cr0);
            ctx->downcount -= 2; /* 800A9704 */
            ctx->gpr[3] = 0;
            ctx->pc = ctx->lr & ~3u;
            return 1;
        }
    }
    ctx->downcount -= 2; /* 800A970C: rlwinm. r0,r4,0,21,21 + bc */
    ctx->gpr[0] = ctx->gpr[4] & 0x00000400u;
    cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
    if (ctx->gpr[0] != 0u) {
        ctx->downcount -= 3; /* 800A9714: lwz + rlwinm.&8 + bc */
        ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[5] + 4u);
        ctx->gpr[0] &= 0x00000008u;
        cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
        if (ctx->gpr[0] != 0u) {
            leafrepl_commit_cr0(ctx, cr0);
            ctx->downcount -= 2; /* 800A9720 */
            ctx->gpr[3] = 0;
            ctx->pc = ctx->lr & ~3u;
            return 1;
        }
    }
    ctx->downcount -= 2; /* 800A9728: rlwinm. r0,r4,0,12,12 + bc */
    ctx->gpr[0] = ctx->gpr[4] & 0x00080000u;
    cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
    if (ctx->gpr[0] != 0u) {
        ctx->downcount -= 3; /* 800A9730: lwz + rlwinm.&0x10 + bc */
        ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[5] + 4u);
        ctx->gpr[0] &= 0x00000010u;
        cr0 = leafrepl_cr0_logic(ctx, ctx->gpr[0]);
        if (ctx->gpr[0] != 0u) {
            leafrepl_commit_cr0(ctx, cr0);
            ctx->downcount -= 2; /* 800A973C */
            ctx->gpr[3] = 0;
            ctx->pc = ctx->lr & ~3u;
            return 1;
        }
    }
    ctx->downcount -= 2; /* 800A9744: li r3,1 + blr */
    leafrepl_commit_cr0(ctx, cr0);
    ctx->gpr[3] = 1;
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* __ptmf_scall (0x80328DE8, generated chunk_0201): ptmf resolver.
 * r0=*(r12+0); r11=*(r12+4); r12=*(r12+8); r3+=r0; cmpwi r11,0 — r11<0
 * takes the direct branch (r12 stays the function ptr); r11>=0 chases
 * r12=*(*(r3+r12)+r11) through the vtable. mtctr r12; bctr.
 * Charges -6 / -2 / -3. Live-outs: r0, r3, r11, r12, ctr, CR0, pc=ctr&~3.
 */
static int leafrepl_ptmf_scall(CPUState* ctx) {
    /* Commit each register as its instruction retires (guest order), so a
     * mid-sequence fault leaves the same partial state the generated
     * body would. */
    ctx->downcount -= 6;
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[12]);      /* lwz r0,0(r12) */
    ctx->gpr[11] = mem_read32_direct(ctx, ctx->gpr[12] + 4u);/* lwz r11,4(r12) */
    ctx->gpr[12] = mem_read32_direct(ctx, ctx->gpr[12] + 8u);/* lwz r12,8(r12) */
    ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];                 /* add r3,r3,r0 */
    leafrepl_commit_cr0(ctx,
        leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[11], 0));    /* cmpwi r11,0 */
    if ((s32)ctx->gpr[11] >= 0) { /* bc 12,0 not taken -> vtable chase */
        ctx->downcount -= 2;
        ctx->gpr[12] = mem_read32_direct(ctx, ctx->gpr[3] + ctx->gpr[12]);
        ctx->gpr[12] = mem_read32_direct(ctx, ctx->gpr[12] + ctx->gpr[11]);
    }
    ctx->downcount -= 3; /* mtctr r12 + bctr */
    ctx->ctr = ctx->gpr[12];
    ctx->pc = ctx->ctr & ~3u;
    return 1;
}

/* PSVECAdd (0x8030DCE0) / PSVECSubtract (0x8030DD04): identical 9-instr
 * bodies apart from ps_add vs ps_sub (generated chunk_0195). Charge -9.
 * Live-outs: f2..f7 result/load lanes + fpscr, out vec at r5, pc=lr. */
static int leafrepl_psvecaddsub(CPUState* ctx, int subtract) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    ctx->downcount -= 9;
    leafrepl_psq_l(ctx, 2, r3 + 0u, 0);
    leafrepl_psq_l(ctx, 4, r4 + 0u, 0);
    if (subtract)
        ppc_ps_sub_op(ctx, 6, 2, 4);
    else
        ppc_ps_add_op(ctx, 6, 2, 4);
    leafrepl_psq_st(ctx, 6, r5 + 0u, 0);
    leafrepl_psq_l(ctx, 3, r3 + 8u, 1);
    leafrepl_psq_l(ctx, 5, r4 + 8u, 1);
    if (subtract)
        ppc_ps_sub_op(ctx, 7, 3, 5);
    else
        ppc_ps_add_op(ctx, 7, 3, 5);
    leafrepl_psq_st(ctx, 7, r5 + 8u, 1);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* PSVECScale (0x8030DD28): psq_l f0,0(r3); psq_l f2,8(r3),w1;
 * ps_muls0 f0,f0,f1; psq_st f0,0(r4); ps_muls0 f0,f2,f1;
 * psq_st f0,8(r4),w1; blr. Charge -7. Live-outs: f0/f2 + fpscr, out at
 * r4, pc=lr. */
static int leafrepl_psvecscale(CPUState* ctx) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    ctx->downcount -= 7;
    leafrepl_psq_l(ctx, 0, r3 + 0u, 0);
    leafrepl_psq_l(ctx, 2, r3 + 8u, 1);
    ppc_ps_muls0(ctx, 0, 0, 1);
    leafrepl_psq_st(ctx, 0, r4 + 0u, 0);
    ppc_ps_muls0(ctx, 0, 2, 1);
    leafrepl_psq_st(ctx, 0, r4 + 8u, 1);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* PSVECSquareDistance (0x8030E0B4): charge -10, no memory writes.
 * Live-outs: f0..f2 + fpscr, f1.ps0 = squared distance, pc=lr. */
static int leafrepl_psvecsquaredistance(CPUState* ctx) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    ctx->downcount -= 10;
    leafrepl_psq_l(ctx, 0, r3 + 4u, 0);
    leafrepl_psq_l(ctx, 1, r4 + 4u, 0);
    ppc_ps_sub_op(ctx, 2, 0, 1);
    leafrepl_psq_l(ctx, 0, r3 + 0u, 0);
    leafrepl_psq_l(ctx, 1, r4 + 0u, 0);
    ppc_ps_mul_op(ctx, 2, 2, 2);
    ppc_ps_sub_op(ctx, 0, 0, 1);
    ppc_ps_madd_op(ctx, 1, 0, 0, 2, false, false);
    ppc_ps_sum0(ctx, 1, 1, 2, 2);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* PSMTXMultVec (0x8030DA44): 4x3 (row-major 3x4 read as row pairs)
 * matrix * vec3 -> 3 f32s at r5+0/4/8 via w=1 psq_st. Charge -21.
 * Live-outs: f0..f6,f8..f12 lanes + fpscr, pc=lr. */
static int leafrepl_psmtxmultvec(CPUState* ctx) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    ctx->downcount -= 21;
    leafrepl_psq_l(ctx, 0, r4 + 0u, 0);  /* f0 = (v0, v1) */
    leafrepl_psq_l(ctx, 2, r3 + 0u, 0);  /* f2 = (m00, m01) */
    leafrepl_psq_l(ctx, 1, r4 + 8u, 1);  /* f1 = (v2, 1.0) */
    ppc_ps_mul_op(ctx, 4, 2, 0);         /* f4 = (m00*v0, m01*v1) */
    leafrepl_psq_l(ctx, 3, r3 + 8u, 0);  /* f3 = (m02, m03) */
    ppc_ps_madd_op(ctx, 5, 3, 1, 4, false, false); /* f5=(m02*v2+m00v0, m03+m01v1) */
    leafrepl_psq_l(ctx, 8, r3 + 16u, 0); /* f8 = (m10, m11) */
    ppc_ps_sum0(ctx, 6, 5, 6, 5);        /* f6 = (row0 dot) */
    leafrepl_psq_l(ctx, 9, r3 + 24u, 0); /* f9 = (m20, m21) */
    ppc_ps_mul_op(ctx, 10, 8, 0);        /* f10 = (m10*v0, m11*v1) */
    leafrepl_psq_st(ctx, 6, r5 + 0u, 1); /* out.x */
    ppc_ps_madd_op(ctx, 11, 9, 1, 10, false, false);
    leafrepl_psq_l(ctx, 2, r3 + 32u, 0); /* f2 = (m30, m31) */
    ppc_ps_sum0(ctx, 12, 11, 12, 11);    /* f12 = (row1 dot) */
    leafrepl_psq_l(ctx, 3, r3 + 40u, 0); /* f3 = (m32, m33) */
    ppc_ps_mul_op(ctx, 4, 2, 0);         /* f4 = (m30*v0, m31*v1) */
    leafrepl_psq_st(ctx, 12, r5 + 4u, 1);/* out.y */
    ppc_ps_madd_op(ctx, 5, 3, 1, 4, false, false);
    ppc_ps_sum0(ctx, 6, 5, 6, 5);        /* f6 = (row2 dot) */
    leafrepl_psq_st(ctx, 6, r5 + 8u, 1); /* out.z */
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* cXyz operator wrappers (generated chunk_0144/0145): 32-byte frame
 * (stwu/mflr/stw r0,36 /stw r31,28), operand regs shuffled into the
 * PSVEC* calling convention, the callee body inlined, then the temp vec
 * is re-read scalar-wise (lfs/stfs x3) into r31 and the frame torn down.
 * All stack traffic is replayed through mem_*_direct in guest order so
 * aliasing between r31, the spill slots and the temp vector stays
 * bit-exact. Per-op blocks and downcount blocks:
 *   __pl__ 0x80245674: -9 entry (..addi r5,r1,8; bl PSVECAdd), -9 callee
 *   __mi__ 0x802456C4: -7 entry, -2 (addi r5,r1,8; bl PSVECSubtract —
 *                      chunk_0144->0145 boundary), -9 callee
 *   __ml__Ff 0x80245714: -8 entry (..addi r4,r1,8; bl PSVECScale), -7
 *   __dv__ 0x802457A8: -26 entry (..addi r4,r1,8; lfs f0,-16744(r2);
 *                      fdivs f1,f0,f1; bl PSVECScale), -7
 *   all: -12 continuation (lfs/stfs x3, lwz r31/r0, mtlr, addi r1, blr)
 * Live-outs: r0=restored lr, r1 restored, r3/r4(/r5) post-shuffle, r31
 * restored, lr restored, f-regs per callee+epilogue, pc=lr. */
static int leafrepl_cxyz(CPUState* ctx, int op /*0=add,1=sub,2=muls,3=div*/) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    static const s32 entry_charge[4]  = { -9, -7, -8, -26 };
    static const s32 callee_charge[4] = { -9, -9, -7, -7 };
    ctx->downcount += entry_charge[op];
    /* Commit each register/memory effect in guest instruction order so a
     * mid-sequence fault leaves the same partial state the generated
     * body would (stwu, mflr, frame stores, operand shuffle). */
    {
        const u32 ea = ctx->gpr[1] - 32u;      /* stwu r1,-32(r1) */
        mem_write32_direct(ctx, ea, ctx->gpr[1]);
        ctx->gpr[1] = ea;
    }
    ctx->gpr[0] = ctx->lr;                      /* mflr r0 */
    mem_write32_direct(ctx, ctx->gpr[1] + 36u, ctx->gpr[0]);
    mem_write32_direct(ctx, ctx->gpr[1] + 28u, ctx->gpr[31]);
    ctx->gpr[31] = ctx->gpr[3];                 /* or r31,r3,r3 */
    ctx->gpr[3] = ctx->gpr[4];                  /* or r3,r4,r4 */
    if (op >= 2) {
        ctx->gpr[4] = ctx->gpr[1] + 8u; /* addi r4,r1,8 — PSVECScale dst */
        if (op == 3) {
            /* lfs f0,-16744(r2); fdivs f1,f0,f1 — inside the -26 */
            leafrepl_lfs_to(ctx, 0, ctx->gpr[2] - 16744u);
            ppc_fdivs(ctx, 1, 0, 1);
        }
    } else {
        ctx->gpr[4] = ctx->gpr[5];              /* or r4,r5,r5 */
        if (op == 1)
            ctx->downcount -= 2;   /* __mi__ chunk edge: addi+bl */
        ctx->gpr[5] = ctx->gpr[1] + 8u;         /* addi r5,r1,8 */
    }
    /* The bl writes lr before the callee body runs; the epilogue's
     * mtlr restores it from the frame. */
    static const u32 bl_ret[4] = { 0x80245698u, 0x802456E8u,
                                   0x80245734u, 0x802457D0u };
    ctx->lr = bl_ret[op];
    ctx->downcount += callee_charge[op];
    if (op >= 2) {
        /* PSVECScale(r3, f1, r4) inline */
        leafrepl_psq_l(ctx, 0, ctx->gpr[3] + 0u, 0);
        leafrepl_psq_l(ctx, 2, ctx->gpr[3] + 8u, 1);
        ppc_ps_muls0(ctx, 0, 0, 1);
        leafrepl_psq_st(ctx, 0, ctx->gpr[4] + 0u, 0);
        ppc_ps_muls0(ctx, 0, 2, 1);
        leafrepl_psq_st(ctx, 0, ctx->gpr[4] + 8u, 1);
    } else {
        /* PSVECAdd/PSVECSubtract(r3, r4, r5) inline */
        leafrepl_psq_l(ctx, 2, ctx->gpr[3] + 0u, 0);
        leafrepl_psq_l(ctx, 4, ctx->gpr[4] + 0u, 0);
        if (op == 1)
            ppc_ps_sub_op(ctx, 6, 2, 4);
        else
            ppc_ps_add_op(ctx, 6, 2, 4);
        leafrepl_psq_st(ctx, 6, ctx->gpr[5] + 0u, 0);
        leafrepl_psq_l(ctx, 3, ctx->gpr[3] + 8u, 1);
        leafrepl_psq_l(ctx, 5, ctx->gpr[4] + 8u, 1);
        if (op == 1)
            ppc_ps_sub_op(ctx, 7, 3, 5);
        else
            ppc_ps_add_op(ctx, 7, 3, 5);
        leafrepl_psq_st(ctx, 7, ctx->gpr[5] + 8u, 1);
    }
    ctx->downcount -= 12; /* continuation: lfs/stfs x3 + frame teardown */
    leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 8u);
    leafrepl_stfs(ctx, 0, ctx->gpr[31] + 0u);
    leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 12u);
    leafrepl_stfs(ctx, 0, ctx->gpr[31] + 4u);
    leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 16u);
    leafrepl_stfs(ctx, 0, ctx->gpr[31] + 8u);
    ctx->gpr[31] = mem_read32_direct(ctx, ctx->gpr[1] + 28u); /* lwz r31 */
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[1] + 36u);  /* lwz r0 */
    ctx->lr = ctx->gpr[0];                                  /* mtlr r0 */
    ctx->gpr[1] = ctx->gpr[1] + 32u;                        /* addi r1,32 */
    ctx->pc = ctx->lr & ~3u;                                /* blr */
    return 1;
}

/* cM3d_Cross_AabCyl (0x8024A8E0, generated chunk_0146): six fcmpo early
 * outs against cyl extent +/- radius, then r3 = !(f0 < f2) extracted
 * through mfcr/cntlzw/rlwinm. Every fadds/fsubs/fcmpo runs the real
 * helpers so fpscr is exact. Charges -6/-4/-5/-4/-6/-7 per block plus -2
 * on each li/blr early-out. Live-outs: f0..f3 lanes, r0 (final path),
 * r3, CR0, fpscr, pc=lr. */
static int leafrepl_cross_aabcyl(CPUState* ctx) {
    if (!leafrepl_fp_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    ctx->downcount -= 6;
    leafrepl_lfs_to(ctx, 1, r3 + 0u);
    leafrepl_lfs_to(ctx, 2, r4 + 0u);
    leafrepl_lfs_to(ctx, 3, r4 + 12u);
    ppc_fadds(ctx, 0, 2, 3);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], true);
    if (ctx->cr & 0x40000000u) { /* f1 > f2+f3 -> bc 4,1 not taken -> 0 */
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 4;
    leafrepl_lfs_to(ctx, 1, r3 + 12u);
    ppc_fsubs(ctx, 0, 2, 3);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], true);
    if (ctx->cr & 0x80000000u) { /* f1 < f2-f3 -> 0 */
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 5;
    leafrepl_lfs_to(ctx, 1, r3 + 8u);
    leafrepl_lfs_to(ctx, 2, r4 + 8u);
    ppc_fadds(ctx, 0, 2, 3);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], true);
    if (ctx->cr & 0x40000000u) {
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 4;
    leafrepl_lfs_to(ctx, 1, r3 + 20u);
    ppc_fsubs(ctx, 0, 2, 3);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], true);
    if (ctx->cr & 0x80000000u) {
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 6;
    leafrepl_lfs_to(ctx, 1, r3 + 4u);
    leafrepl_lfs_to(ctx, 2, r4 + 4u);
    leafrepl_lfs_to(ctx, 0, r4 + 16u);
    ppc_fadds(ctx, 0, 2, 0);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], true);
    if (ctx->cr & 0x40000000u) {
        ctx->downcount -= 2;
        ctx->gpr[3] = 0;
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 7;
    leafrepl_lfs_to(ctx, 0, r3 + 16u);
    ppc_fcmp(ctx, 0, ctx->fpr[0], ctx->fpr[2], true);
    /* mfcr r0; rlwinm r0,r0,1,31,31 -> r0 = cr0.LT; cntlzw; rlwinm
     * r3,r0,27,5,31 -> r3 = (r0==32) = !LT = !(f0 < f2) || UN */
    u32 r0 = ctx->cr;
    r0 = ((r0 << 1) | (r0 >> 31)) & 1u;
    {
        u32 n = 0;
        while (n < 32 && ((r0 & (0x80000000u >> n)) == 0)) n++;
        r0 = n;
    }
    ctx->gpr[0] = r0;
    ctx->gpr[3] = ((r0 << 27) | (r0 >> 5)) & 0x07FFFFFFu;
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* DCInvalidateRange (0x803030AC, generated chunk_0192 label_803030AC):
 *     cmplwi r4,0 / blelr / r5 = r3 & 0x1F / r4 = (r4+r5+31) >> 5 /
 *     mtctr r4 / loop{ dcbi 0,r3; r3 += 32; bdnz } / blr.
 * The per-line dcbi reaches ppc_fallback_instruction -> ctx->cache_control
 * (the chassis HookCacheControl): with the port's coherent host RAM the op
 * itself is a semantic no-op — its only guest-visible costs are the hook's
 * 5-cycle PPCTables debit and the generated loop's -2/iter downcount block,
 * both folded into the single charge below so a claim bills exactly what
 * the generated path would (entry -2 + align -6 + 7/line + blr -1 =
 * -(9 + 7*count); early-out -2). The generated body writes no guest memory.
 * Live-outs: CR0 = cmplwi r4,0 (EQ|SO early-out, GT|SO otherwise — the loop
 * leaves CR untouched); r5 = r3 & 0x1F; r4 = count; r3 += 32*count;
 * ctr = 0; pc = lr & ~3.
 * Decline gates (the generated body then runs identically):
 *  - ctx->exception pre-set: the fb-fallthrough checks !ctx->exception and
 *    would bail with partial loop state.
 *  - ctx->downcount > 0: a residual of exactly +13 (+13+7k deeper in the
 *    loop) makes an fb-fallthrough's `downcount != 0` bail mid-body;
 *    positive residuals never occur, but decline keeps even that edge
 *    exact.
 *  - MSR.PR: dcbi is privileged — Interpreter::dcbi raises a program
 *    exception in user mode (the hook slow path); the repl must not
 *    swallow it.
 *  - computed count == 0 with r4 != 0 (u32 wrap of r4+(r3&0x1F)+31):
 *    mtctr 0 would run a ~2^32-iteration guest loop — decline keeps the
 *    pathological semantics.
 *  - cache_control not installed: with no hook, ppc_fallback_instruction
 *    routes each dcbi to instruction_fallback (opaque to us) or raises
 *    illegal — the repl can only stand in for the known hook path.
 * The -256 back-edge yield inside the generated loop is a dispatch
 * scheduling artifact; the claim bills the identical total in one shot
 * (same guest-time accounting — cf. the judgerepl walk's atomic collapse),
 * and no guest-observable state exists mid-loop beyond the billing. */
static int leafrepl_dcinvalidate(CPUState* ctx) {
    const u32 r4_in = ctx->gpr[4];
    u32 count = 0u;
    if (ctx->exception || ctx->downcount > 0 ||
        (ctx->msr & PPC_MSR_PR) != 0u || ctx->cache_control == 0)
        return 0;
    if (r4_in != 0u) {
        count = (u32)(r4_in + (ctx->gpr[3] & 0x1Fu) + 31u) >> 5;
        if (count == 0u)
            return 0;
    }
    ctx->downcount -= 2;                          /* cmplwi r4,0 + blelr */
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_u32(ctx, r4_in, 0u));
    if (r4_in == 0u) {
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 6;                          /* rlwinm/add/addi/rlwinm/mtctr */
    ctx->gpr[5] = ctx->gpr[3] & 0x1Fu;
    ctx->gpr[4] = count;
    ctx->gpr[3] += count << 5;                    /* addi r3,32 x count */
    ctx->ctr = 0u;                                /* bdnz ran ctr -> 0 */
    ctx->downcount -= (s64)count * 7;             /* -2 loop block + -5 hook/line */
    ctx->downcount -= 1;                          /* blr */
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* DCFlushRange (0x803030D8, generated chunk_0192 label_803030D8): identical
 * loop with dcbf per line, then a trailing `sc` at 0x80303100 before the
 * blr at 0x80303104 — the generated tail charges -2 and calls
 * ppc_system_call_exception (srr0 = 0x80303104, srr1 = old_msr & RFI_MASK,
 * exception |= SYSCALL, msr/pc = exception state). The repl reproduces the
 * raise verbatim; the handler's rfi lands on the blr as usual. dcbf is
 * unprivileged, so there is no MSR.PR gate. Charge: -(10 + 7*count)
 * (-2 entry + -6 align + 7/line + -2 sc), early-out -2. Same remaining
 * gates as leafrepl_dcinvalidate. */
static int leafrepl_dcflush(CPUState* ctx) {
    const u32 r4_in = ctx->gpr[4];
    u32 count = 0u;
    if (ctx->exception || ctx->downcount > 0 || ctx->cache_control == 0)
        return 0;
    if (r4_in != 0u) {
        count = (u32)(r4_in + (ctx->gpr[3] & 0x1Fu) + 31u) >> 5;
        if (count == 0u)
            return 0;
    }
    ctx->downcount -= 2;                          /* cmplwi r4,0 + blelr */
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_u32(ctx, r4_in, 0u));
    if (r4_in == 0u) {
        ctx->pc = ctx->lr & ~3u;
        return 1;
    }
    ctx->downcount -= 6;                          /* rlwinm/add/addi/rlwinm/mtctr */
    ctx->gpr[5] = ctx->gpr[3] & 0x1Fu;
    ctx->gpr[4] = count;
    ctx->gpr[3] += count << 5;
    ctx->ctr = 0u;
    ctx->downcount -= (s64)count * 7;
    ctx->downcount -= 2;                          /* sc */
    ppc_system_call_exception(ctx, 0x80303100u);
    return 1;
}

/* The MultVecArray loop repl keeps the generated back-edge budget check
 * verbatim; in the module build DOLRECOMP_C_LOOP_CYCLE_BUDGET resolves to
 * dolrecomp_cycle_budget via the -include'd cycle_budget.h (same as the
 * generated chunks); the literal fallback matches generated.h's default
 * for any TU that compiles rel_loader.c without the force-include. */
#ifndef DOLRECOMP_C_LOOP_CYCLE_BUDGET
#define DOLRECOMP_C_LOOP_CYCLE_BUDGET 256
#endif

/* J3DFifoLoadPosMtxImm (0x802D8BD8, generated chunk_0181): pushes one
 * XF position-matrix packet to the GX FIFO register at 0xCC008000
 * (r5 = 0xCC010000, all stores at -32768(r5)). Packet: u8 0x10,
 * u16 11, u16 (rotl32(r4,2)&0xFFFC), then the 12 matrix words
 * r3+0..44. Pure integer + mem ops — no fp/psq gates exist in the
 * generated body, so the claim is unconditional; the stb/sth/stw order
 * and widths are the FIFO stream framing and are reproduced verbatim.
 * Charge -32. Live-outs: r0 = last word, r5 = 0xCC010000; r3/r4 kept. */
static int leafrepl_j3dfifo_posmtx(CPUState* ctx) {
    const u32 r3 = ctx->gpr[3];
    ctx->downcount -= 32;
    ctx->gpr[0] = (u32)(s32)(16);                       /* li r0,16 */
    ctx->gpr[5] = ((u32)(s32)(52225) << 16);            /* lis r5,-13311 */
    mem_write8_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                      (u8)ctx->gpr[0]);                 /* stb -32768(r5) */
    ctx->gpr[0] = (u32)(s32)(11);                       /* li r0,11 */
    mem_write16_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                       (u16)ctx->gpr[0]);               /* sth -32768(r5) */
    ctx->gpr[0] = ((ctx->gpr[4] << 2) | (ctx->gpr[4] >> 30)) &
                  0x0000FFFCu;                          /* rlwinm r0,r4,2,16,29 */
    mem_write16_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                       (u16)ctx->gpr[0]);               /* sth -32768(r5) */
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 0u);      /* lwz r0,0(r3) */
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 4u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 8u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 12u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 16u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 20u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 24u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 28u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 32u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 36u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 40u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 44u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->pc = ctx->lr & ~3u;                            /* blr */
    return 1;
}

/* J3DFifoLoadNrmMtxImm (0x802D8C58): same FIFO header except the u16
 * count field is 8 and the index field is r4*3+1024 (r4 *= 3 commits
 * BEFORE the stores — a live-out). Packet body is the 9 normal-matrix
 * words at the non-contiguous offsets r3+{0,4,8,16,20,24,32,36,40}
 * (the 3x3 row stride skips +12). Charge -29. */
static int leafrepl_j3dfifo_nrmmtx(CPUState* ctx) {
    const u32 r3 = ctx->gpr[3];
    ctx->downcount -= 29;
    ctx->gpr[0] = (u32)(s32)(16);
    ctx->gpr[5] = ((u32)(s32)(52225) << 16);
    mem_write8_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                      (u8)ctx->gpr[0]);
    ctx->gpr[0] = (u32)(s32)(8);                        /* li r0,8 */
    mem_write16_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                       (u16)ctx->gpr[0]);
    ctx->gpr[4] = (u32)((s64)(s32)ctx->gpr[4] * (s64)(s32)3); /* mulli r4,r4,3 */
    ctx->gpr[0] = ctx->gpr[4] + (u32)(s32)(1024);       /* addi r0,r4,1024 */
    mem_write16_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                       (u16)ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 0u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 4u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 8u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 16u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 20u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 24u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 32u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 36u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 40u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* J3DFifoLoadNrmMtxImm3x3 (0x802D8CC4): identical to nrmmtx except the
 * matrix source is packed 3x3 — the 9 words are contiguous at
 * r3+{0,4,8,12,16,20,24,28,32}. Same -29 charge and live-outs. */
static int leafrepl_j3dfifo_nrm3x3(CPUState* ctx) {
    const u32 r3 = ctx->gpr[3];
    ctx->downcount -= 29;
    ctx->gpr[0] = (u32)(s32)(16);
    ctx->gpr[5] = ((u32)(s32)(52225) << 16);
    mem_write8_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                      (u8)ctx->gpr[0]);
    ctx->gpr[0] = (u32)(s32)(8);
    mem_write16_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                       (u16)ctx->gpr[0]);
    ctx->gpr[4] = (u32)((s64)(s32)ctx->gpr[4] * (s64)(s32)3);
    ctx->gpr[0] = ctx->gpr[4] + (u32)(s32)(1024);
    mem_write16_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768),
                       (u16)ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 0u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 4u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 8u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 12u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 16u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 20u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 24u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 28u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, r3 + 32u);
    mem_write32_direct(ctx, ctx->gpr[5] + (u32)(s32)(-32768), ctx->gpr[0]);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* psq_lu/psq_stu update forms for MultVecArray's walk: ea = base+off,
 * the load/store runs the same type-0 fast path as leafrepl_psq_*
 * (valid under the entry gates), then base = ea. Under the entry gates
 * the fast path can never raise, so the generated post-access exception
 * check can't fire and the register update always commits. */
static void leafrepl_psq_lu(CPUState* ctx, u8 d, u8 base, s32 off, int w) {
    const u32 ea = ctx->gpr[base] + (u32)off;
    leafrepl_psq_l(ctx, d, ea, w);
    ctx->gpr[base] = ea;
}
static void leafrepl_psq_stu(CPUState* ctx, u8 s, u8 base, s32 off, int w) {
    const u32 ea = ctx->gpr[base] + (u32)off;
    leafrepl_psq_st(ctx, s, ea, w);
    ctx->gpr[base] = ea;
}

/* PSMTXMultVecArray (0x8030DA98, generated chunk_0195): r3 = 3x4 matrix,
 * r4 = src vec array, r5 = dst vec array, r6 = count. The generated body
 * preloads the matrix into f0..f5 ((m00,m01),(m10,m11),(m02,m03),(m12,m13),
 * (m20,m21),(m22,m23) via merges of f13/f12/f11/f10/f4/f5), walks the
 * vectors with update-form psq (r4 advances +4/+8 per element, r5
 * advances +4/+8 per store), and runs a bdnz loop over r6-1 inner
 * iterations with the epilogue writing the last vector. The loop
 * back-edge keeps the generated budget yield verbatim: on
 * downcount <= -BUDGET it returns with pc = 0x8030DAE4 (the loop-head
 * continuation label, which stays a generated dispatch pc — only the
 * function entry is claimed). Charges: -20 entry, -11 per bdnz trip,
 * -5 epilogue. Live-outs: r4/r5 advanced, r6 decremented, ctr, f0..f13
 * lanes + fpscr, dst array, pc = lr (or 0x8030DAE4 on yield). */
static int leafrepl_psmtxmultvecarray(CPUState* ctx) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3];
    ctx->downcount -= 20;
    leafrepl_psq_l(ctx, 13, r3 + 0u, 0);   /* f13 = (m00, m01) */
    leafrepl_psq_l(ctx, 12, r3 + 16u, 0);  /* f12 = (m10, m11) */
    ctx->gpr[6] = ctx->gpr[6] + (u32)(s32)(-1); /* addi r6,r6,-1 */
    leafrepl_psq_l(ctx, 11, r3 + 8u, 0);   /* f11 = (m02, m03) */
    { const f64 ps0 = ctx->fpr[13], ps1 = ctx->fpr[12]; /* ps_merge00 f0 */
      ctx->fpr[0] = ps0; ctx->ps1[0] = ps1; }
    ctx->gpr[5] = ctx->gpr[5] + (u32)(s32)(-4); /* addi r5,r5,-4 */
    leafrepl_psq_l(ctx, 10, r3 + 24u, 0);  /* f10 = (m12, m13) */
    { const f64 ps0 = ctx->ps1[13], ps1 = ctx->ps1[12]; /* ps_merge11 f1 */
      ctx->fpr[1] = ps0; ctx->ps1[1] = ps1; }
    ctx->ctr = ctx->gpr[6];                /* mtctr r6 */
    leafrepl_psq_l(ctx, 4, r3 + 32u, 0);   /* f4 = (m20, m21) */
    { const f64 ps0 = ctx->fpr[11], ps1 = ctx->fpr[10]; /* ps_merge00 f2 */
      ctx->fpr[2] = ps0; ctx->ps1[2] = ps1; }
    leafrepl_psq_l(ctx, 5, r3 + 40u, 0);   /* f5 = (m22, m23) */
    { const f64 ps0 = ctx->ps1[11], ps1 = ctx->ps1[10]; /* ps_merge11 f3 */
      ctx->fpr[3] = ps0; ctx->ps1[3] = ps1; }
    leafrepl_psq_l(ctx, 6, ctx->gpr[4] + 0u, 0);        /* f6 = (v0, v1) */
    leafrepl_psq_lu(ctx, 7, 4, 8, 1);                   /* f7 = (v2, 1.0) */
    ppc_ps_madds0(ctx, 8, 0, 6, 3);        /* f8 = m0*v.ps0 + m03 */
    ppc_ps_mul_op(ctx, 9, 4, 6);           /* f9 = m2*v */
    ppc_ps_madds1(ctx, 8, 1, 6, 8);        /* f8 += m1*v.ps1 */
    ppc_ps_madd_op(ctx, 10, 5, 7, 9, false, false); /* f10 = m2row*v7 + f9 */
    for (;;) {
        ctx->downcount -= 11;              /* loop-head block charge */
        leafrepl_psq_lu(ctx, 6, 4, 4, 0);  /* next (v0, v1); r4 += 4 */
        ppc_ps_madds0(ctx, 12, 2, 7, 8);   /* f12 = m1*v7 + f8 */
        leafrepl_psq_lu(ctx, 7, 4, 8, 1);  /* next (v2, 1.0); r4 += 8 */
        ppc_ps_sum0(ctx, 13, 10, 9, 10);   /* f13 = row2 dot prev */
        ppc_ps_madds0(ctx, 8, 0, 6, 3);    /* f8 = m0*v.ps0 + m03 */
        ppc_ps_mul_op(ctx, 9, 4, 6);       /* f9 = m2*v */
        leafrepl_psq_stu(ctx, 12, 5, 4, 0);/* store (x, y); r5 += 4 */
        ppc_ps_madds1(ctx, 8, 1, 6, 8);    /* f8 += m1*v.ps1 */
        leafrepl_psq_stu(ctx, 13, 5, 8, 1);/* store z; r5 += 8 */
        ppc_ps_madd_op(ctx, 10, 5, 7, 9, false, false);
        ctx->ctr--;                        /* bdnz */
        if (ctx->ctr == 0u)
            break;
        if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) {
            ctx->pc = 0x8030DAE4u;         /* loop-head continuation */
            return 1;
        }
    }
    ctx->downcount -= 5;                   /* epilogue block */
    ppc_ps_madds0(ctx, 12, 2, 7, 8);
    ppc_ps_sum0(ctx, 13, 10, 9, 10);
    leafrepl_psq_stu(ctx, 12, 5, 4, 0);
    leafrepl_psq_stu(ctx, 13, 5, 8, 1);
    ctx->pc = ctx->lr & ~3u;               /* blr */
    return 1;
}

/* PSMTXMultVecSR (0x8030DB24, generated chunk_0195): same product as
 * PSMTXMultVec but single-rate — 3x4 matrix at r3 (row pairs), vec at
 * r4, 3 f32s out at r5 via w=1 psq_st. Charge -21. Live-outs:
 * f0..f13 lanes + fpscr, pc = lr. Same leaf decline gates as
 * leafrepl_psmtxmultvec. */
static int leafrepl_psmtxmultvecsr(CPUState* ctx) {
    if (!leafrepl_fp_ok(ctx) || !leafrepl_gqr0_ps_ok(ctx) || ctx->exception)
        return 0;
    const u32 r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    ctx->downcount -= 21;
    leafrepl_psq_l(ctx, 0, r3 + 0u, 0);    /* f0 = (m00, m01) */
    leafrepl_psq_l(ctx, 6, r4 + 0u, 0);    /* f6 = (v0, v1) */
    leafrepl_psq_l(ctx, 2, r3 + 16u, 0);   /* f2 = (m10, m11) */
    ppc_ps_mul_op(ctx, 8, 0, 6);           /* f8 = m0 * v01 */
    leafrepl_psq_l(ctx, 4, r3 + 32u, 0);   /* f4 = (m20, m21) */
    ppc_ps_mul_op(ctx, 10, 2, 6);          /* f10 = m1 * v01 */
    leafrepl_psq_l(ctx, 7, r4 + 8u, 1);    /* f7 = (v2, 1.0) */
    ppc_ps_mul_op(ctx, 12, 4, 6);          /* f12 = m2 * v01 */
    leafrepl_psq_l(ctx, 3, r3 + 24u, 0);   /* f3 = (m12, m13) */
    ppc_ps_sum0(ctx, 8, 8, 8, 8);          /* f8.ps0 = row0 pair dot */
    leafrepl_psq_l(ctx, 5, r3 + 40u, 0);   /* f5 = (m22, m23) */
    ppc_ps_sum0(ctx, 10, 10, 10, 10);      /* f10.ps0 = row1 pair dot */
    leafrepl_psq_l(ctx, 1, r3 + 8u, 0);    /* f1 = (m02, m03) */
    ppc_ps_sum0(ctx, 12, 12, 12, 12);      /* f12.ps0 = row2 pair dot */
    ppc_ps_madd_op(ctx, 9, 1, 7, 8, false, false);   /* f9 = row0 result */
    leafrepl_psq_st(ctx, 9, r5 + 0u, 1);   /* out.x */
    ppc_ps_madd_op(ctx, 11, 3, 7, 10, false, false); /* f11 = row1 result */
    leafrepl_psq_st(ctx, 11, r5 + 4u, 1);  /* out.y */
    ppc_ps_madd_op(ctx, 13, 5, 7, 12, false, false); /* f13 = row2 result */
    leafrepl_psq_st(ctx, 13, r5 + 8u, 1);  /* out.z */
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* In-module replacements for the process-list judge walkers:
 *   cNdIt_Judge (0x80244F44) — node_class list walk calling judge(node,ud)
 *     per node, returning the judge's first nonzero result
 *     (tww SComponent/c_node_iter.cpp:27; generated chunk_0144
 *     label_80244F44).
 *   cLsIt_Judge (0x80244C28) — node_list_class gate: mSize>0 tailcalls
 *     cNdIt_Judge(mpHead, judge, ud), else NULL (c_list_iter.cpp:19).
 * The mod-side host-call version of the node walk claims ~18K calls/s
 * through a chassis round-trip (frame60-accum mod.c on_cNdIt_Judge); claiming
 * in the dispatch switch costs only the case hit + the native walk. The
 * walkers and the leaf judges are all pure reads — nothing guest-visible is
 * written.
 *
 * Guard: the walk is only reproduced when the judge is one of the known
 * leaf comparators — fpcSch_JudgeForPName (0x80040050: lha node->f8 vs
 * lha *ud, cmpw), fpcSch_JudgeByID (0x80040068: lwz node->f4 vs lwz *ud,
 * cmplw), or cTgIt_JudgeFilter (0x80245640: reads node->mpTagData @+0xC and
 * calls filter->mpJudgeFunc(tag, filter->mpUserData) — claimed only when
 * that inner judge is itself ForPName/ByID). Both leaf judges return their
 * r3 argument (the candidate) on match and 0 on miss, so the walk returns
 * the node directly, or rd32(node+0xC) under the filter. Any other judge,
 * or a ud/iud outside MEM1, declines so the guest body runs.
 *
 * Field offsets (c_node.h / c_list.h / the generated bodies):
 *   node_class: +0x08 mpNextNode — fetched BEFORE each node's judge
 *     evaluation exactly like the original's r31 pipeline; +0x0C
 *     create_tag->mpTagData (filter candidate).
 *   node_list_class: +0x00 mpHead, +0x08 mSize.
 *   ForPName field = +8 (s16), ByID field = +4 (u32); wanted = *iud.
 * All guest reads are bounds-checked against MEM1 (0x80000000..0x81800000);
 * any out-of-bounds read or a walk over JUDGEREPL_WALK_CAP nodes returns
 * 0xFFFFFFFF -> decline -> the original runs (and takes its own exception
 * path on corrupt state). NULL node0 returns a 0 result.
 *
 * On claim: r3 = result; CR0 = (r==0 ? EQ : GT) | XER.SO — the original's
 * last CR0 write is `cmplwi r3,0` (unsigned: EQ or GT); pc = lr; downcount
 * -= the generated entry charge (5 for 0x80244F44 / 6 for 0x80244C28), billed
 * only on the claim path — declines re-enter the guest body, which bills
 * itself. The original's callee-saves (r29-r31 via the inlined savegpr
 * stub, the stwu/mflr frame) are restored before its blr, and r0/r4/r5/
 * r11/r12/ctr are volatile, so the claim path reproduces none of them —
 * only r3, CR0 and pc are architecturally visible. MODERNGEKKO_REPL_JUDGE=0
 * disables. */
static int s_repl_judge = 1;

#define JUDGEREPL_FILTER    0x80245640u /* cTgIt_JudgeFilter */
#define JUDGEREPL_FOR_PNAME 0x80040050u /* fpcSch_JudgeForPName */
#define JUDGEREPL_BY_ID     0x80040068u /* fpcSch_JudgeByID */
#define JUDGEREPL_WALK_CAP  8192u /* lists run ~hundreds; cap guards cycles */
#define JUDGEREPL_MEM1_TOP  0x01800000u
#define JUDGEREPL_IN_MEM1(a) ((u32)((a) - 0x80000000u) < JUDGEREPL_MEM1_TOP)

/* Sized MEM1 read gate (the mod hook's in_ram): the whole ea..ea+n span
 * must lie inside both the fixed MEM1 window and the live RAM buffer.
 * A miss declines — the guest body then takes whatever path its own loads
 * produce (MMIO fallback / DSI), preserving exact semantics. */
static inline int judgerepl_mapped(u32 ea, u32 n) {
    const u32 off = ea - 0x80000000u;
    return s_ram && s_ram_size >= n && off <= s_ram_size - n &&
           off <= JUDGEREPL_MEM1_TOP - n;
}
static inline u32 judgerepl_rd32(u32 ea) {
    return read_be32(s_ram + (ea - 0x80000000u));
}
static inline u32 judgerepl_rd16(u32 ea) {
    return read_be16(s_ram + (ea - 0x80000000u));
}

/* The walk proper: returns the matched candidate (the node, or its tag data
 * under the filter), 0 on list exhaustion, 0xFFFFFFFF on anomaly (decline).
 * next is read before each node's judge evaluation, matching the original. */
static u32 judgerepl_walk(u32 node, int via_filter, int is_name,
                          u32 wanted, u32 foff) {
    u32 next;
    u32 it = 0;
    if (node && !judgerepl_mapped(node + 8u, 4u))
        return 0xFFFFFFFFu;
    next = node ? judgerepl_rd32(node + 8u) : 0u;
    while (node) {
        u32 cand = node;
        u32 field_ea;
        u32 fv;
        if (++it > JUDGEREPL_WALK_CAP)
            return 0xFFFFFFFFu;
        if (!judgerepl_mapped(node + 0x0Cu, 4u))
            return 0xFFFFFFFFu;
        if (via_filter) {
            cand = judgerepl_rd32(node + 0x0Cu); /* create_tag->mpTagData */
            field_ea = cand + foff;
        } else {
            field_ea = node + foff;
        }
        if (!judgerepl_mapped(field_ea, is_name ? 2u : 4u))
            return 0xFFFFFFFFu;
        fv = is_name ? (u32)(s16)judgerepl_rd16(field_ea)
                     : judgerepl_rd32(field_ea);
        if (fv == wanted)
            return cand;
        node = next;
        if (node && !judgerepl_mapped(node + 8u, 4u))
            return 0xFFFFFFFFu;
        next = node ? judgerepl_rd32(node + 8u) : 0u;
    }
    return 0u;
}

/* Shared claim once node0 is known: resolves the judge guard, runs the
 * walk, and on success writes r3/CR0/pc and bills entry_charge (the
 * generated body's entry downcount decrement). Returns 1 claimed / 0
 * decline. */
static int judgerepl_claim(CPUState* ctx, u32 node0, s64 entry_charge) {
    const u32 judge = ctx->gpr[4];
    const u32 ud = ctx->gpr[5];
    u32 inner, iud, wanted, foff, r, cr0;
    int via_filter, is_name;
    if (judge == JUDGEREPL_FILTER) {
        if (!JUDGEREPL_IN_MEM1(ud) ||
            !judgerepl_mapped(ud, 4u) || !judgerepl_mapped(ud + 4u, 4u))
            return 0;
        inner = judgerepl_rd32(ud);        /* filter->mpJudgeFunc */
        iud   = judgerepl_rd32(ud + 4u);   /* filter->mpUserData */
        if ((inner != JUDGEREPL_BY_ID && inner != JUDGEREPL_FOR_PNAME) ||
            !JUDGEREPL_IN_MEM1(iud))
            return 0;
        via_filter = 1;
    } else if (judge == JUDGEREPL_BY_ID || judge == JUDGEREPL_FOR_PNAME) {
        inner = judge;
        iud = ud;
        if (!JUDGEREPL_IN_MEM1(iud))
            return 0;
        via_filter = 0;
    } else {
        return 0;
    }
    /* ForPName compares s16 (lha/cmpw), ByID u32 (lwz/cmplw); equality is
     * width-preserving under sign-extension, so comparing the sign-extended
     * s16 (or raw u32) reproduces both branches exactly. */
    is_name = (inner == JUDGEREPL_FOR_PNAME);
    foff = is_name ? 8u : 4u;
    if (!judgerepl_mapped(iud, is_name ? 2u : 4u))
        return 0;
    wanted = is_name ? (u32)(s16)judgerepl_rd16(iud) : judgerepl_rd32(iud);
    r = judgerepl_walk(node0, via_filter, is_name, wanted, foff);
    if (r == 0xFFFFFFFFu)
        return 0;
    cr0 = (r == 0u) ? 0x2u : 0x4u;
    cr0 |= (ctx->xer >> 31) & 1u;
    ctx->downcount -= entry_charge;
    ctx->gpr[3] = r;
    ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr0 << 28);
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* cLsIt_Judge (0x80244C28): the guest tailcall to cNdIt_Judge is an
 * intra-chunk goto, so claiming here reproduces both the mSize gate and
 * (via judgerepl_claim) the walk. */
static int judgerepl_ls(CPUState* ctx) {
    const u32 plist = ctx->gpr[3];
    s32 msize;
    u32 node0;
    if (!judgerepl_mapped(plist + 8u, 4u))
        return 0;
    msize = (s32)judgerepl_rd32(plist + 8u);      /* pList->mSize */
    if (msize > 0) {
        if (!judgerepl_mapped(plist, 4u))
            return 0;
        node0 = judgerepl_rd32(plist);            /* pList->mpHead */
        return judgerepl_claim(ctx, node0, 6);
    }
    /* NULL-return path: CR0 is the `cmpwi r0,0` on mSize — LT when
     * negative, EQ when zero (GT is unreachable: the NULL branch requires
     * mSize <= 0). */
    {
        u32 cr0 = (msize < 0) ? 0x8u : 0x2u;
        cr0 |= (ctx->xer >> 31) & 1u;
        ctx->downcount -= 6;
        ctx->gpr[3] = 0u;
        ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr0 << 28);
        ctx->pc = ctx->lr & ~3u;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * J3DMtxCalc{Basic,Softimage,Maya}::calcTransform fused bodies
 * (0x802F5090 / 0x802F52BC / 0x802F5508; MODERNGEKKO_REPL_J3D, default
 * on). These are the per-joint vfunc calls hit once per joint per
 * frame; fusing each body removes three dispatch round-trips (the bl
 * callees plus the inlined gpr save/restore stubs) and ~90-140 guest
 * instructions per call.
 *
 * Each replfn is a verbatim transcription of the dol-inline-opt
 * generated body (chunk_0188_text1_802F16E0.c; the Maya tail lives in
 * the 0x802F56E0 continuation chunk) with the straight-line callees
 * inlined:
 *   J3DGetTranslateRotateMtx        0x802DA64C  (-54)  joint* -> mtx
 *   J3DGetTranslateRotateMtx(reg)   0x802DA724  (-48)  angles r3..r5,
 *                                                    trans f1..f3,
 *                                                    out r6
 *   PSMTXConcat                     0x8030D0FC  (-51)  A=r3,B=r4->r5
 *   PSMTXCopy                       0x8030D0C8  (-13)  r3 -> r4 (3x4)
 * plus the [inlined gpr-stub] save/restore sequences the generator
 * already folded in.
 *
 * Unlike the leaf repls there are NO guest-state decline gates: every
 * observable effect is produced by the same accessors and helpers the
 * generated bodies call, so the fused body is bit-exact for all states:
 *  - loads/stores use mem_*_direct (journalling, reservations and the
 *    slow-path routing included);
 *  - lfs/stfs widen/narrow via convert_to_double/convert_to_single
 *    (leafrepl_lfs_to/leafrepl_stfs), stfd/lfd move raw f64 bits;
 *  - scalar FP runs the real ppc_fmuls/fadds/fsubs/fdivs helpers and
 *    fcmpu runs ppc_fcmp, so FPSCR/FPRF and inf/NaN results are the
 *    interpreter's own (this is why the concat can be inlined verbatim
 *    where mtxrepl_psmtxconcat has to decline on non-finite inputs);
 *  - psq_l/psq_st run ppc_psq_load_inline/ppc_psq_store_inline which
 *    self-delegate to ppc_psq_load/ppc_psq_store for quantised GQR0 or
 *    LSQE-clear states — no upfront PS-shape gate needed;
 *  - every FP/psq op keeps the generated ppc_fp_available_inline
 *    preface: on lazy-FP + MSR.FP clear it raises FP-unavailable at the
 *    faulting pc and we still claim (return 1) — the identical
 *    early-return state the generated body produces;
 *  - a psq access that sets ctx->exception bails with pc = the psq
 *    instruction pc, again exactly the generated early-return;
 *  - sraw keeps the generated XER.CA expansion; cmpwi/cmplwi merge
 *    XER.SO into CR0 via the leafrepl_cr0_cmp_* helpers and
 *    leafrepl_commit_cr0.
 *
 * Per-block downcount debits are the generated label charges applied at
 * the same points; callee charges are the callee entry debits. Verified
 * totals are asserted in test_leaf_repl.c.
 * ------------------------------------------------------------------ */
static int s_repl_j3d = 1; /* MODERNGEKKO_REPL_J3D=0 disables */

static u32 j3drepl_rotl32(u32 value, u32 sh) {
    sh &= 31u;
    return sh ? ((value << sh) | (value >> (32u - sh))) : value;
}

/* sraw rD,rA,rB — the generated expansion including the XER.CA update. */
static void j3drepl_sraw(CPUState* ctx, u8 d, u8 a, u8 b) {
    const u32 sh = ctx->gpr[b] & 0x3Fu;
    const u32 value = ctx->gpr[a];
    u32 ca = 0u;
    if (sh == 0u) {
        ctx->gpr[d] = value;
    } else if (sh > 31u) {
        ctx->gpr[d] = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        ca = (value & 0x80000000u) != 0u;
    } else {
        ctx->gpr[d] = (u32)((s32)value >> sh);
        ca = (value & 0x80000000u) && ((value << (32u - sh)) != 0u);
    }
    ctx->xer = (ctx->xer & ~0x20000000u) | (ca ? 0x20000000u : 0u);
}

/* Generated fp-available preface: on failure ppc_take_exception has
 * already installed the vector pc (srr0 carries the faulting pc for the
 * rfi retry). Do NOT rewrite ctx->pc here — clobbering the vector
 * redirect re-dispatches the faulting instruction under the
 * post-exception msr (RI clear), and the second raise then records
 * srr1=0x1000 -> non-recoverable -> the guest parks in PPCHalt. */
#define J3D_FP(pc_)                                                        \
    do {                                                                  \
        if (!ppc_fp_available_inline(ctx, (pc_))) {                       \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* Generated psq_l/psq_st sites: fp preface, inline fast path (falls
 * back internally for quantised/LSQE-clear GQR0), then the generated
 * post-access exception bail (pc likewise left at the raised vector). */
#define J3D_PSQ_L(d_, ea_, pc_)                                            \
    do {                                                                  \
        if (!ppc_fp_available_inline(ctx, (pc_))) {                       \
            return 1;                                                     \
        }                                                                 \
        ppc_psq_load_inline(ctx, (d_), (ea_), false, 0u, false, (pc_));   \
        if (ctx->exception) {                                             \
            return 1;                                                     \
        }                                                                 \
    } while (0)
#define J3D_PSQ_S(s_, ea_, pc_)                                            \
    do {                                                                  \
        if (!ppc_fp_available_inline(ctx, (pc_))) {                       \
            return 1;                                                     \
        }                                                                 \
        ppc_psq_store_inline(ctx, (s_), (ea_), false, 0u, false, (pc_));  \
        if (ctx->exception) {                                             \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* J3DGetTranslateRotateMtx (0x802DA64C): r3 = joint (s16 angles at
 * +12/+14/+16, f32 translate at +20/+24/+28), r4 = out 3x4 mtx.
 * Single -54 debit at entry; straight-line; blr -> lr&~3. Returns 1 on
 * an fp-preface bail (pc already set), else 0. */
static int j3drepl_trs(CPUState* ctx) {
    ctx->downcount -= 54;
    ctx->gpr[6] = mem_read32_direct(ctx, ctx->gpr[13] + (u32)(s32)(-26456));
    ctx->gpr[0] = (u32)(s32)(s16)mem_read16_direct(ctx, ctx->gpr[3] + 12u);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 0u) & 0x0000FFFFu;
    ctx->gpr[5] = mem_read32_direct(ctx, ctx->gpr[13] + (u32)(s32)(-26460));
    j3drepl_sraw(ctx, 0, 0, 5);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 2u) & 0xFFFFFFFCu;
    J3D_FP(0x802DA664u);
    leafrepl_lfs_to(ctx, 1, ctx->gpr[6] + ctx->gpr[0]);       /* sin[x] */
    ctx->gpr[7] = mem_read32_direct(ctx, ctx->gpr[13] + (u32)(s32)(-26452));
    J3D_FP(0x802DA66Cu);
    leafrepl_lfs_to(ctx, 2, ctx->gpr[7] + ctx->gpr[0]);       /* cos[x] */
    ctx->gpr[0] = (u32)(s32)(s16)mem_read16_direct(ctx, ctx->gpr[3] + 14u);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 0u) & 0x0000FFFFu;
    j3drepl_sraw(ctx, 0, 0, 5);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 2u) & 0xFFFFFFFCu;
    J3D_FP(0x802DA680u);
    leafrepl_lfs_to(ctx, 3, ctx->gpr[6] + ctx->gpr[0]);       /* sin[y] */
    J3D_FP(0x802DA684u);
    leafrepl_lfs_to(ctx, 4, ctx->gpr[7] + ctx->gpr[0]);       /* cos[y] */
    ctx->gpr[0] = (u32)(s32)(s16)mem_read16_direct(ctx, ctx->gpr[3] + 16u);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 0u) & 0x0000FFFFu;
    j3drepl_sraw(ctx, 0, 0, 5);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 2u) & 0xFFFFFFFCu;
    J3D_FP(0x802DA698u);
    leafrepl_lfs_to(ctx, 5, ctx->gpr[6] + ctx->gpr[0]);       /* sin[z] */
    J3D_FP(0x802DA69Cu);
    leafrepl_lfs_to(ctx, 6, ctx->gpr[7] + ctx->gpr[0]);       /* cos[z] */
    J3D_FP(0x802DA6A0u);
    ctx->fpr[0] = f64_value(f64_bits(ctx->fpr[3]) ^ 0x8000000000000000ull);
    J3D_FP(0x802DA6A4u);
    leafrepl_stfs(ctx, 0, ctx->gpr[4] + 32u);                 /* -sy */
    J3D_FP(0x802DA6A8u); ppc_fmuls(ctx, 0, 6, 4);
    J3D_FP(0x802DA6ACu); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 0u);
    J3D_FP(0x802DA6B0u); ppc_fmuls(ctx, 0, 5, 4);
    J3D_FP(0x802DA6B4u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 16u);
    J3D_FP(0x802DA6B8u); ppc_fmuls(ctx, 0, 4, 1);
    J3D_FP(0x802DA6BCu); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 36u);
    J3D_FP(0x802DA6C0u); ppc_fmuls(ctx, 0, 4, 2);
    J3D_FP(0x802DA6C4u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 40u);
    J3D_FP(0x802DA6C8u); ppc_fmuls(ctx, 4, 2, 5);
    J3D_FP(0x802DA6CCu); ppc_fmuls(ctx, 7, 1, 6);
    J3D_FP(0x802DA6D0u); ppc_fmuls(ctx, 0, 7, 3);
    J3D_FP(0x802DA6D4u); ppc_fsubs(ctx, 0, 0, 4);
    J3D_FP(0x802DA6D8u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 4u);
    J3D_FP(0x802DA6DCu); ppc_fmuls(ctx, 0, 4, 3);
    J3D_FP(0x802DA6E0u); ppc_fsubs(ctx, 0, 0, 7);
    J3D_FP(0x802DA6E4u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 24u);
    J3D_FP(0x802DA6E8u); ppc_fmuls(ctx, 1, 1, 5);
    J3D_FP(0x802DA6ECu); ppc_fmuls(ctx, 2, 2, 6);
    J3D_FP(0x802DA6F0u); ppc_fmuls(ctx, 0, 2, 3);
    J3D_FP(0x802DA6F4u); ppc_fadds(ctx, 0, 1, 0);
    J3D_FP(0x802DA6F8u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 8u);
    J3D_FP(0x802DA6FCu); ppc_fmuls(ctx, 0, 1, 3);
    J3D_FP(0x802DA700u); ppc_fadds(ctx, 0, 2, 0);
    J3D_FP(0x802DA704u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 20u);
    J3D_FP(0x802DA708u); leafrepl_lfs_to(ctx, 0, ctx->gpr[3] + 20u);
    J3D_FP(0x802DA70Cu); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 12u);
    J3D_FP(0x802DA710u); leafrepl_lfs_to(ctx, 0, ctx->gpr[3] + 24u);
    J3D_FP(0x802DA714u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 28u);
    J3D_FP(0x802DA718u); leafrepl_lfs_to(ctx, 0, ctx->gpr[3] + 28u);
    J3D_FP(0x802DA71Cu); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 44u);
    ctx->pc = ctx->lr & ~3u;                                /* blr */
    return 0;
}

/* J3DGetTranslateRotateMtx register-args variant (0x802DA724): r3/r4/r5
 * = angle words (low 16 used), f1/f2/f3 = translate, r6 = out 3x4 mtx.
 * Single -48 debit; straight-line; blr. Same bail contract as trs. */
static int j3drepl_trs_reg(CPUState* ctx) {
    ctx->downcount -= 48;
    ctx->gpr[7] = mem_read32_direct(ctx, ctx->gpr[13] + (u32)(s32)(-26456));
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[3], 0u) & 0x0000FFFFu;
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[13] + (u32)(s32)(-26460));
    j3drepl_sraw(ctx, 0, 0, 3);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 2u) & 0xFFFFFFFCu;
    J3D_FP(0x802DA738u);
    leafrepl_lfs_to(ctx, 4, ctx->gpr[7] + ctx->gpr[0]);       /* sin[r3] */
    ctx->gpr[8] = mem_read32_direct(ctx, ctx->gpr[13] + (u32)(s32)(-26452));
    J3D_FP(0x802DA740u);
    leafrepl_lfs_to(ctx, 5, ctx->gpr[8] + ctx->gpr[0]);       /* cos[r3] */
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[4], 0u) & 0x0000FFFFu;
    j3drepl_sraw(ctx, 0, 0, 3);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 2u) & 0xFFFFFFFCu;
    J3D_FP(0x802DA750u);
    leafrepl_lfs_to(ctx, 6, ctx->gpr[7] + ctx->gpr[0]);       /* sin[r4] */
    J3D_FP(0x802DA754u);
    leafrepl_lfs_to(ctx, 7, ctx->gpr[8] + ctx->gpr[0]);       /* cos[r4] */
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[5], 0u) & 0x0000FFFFu;
    j3drepl_sraw(ctx, 0, 0, 3);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[0], 2u) & 0xFFFFFFFCu;
    J3D_FP(0x802DA764u);
    leafrepl_lfs_to(ctx, 8, ctx->gpr[7] + ctx->gpr[0]);       /* sin[r5] */
    J3D_FP(0x802DA768u);
    leafrepl_lfs_to(ctx, 9, ctx->gpr[8] + ctx->gpr[0]);       /* cos[r5] */
    J3D_FP(0x802DA76Cu);
    ctx->fpr[0] = f64_value(f64_bits(ctx->fpr[6]) ^ 0x8000000000000000ull);
    J3D_FP(0x802DA770u);
    leafrepl_stfs(ctx, 0, ctx->gpr[6] + 32u);                 /* -sin[y] */
    J3D_FP(0x802DA774u); ppc_fmuls(ctx, 0, 9, 7);
    J3D_FP(0x802DA778u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 0u);
    J3D_FP(0x802DA77Cu); ppc_fmuls(ctx, 0, 8, 7);
    J3D_FP(0x802DA780u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 16u);
    J3D_FP(0x802DA784u); ppc_fmuls(ctx, 0, 7, 4);
    J3D_FP(0x802DA788u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 36u);
    J3D_FP(0x802DA78Cu); ppc_fmuls(ctx, 0, 7, 5);
    J3D_FP(0x802DA790u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 40u);
    J3D_FP(0x802DA794u); ppc_fmuls(ctx, 7, 5, 8);
    J3D_FP(0x802DA798u); ppc_fmuls(ctx, 10, 4, 9);
    J3D_FP(0x802DA79Cu); ppc_fmuls(ctx, 0, 10, 6);
    J3D_FP(0x802DA7A0u); ppc_fsubs(ctx, 0, 0, 7);
    J3D_FP(0x802DA7A4u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 4u);
    J3D_FP(0x802DA7A8u); ppc_fmuls(ctx, 0, 7, 6);
    J3D_FP(0x802DA7ACu); ppc_fsubs(ctx, 0, 0, 10);
    J3D_FP(0x802DA7B0u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 24u);
    J3D_FP(0x802DA7B4u); ppc_fmuls(ctx, 4, 4, 8);
    J3D_FP(0x802DA7B8u); ppc_fmuls(ctx, 5, 5, 9);
    J3D_FP(0x802DA7BCu); ppc_fmuls(ctx, 0, 5, 6);
    J3D_FP(0x802DA7C0u); ppc_fadds(ctx, 0, 4, 0);
    J3D_FP(0x802DA7C4u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 8u);
    J3D_FP(0x802DA7C8u); ppc_fmuls(ctx, 0, 4, 6);
    J3D_FP(0x802DA7CCu); ppc_fadds(ctx, 0, 5, 0);
    J3D_FP(0x802DA7D0u); leafrepl_stfs(ctx, 0, ctx->gpr[6] + 20u);
    J3D_FP(0x802DA7D4u); leafrepl_stfs(ctx, 1, ctx->gpr[6] + 12u);
    J3D_FP(0x802DA7D8u); leafrepl_stfs(ctx, 2, ctx->gpr[6] + 28u);
    J3D_FP(0x802DA7DCu); leafrepl_stfs(ctx, 3, ctx->gpr[6] + 44u);
    ctx->pc = ctx->lr & ~3u;                                /* blr */
    return 0;
}

/* PSMTXConcat (0x8030D0FC): r3 = A, r4 = B, r5 = out (all 3x4). Single
 * -51 debit; nested 64-byte frame saving f14/f15/f31; loads f31 from
 * the 0x803F66F0 constant pair via r6. All ps_muls0/ps_madds1/ps_madds0
 * and the interleaved psq_st ordering match the generated body. */
static int j3drepl_mtxconcat(CPUState* ctx) {
    ctx->downcount -= 51;
    {
        const u32 ea = ctx->gpr[1] + (u32)(s32)(-64);
        mem_write32_direct(ctx, ea, ctx->gpr[1]);
        ctx->gpr[1] = ea;
    }
    J3D_PSQ_L(0, ctx->gpr[3] + 0u, 0x8030D100u);
    J3D_FP(0x8030D104u);
    mem_write64_direct(ctx, ctx->gpr[1] + 8u, f64_bits(ctx->fpr[14]));
    J3D_PSQ_L(6, ctx->gpr[4] + 0u, 0x8030D108u);
    ctx->gpr[6] = ((u32)(s32)(32831) << 16);
    J3D_PSQ_L(7, ctx->gpr[4] + 8u, 0x8030D110u);
    J3D_FP(0x8030D114u);
    mem_write64_direct(ctx, ctx->gpr[1] + 16u, f64_bits(ctx->fpr[15]));
    ctx->gpr[6] += (u32)(s32)(26352);                         /* 0x803F66F0 */
    J3D_FP(0x8030D11Cu);
    mem_write64_direct(ctx, ctx->gpr[1] + 40u, f64_bits(ctx->fpr[31]));
    J3D_PSQ_L(8, ctx->gpr[4] + 16u, 0x8030D120u);
    J3D_FP(0x8030D124u); ppc_ps_muls0(ctx, 12, 6, 0);
    J3D_PSQ_L(2, ctx->gpr[3] + 16u, 0x8030D128u);
    J3D_FP(0x8030D12Cu); ppc_ps_muls0(ctx, 13, 7, 0);
    J3D_PSQ_L(31, ctx->gpr[6] + 0u, 0x8030D130u);
    J3D_FP(0x8030D134u); ppc_ps_muls0(ctx, 14, 6, 2);
    J3D_PSQ_L(9, ctx->gpr[4] + 24u, 0x8030D138u);
    J3D_FP(0x8030D13Cu); ppc_ps_muls0(ctx, 15, 7, 2);
    J3D_PSQ_L(1, ctx->gpr[3] + 8u, 0x8030D140u);
    J3D_FP(0x8030D144u); ppc_ps_madds1(ctx, 12, 8, 0, 12);
    J3D_PSQ_L(3, ctx->gpr[3] + 24u, 0x8030D148u);
    J3D_FP(0x8030D14Cu); ppc_ps_madds1(ctx, 14, 8, 2, 14);
    J3D_PSQ_L(10, ctx->gpr[4] + 32u, 0x8030D150u);
    J3D_FP(0x8030D154u); ppc_ps_madds1(ctx, 13, 9, 0, 13);
    J3D_PSQ_L(11, ctx->gpr[4] + 40u, 0x8030D158u);
    J3D_FP(0x8030D15Cu); ppc_ps_madds1(ctx, 15, 9, 2, 15);
    J3D_PSQ_L(4, ctx->gpr[3] + 32u, 0x8030D160u);
    J3D_PSQ_L(5, ctx->gpr[3] + 40u, 0x8030D164u);
    J3D_FP(0x8030D168u); ppc_ps_madds0(ctx, 12, 10, 1, 12);
    J3D_FP(0x8030D16Cu); ppc_ps_madds0(ctx, 13, 11, 1, 13);
    J3D_FP(0x8030D170u); ppc_ps_madds0(ctx, 14, 10, 3, 14);
    J3D_FP(0x8030D174u); ppc_ps_madds0(ctx, 15, 11, 3, 15);
    J3D_PSQ_S(12, ctx->gpr[5] + 0u, 0x8030D178u);
    J3D_FP(0x8030D17Cu); ppc_ps_muls0(ctx, 2, 6, 4);
    J3D_FP(0x8030D180u); ppc_ps_madds1(ctx, 13, 31, 1, 13);
    J3D_FP(0x8030D184u); ppc_ps_muls0(ctx, 0, 7, 4);
    J3D_PSQ_S(14, ctx->gpr[5] + 16u, 0x8030D188u);
    J3D_FP(0x8030D18Cu); ppc_ps_madds1(ctx, 15, 31, 3, 15);
    J3D_PSQ_S(13, ctx->gpr[5] + 8u, 0x8030D190u);
    J3D_FP(0x8030D194u); ppc_ps_madds1(ctx, 2, 8, 4, 2);
    J3D_FP(0x8030D198u); ppc_ps_madds1(ctx, 0, 9, 4, 0);
    J3D_FP(0x8030D19Cu); ppc_ps_madds0(ctx, 2, 10, 5, 2);
    J3D_FP(0x8030D1A0u);
    ctx->fpr[14] = f64_value(mem_read64_direct(ctx, ctx->gpr[1] + 8u));
    J3D_PSQ_S(15, ctx->gpr[5] + 24u, 0x8030D1A4u);
    J3D_FP(0x8030D1A8u); ppc_ps_madds0(ctx, 0, 11, 5, 0);
    J3D_PSQ_S(2, ctx->gpr[5] + 32u, 0x8030D1ACu);
    J3D_FP(0x8030D1B0u); ppc_ps_madds1(ctx, 0, 31, 5, 0);
    J3D_FP(0x8030D1B4u);
    ctx->fpr[15] = f64_value(mem_read64_direct(ctx, ctx->gpr[1] + 16u));
    J3D_PSQ_S(0, ctx->gpr[5] + 40u, 0x8030D1B8u);
    J3D_FP(0x8030D1BCu);
    ctx->fpr[31] = f64_value(mem_read64_direct(ctx, ctx->gpr[1] + 40u));
    ctx->gpr[1] += 64u;
    ctx->pc = ctx->lr & ~3u;                                /* blr */
    return 0;
}

/* PSMTXCopy (0x8030D0C8): r3 = src, r4 = dst (3x4), via six
 * psq_l/psq_st pairs. Single -13 debit; blr. */
static int j3drepl_mtxcopy(CPUState* ctx) {
    ctx->downcount -= 13;
    J3D_PSQ_L(0, ctx->gpr[3] + 0u, 0x8030D0C8u);
    J3D_PSQ_S(0, ctx->gpr[4] + 0u, 0x8030D0CCu);
    J3D_PSQ_L(1, ctx->gpr[3] + 8u, 0x8030D0D0u);
    J3D_PSQ_S(1, ctx->gpr[4] + 8u, 0x8030D0D4u);
    J3D_PSQ_L(2, ctx->gpr[3] + 16u, 0x8030D0D8u);
    J3D_PSQ_S(2, ctx->gpr[4] + 16u, 0x8030D0DCu);
    J3D_PSQ_L(3, ctx->gpr[3] + 24u, 0x8030D0E0u);
    J3D_PSQ_S(3, ctx->gpr[4] + 24u, 0x8030D0E4u);
    J3D_PSQ_L(4, ctx->gpr[3] + 32u, 0x8030D0E8u);
    J3D_PSQ_S(4, ctx->gpr[4] + 32u, 0x8030D0ECu);
    J3D_PSQ_L(5, ctx->gpr[3] + 40u, 0x8030D0F0u);
    J3D_PSQ_S(5, ctx->gpr[4] + 40u, 0x8030D0F4u);
    ctx->pc = ctx->lr & ~3u;                                /* blr */
    return 0;
}

/* J3DMtxCalcBasic::calcTransform (0x802F5090): r4 = joint index,
 * r5 = J3DTransformInfo (scale +0/+4/+8, s16 angles +12/+14/+16,
 * translate +20/+24/+28). Scales the accumulated product vec at
 * 0x803EDBB0 by the joint scale; if all three products equal 1.0f the
 * unit-scale flag byte (*( *(*(0x803EDA58+56) +132) + (r4 & 0xFFFF)))
 * is set to 1 and the TRS matrix is used unscaled, else flag = 0 and
 * the stack matrix columns are scaled by the joint scale. Then
 * currentMtx (0x803EDB80) = currentMtx . localMtx and the result is
 * copied to jointMtx[r4 & 0xFFFF] = *( *(*(0x803EDA58+56) +140) +
 * (r4 & 0xFFFF)*48 ). Frame 96, saves r29-r31. Charges: -205 flag=1
 * all-equal path, -227 flag=0 path (first compare fails). */
static int j3drepl_basic(CPUState* ctx) {
    /* label_802F5090 (-5): stwu r1,-96; mflr r0; stw r0,100(r1);
     * addi r11,r1,96 */
    ctx->downcount -= 5;
    {
        const u32 ea = ctx->gpr[1] + (u32)(s32)(-96);
        mem_write32_direct(ctx, ea, ctx->gpr[1]);
        ctx->gpr[1] = ea;
    }
    ctx->gpr[0] = ctx->lr;
    mem_write32_direct(ctx, ctx->gpr[1] + 100u, ctx->gpr[0]);
    ctx->gpr[11] = ctx->gpr[1] + 96u;
    /* 0x802F50A0 bl 0x80328F40 (-4): save r29-r31 */
    ctx->lr = 0x802F50A4u;
    ctx->downcount -= 4;
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-12), ctx->gpr[29]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-8), ctx->gpr[30]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-4), ctx->gpr[31]);
    /* label_802F50A4 (-26): product vec *= joint scale, compare vs 1 */
    ctx->downcount -= 26;
    ctx->gpr[30] = ctx->gpr[4] | ctx->gpr[4];
    ctx->gpr[31] = ctx->gpr[5] | ctx->gpr[5];
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);                  /* 0x803F0000 */
    ctx->gpr[4] = ctx->gpr[3] + (u32)(s32)(-9296);            /* 0x803EDBB0 */
    J3D_FP(0x802F50B4u); leafrepl_lfs_to(ctx, 1, ctx->gpr[4] + 0u);
    J3D_FP(0x802F50B8u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 0u);
    J3D_FP(0x802F50BCu); ppc_fmuls(ctx, 0, 1, 0);
    J3D_FP(0x802F50C0u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 0u);
    J3D_FP(0x802F50C4u); leafrepl_lfs_to(ctx, 1, ctx->gpr[4] + 4u);
    J3D_FP(0x802F50C8u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 4u);
    J3D_FP(0x802F50CCu); ppc_fmuls(ctx, 0, 1, 0);
    J3D_FP(0x802F50D0u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 4u);
    J3D_FP(0x802F50D4u); leafrepl_lfs_to(ctx, 1, ctx->gpr[4] + 8u);
    J3D_FP(0x802F50D8u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 8u);
    J3D_FP(0x802F50DCu); ppc_fmuls(ctx, 0, 1, 0);
    J3D_FP(0x802F50E0u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 8u);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[4] + 0u);
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[4] + 4u);
    mem_write32_direct(ctx, ctx->gpr[1] + 8u, ctx->gpr[3]);
    mem_write32_direct(ctx, ctx->gpr[1] + 12u, ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[4] + 8u);
    mem_write32_direct(ctx, ctx->gpr[1] + 16u, ctx->gpr[0]);
    J3D_FP(0x802F50FCu);
    leafrepl_lfs_to(ctx, 1, ctx->gpr[2] + (u32)(s32)(-13072)); /* 1.0f */
    J3D_FP(0x802F5100u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 8u);
    J3D_FP(0x802F5104u);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_basic_neq;   /* bc 4,2 */
    /* label_802F510C (-3) */
    ctx->downcount -= 3;
    J3D_FP(0x802F510Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 12u);
    J3D_FP(0x802F5110u);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_basic_neq;
    /* label_802F5118 (-3) */
    ctx->downcount -= 3;
    J3D_FP(0x802F5118u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 16u);
    J3D_FP(0x802F511Cu);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_basic_neq;
    /* label_802F5124 (-2): r0 = 1; b 5130 */
    ctx->downcount -= 2;
    ctx->gpr[0] = (u32)(s32)(1);
    goto j3d_basic_flag;
j3d_basic_neq:
    /* label_802F512C (-1): r0 = 0 */
    ctx->downcount -= 1;
    ctx->gpr[0] = (u32)(s32)(0);
j3d_basic_flag:
    /* label_802F5130 (-2): cmpwi r0,0; bc 12,2 -> 515C on EQ */
    ctx->downcount -= 2;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[0], 0));
    if ((ctx->cr & 0x20000000u) != 0u) {
        /* label_802F515C (-8): flag byte = 0, r29 = 0 */
        ctx->downcount -= 8;
        ctx->gpr[3] = ((u32)(s32)(32831) << 16);
        ctx->gpr[3] += (u32)(s32)(-9640);                     /* 0x803EDA58 */
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 56u);
        ctx->gpr[4] = (u32)(s32)(0);
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 132u);
        ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
        mem_write8_direct(ctx, ctx->gpr[3] + ctx->gpr[0], (u8)ctx->gpr[4]);
        ctx->gpr[29] = (u32)(s32)(0);
    } else {
        /* label_802F5138 (-9): flag byte = 1, r29 = 1 */
        ctx->downcount -= 9;
        ctx->gpr[3] = ((u32)(s32)(32831) << 16);
        ctx->gpr[3] += (u32)(s32)(-9640);
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 56u);
        ctx->gpr[4] = (u32)(s32)(1);
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 132u);
        ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
        mem_write8_direct(ctx, ctx->gpr[3] + ctx->gpr[0], (u8)ctx->gpr[4]);
        ctx->gpr[29] = (u32)(s32)(1);
    }
    /* label_802F517C (-3): bl J3DGetTranslateRotateMtx(joint, r1+20) */
    ctx->downcount -= 3;
    ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];
    ctx->gpr[4] = ctx->gpr[1] + (u32)(s32)(20);
    ctx->lr = 0x802F5188u;
    if (j3drepl_trs(ctx)) return 1;
    /* label_802F5188 (-2): cmpwi r29,0; bc 4,2 -> 5208 on !EQ */
    ctx->downcount -= 2;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[29], 0));
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_basic_concat;
    /* label_802F5190 (-30): scale stack mtx columns by joint scale
     * f3 = scale.x, f2 = scale.y, f1 = scale.z */
    ctx->downcount -= 30;
    J3D_FP(0x802F5190u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 20u);
    J3D_FP(0x802F5194u); leafrepl_lfs_to(ctx, 3, ctx->gpr[31] + 0u);
    J3D_FP(0x802F5198u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F519Cu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 20u);
    J3D_FP(0x802F51A0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 24u);
    J3D_FP(0x802F51A4u); leafrepl_lfs_to(ctx, 2, ctx->gpr[31] + 4u);
    J3D_FP(0x802F51A8u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F51ACu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 24u);
    J3D_FP(0x802F51B0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 28u);
    J3D_FP(0x802F51B4u); leafrepl_lfs_to(ctx, 1, ctx->gpr[31] + 8u);
    J3D_FP(0x802F51B8u); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F51BCu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 28u);
    J3D_FP(0x802F51C0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 36u);
    J3D_FP(0x802F51C4u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F51C8u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 36u);
    J3D_FP(0x802F51CCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 40u);
    J3D_FP(0x802F51D0u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F51D4u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 40u);
    J3D_FP(0x802F51D8u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 44u);
    J3D_FP(0x802F51DCu); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F51E0u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 44u);
    J3D_FP(0x802F51E4u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 52u);
    J3D_FP(0x802F51E8u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F51ECu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 52u);
    J3D_FP(0x802F51F0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 56u);
    J3D_FP(0x802F51F4u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F51F8u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 56u);
    J3D_FP(0x802F51FCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 60u);
    J3D_FP(0x802F5200u); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F5204u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 60u);
j3d_basic_concat:
    /* label_802F5208 (-5): PSMTXConcat(0x803EDB80, r1+20, 0x803EDB80) */
    ctx->downcount -= 5;
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[3] += (u32)(s32)(-9344);                         /* 0x803EDB80 */
    ctx->gpr[4] = ctx->gpr[1] + (u32)(s32)(20);
    ctx->gpr[5] = ctx->gpr[3] | ctx->gpr[3];
    ctx->lr = 0x802F521Cu;
    if (j3drepl_mtxconcat(ctx)) return 1;
    /* label_802F521C (-12): r4 = jointMtx + (r30 & 0xFFFF)*48,
     * r3 = 0x803EDB80; bl PSMTXCopy */
    ctx->downcount -= 12;
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[3] += (u32)(s32)(-9640);                         /* 0x803EDA58 */
    ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[3] + 56u);
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[3] += (u32)(s32)(-9344);                         /* 0x803EDB80 */
    ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[4] + 140u);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
    ctx->gpr[0] = (u32)((s64)(s32)ctx->gpr[0] * (s64)(s32)48);
    ctx->gpr[4] = ctx->gpr[4] + ctx->gpr[0];
    ctx->lr = 0x802F5244u;
    if (j3drepl_mtxcopy(ctx)) return 1;
    /* label_802F5244 (-2): addi r11,r1,96; bl 0x80328F8C (-4):
     * restore r29-r31 */
    ctx->downcount -= 2;
    ctx->gpr[11] = ctx->gpr[1] + 96u;
    ctx->lr = 0x802F524Cu;
    ctx->downcount -= 4;
    ctx->gpr[29] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-12));
    ctx->gpr[30] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-8));
    ctx->gpr[31] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-4));
    /* label_802F524C (-5): lwz r0,100(r1); mtlr; addi r1,96; blr */
    ctx->downcount -= 5;
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[1] + 100u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] += (u32)96;
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* J3DMtxCalcSoftimage::calcTransform (0x802F52BC): r4 = joint index,
 * r5 = J3DTransformInfo (scale +0/+4/+8, s16 angles +12/+14/+16,
 * translate +20/+24/+28). Softimage order: the joint TRANSLATION is
 * pre-scaled by the accumulated product vec (0x803EDBB0) before the
 * TRS build; currentMtx = currentMtx . localMtx happens first; the
 * product vec is then multiplied by the joint scale and the flag byte
 * is set iff all three products equal 1.0f. flag=1: copy currentMtx
 * straight to jointMtx[r30 & 0xFFFF]; flag=0: copy currentMtx's rows
 * scaled by the product vec to a stack mtx, then to jointMtx.
 * Frame 96, saves r29-r31. Charges: -212 flag=1 all-equal path,
 * -243 flag=0 path (first compare fails). */
static int j3drepl_softimage(CPUState* ctx) {
    /* label_802F52BC (-5) + stub 0x80328F40 (-4): save r29-r31 */
    ctx->downcount -= 5;
    {
        const u32 ea = ctx->gpr[1] + (u32)(s32)(-96);
        mem_write32_direct(ctx, ea, ctx->gpr[1]);
        ctx->gpr[1] = ea;
    }
    ctx->gpr[0] = ctx->lr;
    mem_write32_direct(ctx, ctx->gpr[1] + 100u, ctx->gpr[0]);
    ctx->gpr[11] = ctx->gpr[1] + 96u;
    ctx->lr = 0x802F52D0u;
    ctx->downcount -= 4;
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-12), ctx->gpr[29]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-8), ctx->gpr[30]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-4), ctx->gpr[31]);
    /* label_802F52D0 (-18): angles -> r3/r4/r5, translation *=
     * product vec, r6 = r1+20; bl J3DGetTranslateRotateMtx(reg) */
    ctx->downcount -= 18;
    ctx->gpr[30] = ctx->gpr[4] | ctx->gpr[4];
    ctx->gpr[29] = ctx->gpr[5] | ctx->gpr[5];
    ctx->gpr[3] = (u32)(s32)(s16)mem_read16_direct(ctx, ctx->gpr[5] + 12u);
    ctx->gpr[4] = (u32)(s32)(s16)mem_read16_direct(ctx, ctx->gpr[5] + 14u);
    ctx->gpr[5] = (u32)(s32)(s16)mem_read16_direct(ctx, ctx->gpr[5] + 16u);
    J3D_FP(0x802F52E4u); leafrepl_lfs_to(ctx, 1, ctx->gpr[29] + 20u);
    ctx->gpr[6] = ((u32)(s32)(32831) << 16);
    ctx->gpr[31] = ctx->gpr[6] + (u32)(s32)(-9296);           /* 0x803EDBB0 */
    J3D_FP(0x802F52F0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 0u);
    J3D_FP(0x802F52F4u); ppc_fmuls(ctx, 1, 1, 0);
    J3D_FP(0x802F52F8u); leafrepl_lfs_to(ctx, 2, ctx->gpr[29] + 24u);
    J3D_FP(0x802F52FCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 4u);
    J3D_FP(0x802F5300u); ppc_fmuls(ctx, 2, 2, 0);
    J3D_FP(0x802F5304u); leafrepl_lfs_to(ctx, 3, ctx->gpr[29] + 28u);
    J3D_FP(0x802F5308u); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 8u);
    J3D_FP(0x802F530Cu); ppc_fmuls(ctx, 3, 3, 0);
    ctx->gpr[6] = ctx->gpr[1] + (u32)(s32)(20);
    ctx->lr = 0x802F5318u;
    if (j3drepl_trs_reg(ctx)) return 1;
    /* label_802F5318 (-5): PSMTXConcat(0x803EDB80, r1+20, 0x803EDB80) */
    ctx->downcount -= 5;
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[3] += (u32)(s32)(-9344);                         /* 0x803EDB80 */
    ctx->gpr[4] = ctx->gpr[1] + (u32)(s32)(20);
    ctx->gpr[5] = ctx->gpr[3] | ctx->gpr[3];
    ctx->lr = 0x802F532Cu;
    if (j3drepl_mtxconcat(ctx)) return 1;
    /* label_802F532C (-24): product vec *= joint scale; copy vec to
     * r1+8..16; f1 = 1.0f; first compare */
    ctx->downcount -= 24;
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[4] = ctx->gpr[3] + (u32)(s32)(-9296);            /* 0x803EDBB0 */
    J3D_FP(0x802F5334u); leafrepl_lfs_to(ctx, 1, ctx->gpr[4] + 0u);
    J3D_FP(0x802F5338u); leafrepl_lfs_to(ctx, 0, ctx->gpr[29] + 0u);
    J3D_FP(0x802F533Cu); ppc_fmuls(ctx, 0, 1, 0);
    J3D_FP(0x802F5340u); leafrepl_stfs(ctx, 0, ctx->gpr[4] + 0u);
    J3D_FP(0x802F5344u); leafrepl_lfs_to(ctx, 1, ctx->gpr[31] + 4u);
    J3D_FP(0x802F5348u); leafrepl_lfs_to(ctx, 0, ctx->gpr[29] + 4u);
    J3D_FP(0x802F534Cu); ppc_fmuls(ctx, 0, 1, 0);
    J3D_FP(0x802F5350u); leafrepl_stfs(ctx, 0, ctx->gpr[31] + 4u);
    J3D_FP(0x802F5354u); leafrepl_lfs_to(ctx, 1, ctx->gpr[31] + 8u);
    J3D_FP(0x802F5358u); leafrepl_lfs_to(ctx, 0, ctx->gpr[29] + 8u);
    J3D_FP(0x802F535Cu); ppc_fmuls(ctx, 0, 1, 0);
    J3D_FP(0x802F5360u); leafrepl_stfs(ctx, 0, ctx->gpr[31] + 8u);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[4] + 0u);
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[4] + 4u);
    mem_write32_direct(ctx, ctx->gpr[1] + 8u, ctx->gpr[3]);
    mem_write32_direct(ctx, ctx->gpr[1] + 12u, ctx->gpr[0]);
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[4] + 8u);
    mem_write32_direct(ctx, ctx->gpr[1] + 16u, ctx->gpr[0]);
    J3D_FP(0x802F537Cu);
    leafrepl_lfs_to(ctx, 1, ctx->gpr[2] + (u32)(s32)(-13072)); /* 1.0f */
    J3D_FP(0x802F5380u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 8u);
    J3D_FP(0x802F5384u);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_si_neq;      /* bc 4,2 */
    /* label_802F538C (-3) */
    ctx->downcount -= 3;
    J3D_FP(0x802F538Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 12u);
    J3D_FP(0x802F5390u);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_si_neq;
    /* label_802F5398 (-3) */
    ctx->downcount -= 3;
    J3D_FP(0x802F5398u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 16u);
    J3D_FP(0x802F539Cu);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_si_neq;
    /* label_802F53A4 (-2): r0 = 1; b 53B0 */
    ctx->downcount -= 2;
    ctx->gpr[0] = (u32)(s32)(1);
    goto j3d_si_flag;
j3d_si_neq:
    /* label_802F53AC (-1): r0 = 0 */
    ctx->downcount -= 1;
    ctx->gpr[0] = (u32)(s32)(0);
j3d_si_flag:
    /* label_802F53B0 (-2): cmpwi r0,0; bc 12,2 -> 53DC on EQ */
    ctx->downcount -= 2;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[0], 0));
    if ((ctx->cr & 0x20000000u) != 0u) {
        /* label_802F53DC (-8): flag byte = 0, r0 = 0 */
        ctx->downcount -= 8;
        ctx->gpr[3] = ((u32)(s32)(32831) << 16);
        ctx->gpr[3] += (u32)(s32)(-9640);                     /* 0x803EDA58 */
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 56u);
        ctx->gpr[4] = (u32)(s32)(0);
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 132u);
        ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
        mem_write8_direct(ctx, ctx->gpr[3] + ctx->gpr[0], (u8)ctx->gpr[4]);
        ctx->gpr[0] = (u32)(s32)(0);
    } else {
        /* label_802F53B8 (-9): flag byte = 1, r0 = 1 */
        ctx->downcount -= 9;
        ctx->gpr[3] = ((u32)(s32)(32831) << 16);
        ctx->gpr[3] += (u32)(s32)(-9640);
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 56u);
        ctx->gpr[4] = (u32)(s32)(1);
        ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 132u);
        ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
        mem_write8_direct(ctx, ctx->gpr[3] + ctx->gpr[0], (u8)ctx->gpr[4]);
        ctx->gpr[0] = (u32)(s32)(1);
    }
    /* label_802F53FC (-2): cmpwi r0,0; bc 4,2 -> 54C8 on !EQ
     * (flag=1 -> direct copy of currentMtx) */
    ctx->downcount -= 2;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[0], 0));
    if ((ctx->cr & 0x20000000u) != 0u) {
        /* label_802F5404 (-50): scale currentMtx columns by the
         * product vec into the stack mtx (translations unscaled),
         * then bl PSMTXCopy(r1+20 -> jointMtx[r30 & 0xFFFF]) */
        ctx->downcount -= 50;
        ctx->gpr[3] = ctx->gpr[1] + (u32)(s32)(20);
        ctx->gpr[4] = ((u32)(s32)(32831) << 16);
        ctx->gpr[5] = ctx->gpr[4] + (u32)(s32)(-9344);        /* 0x803EDB80 */
        J3D_FP(0x802F5410u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 0u);
        ctx->gpr[4] = ((u32)(s32)(32831) << 16);
        J3D_FP(0x802F5418u);
        leafrepl_lfs_to(ctx, 3, ctx->gpr[4] + (u32)(s32)(-9296));
        J3D_FP(0x802F541Cu); ppc_fmuls(ctx, 0, 0, 3);
        J3D_FP(0x802F5420u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 20u);
        J3D_FP(0x802F5424u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 4u);
        J3D_FP(0x802F5428u); leafrepl_lfs_to(ctx, 2, ctx->gpr[31] + 4u);
        J3D_FP(0x802F542Cu); ppc_fmuls(ctx, 0, 0, 2);
        J3D_FP(0x802F5430u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 24u);
        J3D_FP(0x802F5434u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 8u);
        J3D_FP(0x802F5438u); leafrepl_lfs_to(ctx, 1, ctx->gpr[31] + 8u);
        J3D_FP(0x802F543Cu); ppc_fmuls(ctx, 0, 0, 1);
        J3D_FP(0x802F5440u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 28u);
        J3D_FP(0x802F5444u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 12u);
        J3D_FP(0x802F5448u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 32u);
        J3D_FP(0x802F544Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 16u);
        J3D_FP(0x802F5450u); ppc_fmuls(ctx, 0, 0, 3);
        J3D_FP(0x802F5454u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 36u);
        J3D_FP(0x802F5458u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 20u);
        J3D_FP(0x802F545Cu); ppc_fmuls(ctx, 0, 0, 2);
        J3D_FP(0x802F5460u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 40u);
        J3D_FP(0x802F5464u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 24u);
        J3D_FP(0x802F5468u); ppc_fmuls(ctx, 0, 0, 1);
        J3D_FP(0x802F546Cu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 44u);
        J3D_FP(0x802F5470u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 28u);
        J3D_FP(0x802F5474u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 48u);
        J3D_FP(0x802F5478u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 32u);
        J3D_FP(0x802F547Cu); ppc_fmuls(ctx, 0, 0, 3);
        J3D_FP(0x802F5480u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 52u);
        J3D_FP(0x802F5484u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 36u);
        J3D_FP(0x802F5488u); ppc_fmuls(ctx, 0, 0, 2);
        J3D_FP(0x802F548Cu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 56u);
        J3D_FP(0x802F5490u); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 40u);
        J3D_FP(0x802F5494u); ppc_fmuls(ctx, 0, 0, 1);
        J3D_FP(0x802F5498u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 60u);
        J3D_FP(0x802F549Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[5] + 44u);
        J3D_FP(0x802F54A0u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 64u);
        ctx->gpr[4] = ((u32)(s32)(32831) << 16);
        ctx->gpr[4] += (u32)(s32)(-9640);                     /* 0x803EDA58 */
        ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[4] + 56u);
        ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[4] + 140u);
        ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
        ctx->gpr[0] = (u32)((s64)(s32)ctx->gpr[0] * (s64)(s32)48);
        ctx->gpr[4] = ctx->gpr[4] + ctx->gpr[0];
        ctx->lr = 0x802F54C4u;
        if (j3drepl_mtxcopy(ctx)) return 1;
        /* label_802F54C4 (-1): b 54F0 */
        ctx->downcount -= 1;
    } else {
        /* label_802F54C8 (-12): bl PSMTXCopy(0x803EDB80 ->
         * jointMtx[r30 & 0xFFFF]) */
        ctx->downcount -= 12;
        ctx->gpr[3] = ((u32)(s32)(32831) << 16);
        ctx->gpr[3] += (u32)(s32)(-9640);                     /* 0x803EDA58 */
        ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[3] + 56u);
        ctx->gpr[3] = ((u32)(s32)(32831) << 16);
        ctx->gpr[3] += (u32)(s32)(-9344);                     /* 0x803EDB80 */
        ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[4] + 140u);
        ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
        ctx->gpr[0] = (u32)((s64)(s32)ctx->gpr[0] * (s64)(s32)48);
        ctx->gpr[4] = ctx->gpr[4] + ctx->gpr[0];
        ctx->lr = 0x802F54F0u;
        if (j3drepl_mtxcopy(ctx)) return 1;
    }
    /* label_802F54F0 (-2) + stub 0x80328F8C (-4): restore r29-r31 */
    ctx->downcount -= 2;
    ctx->gpr[11] = ctx->gpr[1] + 96u;
    ctx->lr = 0x802F54F8u;
    ctx->downcount -= 4;
    ctx->gpr[29] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-12));
    ctx->gpr[30] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-8));
    ctx->gpr[31] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-4));
    /* label_802F54F8 (-5): lwz r0,100(r1); mtlr; addi r1,96; blr */
    ctx->downcount -= 5;
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[1] + 100u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] += (u32)96;
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* J3DMtxCalcMaya::calcTransform (0x802F5508): r4 = joint index,
 * r5 = J3DTransformInfo. r29 = shape flag byte
 * *( *(*(*(0x803EDA58+56) +4) +44) + (r4 & 0x3FFF)*4 + 27 ). Joint
 * scale compared against 1.0f: all equal -> flag byte = 1, r27 = 1,
 * else flag = 0, r27 = 0 and the stack TRS columns are scaled by the
 * joint scale. If r29 == 1 the stack matrix ROWS are additionally
 * scaled by the reciprocal of the previous-scale vec at 0x803EDBBC.
 * currentMtx = currentMtx . localMtx; copy to jointMtx[r30 & 0xFFFF];
 * the previous-scale vec is updated from the joint scale. Frame 80,
 * saves r27-r31; the body crosses the chunk edge at 0x802F56E0 (that
 * continuation entry is NOT claimed — it is inlined here). Charges:
 * -281 all-equal + r29==1, -198 all-equal + r29!=1, -305/-222 for the
 * non-equal variants. */
static int j3drepl_maya(CPUState* ctx) {
    /* label_802F5508 (-5) + stub 0x80328F38 (-6): save r27-r31 */
    ctx->downcount -= 5;
    {
        const u32 ea = ctx->gpr[1] + (u32)(s32)(-80);
        mem_write32_direct(ctx, ea, ctx->gpr[1]);
        ctx->gpr[1] = ea;
    }
    ctx->gpr[0] = ctx->lr;
    mem_write32_direct(ctx, ctx->gpr[1] + 84u, ctx->gpr[0]);
    ctx->gpr[11] = ctx->gpr[1] + 80u;
    ctx->lr = 0x802F551Cu;
    ctx->downcount -= 6;
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-20), ctx->gpr[27]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-16), ctx->gpr[28]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-12), ctx->gpr[29]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-8), ctx->gpr[30]);
    mem_write32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-4), ctx->gpr[31]);
    /* label_802F551C (-15): shape flag byte + first scale compare */
    ctx->downcount -= 15;
    ctx->gpr[30] = ctx->gpr[4] | ctx->gpr[4];
    ctx->gpr[31] = ctx->gpr[5] | ctx->gpr[5];
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[28] = ctx->gpr[3] + (u32)(s32)(-9640);           /* 0x803EDA58 */
    ctx->gpr[5] = mem_read32_direct(ctx, ctx->gpr[28] + 56u);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[5] + 4u);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + 44u);
    ctx->gpr[4] = j3drepl_rotl32(ctx->gpr[4], 0u) & 0x0000FFFFu;
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 2u) & 0x0003FFFCu;
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[3] + ctx->gpr[0]);
    ctx->gpr[29] = mem_read8_direct(ctx, ctx->gpr[3] + 27u);
    J3D_FP(0x802F5548u);
    leafrepl_lfs_to(ctx, 1, ctx->gpr[2] + (u32)(s32)(-13072)); /* 1.0f */
    J3D_FP(0x802F554Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 0u);
    J3D_FP(0x802F5550u);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_maya_neq;    /* bc 4,2 */
    /* label_802F5558 (-3) */
    ctx->downcount -= 3;
    J3D_FP(0x802F5558u); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 4u);
    J3D_FP(0x802F555Cu);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_maya_neq;
    /* label_802F5564 (-3) */
    ctx->downcount -= 3;
    J3D_FP(0x802F5564u); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 8u);
    J3D_FP(0x802F5568u);
    ppc_fcmp(ctx, 0, ctx->fpr[1], ctx->fpr[0], false);
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_maya_neq;
    /* label_802F5570 (-5): flag byte = 1, r27 = 1 */
    ctx->downcount -= 5;
    ctx->gpr[0] = (u32)(s32)(1);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[5] + 132u);
    mem_write8_direct(ctx, ctx->gpr[3] + ctx->gpr[4], (u8)ctx->gpr[0]);
    ctx->gpr[27] = (u32)(s32)(1);
    goto j3d_maya_trs;
j3d_maya_neq:
    /* label_802F5584 (-5): flag byte = 0, r27 = 0 */
    ctx->downcount -= 5;
    ctx->gpr[4] = (u32)(s32)(0);
    ctx->gpr[3] = mem_read32_direct(ctx, ctx->gpr[5] + 132u);
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
    mem_write8_direct(ctx, ctx->gpr[3] + ctx->gpr[0], (u8)ctx->gpr[4]);
    ctx->gpr[27] = (u32)(s32)(0);
j3d_maya_trs:
    /* label_802F5598 (-3): bl J3DGetTranslateRotateMtx(joint, r1+8) */
    ctx->downcount -= 3;
    ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];
    ctx->gpr[4] = ctx->gpr[1] + (u32)(s32)(8);
    ctx->lr = 0x802F55A4u;
    if (j3drepl_trs(ctx)) return 1;
    /* label_802F55A4 (-2): cmpwi r27,0; bc 4,2 -> 5624 on !EQ */
    ctx->downcount -= 2;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_s32(ctx, (s32)ctx->gpr[27], 0));
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_maya_r29;
    /* label_802F55AC (-30): scale stack mtx columns by joint scale
     * f3 = scale.x, f2 = scale.y, f1 = scale.z */
    ctx->downcount -= 30;
    J3D_FP(0x802F55ACu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 8u);
    J3D_FP(0x802F55B0u); leafrepl_lfs_to(ctx, 3, ctx->gpr[31] + 0u);
    J3D_FP(0x802F55B4u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F55B8u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 8u);
    J3D_FP(0x802F55BCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 12u);
    J3D_FP(0x802F55C0u); leafrepl_lfs_to(ctx, 2, ctx->gpr[31] + 4u);
    J3D_FP(0x802F55C4u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F55C8u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 12u);
    J3D_FP(0x802F55CCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 16u);
    J3D_FP(0x802F55D0u); leafrepl_lfs_to(ctx, 1, ctx->gpr[31] + 8u);
    J3D_FP(0x802F55D4u); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F55D8u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 16u);
    J3D_FP(0x802F55DCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 24u);
    J3D_FP(0x802F55E0u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F55E4u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 24u);
    J3D_FP(0x802F55E8u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 28u);
    J3D_FP(0x802F55ECu); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F55F0u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 28u);
    J3D_FP(0x802F55F4u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 32u);
    J3D_FP(0x802F55F8u); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F55FCu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 32u);
    J3D_FP(0x802F5600u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 40u);
    J3D_FP(0x802F5604u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F5608u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 40u);
    J3D_FP(0x802F560Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 44u);
    J3D_FP(0x802F5610u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F5614u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 44u);
    J3D_FP(0x802F5618u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 48u);
    J3D_FP(0x802F561Cu); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F5620u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 48u);
j3d_maya_r29:
    /* label_802F5624 (-3): cmplwi r29&0xFF,1; bc 4,2 -> 56BC on !EQ */
    ctx->downcount -= 3;
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[29], 0u) & 0x000000FFu;
    leafrepl_commit_cr0(ctx, leafrepl_cr0_cmp_u32(ctx, ctx->gpr[0], 1u));
    if ((ctx->cr & 0x20000000u) == 0u) goto j3d_maya_concat;
    /* label_802F5630 (-83): scale stack mtx ROWS by the reciprocal of
     * the previous-scale vec (0x803EDBBC): row0 *= f2, row1 *= f3,
     * row2 *= f1 */
    ctx->downcount -= 83;
    J3D_FP(0x802F5630u);
    leafrepl_lfs_to(ctx, 1, ctx->gpr[2] + (u32)(s32)(-13072)); /* 1.0f */
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    J3D_FP(0x802F5638u);                                    /* lfsu */
    {
        const u32 ea = ctx->gpr[3] + (u32)(s32)(-9284);       /* 0x803EDBBC */
        leafrepl_lfs_to(ctx, 0, ea);
        ctx->gpr[3] = ea;
    }
    J3D_FP(0x802F563Cu); ppc_fdivs(ctx, 2, 1, 0);
    J3D_FP(0x802F5640u); leafrepl_lfs_to(ctx, 0, ctx->gpr[3] + 4u);
    J3D_FP(0x802F5644u); ppc_fdivs(ctx, 3, 1, 0);
    J3D_FP(0x802F5648u); leafrepl_lfs_to(ctx, 0, ctx->gpr[3] + 8u);
    J3D_FP(0x802F564Cu); ppc_fdivs(ctx, 1, 1, 0);
    J3D_FP(0x802F5650u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 8u);
    J3D_FP(0x802F5654u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F5658u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 8u);
    J3D_FP(0x802F565Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 12u);
    J3D_FP(0x802F5660u); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F5664u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 12u);
    J3D_FP(0x802F5668u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 16u);
    J3D_FP(0x802F566Cu); ppc_fmuls(ctx, 0, 0, 2);
    J3D_FP(0x802F5670u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 16u);
    J3D_FP(0x802F5674u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 24u);
    J3D_FP(0x802F5678u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F567Cu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 24u);
    J3D_FP(0x802F5680u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 28u);
    J3D_FP(0x802F5684u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F5688u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 28u);
    J3D_FP(0x802F568Cu); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 32u);
    J3D_FP(0x802F5690u); ppc_fmuls(ctx, 0, 0, 3);
    J3D_FP(0x802F5694u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 32u);
    J3D_FP(0x802F5698u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 40u);
    J3D_FP(0x802F569Cu); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F56A0u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 40u);
    J3D_FP(0x802F56A4u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 44u);
    J3D_FP(0x802F56A8u); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F56ACu); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 44u);
    J3D_FP(0x802F56B0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[1] + 48u);
    J3D_FP(0x802F56B4u); ppc_fmuls(ctx, 0, 0, 1);
    J3D_FP(0x802F56B8u); leafrepl_stfs(ctx, 0, ctx->gpr[1] + 48u);
j3d_maya_concat:
    /* label_802F56BC (-5): PSMTXConcat(0x803EDB80, r1+8, 0x803EDB80) */
    ctx->downcount -= 5;
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[3] += (u32)(s32)(-9344);                         /* 0x803EDB80 */
    ctx->gpr[4] = ctx->gpr[1] + (u32)(s32)(8);
    ctx->gpr[5] = ctx->gpr[3] | ctx->gpr[3];
    ctx->lr = 0x802F56D0u;
    if (j3drepl_mtxconcat(ctx)) return 1;
    /* label_802F56D0 (-4) then the 0x802F56E0 continuation (-6):
     * r4 = jointMtx + (r30 & 0xFFFF)*48; bl PSMTXCopy(0x803EDB80) */
    ctx->downcount -= 4;
    ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[28] + 56u);
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    ctx->gpr[3] += (u32)(s32)(-9344);                         /* 0x803EDB80 */
    ctx->gpr[4] = mem_read32_direct(ctx, ctx->gpr[4] + 140u);
    ctx->downcount -= 6;                                    /* 0x802F56E0 */
    ctx->gpr[0] = j3drepl_rotl32(ctx->gpr[30], 0u) & 0x0000FFFFu;
    ctx->gpr[0] = (u32)((s64)(s32)ctx->gpr[0] * (s64)(s32)48);
    ctx->gpr[4] = ctx->gpr[4] + ctx->gpr[0];
    ctx->lr = 0x802F56F0u;
    if (j3drepl_mtxcopy(ctx)) return 1;
    /* label_802F56F0 (-9): previous-scale vec = joint scale;
     * r11 = r1+80 */
    ctx->downcount -= 9;
    J3D_FP(0x802F56F0u); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 0u);
    ctx->gpr[3] = ((u32)(s32)(32831) << 16);
    J3D_FP(0x802F56F8u);                                    /* stfsu */
    {
        const u32 ea = ctx->gpr[3] + (u32)(s32)(-9284);       /* 0x803EDBBC */
        leafrepl_stfs(ctx, 0, ea);
        ctx->gpr[3] = ea;
    }
    J3D_FP(0x802F56FCu); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 4u);
    J3D_FP(0x802F5700u); leafrepl_stfs(ctx, 0, ctx->gpr[3] + 4u);
    J3D_FP(0x802F5704u); leafrepl_lfs_to(ctx, 0, ctx->gpr[31] + 8u);
    J3D_FP(0x802F5708u); leafrepl_stfs(ctx, 0, ctx->gpr[3] + 8u);
    ctx->gpr[11] = ctx->gpr[1] + 80u;
    /* 0x802F5710 bl 0x80328F84 (-6): restore r27-r31 */
    ctx->lr = 0x802F5714u;
    ctx->downcount -= 6;
    ctx->gpr[27] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-20));
    ctx->gpr[28] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-16));
    ctx->gpr[29] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-12));
    ctx->gpr[30] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-8));
    ctx->gpr[31] = mem_read32_direct(ctx, (u32)(ctx->gpr[11] + (u32)-4));
    /* label_802F5714 (-5): lwz r0,84(r1); mtlr; addi r1,80; blr */
    ctx->downcount -= 5;
    ctx->gpr[0] = mem_read32_direct(ctx, ctx->gpr[1] + 84u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] += (u32)80;
    ctx->pc = ctx->lr & ~3u;
    return 1;
}

/* Static membership prefilter for dolrecomp_dispatch_replacement: an exact
 * mirror of its switch case list. module_glue's cached-dispatch hit path uses
 * it to skip the dispatcher entirely for pcs that provably cannot claim,
 * while still running it for every member (verdicts can depend on runtime
 * args - only membership is static). MUST stay in sync with the switch
 * below: a case added there without a mirror here would wrongly suppress
 * claims. When mt-watch is armed the dispatcher's preamble must run on
 * every call, so the prefilter conservatively claims everything. */
int dolrecomp_replacement_may_claim(u32 address) {
    if (s_mt_watch)
        return 1;
    switch (address) {
    case 0x80006338u:
    case 0x80006464u:
    case 0x80006950u:
    case 0x80006C4Cu:
    case 0x80007BBCu:
    case 0x80008410u:
    case 0x8000AF2Cu:
    case 0x8000BC94u:
    case 0x8002451Cu:
    case 0x80025EA4u:
    case 0x80029E6Cu:
    case 0x80029EC8u:
    case 0x8002B634u:
    case 0x8002B778u:
    case 0x8003F19Cu:
    case 0x80053390u:
    case 0x800537C8u:
    case 0x8006AFBCu:
    case 0x8006B12Cu:
    case 0x8006F01Cu:
    case 0x8006FEE8u:
    case 0x800737F4u:
    case 0x80073C94u:
    case 0x80074324u:
    case 0x800754BCu:
    case 0x800A9684u:
    case 0x8017FD6Cu:
    case 0x8017FE10u:
    case 0x80180118u:
    case 0x801801C8u:
    case 0x801827A0u:
    case 0x80182A90u:
    case 0x80183048u:
    case 0x801831D8u:
    case 0x80183310u:
    case 0x80183428u:
    case 0x801836B4u:
    case 0x80183A90u:
    case 0x801874F4u:
    case 0x8022F7F8u:
    case 0x8022F804u:
    case 0x802305E0u:
    case 0x80230714u:
    case 0x802319B4u:
    case 0x80231A28u:
    case 0x80231A8Cu:
    case 0x80231D28u:
    case 0x80231F48u:
    case 0x802322A0u:
    case 0x80241178u:
    case 0x802411F8u:
    case 0x80244C28u:
    case 0x80244F44u:
    case 0x80245674u:
    case 0x802456C4u:
    case 0x80245714u:
    case 0x802457A8u:
    case 0x8024734Cu:
    case 0x8024A8E0u:
    case 0x802F5090u:
    case 0x802F52BCu:
    case 0x802F5508u:
    case 0x80255354u:
    case 0x80255570u:
    case 0x8027B8D4u:
    case 0x802915C4u:
    case 0x802916C0u:
    case 0x8029C1D8u:
    case 0x8029D134u:
    case 0x8029D560u:
    case 0x802B0338u:
    case 0x802B15D0u:
    case 0x802B1820u:
    case 0x802C4CECu:
    case 0x802C7C34u:
    case 0x802C7CD4u:
    case 0x802C7E30u:
    case 0x802C8088u:
    case 0x802D8BD8u:
    case 0x802D8C58u:
    case 0x802D8CC4u:
    case 0x80301E28u:
    case 0x803030ACu:
    case 0x803030D8u:
    case 0x80304E7Cu:
    case 0x80305424u:
    case 0x80305448u:
    case 0x803056BCu:
    case 0x80305890u:
    case 0x80305908u:
    case 0x80307EF4u:
    case 0x8030803Cu:
    case 0x803086A4u:
    case 0x80308B88u:
    case 0x80309BF0u:
    case 0x80309FD8u:
    case 0x8030B448u:
    case 0x8030D0FCu:
    case 0x8030DA44u:
    case 0x8030DA98u:
    case 0x8030DB24u:
    case 0x8030DCE0u:
    case 0x8030DD04u:
    case 0x8030DD28u:
    case 0x8030E0B4u:
    case 0x8030E478u:
    case 0x8030F2B0u:
    case 0x8030F618u:
    case 0x8030FADCu:
    case 0x8030FB9Cu:
    case 0x8030FBCCu:
    case 0x803115F0u:
    case 0x8031322Cu:
    case 0x80313270u:
    case 0x80313554u:
    case 0x80313ECCu:
    case 0x80314824u:
    case 0x80328DE8u:
    case 0x8033071Cu:
    case 0x80330C84u:
        return 1;
    default:
        return 0;
    }
}

extern u32 dolrecomp_call_depth;
static unsigned s_max_call_depth;

/* Claim thunks (module_glue MG_CLAIMCACHE fast path): glue_call caches one
 * of these as a positive entry's fn for the DOL case-list pcs below; a hit
 * then runs the case arm + original-on-decline directly, skipping the
 * dispatcher preamble and jump table. Each thunk replicates the preamble's
 * observable effects (RAM/EXRAM base refresh, g_rel_dispatched_fn reset),
 * runs the arm exactly as the switch does — same env gate, same call —
 * and on decline falls back to the original generated body. This TU cannot
 * see generated.h's dolrecomp_call_original, so the decline target is a
 * per-pc static the glue passes to dolrecomp_claim_thunk(pc, orig) at
 * populate time (glue resolved orig via find_original, so it is never NULL
 * when a thunk is installed). dolrecomp_claim_thunk returns NULL for pcs
 * without a thunk and whenever mt-watch is armed: the preamble's
 * mt_watch_sync_shadow() must run on every dispatch then.
 * 0x80328DE8 (__ptmf_scall) is deliberately absent — its case arm carries
 * the s_loadseam_trace dump ahead of the claim, which a thunk would skip. */
typedef void (*ClaimThunkFn)(CPUState*);
#define CLAIM_THUNK(fname, pcaddr, origvar, arm)                    \
    static ClaimThunkFn origvar;                                    \
    static void fname(CPUState* ctx) {                              \
        s_ram = ctx->ram;                                           \
        s_ram_size = ctx->ram_size;                                 \
        s_exram = ctx->exram;                                       \
        s_exram_size = ctx->exram_size;                             \
        g_rel_dispatched_fn = 0;                                    \
        arm                                                         \
        ctx->pc = (pcaddr);                                         \
        origvar(ctx);                                               \
    }

CLAIM_THUNK(thunk_judge_nd,   0x80244F44u, s_orig_judge_nd,
    if (s_repl_judge && judgerepl_claim(ctx, ctx->gpr[3], 5)) return;)
CLAIM_THUNK(thunk_judge_ls,   0x80244C28u, s_orig_judge_ls,
    if (s_repl_judge && judgerepl_ls(ctx)) return;)
CLAIM_THUNK(thunk_sin,        0x80330C84u, s_orig_sin,
    if (s_repl_libm) { ctx->fpr[1] = sin(ctx->fpr[1]); ctx->pc = ctx->lr; return; })
CLAIM_THUNK(thunk_cos,        0x8033071Cu, s_orig_cos,
    if (s_repl_libm) { ctx->fpr[1] = cos(ctx->fpr[1]); ctx->pc = ctx->lr; return; })
CLAIM_THUNK(thunk_mtxconcat,  0x8030D0FCu, s_orig_mtxconcat,
    if (s_repl_mtx && mtxrepl_psmtxconcat(ctx)) return;)
CLAIM_THUNK(thunk_idlepoll,   0x80307EF4u, s_orig_idlepoll,
    if (s_repl_idle && idlerepl_selectthread(ctx)) return;)
CLAIM_THUNK(thunk_chkgrp,     0x800A9684u, s_orig_chkgrp,
    if (s_repl_leaf && leafrepl_chkgrpthrough(ctx)) return;)
CLAIM_THUNK(thunk_ascent,     0x8022F7F8u, s_orig_ascent,
    if (s_repl_leaf && leafrepl_fontmetric(ctx, 10u)) return;)
CLAIM_THUNK(thunk_descent,    0x8022F804u, s_orig_descent,
    if (s_repl_leaf && leafrepl_fontmetric(ctx, 12u)) return;)
CLAIM_THUNK(thunk_xyz_add,    0x80245674u, s_orig_xyz_add,
    if (s_repl_leaf && leafrepl_cxyz(ctx, 0)) return;)
CLAIM_THUNK(thunk_xyz_sub,    0x802456C4u, s_orig_xyz_sub,
    if (s_repl_leaf && leafrepl_cxyz(ctx, 1)) return;)
CLAIM_THUNK(thunk_xyz_scale,  0x80245714u, s_orig_xyz_scale,
    if (s_repl_leaf && leafrepl_cxyz(ctx, 2)) return;)
CLAIM_THUNK(thunk_xyz_div,    0x802457A8u, s_orig_xyz_div,
    if (s_repl_leaf && leafrepl_cxyz(ctx, 3)) return;)
CLAIM_THUNK(thunk_samepid,    0x8024734Cu, s_orig_samepid,
    if (s_repl_leaf && leafrepl_chksameactorpid(ctx)) return;)
CLAIM_THUNK(thunk_aabcyl,     0x8024A8E0u, s_orig_aabcyl,
    if (s_repl_leaf && leafrepl_cross_aabcyl(ctx)) return;)
CLAIM_THUNK(thunk_fifo_pos,   0x802D8BD8u, s_orig_fifo_pos,
    if (s_repl_leaf && leafrepl_j3dfifo_posmtx(ctx)) return;)
CLAIM_THUNK(thunk_fifo_nrm,   0x802D8C58u, s_orig_fifo_nrm,
    if (s_repl_leaf && leafrepl_j3dfifo_nrmmtx(ctx)) return;)
CLAIM_THUNK(thunk_fifo_nrm3,  0x802D8CC4u, s_orig_fifo_nrm3,
    if (s_repl_leaf && leafrepl_j3dfifo_nrm3x3(ctx)) return;)
CLAIM_THUNK(thunk_mtxmultvec, 0x8030DA44u, s_orig_mtxmultvec,
    if (s_repl_leaf && leafrepl_psmtxmultvec(ctx)) return;)
CLAIM_THUNK(thunk_mtxmvarray, 0x8030DA98u, s_orig_mtxmvarray,
    if (s_repl_leaf && leafrepl_psmtxmultvecarray(ctx)) return;)
CLAIM_THUNK(thunk_mtxmvsr,    0x8030DB24u, s_orig_mtxmvsr,
    if (s_repl_leaf && leafrepl_psmtxmultvecsr(ctx)) return;)
CLAIM_THUNK(thunk_vecadd,     0x8030DCE0u, s_orig_vecadd,
    if (s_repl_leaf && leafrepl_psvecaddsub(ctx, 0)) return;)
CLAIM_THUNK(thunk_vecsub,     0x8030DD04u, s_orig_vecsub,
    if (s_repl_leaf && leafrepl_psvecaddsub(ctx, 1)) return;)
CLAIM_THUNK(thunk_vecscale,   0x8030DD28u, s_orig_vecscale,
    if (s_repl_leaf && leafrepl_psvecscale(ctx)) return;)
CLAIM_THUNK(thunk_sqdist,     0x8030E0B4u, s_orig_sqdist,
    if (s_repl_leaf && leafrepl_psvecsquaredistance(ctx)) return;)
CLAIM_THUNK(thunk_dcinvalidate, 0x803030ACu, s_orig_dcinvalidate,
    if (s_repl_leaf && leafrepl_dcinvalidate(ctx)) return;)
CLAIM_THUNK(thunk_dcflush,    0x803030D8u, s_orig_dcflush,
    if (s_repl_leaf && leafrepl_dcflush(ctx)) return;)
CLAIM_THUNK(thunk_j3d_basic,  0x802F5090u, s_orig_j3d_basic,
    if (s_repl_j3d && j3drepl_basic(ctx)) return;)
CLAIM_THUNK(thunk_j3d_si,     0x802F52BCu, s_orig_j3d_si,
    if (s_repl_j3d && j3drepl_softimage(ctx)) return;)
CLAIM_THUNK(thunk_j3d_maya,   0x802F5508u, s_orig_j3d_maya,
    if (s_repl_j3d && j3drepl_maya(ctx)) return;)

static const struct { u32 pc; ClaimThunkFn fn; ClaimThunkFn* orig; }
    s_claim_thunks[] = {
    { 0x800A9684u, thunk_chkgrp,     &s_orig_chkgrp },
    { 0x8022F7F8u, thunk_ascent,     &s_orig_ascent },
    { 0x8022F804u, thunk_descent,    &s_orig_descent },
    { 0x80244C28u, thunk_judge_ls,   &s_orig_judge_ls },
    { 0x80244F44u, thunk_judge_nd,   &s_orig_judge_nd },
    { 0x80245674u, thunk_xyz_add,    &s_orig_xyz_add },
    { 0x802456C4u, thunk_xyz_sub,    &s_orig_xyz_sub },
    { 0x80245714u, thunk_xyz_scale,  &s_orig_xyz_scale },
    { 0x802457A8u, thunk_xyz_div,    &s_orig_xyz_div },
    { 0x8024734Cu, thunk_samepid,    &s_orig_samepid },
    { 0x8024A8E0u, thunk_aabcyl,     &s_orig_aabcyl },
    { 0x802D8BD8u, thunk_fifo_pos,   &s_orig_fifo_pos },
    { 0x802D8C58u, thunk_fifo_nrm,   &s_orig_fifo_nrm },
    { 0x802D8CC4u, thunk_fifo_nrm3,  &s_orig_fifo_nrm3 },
    { 0x802F5090u, thunk_j3d_basic,  &s_orig_j3d_basic },
    { 0x802F52BCu, thunk_j3d_si,     &s_orig_j3d_si },
    { 0x802F5508u, thunk_j3d_maya,   &s_orig_j3d_maya },
    { 0x803030ACu, thunk_dcinvalidate, &s_orig_dcinvalidate },
    { 0x803030D8u, thunk_dcflush,    &s_orig_dcflush },
    { 0x80307EF4u, thunk_idlepoll,   &s_orig_idlepoll },
    { 0x8030D0FCu, thunk_mtxconcat,  &s_orig_mtxconcat },
    { 0x8030DA44u, thunk_mtxmultvec, &s_orig_mtxmultvec },
    { 0x8030DA98u, thunk_mtxmvarray, &s_orig_mtxmvarray },
    { 0x8030DB24u, thunk_mtxmvsr,    &s_orig_mtxmvsr },
    { 0x8030DCE0u, thunk_vecadd,     &s_orig_vecadd },
    { 0x8030DD04u, thunk_vecsub,     &s_orig_vecsub },
    { 0x8030DD28u, thunk_vecscale,   &s_orig_vecscale },
    { 0x8030E0B4u, thunk_sqdist,     &s_orig_sqdist },
    { 0x8033071Cu, thunk_cos,        &s_orig_cos },
    { 0x80330C84u, thunk_sin,        &s_orig_sin },
};

ClaimThunkFn dolrecomp_claim_thunk(u32 pc, ClaimThunkFn orig) {
    unsigned lo = 0, hi = (unsigned)(sizeof(s_claim_thunks) / sizeof(s_claim_thunks[0]));
    if (s_mt_watch || !orig)
        return (ClaimThunkFn)0;
    while (lo < hi) {
        const unsigned mid = (lo + hi) >> 1;
        if (pc < s_claim_thunks[mid].pc) hi = mid;
        else if (pc > s_claim_thunks[mid].pc) lo = mid + 1;
        else {
            *s_claim_thunks[mid].orig = orig;
            return s_claim_thunks[mid].fn;
        }
    }
    return (ClaimThunkFn)0;
}

int dolrecomp_dispatch_replacement(CPUState* ctx, u32 address) {
    if (dolrecomp_call_depth > s_max_call_depth) {
        s_max_call_depth = dolrecomp_call_depth;
        fprintf(stderr, "[depth] max_call_depth=%u\n", s_max_call_depth);
    }
    // Keep the journal RAM base current (CPUState is created once at boot).
    s_ram = ctx->ram;
    s_ram_size = ctx->ram_size;
    g_rel_dispatched_fn = 0;
    s_exram = ctx->exram; /* phase3-lreloc-p5b: enables the L-window in
                           * rel_any_ptr (see rel_exram_ptr). */
    s_exram_size = ctx->exram_size;
    /* p12 mt-watch: seed the old-value shadow once RAM is live (pre-guest,
     * so the first watched store records a true old value). */
    if (s_mt_watch)
        mt_watch_sync_shadow();

    switch (address) {
    case 0x8030B448u: /* WriteUARTN(buf, len) — diag */
        {
            static unsigned n;
            if (n < 8 || (n % 100u) == 0) {
                fprintf(stderr, "[uart] WriteUARTN buf=0x%08X len=0x%X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->gpr[4], ctx->lr, n);
                if (n < 8 && ctx->gpr[3] >= 0x80000000u && ctx->gpr[4] <= 0x4000u && s_ram) {
                    uint32_t base = ctx->gpr[3] - 0x80000000u;
                    if (base + ctx->gpr[4] <= s_ram_size) {
                        uint32_t i;
                        fprintf(stderr, "[uart]   content:");
                        for (i = 0; i < ctx->gpr[4] && i < 96; i++) {
                            uint8_t b = s_ram[base + i];
                            fprintf(stderr, "%c", (b >= 0x20 && b < 0x7f) ? b : '.');
                        }
                        fprintf(stderr, "\n[uart]   hex:");
                        for (i = 0; i < ctx->gpr[4] && i < 128; i++) {
                            fprintf(stderr, " %02X", s_ram[base + i]);
                        }
                        fprintf(stderr, "\n");
                    }
                }
                if (n < 4 && s_ram) {
                    uint32_t sp = ctx->gpr[1];
                    int depth;
                    for (depth = 0; depth < 16; depth++) {
                        uint32_t bsp;
                        uint32_t back, lrsave;
                        if (sp < 0x80000000u || sp >= 0x81800000u)
                            break;
                        bsp = sp - 0x80000000u;
                        if (bsp + 8 >= s_ram_size)
                            break;
                        back = read_be32(s_ram + bsp);
                        lrsave = read_be32(s_ram + bsp + 4);
                        if (lrsave >= 0x80000000u && lrsave < 0x81800000u)
                            fprintf(stderr, "[uart]   +%d ret=0x%08X\n", depth, lrsave);
                        if (back <= sp || back >= 0x81800000u)
                            break;
                        sp = back;
                    }
                }
            }
            n++;
        }
        break;
    case 0x80309BF0u: /* EXIImm(chan, buf, len, type, cb) — diag */
        {
            static unsigned n;
            if (n < 40)
                fprintf(stderr,
                        "[uart] EXIImm ch=%u buf=0x%08X len=%u type=%u cb=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7],
                        ctx->lr, n);
            n++;
        }
        break;
    case 0x80309FD8u: /* EXISync(chan) — diag */
        {
            static unsigned n;
            if (n < 40)
                fprintf(stderr, "[uart] EXISync ch=%u lr=0x%08X (call=%u)\n", ctx->gpr[3],
                        ctx->lr, n);
            n++;
        }
        break;
    case 0x80053390u: /* dComIfG_resLoad(phase, arcName) — translate L->R */
        {
            static unsigned n;
            u32 arc = ctx->gpr[4];
            u32 ci;
            for (ci = 0; ci < s_active_count; ci++) {
                const RelActiveModule* m = &s_active[ci];
                if (arc >= m->l && arc < m->l + m->file_size) {
                    ctx->gpr[4] = m->r + (arc - m->l);
                    if (s_debug)
                        fprintf(stderr,
                                "[resl] id=%u arc L=0x%08X -> R=0x%08X (heap-clobbered L "
                                "guard)\n",
                                m->id, arc, ctx->gpr[4]);
                    break;
                }
            }
            if (n < 200) {
                fprintf(stderr, "[resl] dComIfG_resLoad phase=0x%08X arc=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->gpr[4], ctx->lr, n);
                if (s_ram && ctx->gpr[4] >= 0x80000000u &&
                    ctx->gpr[4] - 0x80000000u + 8u <= s_ram_size) {
                    u32 b = ctx->gpr[4] - 0x80000000u;
                    fprintf(stderr, "[resl]   L bytes @0x%08X: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                            ctx->gpr[4],
                            s_ram[b], s_ram[b+1], s_ram[b+2], s_ram[b+3],
                            s_ram[b+4], s_ram[b+5], s_ram[b+6], s_ram[b+7]);
                }
            }
            n++;
        }
        break; /* run the original with the durable arc pointer */
    case 0x8006F01Cu: /* dRes_control_c::syncRes(arcName, pInfo, infoNum) — translate L->R */
        {
            static unsigned n;
            u32 arc = ctx->gpr[3];
            u32 ci;
            for (ci = 0; ci < s_active_count; ci++) {
                const RelActiveModule* m = &s_active[ci];
                if (arc >= m->l && arc < m->l + m->file_size) {
                    ctx->gpr[3] = m->r + (arc - m->l);
                    if (s_debug)
                        fprintf(stderr,
                                "[sync] id=%u arc L=0x%08X -> R=0x%08X (heap-clobbered L "
                                "guard)\n",
                                m->id, arc, ctx->gpr[3]);
                    break;
                }
            }
            if (n < 8) {
                fprintf(stderr, "[sync] syncRes arc=0x%08X info=0x%08X num=%u lr=0x%08X (call=%u)\n",
                        arc, ctx->gpr[3], ctx->gpr[5], ctx->lr, n);
                /* also dump the ORIGINAL (untranslated) L bytes for the known
                 * m_arc_name of module 60: L=0x80679080+0x5974=0x8067E9F4 */
                if (s_ram) {
                    u32 b = 0x8067E9F4u - 0x80000000u;
                    if (b + 8u <= s_ram_size)
                        fprintf(stderr, "[sync]   M60-L @0x8067E9F4: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                s_ram[b], s_ram[b+1], s_ram[b+2], s_ram[b+3],
                                s_ram[b+4], s_ram[b+5], s_ram[b+6], s_ram[b+7]);
                }
            }
            n++;
        }
        break; /* run the original with the durable arc pointer */
    case 0x8030F2B0u: /* DVDConvertPathToEntrynum(path) — diag */
        {
            static unsigned n;
            u32 path = ctx->gpr[3];
            if (n < 400 && path >= 0x80000000u && path < 0x82000000u && s_ram) {
                u32 b = path - 0x80000000u;
                char buf[80];
                u32 i;
                for (i = 0; i < 79 && b + i < s_ram_size && s_ram[b+i]; i++)
                    buf[i] = (char)s_ram[b+i];
                buf[i] = 0;
                fprintf(stderr, "[dvd] ConvertPath '%s' lr=0x%08X (call=%u)\n", buf, ctx->lr, n);
            }
            n++;
        }
        break;
    case 0x8030FADCu: /* DVDReadAsyncPrio(fileInfo, addr, length, offset, callback, priority) — diag */
        {
            static unsigned n;
            if (n < 400)
                fprintf(stderr, "[dvd] DVDReadAsyncPrio addr=0x%08X len=0x%X off=0x%X cb=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->lr, n);
            n++;
        }
        break;
    case 0x8030FB9Cu: /* cbForReadAsync(fileInfo) — diag */
        {
            static unsigned n;
            if (n < 400)
                fprintf(stderr, "[dvd] cbForReadAsync fileInfo=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->lr, n);
            if (s_seam_diag && s_ram) {
                u32 fi = ctx->gpr[3];
                u32 cb = 0;
                if (fi >= 0x80000000u &&
                    fi - 0x80000000u + 0x38u + 4u <= s_ram_size)
                    cb = read_be32(s_ram + (fi - 0x80000000u) + 0x38);
                fprintf(stderr, "[seam] cbForReadAsync #%u fi=0x%08X user_cb=0x%08X %s\n",
                        n, fi, cb,
                        (cb >= 0x80000000u && cb < 0x81800000u) ? "MEM1"
                        : (cb >= 0x90000000u && cb < 0x92000000u) ? "EXRAM"
                        : "?");
            }
            n++;
        }
        break;
    case 0x8030E478u: /* __DVDInterruptHandler(interrupt, context) — seam diag */
        {
            s_seam_isr_n++;
            if (s_seam_diag && ctx->external_read32) {
                /* Guest-style cause decode over live DI MMIO (dvdlow.c):
                 * reg=__DIRegs[0]; mask=reg&0x2a; intr=(reg&0x54)&(mask<<1);
                 * cause = 8|1|2 from cover/TC/ERROR bits. Log the first 32
                 * entries plus any entry whose decode is EMPTY — a zero
                 * cause here means the completion is acked into silence
                 * exactly as observed past the framework.str seam. */
                u32 reg0 = ctx->external_read32(ctx, 0xCC006000u, 0u);
                u32 mask = reg0 & 0x2Au;
                u32 intr = (reg0 & 0x54u) & (mask << 1);
                u32 cause = ((intr & 0x40u) ? 8u : 0u) |
                            ((intr & 0x10u) ? 1u : 0u) |
                            ((intr & 0x04u) ? 2u : 0u);
                if (s_seam_isr_n <= 32 || intr == 0u)
                    fprintf(stderr,
                            "[seam] ISR #%u lr=0x%08X di0=%08X mask=%02X "
                            "intr=%02X cause=%u\n",
                            s_seam_isr_n, ctx->lr, reg0, mask, intr, cause);
            }
        }
        break;
    case 0x80305908u: /* OSSendMessage(queue, msg, flags) — seam diag */
        {
            s_seam_sendmsg_n++;
            if (s_seam_diag &&
                (s_seam_sendmsg_n <= 32 || (s_seam_sendmsg_n & 63u) == 0)) {
                u32 q = ctx->gpr[3];
                fprintf(stderr,
                        "[seam] SendMsg #%u q=0x%08X msg=0x%08X flags=%u lr=0x%08X %s\n",
                        s_seam_sendmsg_n, q, ctx->gpr[4], ctx->gpr[5], ctx->lr,
                        (q >= 0x90000000u && q < 0x92000000u) ? "Q-EXRAM" : "");
            }
        }
        break;
    case 0x803115F0u: /* cbForStateBusy(intType) — seam diag */
        {
            static unsigned n;
            if (s_seam_diag && (n < 200 || (n & 511u) == 0))
                fprintf(stderr,
                        "[seam] cbForStateBusy #%u intType=%u lr=0x%08X\n",
                        n, ctx->gpr[3], ctx->lr);
            n++;
        }
        break;
    case 0x80308B88u: /* OSWakeupThread(threadQueue) — seam diag */
        {
            static unsigned n;
            u32 q = ctx->gpr[3];
            ++n;
            /* Skip the VI-retrace heartbeat queue so remaining volume fits
             * caps; anything else (DVD sync wakeups included) logs. */
            if (s_seam_diag && q != 0x803F7B44u &&
                (n <= 96 || (n & 255u) == 0))
                fprintf(stderr,
                        "[seam] WakeThread #%u q=0x%08X lr=0x%08X %s\n",
                        n, q, ctx->lr,
                        (q >= 0x90000000u && q < 0x92000000u) ? "Q-EXRAM" : "");
        }
        break;
    case 0x8027B8D4u: /* JASystem::Dvd::checkPassDvdT(p1, p2, cb) — diag */
        {
            static unsigned n;
            if (n < 12)
                fprintf(stderr, "[dvdT] checkPassDvdT p1=0x%08X p2=0x%08X cb=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->lr, n);
            n++;
        }
        break;
    case 0x802915C4u: /* JAInter::BankWave::finishSceneSet(p1) — diag */
        {
            static unsigned n;
            if (n < 12)
                fprintf(stderr, "[ws] finishSceneSet p1=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->lr, n);
            n++;
        }
        break;
    case 0x80006C4Cu: /* OSPanic(file, line, fmt, ...) — diag */
        {
            static unsigned n;
            u32 file = ctx->gpr[3];
            if (n < 8)
                fprintf(stderr, "[panic] OSPanic file=0x%08X line=%u lr=0x%08X (call=%u)\n",
                        file, ctx->gpr[4], ctx->lr, n);
            n++;
        }
        break;
    case 0x8029D560u: /* StreamLib::__start() — diag */
        {
            static unsigned n;
            if (n < 4 && s_ram) {
                u32 r13v = ctx->gpr[13];
                u32 sl = rel_rd32(r13v + 0x9570u); /* StreamMgr::streamList via SDA2 */
                u32 ios = rel_rd32(r13v + 0x9574u); /* StreamMgr::initOnCodeStrm via SDA2 */
                u32 seh = rel_rd32(0x803F75FCu); /* JAInter::SeMgr::seHandle */
                u32 i, j;
                fprintf(stderr, "[strm] __start r13=0x%08X streamList=0x%08X initOnCodeStrm=0x%08X abs803F7650=0x%08X seHandle=0x%08X (call=%u)\n", r13v, sl, ios, rel_rd32(0x803F7650u), seh, n);
                /* s_ram holds only s_ram_size bytes — bound reads against
                 * the buffer, not the guest address space. */
                if (sl >= 0x80000000u && sl - 0x80000000u < s_ram_size) {
                    for (i = 0; i < 3; i++) {
                        u32 e = sl + i*0x40u;
                        fprintf(stderr, "[strm]   entry%u field0x10: ", i);
                        for (j = 0; j < 24 && e + 0x10 + j - 0x80000000u < s_ram_size; j++) {
                            u8 c = s_ram[e + 0x10 + j - 0x80000000u];
                            if (c >= 32 && c < 127) fprintf(stderr, "%c", c);
                            else { fprintf(stderr, "\\x%02X", c); break; }
                        }
                        fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "[strm]   raw: ");
                    for (j = 0; j < 0x20 && sl + j - 0x80000000u < s_ram_size; j++)
                        fprintf(stderr, "%02X", s_ram[sl + j - 0x80000000u]);
                    fprintf(stderr, "\n");
                }
            }
            n++;
        }
        break;
    case 0x8029C1D8u: /* StreamMgr::checkWaitStream() — dump streamList state — diag */
        {
            static unsigned n;
            if (n < 4 && s_ram) {
                u32 sl = rel_rd32(0x803F7650u); /* StreamMgr::streamList */
                u32 ios = rel_rd32(0x803F7654u); /* StreamMgr::initOnCodeStrm */
                fprintf(stderr, "[strm] checkWaitStream streamList=0x%08X initOnCodeStrm=0x%08X (call=%u)\n",
                        sl, ios, n);
                if (sl >= 0x80000000u && sl < 0x82000000u) {
                    u32 b = sl - 0x80000000u;
                    u32 i;
                    fprintf(stderr, "[strm]   entry0: field0x0=0x%08X 0x4=0x%08X 0x8=0x%08X 0xc=0x%08X\n",
                            rel_rd32(sl), rel_rd32(sl+4), rel_rd32(sl+8), rel_rd32(sl+0xc));
                    fprintf(stderr, "[strm]   entry0 field0x10: ");
                    for (i = 0; i < 32 && b + 0x10 + i < s_ram_size; i++) {
                        u8 c = s_ram[b + 0x10 + i];
                        if (c >= 32 && c < 127) fprintf(stderr, "%c", c);
                        else { fprintf(stderr, "\\x%02X", c); }
                        if (c == 0) break;
                    }
                    fprintf(stderr, "\n");
                    fprintf(stderr, "[strm]   entry1 field0x10: ");
                    for (i = 0; i < 32 && b + 0x50 + i < s_ram_size; i++) {
                        u8 c = s_ram[b + 0x50 + i];
                        if (c >= 32 && c < 127) fprintf(stderr, "%c", c);
                        else { fprintf(stderr, "\\x%02X", c); }
                        if (c == 0) break;
                    }
                    fprintf(stderr, "\n");
                }
            }
            n++;
        }
        break;
    case 0x802916C0u: /* JAInter::BankWave::checkAllWaveLoadStatus() — title gate — diag */
        {
            static unsigned n;
            if (n < 20)
                fprintf(stderr, "[ws] checkAllWaveLoadStatus lr=0x%08X (call=%u)\n", ctx->lr, n);
            n++;
        }
        break;
    case 0x8030F618u: /* DVDOpen(path, finfo) — stream open — diag */
        {
            static unsigned n;
            u32 p1 = ctx->gpr[3];
            if (n < 8) {
                char b1[100];
                u32 i;
                for (i = 0; i < 99 && p1 - 0x80000000u + i < s_ram_size && s_ram[p1 - 0x80000000u + i]; i++)
                    b1[i] = (char)s_ram[p1 - 0x80000000u + i];
                b1[i] = 0;
                fprintf(stderr, "[strm] DVDOpen r13=0x%08X path='%s' finfo=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[13], b1, ctx->gpr[4], ctx->lr, n);
            }
            n++;
        }
        break;
    case 0x8030FBCCu: /* DVDReadPrio(finfo, addr, len, offset, prio) — stream header read — diag */
        {
            static unsigned n;
            if (n < 400)
                fprintf(stderr, "[strm] DVDReadPrio finfo=0x%08X addr=0x%08X len=0x%X off=0x%X prio=%u lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->lr, n);
            n++;
        }
        break;
    case 0x8029D134u: /* StreamLib::__Decode() — block-boundary diag
                       * (StreamLib::__DecodeADPCM 0x8029CD8C / __DecodePCM
                       * 0x8029CCD0; GC DSP ADPCM: per-call one 8-nibble block,
                       * predictor+scale header byte at adpcm_buffer + adpcm_loadpoint).
                       * Dumps the decode state at the block boundary so the
                       * audio A/B (AU-1) can prove ADPCM predictor parity at
                       * the block level. Diag-only, no behavior change. */
        {
            static unsigned n;
            if (n < 24 || (n % 256u) == 0) {
                u32 hdr = rel_rd16(0x803ED110u + 0xAu);   /* header.field_0xa (format) */
                u32 remain = rel_rd32(0x803F7658u);       /* adpcm_remain */
                u32 loadpt = rel_rd32(0x803F765Cu);       /* adpcm_loadpoint */
                u32 bufp = rel_rd32(0x803F7664u);         /* adpcm_buffer */
                u32 side = rel_rd32(0x803F7678u);         /* playside */
                u32 pbs = rel_rd32(0x803F767Cu);          /* playback_samples */
                u32 lus = rel_rd32(0x803F7680u);          /* loadup_samples */
                u32 bstate = rel_rd32(0x803F7684u);       /* adpcmbuf_state */
                if (hdr == 4u && s_ram && bufp >= 0x80000000u && bufp < 0x81800000u &&
                    loadpt < 0x200000u) {
                    /* __DecodeADPCM reads the predictor|scale header byte from
                     * adpcm_buffer[0] (r9 = adpcm_buffer at 0x8029CDE0; the
                     * per-block frame headers live at the buffer head). */
                    u32 aoff = bufp - 0x80000000u;
                    u8 psc = (aoff < s_ram_size) ? s_ram[aoff] : 0u;
                    fprintf(stderr,
                            "[adpcm] block#=%u loadpt=0x%05X remain=%u playside=%u "
                            "playback=%d loadup=%d state=%u ps=0x%02X (pred=%u scale=%u) lr=0x%08X\n",
                            n, loadpt, remain, side, (int)pbs, (int)lus, bstate, psc,
                            (psc >> 4) & 0xFu, psc & 0xFu, ctx->lr);
                } else {
                    fprintf(stderr, "[strm] __Decode hdr=0x%04X remain=%u loadpt=0x%05X "
                                    "playside=%u state=%u lr=0x%08X (call=%u)\n",
                            hdr, remain, loadpt, side, bstate, ctx->lr, n);
                }
            }
            n++;
        }
        break;
    case 0x802B15D0u: /* JKRExpHeap::create(size, parent, errorFlag) — map arena — diag */
        {
            static unsigned n;
            if (n < 8)
                fprintf(stderr, "[heap] JKRExpHeap::create size=0x%08X parent=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->gpr[4], ctx->lr, n);
            n++;
        }
        break;
    case 0x802B0338u: /* JKRHeap::initArena(&mem, &size, maxHeaps) — diag */
        {
            static unsigned n;
            u32 memp = ctx->gpr[3], sizep = ctx->gpr[4];
            if (n < 4 && s_ram && memp >= 0x80000000u && sizep >= 0x80000000u) {
                fprintf(stderr, "[heap] initArena mem@=0x%08X size@=0x%08X (call=%u)\n", memp, sizep, n);
            }
            n++;
        }
        break;
    case 0x802B1820u: /* JKRExpHeap::do_alloc(size, align) — heap may reuse L region */
        {
            /* B28/pulse12 R2: the in-flight do_alloc PRE-hook is GONE.
             * It carved while JKRExpHeap::create itself was mid-flight and
             * reshuffled which piece served that very allocation (pulse12
             * H1: archiveHeap ended up NULL at dScnLogo phase_0). Triggers
             * are now post-link sweep + dirty-flag only; nothing mutates
             * free lists inside an allocator frame. */
            /* JKRHeap: mStart at +0x30, mEnd at +0x34, mSize at +0x38 */
            static unsigned n;
            u32 hp = ctx->gpr[3];
            if (n < 12 && s_ram && hp >= 0x80000000u && hp < 0x82000000u) {
                fprintf(stderr, "[heap] do_alloc this=0x%08X size=0x%X align=0x%X start=0x%08X end=0x%08X (call=%u)\n",
                        hp, ctx->gpr[4], ctx->gpr[5],
                        rel_rd32(hp + 0x30u), rel_rd32(hp + 0x34u), n);
            }
            n++;
            /* The game heap arena spans the E1 batch linked bases; ANY
             * allocation can land on an active module's L region and
             * clobber its data (the JKR 0xFF/0x00 fill bypasses the guest
             * write journal — it is a host-side memset in the compiled DOL
             * chunk). Bump the heap generation: the REL chunk dispatcher
             * re-verifies (and refreshes if needed) L for modules whose
             * cached generation lags. */
            s_heap_gen++;
            if (s_heap_gen == 0u)
                s_heap_gen = 1u;
        }
            ++g_rel_dispatch_gen;
        break; /* run the original allocation */
    case 0x80305424u: /* OSLink */
        ctx->gpr[3] = rel_loader_link(ctx->gpr[3], ctx->gpr[4], 0);
        ctx->pc = ctx->lr;
        return 1;
    case 0x80305448u: /* OSLinkFixed */
        ctx->gpr[3] = rel_loader_link(ctx->gpr[3], ctx->gpr[4], 1);
        ctx->pc = ctx->lr;
        return 1;
    case 0x8030803Cu: /* OSCreateThread (p13 thr-watch; GZLE01
                       * symbols.txt:12946) */
        if (s_thr_watch)
            thr_watch_create(ctx);
        break; /* run the original create */
    case 0x803086A4u: /* OSResumeThread (p13 thr-watch; GZLE01
                       * symbols.txt:12951) */
        if (s_thr_watch)
            thr_watch_resume(ctx);
        break; /* run the original resume */
    case 0x803056BCu: /* OSUnlink */
        ctx->gpr[3] = rel_loader_unlink(ctx->gpr[3]);
        ctx->pc = ctx->lr;
        return 1;
    case 0x80304E7Cu: /* OSSetStringTable */
        rel_wr32(REL_LOADER_STRING_TABLE, ctx->gpr[3]);
        ctx->pc = ctx->lr;
        return 1;
    case 0x80305890u: /* __OSModuleInit */
        rel_wr32(REL_LOADER_QUEUE_HEAD, 0u);
        rel_wr32(REL_LOADER_QUEUE_TAIL, 0u);
        rel_wr32(REL_LOADER_STRING_TABLE, 0u);
        s_active_count = 0;
        s_active_seq++;
        ++g_rel_dispatch_gen;
        rel_loader_rebuild_journal_bounds();
        if (s_journal_filter_enabled)
            rel_loader_rebuild_journal_mask();
        s_qsig_head = s_qsig_tail = 0u;
        ctx->pc = ctx->lr;
        return 1;
    case 0x80301E28u: /* OSDefaultExceptionHandler(exception, context) — diag */
        {
            u32 exc = ctx->gpr[3];
            u32 cctx = ctx->gpr[4];
            u32 e;
            fprintf(stderr, "[exc] OSDefaultExceptionHandler exc=%u ctx=0x%08X\n", exc, cctx);
            if (cctx >= 0x80000000u && cctx < 0x82000000u) {
                fprintf(stderr, "[exc]   ctx srr0=0x%08X srr1=0x%08X cr=0x%08X lr=0x%08X\n",
                        rel_rd32(cctx + 0x198u), rel_rd32(cctx + 0x19Cu),
                        rel_rd32(cctx + 0x80u), rel_rd32(cctx + 0x84u));
            }
            fprintf(stderr, "[exc]   table@0x3000:");
            for (e = 0; e < 16; e++)
                fprintf(stderr, " [%u]=0x%08X", e, rel_rd32(0x80003000u + e * 4u));
            fprintf(stderr, "\n");
            fprintf(stderr, "[exc]   lomem C0=0x%08X D0=0x%08X D4=0x%08X D8=0x%08X\n",
                    rel_rd32(0x800000C0u), rel_rd32(0x800000D0u), rel_rd32(0x800000D4u),
                    rel_rd32(0x800000D8u));
        }
        break; /* run the original */
    case 0x80241178u: /* ModuleConstructorsX(void (**_ctors)()) */
        {
            /* Defensive: translate a linked table pointer to the durable R
             * copy. At normal link time the L mirror is still fresh, so this
             * is a no-op value-wise; on relink paths it keeps the game off
             * any stale L region.
             * p14 loadseam round 2 (root #7): the ctor walk on a RE-LINKED
             * module can hit a heap-clobbered L window before lazy-refresh
             * wins the race (frame-rate dependent — high-VI boots lose).
             * Mirror the dtors fix: refresh the L image from the durable R
             * copy before translating the table pointer. */
            u32 carg = ctx->gpr[3];
            u32 ci;
            for (ci = 0; ci < s_active_count; ci++) {
                RelActiveModule* m = &s_active[ci];
                if (carg >= m->l && carg < m->l + m->file_size) {
                    rel_loader_refresh_l_image(m);
                    ctx->gpr[3] = m->r + (carg - m->l);
                    if (s_debug)
                        fprintf(stderr,
                                "[rel_loader] ctors: id=%u _ctors L=0x%08X -> R=0x%08X (L "
                                "image refreshed)\n",
                                m->id, carg, ctx->gpr[3]);
                    break;
                }
            }
        }
        break; /* run the original with the translated, durable table */
    case 0x802411F8u: /* ModuleDestructorsX(void (**_dtors)()) */
        {
            /* The unload epilog reads the dtor table at the LINKED address,
             * but the L mirror is heap-clobbered by unload time (dtors run
             * after the game's arena reuses the linked region). Refresh the
             * L image from the durable R copy and translate the table
             * pointer to R. */
            u32 darg = ctx->gpr[3];
            u32 di;
            for (di = 0; di < s_active_count; di++) {
                const RelActiveModule* m = &s_active[di];
                if (darg >= m->l && darg < m->l + m->file_size) {
                    rel_loader_refresh_l_image(m);
                    ctx->gpr[3] = m->r + (darg - m->l);
                    if (s_debug)
                        fprintf(stderr,
                                "[rel_loader] dtors: id=%u _dtors L=0x%08X -> R=0x%08X (L "
                                "image refreshed)\n",
                                m->id, darg, ctx->gpr[3]);
                    break;
                }
            }
        }
        break; /* run the original with the translated, durable table */
    case 0x802C4CECu: /* JUTException::errorHandler(OSError, OSContext*, u32, u32) */
        {
            /* r3=error, r4=OSContext* (guest), srr0 at +0x198, lr at +0x84 */
            u32 err = ctx->gpr[3];
            u32 osctx = ctx->gpr[4];
            u32 srr0 = 0, lr = 0, cr = 0, gpr0 = 0;
            if (osctx >= 0x80000000u && osctx < 0x82000000u) {
                srr0 = rel_rd32(osctx + 0x198u);
                lr = rel_rd32(osctx + 0x84u);
                cr = rel_rd32(osctx + 0x80u);
                gpr0 = rel_rd32(osctx + 0x00u);
            }
            fprintf(stderr, "[JUTEX] error=%08x osctx=%08x srr0=%08x lr=%08x cr=%08x gpr0=%08x\n",
                    err, osctx, srr0, lr, cr, gpr0);
            /* do NOT consume — let the handler run so the crash screen shows */
        }
        break;
    case 0x80006950u: /* OSReport — capture guest debug/exception prints */
        {
            /* r3 = fmt (guest), r4-r9 = args. Forward a bounded rendering of
             * the format string to stderr for boot diagnostics (JUTException
             * prints the crash context here). Simplified: print the raw
             * format string pointer + first bytes; the full varargs render
             * would need the guest va_list layout. */
            u32 fmt = ctx->gpr[3];
            if (fmt == 0x803A09B8u) { /* "Warning: DVDOpen(): file '%s' was not found under %s." */
                /* Sized resolution: an unterminated string at the MEM1 tail
                 * would otherwise run the 128-char scan past the buffer. */
                const u8* a = rel_any_ptr_n(ctx->gpr[4], 128u);
                const u8* b = rel_any_ptr_n(ctx->gpr[5], 128u);
                char sa[129];
                char sb[129];
                u32 n;
                if (a) {
                    for (n = 0; n < 128 && a[n]; n++)
                        sa[n] = (char)a[n];
                    sa[n] = 0;
                } else {
                    sa[0] = 0;
                }
                if (b) {
                    for (n = 0; n < 128 && b[n]; n++)
                        sb[n] = (char)b[n];
                    sb[n] = 0;
                } else {
                    sb[0] = 0;
                }
                fprintf(stderr, "[OSReport] DVDOpen MISSING: '%s' under '%s'\n", sa, sb);
                /* DVD-FST diag: dump the OS boot info (physical 0) + the FST
                 * root entry so we can see what filesystem the game sees. */
                {
                    u32 boot_code = rel_rd32(0x80000020u);
                    u32 fst_loc = rel_rd32(0x80000038u);
                    u32 fst_max = rel_rd32(0x8000003Cu);
                    fprintf(stderr,
                            "[dvd-fst] boot_code=0x%08X fst_location=0x%08X fst_max=0x%08X\n",
                            boot_code, fst_loc, fst_max);
                    if (fst_loc >= 0x80000000u && fst_loc < 0x81800000u) {
                        u32 max_entries = rel_rd32(fst_loc + 8u);
                        u32 root = rel_rd32(fst_loc); /* isDirAndStringOff */
                        u32 root_parent = rel_rd32(fst_loc + 4u);
                        fprintf(stderr,
                                "[dvd-fst] FST@0x%08X max_entries=%u root=%08X (dir=%u strOff=%u) "
                                "parentOrNext=%08X\n",
                                fst_loc, max_entries, root, (root >> 24) & 0xFF, root & 0xFFFFFF,
                                root_parent);
                        /* walk the root directory's direct children (skip
                         * into each subdir via its nextEntryOrLength) */
                        if (max_entries > 0 && max_entries < 0x10000) {
                            u32 name_base = fst_loc + max_entries * 0xCu;
                            u32 e = 1;
                            u32 printed = 0;
                            u32 root_next = rel_rd32(fst_loc + 8u);
                            if (root_next == 0 || root_next > max_entries)
                                root_next = max_entries;
                            while (e < root_next && printed < 64u) {
                                printed++;
                                u32 hdr = rel_rd32(fst_loc + e * 0xCu);
                                u32 isdir = (hdr >> 24) & 0xFF;
                                u32 soff = hdr & 0xFFFFFF;
                                const u8* np = rel_any_ptr_n(name_base + soff, 64u);
                                char nm[65];
                                u32 nn = 0;
                                if (np) {
                                    while (nn < 64 && np[nn]) { nm[nn] = (char)np[nn]; nn++; }
                                }
                                nm[nn] = 0;
                                u32 sub_next = isdir ? rel_rd32(fst_loc + e * 0xCu + 8u) : 0;
                                fprintf(stderr,
                                        "[dvd-fst]   e%u %s '%s' next=%u%s\n", e,
                                        isdir ? "DIR" : "FILE", nm, sub_next,
                                        (isdir && sub_next == 0) ? " (EMPTY?)" : "");
                                if (isdir) {
                                    if (sub_next == 0 || sub_next <= e || sub_next > max_entries)
                                        break;
                                    e = sub_next;
                                } else {
                                    e++;
                                }
                            }
                        }
                    } else {
                        fprintf(stderr, "[dvd-fst] FST not mapped (no RAM)\n");
                    }
                }
            }
            if (fmt >= 0x80000000u && fmt < 0x82000000u) {
                const u8* p = rel_any_ptr_n(fmt, 256u);
                fprintf(stderr, "[OSReport] r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x\n",
                        fmt, ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                        ctx->gpr[7], ctx->gpr[8], ctx->gpr[9]);
                if (p) {
                    char buf[257];
                    u32 n = 0;
                    while (n < 256 && p[n] && p[n] != '\n') { buf[n] = (char)p[n]; n++; }
                    buf[n] = 0;
                    fprintf(stderr, "[OSReport] %s\n", buf);
                }
            }
            ctx->pc = ctx->lr;
            return 1;
        }
    /* p14 loadseam (Layer B) — the four file-select decision points
     * (load-seam-analysis.md §3). Each logs the exact decision inputs and
     * returns 0 so the ORIGINAL dispatch path runs untouched: observation
     * hooks, not replacements. All field reads go through rel_rd32 on the
     * dFs_c instance (heap object, d_s_name.cpp:117) — its pointer is not
     * a static global, so P1/P2 log entry-only and P3/P4 read the fields
     * through the self pointer in r3 (this). */
    case 0x80182A90u: /* P1: dFile_select_c::menuSelect (framework.txt:6113) */
        if (s_loadseam_trace) {
            /* Internals (menuSelect entry — PPC passes this in r3; r31 is
             * the CALLER's saved frame until menuSelect's own prologue runs,
             * so self MUST come from gpr[3] here, never gpr[31]):
             * Field offsets from the decompiled body (chunk_0096):
             *   +14604 = dFs_c* (self->dFs)
             *   +14632 = u8 state machine (0=initial,1..3=panes; incremented
             *             on A at 80182E08, reset to 0 at 80182BC8/80182ED4)
             *   +14626 = u8 slot index (added to this for per-slot fields)
             *   +14612 = u8[slot] per-slot flag array base
             *   +14636 = u8 "A fired" latch (set 1 at 80182BC8 on A)
             *   +14635 = u8 next-state (38 = exit-to-copy path)
             *   +14640 = u16 timer/anim counter (reset on transitions)
             * LR 0x8017FDF0 = caller dFile_select_c::move (the framework
             * pane driver), so menuSelect runs every frame while the
             * dataSelect pane is live. */
            const u32 self = ctx->gpr[3];
            u32 v14632 = 0u, v14626 = 0u, v14636 = 0u, v14635 = 0u;
            u32 slotflag = 0u;
            u32 stA = 0u, stB = 0u, stStart = 0u;
            if (self >= 0x80000000u && self < 0x82000000u) {
                v14632 = rel_rd8(self + 14632u);
                v14626 = rel_rd8(self + 14626u);
                v14636 = rel_rd8(self + 14636u);
                v14635 = rel_rd8(self + 14635u);
                const u32 slotbase = self + v14626;
                if (slotbase >= 0x80000000u && slotbase < 0x82000000u)
                    slotflag = rel_rd8(slotbase + 14612u);
            }
            /* LRELOC-13 discriminator: sample the STControl raw button bytes
             * every menuSelect frame. STControl global base = 0x803A4DF0
             * (lis r3,-32710 => 0x803A0000; addi 19952 => +0x4DF0). Byte+50
             * bit0 = A, bit6 = B (d_lib mapping). Earlier probe read
             * 0x801A4E22 — a lis-arithmetic error (0x803A misread as
             * 0x801A) — and sampled function-body bytes; those readings are
             * void. */
            stA = rel_rd8(0x803A4E22u) & 1u;
            stB = (rel_rd8(0x803A4E22u) >> 6) & 1u;
            stStart = rel_rd8(0x803A4E23u);
            /* dFs bridge fields (dFile_select heap instance): menuSelect's
             * A-branch writes the handoff; the scene consumer (whatever
             * replaces FileSelectMainNormal) reads them. Log every frame so
             * the handoff write is visible even if the consuming proc is
             * never identified. */
            u32 fs3928 = 0xFFFFFFFFu, fs392c = 0xFFFFFFFFu;
            if (self >= 0x80000000u && self < 0x82000000u) {
                const u32 scn = rel_rd32(self + 14604u); /* dScnName back-ptr? */
                (void)scn;
                fs3928 = rel_rd8(self + 14636u); /* alatch mirror */
                fs392c = rel_rd8(self + 14635u); /* nstate mirror */
            }
            fprintf(stderr,
                    "[loadseam] P1 menuSelect lr=0x%08X this=0x%08X state=%u "
                    "slot=%u slotflag=%u alatch=%u nstate=%u stA=%u stB=%u "
                    "stStart=%02X fs3928=%02X fs392c=%02X\n",
                    ctx->lr, self, v14632, v14626, slotflag, v14636, v14635,
                    stA, stB, stStart, fs3928, fs392c);
        }
        break; /* run the original menuSelect */
    case 0x801827A0u: /* P5: dataSelectPaneMove — pane navigation proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P5 dataSelectPaneMove entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x80183048u: /* P6: ToCopyPaneMove — copy-pane transition proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P6 ToCopyPaneMove entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x801831D8u: /* P7: ToErasePaneMove — erase-pane transition proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P7 ToErasePaneMove entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x80183310u: /* P8: ToBackPaneMove — back transition proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P8 ToBackPaneMove entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x801874F4u: /* P2: dFile_select_c::YesNoSelect (framework.txt:6134) */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P2 YesNoSelect entry lr=0x%08X\n", ctx->lr);
        break; /* run the original YesNoSelect */
    case 0x80231A8Cu: /* P3: dScnName_c::FileSelectMainNormal (framework.txt:7880) */
        if (s_loadseam_trace) {
            const u32 self = ctx->gpr[3]; /* PPC: self is r3 */
            u32 v392c = 0u, v3928 = 0u, v3917 = 0u, v3914 = 0u, slot = 0u;
            if (self >= 0x80000000u && self < 0x82000000u) {
                /* dScnName_c::dFs_c @+0x428 (d_s_name.h:150) -> dFile_select_c
                 * heap instance (d_s_name.cpp:117). Field layout from
                 * d_file_select.h: field_0x3914 u8[3] @+0x3914, field_0x3917
                 * u8[3] @+0x3917, saveSlot @+0x3922, field_0x3928 @+0x3928,
                 * field_0x392c @+0x392c. */
                const u32 fs = rel_rd32(self + 0x428u);
                if (fs >= 0x80000000u && fs < 0x82000000u) {
                    v3914 = rel_rd32(fs + 0x3914u);
                    v3917 = rel_rd32(fs + 0x3917u);
                    slot = rel_rd8(fs + 0x3922u);
                    v3928 = rel_rd8(fs + 0x3928u);
                    v392c = rel_rd8(fs + 0x392cu);
                }
            }
            fprintf(stderr,
                    "[loadseam] P3 FileSelectMainNormal 392c=%u 3928=%u "
                    "3917=%06X 3914=%06X slot=%u self=0x%08X\n",
                    v392c, v3928, v3917 & 0xFFFFFFu, v3914 & 0xFFFFFFu,
                    slot, self);
        }
        break; /* run the original FileSelectMainNormal */
    case 0x802322A0u: /* P4: dScnName_c::changeGameScene (framework.txt:7898) */
        if (s_loadseam_trace) {
            const u32 self = ctx->gpr[3]; /* PPC: self is r3 */
            u32 v55f = 0u;
            u32 v392c = 0u;
            if (self >= 0x80000000u && self < 0x82000000u) {
                /* dScnName_c::field_0x55f (d_s_name.h:170): 0=PLAY, 1=OPEN. */
                v55f = rel_rd8(self + 0x55Fu);
                const u32 fs = rel_rd32(self + 0x428u);
                if (fs >= 0x80000000u && fs < 0x82000000u) {
                    v392c = rel_rd8(fs + 0x392cu);
                }
            }
            fprintf(stderr,
                    "[loadseam] P4 changeGameScene entry self=0x%08X "
                    "field_0x55f=%u (0=PLAY, 1=OPEN) 392c=%u peek_check_follows\n",
                    self, v55f, v392c);
        }
        break; /* run the original changeGameScene */
    case 0x802305E0u: { /* P9: dScnName_c::execute — scene-level execute */
        const u32 self = ctx->gpr[3]; /* PPC: self is r3 */
        s16 phase = 0;
        u8 mmain = 0u;
        if (self >= 0x80000000u && self < 0x82000000u) {
            phase = (s16)rel_rd16(self + 8u); /* scene phase field (this+8, s16) */
            mmain = rel_rd8(self + 1364u); /* mMainProc (this+1364, u8) — the
                                            * execute dispatcher selects
                                            * mProc[mmain] at 80230650. */
        }
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P9 dScnName execute entry lr=0x%08X phase=%d mmain=%u\n",
                    ctx->lr, (int)phase, mmain);
        break;
    }
    case 0x80231A28u: { /* P18: FileSelectMain caller fn (call site 0x80231A60) */
        const u32 self = ctx->gpr[3];
        s16 phase = 0;
        if (self >= 0x80000000u && self < 0x82000000u)
            phase = (s16)rel_rd16(self + 8u);
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P18 FileSelectMainCaller entry lr=0x%08X phase=%d\n",
                    ctx->lr, (int)phase);
        break;
    }
    case 0x802319B4u: { /* P20: FileSelectMain precheck fn — contains the
                          * 0x802319CC checkTrigger call site. Logs the scene
                          * phase (this+8, s16) every entry to correlate with
                          * P18 rows and the checkTrigger gate. */
        const u32 self = ctx->gpr[3];
        s16 phase = 0;
        if (self >= 0x80000000u && self < 0x82000000u)
            phase = (s16)rel_rd16(self + 8u);
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P20 precheck entry this=0x%08X phase=%d\n",
                    self, (int)phase);
        break;
    }
    case 0x8017FE10u: { /* P19: STControl checkTrigger — gated on lr being
                          * 0x802319D0 (the FileSelectMain call site) to avoid
                          * log flooding from other callers. Logs the trigger
                          * state bytes (this+12, this+13) and the raw button
                          * bytes at the STControl global 0x803A4E22/3. */
        if (ctx->lr == 0x802319D0u) {
            const u32 self = ctx->gpr[3];
            u8 t12 = 0u, t13 = 0u, stA = 0u, stB = 0u;
            if (self >= 0x80000000u && self < 0x82000000u) {
                t12 = rel_rd8(self + 12u);
                t13 = rel_rd8(self + 13u);
            }
            stA = rel_rd8(0x803A4E22u);
            stB = rel_rd8(0x803A4E23u);
            if (s_loadseam_trace)
                fprintf(stderr,
                        "[loadseam] P19 checkTrigger this=0x%08X t12=%u t13=%u stA=%u stB=%u\n",
                        self, t12, t13, stA, stB);
        }
        break;
    }
    case 0x80230714u: /* P10: dScnName_c::draw — scene-level draw */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P10 dScnName draw entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x80029E6Cu: /* P11: fopScnM_ChangeReq — scene change request */
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P11 fopScnM_ChangeReq scene=0x%08X req=%d pt=%d mode=%u\n",
                    ctx->gpr[3], (s32)ctx->gpr[4], (s32)ctx->gpr[5],
                    (ctx->gpr[6] >> 16) & 0xFFFFu);
        break;
    case 0x8017FD6Cu: /* P12: dFile_select_c::_move — the pane dispatcher.
                        * Reads the global pane byte (0x803B4FFA = ST base
                        * +5722 via 0x803B39A0) and _ptmf_scall's the proc.
                        * Log pane byte + alatch every dispatch: if P12 rows
                        * CONTINUE after the A frame while P1 stopped, the
                        * dispatcher is alive and the pane byte still 2, yet
                        * the ptmf call stopped reaching menuSelect — the
                        * stall is inside _move's dispatch. If P12 rows also
                        * stop, the framework stopped scheduling the process. */
        if (s_loadseam_trace) {
            const u8 pane = rel_rd8(0x803B4FFAu);
            const u32 self = ctx->gpr[3];
            u8 alatch = 0u, nstate = 0u;
            u16 v3930 = 0u;
            u8 v392e = 0u, v392c = 0u, v392b = 0u, v392a = 0u;
            if (self >= 0x80000000u && self < 0x82000000u) {
                alatch = rel_rd8(self + 14636u);
                nstate = rel_rd8(self + 14635u);
                /* close-chain instrumentation (P21-P23 probe): dFs_c lives at
                 * self+0x428 (same pointer dScnName uses for 392c reads). */
                const u32 fs = rel_rd32(self + 0x428u);
                if (fs >= 0x80000000u && fs < 0x82000000u) {
                    v3930 = rel_rd16(fs + 14640u);
                    v392e = rel_rd8(fs + 14638u);
                    v392c = rel_rd8(fs + 14636u);
                    v392b = rel_rd8(fs + 14635u);
                    v392a = rel_rd8(fs + 14634u);
                }
            }
            fprintf(stderr,
                    "[loadseam] P12 move pane=%u this=0x%08X alatch=%u nstate=%u "
                    "fs=0x%08X c3930=%u c392e=%u c392c=%u c392b=%u c392a=%u\n",
                    pane, self, alatch, nstate, rel_rd32(self + 0x428u),
                    v3930, v392e, v392c, v392b, v392a);
        }
        break;
    case 0x80231D28u: { /* P21: dScnName_c::FileSelectClose — MainProc[5].
                         * Ride-20 verdict says the post-A stall lives here or
                         * below. Logs _close()'s return byte (r3) plus the
                         * close-substate fields read through the dFs_c pointer
                         * at self+0x428. */
        const u32 self = ctx->gpr[3];
        u8 v392e = 0u, v392c = 0u, v392a = 0u;
        u16 v3930 = 0u;
        if (self >= 0x80000000u && self < 0x82000000u) {
            const u32 fs = rel_rd32(self + 0x428u);
            if (fs >= 0x80000000u && fs < 0x82000000u) {
                v3930 = rel_rd16(fs + 14640u);
                v392e = rel_rd8(fs + 14638u);
                v392c = rel_rd8(fs + 14636u);
                v392a = rel_rd8(fs + 14634u);
            }
        }
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P21 FileSelectClose entry this=0x%08X fs=0x%08X "
                    "c3930=%u c392e=%u c392c=%u c392a=%u\n",
                    self, rel_rd32(self + 0x428u), v3930, v392e, v392c, v392a);
        break;
    }
    case 0x80180118u: { /* P22: dFile_select_c::_close — the close dispatcher.
                         * Runs every frame while mMainProc==5. ret (r3 at
                         * entry is `this`; the return byte is only observable
                         * in the caller, so log the branch selector instead:
                         * 392e (error path), 392a (error kind), 392c (which
                         * close sub-anim runs), and the 3930 counter. */
        const u32 self = ctx->gpr[3];
        u16 v3930 = 0u;
        u8 v392e = 0u, v392c = 0u, v392b = 0u, v392a = 0u;
        if (self >= 0x80000000u && self < 0x82000000u) {
            v3930 = rel_rd16(self + 14640u);
            v392e = rel_rd8(self + 14638u);
            v392c = rel_rd8(self + 14636u);
            v392b = rel_rd8(self + 14635u);
            v392a = rel_rd8(self + 14634u);
        }
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P22 dFs_close this=0x%08X c3930=%u c392e=%u "
                    "c392c=%u c392b=%u c392a=%u\n",
                    self, v3930, v392e, v392c, v392b, v392a);
        break;
    }
    case 0x801801C8u: { /* P23: dFile_select_c::closeEnd — the 7-pane close
                         * transition stepper. Runs only on the non-error
                         * non-back path (392c!=3, 392e==0). If P23 rows keep
                         * incrementing c3930 while P21 never sees ret==1, the
                         * stall is inside a PaneTrance* that never completes. */
        const u32 self = ctx->gpr[3];
        u16 v3930 = 0u;
        u8 saveSlot = 0u;
        if (self >= 0x80000000u && self < 0x82000000u) {
            v3930 = rel_rd16(self + 14640u);
            saveSlot = rel_rd8(self + 14634u);
        }
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P23 closeEnd this=0x%08X c3930=%u saveSlot=%u\n",
                    self, v3930, saveSlot);
        break;
    }
    case 0x80183428u: /* P13: copyDataToSelect — copy-data selection proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P13 copyDataToSelect entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x801836B4u: /* P14: copyDataSelAnime — copy-selection anim proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P14 copyDataSelAnime entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x80183A90u: /* P15: copyToSelBack — copy-to-select back proc */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P15 copyToSelBack entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x80029EC8u: /* P16: fopScnM_DeleteReq — scene deletion request */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P16 fopScnM_DeleteReq scene=0x%08X\n", ctx->gpr[3]);
        break;
    case 0x8003F19Cu: /* P17: fpcNd_Delete — process node deletion */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P17 fpcNd_Delete node=0x%08X\n", ctx->gpr[3]);
        break;
    case 0x80231F48u: { /* P25: dScnName_c::NameInMain — MainProc for the
                          * name-in/main fileselect phase (runs after the
                          * FileSelectClose chain). Logs the dFs bridge
                          * fields so the load-confirm handoff (3928/392c)
                          * is visible from the scene side. */
        const u32 self = ctx->gpr[3];
        u32 fs = 0u;
        u8 v3928 = 0u, v392c = 0u;
        if (self >= 0x80000000u && self < 0x82000000u)
            fs = rel_rd32(self + 0x428u);
        if (fs >= 0x80000000u && fs < 0x82000000u) {
            v3928 = rel_rd8(fs + 14632u);
            v392c = rel_rd8(fs + 14636u);
        }
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P25 NameInMain entry this=0x%08X fs=0x%08X "
                    "3928=%u 392c=%u\n",
                    self, fs, v3928, v392c);
        break;
    }
    case 0x80328DE8u: { /* P24: _ptmf_scall — the C++ ptr-to-member dispatch
                          * funnel. dScnName execute calls this with
                          * (this, ptmf) to run mProc[mmain]. Gate the log to
                          * this==dScnName-ish range AND lr==0x80230654 (the
                          * execute call site) to avoid flooding from other
                          * ptmf users. Logs the ptmf function/index encoding:
                          * PPC ptmf = { u32 index; u32 function; } — index
                          * selects the vtable slot, function is the fn ptr. */
        if (ctx->lr == 0x80230654u && s_loadseam_trace) {
            const u32 self = ctx->gpr[3];
            /* PPC ptmf call: r12 = &mProc[mmain] (base 0x803941FC, stride
             * 12: {u32 func; u32 voff; u32 adj}). Read the target from the
             * ptmf struct, not from the instance. */
            const u32 ptmf_addr = ctx->gpr[12];
            /* ptmf struct is 12 bytes; the function pointer's offset within
             * it is compiler-dependent — dump all three words so the ride
             * shows which slot holds 0x80231A28 (FileSelectMain) for
             * mmain=4. */
            const u32 w0 = (ptmf_addr >= 0x80000000u &&
                            ptmf_addr < 0x82000000u)
                               ? rel_rd32(ptmf_addr) : 0u;
            const u32 w1 = (ptmf_addr >= 0x80000000u &&
                            ptmf_addr + 4u < 0x82000000u)
                               ? rel_rd32(ptmf_addr + 4u) : 0u;
            const u32 w2 = (ptmf_addr >= 0x80000000u &&
                            ptmf_addr + 8u < 0x82000000u)
                               ? rel_rd32(ptmf_addr + 8u) : 0u;
            fprintf(stderr,
                    "[loadseam] P24 ptmf this=0x%08X ptmf=0x%08X "
                    "w0=%08X w1=%08X w2=%08X\n",
                    self, ptmf_addr, w0, w1, w2);
        }
        /* In-module replacement of the resolver itself: the trace above
         * only reads RAM (rel_rd32) and runs before the claim, so a
         * claimed call still logs. leafrepl_ptmf_scall replays the
         * generated chunk_0201 body — 3 ptmf loads, this-adjust, the
         * r11<0 direct/vtable select, mtctr+bctr — charges -6/-2/-3.
         * MODERNGEKKO_REPL_LEAF=0 off. */
        if (s_repl_leaf && leafrepl_ptmf_scall(ctx)) return 1;
        break;
    }
    case 0x80074324u: { /* P30: dEvent_manager_c::runProc — EDGE-TRIGGERED
                          * state census (evm-diagnosis.md C6). Every call,
                          * scan all evNum states; print ONLY transitions
                          * (ev[i] old→new + name) — catches the 1-2-frame
                          * event lifecycle the 500-call sampling aliased
                          * past. camPlay 1→0 marks endProc running.
                          * On any event entering PLAY, also dump its
                          * mFlagCheckFinish[0] dword (ev+0x88): -1 while
                          * in PLAY = instant-finish by data (H9a). */
        if (s_loadseam_trace) {
            static u32 p30_last[96];
            static u32 p30_cam_last;
            static int p30_init;
            const u32 thisp = ctx->gpr[3];
            const u32 hdr = rel_rd32(thisp + 0x00);
            const u32 evP = rel_rd32(thisp + 0x04);
            const s32 evNum = hdr ? (s32)rel_rd32(hdr + 0x04) : 0;
            const u32 camPlay = rel_rd32(thisp + 0x20);
            int i;
            if (!p30_init) {
                for (i = 0; i < 96; i++) p30_last[i] = 0xFFFFFFFEu;
                p30_init = 1;
            }
            if (camPlay != p30_cam_last) {
                fprintf(stderr, "[loadseam] P30 camPlay %u->%u\n",
                        p30_cam_last, camPlay);
                p30_cam_last = camPlay;
            }
            for (i = 0; i < evNum && i < 96; i++) {
                const u32 ev = evP + (u32)i * 0xB0u;
                const u32 state = (u32)rel_rd32(ev + 0xA4);
                if (state != p30_last[i]) {
                    char name[33];
                    int k;
                    for (k = 0; k < 32; k++) {
                        const char c = (char)rel_rd8(ev + (u32)k);
                        name[k] = (c == 0 || c < 0x20) ? 0 : c;
                        if (name[k] == 0)
                            break;
                    }
                    name[k < 32 ? k : 32] = 0;
                    fprintf(stderr,
                            "[loadseam] P30 EDGE ev[%d] %u->%u name='%s' "
                            "nStaff=%d finish0=%08X\n",
                            i, p30_last[i], state, name,
                            (s32)rel_rd32(ev + 0x7C),
                            state == 2u ? rel_rd32(ev + 0x88) : 0u);
                    p30_last[i] = state;
                }
            }
        }
        break;
    }
    case 0x80073C94u: { /* P27: dEvent_manager_c::create — proves the
                          * event_list.dat binding (cross-chunk from
                          * dStage_Create). One row per stage load. */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P27 evmng create this=0x%08X lr=0x%08X\n",
                    ctx->gpr[3], ctx->lr);
        break;
    }
    case 0x8002451Cu: { /* P31: fopAcM_create — the cross-chunk actor-create
                          * point (framework.map:651; called cross-chunk
                          * from specialProcCreate 0x80072A0C per
                          * chunk_0028:13622-13628). lr==0x80072A10 marks
                          * a create FROM the event staff = the Auzu/Ajav
                          * path. Zero such rows in the storm window = the
                          * CREATE cut is never reached. */
        if (s_loadseam_trace) {
            static unsigned p31_n, p31_ev;
            const s16 prof = (s16)(ctx->gpr[3] & 0xFFFFu);
            if (ctx->lr == 0x80072A10u) {
                p31_ev++;
                fprintf(stderr,
                        "[loadseam] P31 fopAcM_create FROM-STAFF prof=%d(0x%X) "
                        "param=0x%08X lr=0x%08X (staff-creates=%u)\n",
                        prof, ctx->gpr[3] & 0xFFFFu, ctx->gpr[4],
                        ctx->lr, p31_ev);
            } else if (p31_n < 20) {
                fprintf(stderr,
                        "[loadseam] P31 fopAcM_create prof=%d(0x%X) "
                        "param=0x%08X lr=0x%08X\n",
                        prof, ctx->gpr[3] & 0xFFFFu, ctx->gpr[4], ctx->lr);
            }
            p31_n++;
        }
        break;
    }
    case 0x8002B634u: /* P32: fopMsgM_messageSet(u32, fopAc_ac_c*) */
    case 0x8002B778u: { /* fopMsgM_messageSet(u32, cXyz*) — message-gate
                          * witness (H5): repeated P32 rows with the same
                          * message number and no P30 progress = the storm
                          * timeline is message-frozen. */
        if (s_loadseam_trace)
            fprintf(stderr, "[loadseam] P32 msgSet no=0x%08X lr=0x%08X\n",
                    ctx->gpr[3], ctx->lr);
        break;
    }
    case 0x80025EA4u: { /* P33: fopAcM_orderOtherEventId(actor, eventIdx,
                          * ...) — map:691, cross-chunk from exceptionProc
                          * 80074C8C (chunk_0028:31723-31730). r4 =
                          * eventIdx (s16) being ordered: -1 = the
                          * exception's getEventIdx name-resolve FAILED.
                          * r3 = actor (NULL from the exception path). */
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P33 orderOtherEventId actor=0x%08X "
                    "eventIdx=%d infoIdx=%u lr=0x%08X\n",
                    ctx->gpr[3], (s16)(ctx->gpr[4] & 0xFFFFu),
                    ctx->gpr[5] & 0xFFu, ctx->lr);
        break;
    }
    case 0x8006FEE8u: { /* P34: dEvt_control_c::order(eventType, prio,
                          * flag, hind, ac1, ac2, eventIdx, infoIdx) —
                          * map:2425, cross-chunk from orderOtherEventId
                          * 80025F20. eventIdx arrives in r9 (arg7) and
                          * infoIdx in r10 (arg8) per the C6 ABI note;
                          * count queue pressure (mOrderCount cap 8). */
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P34 evt order type=%u prio=%u flag=0x%X "
                    "hind=0x%X eventIdx=%d infoIdx=%u lr=0x%08X\n",
                    ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                    (s16)(ctx->gpr[9] & 0xFFFFu), ctx->gpr[10] & 0xFFu,
                    ctx->lr);
        break;
    }
    case 0x800537C8u: { /* P36: dComIfGp_setNextStage(const char*, s8, s8,
                          * f32, u32, s32, s8) — map:1746. Called
                          * CROSS-CHUNK from dEvDt_Next_Stage at 0x80071900
                          * (chunk_0028:5169-5172) — the demo's Next-Stage
                          * action IS observable. r3 = stage name char*.
                          * The 'title' event closing should order the
                          * sequel demo via this; zero P36 rows after a
                          * 'title' 2->0 close = the title event's staff
                          * never ran its Next-Stage action. */
        if (s_loadseam_trace) {
            const u32 np = ctx->gpr[3];
            char name[33];
            int k;
            for (k = 0; k < 32 && np >= 0x80000000u && np + (u32)k < 0x82000000u; k++) {
                const char c = (char)rel_rd8(np + (u32)k);
                name[k] = (c == 0 || c < 0x20) ? 0 : c;
                if (name[k] == 0)
                    break;
            }
            name[k < 32 ? k : 32] = 0;
            fprintf(stderr,
                    "[loadseam] P36 setNextStage '%s' room=%d lr=0x%08X\n",
                    name, (s8)(ctx->gpr[4] & 0xFFu), ctx->lr);
        }
        break;
    }
    case 0x800737F4u: { /* P37: dEvent_exception_c::setStartDemo(int) —
                          * d_event_manager.cpp:23-24 (map 800737F4, member:
                          * r3=this exception obj, r4=eventInfoIdx s32).
                          * REL->DOL boundary of demo selection: called from
                          * Link create via dComIfGp_evmng_startDemo(idx)
                          * (d_com_inf_game.h:3686, d_a_player_main.cpp:
                          * 12302-12322 — getStartEvent() default branch).
                          * idx semantic (d_event_manager.cpp:26-50):
                          * 0xFF->mEventInfoIdx=206; >=200 passthrough;
                          * -1/out-of-range->0xFF no-demo; else stage
                          * EventInfo idx (spawn-switch may force 206). */
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P37 setStartDemo idx=%d this=0x%08X lr=0x%08X\n",
                    (s32)ctx->gpr[4], ctx->gpr[3], ctx->lr);
        break;
    }
    case 0x800754BCu: { /* P38: dEvent_manager_c::checkStartDemo(void) —
                          * d_event_manager.cpp:807-816 (map 800754BC, member:
                          * r3=this manager). Returns TRUE only when
                          * dComIfGp_event_runCheck() AND
                          * mException.mEventInfoIdx != -1 (offset 0x024+4
                          * = 0x028 in dEvent_manager_c, d_event_manager.h:
                          * 73-75). Entry-only: the FALSE path = demo not
                          * selected / not running. */
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P38 checkStartDemo entry this=0x%08X lr=0x%08X\n",
                    ctx->gpr[3], ctx->lr);
        break;
    }
    case 0x8006B12Cu: { /* P39: dDemo_manager_c::update — the .dem
                          * movie timeline driver (map:2301). Called
                          * CROSS-CHUNK from d_s_play execute at
                          * 0x80235048 (chunk_0140:34447) every frame
                          * the demo is active. The storm cutscene is a
                          * .dem movie (Demo51.arc), NOT event-staff
                          * creates — sea_T's data can only order the
                          * 'title' event (Stage.dzs EVNT={title} alone;
                          * Room44 Link params 0x00FF002C byte3=0).
                          * P39 firing per-frame = the .dem system
                          * runs; 0 rows during the Demo51 window = the
                          * demo movie never starts. */
        if (s_loadseam_trace) {
            static unsigned p39_n;
            if (p39_n < 40 || (p39_n % 500u) == 0)
                fprintf(stderr,
                        "[loadseam] P39 ddemo update this=0x%08X lr=0x%08X (call=%u)\n",
                        ctx->gpr[3], ctx->lr, p39_n);
            p39_n++;
        }
        break;
    }
    case 0x8006AFBCu: { /* P40: dDemo_manager_c::create(const u8*,
                          * cXyz*, f32) — the .dem data mount (map:2299).
                          * One row per demo movie start; r3 = the .dem
                          * data pointer. Demo51 loads (DVD rows) without
                          * P40 = the arc loads but the movie never
                          * mounts. */
        if (s_loadseam_trace)
            fprintf(stderr,
                    "[loadseam] P40 ddemo create data=0x%08X pos=0x%08X lr=0x%08X\n",
                    ctx->gpr[3], ctx->gpr[4], ctx->lr);
        break;
    }
    /* [video-chain] MODERNGEKKO_LOADSEAM_TRACE — probe every function in the
     * fresh-boot video-init path + the per-frame present path. The game
     * reaches the title and issues EFB copies but never writes a VI register,
     * so the question is which of these never runs. */
    case 0x80006338u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] main01 entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x80006464u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] main entry lr=0x%08X\n", ctx->lr);
        break;
    case 0x8000BC94u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] mDoGph_Create lr=0x%08X\n", ctx->lr);
        break;
    case 0x80007BBCu:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] mDoGph_gInf_c::create lr=0x%08X\n", ctx->lr);
        break;
    case 0x80255354u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] JFWDisplay::createManager lr=0x%08X\n", ctx->lr);
        break;
    case 0x802C7C34u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] JUTVideo::createManager lr=0x%08X\n", ctx->lr);
        break;
    case 0x802C7CD4u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] JUTVideo ctor lr=0x%08X\n", ctx->lr);
        break;
    case 0x80313554u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] VIInit lr=0x%08X\n", ctx->lr);
        break;
    case 0x80313ECCu:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] VIConfigure lr=0x%08X\n", ctx->lr);
        break;
    case 0x80314824u: {
        /* VISetNextFrameBuffer(fb) — gpr[3] is the XFB the game wants scanned.
         * Also dump the VI flush "changed" mask at r13-25968/-25964 (the bits
         * that gate which shadow regs get pushed to __VIRegs/MMIO). */
        if (s_loadseam_trace) {
            static int s_nfb = 0;
            if (++s_nfb <= 60) {
                const u32 sda = ctx->gpr[13];
                fprintf(stderr,
                        "[vchain] VISetNextFrameBuffer fb=0x%08X r13=0x%08X "
                        "chg_lo=0x%08X chg_hi=0x%08X lr=0x%08X\n",
                        ctx->gpr[3], sda, rel_rd32(sda - 25968u),
                        rel_rd32(sda - 25964u), ctx->lr);
            }
        }
        break;
    }
    case 0x8031322Cu:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] VISetPreRetraceCallback cb=0x%08X\n", ctx->gpr[3]);
        break;
    case 0x80313270u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] VISetPostRetraceCallback cb=0x%08X\n", ctx->gpr[3]);
        break;
    case 0x802C7E30u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] JUTVideo::preRetraceProc lr=0x%08X\n", ctx->lr);
        break;
    case 0x802C8088u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] JUTVideo::drawDoneCallback lr=0x%08X\n", ctx->lr);
        break;
    case 0x80255570u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] exchangeXfb_double lr=0x%08X\n", ctx->lr);
        break;
    case 0x80008410u:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] mDoGph_AfterOfDraw lr=0x%08X\n", ctx->lr);
        break;
    case 0x8000AF2Cu:
        if (s_loadseam_trace) fprintf(stderr, "[vchain] mDoGph_Painter lr=0x%08X\n", ctx->lr);
        break;
    case 0x80244F44u: /* cNdIt_Judge — in-module replacement for the hot
                        * node-list judge walk (~18K claims/s through the
                        * mod-side host-call hook today; the dispatcher claim
                        * stays in-module). Reproduces the walk only for the
                        * known leaf judges (ForPName/ByID, directly or under
                        * cTgIt_JudgeFilter); anything else declines and the
                        * guest body runs — including its inline host_call,
                        * so the mod hook still covers declines. Charge -5 =
                        * the generated entry decrement (label_80244F44).
                        * MODERNGEKKO_REPL_JUDGE=0 off. */
        if (s_repl_judge && judgerepl_claim(ctx, ctx->gpr[3], 5)) return 1;
        break;
    case 0x80244C28u: /* cLsIt_Judge — node_list_class mSize gate in front of
                        * the same walk (the guest tailcall is an intra-chunk
                        * goto, so this claim covers both halves). Charge -6 =
                        * the generated entry decrement (label_80244C28).
                        * Same env gate. */
        if (s_repl_judge && judgerepl_ls(ctx)) return 1;
        break;
    case 0x80330C84u: /* sin(f1) — in-module replacement: ~54-instr guest
                        * polynomial vs one host libm call. The mod-hook
                        * version of this measured net-negative (host_call
                        * round-trip + chassis re-dispatch); the dispatcher
                        * claim stays in-module, costing only this switch hit
                        * + the libm call. ulp-level diffs vs the guest
                        * polynomial are rendering-tolerant; NaN/Inf
                        * propagate identically. MODERNGEKKO_REPL_LIBM=0 off. */
        if (s_repl_libm) { ctx->fpr[1] = sin(ctx->fpr[1]); ctx->pc = ctx->lr; return 1; }
        break;
    case 0x8033071Cu: /* cos(f1) — same replacement; see sin case. */
        if (s_repl_libm) { ctx->fpr[1] = cos(ctx->fpr[1]); ctx->pc = ctx->lr; return 1; }
        break;
    case 0x8030D0FCu: /* PSMTXConcat — bit-exact paired-single 3x4 multiply;
                        * falls back on non-finite operands/quantised GQR0. */
        if (s_repl_mtx && mtxrepl_psmtxconcat(ctx)) return 1;
        break;
    case 0x80307EF4u: /* SelectThread idle-poll collapse — one poll per
                        * dispatch instead of spinning to the -256 budget.
                        * MODERNGEKKO_REPL_IDLE=0 off. */
        if (s_repl_idle && idlerepl_selectthread(ctx)) return 1;
        break;
    /* --- leaf replacements (MODERNGEKKO_REPL_LEAF) — verified
     *     instruction-for-instruction against the dol-inline-opt
     *     generated bodies; see each leafrepl_* comment for charges,
     *     live-outs and decline gates. --- */
    case 0x800A9684u: /* dBgW::ChkGrpThrough */
        if (s_repl_leaf && leafrepl_chkgrpthrough(ctx)) return 1;
        break;
    case 0x8022F7F8u: /* JUTResFont::getAscent */
        if (s_repl_leaf && leafrepl_fontmetric(ctx, 10u)) return 1;
        break;
    case 0x8022F804u: /* JUTResFont::getDescent */
        if (s_repl_leaf && leafrepl_fontmetric(ctx, 12u)) return 1;
        break;
    case 0x80245674u: /* cXyz::__pl__ (PSVECAdd wrapper) */
        if (s_repl_leaf && leafrepl_cxyz(ctx, 0)) return 1;
        break;
    case 0x802456C4u: /* cXyz::__mi__ (PSVECSubtract wrapper) */
        if (s_repl_leaf && leafrepl_cxyz(ctx, 1)) return 1;
        break;
    case 0x80245714u: /* cXyz::__ml__Ff (PSVECScale wrapper) */
        if (s_repl_leaf && leafrepl_cxyz(ctx, 2)) return 1;
        break;
    case 0x802457A8u: /* cXyz::__dv__ (fdivs + PSVECScale wrapper) */
        if (s_repl_leaf && leafrepl_cxyz(ctx, 3)) return 1;
        break;
    case 0x8024734Cu: /* cBgS_Chk::ChkSameActorPid */
        if (s_repl_leaf && leafrepl_chksameactorpid(ctx)) return 1;
        break;
    case 0x8024A8E0u: /* cM3d_Cross_AabCyl */
        if (s_repl_leaf && leafrepl_cross_aabcyl(ctx)) return 1;
        break;
    case 0x803030ACu: /* DCInvalidateRange — dcbi loop collapse */
        if (s_repl_leaf && leafrepl_dcinvalidate(ctx)) return 1;
        break;
    case 0x803030D8u: /* DCFlushRange — dcbf loop + trailing sc */
        if (s_repl_leaf && leafrepl_dcflush(ctx)) return 1;
        break;
    case 0x802D8BD8u: /* J3DFifoLoadPosMtxImm — XF pos-mtx packet push */
        if (s_repl_leaf && leafrepl_j3dfifo_posmtx(ctx)) return 1;
        break;
    case 0x802D8C58u: /* J3DFifoLoadNrmMtxImm — XF nrm-mtx packet push */
        if (s_repl_leaf && leafrepl_j3dfifo_nrmmtx(ctx)) return 1;
        break;
    case 0x802D8CC4u: /* J3DFifoLoadNrmMtxImm3x3 — packed 3x3 variant */
        if (s_repl_leaf && leafrepl_j3dfifo_nrm3x3(ctx)) return 1;
        break;
    case 0x8030DA44u: /* PSMTXMultVec */
        if (s_repl_leaf && leafrepl_psmtxmultvec(ctx)) return 1;
        break;
    case 0x8030DA98u: /* PSMTXMultVecArray — bdnz walk; may yield with
                        * pc = 0x8030DAE4 (a generated dispatch pc) */
        if (s_repl_leaf && leafrepl_psmtxmultvecarray(ctx)) return 1;
        break;
    case 0x8030DB24u: /* PSMTXMultVecSR */
        if (s_repl_leaf && leafrepl_psmtxmultvecsr(ctx)) return 1;
        break;
    case 0x8030DCE0u: /* PSVECAdd */
        if (s_repl_leaf && leafrepl_psvecaddsub(ctx, 0)) return 1;
        break;
    case 0x8030DD04u: /* PSVECSubtract */
        if (s_repl_leaf && leafrepl_psvecaddsub(ctx, 1)) return 1;
        break;
    case 0x8030DD28u: /* PSVECScale */
        if (s_repl_leaf && leafrepl_psvecscale(ctx)) return 1;
        break;
    case 0x8030E0B4u: /* PSVECSquareDistance */
        if (s_repl_leaf && leafrepl_psvecsquaredistance(ctx)) return 1;
        break;
    /* --- J3DMtxCalc::calcTransform fused bodies (MODERNGEKKO_REPL_J3D) —
     *     verbatim transcriptions of the generated bodies with the
     *     J3DGetTranslateRotateMtx / PSMTXConcat / PSMTXCopy callees and
     *     the gpr save/restore stubs inlined. No guest-state declines:
     *     every op runs the same helper/accessor the generated body
     *     calls, so the result is bit-exact for all inputs including
     *     fp-unavailable and psq-exception bail paths (claimed with the
     *     generated partial state). Only the function-entry pcs below
     *     are claimed — the interior continuation labels remain
     *     dispatchable for the unmodified generated path. --- */
    case 0x802F5090u: /* J3DMtxCalcBasic::calcTransform */
        if (s_repl_j3d && j3drepl_basic(ctx)) return 1;
        break;
    case 0x802F52BCu: /* J3DMtxCalcSoftimage::calcTransform */
        if (s_repl_j3d && j3drepl_softimage(ctx)) return 1;
        break;
    case 0x802F5508u: /* J3DMtxCalcMaya::calcTransform */
        if (s_repl_j3d && j3drepl_maya(ctx)) return 1;
        break;
    default:
        break;
    }

    // REL chunk dispatch: linked addresses 0x80500000+ resolve to the
    // compiled chunk functions (the DOL dispatch lookup covers DOL only).
    if (address >= 0x80500000u) {
        /* Chunk memo: consecutive REL dispatches overwhelmingly stay inside
         * the same chunk — a hit skips the 542-entry binsearch. Pure cache
         * of the search result (same containment predicate; the table is
         * static const), so a memo hit is bit-identical to a binsearch hit. */
        int mid = -1;
        if (s_last_chunk && address >= s_last_chunk->start &&
            address < s_last_chunk->end) {
            mid = (int)(s_last_chunk - s_rel_loader_chunks);
        } else {
            int lo = 0;
            int hi = (int)REL_LOADER_CHUNK_COUNT - 1;
            while (lo <= hi) {
                int m2 = (lo + hi) >> 1;
                if (address < s_rel_loader_chunks[m2].start) {
                    hi = m2 - 1;
                } else if (address >= s_rel_loader_chunks[m2].end) {
                    lo = m2 + 1;
                } else {
                    mid = m2;
                    break;
                }
            }
            s_last_chunk = mid >= 0 ? &s_rel_loader_chunks[mid] : NULL;
        }
        if (mid >= 0) {
                /* TEMP diag: dump mode_tbl at modeProc entry */
                if (address == 0x8067B618u || address == 0x8067B904u) {
                    static unsigned nd;
                    u32 ci;
                    for (ci = 0; ci < s_active_count; ci++) {
                        const RelActiveModule* m = &s_active[ci];
                        if (m->id != 60u)
                            continue;
                        if (nd < 6) {
                            u32 bo = 0x5DC8u;
                            u32 i;
                            fprintf(stderr, "[mtbl] modeProc entry 0x%08X L=0x%08X R=0x%08X\n",
                                    address, m->l + bo, m->r + bo);
                            for (i = 0; i < 3; i++) {
                                fprintf(stderr,
                                        "[mtbl]  [%u] L: %08X %08X %08X | R: %08X %08X %08X\n",
                                        i,
                                        rel_rd32(m->l + bo + i*0x1Cu),
                                        rel_rd32(m->l + bo + i*0x1Cu + 4u),
                                        rel_rd32(m->l + bo + i*0x1Cu + 8u),
                                        rel_rd32(m->r + bo + i*0x1Cu),
                                        rel_rd32(m->r + bo + i*0x1Cu + 4u),
                                        rel_rd32(m->r + bo + i*0x1Cu + 8u));
                            }
                        }
                        nd++;
                        break;
                    }
                }
                // The game's JKR arena reuses the E1 batch linked bases after
                // link, clobbering module DATA the compiled chunks read via
                // baked L constants. L is verified lazily: only when the heap
                // generation moved since the module's last verification (any
                // allocation can land on an L base), and a refresh is only
                // issued when the cached probe region actually differs from R.
                {
                    RelActiveModule* m = NULL;
                    u32 ci;
                    /* Active-table memos refresh once per link/unlink/resync
                     * (s_active_seq): the e4 gate address and the L-owner
                     * memo both live here. */
                    if (s_memo_seq != s_active_seq) {
                        u32 i;
                        s_memo_seq = s_active_seq;
                        s_last_owner = 0xFFFFFFFFu;
                        s_e4_addr = 0u;
                        s_e4_dup = 0;
                        s_l_overlap = 0;
                        for (i = 0; i < s_active_count; i++) {
                            const RelActiveModule* mm = &s_active[i];
                            u32 j;
                            if (mm->id == 1u && mm->l) {
                                if (s_e4_addr)
                                    s_e4_dup = 1; /* second id-1 entry: keep the scan */
                                else {
                                    s_e4_addr = mm->l + 0xECu;
                                    s_e4_val = mm->r + 0x178u;
                                }
                            }
                            if (mm->l == 0u || s_l_overlap)
                                continue;
                            for (j = i + 1; j < s_active_count; j++) {
                                const RelActiveModule* mn = &s_active[j];
                                if (mn->l && mm->l < mn->l + mn->file_size &&
                                    mn->l < mm->l + mm->file_size)
                                    s_l_overlap = 1;
                            }
                        }
                    }
                    /* Owner memo: consecutive dispatches almost always stay
                     * in the same module — try the last hit first, then the
                     * full scan (which repopulates the memo). Disabled if any
                     * two L ranges overlap: the scan returns the FIRST
                     * containing module, the memo could pick a later one. */
                    if (!s_l_overlap && s_last_owner < s_active_count) {
                        RelActiveModule* t = &s_active[s_last_owner];
                        if (t->l && address >= t->l &&
                            address < t->l + t->file_size)
                            m = t;
                    }
                    if (!m) {
                        for (ci = 0; ci < s_active_count; ci++) {
                            RelActiveModule* t = &s_active[ci];
                            if (t->l == 0u || address < t->l ||
                                address >= t->l + t->file_size)
                                continue;
                            m = t;
                            if (!s_l_overlap)
                                s_last_owner = ci;
                            break;
                        }
                    }
                    if (m) {
                        /* l_dirty (journal-flagged direct L writes) is honored
                         * on every dispatch; the section-table mismatch probe
                         * only runs when the heap generation moved since this
                         * module's last verification — an allocation can land
                         * on an L base, none can happen in between. */
                        if (m->l_dirty || (m->l_gen != s_heap_gen &&
                                           rel_loader_l_data_mismatch(m))) {
                            rel_loader_refresh_l_image(m);
                            m->l_dirty = 0u;
                            if (s_debug)
                                fprintf(stderr,
                                        "[rel_loader] L refresh id=%u before 0x%08X "
                                        "(heap-clobbered L guard)\n",
                                        m->id, address);
                        }
                        m->l_gen = s_heap_gen;
                    }
                }
                const int saved_in_rel = s_in_rel_chunk;
                s_in_rel_chunk = 1;
                s_rel_loader_chunks[mid].fn(ctx);
                s_in_rel_chunk = saved_in_rel;
                /* Publish AFTER the call: chunk code may re-enter
                 * dolrecomp_dispatch_replacement (nested REL calls), and the
                 * entry-clear at :2494 plus inner publishes would otherwise
                 * leave the INNERMOST dispatch in the export — caching the
                 * wrong fn for this pc. Assigning on unwind makes each frame
                 * restore its own resolution. The E4 gate address is never
                 * memoizable (its post-call global fixup must re-run). */
                g_rel_dispatched_fn =
                    (address == s_e4_addr) ? (void(*)(CPUState*))0
                                           : s_rel_loader_chunks[mid].fn;
                g_rel_dispatched_pc = address;

                // E4 (load gate) — f_pc_profile_lst (module 1) prolog fix.
                //
                // The compiled prolog chunk stores the DESCRIPTOR's linked
                // profile-list address (L+0x178 = 0x80701F18) into the DOL
                // global g_fpcPf_ProfileList_p (0x803F6A68). The game reads
                // the profile array through that global, but module 1's E1
                // linked base (0x80701DA0) sits inside the game's live JKR
                // heap arena: the audio solid heap (created right after
                // cDyl_InitAsync) lands at ~0x80700000 and clobbers the
                // mirrored L copy before the first process is created
                // (fpcBs_Create -> fpcPf_Get then reads heap garbage such as
                // 0xFF00300C and faults at the profile struct deref). The
                // game's own JKR allocation (the R copy) is heap-stable and
                // carries the correctly-relocated array at R+0x178 (section
                // 4 = profile pointer table), so repoint the global at the
                // R copy. The hook sits at the prolog's post-ctor dispatch
                // re-entry, module 1 chunk offset 0xEC (retail-linked
                // 0x80701E8C; EXRAM LRELOC lineage 0x91301E8C): the store
                // helper (0x80701EEC / 0x91301EEC, "g_fpcPf_ProfileList_p
                // = r3") runs inside that chunk pass, so overriding
                // afterwards sticks.
                // phase3-lreloc-p6 wave (2026-08-28): the match was the
                // hard-coded retail re-entry 0x80701E8C and NEVER fired on
                // the EXRAM lineage (dispatch miss row #2 addr=0x91301E8C,
                // no e4 row in boot-seam2-p5d.log), leaving
                // g_fpcPf_ProfileList_p NULL -> cDyl_LinkASync NULL+0x10/
                // +0x14 fault at PC=0x8003e2dc on the first module-1
                // create. Match the LIVE descriptor base (l+0xEC) so the
                // gate is lineage-independent; on MEM1 builds this is the
                // same address as before, byte-for-byte behavior.
                {
                    /* E4 via the cached gate address (s_e4_addr/s_e4_val are
                     * refreshed with the active-table memos above). The
                     * original scan survives only for the never-observed
                     * duplicate id-1 case, where first-match ordering matters. */
                    if (!s_e4_dup) {
                        if (address == s_e4_addr) {
                            rel_wr32(0x803F6A68u, s_e4_val);
                            if (s_debug)
                                fprintf(stderr,
                                        "[rel_loader] e4: g_fpcPf_ProfileList_p -> R+0x178 "
                                        "(0x%08X)\n",
                                        s_e4_val);
                        }
                    } else {
                        u32 mi;
                        for (mi = 0; mi < s_active_count; mi++) {
                            if (s_active[mi].id == 1u &&
                                address == s_active[mi].l + 0xECu) {
                                rel_wr32(0x803F6A68u, s_active[mi].r + 0x178u);
                                if (s_debug)
                                    fprintf(stderr,
                                            "[rel_loader] e4: g_fpcPf_ProfileList_p -> R+0x178 "
                                            "(0x%08X)\n",
                                            s_active[mi].r + 0x178u);
                                break;
                            }
                        }
                    }
                }
                return 1;
        }
        /* phase3-lreloc-p5b: [seam] DISPATCH MISS — a REL-domain entry the
         * compiled chunk tree does not cover. dolrecomp_call then falls
         * through host-call and DOL-original lookups, so unless a physical
         * alias matches, this block resumes under the GXRuntime interpreter
         * against raw guest bytes (L-image may not even hold PPC code there).
         * Pure logging; search below is unchanged. NOTE: this must sit AFTER
         * the chunk-table search — logging at the top mislabeled every
         * covered REL dispatch as a miss. */
        if (s_seam_diag) {
            s_seam_dispatch_misses++;
            if (s_seam_dispatch_misses == 1) {
                u32 di;
                fprintf(stderr, "[seam] active table count=%u\n", s_active_count);
                for (di = 0; di < s_active_count; di++)
                    fprintf(stderr, "[seam]   id=%u R=0x%08X..0x%08X L=0x%08X fsz=0x%X\n",
                            s_active[di].id, s_active[di].r,
                            s_active[di].r + s_active[di].file_size,
                            s_active[di].l, s_active[di].file_size);
            }
            if (s_seam_dispatch_misses <= 64 ||
                (s_seam_dispatch_misses & 255u) == 0) {
                u32 ci;
                int id = -1;
                for (ci = 0; ci < s_active_count; ci++) {
                    if (address >= s_active[ci].l &&
                        address < s_active[ci].l + s_active[ci].file_size) {
                        id = (int)s_active[ci].id;
                        break;
                    }
                }
                fprintf(stderr,
                        "[seam] DISPATCH MISS #%u addr=0x%08X lr=0x%08X mod=%d\n",
                        s_seam_dispatch_misses, address, ctx->lr, id);
            }
        }
        /* phase3-lreloc-p18 — retail-band dispatch-miss twin translation
         * (root #7 second carrier). The attract demo calls REL functions
         * through retail-band pointers (0x80500000..0x80C873D0): factory
         * method tables / event-list entries hold the RETAIL twin of the
         * module's code (p8 module-9 self 0x8061B7E0, p10 module-96 self
         * 0x806D2550, boots 6-8 module-9 0x80618C80). The chunk table above
         * only covers LRELOC-linked addresses, so a retail-band entry falls
         * through to the GXRuntime interpreter, which interprets raw guest
         * bytes (zeros/garbage) at the retail EXRAM window -> IntCPU halt.
         * Fix: on a chunk-table miss, retry the lookup with the retail->
         * LRELOC twin (+0x10C00000, the fixed retail->L delta). If the twin
         * hits a compiled chunk AND the twin lies inside an active module's
         * L window, run that chunk with ctx->pc patched to the twin so any
         * pc-relative reads inside the chunk see the right lineage.
         * In-band addresses whose twin misses everything are left alone
         * (fall through to legacy behavior — logging only). */
        {
            /* Retail-alias dispatch: vtables / process records hold pointers
             * into a module's R image, whose base is wherever the game heap
             * placed it — it varies per scene, so no fixed delta can map it.
             * Find the active module whose R window owns the address and
             * twin to l + (address - r); dispatch only when that twin lands
             * in a compiled chunk. R windows can overlap, so keep scanning
             * owners until one twins into compiled code. */
            u32 ci;
            if (address < 0x80500000u)
                goto retail_done; /* DOL addresses can never twin — skip scan */
            for (ci = 0; ci < s_active_count; ci++) {
                const RelActiveModule* am = &s_active[ci];
                u32 twin;
                int tlo, thi, tmid;
                void (*twin_fn)(CPUState*);
                int e4_twin = 0;
                if (!am->l || address < am->r ||
                    address >= am->r + am->file_size)
                    continue;
                twin = am->l + (address - am->r);
                tlo = 0;
                thi = (int)REL_LOADER_CHUNK_COUNT - 1;
                tmid = -1;
                while (tlo <= thi) {
                    tmid = (tlo + thi) >> 1;
                    if (twin < s_rel_loader_chunks[tmid].start) {
                        thi = tmid - 1;
                    } else if (twin >= s_rel_loader_chunks[tmid].end) {
                        tlo = tmid + 1;
                    } else {
                        break;
                    }
                }
                if (tlo > thi)
                    continue; /* R-window hit but twin not compiled — next */
                if (s_seam_diag || s_debug) {
                    fprintf(stderr,
                            "[rel_loader] retail-alias 0x%08X -> twin 0x%08X "
                            "(module %u)\n",
                            address, twin, am->id);
                }
                ctx->pc = twin;
                twin_fn = s_rel_loader_chunks[tmid].fn;
                {
                    const int saved_in_rel = s_in_rel_chunk;
                    s_in_rel_chunk = 1;
                    twin_fn(ctx);
                    s_in_rel_chunk = saved_in_rel;
                }
                /* E4 gate via the retail alias must still run the
                 * profile-list fixup — and is never memoized. */
                if (!s_e4_dup) {
                    if (twin == s_e4_addr) {
                        rel_wr32(0x803F6A68u, s_e4_val);
                        e4_twin = 1;
                    }
                } else {
                    u32 mi;
                    for (mi = 0; mi < s_active_count; mi++) {
                        if (s_active[mi].id == 1u &&
                            twin == s_active[mi].l + 0xECu) {
                            rel_wr32(0x803F6A68u, s_active[mi].r + 0x178u);
                            e4_twin = 1;
                            break;
                        }
                    }
                }
                if (!e4_twin) {
                    g_rel_dispatched_fn = twin_fn;
                    g_rel_dispatched_pc = twin;
                }
                return 1;
            }
        }
        retail_done: ;
    }
    return 0;
}

/* DOL-text write watch export (phase2-dol-watch-spec.md §1.3): print the
 * filtered ring entries (stores into SMC-guarded DOL chunks only) that fall
 * in [chunk_start, chunk_end), newest-first; if fewer than 8 hit, expand
 * outward to the nearest entries either side (a pointer-computed target can
 * land outside the chunk). At most 64 entries, then a tail summary. All
 * output to stderr with the [smc-journal] prefix. Called by the runner's
 * SMC-mismatch dump (StaticRecompCore_SMC.cpp). With the filter the
 * corruption entry is normally the ONLY one — the whole-session history. */
__attribute__((visibility("default"))) void ppc_smc_dump_write_journal(u32 chunk_start,
                                                                        u32 chunk_end) {
    u32 count;
    u32 newest;
    u32 k;
    u32 in_window = 0u;
    u32 printed = 0u;
    u32 nearest_below = 0u; /* entries just below the window */
    u32 nearest_above = 0u; /* entries just above the window */
    u32 below_seen = 0u;
    u32 above_seen = 0u;

    if (s_smc_ring_size == 0u || s_smc_ring_seq == 0u) {
        fprintf(stderr, "[smc-journal] dol ring empty (no DOL-text writes journaled)\n");
        return;
    }
    count = s_smc_ring_seq < (u64)s_smc_ring_size ? (u32)s_smc_ring_seq : s_smc_ring_size;
    newest = s_smc_ring_head == 0u ? s_smc_ring_size - 1u : s_smc_ring_head - 1u;
    fprintf(stderr,
            "[smc-journal] dol ring: %u entries, seq %llu..%llu, window [0x%08X,0x%08X)\n",
            count, (unsigned long long)(s_smc_ring_seq - (u64)count),
            (unsigned long long)(s_smc_ring_seq - 1u), chunk_start, chunk_end);
    for (k = 0; k < count && printed < 64u; k++) {
        u32 slot = (newest + s_smc_ring_size - k) % s_smc_ring_size;
        const SmcWriteRingEntry* e = &s_smc_ring[slot];
        u32 guest = 0x80000000u + e->offset;
        if (guest >= chunk_start && guest < chunk_end) {
            in_window++;
            printed++;
            fprintf(stderr, "[smc-journal]   @0x%08X (+0x%04X) size=%u seq=%llu tb=%llu val=%08X\n",
                    guest, guest - chunk_start, e->size,
                    (unsigned long long)e->seq, (unsigned long long)e->tb, e->value);
        } else if (guest < chunk_start && below_seen < 8u) {
            below_seen++;
            nearest_below = guest;
        } else if (guest >= chunk_end && above_seen < 8u) {
            above_seen++;
            nearest_above = guest;
        }
    }
    if (in_window == 0u)
        fprintf(stderr,
                "[smc-journal]   (no writes in window; %u nearest below 0x%08X, %u nearest "
                "above 0x%08X) — writer PC unknown, correlate with Invalid-write family at "
                "this guest time (phase2-smc-anomaly-dig.md §6c)\n",
                below_seen, nearest_below, above_seen, nearest_above);
    fprintf(stderr, "[smc-journal] end (printed %u of %u in window)\n", printed, in_window);
}

/* p12 mt-watch dump export: print the method-table write ring newest-first
 * ([mt-watch] prefix), the ppc_smc_dump_write_journal pattern. Called
 * from a halt/SMC context to correlate the 0x80AB1EE0 method-table writer
 * with the boot timeline. Journal pushes carry no pc — correlation is by
 * seq/gen relative to the [rel_loader] link rows. */
__attribute__((visibility("default"))) void ppc_mt_dump(void) {
    u32 count;
    u32 newest;
    u32 k;

    if (!s_mt_watch) {
        fprintf(stderr, "[mt-watch] not armed (set MODERNGEKKO_MT_WATCH=1)\n");
        return;
    }
    if (s_mt_ring_seq == 0u) {
        fprintf(stderr, "[mt-watch] ring empty (no writes hit "
                        "[0x%08X..0x%08X))\n", MT_WATCH_LO, MT_WATCH_HI);
        return;
    }
    count = s_mt_ring_seq < (u64)MT_RING_MAX ? (u32)s_mt_ring_seq : MT_RING_MAX;
    newest = s_mt_ring_head == 0u ? MT_RING_MAX - 1u : s_mt_ring_head - 1u;
    fprintf(stderr, "[mt-watch] ring: %u/%u entries, seq 0..%llu, watch "
                    "[0x%08X..0x%08X)\n", count, MT_RING_MAX,
            (unsigned long long)(s_mt_ring_seq - 1u), MT_WATCH_LO, MT_WATCH_HI);
    for (k = 0; k < count; k++) {
        const MtWatchRingEntry* e = &s_mt_ring[(newest + MT_RING_MAX - k) % MT_RING_MAX];
        fprintf(stderr,
                "[mt-watch]   @0x%08X sz=%u seq=%llu gen=%u tb=%llu old=%08X "
                "new=%08X%s\n",
                0x80000000u + e->offset, e->size, (unsigned long long)e->seq,
                e->gen, (unsigned long long)e->tb, e->oldv, e->newv,
                ((e->newv & 3u) == 0u && e->newv >= MT_RETAIL_LO &&
                 e->newv < MT_RETAIL_HI)
                    ? " <== RETAIL-POINTER"
                    : "");
    }
    fprintf(stderr, "[mt-watch] end (%u entries)\n", count);
}

/* Rebuild the O2 journal page-filter so it remains a SUPERSET of every
 * offset the journal can act on: SMC-guarded DOL chunk pages (ring push),
 * the watched relocation globals (0x803F7650/0x803F75FC), AND every active
 * module's R range and L mirror range — the journal's per-store module scan
 * (L-mirror sync on data-section writes, code-write TRAP, l_dirty marking)
 * must see every store that lands in those ranges, exactly as before.
 * Called at init and whenever the active-module set changes; the journal and
 * link/unlink run on the same dispatch thread, so publication is ordered. */
static void rel_loader_rebuild_journal_mask(void) {
    static uint64_t mask[24];
    /* The mask is 24 words = 1536 pages = exactly MEM1 (0x1800000). Page
     * indexes are derived from end pointers, so every clamp below uses the
     * mask's own capacity, not raw s_ram_size — an oversized RAM report
     * would otherwise index mask[] out of bounds. */
    const uint32_t mask_lim = 0x80000000u +
        (s_ram_size < SMC_MEM1_SIZE ? s_ram_size : SMC_MEM1_SIZE);
    uint32_t i;
    memset(mask, 0, sizeof(mask));
    for (i = 0; i < s_smc_dol_count; i++) {
        uint32_t start = s_smc_dol_ranges[i * 2u];
        uint32_t end = s_smc_dol_ranges[i * 2u + 1u];
        uint32_t page;
        if (start < 0x80000000u)
            start = 0x80000000u;
        if (end > mask_lim)
            end = mask_lim;
        if (start >= end)
            continue;
        for (page = (start - 0x80000000u) >> 14; page <= ((end - 1u - 0x80000000u) >> 14);
             page++)
            mask[page >> 6] |= (uint64_t)1 << (page & 63u);
    }
    /* Watched relocation globals: mark their whole page (stores of exactly
     * these offsets trigger; page granularity is a safe superset). */
    mask[(0x3F7650u >> 14) >> 6] |= (uint64_t)1 << ((0x3F7650u >> 14) & 63u);
    mask[(0x3F75FCu >> 14) >> 6] |= (uint64_t)1 << ((0x3F75FCu >> 14) & 63u);
    /* p12 mt-watch: mark the watched method-table region's pages too,
     * else the page-filter hides every write the ring needs to see. */
    if (s_mt_watch) {
        uint32_t page;
        for (page = ((MT_WATCH_LO - 0x80000000u) >> 14);
             page <= ((MT_WATCH_HI - 1u - 0x80000000u) >> 14); page++)
            mask[page >> 6] |= (uint64_t)1 << (page & 63u);
    }
    /* Section-aware coverage (perf): the journal body can only ACT on
     * file-backed NON-EXECUTABLE sections — a store into an exec section of R
     * only logs TRAP (see the body's R-leg classification) and an exec section
     * of L is an explicit continue, while the R->L mirror and l_dirty marking
     * only ever touch data sections. Marking each module's whole file_size
     * therefore marked ~56-64% of MEM1 for no semantic gain; marking only the
     * data sections covers every offset the body can act on. The exec-write
     * TRAP log is a diagnostic that never fires in a clean boot (parity run:
     * exactly 10 [rel_loader] journal lines). The classification below reuses
     * the body's own arithmetic (m->r + m->sio + si*8, rel_rd32(entry) & 1u,
     * +4 size, bss skip), so the mask cannot drift from the body. */
    for (i = 0; i < s_active_count; i++) {
        const RelActiveModule* m = &s_active[i];
        uint32_t si;
        uint32_t r_end = m->r + m->file_size;
        if (r_end > mask_lim)
            r_end = mask_lim;
        if (m->r < 0x80000000u || m->r >= r_end)
            continue;
        for (si = 1; si < m->num_sections; si++) {
            uint32_t entry = m->r + m->sio + si * 8u;
            uint32_t off = rel_rd32(entry) & ~1u;
            uint32_t sec_len = rel_rd32(entry + 4u);
            uint32_t lo, hi, page;
            if (sec_len == 0u)
                continue;
            if (off < m->r || off >= r_end)
                continue; /* bss: no file image */
            if (rel_rd32(entry) & 1u)
                continue; /* executable: body logs only, L leg skips */
            lo = off;
            hi = off + sec_len;
            if (hi > r_end)
                hi = r_end;
            for (page = (lo - 0x80000000u) >> 14; page <= ((hi - 1u - 0x80000000u) >> 14); page++)
                mask[page >> 6] |= (uint64_t)1 << (page & 63u);
            if (m->l != 0u && m->l >= 0x80000000u) {
                uint32_t l_lo = m->l + (off - m->r);
                uint32_t l_hi = l_lo + (hi - lo);
                uint32_t l_lim = mask_lim;
                if (l_lo < 0x80000000u || l_lo >= l_lim)
                    continue;
                if (l_hi > l_lim)
                    l_hi = l_lim;
                for (page = (l_lo - 0x80000000u) >> 14;
                     page <= ((l_hi - 1u - 0x80000000u) >> 14); page++)
                    mask[page >> 6] |= (uint64_t)1 << (page & 63u);
            }
        }
    }
    /* A compiled store is 1/2/4/8 bytes, so one that STARTS in the previous
     * page can straddle into a marked one. Section-aware marking made this
     * observable (whole-window marking covered straddles implicitly), so add
     * predecessor pages simultaneously — not recursively, matching the
     * fleet's journal-filter mask builder. */
    for (i = 0; i < 24; ++i) {
        uint64_t following = i + 1u < 24u ? mask[i + 1u] : 0;
        mask[i] |= (mask[i] >> 1) | (following << 63);
    }
    ppc_set_mem_write_journal_filter(mask, 24);
    /* Coverage oracle (no hot-path cost): report how much of MEM1 the mask
     * admits, so the section-aware reduction is a measured number rather than
     * a model. Printed only under MODERNGEKKO_LOADER_DEBUG. */
    if (s_debug) {
        uint64_t marked = 0;
        uint32_t w;
        for (w = 0; w < 24; ++w)
            marked += (uint64_t)__builtin_popcountll(mask[w]);
        fprintf(stderr,
                "[rel_loader] journal mask: %llu/%llu pages (%llu/%llu KiB, %.1f%% of MEM1) at %u module(s)\n",
                (unsigned long long)marked, (unsigned long long)(s_ram_size >> 14),
                (unsigned long long)(marked << 4), (unsigned long long)(s_ram_size >> 10),
                100.0 * (double)marked / (double)(s_ram_size >> 14), (unsigned)s_active_count);
    }
}

__attribute__((constructor)) static void rel_loader_init(void) {
    const char* dbg = getenv("MODERNGEKKO_LOADER_DEBUG");
    s_debug = dbg && *dbg && dbg[0] != '0';
    {
        const char* rl = getenv("MODERNGEKKO_REPL_LIBM");
        s_repl_libm = !(rl && rl[0] == '0');
        const char* rm = getenv("MODERNGEKKO_REPL_MTX");
        s_repl_mtx = !(rm && rm[0] == '0');
        const char* rj = getenv("MODERNGEKKO_REPL_JUDGE");
        s_repl_judge = !(rj && rj[0] == '0');
        const char* ri = getenv("MODERNGEKKO_REPL_IDLE");
        s_repl_idle = !(ri && ri[0] == '0');
        const char* rf = getenv("MODERNGEKKO_REPL_LEAF");
        s_repl_leaf = !(rf && rf[0] == '0');
        const char* rj3 = getenv("MODERNGEKKO_REPL_J3D");
        s_repl_j3d = !(rj3 && rj3[0] == '0');
    }
    /* The dispatch hook binary-searches s_rel_loader_chunks unconditionally;
     * a malformed generated table (unsorted or empty/inverted ranges) would
     * silently mis-dispatch every REL call. Verify the generator's contract
     * once up front, mirroring the band-table check below. */
    {
        unsigned k;
        for (k = 0; k < REL_LOADER_CHUNK_COUNT; k++) {
            if (!(s_rel_loader_chunks[k].start < s_rel_loader_chunks[k].end) ||
                (k != 0 && s_rel_loader_chunks[k - 1].end > s_rel_loader_chunks[k].start)) {
                fprintf(stderr,
                        "[rel_loader] FATAL: chunk dispatch table malformed at %u "
                        "(0x%08X..0x%08X)\n",
                        k, s_rel_loader_chunks[k].start, s_rel_loader_chunks[k].end);
                exit(3);
            }
        }
    }
    const char* ring_env = getenv("STATICRECOMP_SMC_RING_SIZE");
    if (ring_env && ring_env[0]) {
        /* Whole-token parse, matching the rest of the env knobs: "12x" or
         * "-4" must not silently become 12 or a wrapped u32. */
        char* rend = NULL;
        unsigned long rv = strtoul(ring_env, &rend, 10);
        if (rend != ring_env && rend && *rend == '\0' && ring_env[0] != '-')
            s_smc_ring_size = rv > SMC_RING_MAX ? SMC_RING_MAX : (u32)rv;
    }
    if (s_smc_ring_size > SMC_RING_MAX)
        s_smc_ring_size = SMC_RING_MAX;
    s_smc_dol_count = ppc_smc_dol_chunk_count();
    /* phase3-lreloc-p5b: MODERNGEKKO_SEAM_DIAG=1 arms the [seam] probe
     * family (default OFF — bit-identical boots when unset). */
    {
        const char* sd = getenv("MODERNGEKKO_SEAM_DIAG");
        if (sd && sd[0] == '1') {
            s_seam_diag = 1;
            fprintf(stderr, "[seam] diag armed (dispatch-miss + cb/ISR/SendMsg probes)\n");
        }
    }
    /* phase3-lreloc-p11: MODERNGEKKO_P11_SWEEP=1 arms the retail self-pointer
     * sweep at link time (default OFF = bit-identical legacy boots). */
    {
        const char* p11 = getenv("MODERNGEKKO_P11_SWEEP");
        if (p11 && p11[0] == '1') {
            s_p11_sweep = 1;
            if (s_debug)
                fprintf(stderr, "[rel_loader] p11 sweep armed (MODERNGEKKO_P11_SWEEP=1)\n");
        }
    }
    /* p12 mt-watch: MODERNGEKKO_MT_WATCH=1 arms the method-table write
     * watch on [0x80AB1000..0x80AB3000) (default OFF — zero cost when
     * unset, single flag test at the journal top). */
    {
        const char* mt = getenv("MODERNGEKKO_MT_WATCH");
        if (mt && mt[0] == '1') {
            s_mt_watch = 1;
            if (s_journal_filter_enabled)
                rel_loader_rebuild_journal_mask();
            fprintf(stderr,
                    "[mt-watch] armed: watch [0x%08X..0x%08X), ring %u "
                    "(MODERNGEKKO_MT_WATCH=1)\n",
                    MT_WATCH_LO, MT_WATCH_HI, MT_RING_MAX);
        }
    }
    /* p13 thr-watch: MODERNGEKKO_THREAD_WATCH=1 arms the OSCreateThread /
     * OSResumeThread watch (default OFF — zero cost when unset, single
     * flag test per hook). Independent of MODERNGEKKO_MT_WATCH. */
    {
        const char* tw = getenv("MODERNGEKKO_THREAD_WATCH");
        if (tw && tw[0] == '1') {
            s_thr_watch = 1;
            fprintf(stderr,
                    "[thr-watch] armed: OSCreateThread 0x8030803C + "
                    "OSResumeThread 0x803086A4 (MODERNGEKKO_THREAD_WATCH=1)\n");
        }
    }
    /* p14 loadseam: MODERNGEKKO_LOADSEAM_TRACE=1 arms the file-select
     * decision-point watch (default OFF — bit-identical boots when unset). */
    {
        const char* ls = getenv("MODERNGEKKO_LOADSEAM_TRACE");
        if (ls && ls[0] == '1') {
            s_loadseam_trace = 1;
            fprintf(stderr,
                    "[loadseam] armed: menuSelect/YesNoSelect/392c/"
                    "changeGameScene watch + mc-control journal "
                    "(MODERNGEKKO_LOADSEAM_TRACE=1)\n");
        }
    }
    s_smc_dol_ranges = ppc_smc_dol_chunk_ranges();
    smc_build_page_classification();
    ppc_set_mem_write_journal(rel_loader_write_journal, NULL);
    {
        const char* filt = getenv("MODERNGEKKO_JOURNAL_FILTER");
        if (filt && filt[0] == '1') {
            s_journal_filter_enabled = 1;
            rel_loader_rebuild_journal_mask();
            if (s_debug)
                fprintf(stderr, "[rel_loader] journal page-filter armed\n");
        }
    }
    /* W-c O3 phase 1: MODERNGEKKO_INLINE_XLAT=1 arms the single-probe direct
     * MEM1 access path (cpu_init validates the GC-sized RAM before it goes
     * live; default OFF = bit-exact legacy probes). */
    {
        const char* xlat = getenv("MODERNGEKKO_INLINE_XLAT");
        if (xlat && xlat[0] == '1') {
            ppc_set_mem_inline_xlat(1);
            if (s_debug)
                fprintf(stderr, "[rel_loader] inline-xlat requested (arms at cpu_init)\n");
        }
    }
    /* B28/pulse12: MODERNGEKKO_BAND_PROTECT=1 arms the allocator-level L-band
     * reservation (default OFF = bit-identical legacy behavior). Bands are
     * static data; verify the generator's sort contract once up front so the
     * binary-search helper can trust disjointness forever. */
    {
        const char* bp = getenv("MODERNGEKKO_BAND_PROTECT");
        if (bp && bp[0] == '1') {
            unsigned k;
            if (REL_BAND_COUNT == 0u) {
                /* The armed banner below dereferences s_rel_bands[0]; an
                 * empty table is a generator bug, not "protect nothing". */
                fprintf(stderr, "[band] FATAL: band table is empty\n");
                exit(3);
            }
            for (k = 0; k < REL_BAND_COUNT; k++) {
                /* Per-row non-emptiness (row 0 was previously unchecked) plus
                 * the sorted/disjoint contract between neighbours. */
                if (!(s_rel_bands[k].lo < s_rel_bands[k].hi) ||
                    (k != 0 && !(s_rel_bands[k - 1].lo < s_rel_bands[k].lo &&
                                 s_rel_bands[k - 1].hi <= s_rel_bands[k].lo))) {
                    fprintf(stderr, "[band] FATAL: band table not sorted/disjoint at %u\n",
                            k);
                    exit(3);
                }
            }
            s_band_protect = 1;
            fprintf(stderr,
                    "[band] armed: %u bands covering 0x%08X..0x%08X "
                    "(MODERNGEKKO_BAND_PROTECT=1)\n",
                    (unsigned)REL_BAND_COUNT, s_rel_bands[0].lo,
                    s_rel_bands[REL_BAND_COUNT - 1].hi);
        }
    }
    if (s_debug)
        fprintf(stderr, "[rel_loader] init: post-write journal installed, %u REL chunks, "
                "%u DOL watch ranges\n",
                (unsigned)REL_LOADER_CHUNK_COUNT, (unsigned)s_smc_dol_count);
}

/* Site inline-cache resolver (REL half) — see cycle_budget.h. Transformed
 * generated chunks call this through mg_lookup_fn on their miss path:
 *   - L-band pc inside a compiled chunk AND inside an active module's L
 *     window        -> that chunk fn
 *   - retail pc inside an active module's R window whose L twin is compiled
 *                   -> that fn, *resolved_pc patched to the twin (the pc the
 *                      chunk code must see for pc-relative reads)
 * Returns NULL for uncompiled pcs, dead-module pcs, and every E4-gate
 * address (its post-call profile-list fixup must re-run per dispatch, so it
 * is never site-cacheable — NULL sends the site back to the claim path). */
static int rel_is_e4_gate(u32 l_addr)
{
    if (!s_e4_dup)
        return l_addr == s_e4_addr;
    {
        u32 mi;
        for (mi = 0; mi < s_active_count; mi++) {
            if (s_active[mi].id == 1u &&
                l_addr == s_active[mi].l + 0xECu)
                return 1;
        }
    }
    return 0;
}

void (*mg_rel_lookup(u32 pc, u32* resolved_pc))(CPUState*)
{
    u32 ci;
    *resolved_pc = pc;
    if (pc < 0x80500000u)
        return (void (*)(CPUState*))0;
    /* L-band direct: compiled chunk + an active module owns the address. */
    {
        int lo = 0, hi = (int)REL_LOADER_CHUNK_COUNT - 1, mid = -1;
        while (lo <= hi) {
            mid = (lo + hi) >> 1;
            if (pc < s_rel_loader_chunks[mid].start) hi = mid - 1;
            else if (pc >= s_rel_loader_chunks[mid].end) lo = mid + 1;
            else break;
        }
        if (lo <= hi) {
            for (ci = 0; ci < s_active_count; ci++) {
                if (pc >= s_active[ci].l &&
                    pc < s_active[ci].l + s_active[ci].file_size) {
                    if (rel_is_e4_gate(pc))
                        return (void (*)(CPUState*))0;
                    return s_rel_loader_chunks[mid].fn;
                }
            }
            return (void (*)(CPUState*))0;
        }
    }
    /* Retail alias: find the active module whose R window owns the address
     * and twin to l + (pc - r); resolve only when the twin is compiled.
     * R windows can overlap, so keep scanning owners until one twins into
     * compiled code (mirrors the dispatch path's p18 loop). */
    for (ci = 0; ci < s_active_count; ci++) {
        const RelActiveModule* am = &s_active[ci];
        u32 twin;
        int tlo, thi, tmid;
        if (!am->l || pc < am->r || pc >= am->r + am->file_size)
            continue;
        twin = am->l + (pc - am->r);
        tlo = 0; thi = (int)REL_LOADER_CHUNK_COUNT - 1; tmid = -1;
        while (tlo <= thi) {
            tmid = (tlo + thi) >> 1;
            if (twin < s_rel_loader_chunks[tmid].start) thi = tmid - 1;
            else if (twin >= s_rel_loader_chunks[tmid].end) tlo = tmid + 1;
            else break;
        }
        if (tlo > thi)
            continue;
        if (rel_is_e4_gate(twin))
            return (void (*)(CPUState*))0;
        *resolved_pc = twin;
        return s_rel_loader_chunks[tmid].fn;
    }
    return (void (*)(CPUState*))0;
}
