/*
 * mods/frame60-accum/mod.c — Option B 30Hz-logic / 60Hz-present accumulator.
 *
 * Implements the decoupling described in .org/port/phase3/frame60-impl-plan.md
 * §4.2 (+ arbitration Q3 D11) as a REVIEWABLE PATCH. Host built as a ModernGekko
 * .mgm mod (MODERNGEKKO_MOD_ABI_VERSION 1, CPU ABI 3, game_id GZLE01).
 *
 * Hook sites (all DOL, guest addresses):
 *   H1  JFWDisplay::beginRender  0x802558CC  — frame gate + accumulator.
 *       Forces retrace pacing via guest-memory writes (persist under stock ABI)
 *       and decides s_logic_this_frame for this iteration. Runs BEFORE the
 *       original waitForTick; at entry field_0x34 holds the PREVIOUS slot's
 *       OSGetTick delta (JFWDisplay.cpp:260).
 *   H2  fpcM_Execute             0x8003E370  — per-process update gate.
 *       On render-only (R) frames, `state->pc = state->lr` skips that process
 *       update. Requires R-0 (entry hooks persist) — the ModManager::Dispatch
 *       patch in this same patchset. Queue walk is traversal-safe
 *       (cNdIt_Method ignores callback return).
 *   H3  fapGm_After              0x800231BC  — scene/overlay/camera manager gate.
 *       Same R-frame skip (fopScnM/fopOvlpM/fopCamM at 30 Hz).
 *   H4  fpcDt_Handler            0x8003D314  — process delete queue (void).
 *   H5  fpcPi_Handler            0x8003FF00  — process priority queue (s32,
 *       must return nonzero or f_pc_manager asserts).
 *   H6  fpcCt_Handler            0x8003D150  — process create queue (BOOL,
 *       must return nonzero or f_pc_manager asserts).
 *   H7  cCt_Counter              0x802449AC  — global frame counters (void).
 *   H8  mDoAud_Execute           0x80007224  — JAI control pump (av-sync).
 *   H9  mDoCPd_Read              0x800078C0  — pad sampling (input polling).
 *   H10 waitForTick              0x80255D34  — render-interval override.
 *
 * Adaptive rule: split ONLY when guest requests the 30fps tick lock
 * (mTickRate == 1_350_000 == (OS_BUS_CLOCK/4)/30). Pass-through scenes
 * (menu/logo/movie: mTickRate != 1_350_000) run logic every frame.
 *
 * EXIT-1 gate: the accumulator is PRESENTATION-only. Below ~100% emulation
 * throughput the per-slot delta >= 1_350_000 so logic fires every frame —
 * behaviourally identical to retail. The split is EXIT-1-gated by definition
 * (arbitration Q3 §6.2): the mod ships disabled by default and is enabled
 * via conf `frame60_accum = true`, env `MODERNGEKKO_FRAME60_ACCUM=1`, or the
 * open-rate path: runner `--uncapped` / `--open-frame-rate`, env
 * `MODERNGEKKO_UNCAPPED=1`, config `frame_rate=uncapped` (the runner exports
 * MODERNGEKKO_FRAME60_ACCUM=1; the direct MODERNGEKKO_UNCAPPED check below
 * covers hosts that bypass the runner, e.g. the launcher).
 *
 * TIMER_RATIO=8 — EXPERIMENT-ONLY / NEVER-SHIP. This mod does NOT use
 * TIMER_RATIO. The SystemTimers TIMER_RATIO constant (vendor/dolphin/
 * Source/Core/Core/HW/SystemTimers.h:41) is a guest-time<->cycle calibration,
 * not a throughput lever. Ratio 8 lifts 254M from 15.7 -> 23.5 FPS at
 * 0.78x game speed, fast-forwards above 324M, and decouples audio. Marked
 * experiment-only per arbitration Q3 D11 §1.2 and timer-ratio-spec.md. No
 * release build or default config may set it; the calibration experiment
 * runs ONCE (V8+V6) and is discarded.
 */
#include "moderngekko/mod_abi.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#if defined(_WIN32)
#include <malloc.h>  /* _aligned_malloc / _aligned_free */
#else
#include <time.h>   /* clock_gettime fallback for host_now_ns */
#endif

/* ---- host wall clock (frame-cost accounting) -------------------------------
 * The mod ABI has no timer: ModernGekkoModHostApi only exposes
 * trigger_event/find_export and CPUState carries guest-facing fields. Guest
 * work is sampled via state->timebase (TB ticks, 40.5 MHz at the nominal
 * clock — the StaticRecompCore run loop advances it once per dispatch
 * segment, so deltas between two Painter entries are the guest cycles/12 the
 * frame consumed). For wall time the mod is a hosted DLL: use the host's own
 * monotonic clock. QPC is declared as a raw extern so <windows.h> stays out
 * of this TU (kernel32 is already linked for the CRT); llvm-mingw's
 * clock_gettime would pull in winpthreads, which the mod does not link. */
#if defined(_WIN32)
extern int QueryPerformanceCounter(long long* lpPerformanceCount);
extern int QueryPerformanceFrequency(long long* lpFrequency);
#endif
static uint64_t host_now_ns(void)
{
#if defined(_WIN32)
    static uint64_t s_freq = 0;
    long long v = 0;
    if (!s_freq) {
        long long f = 1;
        QueryPerformanceFrequency(&f);
        s_freq = (uint64_t)f;
    }
    QueryPerformanceCounter(&v);
    {
        const uint64_t uv = (uint64_t)v, f = s_freq;
        /* split the product so v*1e9 can't wrap u64 on long runs */
        return (uv / f) * 1000000000ull + ((uv % f) * 1000000000ull) / f;
    }
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* JFWDisplay singleton and field offsets (JFWDisplay.h:111-113). */
#define JFW_SINGLETON      0x803F7390u  /* sManager__10JFWDisplay (.sbss) */
#define JFW_OFF_FRAMERATE  0x20u        /* u16 mFrameRate */
#define JFW_OFF_TICKRATE   0x24u        /* u32 mTickRate  */
#define JFW_OFF_TICKDELTA  0x34u        /* u32 field_0x34: OSGetTick delta */
#define TICK_30FPS         1350000ull   /* (OS_BUS_CLOCK/4)/30 */
#define F60_FIELD_TICKS    (TICK_30FPS / 2ull) /* 675000: one 60Hz field  */
/* Pair-lock pad floor (MODERNGEKKO_F60_PAIR_LOCK). An R-iteration whose
 * preceding L-slot overran one field pads the upcoming slot to
 * (TICK_30FPS - d_L) instead of a full field so each L+R pair sums to one
 * 30Hz slot. 600K (~89% of a field) keeps the pair exact for d_L <= 750K —
 * the observed L-cost range is p50 ~692K / p99 ~702K with ~736K outliers —
 * bounds the residual overrun to (d_L - 750K) beyond that, and keeps the
 * slot from collapsing toward zero on a hitch (d_L >= TICK_30FPS would
 * otherwise wrap negative). */
#define F60_PAIR_MIN_PAD   600000ull

#ifndef MODERNGEKKO_FRAME60_ACCUM_DEFAULT
#define MODERNGEKKO_FRAME60_ACCUM_DEFAULT 0
#endif

#ifndef MODERNGEKKO_J3D_INTERP_DEFAULT
#define MODERNGEKKO_J3D_INTERP_DEFAULT 0
#endif

/* Host config: s_enabled = accumulator gate (EXIT-1); s_j3d_interp = Stage-B J3D history lerp.
 * Both default 0; enabled via env MODERNGEKKO_FRAME60_ACCUM=1 / MODERNGEKKO_J3D_INTERP=1.
 * s_enabled additionally honors MODERNGEKKO_UNCAPPED=1 (the --uncapped contract). */
static uint32_t s_enabled = MODERNGEKKO_FRAME60_ACCUM_DEFAULT;
static uint32_t s_j3d_interp = MODERNGEKKO_J3D_INTERP_DEFAULT;

/* Per-iteration state (host-side). */
static uint32_t s_split_mode = 0;       /* 1 = guest wants 30fps tick */
static uint32_t s_logic_this_frame = 1; /* 1 = run logic this iteration */
static float s_interp_alpha = 0.0f; /* sampled in H1 from s_acc/TICK_30FPS (Stage-B) */
static uint64_t s_acc = 0;              /* guest-timebase accumulator */
static uint64_t s_last_delta = 0;      /* tickdelta of previous iteration (decide) */
/* D-pairlock (MODERNGEKKO_F60_PAIR_LOCK): real cadence logs show L-slots
 * work-bound at ~692K and R-slots pad-bound at ~675K — each L+R pair spans
 * ~1367K, so the accumulator drifts ~16.8K per pair and slips an extra L
 * every ~40 pairs (a visible hitch; XFB copy boundaries also slide against
 * the VI field grid). decide() remembers the last L-class delta (d_L) and
 * the previous iteration's class; on an R-iteration it arms a shortened
 * waitForTick pad so the pair totals exactly one 30Hz slot. */
static uint32_t s_pair_lock = 1;        /* MODERNGEKKO_F60_PAIR_LOCK (default on) */
static uint32_t s_vi_lock = 1;          /* MODERNGEKKO_F60_VI_LOCK — retrace phase lock */
static uint32_t s_vi_locked = 0;        /* the last frame-gate wait was retrace-locked  */
static uint32_t s_display_alpha = 1;    /* MODERNGEKKO_F60_DISPLAY_ALPHA — R alpha from display time */
static uint32_t s_auto_degrade = 1;     /* MODERNGEKKO_F60_AUTO_DEGRADE — dup R-frames on a slow host */
static uint32_t s_prev_frame_class = 1; /* s_logic_this_frame at decide() entry */
static uint64_t s_l_delta = 0;          /* delta read by the last L-class decide  */
static uint32_t s_pair_pad = 0;         /* this iteration's wait pad (0 = normal) */
static uint32_t s_first = 0;            /* first-beginRender guard */
static uint32_t s_render_hz = 60;
static uint32_t s_jfw_display = 0;    /* cached JFWDisplay singleton */
static uint32_t s_debug = 0;          /* MODERNGEKKO_FRAME60_DEBUG */
static uint32_t s_painter_skip_off = 0; /* MODERNGEKKO_NO_PAINTER_SKIP — leaf-gate fallback */
static uint32_t s_rframe_render = 0;  /* MODERNGEKKO_RFRAME_RENDER — Painter runs on R-frames */
static uint32_t s_fifo_replay = 0;    /* MODERNGEKKO_FIFO_REPLAY — GP-stream replay on R-frames */
static uint32_t s_fifo_verify = 0;    /* MODERNGEKKO_FIFO_VERIFY — capture+scan only, no inject */
static uint32_t s_fifo_maxlen = 0;    /* MODERNGEKKO_FIFO_MAXLEN — bisect: cap injected bytes */
static uint32_t s_fifo_nop = 0;       /* MODERNGEKKO_FIFO_NOP — bisect: inject pure NOPs */
static uint32_t s_fifo_tail = 0;      /* MODERNGEKKO_FIFO_TAIL — bisect: inject frame tail */
static uint32_t s_fifo_multi = 1;     /* MODERNGEKKO_FIFO_MULTI — max injects per R-frame */
static uint32_t s_fifo_nocopy = 0;    /* MODERNGEKKO_FIFO_NOCOPY — bisect: drop trailing XFB copy */
static uint32_t s_judge_fast = 0;     /* MODERNGEKKO_JUDGE_FAST — native leaf-judge list walks */
static uint32_t s_libm_fast = 0;      /* MODERNGEKKO_LIBM_FAST — host libm for guest sin/cos */
static uint32_t s_xfbhash = 0;        /* MODERNGEKKO_F60_XFBHASH — log per-frame XFB content hash */
static uint32_t s_verify = 0;         /* MODERNGEKKO_F60_VERIFY — log R-frame fix invariants */
static uint32_t s_purge_free = 0;     /* MODERNGEKKO_F60_PURGE_FREE — per-free JKR purge hooks (off: dispatch cost) */
static uint32_t s_purge_bulk = 1;     /* MODERNGEKKO_F60_PURGE_BULK — freeAll/freeTail/destroy   */
static uint32_t s_drawlist_chk = 1;   /* MODERNGEKKO_F60_DRAWLIST_CHECK — draw() chain preflight */
static uint32_t s_model_alive = 1;    /* MODERNGEKKO_F60_MODEL_ALIVE — per-consume liveness      */
static uint32_t s_cam_interp = 1;     /* MODERNGEKKO_F60_CAM_INTERP — camera view/proj lerp      */
static uint32_t s_noinject = 0;       /* MODERNGEKKO_F60_NOINJECT — R-render repaint, no lerp    */
static uint32_t s_nocopyclr = 0;      /* MODERNGEKKO_F60_NOCOPYCLEAR — diag: R-copy skips EFB clear */
static uint32_t s_xfb_lum_enabled = 0;/* MODERNGEKKO_F60_XFBLUM — diag: log XFB luminance per copy    */
static uint32_t s_vilog = 0;          /* MODERNGEKKO_F60_VILOG — diag: per-copy/per-retrace timeline */
static float    s_vl_view_rend[12];   /* VILOG: view matrix of the image this iteration renders */
static float    s_vl_view_img[12];    /* VILOG: view matrix of the image the next copy ships    */
static char     s_stream_dump_dir[512];/* MODERNGEKKO_STREAM_DUMP — per-iteration GP byte dumps  */
static uint32_t s_stream_dump_n = 0;
static uint8_t* s_stream_dump_buf = 0;
static uint32_t s_stream_dump_max = 600u;/* MODERNGEKKO_STREAM_DUMP_MAX — file-count cap         */
static uint32_t s_wnum_gate = 1;      /* MODERNGEKKO_F60_WNUM_GATE — windowNum==0 -> dup present */
static uint32_t s_gp_gate = 0;        /* MODERNGEKKO_F60_GP_GATE — opt-in Painter-emission veto */
static uint32_t s_gp_min = 16384u;    /* MODERNGEKKO_F60_GP_MIN — bytes/frame qualifying as content */
static uint32_t s_trace = 0;          /* MODERNGEKKO_F60_TRACE — per-entry xfb/present trace */
static uint32_t s_xfb_fix = 1;        /* MODERNGEKKO_F60_XFB_FIX — force copy-branch on R-frames */
static uint32_t s_heap_pin = 1;       /* MODERNGEKKO_F60_HEAP_PIN — list-heap cadence fix       */
static uint32_t s_fader_fix = 1;      /* MODERNGEKKO_F60_FADER_FIX — R-frame fader replays L-frame draw */
static uint32_t s_sea_fix = 1;        /* MODERNGEKKO_F60_SEA_FIX — sea anim counter stays 30Hz          */
static uint32_t s_light_fix = 1;      /* MODERNGEKKO_F60_LIGHT_FIX — R-frame relight under lerped view  */
static uint32_t s_foliage_fix = 1;    /* MODERNGEKKO_F60_FOLIAGE_FIX — grass/flower/tree mtx lerp        */
static uint32_t s_jpa_fix = 1;        /* MODERNGEKKO_F60_JPA_FIX — particle mGlobalPosition lerp        */
static uint32_t s_fadelog = 0;        /* MODERNGEKKO_F60_FADELOG — fade-state forensics           */
static uint32_t s_rnglog = 0;         /* MODERNGEKKO_F60_RNGLOG — cM_rnd state per L-frame        */
static uint32_t s_rng_lcount = 0;     /* L-frames logged by RNGLOG (capped at 400)                */
static uint32_t s_dltlog = 0;         /* MODERNGEKKO_F60_DLTLOG — per-iter tick delta + class     */
static uint32_t s_dlt_n = 0;          /* iterations logged by DLTLOG (capped at 3000)             */
static uint32_t s_camlog = 0;         /* MODERNGEKKO_F60_CAMLOG — view-matrix cut diagnostics     */
static uint32_t s_purge_epoch_always = 0; /* MODERNGEKKO_F60_PURGE_EPOCH_ALWAYS — legacy bump     */
static uint32_t s_force_else = 0;     /* MODERNGEKKO_F60_FORCE_ELSE — L-frames take else-branch */
static uint32_t s_overlap_active = 0; /* fopOvlpM overlap in flight — R-frames take dup present  */
static uint32_t s_r_dup = 0;          /* this iteration: R-frame was redirected to dup present —
                                       * render-path gates key off it so hybrid dup frames get
                                       * the exact pure-dup semantics (no exchange, no endGX,
                                       * no draw-funnel runs). */
/* Camera-view state flags (history lives with the camera block — the flags
 * are declared here because on_painter_skip consumes them). */
static uint32_t s_cam_injected = 0;   /* live camera fields hold our lerp bits    */
static uint32_t s_cam_cut = 0;        /* view discontinuity: this R-frame = plain repaint */
static uint32_t s_cam_view = 0;       /* mCurrentView pointer being tracked/patched */
static uint32_t s_fol_cut = 0;        /* cam_cut mirrored for the packet draw hooks —
                                       * s_cam_cut is consumed/cleared at Painter
                                       * entry, before the draw calls run         */
static int32_t  s_trace_prev_drawn = -2; /* [ft] trace: previous drawn-XFB index   */
#define GINF_MCURRHEAP 0x803F68A8u        /* mDoGph_gInf_c::mCurrentHeap (r13-0x7838) */
/* Heap that holds the live packet lists — the one the last L-frame's fpcDw
 * allocated into. Under the Painter-entry pin, post-free mCurrentHeap is
 * always s_list_heap^1, so this just flips once per L-frame. Seeded from the
 * live mCurrentHeap at split engagement and mirrored while unsplit (H1). */
static uint32_t s_list_heap = 0;
static uint64_t s_dbg_jfast = 0, s_dbg_jmiss = 0;
static uint64_t s_dbg_lframes = 0, s_dbg_rframes = 0;

/* ---- per-frame-class cost accounting --------------------------------------
 * MODERNGEKKO_FRAME60_COST=1 (or MODERNGEKKO_FRAME60_DEBUG=1) arms this. At
 * each on_painter_skip entry, the delta since the PREVIOUS entry is charged
 * to the frame class that ran during the interval — s_split_mode /
 * s_logic_this_frame still hold that class at hook top because decide() runs
 * below and only then overwrites them. Three buckets: L (split, logic+render),
 * R (split, render-only), O (unsplit / passthrough frames). Two meters each:
 * wall ns (host_now_ns) and guest TB ticks (state->timebase). Wall ns is the
 * number that maps to native_ms share; TB ticks isolate guest work from any
 * host-side stall the interval happened to include. Off = one predictable
 * branch at painter entry, same contract as s_debug. */
static uint32_t s_cost = 0;
static uint64_t s_cost_prev_ns = 0, s_cost_prev_tb = 0;
static uint32_t s_cost_prev_valid = 0;
static uint64_t s_cost_l_ns = 0, s_cost_r_ns = 0, s_cost_o_ns = 0;
static uint64_t s_cost_l_tb = 0, s_cost_r_tb = 0, s_cost_o_tb = 0;
static uint64_t s_cost_l_n = 0,  s_cost_r_n = 0,  s_cost_o_n = 0;
/* Windowed twins — reset on each [f60-cost] line so the printout shows the
 * last ~8 iterations, while the s_cost_* set stays cumulative. */
static uint64_t s_wl_ns = 0, s_wr_ns = 0, s_wo_ns = 0;
static uint64_t s_wl_tb = 0, s_wr_tb = 0, s_wo_tb = 0;
static uint32_t s_wl_n = 0, s_wr_n = 0, s_wo_n = 0;
/* Mod self-work: wall ns + call count spent inside the mod's own per-frame
 * helpers (separates mod overhead from guest Painter work). */
static uint64_t s_c_snap_ns = 0, s_c_snap_n = 0;  /* j3d_snapshot_pose  (L) */
static uint64_t s_c_refr_ns = 0, s_c_refr_n = 0;  /* j3d_rframe_refresh (R) */
static uint64_t s_c_inj_ns  = 0, s_c_inj_n  = 0;  /* j3d_rframe_inject  (R) */
static uint64_t s_c_frep_ns = 0, s_c_frep_n = 0;  /* fifo_replay_frame  (R) */

/* Forward: env/config reader — runs at mod load, before guest starts. */
static void frame60_accum_on_load(const ModernGekkoModHostApi* api)
{
    (void)api;
#if defined(__STDC_HOSTED__) || 1
    {
        extern char* getenv(const char* name);
        const char* v = getenv("MODERNGEKKO_FRAME60_ACCUM");
        if (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T'))
            s_enabled = 1;
        /* Open-rate alias (--uncapped / MODERNGEKKO_UNCAPPED / frame_rate=uncapped). */
        const char* u = getenv("MODERNGEKKO_UNCAPPED");
        if (u && (u[0] == '1' || u[0] == 'y' || u[0] == 'Y' || u[0] == 't' || u[0] == 'T'))
            s_enabled = 1;
        const char* v2 = getenv("MODERNGEKKO_J3D_INTERP");
        if (v2 && (v2[0] == '1' || v2[0] == 'y' || v2[0] == 'Y' || v2[0] == 't' || v2[0] == 'T'))
            s_j3d_interp = 1;
        const char* render_hz = getenv("MODERNGEKKO_RENDER_HZ");
        if (render_hz && render_hz[0]) {
            char* end = 0;
            unsigned long parsed = strtoul(render_hz, &end, 10);
            if (end && *end == '\0' && parsed <= 1000ul)
                s_render_hz = (uint32_t)parsed;
        }
        const char* dbg = getenv("MODERNGEKKO_FRAME60_DEBUG");
        if (dbg && dbg[0] == '1')
            s_debug = 1;
        const char* ce = getenv("MODERNGEKKO_FRAME60_COST");
        if (ce && ce[0] == '1')
            s_cost = 1;
        if (s_debug)
            s_cost = 1;   /* debug implies cost accounting */
        const char* nps = getenv("MODERNGEKKO_NO_PAINTER_SKIP");
        if (nps && nps[0] == '1')
            s_painter_skip_off = 1;
        const char* rfr = getenv("MODERNGEKKO_RFRAME_RENDER");
        if (rfr && rfr[0] == '1')
            s_rframe_render = 1;
        const char* frep = getenv("MODERNGEKKO_FIFO_REPLAY");
        if (frep && frep[0] == '1')
            s_fifo_replay = 1;
        const char* fver = getenv("MODERNGEKKO_FIFO_VERIFY");
        if (fver && fver[0] == '1')
            s_fifo_verify = 1;
        const char* fmax = getenv("MODERNGEKKO_FIFO_MAXLEN");
        if (fmax && fmax[0])
            s_fifo_maxlen = (uint32_t)strtoul(fmax, 0, 0);
        const char* fnop = getenv("MODERNGEKKO_FIFO_NOP");
        if (fnop && fnop[0] == '1')
            s_fifo_nop = 1;
        const char* ftail = getenv("MODERNGEKKO_FIFO_TAIL");
        if (ftail && ftail[0] == '1')
            s_fifo_tail = 1;
        const char* fnc = getenv("MODERNGEKKO_FIFO_NOCOPY");
        if (fnc && fnc[0] == '1')
            s_fifo_nocopy = 1;
        const char* fmul = getenv("MODERNGEKKO_FIFO_MULTI");
        if (fmul && fmul[0]) {
            uint32_t m = (uint32_t)strtoul(fmul, 0, 0);
            s_fifo_multi = m < 1u ? 1u : (m > 4u ? 4u : m);
        }
        const char* xh = getenv("MODERNGEKKO_F60_XFBHASH");
        if (xh && xh[0] == '1')
            s_xfbhash = 1;
        const char* vf = getenv("MODERNGEKKO_F60_VERIFY");
        if (vf && vf[0] == '1')
            s_verify = 1;
        const char* wg = getenv("MODERNGEKKO_F60_WNUM_GATE");
        if (wg && wg[0] == '0')
            s_wnum_gate = 0;
        const char* gg = getenv("MODERNGEKKO_F60_GP_GATE");
        if (gg && gg[0] == '1')
            s_gp_gate = 1;
        const char* gm = getenv("MODERNGEKKO_F60_GP_MIN");
        if (gm) {
            const int v = atoi(gm);
            if (v > 0) s_gp_min = (uint32_t)v;
        }
        const char* tc = getenv("MODERNGEKKO_F60_TRACE");
        if (tc && tc[0] == '1')
            s_trace = 1;
        const char* xf = getenv("MODERNGEKKO_F60_XFB_FIX");
        if (xf && xf[0] == '0')
            s_xfb_fix = 0;
        const char* hp = getenv("MODERNGEKKO_F60_HEAP_PIN");
        if (hp && hp[0] == '0')
            s_heap_pin = 0;
        const char* ff = getenv("MODERNGEKKO_F60_FADER_FIX");
        if (ff && ff[0] == '0')
            s_fader_fix = 0;
        const char* fl = getenv("MODERNGEKKO_F60_FADELOG");
        if (fl && fl[0] == '1')
            s_fadelog = 1;
        const char* rl = getenv("MODERNGEKKO_F60_RNGLOG");
        if (rl && rl[0] == '1')
            s_rnglog = 1;
        const char* dl = getenv("MODERNGEKKO_F60_DLTLOG");
        if (dl && dl[0] == '1')
            s_dltlog = 1;
        const char* pk = getenv("MODERNGEKKO_F60_PAIR_LOCK");
        if (pk && pk[0] == '0')
            s_pair_lock = 0;
        const char* vk = getenv("MODERNGEKKO_F60_VI_LOCK");
        if (vk && vk[0] == '0')
            s_vi_lock = 0;
        const char* da = getenv("MODERNGEKKO_F60_DISPLAY_ALPHA");
        if (da && da[0] == '0')
            s_display_alpha = 0;
        const char* ad = getenv("MODERNGEKKO_F60_AUTO_DEGRADE");
        if (ad && ad[0] == '0')
            s_auto_degrade = 0;
        const char* cl = getenv("MODERNGEKKO_F60_CAMLOG");
        if (cl && cl[0] == '1')
            s_camlog = 1;
        const char* pa = getenv("MODERNGEKKO_F60_PURGE_EPOCH_ALWAYS");
        if (pa && pa[0] == '1')
            s_purge_epoch_always = 1;
        const char* sf = getenv("MODERNGEKKO_F60_SEA_FIX");
        if (sf && sf[0] == '0')
            s_sea_fix = 0;
        const char* lf = getenv("MODERNGEKKO_F60_LIGHT_FIX");
        if (lf && lf[0] == '0')
            s_light_fix = 0;
        const char* fol = getenv("MODERNGEKKO_F60_FOLIAGE_FIX");
        if (fol) s_foliage_fix = (fol[0] != '0');
        const char* jp = getenv("MODERNGEKKO_F60_JPA_FIX");
        if (jp) s_jpa_fix = (jp[0] != '0');
        const char* fe = getenv("MODERNGEKKO_F60_FORCE_ELSE");
        if (fe && fe[0] == '1')
            s_force_else = 1;
        /* Crash-fix kill switches — defaults are the shipped config; setting
         * =0 or =1 forces the piece off/on for A/B perf isolation.
         * PURGE_FREE controls the four per-free funnels (operator delete x2,
         * JKRHeap::free static+member) — default off: each free is a host
         * hook dispatch even when the body early-outs, and frees run many
         * times per frame; bulk+dtor+model_alive+armed preflight cover the
         * stale-pointer window. PURGE_BULK the freeAll/freeTail/destroy
         * trio; DRAWLIST_CHECK the draw() packet preflight; MODEL_ALIVE the
         * per-consume vtbl/mModelData check. */
        const char* pf = getenv("MODERNGEKKO_F60_PURGE_FREE");
        if (pf) s_purge_free = (pf[0] != '0');
        const char* pb = getenv("MODERNGEKKO_F60_PURGE_BULK");
        if (pb) s_purge_bulk = (pb[0] != '0');
        const char* dc = getenv("MODERNGEKKO_F60_DRAWLIST_CHECK");
        if (dc) s_drawlist_chk = (dc[0] != '0');
        const char* ma = getenv("MODERNGEKKO_F60_MODEL_ALIVE");
        if (ma) s_model_alive = (ma[0] != '0');
        const char* ci = getenv("MODERNGEKKO_F60_CAM_INTERP");
        if (ci) s_cam_interp = (ci[0] != '0');
        const char* nj = getenv("MODERNGEKKO_F60_NOINJECT");
        if (nj) s_noinject = (nj[0] != '0');
        const char* nc2 = getenv("MODERNGEKKO_F60_NOCOPYCLEAR");
        if (nc2) s_nocopyclr = (nc2[0] != '0');
        const char* xl = getenv("MODERNGEKKO_F60_XFBLUM");
        if (xl) s_xfb_lum_enabled = (xl[0] != '0');
        const char* vl = getenv("MODERNGEKKO_F60_VILOG");
        if (vl) s_vilog = (vl[0] != '0');
        const char* sd = getenv("MODERNGEKKO_STREAM_DUMP");
        if (sd && sd[0])
            snprintf(s_stream_dump_dir, sizeof(s_stream_dump_dir), "%s", sd);
        const char* sm = getenv("MODERNGEKKO_STREAM_DUMP_MAX");
        if (sm) {
            const int v = atoi(sm);
            if (v > 0) s_stream_dump_max = (uint32_t)v;
        }
        const char* tr = getenv("MODERNGEKKO_TIMER_RATIO");
        if (tr && tr[0] == '8') {
            /* workers: TIMER_RATIO=8 is calibration experiment only — never ship */
        }
        const char* jf = getenv("MODERNGEKKO_JUDGE_FAST");
        /* Default on: the fast path is a semantics-exact native rewrite of
         * pure-read leaf judges; MODERNGEKKO_JUDGE_FAST=0 disables it. */
        s_judge_fast = !(jf && jf[0] == '0');
        const char* lm = getenv("MODERNGEKKO_LIBM_FAST");
        /* Default OFF — measured net-negative (2026-09-20, clean capped A/B):
         * ~47.5 VI Hz on vs ~52 off. The ppc_host_call round-trip + fpr write
         * + lr re-dispatch costs more than the ~40-instr guest bodies. Host
         * replacement only pays for long guest functions. LIBM_FAST=1 opts in. */
        s_libm_fast = (lm && lm[0] == '1');
    }
#endif
}

static void j3d_rframe_inject(CPUState* state, float alpha);
static void j3d_rframe_refresh(CPUState* state);
static void j3d_restore_injected(CPUState* state);
static void j3d_snapshot_pose(CPUState* state);
static void camview_lframe(CPUState* state);
static void camview_rframe(CPUState* state);
static void camview_install(CPUState* state, float alpha);
static void camview_restore(CPUState* state, uint32_t view);
static void jpa_lframe(CPUState* state);
static void jpa_rframe(CPUState* state, float alpha);
static void f60_reset_runtime_state(CPUState* state);
static void vilog_note_eye(CPUState* state);
static void fifo_probe(CPUState* st);
static void fifo_replay_frame(CPUState* st);
static void fifo_capture(CPUState* st, uint32_t base, uint32_t end,
                         uint32_t start, uint32_t len, uint8_t* dst);
/* Guest-memory accessors — the per-frame paths all use the direct-RAM
 * helpers (one bounds check, then plain BE loads/stores on state->ram;
 * the moderngekko_mod_read/write externs go through the chassis
 * TranslateRelAddress+MSR+MMU path per word). Definitions live with the
 * FIFO/J3D block below; non-MEM1 targets fall back to the safe accessors. */
static uint32_t rd32_fast(CPUState* s, uint32_t a);
static uint32_t rd16_fast(CPUState* s, uint32_t a);
static uint32_t rd8_fast(CPUState* s, uint32_t a);
static int in_ram(const CPUState* s, uint32_t ea, uint32_t size);
static void wr32_fast(CPUState* s, uint32_t a, uint32_t v);
static void wr16_fast(CPUState* s, uint32_t a, uint32_t v);
static void wr8_fast(CPUState* s, uint32_t a, uint32_t v);
/* diag counters (defined with the J3D history block) */
static uint64_t s_inj_models, s_inj_writes, s_rest_models, s_rest_writes, s_snap_models;
static uint64_t s_dbg_snapcall, s_dbg_skipni;
static uint64_t s_dbg_replays, s_dbg_capbytes, s_dbg_patched, s_dbg_nofifo;
static uint64_t s_dbg_np, s_dbg_nc;   /* scan_xf_loads found in prev/curr */
static uint64_t s_dbg_purged;         /* history slots dropped by the heap-free purge */
static uint64_t s_dbg_ovr_dup;        /* R-frames steered to dup-present by an active overlap */
static uint64_t s_dbg_w0_dup;         /* R-frames steered to dup-present by windowNum==0        */
static uint64_t s_dbg_camcut;         /* R-frames repainted un-interpolated after a camera cut */
static uint64_t s_dbg_caminj;         /* camera view/proj mid-view installs */
static uint64_t s_dbg_lightgate;      /* dKy_setLight R-frame intercepts (skip or relight) */
static uint64_t s_dbg_folinj;         /* foliage element matrices lerped on R-frames        */
static uint64_t s_dbg_jpainj;         /* JPA particle positions lerped on R-frames          */
static uint32_t s_hist_used;
static uint32_t s_matrices_injected = 0; /* live guest arrays currently hold our lerp */

/* g_dComIfG_gameInfo (0x803C4C08) + play (0x12A0) + mDlstWindowNum (0x4841).
 * Painter's entire 3D path is inside `if (dComIfGp_getWindowNum() != 0)` —
 * the deferred dDlst packet pipeline only carries a scene when a window is
 * up (dScnPlay sets 1; title/logo/opening/menu scenes leave it 0 and draw
 * inline inside fpcDw, which R-frames gate). Re-rendering those scenes on
 * an R-frame replays empty/stale lists -> a black presented frame (the
 * strict content/black alternation seen across the whole title+attract).
 * windowNum==0 therefore means "nothing to replay" — dup-present instead. */
#define GAMEINFO_WNUM      0x803CA6E9u  /* u8 — mDlstWindowNum (0x803C4C08+0x12A0+0x4841) */
#define GINF_MCAPTURESTEP  0x803F68BEu  /* s16 — mDoGph_Painter picto-box capture state   */

/* PI fifo write-pointer regs (MMIO): the gather pipe write position only
 * advances when guest code emits display commands. Sampling it at each
 * Painter entry measures exactly how many GP bytes the previous frame's
 * Painter body produced — i.e. what an R-frame re-render of the persistent
 * lists would draw. ~0 means the scene is drawn inline inside fpcDw
 * (title/opening/logos/dialog captures), so a Painter-only replay would
 * present an empty/black frame -> duplicate-present instead. A false
 * negative only costs interpolation for that frame (30Hz present, still
 * correct) — the gate is deliberately biased toward dup. */
#define PI_FIFO_BASE_REG   0xCC00300Cu
#define PI_FIFO_END_REG    0xCC003010u
#define PI_FIFO_WPTR_REG   0xCC003014u
#define FIFO_STREAM_MAX    (768u * 1024u)
#define JUTXFB_MANAGER     0x803F78F0u
static uint32_t s_wptr_prev = 0;
static uint32_t s_wptr_fsize = 0;
static int      s_wptr_valid = 0;
static uint32_t s_pd_bytes = 0;       /* GP bytes the last L-frame's Painter emitted */
static uint32_t s_render_conf = 0;    /* consecutive qualifying L-frames (>=2 => render) */
static uint64_t s_dbg_gp_dup = 0;     /* R-frames steered to dup-present by the GP gate        */
static uint64_t s_dbg_wnum_dup = 0;   /* R-frames steered to dup-present by windowNum==0       */
static uint64_t s_dbg_cap_dup = 0;    /* R-frames steered to dup-present by mCaptureStep!=0    */
static uint32_t s_rd_wptr0 = 0;       /* wptr at R-frame render-path start */
static uint32_t s_rd_bytes = 0;       /* GP bytes the last R-frame Painter body emitted */
/* Emission split for the dup/render discriminator: the frame loop runs
 * Painter[N] -> fpcEx/fpcDw[N] -> Painter[N+1]. Sampling wptr at each
 * function entry splits the frame into Painter's own replay emission
 * (what an R-frame re-render would draw) and fpcDw's inline emission
 * (what only fpcDw produces — movie quads, J2D screens, actor draws).
 * An R-frame can only reproduce the Painter part; if the visible content
 * rides in fpcDw's span, re-rendering presents black -> dup instead. */
static uint32_t s_painter_entry_wptr = 0;
static uint32_t s_fcdw_entry_wptr = 0;
static uint32_t s_painter_bytes = 0;  /* GP bytes inside the last Painter body */
static uint32_t s_fcdw_bytes = 0;     /* GP bytes inside the last fpcDw+execute span */
static int      s_split_valid = 0;
/* MODERNGEKKO_STREAM_DUMP=<dir>: at each Painter entry, write the fifo region
 * emitted since the previous Painter entry (one full iteration's GP bytes:
 * Painter replay + fpcDw inline for L, Painter replay only for R) to
 * <dir>/<seq>_{L,R}.bin — offline diff shows exactly what each class emits. */

/* H1: JFWDisplay::beginRender 0x802558CC — frame gate + accumulator.
 * Entry hook: runs BEFORE the original (which does waitForTick -> XFB
 * exchange). Guest-memory writes persist under stock ABI; no CPUState
 * mutation needed here. */
/* Accumulator + split detection. MUST live in a hook that fires on every
 * frame-loop iteration. Painter (0x8000AF2C) is invoked once per iteration by
 * fpcM_Management, so on_painter_skip is the correct owner — beginRender's
 * entry hook does NOT fire when the frame reaches beginRender via the
 * redirect->passthrough->JIT path (the JIT runs the block without
 * re-dispatching its hook). */
static void frame60_accum_decide(CPUState* state, uint32_t* disp_out)
{
    /* At Painter entry s_logic_this_frame still holds the PREVIOUS
     * iteration's class (nothing else writes it between calls) — snapshot
     * before the early-outs below can overwrite it; the pair-lock check
     * needs "was the frame that just ended an L?". */
    s_prev_frame_class = s_logic_this_frame;
    uint32_t disp = rd32_fast(state, JFW_SINGLETON);
    /* disp is guest memory — a corrupt value could wrap disp+offset into an
     * aliased in-bounds address that returns a plausible mTickRate and arms
     * the split spuriously. Require the whole object window (fields to
     * +0x38) to sit inside MEM1 (0x80000000..0x81800000). The subtract wraps
     * disp < 0x80000000 to a huge value, so one compare covers both ends. */
    if (disp == 0u || (disp - 0x80000000u) > (0x01800000u - 0x38u)) {
        s_pair_pad = 0;
        if (disp_out) *disp_out = 0u;
        return;
    }
    s_jfw_display = disp;
    if (disp_out) *disp_out = disp;

    uint32_t mTick = rd32_fast(state, disp + JFW_OFF_TICKRATE);
    uint32_t next_split = (s_enabled && mTick == (uint32_t)TICK_30FPS) ? 1u : 0u;
    if (s_debug) {
        static uint32_t last_mt = 0xFFFFFFFFu;
        if (mTick != last_mt) {
            fprintf(stderr, "[f60] mTick=%u split=%u\n", mTick, next_split);
            last_mt = mTick;
        }
    }
    if (!next_split) {
        if (s_split_mode)
            f60_reset_runtime_state(state);   /* disengage edge (M1) */
        /* While unsplit, mirror the live heap instead of blindly flipping:
         * post-free mCurrentHeap names the heap fpcDw last filled = the
         * live-list heap, so this keeps the parity truthful across any
         * unsplit stretch and re-engagement (H1). */
        s_list_heap = rd8_fast(state, GINF_MCURRHEAP) & 1u;
        s_split_mode = 0;
        s_logic_this_frame = 1;
        s_interp_alpha = 0.0f;
        s_acc = 0;
        s_first = 0;
        s_overlap_active = 0;
        s_pair_pad = 0;
        s_l_delta = 0;
        return;
    }

    if (!s_split_mode) {
        /* Engagement edge: reset every per-frame runtime static (M1) and
         * seed the list-heap mirror from the live mCurrentHeap (H1) — at
         * Painter entry it names the heap the last fpcDw filled = the
         * live-list heap. Seeding kills the parity window where the pin
         * could name the LIVE heap and free() would destroy the replay
         * packet list. (There is no state-load hook in the mod ABI — only
         * on_load/on_unload — so runtime state is rebuilt here instead of
         * being restored.) */
        f60_reset_runtime_state(state);
        s_jfw_display = disp;
    }
    s_split_mode = 1;
    if (!s_first) {
        s_first = 1;
        s_logic_this_frame = 1;
        s_interp_alpha = 0.0f;
        s_pair_pad = 0;
        if (s_debug) {
            fprintf(stderr, "[f60] split engaged (mTick=%u)\n", mTick);
        }
        return;
    }

    uint32_t delta = rd32_fast(state, disp + JFW_OFF_TICKDELTA);
    s_last_delta = delta;
    s_acc += (uint64_t)delta;
    if (s_acc >= TICK_30FPS) {
        s_logic_this_frame = 1;
        s_acc -= TICK_30FPS;
    } else {
        s_logic_this_frame = 0;
    }
    /* Pair-lock (D-judder). waitForTick's p1 floors the interval to the NEXT
     * wait exit (nextTick = time + p1 at exit, JFWDisplay.cpp:347-356), so
     * the arg this iteration's beginRender writes paces the slot ending at
     * beginRender_{N+1}. On an R-iteration that slot is the pad-bound one —
     * shorten it by the preceding L-slot's over-field excess so each pair
     * sums to one 30Hz slot. d_L is s_l_delta: the delta the last L decide
     * consumed (the work-bound interval containing the L's render tail);
     * the in-flight interval ending at this R's beginRender is not yet
     * measurable at hook time. */
    if (s_logic_this_frame) {
        s_l_delta = delta;
        s_pair_pad = 0u;
    } else {
        s_pair_pad = 0u;
        if (s_pair_lock && s_prev_frame_class &&
            s_l_delta > (uint64_t)F60_FIELD_TICKS) {
            uint64_t pp = (s_l_delta < TICK_30FPS)
                        ? (TICK_30FPS - s_l_delta) : 0ull;
            if (pp < (uint64_t)F60_PAIR_MIN_PAD)
                pp = (uint64_t)F60_PAIR_MIN_PAD;
            s_pair_pad = (uint32_t)pp;
        }
    }
    if (s_dltlog && s_dlt_n < 3000u) {
        /* Per-iteration guest-tick delta + class — the judder probe: real
         * scene cadence is L-over/R-under alternating, the input L3's
         * pair-lock and replay harnesses consume. */
        fprintf(stderr, "[dlt] %c %u\n", s_logic_this_frame ? 'L' : 'R',
                (unsigned)delta);
        ++s_dlt_n;
    }
    if (s_acc >= TICK_30FPS)
        s_acc = (uint64_t)(TICK_30FPS - 1ull);
    /* alpha in [0,1): how far into the 30Hz logic slot this render frame sits.
     * Used by both the J3D matrix path and the GP-stream lerp. */
    s_interp_alpha = (float)((double)s_acc / (double)TICK_30FPS);
    /* Display-time alpha. L-frames draw their pose exactly (alpha 0), so an
     * R-frame must sit at the fraction of DISPLAY time between the two
     * L-frames around it — not at the accumulator residual, which is just
     * the logic schedule's phase against the render schedule and parks
     * anywhere in [0.5,1). Measured (VILOG eye trace, flyover): alpha ~0.8
     * gave 24/6-unit alternating camera steps where 15/15 was due. Under
     * the VI lock every image spans exactly one field and the pattern is
     * L,R,L,R (slips are L,L — never R,R), so an R right after an L is
     * exactly half-way. Other rates/unlocked pacing keep the residual. */
    if (s_display_alpha && s_vi_locked && s_render_hz == 60u &&
        !s_logic_this_frame && s_prev_frame_class)
        s_interp_alpha = 0.5f;
    /* Transition-churn guard: while an overlap request is in flight the
     * scene is tearing down/creating processes and heaps — the persistent
     * packet list and the J3D history both reference memory that can be
     * released outside the Dt-ordered path (heap destroy/freeAll, REL
     * unload). The danger is only the R-frame REPAINT walking those stale
     * pointers, so keep the normal 30Hz L/R cadence here and let
     * on_painter_skip steer R-frames to the duplicate-present tail-call —
     * a dup R-frame touches neither the packet list nor the J3D arrays.
     * (The previous version forced s_logic_this_frame=1 for the whole
     * ~1-1.7s create->cover->peek->teardown->reveal sequence, double-timing
     * JUTFader, mFadeInTime/OutTime, peektime, door wipes and every gated
     * system — fades completed in ~0.43s.) If the request ever wedges, the
     * cadence still holds at 30Hz — no 2x logic, just dup presents.
     * l_fopOvlpM_overlap[0] @0x803F6160 (GZLE01 BSS). */
    {
        const uint32_t ovr = (rd32_fast(state, 0x803F6160u) != 0u) ? 1u : 0u;
        if (s_debug && ovr != s_overlap_active)
            fprintf(stderr, "[f60] overlap %s — R-frames %s\n",
                    ovr ? "engaged" : "cleared",
                    ovr ? "-> dup present" : "resume normal path");
        s_overlap_active = ovr;
    }
    if (s_debug) {
        if (s_logic_this_frame) ++s_dbg_lframes; else ++s_dbg_rframes;
        if (((s_dbg_lframes + s_dbg_rframes) & 0x07u) == 0) {
            fprintf(stderr, "[f60] L=%llu R=%llu (%.1f%% L) acc=%llu inj=%llu/%llu res=%llu/%llu snap=%llu trk=%u snapcall=%llu skipni=%llu rep=%llu capKB=%llu pat=%llu nofifo=%llu np=%llu nc=%llu\n",
                    (unsigned long long)s_dbg_lframes,
                    (unsigned long long)s_dbg_rframes,
                    100.0 * (double)s_dbg_lframes / (double)(s_dbg_lframes + s_dbg_rframes),
                    (unsigned long long)s_acc,
                    (unsigned long long)s_inj_models, (unsigned long long)s_inj_writes,
                    (unsigned long long)s_rest_models, (unsigned long long)s_rest_writes,
                    (unsigned long long)s_snap_models, s_hist_used,
                    (unsigned long long)s_dbg_snapcall,
                    (unsigned long long)s_dbg_skipni,
                    (unsigned long long)s_dbg_replays,
                    (unsigned long long)(s_dbg_capbytes / 1024ull),
                    (unsigned long long)s_dbg_patched,
                    (unsigned long long)s_dbg_nofifo,
                    (unsigned long long)s_dbg_np,
                    (unsigned long long)s_dbg_nc);
            {
                /* pk = non-NULL packet count of each draw buffer's mpBuf
                 * (scan capped at 96 slots); frameInit() zeroes the array,
                 * entry calls repopulate inside fpcDw — nonzero at R-time
                 * means packets persist for the re-draw. */
                uint32_t pk[3]; const uint32_t baddr[3] = {0x803CA940u, 0x803CA944u, 0x803CA95Cu};
                for (int bi = 0; bi < 3; ++bi) {
                    uint32_t bp = rd32_fast(state, baddr[bi]);
                    pk[bi] = 0;
                    if (!in_ram(state, bp, 8u)) continue;
                    uint32_t buf = rd32_fast(state, bp);
                    uint32_t n = rd32_fast(state, bp + 4u);
                    if (n > 96u) n = 96u;
                    if (!in_ram(state, buf, n * 4u)) continue;
                    for (uint32_t ei = 0; ei < n; ++ei)
                        if (rd32_fast(state, buf + ei * 4u)) ++pk[bi];
                }
                fprintf(stderr, "[f60] jf=%llu jm=%llu prg=%llu ovrR=%llu w0d=%llu capd=%llu gpd=%llu cut=%llu camI=%llu lg=%llu wnum=%u pd=%u rd=%u rc=%u xm=%u xi=%d,%d,%d lst=%u,%u,%u,%u cam=%08X pk=%08X,%08X,%08X pb=%u fb=%u fol=%llu jpa=%llu\n",
                    (unsigned long long)s_dbg_jfast,
                    (unsigned long long)s_dbg_jmiss,
                    (unsigned long long)s_dbg_purged,
                    (unsigned long long)s_dbg_ovr_dup,
                    (unsigned long long)s_dbg_wnum_dup,
                    (unsigned long long)s_dbg_cap_dup,
                    (unsigned long long)s_dbg_gp_dup,
                    (unsigned long long)s_dbg_camcut,
                    (unsigned long long)s_dbg_caminj,
                    (unsigned long long)s_dbg_lightgate,
                    (unsigned)rd8_fast(state, GAMEINFO_WNUM),
                    (unsigned)s_pd_bytes, (unsigned)s_rd_bytes, (unsigned)s_render_conf,
                    (unsigned)rd32_fast(state, s_jfw_display + 0x1Cu),
                    (int)(int16_t)rd16_fast(state, rd32_fast(state, JUTXFB_MANAGER) + 0x14u),
                    (int)(int16_t)rd16_fast(state, rd32_fast(state, JUTXFB_MANAGER) + 0x16u),
                    (int)(int16_t)rd16_fast(state, rd32_fast(state, JUTXFB_MANAGER) + 0x18u),
                    (unsigned)(rd32_fast(state, 0x803CA970u) - 0x803CA960u),
                    (unsigned)(rd32_fast(state, 0x803CA9B8u) - 0x803CA978u),
                    (unsigned)(rd32_fast(state, 0x803CAAC0u) - 0x803CA9C0u),
                    (unsigned)(rd32_fast(state, 0x803CAB48u) - 0x803CAAC8u),
                    (unsigned)rd32_fast(state, 0x803CAB58u),
                    (unsigned)pk[0], (unsigned)pk[1], (unsigned)pk[2],
                    (unsigned)s_painter_bytes, (unsigned)s_fcdw_bytes,
                    (unsigned long long)s_dbg_folinj,
                    (unsigned long long)s_dbg_jpainj);
            }
        }
        /* The probe walks the fifo backward one word per external read —
         * tens of thousands of calls per frame. Only run it when the fifo
         * path is actually armed; debug of the render path doesn't need it. */
        if (s_fifo_replay || s_fifo_verify)
            fifo_probe(state);
    }
}

/* beginRender (0x802558CC) is intentionally NOT hooked: the accumulator lives
 * in on_painter_skip, which fires on every frame-loop iteration. Hooking
 * beginRender too would double-accumulate on L-frames (Painter calls it
 * normally) and its entry hook does not fire at all on the R-frame
 * redirect->passthrough->JIT path, so it cannot own the accumulator. */

/* waitForTick is also called by waitBlanking (0x80255CE4-0x80255D34) for
 * timed fades; only retime the frame-gate call inside beginRender
 * (0x802558CC-0x80255AB8). The caller is identified by the return address. */
#define JFW_BEGINRENDER_START 0x802558CCu
#define JFW_BEGINRENDER_END   0x80255AB8u

/* VI phase lock (MODERNGEKKO_F60_VI_LOCK, default on). Tick-exact pacing puts
 * the pair on a 1_350_000-tick period while NTSC VI retraces every ~675_675
 * ticks, so each copy's phase against the retrace grid drifts ~1_351 ticks
 * per pair. Whenever the L-slot is work-bound past one field (sailing:
 * ~760K), the L-image is the newest XFB for less than a field, and while the
 * drift parks that window between two retraces no retrace scans it out.
 * exchangeXfb's else-branch then discards the R-image, too. Measured
 * (VILOG, playsea): 6-10 s bursts of every-other-pair drops about every
 * 40 s, i.e. seconds of 30 fps judder.
 * Fix: end every frame-gate wait a fixed offset after a retrace. Each image
 * then spans a retrace as long as L+R work fits in two fields. Logic cadence
 * is untouched: decide() still accumulates real tick deltas against
 * TICK_30FPS, so logic stays at 30.000 Hz. The ~0.1% VI/logic mismatch then
 * surfaces as one L,L pair every ~16.6 s. Retail shows the same slip as one
 * 3-field frame every ~16.7 s. */
#define JFW_NEXTTICK       0x803F73A0u  /* waitForTick's static s64 nextTick$2569 */
#define JUTVIDEO_LASTTICK  0x803F78DCu  /* JUTVideo::sVideoLastTick (TBL at retrace) */
#define JUTVIDEO_INTERVAL  0x803F78E0u  /* JUTVideo::sVideoInterval (ticks/field)   */
#define VI_FIELD_NTSC      675675u      /* 40.5 MHz / 59.94 Hz (test harness grid)  */

/* Pad that makes the NEXT wait exit land field/8 (~2 ms) after a retrace —
 * late enough that preRetraceProc has run, early enough to leave the GP most
 * of the field to finish the copy before the next pick. waitForTick sets
 * nextTick = exit + pad, so the target is the first grid point at least one
 * offset past this wait's exit. Returns 0 when there is no usable retrace
 * grid (VI blacked out/stalled, non-60 Hz) — caller keeps its pad. */
static uint32_t vi_lock_pad(CPUState* state)
{
    /* sVideoInterval is 0 until two retraces have run, and a non-60 Hz mode
     * has no business on this grid: no lock, keep the tick pad. */
    const uint32_t field = rd32_fast(state, JUTVIDEO_INTERVAL);
    if (field < 600000u || field > 760000u)
        return 0u;
    const uint64_t next_prev =
        ((uint64_t)rd32_fast(state, JFW_NEXTTICK) << 32) |
        (uint64_t)rd32_fast(state, JFW_NEXTTICK + 4u);
    const uint64_t now = state->timebase;
    const uint64_t exit_tb = next_prev > now ? next_prev : now;
    /* TBL domain: sVideoLastTick is OSGetTick() = low word of the timebase. */
    const uint32_t since = (uint32_t)exit_tb - rd32_fast(state, JUTVIDEO_LASTTICK);
    if (since > 4u * field)
        return 0u;
    const uint64_t offset = field / 8u;
    uint64_t target = exit_tb - since + offset;
    while (target < exit_tb + offset)
        target += field;
    return (uint32_t)(target - exit_tb);
}

static void on_wait_for_tick(CPUState* state)
{
    if (s_debug) {
        static uint32_t wft_n = 0;
        if (++wft_n <= 40 || (wft_n & 0x3FFu) == 0)
            fprintf(stderr, "[f60-wft] n=%u en=%u split=%u lr=0x%08X r3=%u r4=%u rhz=%u\n",
                    wft_n, s_enabled, s_split_mode, state->lr,
                    state->gpr[3], state->gpr[4], s_render_hz);
    }
    if (!s_enabled || !s_split_mode)
        return;
    if (state->lr < JFW_BEGINRENDER_START || state->lr >= JFW_BEGINRENDER_END)
        return;
    if (s_render_hz != 0u) {
        uint32_t pad = (uint32_t)(40500000u / s_render_hz);
        if (pad == 0u)
            pad = 1u;
        /* Pair-lock: decide() armed a shortened pad for this R-iteration so
         * its L+R pair sums to one 30Hz slot. Substitutes only the exact
         * 675K field pad — other render rates pace differently. */
        if (s_pair_pad != 0u && pad == (uint32_t)F60_FIELD_TICKS)
            pad = s_pair_pad;
        /* VI lock supersedes the pair-lock at the 60 Hz rate; it keeps the
         * tick-derived pad when there is no retrace grid to lock to. */
        s_vi_locked = 0u;
        if (s_vi_lock && s_render_hz == 60u) {
            const uint32_t locked = vi_lock_pad(state);
            if (locked) {
                pad = locked;
                s_vi_locked = 1u;
            }
        }
        state->gpr[3] = pad;
        state->gpr[4] = 0u;
        return;
    }
    /* Unlimited mode: a 1-tick wait makes render-only iterations nearly
     * free in guest time (~500 ticks) while each still costs ~50us of host
     * CPU in bookkeeping. In a host-bound scene the frame loop free-runs
     * thousands of iterations/s and starves the guest thread — a
     * self-reinforcing collapse (acc never reaches TICK_30FPS, logic
     * cadence collapses, VI crawls). Pad the wait so each iteration costs
     * at least TICK_30FPS/8 guest ticks: <=8 render iterations per 30Hz
     * logic slot (~240 iter/s at full speed). Iterations that already
     * consumed that much real work get no extra wait. Below ~full speed the
     * padding throttles bookkeeping-only iterations, restoring a retail-like
     * cadence instead of the spin. */
    {
        const uint64_t min_ticks = TICK_30FPS / 8ull;
        uint64_t pad = s_last_delta >= min_ticks ? 1ull
                                                 : (min_ticks - s_last_delta);
        state->gpr[3] = (uint32_t)pad;
        state->gpr[4] = 0u;
    }
}

/* H2: fpcM_Execute 0x8003E370 — per-process update gate. Needs R-0
 * (entry hook persists) so `pc = lr` actually skips the update. */
static void on_execute_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* H3: fapGm_After 0x800231BC — scene/overlay/camera manager gate.
 * Same R-frame skip; keeps managers at retail 30 Hz cadence. */
static void on_after_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* C2: mDoAud_Execute 0x80007224 — JAI control pump gate (av-sync).
 * Same R-frame skip; prevents double-pump of sequence/stream retarget
 * when MODERNGEKKO_FRAME60_ACCUM=1. Latent under default passthrough. */
static void on_aud_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* C3: mDoCPd_Read 0x800078C0 — pad sample gate (input polling).
 * Same R-frame skip; prevents 60Hz PADRead while logic runs 30Hz
 * (would double-poll and miss triggers). Latent under default passthrough. */
static void on_cpad_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

static void on_void_logic_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* F5: gInf heap cadence fix. free() at Painter swaps mCurrentHeap and
 * freeAll()s the newly-current heap every iteration, but with fpcDw gated
 * on R-frames each packet list is consumed TWICE (R_k + L_{k+1} Painter),
 * so the L-frame's freeAll destroys the list it is about to replay ->
 * black alternating frames. Fix: pin mCurrentHeap to s_list_heap (the heap
 * the last fpcDw built into) at every Painter entry, so free() always
 * swaps into the OTHER heap and recycles only dead data; the live list is
 * never freed early. s_list_heap flips once per L-frame (the frame's
 * free()+fpcDw fills the other heap). Under irregular cadence the pin is
 * a retail-identical no-op and allocation occupancy stays
 * one-iteration-per-heap. mCurrentHeap = r13-0x7838 = 0x803F68A8.
 * GINF_MCURRHEAP and s_list_heap are declared near the top of the file —
 * decide() seeds/mirrors the parity on split edges (H1). */

/* F-2: dKy_setLight 0x80194BDC — render-mode R-frame relight.
 * Painter calls it once per iteration (m_Do_graphic.cpp:1651) immediately
 * after j3dSys.setViewMtx(camera->mViewMtx) (:1650), so on R-frames the body
 * sees the lerped view camview_install just wrote into the camera — the
 * same matrix the R-frame's re-drawn geometry uses. The pre-fix gate skipped
 * the body entirely because it is NOT idempotent (two cM_rndF draws at
 * d_kankyo.cpp:2514/2552 share the cM_rnd stream with actor AI; cLib_addCalc
 * advances lightStatusPt positions and the function-static flicker targets).
 * But the tail (d_kankyo.cpp:2563-2595) also uploads view-space light
 * pos/dir for every masked light (GXInitLightPos(viewMtx*mPos),
 * GXInitLightDir(invView*dir), GXLoadLightObjImm), so skipping left all
 * lights baked under the L-frame view while geometry used the lerped one —
 * and dPa_control_c::draw's dKy_setLight_again (d_kankyo.cpp:2598-2625)
 * re-uploaded ONLY light 0 under the lerped view, splitting light 0 from
 * lights 1-7 within one frame.
 *
 * Fix: let the body run on R-frames and snapshot/restore its complete
 * guest-RAM write set — all uploads land under the lerped view with zero
 * net logic-state change. Write set proven by enumerating every non-stack
 * store in the compiled body (build/GZLE01/asm/d/d_kankyo.s,
 * 0x80194BDC-0x80195148) plus every callee's writes:
 *   via lightStatusPt (r3 = lwz lightStatusPt@sda21; always ==
 *     lightStatusData 0x803E5750 — single assignment d_kankyo.cpp:2135):
 *     [0]+0x00..0x0B mPos (cLib_addCalc x3, :2488-2490), [0]+0x0C..0x17
 *     mPos2 (:2482), [0]+0x18 mColor.r (:2519); [1]+0xE8..0xF3 mPos (:2532),
 *     [1]+0x100..0x102 mColor.rgb (:2557-2559). Snapshot covers
 *     [0]+0x00..0x1B and [1]+0xE8..0x103 (0x1C each).
 *   via g_env_light (r31 = 0x803E4AB4): +0xA90 mLightDir (PSMTXMultVec out
 *     param at :2576, i==0 only), +0xA9C mSunPos2 (:2486), +0xAA8
 *     mPLightNearPlayer (:2520) — contiguous 0x24 bytes at 0x803E5544.
 *   sdata/sbss: u16 lightMask @0x803F62F0 (:2528/:2531); flicker statics
 *     target$6206 @0x803F6FB0 + init$6207 @0x803F6FB4 (+pad) and
 *     target$6225 @0x803F6FB8 + init$6226 @0x803F6FBC (+pad) — one 16B
 *     block at 0x803F6FB0.
 *   cM_rnd state r0/r1/r2 @0x803F7338..0x803F7343 (c_math.cpp:167-181,
 *   stores at c_math.s 802462EC/80246308/80246324) — the two cM_rndF draws
 *   are repaid, keeping the stream in 30Hz parity for actor AI.
 *   Callees with no other guest writes (verified in decomp source):
 *   dKy_light_influence_id / dKy_eflight_influence_{id,pos,power,distance,
 *   yuragi} (d_kankyo.cpp:163-293 — locals + reads only), cLib_addCalc /
 *   cLib_addCalc2 (c_lib.cpp:22-66 — *pValue only), dKyr_get_vectle_calc
 *   (d_kankyo_rain.cpp:38-51 — *o_out only), mDoMtx_inverseTranspose
 *   (m_Do_mtx.cpp:238-271 — *b only), PSMTXMultVec/PSVECSquareDistance/fmod
 *   (out-param/pure), GXInitLight* (GXLight.c:10-129 — stack GXLightObj),
 *   _savegpr_25/_restgpr_25 (frame). GXLoadLightObjImm writes WGPIPE — the
 *   wanted upload — and gx->bpSentNot=1 (GXLight.c:174), already 1 this
 *   pass: GXSetProjection set it at :1648 (GXTransform.c:101) before the
 *   call site — not part of the differential set.
 * Kill switch: MODERNGEKKO_F60_LIGHT_FIX=0 restores the pre-fix full skip. */
#define DKY_LST_PT      0x803F62F4u  /* lightStatusPt (.sdata ptr -> lightStatusData) */
#define DKY_LIGHTMASK   0x803F62F0u  /* u16 lightMask (.sdata)                        */
#define DKY_ENV_TAIL    0x803E5544u  /* g_env_light+0xA90: mLightDir/mSunPos2/mPLightNearPlayer */
#define DKY_FLICKER_BLK 0x803F6FB0u  /* target$6206/init$6207 .. target$6225/init$6226, 16B    */
#define DKY_RND_STATE   0x803F7338u  /* cM_rnd r0/r1/r2 (.sbss), 12B                           */
#define DKY_STTS_WORDS  7u           /* 0x1C bytes: [0]+0x00..0x1B or [1]+0xE8..0x103          */
#define DKY_STTS1_OFF   0xE8u        /* lightStatusPt[1] base                                  */
#define DKY_PT_SPAN     0x104u       /* highest byte we touch via lightStatusPt + 1            */

static int      s_light_snap = 0;   /* snapshot live — return hook must restore   */
static uint32_t s_light_pt   = 0;   /* resolved lightStatusPt at entry          */
static uint32_t s_light_stts0[DKY_STTS_WORDS];
static uint32_t s_light_stts1[DKY_STTS_WORDS];
static uint32_t s_light_env[9];
static uint32_t s_light_statics[4];
static uint32_t s_light_rnd[3];
static uint32_t s_light_mask = 0;
static uint64_t s_dbg_lightfix = 0;  /* R-frame relights applied                  */
static uint64_t s_dbg_lightbad = 0;  /* fallbacks to full-skip (insane lst ptr)   */

static void on_dky_setlight_gate(CPUState* state)
{
    if (!(s_enabled && s_split_mode && !s_logic_this_frame && s_rframe_render))
        return;
    ++s_dbg_lightgate;
    if (!s_light_fix) {
        state->pc = state->lr;   /* pre-fix behaviour: skip the whole body */
        return;
    }
    const uint32_t pt = rd32_fast(state, DKY_LST_PT);
    if (!in_ram(state, pt, DKY_PT_SPAN)) {
        /* Insane lightStatusPt — keep the safe skip rather than let the body
         * store through a bad pointer. Never observed; defensive only. */
        ++s_dbg_lightbad;
        state->pc = state->lr;
        return;
    }
    uint32_t i;
    s_light_pt = pt;
    for (i = 0; i < DKY_STTS_WORDS; ++i) {
        s_light_stts0[i] = rd32_fast(state, pt + i * 4u);
        s_light_stts1[i] = rd32_fast(state, pt + DKY_STTS1_OFF + i * 4u);
    }
    for (i = 0; i < 9u; ++i)
        s_light_env[i] = rd32_fast(state, DKY_ENV_TAIL + i * 4u);
    for (i = 0; i < 4u; ++i)
        s_light_statics[i] = rd32_fast(state, DKY_FLICKER_BLK + i * 4u);
    for (i = 0; i < 3u; ++i)
        s_light_rnd[i] = rd32_fast(state, DKY_RND_STATE + i * 4u);
    s_light_mask = rd16_fast(state, DKY_LIGHTMASK);
    s_light_snap = 1;
    ++s_dbg_lightfix;
    if (s_verify)
        fprintf(stderr, "[f60-light] R pre pt=%08X mask=%04X rng=%08X,%08X,%08X tgt=%08X,%08X pos0=%08X,%08X,%08X\n",
                (unsigned)pt, (unsigned)s_light_mask,
                (unsigned)s_light_rnd[0], (unsigned)s_light_rnd[1], (unsigned)s_light_rnd[2],
                (unsigned)s_light_statics[0], (unsigned)s_light_statics[2],
                (unsigned)s_light_stts0[0], (unsigned)s_light_stts0[1], (unsigned)s_light_stts0[2]);
    /* fall through — the body runs and re-uploads every masked light under
     * the lerped j3dSys view; the return hook rewinds the write set. */
}

static void on_dky_setlight_return(CPUState* state)
{
    if (!s_light_snap)
        return;
    s_light_snap = 0;
    const uint32_t pt = s_light_pt;
    if (s_verify) {
        /* Post-run, pre-restore reads — evidence the body really ran (RNG
         * drawn, mask recomputed, positions chased) before the rewind. */
        fprintf(stderr, "[f60-light] R ran mask %04X->%04X rng0 %08X->%08X pos0.x %08X->%08X ldir.x %08X->%08X\n",
                (unsigned)s_light_mask, (unsigned)rd16_fast(state, DKY_LIGHTMASK),
                (unsigned)s_light_rnd[0], (unsigned)rd32_fast(state, DKY_RND_STATE),
                (unsigned)s_light_stts0[0],
                in_ram(state, pt, 4u) ? (unsigned)rd32_fast(state, pt) : 0u,
                (unsigned)s_light_env[0], (unsigned)rd32_fast(state, DKY_ENV_TAIL));
    }
    uint32_t i;
    if (in_ram(state, pt, DKY_PT_SPAN)) {
        for (i = 0; i < DKY_STTS_WORDS; ++i) {
            wr32_fast(state, pt + i * 4u, s_light_stts0[i]);
            wr32_fast(state, pt + DKY_STTS1_OFF + i * 4u, s_light_stts1[i]);
        }
    }
    for (i = 0; i < 9u; ++i)
        wr32_fast(state, DKY_ENV_TAIL + i * 4u, s_light_env[i]);
    for (i = 0; i < 4u; ++i)
        wr32_fast(state, DKY_FLICKER_BLK + i * 4u, s_light_statics[i]);
    for (i = 0; i < 3u; ++i)
        wr32_fast(state, DKY_RND_STATE + i * 4u, s_light_rnd[i]);
    wr16_fast(state, DKY_LIGHTMASK, s_light_mask);
    if (s_verify) {
        /* Post-restore audit: re-read every restored region and count
         * mismatches against the snapshot — diff=0 proves net-zero. */
        uint32_t diff = 0;
        if (in_ram(state, pt, DKY_PT_SPAN)) {
            for (i = 0; i < DKY_STTS_WORDS; ++i) {
                diff += (rd32_fast(state, pt + i * 4u) != s_light_stts0[i]);
                diff += (rd32_fast(state, pt + DKY_STTS1_OFF + i * 4u) != s_light_stts1[i]);
            }
        }
        for (i = 0; i < 9u; ++i)
            diff += (rd32_fast(state, DKY_ENV_TAIL + i * 4u) != s_light_env[i]);
        for (i = 0; i < 4u; ++i)
            diff += (rd32_fast(state, DKY_FLICKER_BLK + i * 4u) != s_light_statics[i]);
        for (i = 0; i < 3u; ++i)
            diff += (rd32_fast(state, DKY_RND_STATE + i * 4u) != s_light_rnd[i]);
        diff += (rd16_fast(state, DKY_LIGHTMASK) != s_light_mask);
        fprintf(stderr, "[f60-light] R restored diff=%u (0 = net-zero) fix=%llu bad=%llu\n",
                (unsigned)diff,
                (unsigned long long)s_dbg_lightfix,
                (unsigned long long)s_dbg_lightbad);
    }
}

static void on_true_logic_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        moderngekko_mod_return_u32(state, 1u);
}

/* ========== Render-path split gates (fade/wipe steppers) ==========
 * These run inside the mDoGph_Painter / endGX path every iteration and mix
 * a frame-timed state advance with mandatory overlay drawing. On render-only
 * frames the overlay must still draw while the advance must not run.
 *
 * F1: JUTFader::control 0x802C85F0 ends with a draw() call (0x802C86F0) —
 *     except when mStatus == WaitIn, where control() returns without
 *     drawing at all (decomp JUTFader.cpp:21-46; draw() itself only checks
 *     mColor.a != 0). R-frames replay the L-frame's fader draw exactly:
 *     skipped when the L-frame's control() never reached draw(), else a
 *     draw() tail-call with the colour the L-frame used (saved/restored
 *     around the call so guest state is untouched). Two observed failure
 *     modes of the live-state version: (1) WaitIn can sit at mColor.a=0xFF
 *     (title flyover: status=WaitIn a=255 time=timer=26) so the old
 *     unconditional tail-call painted an opaque quad on every R-frame;
 *     (2) during the flyover's closing FadeOut, per-frame execute()
 *     (d_a_title/d_s_play setFadeColor(black,a=0xFF)) rewrites mColor
 *     between the L-frame's draw and the R-frame's, so a live-state draw
 *     painted a=0xFF black again — same visible strobe, opposite parity.
 *     Still zero state mutation (mStatus/mTimer/mDelayTimer untouched;
 *     mColor restored at the next control() entry).
 * F2: mDoGph_gInf_c::calcFade 0x80007FE8 mutates statics then draws the fade
 *     quad inline; snapshot the mutated fields on entry and restore them on
 *     return (guest-memory writes persist past the observer-only return-hook
 *     state restore). The overlay redraws the frozen logic-frame value.
 *     Additionally calcFade mutates mFadeRate/mFadeColor.a BEFORE emitting
 *     the fade quad, so an R-frame would emit alpha one increment ahead of
 *     the L-frame. Fix: at entry, pre-decrement mFadeRate by mFadeSpeed so
 *     the function re-derives exactly the L-frame's rate — the emitted quad
 *     replays the L-frame's alpha and the return-restore keeps state clean.
 * F3: dDlst_list_c::calcWipe 0x800866F0 — same pattern; the wipe packet is
 *     enqueued by pointer (dComIfGd_set2DXlu), so restoring the statics also
 *     restores what the packet renders later this frame. The enqueue itself
 *     is NOT idempotent: dComIfGd_set2DXlu writes *mp2DXlu and advances the
 *     cursor, and mp2DXlu is only rewound by dComIfGd_reset — which lives
 *     inside the fpcDw_Handler gate (L-frames only). On an R-frame the
 *     L-frame's appended &mWipeDlst is still in the list; calcWipe appends
 *     the SAME packet a second time and draw2DXlu draws it twice — the
 *     overlay blends at 2x alpha on every R-frame vs 1x on L-frames (30 Hz
 *     blink during wipes). Fix: restore mp2DXlu in the return hook. That
 *     drops only the R-frame's duplicate append; the L-frame's entry stays
 *     and draws with the restored L-frame scroll values — identical output.
 * F4: mDoGph_gInf_c::calcMonotone 0x80008354 — pure state chase
 *     (cLib_chaseS + idempotent offMonotone); plain pc=lr gate. */
#define JUT_FADER_DRAW     0x802C86F0u
#define GINF_MFADE         0x803F68ABu  /* u8   */
#define GINF_MFADERATE     0x803F68ACu  /* f32  */
#define GINF_MFADESPEED    0x803F68B0u  /* f32  */
#define GINF_MFADECOLOR    0x803F6104u  /* GXColor rgba */
#define DDLST_MWIPE        0x803F6C54u  /* u8   */
#define DDLST_MWIPERATE    0x803F6C58u  /* f32  */
#define DDLST_MWIPESCROLLS 0x803E232Cu  /* f32 — mWipeDlst.mScrollS (+0x34) */
#define DDLST_MWIPESCROLLT 0x803E2330u  /* f32 — mWipeDlst.mScrollT (+0x38) */
/* g_dComIfG_gameInfo (0x803C4C08) + drawlist (0x5D1C) + mp2DXlu (0x224).
 * mp2DXlu is the write cursor for the 2D-XLU packet list; draw2DXlu
 * traverses [mp2DXluArr, mp2DXlu). */
#define DDLST_MP2DXLU      0x803CAB48u  /* dDlst_base_c** */
/* drawlist + 0x1A4 = mp2DXluArr[0] — base of the 2D-XLU packet array. */
#define DDLST_MP2DXLUARR   0x803CAAC8u  /* dDlst_base_c*[32] */
#define DDLST_MWIPEDLST    0x803E22F8u  /* &dDlst_list_c::mWipeDlst */

static int s_fade_snap;
static uint64_t s_fade_flag, s_fade_rate, s_fade_color;
static int s_wipe_snap;
static uint64_t s_wipe_flag, s_wipe_rate, s_wipe_scrolls, s_wipe_scrollt;
static uint32_t s_wipe_xlu;         /* mp2DXlu value at calcWipe entry */

/* Count &mWipeDlst occurrences in the live 2D-XLU list [mp2DXluArr,cursor).
 * Used by the F60_VERIFY probe to prove the R-frame duplicate append is
 * rewound before draw2DXlu sees it. */
static int xlu_wipe_count(CPUState* state, uint32_t cursor)
{
    int n = 0;
    uint32_t p;
    for (p = DDLST_MP2DXLUARR; p < cursor && p < DDLST_MP2DXLUARR + 32u * 4u; p += 4u)
        if (rd32_fast(state, p) == DDLST_MWIPEDLST)
            ++n;
    return n;
}

static uint64_t s_dbg_fader_calls = 0;
static uint32_t s_fdl_hold = 0;       /* fadelog stays hot 200 calls after the last trigger */
static uint32_t s_fdl_self = 0;       /* this-ptr captured at control() entry for the ret log */
/* F1 replay state: what the preceding L-frame's control() actually did. */
static uint32_t s_fdr_l_drew = 0;     /* L-frame's control() reached draw() this frame    */
static uint32_t s_fdr_l_color = 0;    /* mColor word (+0x0C) at the L-frame's draw()      */
static uint32_t s_fdr_r_pending = 0;  /* R-frame overrode mColor — restore on next entry  */
static uint32_t s_fdr_r_saved = 0;    /* mColor word before the R-frame override          */
static uint32_t s_fdr_self = 0;       /* fader this-ptr while an R-frame override is out  */

/* JUTFader::draw entry — fires for control()'s internal tail call on L-frames
 * (records the colour it really painted) and for our R-frame pc redirect
 * (s_logic_this_frame==0 there, so the redirect is never recorded). */
static void on_fader_draw_seen(CPUState* state)
{
    if (!s_split_mode || s_logic_this_frame) {
        const uint32_t self = (uint32_t)state->gpr[3];
        if (in_ram(state, self, 0x10u)) {
            s_fdr_l_drew = 1;
            s_fdr_l_color = rd32_fast(state, self + 0x0Cu);
        }
    }
}

static void on_fader_gate(CPUState* state)
{
    const uint32_t self = (uint32_t)state->gpr[3];
    const int ok = in_ram(state, self, 0x28u);
    /* Undo the R-frame's colour override before anything — control() itself
     * or the fadelog probe — reads mColor. No guest logic runs between the
     * R-frame's draw() and the next control() entry. */
    if (s_fdr_r_pending) {
        s_fdr_r_pending = 0;
        /* Restore through the fader the override was written to — a
         * setFader() swap between the two calls must not stamp the old
         * fader's colour onto the new one. */
        if (in_ram(state, s_fdr_self, 0x10u))
            wr32_fast(state, s_fdr_self + 0x0Cu, s_fdr_r_saved);
    }
    const uint32_t status = ok ? rd32_fast(state, self + 0x04u) : 0xFFFFFFFFu;
    /* FADELOG: entry sample fires on both classes — on L-frames this is
     * control() entry, before control mutates a/timer (ret hook logs post). */
    if (s_fadelog) {
        const int trig = (status == 2u || status == 3u);
        if (trig) s_fdl_hold = 200u;
        if (trig || s_fdl_hold) {
            if (!trig) --s_fdl_hold;
            fprintf(stderr, "[fadelog] fdr cls=%c status=%u a=%u time=%u timer=%u dly=%d drew=%u lc=%08X\n",
                    s_logic_this_frame ? 'L' : 'R', status,
                    ok ? rd8_fast(state, self + 0x0Fu) : 0u,
                    ok ? rd16_fast(state, self + 0x08u) : 0u,
                    ok ? rd16_fast(state, self + 0x0Au) : 0u,
                    ok ? (int)(int32_t)rd32_fast(state, self + 0x20u) : 0,
                    (unsigned)s_fdr_l_drew, (unsigned)s_fdr_l_color);
        }
        if (s_logic_this_frame)
            s_fdl_self = ok ? self : 0u;
    }
    /* L-frame control() entry: arm the drew flag; draw_seen flips it back on
     * iff control() actually reaches draw() (WaitIn returns early). */
    if (s_logic_this_frame)
        s_fdr_l_drew = 0;
    if (!(s_enabled && s_split_mode && !s_logic_this_frame))
        return;
    /* R-frame: replay exactly what the preceding L-frame drew — skip when it
     * didn't draw, otherwise paint with ITS colour. Live mColor is wrong here:
     * game execute() may have overwritten it after the L draw (observed:
     * d_a_title/d_s_play setFadeColor(black,a=0xFF) once per frame during the
     * flyover FadeOut — R-frames then drew an opaque quad). */
    const uint32_t early_out = (s_fader_fix && !s_fdr_l_drew) ? 1u : 0u;
    if (s_debug || s_verify) {
        ++s_dbg_fader_calls;
        if (s_dbg_fader_calls <= 20u || (s_dbg_fader_calls % 240u) == 0u) {
            const uint32_t a = ok ? rd8_fast(state, self + 0x0Fu) : 0u;
            const uint32_t ftime = ok ? rd16_fast(state, self + 0x08u) : 0u;
            const uint32_t timer = ok ? rd16_fast(state, self + 0x0Au) : 0u;
            fprintf(stderr, "[f60-fader] status=%u a=%u time=%u timer=%u drew=%u fix=%u\n",
                    status, a, ftime, timer, (unsigned)s_fdr_l_drew, early_out);
        }
    }
    if (early_out) {
        state->pc = state->lr;
        return;
    }
    if (s_fader_fix && ok) {
        s_fdr_r_saved = rd32_fast(state, self + 0x0Cu);
        wr32_fast(state, self + 0x0Cu, s_fdr_l_color);
        s_fdr_r_pending = 1;
        s_fdr_self = self;
    }
    state->pc = JUT_FADER_DRAW;
}

/* FADELOG return sample: post-control() state on L-frames (r3 is scratch at
 * blr, so reuse the this-ptr captured at entry). */
static void on_fader_return(CPUState* state)
{
    if (!(s_fadelog && s_logic_this_frame && s_fdl_self))
        return;
    const uint32_t self = s_fdl_self;
    fprintf(stderr, "[fadelog] ret cls=L status=%u a=%u time=%u timer=%u dly=%d\n",
            rd32_fast(state, self + 0x04u), rd8_fast(state, self + 0x0Fu),
            rd16_fast(state, self + 0x08u), rd16_fast(state, self + 0x0Au),
            (int)(int32_t)rd32_fast(state, self + 0x20u));
}

static void on_calcfade_entry(CPUState* state)
{
    if (s_fadelog) {
        const uint32_t mfade = rd8_fast(state, GINF_MFADE);
        const int trig = (mfade != 0u);
        if (trig) s_fdl_hold = 200u;
        if (trig || s_fdl_hold) {
            if (!trig) --s_fdl_hold;
            union { uint32_t u; float f; } rt, sp;
            rt.u = rd32_fast(state, GINF_MFADERATE);
            sp.u = rd32_fast(state, GINF_MFADESPEED);
            fprintf(stderr, "[fadelog] gph cls=%c mFade=%u rate=%.4f speed=%.4f color=%08X\n",
                    s_logic_this_frame ? 'L' : 'R', mfade, (double)rt.f,
                    (double)sp.f, (unsigned)rd32_fast(state, GINF_MFADECOLOR));
        }
    }
    if (!(s_enabled && s_split_mode && !s_logic_this_frame))
        return;
    s_fade_flag  = rd8_fast(state, GINF_MFADE);
    s_fade_rate  = rd32_fast(state, GINF_MFADERATE);
    s_fade_color = rd32_fast(state, GINF_MFADECOLOR);
    s_fade_snap  = 1;
    /* Replay semantics: calcFade mutates mFadeRate BEFORE drawing the fade
     * quad, so the R-frame would emit alpha one mFadeSpeed step ahead of the
     * L-frame. Pre-decrement so the body recomputes the L-frame's rate and
     * draws the identical quad; the return hook restores the saved state. */
    if (s_fade_flag) {
        union { uint32_t u; float f; } r, sp;
        r.u  = (uint32_t)s_fade_rate;
        sp.u = rd32_fast(state, GINF_MFADESPEED);
        r.f -= sp.f;
        wr32_fast(state, GINF_MFADERATE, r.u);
        if (s_verify)
            fprintf(stderr, "[f60-verify] fade rate=%.4f pre=%.4f speed=%.4f\n",
                    (double)r.f + (double)sp.f, (double)r.f, (double)sp.f);
    }
}

static void on_calcfade_return(CPUState* state)
{
    if (!s_fade_snap)
        return;
    s_fade_snap = 0;
    /* GINF_* are MEM1 statics (data, never code): the direct-RAM store is
     * the same write the external accessor ends in, minus the per-word
     * translate+MMU trip and the SMC journal (text-only by design). */
    wr8_fast(state, GINF_MFADE, (uint32_t)s_fade_flag);
    wr32_fast(state, GINF_MFADERATE, (uint32_t)s_fade_rate);
    wr32_fast(state, GINF_MFADECOLOR, (uint32_t)s_fade_color);
}

static void on_calcwipe_entry(CPUState* state)
{
    if (!(s_enabled && s_split_mode && !s_logic_this_frame))
        return;
    s_wipe_flag    = rd8_fast(state, DDLST_MWIPE);
    s_wipe_rate    = rd32_fast(state, DDLST_MWIPERATE);
    s_wipe_scrolls = rd32_fast(state, DDLST_MWIPESCROLLS);
    s_wipe_scrollt = rd32_fast(state, DDLST_MWIPESCROLLT);
    s_wipe_xlu     = rd32_fast(state, DDLST_MP2DXLU);
    s_wipe_snap    = 1;
}

static void on_calcwipe_return(CPUState* state)
{
    if (!s_wipe_snap)
        return;
    s_wipe_snap = 0;
    wr8_fast(state, DDLST_MWIPE, (uint32_t)s_wipe_flag);
    wr32_fast(state, DDLST_MWIPERATE, (uint32_t)s_wipe_rate);
    wr32_fast(state, DDLST_MWIPESCROLLS, (uint32_t)s_wipe_scrolls);
    wr32_fast(state, DDLST_MWIPESCROLLT, (uint32_t)s_wipe_scrollt);
    /* Repay the cursor NOW, before this pass's draw2DXlu: the R-frame's
     * append is a duplicate of the L-frame's &mWipeDlst entry still in the
     * list (fpcDw_Handler — and with it dComIfGd_reset — was skipped), so
     * rewinding leaves exactly the L-frame's list for draw2DXlu to draw.
     * Without this the wipe blends at 2x alpha on every R-frame (30 Hz
     * blink); restoring any later would still leave the duplicate visible
     * to this pass's own draw2DXlu. */
    if (s_verify) {
        /* Live proof the duplicate existed and was neutralized: count
         * &mWipeDlst entries before and after the rewind. */
        const uint32_t cur = rd32_fast(state, DDLST_MP2DXLU);
        const int pre = xlu_wipe_count(state, cur);
        wr32_fast(state, DDLST_MP2DXLU, s_wipe_xlu);
        const int post = xlu_wipe_count(state, s_wipe_xlu);
        fprintf(stderr, "[f60-verify] wipe xlu=%08X->%08X dup=%d->%d\n",
                cur, s_wipe_xlu, pre, post);
    } else {
        wr32_fast(state, DDLST_MP2DXLU, s_wipe_xlu);
    }
}

/* F-1: daSea_packet_c::draw 0x8015C75C does `mAnimCounter += 1; if (>300)
 * =0` (s16 at this+0x144, d_a_sea.cpp:715-721) on every draw, so an
 * R-frame re-render scrolls the indirect-warp texture at 2x speed.
 * Entry pre-decrements so the body's ++ reproduces the L-frame's value;
 * the return hook restores the original — which also covers the
 * ChkCullStop/alloc-NULL early returns where the ++ never ran. */
static int      s_sea_snap = 0;
static uint32_t s_sea_self = 0;
static uint32_t s_sea_orig = 0;

static void on_sea_draw_entry(CPUState* state)
{
    if (!(s_enabled && s_split_mode && s_sea_fix && !s_logic_this_frame && !s_r_dup))
        return;
    const uint32_t self = (uint32_t)state->gpr[3];
    if (!in_ram(state, self, 0x146u))
        return;
    s_sea_self = self;
    s_sea_orig = rd16_fast(state, self + 0x144u);
    wr16_fast(state, self + 0x144u, (s_sea_orig - 1u) & 0xFFFFu);
    s_sea_snap = 1;
}

static void on_sea_draw_return(CPUState* state)
{
    if (!s_sea_snap)
        return;
    s_sea_snap = 0;
    if (in_ram(state, s_sea_self, 0x146u))
        wr16_fast(state, s_sea_self + 0x144u, s_sea_orig);
}

/* ========== Present-only frames (Design A — duplicate present) ==========
 * The draw pipeline is one-frame deferred: fpcDw produces packet lists at
 * iteration N, Painter consumes them at N+1. On render-only frames we skip
 * the entire production side and re-present the last completed XFB —
 * preRetraceProc already re-issues VISetNextFrameBuffer every retrace while
 * sDrawWaiting is clear, so a duplicate present is a retail code path.
 *  D1: mDoGph_Painter entry 0x8000AF2C → tail-call beginRender with
 *      r3 = JFWDisplay singleton; beginRender returns to Painter's caller
 *      (fpcM_Management), skipping BeforeOfDraw, the draw body, and
 *      endRender while keeping pacing + the accumulator input.
 *      (The earlier mid-Painter site 0x8000AFB4 sits on a `bl` and
 *      livelocks the fallback-JIT yield/passthrough path — do not hook
 *      call instructions.)
 *  D2: fpcDw_Handler 0x800404CC → return 1; skips dComIfGd_reset, the
 *      process Draw iterate, and AfterOfDraw. Return value is ignored.
 *  D3: exchangeXfb_double 0x80255570 → skip; no EFB→XFB copy/clear, EFB
 *      retains the frame; next L-frame exchange ships it before repaint.
 *  D4: endGX 0x802557C0 → skip; no fader advance/double-blend, no GXFlush.
 *      (endGX is only reached through endRender; kept for defense-in-depth.)
 * F1-F3 still cover mDoGph_AfterOfDraw's fade/wipe stepping, which runs
 * outside Painter and is not caught by the D5-D11 draw-submission gates. */
/* D1: Painter-entry -> beginRender tail-call. On render-only frames, jump
 * straight to JFWDisplay::beginRender (r3 = the JFW singleton). beginRender
 * runs the retimed wait + tick update + XFB exchange + preGX, then returns
 * directly to Painter's caller (fpcM_Management). cAPIGph_Painter discards
 * Painter's bool return, so beginRender's void return is harmless. The D5-D11
 * leaf gates below are the fallback when the tail-call is disabled. */
#define JFW_BEGIN_RENDER 0x802558CCu

/* JUTXfb singleton (sManager__6JUTXfb) + field offsets for the F60_XFBHASH
 * probe: mBuffer[3] @0x00, mDrawnXfbIndex (s16) @0x16. */
#define JUTXFB_OFF_BUFFERS  0x00u
#define JUTXFB_OFF_DRAWNIDX 0x16u

/* [f60-cost] report — windowed per-frame averages + cumulative R-share.
 * Field map: wL/wR/wO = windowed mean wall ms per frame (n= frames this
 * window), wtbL/wtbR = windowed mean guest TB ticks per frame (~24.7 ns/tick,
 * TICK_30FPS = 1_350_000 per 30Hz slot), Rw/cRw = windowed/cumulative R share
 * of wall ns across L+R frames, cL/cR/cO = cumulative mean wall ms, and the
 * snap/refr/inj/frep fields are cumulative mean microseconds per call into
 * the mod's own helpers. */
static void frame60_cost_report(void)
{
    const double w_l_ms = s_wl_n ? (double)s_wl_ns / (double)s_wl_n / 1.0e6 : 0.0;
    const double w_r_ms = s_wr_n ? (double)s_wr_ns / (double)s_wr_n / 1.0e6 : 0.0;
    const double w_o_ms = s_wo_n ? (double)s_wo_ns / (double)s_wo_n / 1.0e6 : 0.0;
    const double w_l_tb = s_wl_n ? (double)s_wl_tb / (double)s_wl_n : 0.0;
    const double w_r_tb = s_wr_n ? (double)s_wr_tb / (double)s_wr_n : 0.0;
    const double w_rw = (s_wl_ns + s_wr_ns)
        ? 100.0 * (double)s_wr_ns / (double)(s_wl_ns + s_wr_ns) : 0.0;
    const double c_l_ms = s_cost_l_n ? (double)s_cost_l_ns / (double)s_cost_l_n / 1.0e6 : 0.0;
    const double c_r_ms = s_cost_r_n ? (double)s_cost_r_ns / (double)s_cost_r_n / 1.0e6 : 0.0;
    const double c_o_ms = s_cost_o_n ? (double)s_cost_o_ns / (double)s_cost_o_n / 1.0e6 : 0.0;
    const double c_rw = (s_cost_l_ns + s_cost_r_ns)
        ? 100.0 * (double)s_cost_r_ns / (double)(s_cost_l_ns + s_cost_r_ns) : 0.0;
    fprintf(stderr,
        "[f60-cost] wL=%u/%.3fms wR=%u/%.3fms wO=%u/%.3fms wtbL=%.0f wtbR=%.0f "
        "Rw=%.1f%% | cumL=%.3fms cumR=%.3fms cumO=%.3fms cRw=%.1f%% "
        "cumLN=%llu cumRN=%llu | "
        "snap=%.1fus(%llu) refr=%.1fus(%llu) inj=%.1fus(%llu) frep=%.1fus(%llu)\n",
        s_wl_n, w_l_ms, s_wr_n, w_r_ms, s_wo_n, w_o_ms, w_l_tb, w_r_tb,
        w_rw, c_l_ms, c_r_ms, c_o_ms, c_rw,
        (unsigned long long)s_cost_l_n, (unsigned long long)s_cost_r_n,
        s_c_snap_n ? (double)s_c_snap_ns / (double)s_c_snap_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_snap_n,
        s_c_refr_n ? (double)s_c_refr_ns / (double)s_c_refr_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_refr_n,
        s_c_inj_n ? (double)s_c_inj_ns / (double)s_c_inj_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_inj_n,
        s_c_frep_n ? (double)s_c_frep_ns / (double)s_c_frep_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_frep_n);
    s_wl_ns = s_wr_ns = s_wo_ns = 0;
    s_wl_tb = s_wr_tb = s_wo_tb = 0;
    s_wl_n = s_wr_n = s_wo_n = 0;
}

/* Headroom governor (MODERNGEKKO_F60_AUTO_DEGRADE, default on). An R-frame
 * re-render costs real host time: ~7 ms on top of ~27 ms of L work per
 * 33.3 ms pair in the heaviest scene measured. A host that cannot fit both
 * never reaches the throttle's sleep, so guest time falls behind wall time
 * and the whole game (logic, audio, VI) runs in slow motion at 50-56 fps
 * instead of full speed at 30. The governor compares guest seconds with
 * wall seconds per L->L pair and judges the MEDIAN pair of each ~2 s
 * window. R-frame cost is uniform (every pair runs long), while shader
 * compiles and disc reads are spikes: a few long pairs, then a throttle
 * catch-up run above 1.0x. Dropping R-frames cures only the former, and the
 * median ignores the latter. After GOV_SLOW_WINDOWS consecutive windows
 * below GOV_SLOW it steers R-frames to the duplicate-present path.
 * That is the same path the overlap guard uses; logic cadence is
 * untouched. Interpolation is retried after a hold that doubles on every
 * failed retry (15 s .. 4 min). The retry is skipped while even the dup
 * cadence is short of full speed, since interpolating can only be slower.
 * L->L gaps over GOV_GAP_MAX_NS (pause, window drag, disc or shader stall)
 * are dropped; they say nothing about sustained headroom. */
#define GOV_WINDOW        60u                /* L-frames per verdict (~2 s)       */
#define GOV_GAP_MAX_NS    150000000ull       /* longer L->L gap: not a sample     */
#define GOV_SLOW          0.95               /* speed below this: no headroom     */
#define GOV_DUP_OK        0.97               /* dup-mode speed needed to retry    */
#define GOV_SLOW_WINDOWS  2u                 /* consecutive slow verdicts to act  */
#define GOV_HOLD_MIN_NS   15000000000ull
#define GOV_HOLD_MAX_NS   240000000000ull
#define GOV_CLEAN_WINDOWS 30u                /* ~60 s at speed: hold back to min  */

typedef struct {
    uint64_t prev_tb, prev_ns;
    uint32_t prev_valid;
    float    win[GOV_WINDOW];   /* per-pair speed samples of this window */
    uint32_t win_n;
    uint32_t slow_windows, clean_windows;
    uint32_t degraded;          /* R-frames take the dup-present path            */
    uint32_t probing;           /* interpolating again after a hold: one verdict */
    uint64_t hold_ns;           /* current back-off                              */
    uint64_t retry_at_ns;       /* degraded: earliest retry                      */
    double   last_speed;        /* guest s / wall s of the last verdict          */
    uint64_t dup_frames;        /* R-frames the governor steered to dup          */
} F60Gov;

static F60Gov s_gov = { 0, 0, 0, { 0 }, 0, 0, 0, 0, 0, GOV_HOLD_MIN_NS, 0, 1.0, 0 };

/* One L-frame: tb = guest timebase, ns = host wall clock. */
static void gov_on_lframe(F60Gov* g, uint64_t tb, uint64_t ns)
{
    if (g->prev_valid && tb > g->prev_tb && ns > g->prev_ns) {
        const uint64_t dtb = tb - g->prev_tb;
        const uint64_t dns = ns - g->prev_ns;
        if (dtb <= 4ull * TICK_30FPS && dns <= GOV_GAP_MAX_NS && g->win_n < GOV_WINDOW)
            g->win[g->win_n++] =
                (float)((double)dtb * (1.0e9 / 40500000.0) / (double)dns);
    }
    g->prev_tb = tb;
    g->prev_ns = ns;
    g->prev_valid = 1u;
    if (g->win_n < GOV_WINDOW)
        return;
    /* median pair speed (insertion sort of 60 floats every ~2 s) */
    for (uint32_t i = 1; i < GOV_WINDOW; ++i) {
        const float v = g->win[i];
        uint32_t j = i;
        while (j > 0 && g->win[j - 1] > v) {
            g->win[j] = g->win[j - 1];
            --j;
        }
        g->win[j] = v;
    }
    const double speed = 0.5 * ((double)g->win[GOV_WINDOW / 2 - 1] +
                                (double)g->win[GOV_WINDOW / 2]);
    g->win_n = 0;
    g->last_speed = speed;
    if (g->degraded) {
        if (ns < g->retry_at_ns)
            return;
        if (speed < GOV_DUP_OK) {
            g->retry_at_ns = ns + g->hold_ns;
            return;
        }
        g->degraded = 0u;
        g->probing = 1u;
        fprintf(stderr, "[f60] headroom: retrying 60 fps (%.2fx speed at 30 fps)\n", speed);
        return;
    }
    if (speed < GOV_SLOW) {
        if (!g->probing && ++g->slow_windows < GOV_SLOW_WINDOWS)
            return;
        if (g->probing) {
            g->hold_ns *= 2u;
            if (g->hold_ns > GOV_HOLD_MAX_NS)
                g->hold_ns = GOV_HOLD_MAX_NS;
        }
        g->degraded = 1u;
        g->probing = 0u;
        g->slow_windows = 0u;
        g->clean_windows = 0u;
        g->retry_at_ns = ns + g->hold_ns;
        fprintf(stderr, "[f60] headroom: host at %.2fx speed - interpolated frames off "
                        "(30 fps at full speed), retry in %llu s\n",
                speed, (unsigned long long)(g->hold_ns / 1000000000ull));
        return;
    }
    g->slow_windows = 0u;
    if (g->probing) {
        g->probing = 0u;
        fprintf(stderr, "[f60] headroom: 60 fps restored (%.2fx speed)\n", speed);
    }
    if (++g->clean_windows >= GOV_CLEAN_WINDOWS) {
        g->clean_windows = GOV_CLEAN_WINDOWS;
        g->hold_ns = GOV_HOLD_MIN_NS;
    }
}

static void on_painter_skip(CPUState* state)
{
    /* F5: pin mCurrentHeap to the heap the last fpcDw built the live packet
     * lists into. Nothing between Painter entry and free() reads it, and
     * this makes free() always recycle the heap that does NOT hold the
     * lists Painter is about to replay. s_list_heap is seeded from the live
     * mCurrentHeap at split engagement and mirrored while unsplit (H1); in
     * non-split mode the write below is masked off entirely. */
    if (s_enabled && s_split_mode && s_heap_pin)
        wr8_fast(state, GINF_MCURRHEAP, s_list_heap);
    /* Fires on every Painter call = once per frame-loop iteration. Owns the
     * accumulator (frame60_accum_decide) and, on R-frames, either redirects
     * to beginRender (duplicate-present path) or — when MODERNGEKKO_RFRAME_RENDER
     * is set — injects lerped matrices and lets Painter re-draw the persistent
     * packet list (unique interpolated frame). */
    /* GP-emission sampling: delta of the PI fifo write pointer between this
     * entry and the previous one = display bytes emitted by the frame that
     * just finished. s_logic_this_frame still holds THAT frame's class
     * (decide() below hasn't run), so a delta collected at an R-entry covers
     * the L-frame's Painter body alone — the inline fpcDw draws of the next
     * L-frame land in the following span, which we deliberately ignore.
     * Pure diagnostic feeder (GP_GATE veto, STREAM_DUMP, F60_TRACE/DEBUG
     * prints): the PI regs are MMIO reads, so skip the whole block when no
     * consumer is armed — zero cost with diagnostics off. */
    if (s_gp_gate || s_stream_dump_dir[0] || s_debug || s_trace) {
        const uint32_t base = rd32_fast(state, PI_FIFO_BASE_REG);
        const uint32_t end  = rd32_fast(state, PI_FIFO_END_REG);
        const uint32_t wptr = rd32_fast(state, PI_FIFO_WPTR_REG);
        if (base < 0x01800000u && end <= 0x01800000u && end > base &&
            wptr >= base && wptr < end) {
            const uint32_t fsize = end - base;
            if (s_wptr_valid && s_logic_this_frame) {
                uint32_t d = wptr - s_wptr_prev;
                if (wptr < s_wptr_prev) d += fsize;
                if (d < fsize) {
                    s_pd_bytes = d;
                    if (d >= s_gp_min) {
                        if (s_render_conf < 4u) ++s_render_conf;
                    } else {
                        s_render_conf = 0;
                    }
                }
            }
            /* Emission split: this Painter entry closes the fpcDw span —
             * everything emitted since the last fpcDw entry was inline
             * draw work (fpcDw body + loop tail) that an R-frame re-render
             * can never reproduce. */
            if (s_split_valid) {
                uint32_t fd = wptr - s_fcdw_entry_wptr;
                if (wptr < s_fcdw_entry_wptr) fd += fsize;
                if (fd < fsize) s_fcdw_bytes = fd;
            }
            /* STREAM_DUMP: the region closing here = the full iteration that
             * just ran (s_logic_this_frame still holds ITS class — decide()
             * below hasn't overwritten it). One file per iteration. */
            if (s_stream_dump_dir[0] && s_stream_dump_n < s_stream_dump_max) {
                if (!s_stream_dump_buf)
                    s_stream_dump_buf = (uint8_t*)malloc(FIFO_STREAM_MAX);
                uint32_t prev = s_painter_entry_wptr;
                uint32_t d = wptr - prev;
                if (wptr < prev) d += fsize;
                if (s_stream_dump_buf && d > 0u && d < FIFO_STREAM_MAX) {
                    char path[600];
                    snprintf(path, sizeof(path), "%s/%06lu_%c.bin",
                             s_stream_dump_dir,
                             (unsigned long)s_stream_dump_n++,
                             s_logic_this_frame ? 'L' : 'R');
                    fifo_capture(state, base, end, prev, d, s_stream_dump_buf);
                    FILE* f = fopen(path, "wb");
                    if (f) {
                        fwrite(s_stream_dump_buf, 1u, d, f);
                        fclose(f);
                    }
                }
            }
            s_painter_entry_wptr = wptr;
            s_split_valid = 1;
            s_wptr_prev = wptr; s_wptr_valid = 1;
            s_wptr_fsize = fsize;   /* modulo base for s_rd_bytes (render gate) */
        } else {
            s_wptr_valid = 0;
        }
    }
    /* Frame-cost accounting (MODERNGEKKO_FRAME60_COST / _DEBUG). Entry-to-
     * entry delta = the cost of the frame that just ran; its class is still
     * live in s_split_mode/s_logic_this_frame because decide() below has not
     * yet overwritten them for the frame starting now. state->timebase lags
     * the in-flight segment by design (the run loop flushes charges at
     * segment end), which is exactly consistent across identical anchors. */
    if (s_cost) {
        const uint64_t now = host_now_ns();
        const uint64_t tb = state->timebase;
        if (s_cost_prev_valid) {
            const uint64_t dns = now - s_cost_prev_ns;
            const uint64_t dtb = tb - s_cost_prev_tb;
            if (s_split_mode && !s_logic_this_frame) {
                s_cost_r_ns += dns; s_cost_r_tb += dtb; ++s_cost_r_n;
                s_wr_ns += dns; s_wr_tb += dtb; ++s_wr_n;
            } else if (s_split_mode) {
                s_cost_l_ns += dns; s_cost_l_tb += dtb; ++s_cost_l_n;
                s_wl_ns += dns; s_wl_tb += dtb; ++s_wl_n;
            } else {
                s_cost_o_ns += dns; s_cost_o_tb += dtb; ++s_cost_o_n;
                s_wo_ns += dns; s_wo_tb += dtb; ++s_wo_n;
            }
            if (((s_wl_n + s_wr_n + s_wo_n) & 0x07u) == 0x00u)
                frame60_cost_report();
        }
        s_cost_prev_ns = now; s_cost_prev_tb = tb; s_cost_prev_valid = 1;
    }
    /* MODERNGEKKO_F60_XFBHASH=1: hash the last completed XFB (drawn index)
     * once per iteration. On a static scene L and R frames should hash
     * identically; an alternating hash proves R-frame content differs
     * (the flicker signature), and the cls label tells which class
     * produced the deviating frame. The label uses the PREVIOUS frame's
     * class — s_logic_this_frame still holds it because decide() below
     * has not yet overwritten it for the frame starting now, and the XFB
     * being hashed was painted during that previous iteration. */
    if (s_xfbhash && s_split_mode) {
        static uint32_t hseq = 0;
        uint32_t h = 0x9E3779B9u;
        int idx = -1;
        uint32_t buf = 0;
        const uint32_t mgr = rd32_fast(state, JUTXFB_MANAGER);
        if (in_ram(state, mgr, 0x20u)) {
            idx = (int)(int16_t)rd16_fast(state, mgr + JUTXFB_OFF_DRAWNIDX);
            if (idx >= 0 && idx < 3)
                buf = rd32_fast(state, mgr + JUTXFB_OFF_BUFFERS + (uint32_t)idx * 4u);
        }
        if (in_ram(state, buf, 0x80000u)) {
            const uint8_t* p = state->ram + (buf - 0x80000000u);
            uint32_t off;
            for (off = 0; off < 0x80000u; off += 991u)
                h = (h ^ (uint32_t)p[off]) * 0x01000193u;
        }
        fprintf(stderr, "[f60-xfb] %u cls=%c idx=%d buf=%08X h=%08X\n",
                ++hseq, s_logic_this_frame ? 'L' : 'R', idx, buf, h);
    }
    if (s_vilog)
        memcpy(s_vl_view_img, s_vl_view_rend, sizeof(s_vl_view_img));
    frame60_accum_decide(state, 0);
    s_r_dup = 0;
    if (s_auto_degrade && s_enabled && s_split_mode && s_logic_this_frame &&
        s_rframe_render && s_j3d_interp)
        gov_on_lframe(&s_gov, state->timebase, host_now_ns());
    if (s_rnglog && s_logic_this_frame && s_rng_lcount < 400u) {
        /* Determinism probe: the cM_rnd stream (r0/r1/r2 @0x803F7338) is
         * shared by lighting flicker and actor AI — the R-frame relight
         * must leave it identical to the pre-fix run. */
        fprintf(stderr, "[rng] L#%u %08X %08X %08X\n", (unsigned)s_rng_lcount,
                (unsigned)rd32_fast(state, DKY_RND_STATE + 0u),
                (unsigned)rd32_fast(state, DKY_RND_STATE + 4u),
                (unsigned)rd32_fast(state, DKY_RND_STATE + 8u));
        ++s_rng_lcount;
    }
    /* F5: on L-frames, this frame's free() swaps mCurrentHeap into the
     * other heap and fpcDw fills it — that heap becomes the live-list heap
     * for the next Painter-entry pin. Split-only: while unsplit decide()
     * mirrors the live mCurrentHeap each iteration instead (H1). */
    if (s_logic_this_frame && s_heap_pin && s_split_mode)
        s_list_heap ^= 1u;
    if (s_trace && s_split_mode) {
        /* Per-entry trace: frame class + xfb indices + which exchange branch
         * the PREVIOUS beginRender took (drawn changed => copy-branch). */
        const uint32_t mgr = rd32_fast(state, JUTXFB_MANAGER);
        if (in_ram(state, mgr, 0x20u)) {
            const int16_t drw = (int16_t)rd16_fast(state, mgr + 0x14u);
            const int16_t drn = (int16_t)rd16_fast(state, mgr + 0x16u);
            const int16_t dsp = (int16_t)rd16_fast(state, mgr + 0x18u);
            fprintf(stderr, "[ft] %c wptr=%08X xb=%d,%d,%d exch=%c rc=%u dw=%u mh=%u gw=%u vb=%08X,%08X xb0=%08X xb1=%08X\n",
                    s_logic_this_frame ? 'L' : 'R',
                    (unsigned)rd32_fast(state, PI_FIFO_WPTR_REG),
                    (int)drw, (int)drn, (int)dsp,
                    (s_trace_prev_drawn == -2 || drn == (int16_t)s_trace_prev_drawn) ? 'e' : 'C',
                    (unsigned)s_render_conf,
                    (unsigned)rd8_fast(state, 0x803F78E4u),
                    (unsigned)rd8_fast(state, 0x803F68A8u),
                    (unsigned)rd8_fast(state, 0x803F68C1u),
                    (unsigned)rd32_fast(state, 0x803F128Cu),
                    (unsigned)rd32_fast(state, 0x803F1290u),
                    (unsigned)rd32_fast(state, rd32_fast(state, JUTXFB_MANAGER) + 0x00u),
                    (unsigned)rd32_fast(state, rd32_fast(state, JUTXFB_MANAGER) + 0x04u));
            s_trace_prev_drawn = (int32_t)drn;
        }
    }
    if (!(s_enabled && s_split_mode))
        return;
    if (s_logic_this_frame) {
        /* EXPERIMENT MODERNGEKKO_F60_FORCE_ELSE: make L-frames take the
         * exchange else-branch (clearEfb, no copy) so ONLY L-renders are
         * ever copied/presented — isolates "R-render produces black" from
         * "copy grabs an empty EFB regardless of renderer". */
        if (s_force_else) {
            const uint32_t mgr = rd32_fast(state, JUTXFB_MANAGER);
            if (in_ram(state, mgr, 0x20u)) {
                const uint32_t drn = (uint32_t)(int32_t)(int16_t)rd16_fast(state, mgr + 0x16u);
                wr16_fast(state, mgr + 0x18u, (uint32_t)(drn ^ 1u));
            }
        }
        /* L-frame: merged restore+snapshot. Arrays still holding our lerp bits
         * get curr restored before Painter draws (production skipped them);
         * arrays production rewrote are observed directly as the new pose.
         * One guest read per array — no separate restore pass. */
        if (s_rframe_render && s_j3d_interp) {
            const uint64_t t0 = s_cost ? host_now_ns() : 0;
            j3d_snapshot_pose(state);
            if (s_cost) { s_c_snap_ns += host_now_ns() - t0; ++s_c_snap_n; }
            /* Camera view/proj: same restore+observe (the field may still
             * hold our R-frame lerp — camera_draw/view_setup are gated to
             * L-frame production, so an unchanged field restores cam_curr). */
            if (s_cam_interp)
                camview_lframe(state);
            /* JPA: restore any lerped particle positions before Painter
             * re-draws the persistent list (the list is consumed twice —
             * the R-frame's inject must not leak into this authoritative
             * frame), then snapshot live positions as the lerp's L endpoint.
             * Runs at Painter ENTRY, before any particle draw this frame. */
            jpa_lframe(state);
            s_fol_cut = 0;
        }
        if (s_vilog)
            vilog_note_eye(state);
        return;
    }
    /* Overlap in flight: take the SAFE R-frame — the duplicate-present
     * tail-call touches neither the persistent packet list nor the J3D
     * arrays while scene teardown may be freeing them (the reason the guard
     * exists). The 30Hz cadence is untouched (see frame60_accum_decide), so
     * fades/wipes/timers keep retail timing. FIFO replay also falls back:
     * re-patching a captured stream is unsafe mid-teardown. */
    if (s_overlap_active) {
        ++s_dbg_ovr_dup;
    } else if (s_wnum_gate && rd8_fast(state, GAMEINFO_WNUM) == 0u) {
        /* windowNum==0: Painter's whole 3D/deferred pipeline is off and the
         * scene draws inline inside fpcDw (title, opening/storybook movie,
         * logos, fades, dialog/door overlaps) — nothing persists for an
         * R-frame re-render; it would present a black frame. Dup-present. */
        ++s_dbg_wnum_dup;
    } else if ((int16_t)rd16_fast(state, GINF_MCAPTURESTEP) != 0) {
        /* Picto-box capture in flight: mCaptureStep advances once per
         * Painter call, so an R-frame re-render would double-step the
         * capture state machine. Same treatment as windowNum==0: dup. */
        ++s_dbg_cap_dup;
    } else if (s_auto_degrade && s_gov.degraded) {
        /* Host too slow for the re-render (headroom governor above): a
         * dup-present keeps the game at full speed at 30 fps. */
        ++s_gov.dup_frames;
    } else if (s_gp_gate && s_render_conf < 2u) {
        /* Secondary veto (opt-in via MODERNGEKKO_F60_GP_GATE): a windowed
         * scene whose last L-frame Painter still emitted ~nothing — e.g.
         * lists not yet populated during scene init. Safer to dup. */
        ++s_dbg_gp_dup;
    } else if (s_rframe_render && s_j3d_interp) {
        /* Refresh observes whatever production left between the L-frame's
         * snapshot and now: the L-frame's OWN production ran after its
         * Painter (execute/draw tail of fpcM_Management), so node/env AND
         * buf[1][viewNo] can all hold a pose newer than what was snapped.
         * The refresh is discriminating, not a re-snapshot — an array still
         * holding OUR lerp (scratch match) is skipped, one matching curr is
         * skipped, and only a genuinely new pose rotates in. That keeps the
         * pair (last-shown, newly-produced) so the inject lerps forward
         * instead of re-drawing an older midpoint over the produced pose. */
        s_fol_cut = 0;   /* set below iff this R-frame crosses a camera cut */
        if (s_noinject) {
            /* Repaint probe: Painter replays the persistent lists on the
             * L-endpoint state — no refresh/inject/install. If this presents
             * clean where inject flickers, the lerped matrices are the bug. */
        } else if (s_cost) {
            const uint64_t t0 = host_now_ns();
            j3d_rframe_refresh(state);
            const uint64_t t1 = host_now_ns();
            s_c_refr_ns += t1 - t0; ++s_c_refr_n;
            if (s_cam_interp)
                camview_rframe(state);          /* may set s_cam_cut */
            if (s_cam_cut) {
                /* Camera cut/teleport this step: a mid-view or mid-pose
                 * would smear across the jump — plain repaint: restore the
                 * camera field AND every injected pose array to their CURR
                 * endpoints (what production last wrote), skip the inject. */
                j3d_restore_injected(state);
                if (s_cam_injected) {
                    camview_restore(state, s_cam_view);
                    s_cam_injected = 0;
                }
                ++s_dbg_camcut;
                s_cam_cut = 0;
                s_fol_cut = 1;  /* foliage bake is view-relative: same jump */
            } else {
                j3d_rframe_inject(state, s_interp_alpha);
                if (s_cam_interp)
                    camview_install(state, s_interp_alpha);
            }
            s_c_inj_ns += host_now_ns() - t1; ++s_c_inj_n;
        } else {
            j3d_rframe_refresh(state);
            if (s_cam_interp)
                camview_rframe(state);
            if (s_cam_cut) {
                j3d_restore_injected(state);
                if (s_cam_injected) {
                    camview_restore(state, s_cam_view);
                    s_cam_injected = 0;
                }
                ++s_dbg_camcut;
                s_cam_cut = 0;
                s_fol_cut = 1;
            } else {
                j3d_rframe_inject(state, s_interp_alpha);
                if (s_cam_interp)
                    camview_install(state, s_interp_alpha);
            }
        }
        /* JPA particles are world-space — they lerp even across a camera cut
         * (the world did not jump, only the view did). Skipped entirely by the
         * noinject probe like every other inject. */
        if (s_jpa_fix && !s_noinject)
            jpa_rframe(state, s_interp_alpha);
        /* Present-path fix: at 60Hz the R-frame's beginRender always finds
         * drawn != displaying (the L-frame's XFB has not been consumed by VI
         * yet), so exchangeXfb_double takes its else-branch — clearEfb wipes
         * the EFB Painter is about to redraw and no XFB bookkeeping runs,
         * which is what produced the strict content/black alternation. Lie
         * to the manager: claim the drawn XFB is already displayed. The copy
         * branch then runs like a real frame — GXCopyDisp into XFB[drawing],
         * drawn=drawing, drawing^=1 — and the VI handler consumes it as
         * usual. Env kill-switch: MODERNGEKKO_F60_XFB_FIX=0. */
        if (s_xfb_fix) {
            const uint32_t mgr = rd32_fast(state, JUTXFB_MANAGER);
            if (in_ram(state, mgr, 0x20u)) {
                const uint32_t drn = (uint32_t)(int32_t)(int16_t)rd16_fast(state, mgr + 0x16u);
                const uint32_t dsp = (uint32_t)(int32_t)(int16_t)rd16_fast(state, mgr + 0x18u);
                if (drn != dsp)
                    wr16_fast(state, mgr + 0x18u, drn);
            }
        }
        if (s_debug)
            s_rd_wptr0 = rd32_fast(state, PI_FIFO_WPTR_REG);
        if (s_vilog)
            vilog_note_eye(state);
        return;   /* Painter runs: re-draws last packet list */
    } else if (s_fifo_replay) {
        /* Capture the last complete GP frame from the PI fifo and append a
         * (matrix-lerped) copy — the video thread re-renders a produced frame
         * without the emu thread re-running Painter's traversal. Falls back
         * to the dup redirect below if the fifo can't be parsed. */
        if (s_cost) {
            const uint64_t t0 = host_now_ns();
            fifo_replay_frame(state);
            s_c_frep_ns += host_now_ns() - t0; ++s_c_frep_n;
        } else {
            fifo_replay_frame(state);
        }
    }
    if (s_jfw_display && !s_painter_skip_off) {
        s_r_dup = 1;   /* arm BEFORE the tail-call: beginRender's inner calls
                        * (exchangeXfb/endGX/draw funnels) hit the gates while
                        * the flag is live, so this dup frame skips the EFB->XFB
                        * copy AND the else-branch clearEfb — a pure re-present
                        * of the last completed XFB, identical to dup mode. */
        state->gpr[3] = s_jfw_display;
        state->pc = JFW_BEGIN_RENDER;   /* return goes to Painter's caller via lr */
    }
}

/* D2 fpcDw_Handler — production stays gated on R-frames in BOTH modes: the
 * packet list persists (frameInit only runs inside fpcDw), so Painter can
 * re-draw it. */
static void on_fcdw_gate(CPUState* state)
{
    /* Emission split: this fpcDw entry closes the Painter span — everything
     * emitted since the last Painter entry is Painter's own replay output
     * (the persistent-list redraw an R-frame CAN reproduce). */
    if (s_split_valid) {
        const uint32_t w = rd32_fast(state, PI_FIFO_WPTR_REG);
        const uint32_t base = rd32_fast(state, PI_FIFO_BASE_REG);
        const uint32_t end = rd32_fast(state, PI_FIFO_END_REG);
        const uint32_t fsize = end - base;
        if (base < 0x01800000u && end <= 0x01800000u && end > base &&
            w >= base && w < end) {
            uint32_t d = w - s_painter_entry_wptr;
            if (w < s_painter_entry_wptr) d += fsize;
            if (d < fsize) s_painter_bytes = d;
        }
        s_fcdw_entry_wptr = w;
    }
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        moderngekko_mod_return_u32(state, 1u);
}

/* Draw-path gates: on R-frames that run Painter (render mode) these pass
 * through so the draw calls actually submit; on R-frames redirected to the
 * dup-present tail-call (dup mode, overlap, or windowNum==0) they stay gated
 * — most importantly exchangeXfb_double, whose else-branch would otherwise
 * clearEfb mid-dup and poison the next present. */
static void on_void_render_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame) {
        if (s_r_dup) {
            state->pc = state->lr;
        } else if (s_debug) {
            /* R-frame render path: last gate to fire inside Painter leaves
             * the running emission total in s_rd_bytes (debug-only). The
             * delta is modulo the fifo size: a wrapped wptr adds fsize; a
             * stale/reset pointer or unknown size clamps to 0 instead of
             * reporting ~4.29e9. */
            const uint32_t wptr = rd32_fast(state, PI_FIFO_WPTR_REG);
            uint32_t d = wptr - s_rd_wptr0;
            if (wptr < s_rd_wptr0) d += s_wptr_fsize;
            if (s_wptr_fsize == 0u || d >= s_wptr_fsize)
                d = 0u;
            s_rd_bytes = d;
        }
    }
}

static void on_true_render_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame && s_r_dup)
        moderngekko_mod_return_u32(state, 1u);
}

/* Trace-only probes for the EFB->XFB copy / clear timeline. GXCopyDisp is
 * the single funnel every XFB copy flows through (beginRender copy-branch);
 * clearEfb is the else-branch wipe. Logging caller+dest+class against the
 * frame dump identifies which iteration's render each presented XFB holds. */
/* XFB luminance sampler (diag): with XFBToTextureEnable=False the copy writes
 * YUYV to guest RAM at the dest addr. Sample the Y channel of the buffer the
 * PREVIOUS copy wrote — by the time this copy runs it has long been decoded.
 * YUYV black is 0x10 (~16); real content is far brighter. Self-contained
 * black/bright verdict per copy, no frame dumps needed. */
static uint32_t s_prev_xfb_dest = 0;
static uint32_t sample_xfb_y(CPUState* st, uint32_t ea)
{
    if (!(ea >= 0x80000000u && ea < 0x81800000u)) return 0xFFFFFFFFu;
    const uint32_t region = 640u * 480u * 2u;   /* YUYV 4:2:2 native */
    if (ea + region > 0x81800000u) return 0xFFFFFFFFu;
    const uint8_t* p = st->ram + (ea - 0x80000000u);
    uint64_t acc = 0; uint32_t cnt = 0;
    /* Y at even byte offsets within each 4-byte group; stride for speed. */
    for (uint32_t i = 0; i + 3 < region; i += 16u) { acc += p[i] + p[i + 2u]; cnt += 2u; }
    return cnt ? (uint32_t)(acc / cnt) : 0xFFFFFFFFu;
}
static void on_gxcopydisp(CPUState* state)
{
    if (s_xfb_lum_enabled && s_prev_xfb_dest) {
        uint32_t y = sample_xfb_y(state, s_prev_xfb_dest);
        fprintf(stderr, "[xl] %c prev_dest=%08X ymean=%u\n",
                s_logic_this_frame ? 'L' : 'R', (unsigned)s_prev_xfb_dest, y);
    }
    if (s_xfb_lum_enabled)
        s_prev_xfb_dest = (uint32_t)state->gpr[3];
    /* VILOG: the copy inside beginRender ships the image the PREVIOUS
     * iteration rendered — decide() already ran for this one, so its class
     * is s_prev_frame_class. Joined offline with [vir] to reconstruct which
     * image each retrace scanned out. */
    if (s_vilog) {
        const float* v = s_vl_view_img;
        fprintf(stderr, "[vcp] tb=%llu img=%c dest=%08X view=%.6g,%.6g,%.6g,%.6g,"
                "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g\n",
                (unsigned long long)state->timebase,
                !s_split_mode ? 'O' : (s_prev_frame_class ? 'L' : 'R'),
                (unsigned)state->gpr[3], (double)v[0], (double)v[1], (double)v[2],
                (double)v[3], (double)v[4], (double)v[5], (double)v[6], (double)v[7],
                (double)v[8], (double)v[9], (double)v[10], (double)v[11]);
    }
    if (s_trace)
        fprintf(stderr, "[cd] %c dest=%08X clr=%u lr=%08X\n",
                s_logic_this_frame ? 'L' : 'R',
                (unsigned)state->gpr[3], (unsigned)state->gpr[4], (unsigned)state->lr);
    /* DIAG MODERNGEKKO_F60_NOCOPYCLEAR: force the R-frame's XFB copy to skip its
     * post-copy EFB clear, so the EFB retains L_k's content through the re-walk.
     * If black frames turn bright, the re-walk wasn't refilling a cleared EFB;
     * if they stay black, the re-walk itself wipes/draws black. */
    if (s_nocopyclr && s_enabled && s_split_mode && !s_logic_this_frame)
        state->gpr[4] = 0;
}
static void on_clear_efb(CPUState* state)
{
    if (s_trace)
        fprintf(stderr, "[ce] %c lr=%08X\n",
                s_logic_this_frame ? 'L' : 'R', (unsigned)state->lr);
    /* VILOG: exchangeXfb else-branch — the image in the EFB is discarded
     * without ever reaching an XFB (a dropped frame). */
    if (s_vilog)
        fprintf(stderr, "[vce] tb=%llu img=%c\n",
                (unsigned long long)state->timebase,
                !s_split_mode ? 'O' : (s_prev_frame_class ? 'L' : 'R'));
}

/* VILOG: JUTVideo::preRetraceProc entry — the guest's per-retrace pick. With
 * a double XFB it scans out mBuffer[drawn] iff !sDrawWaiting, else repeats. */
#define JUTVIDEO_SDRAWWAITING 0x803F78E4u
static void on_vi_retrace(CPUState* state)
{
    if (!s_vilog)
        return;
    const uint32_t mgr = rd32_fast(state, JUTXFB_MANAGER);
    int drn = -9, dsp = -9;
    uint32_t addr = 0;
    if (in_ram(state, mgr, 0x20u)) {
        drn = (int)(int16_t)rd16_fast(state, mgr + 0x16u);
        dsp = (int)(int16_t)rd16_fast(state, mgr + 0x18u);
        if (drn >= 0 && drn < 3)
            addr = rd32_fast(state, mgr + (uint32_t)drn * 4u);
    }
    fprintf(stderr, "[vir] tb=%llu wait=%u drawn=%d disp=%d addr=%08X\n",
            (unsigned long long)state->timebase,
            (unsigned)rd8_fast(state, JUTVIDEO_SDRAWWAITING), drn, dsp, (unsigned)addr);
}

/* ========== Stage-B J3D history buffer (env-gated MODERNGEKKO_J3D_INTERP=1) ========== */
/* J3D Mtx[3][4] = 12 f32 = 48 bytes. Per-model history stores prev/curr/scratch. */
#define J3D_MTX_BYTES 48u
#define J3D_MAX_MODELS 4096u
#define J3D_MAX_JOINTS 256u  /* per-model cap; mirrors J3DModelData jointNum typical < 100 */

typedef struct {
    float m[3][4];
} J3DMtx;

typedef struct {
    uint32_t guest_model_ptr; /* guest address of J3DModel* (key) */
    uint32_t joint_num;
    uint32_t wEvlp_num;       /* envelope count, 0 if none */
    uint32_t has_prev;
    uint32_t dirty;
    uint32_t teleported;
    uint32_t no_interp;
    uint32_t gen;
    J3DMtx* prev;
    J3DMtx* curr;
    J3DMtx* scratch;
    J3DMtx* prev_env;
    J3DMtx* curr_env;
    J3DMtx* scratch_env;
    /* Live-injection state (R-frame render). */
    uint32_t node_ptr;        /* guest mpNodeMtx array */
    uint32_t env_ptr;         /* guest mpWeightEnvMtx array */
    uint32_t model_data;      /* guest J3DModelData* */
    uint32_t flags_f0;        /* modelData->flags & 0xF0 (0x20 = ConcatView) */
    uint32_t draw_mtx_num;    /* J3DDrawMtxData.mEntryNum */
    uint32_t view_no;         /* mCurrentViewNo at production */
    uint32_t draw_ptr;        /* resolved mpDrawMtxBuf[1][viewNo] at production */
    uint32_t draw_buf1;       /* mpDrawMtxBuf[1] pointer-array that draw_ptr was read through */
    uint32_t draw_gen;        /* s_draw_gen stamp of the refresh that resolved draw_ptr */
    uint32_t has_prev_env;
    uint32_t has_prev_draw;
    uint32_t dirty_draw;
    uint32_t injected;        /* live arrays currently hold our lerp bits */
    J3DMtx* prev_draw;
    J3DMtx* curr_draw;
    J3DMtx* scratch_draw;
    uint32_t seen_epoch;    /* s_purge_epoch at this slot's last ensure()   */
    int used;
} J3DHistory;

static J3DHistory s_hist[J3D_MAX_MODELS];
static uint32_t s_hist_gen = 1;
/* Spawn-reuse epoch: bumped by every purge path (j3d_purge_range calls and
 * every slot drop in j3d_slot_reset — dtors and liveness drops included).
 * ensure() compares it against the slot's seen_epoch; a mismatch means guest
 * memory was recycled since this slot last registered, so the kept pose
 * pair may belong to the previous owner of this address — invalidate once
 * rather than lerp one frame across the ownership change. Cheap: one u32
 * bump per purge call/drop, one compare per ensure(). Note the interaction:
 * with MODERNGEKKO_F60_PURGE_FREE=1 every operator-delete bumps the epoch,
 * so survivors re-seed constantly — that knob is a crash-hunt aid, not a
 * shipped config. */
static uint32_t s_purge_epoch = 0;
/* Drawlist preflight gating: the walk exists to catch draw packets whose
 * memory was recycled onto a stale history pointer — only possible after a
 * slot teardown. j3d_slot_reset arms a bounded window of draws; a sparse
 * periodic stride keeps a backstop for teardown paths the hooks can't see
 * (intra-chunk frees inside JKRHeap.cpp that bypass every funnel). */
static uint32_t s_dlchk_budget = 0;
static uint32_t s_dlchk_draws = 0;

/* TEMP diagnostic: per-hook fire counts to localize the livelock. */
static uint64_t s_dbg_entrymd, s_dbg_calcent, s_dbg_calcret,
                s_dbg_vcent, s_dbg_vcret, s_dbg_dtor;
static void j3d_dbg_counts(const char* tag)
{
    fprintf(stderr,
        "[j3d-dbg] %s entryMD=%llu calcE=%llu calcR=%llu vcE=%llu vcR=%llu dtor=%llu\n",
        tag, (unsigned long long)s_dbg_entrymd, (unsigned long long)s_dbg_calcent,
        (unsigned long long)s_dbg_calcret, (unsigned long long)s_dbg_vcent,
        (unsigned long long)s_dbg_vcret, (unsigned long long)s_dbg_dtor);
}

static J3DHistory* j3d_find(uint32_t guest_ptr)
{
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i)
        if (s_hist[i].used && s_hist[i].guest_model_ptr == guest_ptr)
            return &s_hist[i];
    return 0;
}
static J3DHistory* j3d_alloc_slot(uint32_t guest_ptr)
{
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i)
        if (!s_hist[i].used)
        {
            s_hist[i].used = 1;
            s_hist[i].guest_model_ptr = guest_ptr;
            ++s_hist_used;
            return &s_hist[i];
        }
    return 0;
}
static void j3d_free_mtx(J3DMtx* p)
{
#if defined(_WIN32)
    if (p) _aligned_free(p);
#else
    if (p) free(p);
#endif
}
static J3DMtx* j3d_alloc_mtx(uint32_t n)
{
    if (n == 0 || n > J3D_MAX_JOINTS) return 0;
    /* 32-byte aligned for Paired-Single; malloc is 16-aligned, over-allocate */
    size_t bytes = (size_t)n * J3D_MTX_BYTES;
#if defined(_WIN32)
    return (J3DMtx*)_aligned_malloc(bytes, 32u);
#else
    return (J3DMtx*)aligned_alloc(32u, (bytes + 31u) & ~(size_t)31u);
#endif
}
static void j3d_lerp_mtx(const J3DMtx* a, const J3DMtx* b, float alpha, J3DMtx* dst, uint32_t n)
{
    /* Non-finite guard (mirrors stream_patch_matrices): a captured pose can
     * carry NaN/Inf (a corrupt array read or a mid-write observation), and
     * lerping it writes NaN vertices into the live array the draw consumes.
     * Fall back to the newest endpoint (b — what production actually
     * computed) for any element whose blend isn't finite. */
    for (uint32_t j = 0; j < n; ++j)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) {
                const float v = a[j].m[r][c] + alpha * (b[j].m[r][c] - a[j].m[r][c]);
                dst[j].m[r][c] = isfinite(v) ? v : b[j].m[r][c];
            }
}
/* Exposed for TB tests (linkage-visible via mod .so symbol). */
void j3d_test_lerpMatrix(const float* prev, const float* curr, float alpha, float* out, uint32_t floats)
{
    for (uint32_t i = 0; i < floats; ++i) out[i] = prev[i] + alpha * (curr[i] - prev[i]);
}
/* Helpers for ctest synthetic harness (not guest hooks): allocate/rotate/dirty/teleport/view redirection */
J3DHistory* j3d_history_ensure(uint32_t guest_ptr, uint32_t joint_num, uint32_t wEvlp, int no_interp_flag);
J3DHistory* j3d_history_ensure(uint32_t guest_ptr, uint32_t joint_num, uint32_t wEvlp, int no_interp_flag)
{
    if (!s_j3d_interp) return 0;
    J3DHistory* h = j3d_find(guest_ptr);
    if (h && (h->joint_num != joint_num || h->wEvlp_num != wEvlp))
    {
        j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch);
        j3d_free_mtx(h->prev_env); j3d_free_mtx(h->curr_env); j3d_free_mtx(h->scratch_env);
        j3d_free_mtx(h->prev_draw); j3d_free_mtx(h->curr_draw); j3d_free_mtx(h->scratch_draw);
        h->prev = h->curr = h->scratch = 0;
        h->prev_env = h->curr_env = h->scratch_env = 0;
        h->prev_draw = h->curr_draw = h->scratch_draw = 0;
        h->has_prev = 0; h->dirty = 0; h->teleported = 0;
        /* The draw buffers hold poses from the OLD joint layout — a stale
         * has_prev_draw would lerp across the resize. Reset all history and
         * every cached field so the slot re-derives from scratch (model_data
         * keys the calc-time configure path; zeroing it forces flags_f0 /
         * draw_mtx_num to be re-read, and the snapshot skips the slot until
         * then via its !h->model_data guard). */
        h->has_prev_env = 0; h->has_prev_draw = 0; h->dirty_draw = 0;
        h->draw_mtx_num = 0; h->draw_ptr = 0; h->draw_buf1 = 0; h->view_no = 0;
        h->draw_gen = 0;
        h->node_ptr = h->env_ptr = 0;
        h->model_data = 0; h->flags_f0 = 0; h->injected = 0;
    }
    if (!h) h = j3d_alloc_slot(guest_ptr);
    if (!h) return 0;
    /* A purge/dtor/liveness drop since this slot's last registration means
     * guest memory moved — a recycled J3DModel at the same addr+counts would
     * otherwise keep the old pose and lerp one frame from its previous
     * owner. Drop the endpoints once; the next observation re-seeds. */
    if (h->seen_epoch != s_purge_epoch) {
        h->seen_epoch = s_purge_epoch;
        h->has_prev = 0; h->dirty = 0; h->teleported = 0;
        h->has_prev_env = 0; h->has_prev_draw = 0; h->dirty_draw = 0;
        h->injected = 0;
    }
    if (!h->prev)
    {
        h->joint_num = joint_num; h->wEvlp_num = wEvlp;
        h->node_ptr = h->env_ptr = 0;
        h->model_data = 0;
        h->flags_f0 = 0; h->draw_mtx_num = 0; h->view_no = 0; h->draw_ptr = 0;
        h->draw_buf1 = 0; h->draw_gen = 0;
        h->has_prev_env = 0; h->has_prev_draw = 0; h->dirty_draw = 0;
        h->injected = 0;
        h->no_interp = no_interp_flag ? 1u : 0u;
        h->has_prev = 0; h->dirty = 0; h->teleported = 0;
        h->gen = s_hist_gen++;
        if (joint_num > 0 && joint_num <= J3D_MAX_JOINTS) {
            h->prev = j3d_alloc_mtx(joint_num);
            h->curr = j3d_alloc_mtx(joint_num);
            h->scratch = j3d_alloc_mtx(joint_num);
            if (wEvlp > 0) {
                h->prev_env = j3d_alloc_mtx(wEvlp);
                h->curr_env = j3d_alloc_mtx(wEvlp);
                h->scratch_env = j3d_alloc_mtx(wEvlp);
            } else h->prev_env = h->curr_env = h->scratch_env = 0;
        }
        if (!h->prev || !h->curr || !h->scratch)
        {
            /* All-or-nothing: a partial alloc (e.g. prev fails, curr succeeds)
             * would leave h->curr non-NULL and slip past j3d_history_rotate's
             * !h->curr guard into a NULL-prev memcpy. Free the partial set so
             * the buffers are either all valid or all NULL. */
            j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch);
            h->prev = h->curr = h->scratch = 0;
            h->no_interp = 1u;
        }
        else if (h->wEvlp_num > 0 &&
                 (!h->prev_env || !h->curr_env || !h->scratch_env))
        {
            /* Envelope history requested but only partially allocated (OOM or
             * wEvlp_num > J3D_MAX_JOINTS). Lerping joints while the envelope
             * stays frozen produces a half-interpolated frame; mark the model
             * no_interp so both halves stay consistent. */
            h->no_interp = 1u;
        }
    }
    return h;
}
/* O(1) slot teardown shared by j3d_history_free and the heap-free purge —
 * frees every host buffer and clears all consumed-pointer bookkeeping so a
 * dropped slot can never feed a stale pointer back into the inject paths. */
static void j3d_slot_reset(J3DHistory* h)
{
    j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch);
    j3d_free_mtx(h->prev_env); j3d_free_mtx(h->curr_env); j3d_free_mtx(h->scratch_env);
    j3d_free_mtx(h->prev_draw); j3d_free_mtx(h->curr_draw); j3d_free_mtx(h->scratch_draw);
    h->prev = h->curr = h->scratch = 0;
    h->prev_env = h->curr_env = h->scratch_env = 0;
    h->prev_draw = h->curr_draw = h->scratch_draw = 0;
    h->used = 0; h->has_prev = 0; h->dirty = 0; h->teleported = 0;
    h->has_prev_env = 0; h->has_prev_draw = 0; h->dirty_draw = 0;
    h->draw_mtx_num = 0; h->draw_ptr = 0; h->draw_buf1 = 0; h->draw_gen = 0;
    h->node_ptr = h->env_ptr = 0; h->model_data = 0; h->flags_f0 = 0;
    h->injected = 0;
    if (s_hist_used) --s_hist_used;
    /* Ownership changed under this address — bump the reuse epoch so every
     * other slot revalidates its pose pair on next ensure() (same-count
     * address recycling would otherwise keep the old owner's pose). */
    ++s_purge_epoch;
    /* A dropped slot means some guest block was freed underneath tracked
     * pointers — the exact window in which a draw packet can be corrupted.
     * Arm the drawlist preflight for a bounded stretch of draw calls. */
    s_dlchk_budget = 120u;
}
void j3d_history_free(uint32_t guest_ptr);
void j3d_history_free(uint32_t guest_ptr)
{
    J3DHistory* h = j3d_find(guest_ptr);
    if (!h) return;
    j3d_slot_reset(h);
}
int j3d_history_rotate(uint32_t guest_ptr, const J3DMtx* new_mtx, uint32_t n);
int j3d_history_rotate(uint32_t guest_ptr, const J3DMtx* new_mtx, uint32_t n)
{
    if (!s_j3d_interp) return 0;
    J3DHistory* h = j3d_find(guest_ptr);
    if (!h || !h->curr || !h->prev || !h->scratch || n != h->joint_num) return 0;
    /* new_mtx carries n joints; the env buffers are wEvlp_num entries. When
     * wEvlp_num > n, copying wEvlp_num from new_mtx would read past the
     * caller's array, so clamp the new_mtx->env placeholder copies. */
    const uint32_t env_src_n = h->wEvlp_num < n ? h->wEvlp_num : n;
    if (!h->has_prev) {
        memcpy(h->curr, new_mtx, (size_t)n * J3D_MTX_BYTES);
        memcpy(h->prev, h->curr, (size_t)n * J3D_MTX_BYTES);
        if (h->wEvlp_num && h->curr_env) memcpy(h->curr_env, new_mtx, (size_t)env_src_n * J3D_MTX_BYTES);
        if (h->wEvlp_num && h->prev_env && h->curr_env) memcpy(h->prev_env, h->curr_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES);
        h->has_prev = 1; h->dirty = 0; h->teleported = 0;
        return 1;
    }
    /* teleport check: root translation jump >400 per-axis or hypot>500 */
    float dx = new_mtx[0].m[0][3] - h->curr[0].m[0][3];
    float dy = new_mtx[0].m[1][3] - h->curr[0].m[1][3];
    float dz = new_mtx[0].m[2][3] - h->curr[0].m[2][3];
    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
    float hypot = ax + ay + az; /* L1 proxy; true euclidian not needed for warp gate */
    /* also check base transform jump via same mtx as proxy */
    int is_teleport = (ax > 400.0f || ay > 400.0f || az > 400.0f || hypot > 500.0f) ? 1 : 0;
    if (is_teleport) {
        memcpy(h->curr, new_mtx, (size_t)n * J3D_MTX_BYTES);
        memcpy(h->prev, h->curr, (size_t)n * J3D_MTX_BYTES);
        /* Reseed env history the same way the main history is reseeded:
         * curr_env <- new pose, prev_env <- curr_env. Without the curr_env
         * copy the env pair straddles the teleport (prev_env stays at the
         * pre-teleport env while main prev/curr are post-teleport), so the
         * next steady frame would lerp the envelope across the warp. */
        if (h->wEvlp_num && h->curr_env) memcpy(h->curr_env, new_mtx, (size_t)env_src_n * J3D_MTX_BYTES);
        if (h->wEvlp_num && h->prev_env && h->curr_env) memcpy(h->prev_env, h->curr_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES);
        h->teleported = 1; h->dirty = 0;
        return 1;
    }
    /* steady: prev <- curr, curr <- new */
    memcpy(h->prev, h->curr, (size_t)n * J3D_MTX_BYTES);
    if (h->wEvlp_num && h->prev_env && h->curr_env) memcpy(h->prev_env, h->curr_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES);
    memcpy(h->curr, new_mtx, (size_t)n * J3D_MTX_BYTES);
    if (h->wEvlp_num && h->curr_env) memcpy(h->curr_env, new_mtx, (size_t)env_src_n * J3D_MTX_BYTES); /* placeholder copy */
    /* dirty if any joint moved > eps */
    const float eps = 0.02f;
    int dirty = 0;
    for (uint32_t j = 0; j < n && !dirty; ++j)
        for (int r = 0; r < 3 && !dirty; ++r)
            for (int c = 0; c < 4; ++c) {
                float d = new_mtx[j].m[r][c] - h->prev[j].m[r][c];
                if (d < 0) d = -d;
                if (d > eps) { dirty = 1; break; }
            }
    h->dirty = dirty ? 1u : 0u;
    h->teleported = 0;
    return 1;
}
/* viewCalc predicate — should we lerp? */
static int j3d_should_lerp(J3DHistory* h, float alpha)
{
    if (!h || !h->has_prev || h->no_interp || h->teleported || !h->dirty) return 0;
    if (alpha <= 0.0f || alpha >= 1.0f) return 0;
    return 1;
}
/* B4 helper used by ctest: redirect to scratch if should_lerp */
int j3d_viewcalc_should_redirect(uint32_t guest_ptr, float alpha);
int j3d_viewcalc_should_redirect(uint32_t guest_ptr, float alpha)
{
    J3DHistory* h = j3d_find(guest_ptr);
    return j3d_should_lerp(h, alpha);
}

/* ========== Live injection (MODERNGEKKO_RFRAME_RENDER=1) ==========
 * Snapshot the consumed matrix arrays on L-frames; on R-frames write
 * lerp(prev,curr,alpha) into the guest arrays the persistent packet list
 * resolves, then let Painter re-draw -> a unique interpolated frame.
 *
 *   modelData->flags & 0xF0 == 0x20 (NoUseDrawMtx / ConcatView): shapes read
 *       mpNodeMtx + mpWeightEnvMtx directly at draw -> snapshot/inject those.
 *   flags 0x00 / 0x10 (double/single-buffered): shapes consume
 *       mpDrawMtxBuf[1][viewNo] (view x anm, computed by viewCalc) ->
 *       snapshot/inject the resolved draw array.
 * CPU-skin models (mpSkinDeform != 0) bake verts inside calc -> no_interp.
 * Everything self-heals: the next calc()/viewCalc() rewrites the arrays. */

/* MEM1 bounds check — a corrupt guest pointer must not turn into an
 * aliased in-bounds access. */
#define J3D_IN_MEM1(a) ((uint32_t)((a) - 0x80000000u) < 0x01800000u)

/* ---- GP FIFO replay feasibility probe --------------------------------------
 * The PI fifo is a circular byte buffer in guest RAM (m_fifo_cpu_base..end,
 * write pointer at m_fifo_cpu_write_pointer). If the mod can read the regs +
 * fifo contents and write to the wptr register, an R-frame can replay the
 * captured stream without any engine-side changes. This probe validates that
 * access and measures per-frame stream size + the copy-exec marker. */
static uint32_t rd32(CPUState* st, uint32_t addr)
{
    return (uint32_t)moderngekko_mod_read(st, addr, 4u);
}
static uint32_t rd16(CPUState* st, uint32_t addr)
{
    return (uint32_t)moderngekko_mod_read(st, addr, 2u) & 0xFFFFu;
}

/* Direct MEM1 access for hot paths: the external accessors go through
 * TranslateRelAddress+MSR+MMU per word — fatal at J3D/judge rates. All uses
 * are bounds-checked against ram_size; anything outside MEM1 falls back to
 * the safe accessor (MMIO/exceptions keep exact semantics there). */
static int in_ram(const CPUState* s, uint32_t ea, uint32_t size)
{
    return s->ram && (ea - 0x80000000u) <= s->ram_size - size;
}
static uint32_t rd32_ram(const CPUState* s, uint32_t a)
{
    const uint8_t* p = s->ram + (a - 0x80000000u);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t rd16_ram(const CPUState* s, uint32_t a)
{
    const uint8_t* p = s->ram + (a - 0x80000000u);
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}
static uint32_t rd32_fast(CPUState* s, uint32_t a)
{
    return in_ram(s, a, 4u) ? rd32_ram(s, a) : rd32(s, a);
}
static uint32_t rd16_fast(CPUState* s, uint32_t a)
{
    return in_ram(s, a, 2u) ? rd16_ram(s, a) : rd16(s, a);
}
static uint32_t rd8_fast(CPUState* s, uint32_t a)
{
    return in_ram(s, a, 1u) ? (uint32_t)s->ram[a - 0x80000000u]
                            : (uint32_t)moderngekko_mod_read(s, a, 1u) & 0xFFu;
}
/* Direct-RAM stores (BE). Only ever used on MEM1 data addresses — the
 * in_ram gate keeps MMIO on the external_write path — so the result is
 * bit-identical to moderngekko_mod_write minus translate+MMU per call
 * (data writes don't need the SMC journal — see write_mtx_arr's comment). */
static void wr32_ram(const CPUState* s, uint32_t a, uint32_t v)
{
    uint8_t* p = s->ram + (a - 0x80000000u);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void wr32_fast(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 4u)) wr32_ram(s, a, v);
    else moderngekko_mod_write(s, a, v, 4u);
}
static void wr16_fast(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 2u)) {
        uint8_t* p = s->ram + (a - 0x80000000u);
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
    } else moderngekko_mod_write(s, a, v, 2u);
}
static void wr8_fast(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 1u)) s->ram[a - 0x80000000u] = (uint8_t)v;
    else moderngekko_mod_write(s, a, v, 1u);
}

/* 8-byte guest accesses where the array is 8B-aligned (J3D Mtx arrays are
 * 32B-aligned); falls back to 4B otherwise. A u64 external access byteswaps
 * the WHOLE word: MMU::Read<u64> returns hi-lane<<32|lo-lane and Write<u64>
 * stores hi then lo, so the two f32 lanes inside each u64 are handled
 * explicitly — out[i]/in[i] stay in guest element order. Bits move through
 * memcpy (like be_f32/wr_be_f32) so the J3DMtx floats keep float effective
 * type and later element reads stay defined under strict aliasing. */
/* Whole-span MEM1 probe for the matrix-array movers below. The J3D paths
 * run ~1M external reads+writes per bench minute through the full
 * TranslateRelAddress+MSR+MMU stack; every target is a J3DMtx/node/env array
 * in main RAM, so a single bounds check licenses a straight byteswap loop on
 * state->ram (data writes — never code — so bypassing the SMC journal is
 * sound). Falls back to the external accessors for anything outside MEM1. */
static int arr_in_ram(const CPUState* st, uint32_t addr, uint32_t bytes)
{
    return st->ram && (addr - 0x80000000u) <= st->ram_size - bytes;
}

static void read_mtx_arr(CPUState* st, uint32_t addr, J3DMtx* dst, uint32_t n)
{
    float* out = (float*)dst;
    uint32_t floats = n * 12u;
    if (arr_in_ram(st, addr, floats * 4u)) {
        const uint8_t* p = st->ram + (addr - 0x80000000u);
        for (uint32_t i = 0; i < floats; ++i) {
            uint32_t w = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                        ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
            memcpy(&out[i], &w, 4u);
        }
        return;
    }
    if ((addr & 7u) == 0) {
        for (uint32_t i = 0; i < floats; i += 2u) {
            uint64_t v = moderngekko_mod_read(st, addr + i * 4u, 8u);
            uint32_t w0 = (uint32_t)(v >> 32);   /* hi lane = first float */
            uint32_t w1 = (uint32_t)v;           /* lo lane = second float */
            memcpy(&out[i], &w0, 4u);
            memcpy(&out[i + 1u], &w1, 4u);
        }
    } else {
        for (uint32_t i = 0; i < floats; ++i) {
            uint32_t w = rd32(st, addr + i * 4u);
            memcpy(&out[i], &w, 4u);
        }
    }
}
static void write_mtx_arr(CPUState* st, uint32_t addr, const J3DMtx* src, uint32_t n)
{
    const float* in = (const float*)src;
    uint32_t floats = n * 12u;
    if (arr_in_ram(st, addr, floats * 4u)) {
        uint8_t* p = st->ram + (addr - 0x80000000u);
        for (uint32_t i = 0; i < floats; ++i) {
            uint32_t w;
            memcpy(&w, &in[i], 4u);
            p[i*4]   = (uint8_t)(w >> 24);
            p[i*4+1] = (uint8_t)(w >> 16);
            p[i*4+2] = (uint8_t)(w >> 8);
            p[i*4+3] = (uint8_t)w;
        }
        return;
    }
    if ((addr & 7u) == 0) {
        for (uint32_t i = 0; i < floats; i += 2u) {
            uint32_t w0, w1;
            memcpy(&w0, &in[i], 4u);
            memcpy(&w1, &in[i + 1u], 4u);
            moderngekko_mod_write(st, addr + i * 4u,
                                  ((uint64_t)w0 << 32) | w1, 8u);
        }
    } else {
        for (uint32_t i = 0; i < floats; ++i) {
            uint32_t w;
            memcpy(&w, &in[i], 4u);
            moderngekko_mod_write(st, addr + i * 4u, w, 4u);
        }
    }
}

/* ---- stale-entry purge + liveness (post-transition crash fix) ---------------
 * The B3 dtor hook only frees history for models that die through
 * ~J3DModel. JKR bulk frees — heap destroy()/freeAll() at scene teardown,
 * REL unload, actor-heaps — kill model memory WITHOUT running C++ dtors,
 * leaving slots whose cached node/env/draw pointers are stale. The inject/
 * refresh paths then write lerp data into whatever reuses the freed block
 * (draw-buffer packets included: the observed crash was a packet whose
 * vtbl word held a lerped -0.0f = 0x80000000 -> null vcall in drawHead).
 *
 * Every consumed pointer hangs off a block released through the JKRHeap
 * API, so purge at the funnels:
 *   - JKRHeap::free(void*)          0x802B0560 — member funnel (r4 = ptr);
 *   - JKRHeap::free(void*,JKRHeap*) 0x802B0518 — static (r3 = ptr); callers
 *     that name a heap take this path;
 *   - __dl__FPv/__dla__FPv 0x802B0D28/0x802B0D4C — operator delete/delete[]
 *     (r3 = ptr), THE funnel for `delete` — the static->member tail call
 *     inside is intra-chunk so it never re-dispatches a hook;
 *   - JKRHeap::freeAll/freeTail/destroy 0x802B0634/0x802B069C/0x802B0408 —
 *     bulk ops (r3 = this): purge every pointer inside the heap's own
 *     mStart..mEnd range (over-drop is safe — live models re-register
 *     through the normal calc path). */
static void j3d_purge_range(CPUState* st, uint32_t lo, uint32_t hi)
{
    (void)st;
    if (!s_j3d_interp || !s_hist_used || hi <= lo) return;
    /* The epoch bumps ONLY when a slot is actually dropped (j3d_slot_reset,
     * below). mDoGph_gInf_c::free() calls JKRHeap::freeAll on the swapped
     * gInf heap EVERY Painter iteration (m_Do_graphic.cpp:143-145), so the
     * old unconditional bump cycled the epoch at 60Hz — every ensure() saw
     * seen_epoch != s_purge_epoch, cleared has_prev, and j3d_apply_pose
     * never interpolated (inj=0/0 in the [f60] stats). A purge that drops
     * nothing frees only untracked memory, so no slot needs revalidation.
     * MODERNGEKKO_F60_PURGE_EPOCH_ALWAYS=1 restores the old always-bump. */
    if (s_purge_epoch_always)
        ++s_purge_epoch;
    const uint32_t span = hi - lo;
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        J3DHistory* h = &s_hist[i];
        if (!h->used) continue;
        if ((h->guest_model_ptr - lo) < span || (h->model_data - lo) < span ||
            (h->node_ptr - lo) < span || (h->env_ptr - lo) < span ||
            (h->draw_buf1 - lo) < span || (h->draw_ptr - lo) < span) {
            j3d_slot_reset(h);
            ++s_dbg_purged;
        }
    }
}
/* Purge entries pointing into the block that owns `ptr` — ExpHeap blocks
 * carry CMemBlock{'HM',flags,group,size,prev,next} at ptr-0x10 so embedded
 * objects/arrays drop too; unheaded allocations (SolidHeap) fall back to
 * the pointer itself. Runs BEFORE the free executes — header still live. */
static void j3d_purge_ptr_block(CPUState* st, uint32_t ptr)
{
    uint32_t span = 4u;
    const uint32_t blk = ptr - 0x10u;
    if (J3D_IN_MEM1(blk) && rd16_fast(st, blk) == 0x484Du /* 'HM' */) {
        const uint32_t sz = rd32_fast(st, blk + 4u);
        if (sz && sz < 0x01800000u) span = sz + 0x10u;
    }
    j3d_purge_range(st, ptr, ptr + span);
}
static void on_jkr_opdel(CPUState* state)          /* r3 = ptr */
{
    if (!s_j3d_interp || !s_hist_used) return;
    const uint32_t ptr = (uint32_t)state->gpr[3];
    if (!J3D_IN_MEM1(ptr)) return;
    j3d_purge_ptr_block(state, ptr);
}
static void on_jkr_free_static(CPUState* state)    /* r3 = ptr */
{
    on_jkr_opdel(state);
}
static void on_jkr_free_member(CPUState* state)    /* r4 = ptr, r3 = this */
{
    if (!s_j3d_interp || !s_hist_used) return;
    const uint32_t ptr = (uint32_t)state->gpr[4];
    if (!J3D_IN_MEM1(ptr)) return;
    j3d_purge_ptr_block(state, ptr);
}
static void on_jkr_bulk(CPUState* state)           /* r3 = this (heap) */
{
    if (!s_j3d_interp || !s_hist_used) return;
    const uint32_t heap = (uint32_t)state->gpr[3];
    if (!J3D_IN_MEM1(heap)) return;
    const uint32_t hs = rd32_fast(state, heap + 0x30u); /* JKRHeap::mStart */
    const uint32_t he = rd32_fast(state, heap + 0x34u); /* JKRHeap::mEnd   */
    if (J3D_IN_MEM1(hs) && he > hs && (he - 0x80000000u) <= 0x01800000u)
        j3d_purge_range(state, hs, he);
}
/* Per-consume liveness backstop — frees issued by callers inside
 * JKRHeap.cpp's own chunk dispatch as gotos and bypass the funnels, so a
 * slot can still outlive its model. A live J3DModel keeps __vt__8J3DModel
 * at +0 and mModelData at +4 for its entire life; a reused address fails
 * at least one. (Merely-freed-but-intact memory still passes both — that
 * window is owned by the funnel above.) */
static int j3d_model_alive(CPUState* s, const J3DHistory* h)
{
    if (!J3D_IN_MEM1(h->guest_model_ptr) || !J3D_IN_MEM1(h->model_data)) return 0;
    if (rd32_fast(s, h->guest_model_ptr) != 0x8039EA50u /* __vt__8J3DModel */)
        return 0;
    if (rd32_fast(s, h->guest_model_ptr + 0x04u) != h->model_data) return 0;
    return 1;
}

/* J3DDrawBuffer::draw preflight — the persistent packet list references
 * process-owned memory, and on R-frames it is re-walked after ungated time
 * has passed. A corrupted packet is a FATAL null vcall inside drawHead
 * (observed: vtbl=0x80000000 -> *(vtbl+0x10)=0 -> bctrl to 0 -> DSI -> the
 * retail exception handler parks the emu thread while VI keeps running —
 * the "game logic froze but presentation continues" signature). Walk the
 * chains first (bounded): packet must be in MEM1, its vtbl must point into
 * real vtbl space (>= DOL text base, inside MEM1), and the draw() slot at
 * vtbl+0x10 must be a nonzero code-range pointer. Anything dead skips just
 * this buffer's draw — a dropped sub-frame instead of a parked guest. */
static int j3d_drawlist_dead(CPUState* state, uint32_t db)
{
    if (!J3D_IN_MEM1(db)) return 1;
    const uint32_t mpBuf = rd32_fast(state, db + 0x00u);
    const uint32_t n = rd32_fast(state, db + 0x04u);
    if (!J3D_IN_MEM1(mpBuf) || n == 0u || n > 0x400u) return 1;
    uint32_t walked = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = rd32_fast(state, mpBuf + i * 4u);
        while (p) {
            if (!J3D_IN_MEM1(p) || ++walked > 0x4000u) return 1;
            const uint32_t vt = rd32_fast(state, p);
            if (!J3D_IN_MEM1(vt) || vt < 0x80003100u) return 1;
            const uint32_t fn = rd32_fast(state, vt + 0x10u);
            if (!J3D_IN_MEM1(fn) || fn < 0x80003100u) return 1;
            p = rd32_fast(state, p + 4u);
        }
    }
    return 0;
}
static void on_drawbuffer_gate(CPUState* state)
{
    if (!s_enabled) return;
    if (s_drawlist_chk) {
        int run = 0;
        if (s_dlchk_budget) { --s_dlchk_budget; run = 1; }
        else if ((++s_dlchk_draws & 0x3FFu) == 0) run = 1;
        if (run && j3d_drawlist_dead(state, (uint32_t)state->gpr[3])) {
            state->pc = state->lr;
            return;
        }
    }
    if (s_split_mode && !s_logic_this_frame && s_r_dup)
        state->pc = state->lr;
}

/* One-shot-ish FIFO probe: reports geometry + scans back from wptr for the
 * previous XFB-copy-exec (0x61 0x52 …) to size the last emitted stream. */
static void fifo_probe(CPUState* st)
{
    /* PI FIFO regs hold PHYSICAL addresses (MEM1: 0x00000000..0x017FFFFF);
     * external reads want effective addresses -> +0x80000000. */
    uint32_t base = rd32(st, PI_FIFO_BASE_REG);
    uint32_t end = rd32(st, PI_FIFO_END_REG);
    uint32_t wptr = rd32(st, PI_FIFO_WPTR_REG);
    if (base >= 0x01800000u || end > 0x01800000u || wptr >= 0x01800000u || end <= base ||
        wptr < base || wptr >= end) {
        fprintf(stderr, "[fifo] BAD geometry base=%08x end=%08x wptr=%08x\n", base, end, wptr);
        return;
    }
    uint32_t fsize = end - base;
    /* Walk back through the circular buffer (wrap at base->end) for '61 52'. */
    uint32_t found = 0, found_at = 0;
    uint32_t a = wptr;
    uint32_t steps = fsize / 4u;
    if (steps > (1u << 20) / 4u) steps = (1u << 20) / 4u;
    for (uint32_t i = 1; i <= steps; ++i) {
        a -= 4u;
        if (a < base) a = end - 4u;
        /* fifo body is MEM1 — rd32_fast takes the direct-RAM path (the
         * reg reads above stay external: 0xCC0030xx is MMIO). */
        uint32_t v = rd32_fast(st, a + 0x80000000u);
        if ((v & 0xFFFF0000u) == 0x61520000u) {
            found = 1; found_at = i * 4u; break;
        }
    }
    fprintf(stderr, "[fifo] base=%08x end=%08x wptr=%08x fsize=%u last-copy@%uB-back found=%u\n",
            base, end, wptr, fsize, found_at, found);
}

/* ========== GP stream capture + replay (MODERNGEKKO_FIFO_REPLAY=1) ==========
 * A "frame stream" = fifo bytes between consecutive XFB copy-exec commands
 * (BP write 0x61 0x52 …). The PI fifo is a circular buffer in guest RAM with
 * ~640KB — ~7 frames of history. On an R-frame the last complete frame is
 * appended back into the fifo and the CP write/distance registers are bumped,
 * so the video thread re-renders it — a produced frame without re-running
 * guest draw code (~91KB of MMIO writes vs ~28ms of Painter traversal).
 *
 * Interpolation: two consecutive captures of identical length are the same
 * packet list re-emitted — differing bytes are the animated data (matrix/
 * color payloads; structural fields are stable). Lerp differing u32-as-float
 * regions between prev and curr at s_interp_alpha. If lengths mismatch,
 * replay curr unpatched (a duplicate frame — graceful degradation). */

#define CP_FIFO_RW_DISTANCE_REG 0xCC000030u   /* u16 lo @0x30, hi @0x32 */
#define CP_FIFO_WPTR_REG        0xCC000034u   /* u16 lo @0x34, hi @0x36 */
#define FIFO_MIN_FRAME          256u          /* real frames are ~91KB; <this = noise */

static uint8_t* s_stream_prev = 0, * s_stream_curr = 0;
static uint8_t* s_stream_out = 0, * s_carry = 0;
static uint32_t s_stream_prev_len = 0, s_stream_curr_len = 0;
static uint32_t s_carry_len = 0;           /* partial-frame bytes awaiting the next region */
static uint32_t s_capture_gen = 0;         /* bumped when a new frame lands in curr */
static uint32_t s_owed_injects = 0;        /* produced frames still owed an injected companion */
static uint32_t s_injects_in_pair = 0;     /* injects emitted for the current prev/curr pair */
/* Frame capture by write-pointer delta: between two consecutive R-frame
 * observations, every fifo byte was written by the GAME (our own injections
 * advance the observation point past themselves via the post-inject re-read).
 * No marker scanning, no injected-span bookkeeping, no false boundaries. */
static uint32_t s_obs_wptr = 0;
static int      s_obs_valid = 0;
static uint32_t s_obs_base = 0, s_obs_end = 0;  /* fifo geometry at last observation */
static uint32_t s_scan_resync = 0;   /* stream discontinuity: next scan must relock clean */

static int fifo_geom(CPUState* st, uint32_t* base, uint32_t* end, uint32_t* wptr)
{
    uint32_t b = rd32(st, PI_FIFO_BASE_REG);
    uint32_t e = rd32(st, PI_FIFO_END_REG);
    uint32_t w = rd32(st, PI_FIFO_WPTR_REG);
    if (b >= 0x01800000u || e > 0x01800000u || e <= b || w < b || w >= e)
        return 0;
    *base = b; *end = e; *wptr = w;
    return 1;
}

/* frame60 bulk-access page (engine fast path in HookExternalWrite): one
 * register write replaces tens of thousands of per-word MMIO dispatches. */
#define MG_BULK_PTR_EA   0xCC00F000u   /* w8: host buffer pointer            */
#define MG_BULK_FIFO_EA  0xCC00F008u   /* w4: inject N bytes ptr -> fifo     */
#define MG_BULK_SRC_EA   0xCC00F010u   /* w8: guest phys MEM1 offset         */
#define MG_BULK_READ_EA  0xCC00F018u   /* w4: bulk read N bytes RAM -> ptr   */
#define MG_SCAN_EA       0xCC00F020u   /* w4: GP-walk N bytes ptr -> out[]   */
#define MG_SCAN_OUT_EA   0xCC00F028u   /* w8: host u32 array for offsets     */
#define MG_BULK_STAT_EA  0xCC00F030u   /* r4: last inject result 1/0          */
#define MG_SCAN_RESET_EA 0xCC00F038u   /* w4: drop scanner's carried CP state */

static void bulk_read(CPUState* st, uint32_t phys, uint8_t* dst, uint32_t len)
{
    moderngekko_mod_write(st, MG_BULK_PTR_EA, (uint64_t)(uintptr_t)dst, 8u);
    moderngekko_mod_write(st, MG_BULK_SRC_EA, phys, 8u);
    moderngekko_mod_write(st, MG_BULK_READ_EA, len, 4u);
}

/* Engine-side GP frame-boundary scan: walks `src` with the real OpcodeDecoder
 * and reports the byte offset of every 61 52 XFB-copy command (the true frame
 * end). Byte-pattern matching can't do this reliably — the copy byte sequence
 * appears inside vertex/matrix payload, which was splitting frames mid-command
 * and desyncing the replay decoder ('0x41'/'0x3c' warnings). Returns the
 * number of boundaries found; offsets land in s_scan_bounds[1..n]. */
static uint32_t s_scan_bounds[268];
static uint32_t s_scan_unknowns;
static uint32_t s_scan_first_unknown;
static uint32_t s_scan_xf, s_scan_dl, s_scan_prim, s_scan_bp;
static uint32_t s_scan_xfmtx, s_scan_idx;
static uint32_t fifo_scan_boundaries(CPUState* st, const uint8_t* src, uint32_t len)
{
    s_scan_bounds[0] = 0;
    moderngekko_mod_write(st, MG_BULK_PTR_EA, (uint64_t)(uintptr_t)src, 8u);
    moderngekko_mod_write(st, MG_SCAN_OUT_EA, (uint64_t)(uintptr_t)s_scan_bounds, 8u);
    moderngekko_mod_write(st, MG_SCAN_EA, len, 4u);
    uint32_t n = s_scan_bounds[0];
    if (n > 255u) n = 255u;
    s_scan_unknowns = s_scan_bounds[257];
    s_scan_first_unknown = s_scan_bounds[258];
    s_scan_xf   = s_scan_bounds[259];
    s_scan_dl   = s_scan_bounds[260];
    s_scan_prim = s_scan_bounds[261];
    s_scan_bp   = s_scan_bounds[262];
    s_scan_xfmtx = s_scan_bounds[263];
    s_scan_idx   = s_scan_bounds[264];
    return n;
}

/* Copy a circular range [start, start+len) of physical fifo bytes into dst.
 * Uses the engine bulk-read fast path (one call per contiguous run). */
static void fifo_capture(CPUState* st, uint32_t base, uint32_t end,
                         uint32_t start, uint32_t len, uint8_t* dst)
{
    uint32_t first = end - start;              /* bytes until wrap point */
    if (first > len) first = len;
    bulk_read(st, start, dst, first);
    if (len > first)                           /* wrapped: read rest from base */
        bulk_read(st, base, dst + first, len - first);
}

/* Inject a captured stream through the REAL gather-pipe path: effective
 * 0xCC008000 writes hit StaticRecompCore::HookExternalWrite's fast path ->
 * GPFifo::Write32 -> UpdateGatherPipe -> GatherPipeBursted, which performs
 * all CP/PI pointer + read-write-distance updates atomically (fetch_add vs
 * the video thread's fetch_sub — no torn state) and wakes RunGpu. The only
 * externally-visible effect is a produced frame in the fifo; no register
 * pokes, no manual pointer math. Stream bytes are the BE command stream, so
 * each u32 is composed as b0<<24|b1<<16|b2<<8|b3 (Write32 re-swaps). */
#define GP_GATHER_PIPE_EA 0xCC008000u
static int fifo_inject(CPUState* st, const uint8_t* src, uint32_t len)
{
    uint32_t base, end, wptr;
    if (!fifo_geom(st, &base, &end, &wptr)) return 0;
    uint32_t fsize = end - base;
    if (len + 64u >= fsize) return 0;
    /* Engine bulk-inject: bytes go through GPFifo::FastWrite64 in the hook —
     * identical gather-pipe accounting to per-word writes, ~0.01x the MMIO
     * cost. `len` must already include any tail padding: the engine's
     * free-space gate is checked once per call, so frame+pad injected as a
     * single write is a guaranteed fit — a separate pad write could be
     * refused in the narrow window where free >= len+96 but < len+pad+96,
     * leaving the stream tail mid-burst. */
    moderngekko_mod_write(st, MG_BULK_PTR_EA, (uint64_t)(uintptr_t)src, 8u);
    moderngekko_mod_write(st, MG_BULK_FIFO_EA, len, 4u);
    /* Engine refuses the whole frame atomically when it won't fit the fifo's
     * FREE space (not just its size) — read the result back so `rep` stays
     * honest. */
    if (moderngekko_mod_read(st, MG_BULK_STAT_EA, 4u) == 0)
        return 0;
    return 1;
}

/* ---- XF matrix-load matching ---------------------------------------------
 * GX_LOAD_XF_REG (opcode 0x10) streams register blocks: [10 <cnt-1:u16>
 * <addr:u16> <cnt*4 payload>]. Position/normal/texture matrices live in the
 * low XF register file (<0x0800) and each joint's slot is stable across
 * frames — the SAME (addr,cnt) load carries the same matrix even when the
 * total frame length varies. Lerp those payloads between prev/curr, matched
 * by register address; every other byte copies curr verbatim. This is the
 * only semantically safe patch point: BP/CP payload words are integers —
 * diff-lerping them smears register config into garbage. */
#define XF_LOAD_MAX 4096
typedef struct { uint16_t addr; uint16_t cnt; uint32_t off; } XfLoad;

/* Scan a stream for validated XF loads into the matrix register file. A
 * candidate must parse (cnt 1..256, addr <0x0800, payload in bounds) AND be
 * followed by a plausible GP opcode — a '10' byte inside vertex/texture
 * payload data rarely passes both checks. Validated candidates are stepped
 * over (a real load's payload can't hold a nested opcode). */
static uint32_t scan_xf_loads(const uint8_t* s, uint32_t len, XfLoad* out, uint32_t max)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i + 5 <= len && n < max; ++i) {
        if (s[i] != 0x10u) continue;
        uint16_t cntm1 = (uint16_t)((s[i + 1] << 8) | s[i + 2]);
        uint16_t addr  = (uint16_t)((s[i + 3] << 8) | s[i + 4]);
        uint32_t cnt = (uint32_t)cntm1 + 1u;
        if (cntm1 == 0u || cntm1 > 0xFFu || addr >= 0x0800u) continue;
        uint32_t end = i + 5u + cnt * 4u;
        if (end > len) continue;
        if (end < len) {
            /* next byte must be a real GP opcode — the FULL valid set. The old
             * list omitted 0x61 (BP, the most common command) + 0x08 (CP) +
             * 0x28/0x30/0x38 (indexed loads), so it rejected every real load
             * that was followed by a BP write — np/nc came back 0. */
            uint8_t op = s[end];
            if (!(op == 0x00u || op == 0x08u || op == 0x10u ||
                  (op >= 0x20u && op <= 0x38u && (op & 7u) == 0u) ||
                  op == 0x40u || op == 0x44u || op == 0x48u || op == 0x61u ||
                  op >= 0x80u))
                continue;
        }
        out[n].addr = addr; out[n].cnt = (uint16_t)cnt; out[n].off = i + 5u; ++n;
        i = end - 1u;
    }
    return n;
}

static float be_f32(const uint8_t* p)
{
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    float f; memcpy(&f, &v, 4); return f;
}
static void wr_be_f32(uint8_t* p, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Patch curr's matrix payloads with lerp(prev,curr,alpha): for every XF load
 * in curr, find the load at the same (addr,cnt) in prev and blend the float
 * payload words. Streams can differ in length — only addr-matched payloads
 * are touched. NaN/huge endpoints are copied verbatim (light/register words
 * that aren't really matrices). Returns count of payload words lerped. */
static uint32_t stream_patch_matrices(const uint8_t* prev, uint32_t plen,
                                      const uint8_t* curr, uint32_t clen,
                                      uint8_t* out, float alpha)
{
    static XfLoad pl[XF_LOAD_MAX], cl[XF_LOAD_MAX];
    uint32_t np = scan_xf_loads(prev, plen, pl, XF_LOAD_MAX);
    uint32_t nc = scan_xf_loads(curr, clen, cl, XF_LOAD_MAX);
    s_dbg_np = np; s_dbg_nc = nc;   /* diagnostic: loads found per stream */
    memcpy(out, curr, clen);
    uint32_t lerped = 0;
    for (uint32_t i = 0; i < nc; ++i) {
        for (uint32_t j = 0; j < np; ++j) {
            if (cl[i].addr != pl[j].addr || cl[i].cnt != pl[j].cnt) continue;
            for (uint32_t k = 0; k < (uint32_t)cl[i].cnt * 4u; k += 4u) {
                float a = be_f32(prev + pl[j].off + k);
                float b = be_f32(curr + cl[i].off + k);
                float am = a < 0 ? -a : a, bm = b < 0 ? -b : b;
                if (a != a || b != b || am > 1.0e7f || bm > 1.0e7f) continue;
                float r = a + alpha * (b - a);
                if (r != r) continue;
                wr_be_f32(out + cl[i].off + k, r);
                ++lerped;
            }
            break;
        }
    }
    return lerped;
}

/* Plausible GP command head — the allow-list shared by region-start
 * alignment and frame-head validation (NOP, CP/XF/BP loads, indexed loads,
 * display-list call, primitives). */
static int gp_head_ok(uint8_t op)
{
    return op == 0x00u || op == 0x08u || op == 0x10u ||
           op == 0x20u || op == 0x28u || op == 0x30u || op == 0x38u ||
           op == 0x40u || op == 0x44u || op == 0x48u || op == 0x61u ||
           (op >= 0x80u && op <= 0xBFu);
}

/* Region starts are not command-aligned: the write-ptr delta cuts wherever
 * the last gather-pipe burst landed, so the first bytes are the tail of a
 * command whose head sat in the previous region's undecoded tail. Skip
 * leading bytes until one parses as a plausible opcode head so the decoder
 * walk resumes at a real command boundary instead of desyncing inside
 * payload (the observed signature: a few unknowns, then a +1B relock).
 * Bounded — if no head shows up the start is deep inside a long payload
 * and position 0 is no worse than anywhere else. */
static uint32_t stream_align_head(const uint8_t* s, uint32_t len)
{
    const uint32_t bound = len < 64u ? len : 64u;
    for (uint32_t i = 0; i < bound; ++i)
        if (gp_head_ok(s[i]))
            return i;
    return 0;
}

/* R-frame entry point, delta-capture design: the fifo bytes written between
 * two consecutive R-frame observations are entirely GAME output — our own
 * injections advance the observation point past themselves via the post-inject
 * re-read, so regions never contain our bytes. A region of frame size is one
 * produced frame; a tiny region is an R-frame gap (only the beginRender
 * micro-stream) and is ignored. This replaces marker scanning entirely: no
 * false 61 52 boundaries, no span bookkeeping, no feedback paths. */
static void fifo_replay_frame(CPUState* st)
{
    uint32_t base, end, wptr;
    if (!fifo_geom(st, &base, &end, &wptr)) { ++s_dbg_nofifo; return; }
    uint32_t fsize = end - base;
    if (!s_stream_curr) {
        s_stream_curr = (uint8_t*)malloc(FIFO_STREAM_MAX);
        s_stream_prev = (uint8_t*)malloc(FIFO_STREAM_MAX);
        /* +64 tail slack: injects are padded to a 32B boundary inside this
         * buffer so frame+pad go out in ONE atomic fifo_inject call. */
        s_stream_out  = (uint8_t*)malloc(FIFO_STREAM_MAX + 64u);
        s_carry       = (uint8_t*)malloc(FIFO_STREAM_MAX * 2u);
    }
    if (!s_stream_curr || !s_stream_prev || !s_stream_out || !s_carry) return;

    if (!s_obs_valid) {
        s_obs_wptr = wptr; s_obs_base = base; s_obs_end = end;
        s_obs_valid = 1; return;
    }

    /* Forward distance from the last observation = bytes written since.
     * The fifo is circular: when wptr wrapped to base it is numerically
     * BELOW the previous observation, so the plain subtraction underflowed
     * to ~4GiB and the old `while (delta >= fsize) delta -= fsize` mod-reduced
     * by the non-pow2 fsize into a garbage region — the abort-loop amplifier.
     * Correct wrap delta: bytes-to-end + bytes-from-base = fsize-(prev-wptr). */
    uint32_t delta = (wptr >= s_obs_wptr)
                     ? wptr - s_obs_wptr
                     : fsize - (s_obs_wptr - wptr);
    uint32_t region_start = s_obs_wptr;
    /* Resync on any stream discontinuity: delta==0 (no writes — covers a
     * reset that parks wptr where it was), the delta>fsize sanity bound
     * (corrupt observation; the old `delta > FIFO_STREAM_MAX` guard was dead
     * code since fsize < MAX), or the fifo geometry moving under us
     * (fifo re-created / abort reset). GXAbortFrame -> __GXCleanGPFifo is the
     * real case: rptr=wptr=base makes the stream non-adjacent to whatever the
     * scanner last saw. Drop the partial carry and mark the scanner carry
     * stale rather than appending a range that was never written post-reset. */
    int resync = (delta == 0 || delta > fsize ||
                  base != s_obs_base || end != s_obs_end ||
                  s_obs_wptr < base || s_obs_wptr >= end);
    s_obs_wptr = wptr; s_obs_base = base; s_obs_end = end;  /* consume regardless */

    /* Regions are NOT frame-aligned: the gather-pipe write pointer lags by a
     * partial 32B burst, so a region can start/end mid-frame. Append the
     * region to the carry buffer and split on the copy-exec signature
     * (61 52 01 — the frame's final command, trigger value stable) to recover
     * exact frame boundaries. The carry holds the partial tail between
     * regions. Regions contain only game bytes — our injections move the
     * observation point past themselves, so no self-feedback is possible. */
    if (resync) {
        s_carry_len = 0;              /* drop partial frame */
        s_scan_resync = 1;            /* engine scanner carry is stale */
        /* Tell the engine to drop its carried scan CP state (s_scan_cp): the
         * next boundary walk re-seeds from the live g_main_cp_state instead
         * of decoding the post-reset region with the pre-reset VAT. Without
         * this the first post-abort scan mis-sizes draws and the relock gate
         * below drops extra regions while the carry self-heals. */
        moderngekko_mod_write(st, MG_SCAN_RESET_EA, 0u, 4u);
        ++s_dbg_nofifo;
    } else {
        if (s_carry_len + delta > FIFO_STREAM_MAX * 2u)
            s_carry_len = 0;          /* can't happen with frame-rate deltas; be safe */
        uint32_t region_off = s_carry_len;   /* where this region lands in carry */
        fifo_capture(st, base, end, region_start, delta, s_carry + s_carry_len);
        s_carry_len += delta;
        s_dbg_capbytes += delta;
        /* extract complete frames using the ENGINE-side GP walker: it decodes
         * real command lengths (vertex sizes via VAT) and reports the byte
         * offset of each 61 52 XFB-copy — the true frame end. The old
         * byte-pattern match failed because 61 52 appears inside payload,
         * cutting frames mid-command and desyncing the replay decoder.
         * Only the NEW region is scanned: the engine keeps CP state continuous
         * across calls, so each region decodes with the VAT carried from the
         * previous (adjacent) one — re-walking the whole carry would seed a
         * stale VAT and desync the draw sizes. A span whose head isn't a real
         * opcode is a mis-aligned false boundary and is skipped. */
        /* Command-align the region start before the walk: the wptr delta
         * cut wherever the last burst landed, so the first bytes are the
         * tail of a command whose head the previous scan dropped. Skipping
         * to a plausible opcode head keeps the decoder from desyncing on a
         * mid-command start (offsets below are relative to the ALIGNED
         * base, so fend adds skip back). */
        uint32_t skip = stream_align_head(s_carry + region_off, delta);
        uint32_t scan_base = region_off + skip;
        uint32_t nb = fifo_scan_boundaries(st, s_carry + scan_base, delta - skip);
        /* Post-reset relock gate: the engine's s_scan_cp VAT carry is stale
         * across a stream discontinuity and is not mod-reachable, but every
         * scan re-bases it on the live stream. Only trust boundaries from a
         * walk that relocked without desyncing (unknowns == 0); otherwise
         * drop the region's bytes and keep waiting for a clean walk — a
         * stale-VAT walk reports false boundaries that corrupt the frame. */
        if (s_scan_resync) {
            if (s_scan_unknowns == 0u)
                s_scan_resync = 0;
            else {
                s_carry_len = 0;
                nb = 0;
            }
        }
        if (s_debug) {
            static uint32_t s_sc_n = 0;
            if (++s_sc_n <= 16u)
                fprintf(stderr, "[sc] n=%u bounds=%u unk=%u first_unk=%u xf=%u fmtx=%u idx=%u dl=%u prim=%u bp=%u roff=%u delta=%u\n",
                        s_sc_n, nb, s_scan_unknowns, s_scan_first_unknown,
                        s_scan_xf, s_scan_xfmtx, s_scan_idx, s_scan_dl,
                        s_scan_prim, s_scan_bp, region_off, delta);
        }
        uint32_t fstart = 0;
        for (uint32_t bi = 0; bi < nb; ++bi) {
            uint32_t fend = scan_base + s_scan_bounds[1 + bi] + 5u;  /* absolute copy end */
            /* Skip out-of-order or beyond-buffered boundaries WITHOUT touching
             * fstart: advancing past s_carry_len would underflow the memmove
             * length below into a giant copy. (Unreachable today — the decoder
             * only reports fully-consumed commands — but stay safe.) */
            if (fend <= fstart || fend > s_carry_len) continue;
            uint32_t flen = fend - fstart;
            if (flen >= FIFO_MIN_FRAME && flen <= FIFO_STREAM_MAX) {
                /* head must be a real GP opcode — a mis-aligned false boundary
                 * leaves a mid-command head that would desync on replay. */
                uint8_t op = s_carry[fstart];
                int ok = gp_head_ok(op);
                if (ok && !(flen == s_stream_curr_len &&
                            memcmp(s_carry + fstart, s_stream_curr, flen) == 0)) {
                    /* new frame content: rotate curr->prev, carry->curr */
                    uint8_t* tb = s_stream_prev; s_stream_prev = s_stream_curr; s_stream_curr = tb;
                    s_stream_prev_len = s_stream_curr_len;
                    memcpy(s_stream_curr, s_carry + fstart, flen);
                    s_stream_curr_len = flen;
                    s_capture_gen++;
                    s_injects_in_pair = 0;   /* new pair: restart the alpha stepping */
                    /* each produced frame earns one injected companion; the
                     * owed pool drains across R-frames at spread alphas so
                     * multi-frame regions still get per-frame interps. */
                    if (s_owed_injects < 8u) ++s_owed_injects;
                    if (s_debug) {
                        static uint32_t s_fr_n = 0;
                        if (++s_fr_n <= 20u || (s_fr_n & 0xFFu) == 0) {
                            fprintf(stderr, "[fr] n=%u gen=%u len=%u head=", s_fr_n, s_capture_gen, flen);
                            for (int k = 0; k < 40 && k < (int)flen; ++k)
                                fprintf(stderr, "%02x", s_stream_curr[k]);
                            fprintf(stderr, " tail=");
                            for (uint32_t k = flen > 8 ? flen - 8 : 0; k < flen; ++k)
                                fprintf(stderr, "%02x", s_stream_curr[k]);
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
            fstart = fend;
        }
        if (fstart) {
            memmove(s_carry, s_carry + fstart, s_carry_len - fstart);
            s_carry_len -= fstart;
        }
    }
    if (s_fifo_verify)
        return;   /* bisect: capture+scan only, skip the inject */
    if (s_owed_injects == 0 || !s_stream_curr_len)
        return;   /* no produced frame has earned a companion yet */

    /* sanity: a real frame starts on a known command opcode; a false-marker
     * boundary (61 52 01 inside payload data) leaves a mid-command head that
     * would desync the decoder on replay — reject those. */
    {
        uint8_t op = s_stream_curr[0];
        int ok = (op == 0x00u || op == 0x61u || op == 0x10u || op == 0x20u ||
                  op == 0x40u || op == 0x45u || op == 0x48u || op == 0x60u ||
                  (op >= 0x80u && op <= 0xB8u) || op == 0x98u || op == 0xA0u ||
                  op == 0xA8u || op == 0xB0u || op == 0xC0u || op == 0xC8u);
        if (!ok) { ++s_dbg_nofifo; return; }
    }

    /* debug bisect modes: single raw inject of curr (or a slice), still
     * drawing down the owed pool so diagnostics stay comparable. */
    if (s_fifo_nop || s_fifo_tail || s_fifo_maxlen) {
        const uint8_t* src = s_stream_curr;
        uint32_t ilen = s_stream_curr_len;
        if (s_fifo_maxlen && ilen > s_fifo_maxlen) ilen = s_fifo_maxlen;
        if (s_fifo_nop) {
            static const uint8_t zeros[64] = {0};
            for (uint32_t i = 0; i < ilen; i += 64)
                fifo_inject(st, zeros, ilen - i < 64 ? ilen - i : 64);
            ++s_dbg_replays;
        } else {
            if (s_fifo_tail && ilen < s_stream_curr_len)
                src = s_stream_curr + (s_stream_curr_len - ilen);
            if (fifo_inject(st, src, ilen)) ++s_dbg_replays;
        }
        uint32_t b2, e2, w2;
        if (fifo_geom(st, &b2, &e2, &w2)) {
            s_obs_wptr = w2; s_obs_base = b2; s_obs_end = e2;
        }
        if (s_owed_injects) --s_owed_injects;
        return;
    }

    /* Production path: drain up to s_fifo_multi owed companions this call.
     * Each inject of the current (prev,curr) pair steps alpha by 0.25 —
     * quarter-point interpolation positions — so consecutive R-frames
     * produce DISTINCT sub-frames instead of re-emitting one pose. An
     * unpatched inject would re-render a pixel-identical frame, so a failed
     * patch ends the burst early (the dup redirect presents the XFB anyway).
     * The owed pool + multi cap bound worst-case fifo occupancy: at most
     * `multi` extra ~frame-size streams per R-frame, near the game's own
     * output rate — without the cap, stacked injects outrun the video
     * thread's drain and CPReadWriteDistance overflows the 640KB fifo. */
    {
        uint32_t n = s_owed_injects < s_fifo_multi ? s_owed_injects : s_fifo_multi;
        for (uint32_t j = 0; j < n; ++j) {
            float alpha = 0.25f * (float)(s_injects_in_pair + 1u);
            if (alpha >= 1.0f) break;      /* alpha=1 is curr itself — nothing new to show */
            if (!s_stream_prev_len) break;
            uint32_t patched = stream_patch_matrices(s_stream_prev, s_stream_prev_len,
                                                     s_stream_curr, s_stream_curr_len,
                                                     s_stream_out, alpha);
            if (!patched) break;
            ++s_dbg_patched;
            /* Strip side-effect commands before the staged frame replays.
             * A captured frame STARTS with the game's post-copy housekeeping
             * — 61 45 (BPMEM_SETDRAWDONE), 61 47/61 48 (PE finish/token
             * triggers) fire a real PE_FINISH IRQ that steals the game's own
             * DrawDone (the abort-loop trigger), and the trailing 61 52
             * copy-exec re-runs an EFB->XFB copy to a stale destination.
             * The copy-exec is dropped outright — if a copy is ever wanted
             * it must be retargeted to the CURRENT XFB, not replayed.
             * The 0x61-prefixed regs are neutralized in place as five NOPs:
             * a false positive inside payload only zeroes 5 data bytes (the
             * enclosing command still consumes them, so the replay walk can
             * never desync), while a real occurrence loses a pure-trigger
             * write that must not replay anyway. */
            uint32_t ilen = s_stream_curr_len;
            if (ilen > 5u && s_stream_out[ilen - 5u] == 0x61u &&
                s_stream_out[ilen - 4u] == 0x52u)
                ilen -= 5u;   /* verified trailing copy-exec: drop */
            else if (s_fifo_nocopy && ilen > 5u)
                ilen -= 5u;   /* legacy bisect knob: trim unverified tail */
            for (uint32_t i = 0; i + 5u <= ilen; ++i) {
                if (s_stream_out[i] == 0x61u &&
                    (s_stream_out[i + 1u] == 0x45u || s_stream_out[i + 1u] == 0x47u ||
                     s_stream_out[i + 1u] == 0x48u || s_stream_out[i + 1u] == 0x52u))
                    memset(s_stream_out + i, 0, 5u);
            }
            /* Zero-pad to a 32B boundary INSIDE the staging buffer (allocated
             * with +64 slack) so frame+pad go out in one atomic inject — the
             * engine's free-space gate then covers both. NOP fill keeps the
             * game's next command out of our last gather-pipe burst. */
            uint32_t padded = (ilen + 31u) & ~31u;
            memset(s_stream_out + ilen, 0, padded - ilen);
            if (!fifo_inject(st, s_stream_out, padded)) { ++s_dbg_nofifo; break; }
            ++s_dbg_replays;
            ++s_injects_in_pair;
            --s_owed_injects;
            /* our stream just became fifo history — re-read wptr so the next
             * delta region starts AFTER our bytes (clean self-excision) */
            uint32_t b2, e2, w2;
            if (fifo_geom(st, &b2, &e2, &w2)) {
                s_obs_wptr = w2; s_obs_base = b2; s_obs_end = e2;
            }
        }
    }
}

/* Generic prev<-curr / curr<-new rotation with dirty + teleport detection,
 * operating on caller-owned buffers (mirrors j3d_history_rotate's rules). */
static void hist_rotate(J3DMtx* prev, J3DMtx* curr, const J3DMtx* newm, uint32_t n,
                        uint32_t* has_prev_io, uint32_t* dirty_io, uint32_t* teleported_io)
{
    if (!prev || !curr || !newm || n == 0) return;
    if (!*has_prev_io) {
        memcpy(curr, newm, (size_t)n * J3D_MTX_BYTES);
        memcpy(prev, curr, (size_t)n * J3D_MTX_BYTES);
        *has_prev_io = 1; *dirty_io = 0; *teleported_io = 0;
        return;
    }
    float dx = newm[0].m[0][3] - curr[0].m[0][3];
    float dy = newm[0].m[1][3] - curr[0].m[1][3];
    float dz = newm[0].m[2][3] - curr[0].m[2][3];
    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
    float hypot = ax + ay + az;
    int is_teleport = (ax > 400.0f || ay > 400.0f || az > 400.0f || hypot > 500.0f) ? 1 : 0;
    if (is_teleport) {
        memcpy(curr, newm, (size_t)n * J3D_MTX_BYTES);
        memcpy(prev, curr, (size_t)n * J3D_MTX_BYTES);
        *teleported_io = 1; *dirty_io = 0;
        return;
    }
    memcpy(prev, curr, (size_t)n * J3D_MTX_BYTES);
    memcpy(curr, newm, (size_t)n * J3D_MTX_BYTES);
    const float eps = 0.02f;
    int dirty = 0;
    for (uint32_t j = 0; j < n && !dirty; ++j)
        for (int r = 0; r < 3 && !dirty; ++r)
            for (int c = 0; c < 4; ++c) {
                float d = newm[j].m[r][c] - prev[j].m[r][c];
                if (d < 0) d = -d;
                if (d > eps) { dirty = 1; break; }
            }
    *dirty_io = dirty ? 1u : 0u;
    *teleported_io = 0;
}

/* Allocate the draw-matrix history for a flag-0/0x10 model. */
static void j3d_history_ensure_draw(J3DHistory* h, uint32_t draw_mtx_num)
{
    if (h->draw_mtx_num == draw_mtx_num && h->prev_draw && h->curr_draw && h->scratch_draw)
        return;
    j3d_free_mtx(h->prev_draw); j3d_free_mtx(h->curr_draw); j3d_free_mtx(h->scratch_draw);
    h->prev_draw = h->curr_draw = h->scratch_draw = 0;
    h->has_prev_draw = 0; h->dirty_draw = 0;
    h->draw_mtx_num = draw_mtx_num;
    if (draw_mtx_num == 0 || draw_mtx_num > J3D_MAX_JOINTS) { h->no_interp = 1u; return; }
    h->prev_draw = j3d_alloc_mtx(draw_mtx_num);
    h->curr_draw = j3d_alloc_mtx(draw_mtx_num);
    h->scratch_draw = j3d_alloc_mtx(draw_mtx_num);
    if (!h->prev_draw || !h->curr_draw || !h->scratch_draw) {
        j3d_free_mtx(h->prev_draw); j3d_free_mtx(h->curr_draw); j3d_free_mtx(h->scratch_draw);
        h->prev_draw = h->curr_draw = h->scratch_draw = 0;
        h->no_interp = 1u;
    }
}

static J3DMtx s_tmp_mtx[J3D_MAX_JOINTS];  /* shared guest-read staging (emu thread only) */

/* B2: J3DModel::calc @0x802EE8C0 — ENTRY hook registers each animated model and
 * caches the consumed matrix-array pointers. calc() runs once per animated model
 * per L-frame during production, so this also discovers models a savestate already
 * had (entryModelData never re-fires for those). Registration only — no return
 * hook: those livelock on hot functions via pending_returns saturation. */
static void on_j3d_calc_entry(CPUState* state)
{
    if (!s_j3d_interp) return;
    if (s_debug && ((++s_dbg_calcent) & 0xFFu) == 0u) j3d_dbg_counts("calcE");
    uint32_t mdl = (uint32_t)state->gpr[3];
    if (!J3D_IN_MEM1(mdl)) return;
    uint32_t modelData = rd32_fast(state, mdl + 0x04u);
    if (!J3D_IN_MEM1(modelData)) return;
    uint32_t skin = rd32_fast(state, mdl + 0xC0u);
    /* Always run the slot through ensure() — not only on first registration.
     * The history table is keyed by guest pointer alone, so an actor-heap
     * address reuse or a savestate restore that skipped the destructor hook
     * can re-point mdl at a model whose joint/envelope counts differ from
     * what the slot was sized for. A stale joint_num turns the node/env
     * inject write into an out-of-bounds guest-heap write. ensure() is a
     * no-op when the layout matches and fully resets the slot (frees and
     * re-sizes the matrix buffers) when it doesn't. */
    uint32_t jointNum = rd16_fast(state, modelData + 0x28u);
    uint32_t wEvlpNum = rd16_fast(state, modelData + 0x30u);
    J3DHistory* h = j3d_history_ensure(mdl, jointNum, wEvlpNum, skin != 0);
    if (!h) return;
    /* Configure the slot whenever it hasn't seen THIS modelData — not only on
     * first registration. B1 entryModelData pre-creates zero-field slots for
     * actors spawned while the mod is loaded; without this, those slots kept
     * flags_f0=0/draw_mtx_num=0 forever -> draw path -> permanent no_interp
     * (only savestate-loaded models ever interpolated). flags_f0==0 is a
     * legal value, so key on model_data. */
    if (h->model_data != modelData) {
        h->model_data = modelData;
        h->flags_f0 = rd32_fast(state, modelData + 0x08u) & 0xF0u;
        h->draw_mtx_num = rd16_fast(state, modelData + 0x44u);
        if (skin) h->no_interp = 1u;
        /* A modelData swap on a live slot leaves the pose pair describing
         * the OLD model's geometry — invalidate it so the next observation
         * re-seeds instead of lerping the stale pose into the new model's
         * arrays (or across a savestate boundary). */
        h->has_prev = 0u; h->dirty = 0u; h->teleported = 0u;
        h->has_prev_env = 0u; h->has_prev_draw = 0u; h->dirty_draw = 0u;
        h->injected = 0u;
    }
    /* Refresh consumed-array pointers every call (cheap; buffers can realloc). */
    h->node_ptr = rd32_fast(state, mdl + 0x8Cu);
    h->env_ptr = rd32_fast(state, mdl + 0x90u);
}

/* Per-array restore probe: if the live array still holds OUR lerp bits
 * (memcmp against the scratch we wrote), production never touched it this
 * L-frame — put curr back so Painter draws the true pose, and observe curr.
 * If it differs, production already overwrote our lerp with the new pose —
 * restoring would clobber it, so observe live. Returns the pose to rotate
 * into history in s_tmp_mtx. */
static void snap_or_restore(CPUState* state, uint32_t arr_ea,
                            J3DMtx* curr, J3DMtx* scratch, uint32_t n)
{
    read_mtx_arr(state, arr_ea, s_tmp_mtx, n);
    if (scratch && memcmp(s_tmp_mtx, scratch, (size_t)n * J3D_MTX_BYTES) == 0) {
        write_mtx_arr(state, arr_ea, curr, n);
        memcpy(s_tmp_mtx, curr, (size_t)n * J3D_MTX_BYTES);
        ++s_rest_models;
        s_rest_writes += n;
    }
}

/* Pose snapshot — L-frame painter-entry only. node/env hold the frame's final
 * pose (calc ran during this frame's production) and mpDrawMtxBuf[1][viewNo]
 * the frame's view matrices (viewCalc ran inside the same production pass);
 * on R-frames production is gated so none of these arrays change — which is
 * why this must NOT run on R-frames: re-observing an unchanged array as a
 * "new" pose rotates prev=curr and clears dirty, permanently disarming the
 * injection for ConcatView (0x20) models.
 * Restore is folded in via snap_or_restore: arrays still holding our lerp get
 * curr back before Painter draws; arrays production rewrote are observed
 * directly — one read per array instead of a separate restore+snapshot pass. */
static void j3d_snapshot_pose(CPUState* state)
{
    ++s_dbg_snapcall;
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        J3DHistory* h = &s_hist[i];
        if (!h->used) continue;
        if (h->no_interp) { ++s_dbg_skipni; continue; }
        /* B1-registered slot awaiting its first calc: model_data is still 0,
         * so flags_f0/draw_mtx_num are unset — the draw path's !dn check
         * would mark it no_interp permanently. Skip until calc configures it. */
        if (!h->model_data) continue;
        /* Stale-slot drop: model memory dead/reused without the dtor hook. */
        if (s_model_alive && !j3d_model_alive(state, h)) { j3d_slot_reset(h); continue; }
        ++s_snap_models;
        const uint32_t was_injected = h->injected;
        h->injected = 0;
        if (h->flags_f0 == 0x20u) {
            /* ConcatView models consume node/env directly at draw. */
            if (!(h->joint_num && h->prev && h->curr && J3D_IN_MEM1(h->node_ptr)))
                continue;
            if (was_injected && h->scratch)
                snap_or_restore(state, h->node_ptr, h->curr, h->scratch, h->joint_num);
            else
                read_mtx_arr(state, h->node_ptr, s_tmp_mtx, h->joint_num);
            hist_rotate(h->prev, h->curr, s_tmp_mtx, h->joint_num,
                        &h->has_prev, &h->dirty, &h->teleported);
            if (h->wEvlp_num && h->prev_env && h->curr_env && J3D_IN_MEM1(h->env_ptr)) {
                uint32_t d = 0, t = 0;
                if (was_injected && h->scratch_env)
                    snap_or_restore(state, h->env_ptr, h->curr_env, h->scratch_env, h->wEvlp_num);
                else
                    read_mtx_arr(state, h->env_ptr, s_tmp_mtx, h->wEvlp_num);
                hist_rotate(h->prev_env, h->curr_env, s_tmp_mtx, h->wEvlp_num,
                            &h->has_prev_env, &d, &t);
                if (d) h->dirty = 1u;
                if (t) h->teleported = 1u;   /* env jump = model jump: snap, don't smear */
            }
        } else {
            /* Double/single-buffered models consume mpDrawMtxBuf[1][viewNo]. */
            uint32_t mdl = h->guest_model_ptr;
            uint32_t dn = h->draw_mtx_num;
            if (!dn || dn > J3D_MAX_JOINTS) { h->no_interp = 1u; continue; }
            uint32_t viewNo = rd32_fast(state, mdl + 0xB0u);
            if (viewNo > 3u) continue;
            uint32_t buf1 = rd32_fast(state, mdl + 0x98u);
            if (!J3D_IN_MEM1(buf1)) continue;
            uint32_t arr = rd32_fast(state, buf1 + viewNo * 4u);
            if (!J3D_IN_MEM1(arr)) continue;
            j3d_history_ensure_draw(h, dn);
            if (!h->prev_draw || !h->curr_draw) continue;
            h->draw_ptr = arr;
            h->draw_buf1 = buf1;
            h->view_no = viewNo;
            if (was_injected && h->scratch_draw)
                snap_or_restore(state, arr, h->curr_draw, h->scratch_draw, dn);
            else
                read_mtx_arr(state, arr, s_tmp_mtx, dn);
            hist_rotate(h->prev_draw, h->curr_draw, s_tmp_mtx, dn,
                        &h->has_prev_draw, &h->dirty_draw, &h->teleported);
        }
    }
}

/* R-frame refresh — observe what production left since the L-frame snapshot.
 * Two writers can have touched the arrays in between: (a) the L-frame's OWN
 * production, which runs AFTER its Painter inside fpcM_Management and writes
 * the NEXT pose into node/env and via viewCalc into buf[1][viewNo]; and
 * (b) dDlst draw callbacks inside Painter's traversal (shadowReal::imageDraw
 * when ungated) which swapDrawMtx()+rewrite buf[1][v]. For every array the
 * rule is the same: still holding OUR lerp (scratch match) is not a new
 * observation; matching curr means nothing was produced; anything else is a
 * fresh pose — rotate it in so inject lerps (last-shown, newly-produced).
 * 0x20 ConcatView models take the node/env branch; buffered models take the
 * buf[1][viewNo] branch. */
/* Refresh-pass nonce: bumped once per j3d_rframe_refresh call and stamped
 * into draw_gen alongside draw_ptr, so j3d_apply_pose can tell "resolved
 * THIS R-frame" (reuse draw_ptr — identical to re-reading buf1[view_no], no
 * guest code ran between the two calls) from "stale/never resolved" (fall
 * back to its own re-resolution — e.g. refresh bailed early or inject ran
 * standalone). Starts at 0 and is always incremented before stamping, so a
 * reset slot's draw_gen==0 can never false-match. */
static uint32_t s_draw_gen = 0;
static void j3d_rframe_refresh(CPUState* state)
{
    ++s_draw_gen;
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        J3DHistory* h = &s_hist[i];
        if (!h->used || h->no_interp || h->teleported) continue;
        if (!h->model_data) continue;
        if (s_model_alive && !j3d_model_alive(state, h)) { j3d_slot_reset(h); continue; }
        if (h->flags_f0 == 0x20u) {
            /* ConcatView: node/env are consumed directly at draw. Between the
             * L-frame snapshot and this R-frame entry the L-frame's OWN
             * production already ran (Painter precedes execute/draw inside
             * fpcM_Management), writing the NEXT pose into them — treat them
             * exactly like the draw buffer below: an array still holding OUR
             * lerp (scratch match, consecutive R-frames) is not a new
             * observation; matching curr means nothing was produced;
             * anything else is a fresh pose — rotate it in so the inject
             * lerps (last-shown, newly-produced) instead of re-showing an
             * older midpoint and clobbering the pose the next L-frame needs. */
            uint32_t np = rd32_fast(state, h->guest_model_ptr + 0x8Cu);
            if (J3D_IN_MEM1(np)) h->node_ptr = np;
            uint32_t ep = rd32_fast(state, h->guest_model_ptr + 0x90u);
            if (J3D_IN_MEM1(ep)) h->env_ptr = ep;
            if (h->joint_num && h->curr && J3D_IN_MEM1(h->node_ptr)) {
                read_mtx_arr(state, h->node_ptr, s_tmp_mtx, h->joint_num);
                int ours = h->injected && h->scratch &&
                    memcmp(s_tmp_mtx, h->scratch, (size_t)h->joint_num * J3D_MTX_BYTES) == 0;
                if (!ours && memcmp(s_tmp_mtx, h->curr, (size_t)h->joint_num * J3D_MTX_BYTES) != 0)
                    hist_rotate(h->prev, h->curr, s_tmp_mtx, h->joint_num,
                                &h->has_prev, &h->dirty, &h->teleported);
            }
            if (h->wEvlp_num && h->curr_env && J3D_IN_MEM1(h->env_ptr)) {
                read_mtx_arr(state, h->env_ptr, s_tmp_mtx, h->wEvlp_num);
                int ours = h->injected && h->scratch_env &&
                    memcmp(s_tmp_mtx, h->scratch_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES) == 0;
                if (!ours && memcmp(s_tmp_mtx, h->curr_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES) != 0) {
                    uint32_t d = 0, t = 0;
                    hist_rotate(h->prev_env, h->curr_env, s_tmp_mtx, h->wEvlp_num,
                                &h->has_prev_env, &d, &t);
                    if (d) h->dirty = 1u;
                    if (t) h->teleported = 1u;
                }
            }
            continue;
        }
        uint32_t dn = h->draw_mtx_num;
        if (!dn || dn > J3D_MAX_JOINTS || !h->curr_draw) continue;
        uint32_t viewNo = rd32_fast(state, h->guest_model_ptr + 0xB0u);
        if (viewNo > 3u) continue;
        uint32_t buf1 = rd32_fast(state, h->guest_model_ptr + 0x98u);
        if (!J3D_IN_MEM1(buf1)) continue;
        uint32_t arr = rd32_fast(state, buf1 + viewNo * 4u);
        if (!J3D_IN_MEM1(arr)) continue;
        h->draw_ptr = arr;
        h->draw_buf1 = buf1;
        h->view_no = viewNo;
        h->draw_gen = s_draw_gen;
        read_mtx_arr(state, arr, s_tmp_mtx, dn);
        if (h->injected) {
            /* Buffer still holding OUR lerp bits is not a new observation —
             * rotating it in would taint curr_draw with our own output and
             * compound the lerp on consecutive R-frames. A foreign writer
             * (in-painter viewCalc) means it was overwritten: clear the flag
             * and observe what it left. */
            if (h->scratch_draw &&
                memcmp(s_tmp_mtx, h->scratch_draw, (size_t)dn * J3D_MTX_BYTES) == 0)
                continue;
            h->injected = 0;
        }
        if (memcmp(s_tmp_mtx, h->curr_draw, (size_t)dn * J3D_MTX_BYTES) != 0)
            hist_rotate(h->prev_draw, h->curr_draw, s_tmp_mtx, dn,
                        &h->has_prev_draw, &h->dirty_draw, &h->teleported);
    }
}

/* R-frame injection. Painter draws the persistent packet list whose
 * shape->drawMtx pointers reference the model arrays; writing
 * lerp(prev,curr,alpha) into them makes the re-draw a unique interpolated
 * frame. Anything production skips stays lerped until the next L-frame's
 * snapshot restores it (scratch-compare), so no separate restore pass runs
 * here. Only dirty models are touched; the writes self-heal via the next
 * production. */
static void j3d_apply_pose(CPUState* state, float alpha)
{
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        J3DHistory* h = &s_hist[i];
        if (!h->used || h->no_interp || h->teleported) continue;
        /* Stale-slot drop before ANY write: model_data==0 means a B1 slot
         * still awaiting calc (legit, skip); model_data set but the model
         * link broken means freed/reused memory — drop so a re-created
         * model re-registers cleanly instead of taking a wild write. */
        if (!h->model_data) continue;
        if (s_model_alive && !j3d_model_alive(state, h)) { j3d_slot_reset(h); continue; }
        if (h->flags_f0 == 0x20u) {
            if (!(h->has_prev && h->dirty)) continue;
            uint32_t did = 0;
            if (h->joint_num && h->prev && h->curr && h->scratch && J3D_IN_MEM1(h->node_ptr)) {
                j3d_lerp_mtx(h->prev, h->curr, alpha, h->scratch, h->joint_num);
                write_mtx_arr(state, h->node_ptr, h->scratch, h->joint_num);
                did += h->joint_num;
            }
            /* has_prev_env required: the env pair seeds on its first real
             * observation — without the gate an unseeded (uninitialized)
             * env buffer would be lerped into a live array. */
            if (h->wEvlp_num && h->has_prev_env && h->prev_env && h->curr_env && h->scratch_env && J3D_IN_MEM1(h->env_ptr)) {
                j3d_lerp_mtx(h->prev_env, h->curr_env, alpha, h->scratch_env, h->wEvlp_num);
                write_mtx_arr(state, h->env_ptr, h->scratch_env, h->wEvlp_num);
                did += h->wEvlp_num;
            }
            if (did) { ++s_inj_models; s_inj_writes += did; h->injected = 1; s_matrices_injected = 1; }
        } else {
            if (!(h->has_prev_draw && h->dirty_draw)) continue;
            if (!h->prev_draw || !h->curr_draw || !h->scratch_draw || !h->draw_mtx_num) continue;
            /* Resolve the CURRENT buf[1][viewNo]: packets dereference this
             * slot at draw, and nothing swaps between Painter entry and the
             * draws (imageDrawShadow stays gated on R-frames). When this
             * R-frame's refresh already resolved the slot (draw_gen stamp),
             * draw_ptr holds the identical word — refresh ran moments ago
             * with no guest code in between — so skip the two-word re-read.
             * Any other case (refresh bailed early, standalone inject) takes
             * the original re-resolution path unchanged. */
            uint32_t arr;
            if (h->draw_gen == s_draw_gen) {
                arr = h->draw_ptr;
            } else {
                uint32_t buf1 = rd32_fast(state, h->guest_model_ptr + 0x98u);
                if (!J3D_IN_MEM1(buf1)) continue;
                arr = rd32_fast(state, buf1 + h->view_no * 4u);
                h->draw_buf1 = buf1;
            }
            if (!J3D_IN_MEM1(arr)) continue;
            j3d_lerp_mtx(h->prev_draw, h->curr_draw, alpha, h->scratch_draw, h->draw_mtx_num);
            write_mtx_arr(state, arr, h->scratch_draw, h->draw_mtx_num);
            ++s_inj_models; s_inj_writes += h->draw_mtx_num;
            h->injected = 1; s_matrices_injected = 1;
        }
    }
}
static void j3d_rframe_inject(CPUState* state, float alpha)
{
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    j3d_apply_pose(state, alpha);
}

/* Camera-cut R-frame cleanup: put every array that still holds OUR lerp back
 * to its CURR endpoint — the pose production last wrote — so Painter's repaint
 * shows the true post-cut frame rather than a stale midpoint. This matters for
 * buffered (view-space) draw matrices in particular: Cam is baked into them,
 * so even a camera-only cut smears their lerp. Foreign-touched arrays are left
 * alone (they hold something newer than curr). Same scratch-match protocol as
 * snap_or_restore; no history rotation — this frame's refresh already has the
 * pair right. */
static void j3d_restore_injected(CPUState* state)
{
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        J3DHistory* h = &s_hist[i];
        if (!h->used || !h->injected) continue;
        h->injected = 0;
        if (!h->model_data) continue;
        if (s_model_alive && !j3d_model_alive(state, h)) { j3d_slot_reset(h); continue; }
        if (h->flags_f0 == 0x20u) {
            if (h->joint_num && h->curr && h->scratch && J3D_IN_MEM1(h->node_ptr)) {
                read_mtx_arr(state, h->node_ptr, s_tmp_mtx, h->joint_num);
                if (memcmp(s_tmp_mtx, h->scratch, (size_t)h->joint_num * J3D_MTX_BYTES) == 0) {
                    write_mtx_arr(state, h->node_ptr, h->curr, h->joint_num);
                    ++s_rest_models; s_rest_writes += h->joint_num;
                }
            }
            if (h->wEvlp_num && h->has_prev_env && h->curr_env && h->scratch_env &&
                J3D_IN_MEM1(h->env_ptr)) {
                read_mtx_arr(state, h->env_ptr, s_tmp_mtx, h->wEvlp_num);
                if (memcmp(s_tmp_mtx, h->scratch_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES) == 0) {
                    write_mtx_arr(state, h->env_ptr, h->curr_env, h->wEvlp_num);
                    ++s_rest_models; s_rest_writes += h->wEvlp_num;
                }
            }
        } else if (h->has_prev_draw && h->curr_draw && h->scratch_draw && h->draw_mtx_num) {
            uint32_t arr;
            if (h->draw_gen == s_draw_gen) {
                arr = h->draw_ptr;   /* refresh already resolved this frame */
            } else {
                const uint32_t buf1 = rd32_fast(state, h->guest_model_ptr + 0x98u);
                if (!J3D_IN_MEM1(buf1)) continue;
                arr = rd32_fast(state, buf1 + h->view_no * 4u);
            }
            if (J3D_IN_MEM1(arr)) {
                read_mtx_arr(state, arr, s_tmp_mtx, h->draw_mtx_num);
                if (memcmp(s_tmp_mtx, h->scratch_draw, (size_t)h->draw_mtx_num * J3D_MTX_BYTES) == 0) {
                    write_mtx_arr(state, arr, h->curr_draw, h->draw_mtx_num);
                    ++s_rest_models; s_rest_writes += h->draw_mtx_num;
                }
            }
        }
    }
    s_matrices_injected = 0;
}

/* ---- camera view/proj interpolation (render+interp mode) -------------------
 * Buffered models' draw matrices bake Cam·W at viewCalc time, so lerping them
 * produces mid-view·mid-world — while ConcatView shapes, particles, the sea,
 * UI/shadow/alpha passes and the light setup all read camera->mViewMtx live
 * inside Painter. Under camera motion the two classes split by half a frame
 * (double image / shear — the reported "blurry/ghosted motion"). Fix: keep a
 * two-deep history of the view Painter itself installs — the camera reached
 * via g_dComIfG_gameInfo.play.mCurrentView (written by
 * dComIfGp_setCurrentView at m_Do_graphic.cpp:1646) — and at R-frame Painter
 * ENTRY write lerp(prev,curr,alpha) into camera->mViewMtx/mProjMtx. Every
 * downstream consumer then shares the mid-view: the JPADrawInfo ctor at :1636,
 * GXSetProjection at :1648, j3dSys.setViewMtx at :1650 (j3dSys.mViewMtx needs
 * no separate write — Painter re-copies it from the camera each run), and the
 * drawShadow/drawAlphaModel/drawSpot passes.
 *
 * Lifetime: view_setup() (0x8017BEB0) and camera_draw() (0x8017C350) rewrite
 * mViewMtx/mProjMtx only inside gated production — the fields are stable
 * across an R-frame and self-heal on the next L-frame, the same contract the
 * J3D arrays use. The L-frame snapshot restores our lerp bits first when the
 * field still holds them (scratch-compare, same protocol as snap_or_restore).
 * Known minor gaps, endpoint-frozen for one sub-frame: mInvViewMtx/mProjViewMtx
 * (cull/proj helpers) and dPa_control_c::mWindViewMatrix. */
#define GAMEINFO_MCURRVIEW 0x803CA900u  /* g_dComIfG_gameInfo.play.mCurrentView
                                         * (0x803C4C08 + 0x12A0 + 0x4A58)      */
#define VIEW_OFF_PROJMTX   0x100u       /* view_class::mProjMtx — Mtx44, 16f32 */
#define VIEW_OFF_VIEWMTX   0x140u       /* view_class::mViewMtx — Mtx, 12 f32  */

/* Cut thresholds: translation-column L1 in world units (same 400 scale the
 * model teleport gate uses) or the rotation 3x3 frobenius past ~90deg — a
 * camera cut/teleport, not a pan. Only the rotation block is frob'd: the
 * translation column alone would swamp the norm on legit long pan moves. */
#define CAM_CUT_EYE     1500.0f         /* L1 delta of reconstructed eye = -R^T t */
#define CAM_CUT_ROTF2   4.0f            /* frob^2 of the 3x3 delta > 4 => ~90deg+ */

typedef struct { float m[4][4]; } Mtx44f;

static J3DMtx  s_cam_prev, s_cam_curr, s_cam_scratch;
static Mtx44f  s_cam_prev_p, s_cam_curr_p, s_cam_scratch_p;
static uint32_t s_cam_has_prev = 0;   /* endpoints seeded (prev/curr valid)   */
/* s_cam_view / s_cam_injected / s_cam_cut are declared with the top state
 * block — on_painter_skip consumes them before this point in the file. */

/* BE float-span movers — same direct-RAM fast path as read/write_mtx_arr,
 * arbitrary count so the 16-float proj and 12-float view share one helper. */
static void rd_f32_arr(CPUState* st, uint32_t addr, float* dst, uint32_t n)
{
    if (arr_in_ram(st, addr, n * 4u)) {
        const uint8_t* p = st->ram + (addr - 0x80000000u);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t w = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
            memcpy(&dst[i], &w, 4u);
        }
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t w = rd32(st, addr + i * 4u);
        memcpy(&dst[i], &w, 4u);
    }
}
static void wr_f32_arr(CPUState* st, uint32_t addr, const float* src, uint32_t n)
{
    if (arr_in_ram(st, addr, n * 4u)) {
        uint8_t* p = st->ram + (addr - 0x80000000u);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t w;
            memcpy(&w, &src[i], 4u);
            p[i*4]   = (uint8_t)(w >> 24);
            p[i*4+1] = (uint8_t)(w >> 16);
            p[i*4+2] = (uint8_t)(w >> 8);
            p[i*4+3] = (uint8_t)w;
        }
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w;
        memcpy(&w, &src[i], 4u);
        moderngekko_mod_write(st, addr + i * 4u, w, 4u);
    }
}

static int camview_finite(const float* f, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (!isfinite(f[i])) return 0;
    return 1;
}

/* Discontinuity test between two observed view matrices. The translation
 * column t = -R*eye can't be used directly: a pure rotation at large eye
 * coordinates swings t by |eye|*angle (measured: playsea/flyover rotations
 * hit tl ~10K-340K while the eye barely moves). Reconstruct the eye
 * position eye = -R^T t and compare in world space instead — real cuts
 * (shot changes) measure 1840-4234 vs <=333 for normal motion. */
static int camview_is_cut(const J3DMtx* a, const J3DMtx* b)
{
    float eye = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float ea = -(a->m[0][i] * a->m[0][3] + a->m[1][i] * a->m[1][3] +
                           a->m[2][i] * a->m[2][3]);
        const float eb = -(b->m[0][i] * b->m[0][3] + b->m[1][i] * b->m[1][3] +
                           b->m[2][i] * b->m[2][3]);
        const float d = eb - ea;
        eye += d < 0.0f ? -d : d;
    }
    if (eye > CAM_CUT_EYE) return 1;
    float frob2 = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const float d = b->m[r][c] - a->m[r][c];
            frob2 += d * d;
        }
    return frob2 > CAM_CUT_ROTF2;
}

/* Feed a new observation into the two-deep history. First obs after a
 * (re)seed collapses BOTH endpoints to obs — same contract as hist_rotate,
 * so a stale pre-reseed curr can never leak into prev and smear frame one.
 * A cut also collapses prev<-curr<-obs so any later lerp evaluates to curr —
 * structurally unable to smear across the jump. Returns nonzero on a cut. */
static int camview_observe(const J3DMtx* ov, const Mtx44f* op)
{
    const int cut = s_cam_has_prev ? camview_is_cut(&s_cam_curr, ov) : 0;
    if (s_camlog && s_cam_has_prev) {
        /* Cut-metric forensics: the translation column t = -R*eye means a
         * pure rotation at large eye coordinates reads as a huge "move".
         * Log the eye-space L1 delta alongside the translation-column one
         * so the real cut signal (shot changes) separates from rotations.
         * BOTH classes are logged — the false-positive cuts live on the
         * R-frame observations, which see a full logic-step delta. */
        const J3DMtx* a = &s_cam_curr;
        const J3DMtx* b = ov;
        float ea[3], eb[3], tl = 0.0f, eye = 0.0f, frob2 = 0.0f;
        for (int i = 0; i < 3; ++i) {
            ea[i] = -(a->m[0][i] * a->m[0][3] + a->m[1][i] * a->m[1][3] +
                      a->m[2][i] * a->m[2][3]);
            eb[i] = -(b->m[0][i] * b->m[0][3] + b->m[1][i] * b->m[1][3] +
                      b->m[2][i] * b->m[2][3]);
            const float dt = b->m[i][3] - a->m[i][3];
            tl += dt < 0.0f ? -dt : dt;
            const float de = eb[i] - ea[i];
            eye += de < 0.0f ? -de : de;
        }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const float d = b->m[r][c] - a->m[r][c];
                frob2 += d * d;
            }
        fprintf(stderr, "[camlog] cls=%c tl=%.1f eye=%.1f frob2=%.4f cut=%d\n",
                s_logic_this_frame ? 'L' : 'R', tl, eye, frob2, cut);
    }
    if (!s_cam_has_prev || cut) {
        s_cam_curr = *ov; s_cam_prev = *ov;
        s_cam_curr_p = *op; s_cam_prev_p = *op;
    } else {
        s_cam_prev = s_cam_curr; s_cam_curr = *ov;
        s_cam_prev_p = s_cam_curr_p; s_cam_curr_p = *op;
    }
    s_cam_has_prev = 1;
    return cut;
}

/* Write the true endpoints back into a camera's fields (cut-skip unpatch /
 * L-frame restore — only ever called on the still-live s_cam_view). */
static void camview_restore(CPUState* st, uint32_t view)
{
    wr_f32_arr(st, view + VIEW_OFF_VIEWMTX, (const float*)&s_cam_curr, 12u);
    wr_f32_arr(st, view + VIEW_OFF_PROJMTX, (const float*)&s_cam_curr_p, 16u);
}

/* Resolve mCurrentView and keep s_cam_view honest — returns the live view
 * pointer or 0. On camera-object change (scene swap) we reseed rather than
 * restore: the old address may already be recycled memory (see below). */
static uint32_t camview_current(CPUState* st)
{
    const uint32_t view = rd32_fast(st, GAMEINFO_MCURRVIEW);
    if (!J3D_IN_MEM1(view)) {
        /* No live view (menus/pre-scene/early boot): drop all state so a
         * stale pointer can never take a write. */
        s_cam_view = 0; s_cam_injected = 0; s_cam_has_prev = 0; s_cam_cut = 0;
        return 0;
    }
    if (view != s_cam_view) {
        /* NOTE: deliberately NOT restoring into the old pointer — a camera
         * change means that address was freed and may already be recycled
         * onto a live object; a write could corrupt it. The stale lerp
         * self-heals via camera_draw the next time that camera produces. */
        s_cam_view = view;
        s_cam_injected = 0;
        s_cam_has_prev = 0;      /* a cross-camera lerp is nonsense: reseed */
    }
    return view;
}

/* L-frame snapshot: restore our patch if the field still holds it, then
 * observe. Runs at Painter entry — before the body reads mViewMtx — so the
 * restored endpoint is what Painter draws under. */
static void camview_lframe(CPUState* st)
{
    const uint32_t view = camview_current(st);
    if (!view) return;
    J3DMtx obs; Mtx44f obs_p;
    rd_f32_arr(st, view + VIEW_OFF_VIEWMTX, (float*)&obs, 12u);
    rd_f32_arr(st, view + VIEW_OFF_PROJMTX, (float*)&obs_p, 16u);
    if (!camview_finite((const float*)&obs, 12u) ||
        !camview_finite((const float*)&obs_p, 16u)) {
        s_cam_has_prev = 0; s_cam_injected = 0;   /* torn-down camera: reseed */
        return;
    }
    if (s_cam_injected) {
        s_cam_injected = 0;
        /* Still OUR bits: production (view_setup/camera_draw) is gated to
         * L-frames, so an unchanged field means the true endpoint is still
         * cam_curr — restore before Painter draws and observe curr. A
         * differing field was rewritten by production (L,L cadence slip):
         * observe live, like snap_or_restore. */
        if (memcmp(&obs, &s_cam_scratch, sizeof(obs)) == 0 &&
            memcmp(&obs_p, &s_cam_scratch_p, sizeof(obs_p)) == 0) {
            camview_restore(st, view);
            obs = s_cam_curr; obs_p = s_cam_curr_p;
        }
    }
    /* Feed EVERY observation — even obs==curr. hist_rotate's contract: an
     * unchanged pose collapses prev<-curr so the pending lerp converges to
     * the endpoint. Keeping a stale (V_{k-1},V_k) pair through a static step
     * would re-show that midpoint on every R-frame until the camera moves
     * again — a permanent half-frame-behind view (jitter). */
    s_cam_cut = camview_observe(&obs, &obs_p);   /* cut -> next R skips interp */
}

/* R-frame refresh: observe what production left between the L-frame snapshot
 * and now (the L-frame's own execute/draw writes the next view after its
 * Painter). Fields still holding OUR lerp are not a new observation; matching
 * curr means nothing was produced; anything else rotates/collapses in and may
 * raise s_cam_cut for this frame's install decision. */
static void camview_rframe(CPUState* st)
{
    const uint32_t view = camview_current(st);
    if (!view) return;
    J3DMtx obs; Mtx44f obs_p;
    rd_f32_arr(st, view + VIEW_OFF_VIEWMTX, (float*)&obs, 12u);
    rd_f32_arr(st, view + VIEW_OFF_PROJMTX, (float*)&obs_p, 16u);
    if (!camview_finite((const float*)&obs, 12u) ||
        !camview_finite((const float*)&obs_p, 16u)) {
        s_cam_has_prev = 0; s_cam_injected = 0;
        return;
    }
    if (s_cam_injected) {
        if (memcmp(&obs, &s_cam_scratch, sizeof(obs)) == 0 &&
            memcmp(&obs_p, &s_cam_scratch_p, sizeof(obs_p)) == 0)
            return;   /* still ours — consecutive R-frame, keep the pair */
        s_cam_injected = 0;   /* a foreign writer overwrote us: observe live */
    }
    if (memcmp(&obs, &s_cam_curr, sizeof(obs)) != 0 ||
        memcmp(&obs_p, &s_cam_curr_p, sizeof(obs_p)) != 0) {
        if (camview_observe(&obs, &obs_p))
            s_cam_cut = 1;    /* consumed by this R-frame's install decision */
    }
}

/* VILOG: the view matrix Painter is about to draw with (after any R-frame
 * install). Logged whole: an element-wise lerped view is not orthonormal, so
 * offline analysis projects fixed world points rather than reconstructing
 * an eye position. */
static void vilog_note_eye(CPUState* st)
{
    const uint32_t view = rd32_fast(st, GAMEINFO_MCURRVIEW);
    if (!J3D_IN_MEM1(view)) {
        memset(s_vl_view_rend, 0, sizeof(s_vl_view_rend));
        return;
    }
    rd_f32_arr(st, view + VIEW_OFF_VIEWMTX, s_vl_view_rend, 12u);
}

/* Install the mid-view: lerp(prev,curr,alpha) into the live camera's
 * mViewMtx/mProjMtx so every Painter consumer shares it. Same elementwise
 * convention + non-finite guard as j3d_lerp_mtx. After a cut prev==curr, so
 * the write is the endpoint — a plain repaint by construction. */
static void camview_install(CPUState* st, float alpha)
{
    if (!s_cam_has_prev || !J3D_IN_MEM1(s_cam_view)) return;
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    j3d_lerp_mtx(&s_cam_prev, &s_cam_curr, alpha, &s_cam_scratch, 1u);
    {
        const float* a = (const float*)&s_cam_prev_p;
        const float* b = (const float*)&s_cam_curr_p;
        float* d = (float*)&s_cam_scratch_p;
        for (int i = 0; i < 16; ++i) {
            const float v = a[i] + alpha * (b[i] - a[i]);
            d[i] = isfinite(v) ? v : b[i];
        }
    }
    wr_f32_arr(st, s_cam_view + VIEW_OFF_VIEWMTX, (const float*)&s_cam_scratch, 12u);
    wr_f32_arr(st, s_cam_view + VIEW_OFF_PROJMTX, (const float*)&s_cam_scratch_p, 16u);
    s_cam_injected = 1;
    ++s_dbg_caminj;
}

/* ========== D4-a foliage composite-matrix lerp (MODERNGEKKO_F60_FOLIAGE_FIX) ==
 * dGrass/dFlower/dTree packet update() bakes j3dSys.getViewMtx()*animMtx into
 * each element's matrix and draw() uploads them via GXLoadPosMtxImm — so on
 * an R-frame the whole field is pinned to the stale L-frame view while
 * terrain/actors draw under the lerped view (visible swim on camera motion).
 * The element arrays are embedded INSIDE the packet object (this = r3 at the
 * draw hook) — all offsets verified against the decomp:
 *
 *   grass  draw @0x80077CDC (symbols.txt:2490): dGrass_packet_c::mGrassData
 *          [1500] @ this+0x14, elem 0x44 — d_grass.h:23-28 (mState@+0x00,
 *          mInitFlags@+0x01, mAnimIdx@+0x02, mItemIdx@+0x03, mModelMtx@+0x10).
 *          Draw walks the room lists and draws iff !(mInitFlags&0x02)
 *          (d_grass.cpp:305-313); update() bakes only mState!=0 unclipped
 *          elements (d_grass.cpp:386-407) — in-list <=> mState!=0.
 *   flower draw @0x800C05DC (symbols.txt:3401): mData[200] @ this+0x14, elem
 *          0x44 — d_flower.h:22-28 (field_0x00 flags, mMtx@+0x10). Drawn iff
 *          !(f0&0x04) in a room list (d_flower.cpp:345,373); update() bakes
 *          only f0&0x02 unclipped elements (d_flower.cpp:455-476).
 *   tree   draw @0x8007960C (symbols.txt:2510): mData[64] @ this+0x14, elem
 *          0x104 — d_tree.h:31-41 (mState@+0x00, mtx @+0x10/+0x40/+0x70/
 *          +0xA0 mShadowMtx/+0xD0). Draw uploads field_0x0d0 (shadow) then
 *          field_0x010/field_0x040 for in-list !(mState&0x04) elements
 *          (d_tree.cpp:407-446); update() bakes those three view composites
 *          only for mState&0x02 unclipped elements (d_tree.cpp:565-594) —
 *          +0x70/+0xA0 are world-space concat INPUTS, never drawn raw.
 *
 * Entry hook: on an L-entry, capture live matrices + validity as the lerp's
 * PREV endpoint (the pose this Painter is about to draw). On an R-entry,
 * live arrays hold the NEXT produced pose; write lerp(prev, live, alpha) for
 * elements valid — and same identity tag — in BOTH snapshots, then restore
 * the live values at the RETURN hook because update() does NOT rewrite
 * clipped elements' matrices (no self-heal like J3D). Camera-cut R-frames
 * (s_fol_cut) and the noinject probe skip the lerp entirely — plain repaint.
 * The tag guards pool reuse: a slot recycled to a different blade/flower/
 * tree between ticks fails the tag match and keeps its live pose.
 * Static host buffers sized to the fixed retail counts — zero per-frame
 * alloc; ~700KB BSS total. */
#define FOL_OFF_DATA 0x14u
enum { FOL_T_GRASS = 0, FOL_T_FLOWER = 1, FOL_T_TREE = 2 };
typedef struct {
    uint32_t self;              /* packet `this` this history is bound to   */
    uint32_t armed;             /* R-frame lerp live -> restore at return   */
    uint32_t count, stride, nmtx, type;
    const uint32_t* moff;       /* per-element matrix offsets               */
    J3DMtx* curr;               /* L-entry snapshot (lerp prev endpoint)    */
    J3DMtx* bak;                /* live values saved for the return restore */
    uint8_t* ok_curr;           /* element was drawable at the L snapshot   */
    uint8_t* inj;               /* element was lerped on this R-entry       */
    uint16_t* tag_curr;         /* identity tag at the L snapshot           */
} FolHist;

static J3DMtx   s_folg_curr[1500], s_folg_bak[1500];
static uint8_t  s_folg_ok[1500], s_folg_inj[1500];
static uint16_t s_folg_tag[1500];
static J3DMtx   s_folf_curr[200], s_folf_bak[200];
static uint8_t  s_folf_ok[200], s_folf_inj[200];
static uint16_t s_folf_tag[200];
static J3DMtx   s_folt_curr[192], s_folt_bak[192];   /* 64 elems x 3 mtx    */
static uint8_t  s_folt_ok[64], s_folt_inj[64];
static uint16_t s_folt_tag[64];
static const uint32_t s_fol_moff1[] = { 0x10u };
static const uint32_t s_fol_moff3[] = { 0x10u, 0x40u, 0xD0u };
static FolHist s_fol_grass  = { 0u, 0u, 1500u, 0x44u,  1u, FOL_T_GRASS,
    s_fol_moff1, s_folg_curr, s_folg_bak, s_folg_ok, s_folg_inj, s_folg_tag };
static FolHist s_fol_flower = { 0u, 0u,  200u, 0x44u,  1u, FOL_T_FLOWER,
    s_fol_moff1, s_folf_curr, s_folf_bak, s_folf_ok, s_folf_inj, s_folf_tag };
static FolHist s_fol_tree   = { 0u, 0u,   64u, 0x104u, 3u, FOL_T_TREE,
    s_fol_moff3, s_folt_curr, s_folt_bak, s_folt_ok, s_folt_inj, s_folt_tag };

/* Drawable-at-draw predicate + identity tag, one call per element.
 * The tag distinguishes "same slot, same owner" from pool reuse: grass uses
 * mAnimIdx|mItemIdx (the blade's animation + grass type — a recycled slot
 * that changes either is a different blade), flower/tree use the two bytes
 * at +0x01/+0x02 (flower f1/f2 rand fields; tree mShadowTableIdx|mAnimIdx —
 * constant for a live tree, different on reuse). */
static int fol_elem_ok(CPUState* st, uint32_t e, uint32_t type, uint16_t* tag)
{
    const uint32_t b0 = rd8_fast(st, e);
    const uint32_t b1 = rd8_fast(st, e + 1u);
    const uint32_t b2 = rd8_fast(st, e + 2u);
    const uint32_t b3 = rd8_fast(st, e + 3u);
    switch (type) {
    case FOL_T_GRASS:   /* mState!=0 && !(mInitFlags&0x02) */
        *tag = (uint16_t)(b2 | (b3 << 8));
        return b0 != 0u && !(b1 & 0x02u);
    case FOL_T_FLOWER:  /* alive bit 0x02, clip bit 0x04 */
        *tag = (uint16_t)(b1 | (b2 << 8));
        return (b0 & 0x02u) && !(b0 & 0x04u);
    default:            /* FOL_T_TREE: alive 0x02, clip 0x04 */
        *tag = (uint16_t)(b2 | (b1 << 8));
        return (b0 & 0x02u) && !(b0 & 0x04u);
    }
}

static void fol_reset(FolHist* h)
{
    h->self = 0u;
    h->armed = 0u;
    memset(h->ok_curr, 0, h->count);
    memset(h->inj, 0, h->count);
    /* mtx/tag need no clearing — ok==0 gates every use */
}

static void fol_entry(CPUState* st, FolHist* h, uint32_t self)
{
    if (!(s_enabled && s_split_mode && s_foliage_fix &&
          s_rframe_render && s_j3d_interp))
        return;
    const uint32_t count = h->count, stride = h->stride;
    if (!in_ram(st, self, FOL_OFF_DATA + count * stride))
        return;
    if (h->self != self) {
        fol_reset(h);
        h->self = self;
    }
    const uint32_t base = self + FOL_OFF_DATA;
    uint32_t i, m;
    if (s_logic_this_frame) {
        /* L draw entry: the array holds exactly the pose this Painter draws
         * (the producing update ran inside the PREVIOUS L iteration).
         * Capture it as the lerp's prev endpoint. */
        for (i = 0; i < count; ++i) {
            const uint32_t e = base + i * stride;
            uint16_t tag = 0;
            const int ok = fol_elem_ok(st, e, h->type, &tag);
            h->ok_curr[i] = (uint8_t)ok;
            h->tag_curr[i] = tag;
            h->inj[i] = 0;
            if (!ok) continue;
            for (m = 0; m < h->nmtx; ++m)
                rd_f32_arr(st, e + h->moff[m],
                           h->curr[i * h->nmtx + m].m[0], 12u);
        }
        return;
    }
    /* R draw entry: live arrays hold the NEXT produced pose (this L-slot's
     * update ran between the L snapshot and now). */
    if (s_r_dup || s_noinject || s_fol_cut)
        return;
    if (s_interp_alpha <= 0.0f || s_interp_alpha >= 1.0f)
        return;
    const float alpha = s_interp_alpha;
    for (i = 0; i < count; ++i) {
        const uint32_t e = base + i * stride;
        uint16_t tag = 0;
        const int ok = fol_elem_ok(st, e, h->type, &tag);
        h->inj[i] = 0;
        if (!(ok && h->ok_curr[i] && tag == h->tag_curr[i]))
            continue;   /* appeared/disappeared/recycled between snapshots */
        for (m = 0; m < h->nmtx; ++m) {
            J3DMtx* b = &h->bak[i * h->nmtx + m];
            J3DMtx lm;
            rd_f32_arr(st, e + h->moff[m], b->m[0], 12u);
            j3d_lerp_mtx(&h->curr[i * h->nmtx + m], b, alpha, &lm, 1u);
            wr_f32_arr(st, e + h->moff[m], lm.m[0], 12u);
        }
        h->inj[i] = 1;
        h->armed = 1;
        ++s_dbg_folinj;
    }
}

static void fol_return(CPUState* st, FolHist* h)
{
    if (!h->armed)
        return;
    h->armed = 0;
    const uint32_t count = h->count, stride = h->stride;
    if (!in_ram(st, h->self, FOL_OFF_DATA + count * stride))
        return;
    const uint32_t base = h->self + FOL_OFF_DATA;
    for (uint32_t i = 0; i < count; ++i) {
        if (!h->inj[i]) continue;
        h->inj[i] = 0;
        const uint32_t e = base + i * stride;
        for (uint32_t m = 0; m < h->nmtx; ++m)
            wr_f32_arr(st, e + h->moff[m], h->bak[i * h->nmtx + m].m[0], 12u);
    }
}

static void on_grass_draw_entry(CPUState* st)
{
    fol_entry(st, &s_fol_grass, (uint32_t)st->gpr[3]);
}
static void on_grass_draw_return(CPUState* st)
{
    fol_return(st, &s_fol_grass);
}
static void on_flower_draw_entry(CPUState* st)
{
    fol_entry(st, &s_fol_flower, (uint32_t)st->gpr[3]);
}
static void on_flower_draw_return(CPUState* st)
{
    fol_return(st, &s_fol_flower);
}
static void on_tree_draw_entry(CPUState* st)
{
    fol_entry(st, &s_fol_tree, (uint32_t)st->gpr[3]);
}
static void on_tree_draw_return(CPUState* st)
{
    fol_return(st, &s_fol_tree);
}

/* ========== D4-b JPA particle position lerp (MODERNGEKKO_F60_JPA_FIX) ========
 * JPABaseParticle::mGlobalPosition (+0x28, 3 f32) is recomputed every calc
 * tick (calcPosition, JPAParticle.cpp ~L204-207) and read by the draw
 * visitors — under the split particles step at 30 Hz. Fix: at each L-frame
 * Painter entry rebuild a host table {addr -> frame, pos} covering every
 * live particle; at the R-frame entry write lerp(L_pos, live_pos, alpha)
 * into mGlobalPosition for entries whose live mCurFrame == stored + 1.0 —
 * incFrame() adds exactly 1.0 per calc tick and pooled reuse restarts at
 * -1.0 (JPAParticle.cpp:88/133 initParticle/initChild, :165 incFrame adds
 * exactly 1.0), so frame continuity IS the identity key: a recycled address
 * can only alias when the L snapshot held frame -1.0 — a corner narrow
 * enough to accept (one-frame midpoint on a just-spawned particle). New
 * particles (miss) and recycled ones (mismatch) are never lerped.
 * Restore: the next L-entry writes the recorded live positions back before
 * Painter draws — the persistent emitter lists are consumed TWICE (R_k and
 * L_{k+1}), so an unrestored lerp would leak into the authoritative L frame.
 * calcPosition() rewrites the field for every live particle each tick
 * anyway, so restore is for exactness, not leak-safety.
 * Traversal: mEmitterMng (dPa_control_c .sbss 0x803F6C28, symbols.txt:18804)
 * -> mEmtrGroup[16] @+0x50 (JPAEmitterManager.h:47; JSUPtrList: mHead@+0,
 * mTail@+4, mLength@+8, JSUList.h:73-75) -> emitters via mLink @+0x90
 * (JPAEmitter.h:393; JSUPtrLink mNext @+0x0C, JSUList.h:31) ->
 * mActiveParticles @+0x17C / mChildParticles @+0x188 (JPAEmitter.h:398-399).
 * Particle link mLink is member +0x00 (JPAParticle.h:91), so link addr ==
 * particle addr; mGlobalPosition @+0x28, mCurFrame @+0x78
 * (JPAParticle.h:92,100). Bounded: 16 groups, <=256 emitters each, <=4096
 * particles per walk; every pointer in_ram-guarded. The table is rebuilt
 * (memset + reseed) at every L entry — ~320KB memset once per 33ms, and it
 * self-heals any stale address forever. */
#define JPA_MGR_EA      0x803F6C28u   /* dPa_control_c::mEmitterMng           */
#define JPA_OFF_GROUPS  0x50u         /* JPAEmitterManager::mEmtrGroup[16]    */
#define JPA_EMTR_LINK   0x90u         /* JPABaseEmitter::mLink                */
#define JPA_EMTR_ACT    0x17Cu        /* JPABaseEmitter::mActiveParticles     */
#define JPA_EMTR_CHLD   0x188u        /* JPABaseEmitter::mChildParticles      */
#define JPA_PTCL_GLOBAL 0x28u         /* JPABaseParticle::mGlobalPosition     */
#define JPA_PTCL_FRAME  0x78u         /* JPABaseParticle::mCurFrame           */
#define JPA_LINK_NEXT   0x0Cu         /* JSUPtrLink::mNext                    */
#define JPA_PTCL_SPAN   0x80u         /* bytes we touch inside a particle     */
#define JPA_MAX_EMTR    256u          /* per-group emitter walk cap           */
#define JPA_MAX_PTCL    4096u         /* total particles per walk             */
#define JPA_HASH_SIZE   8192u
#define JPA_HASH_PROBE  8u

typedef struct {
    uint32_t addr;      /* guest particle address; 0 = empty slot           */
    float    frame;     /* mCurFrame observed at the L snapshot             */
    float    lpos[3];   /* position the last L-frame drew                   */
    float    rpos[3];   /* live position read at the R entry (for restore)  */
    uint8_t  injected;  /* live mGlobalPosition currently holds our lerp    */
} JpaEnt;
static JpaEnt s_jpa[JPA_HASH_SIZE];

static JpaEnt* jpa_lookup(uint32_t addr)
{
    const uint32_t h = ((addr >> 4) * 2654435761u) >> (32u - 13u);
    for (uint32_t p = 0; p < JPA_HASH_PROBE; ++p) {
        JpaEnt* e = &s_jpa[(h + p) & (JPA_HASH_SIZE - 1u)];
        if (e->addr == 0u || e->addr == addr)
            return e;
    }
    return 0;   /* probe chain full — caller leaves live untouched */
}

static void jpa_write_lerp(CPUState* st, uint32_t p,
                           const float* a, const float* b, float alpha)
{
    float out[3];
    for (int k = 0; k < 3; ++k) {
        const float v = a[k] + alpha * (b[k] - a[k]);
        out[k] = isfinite(v) ? v : b[k];   /* same non-finite policy as J3D */
    }
    wr_f32_arr(st, p + JPA_PTCL_GLOBAL, out, 3u);
}

static void jpa_visit(CPUState* st, uint32_t p, int rframe, float alpha)
{
    float pos[3];
    uint32_t fw;
    float frame;
    JpaEnt* e = jpa_lookup(p);
    if (!rframe) {
        /* L walk: (re)seed — the pos this frame draws is the prev endpoint */
        if (!e) return;
        rd_f32_arr(st, p + JPA_PTCL_GLOBAL, pos, 3u);
        fw = rd32_fast(st, p + JPA_PTCL_FRAME);
        memcpy(&frame, &fw, 4u);
        e->addr = p;
        e->frame = frame;
        memcpy(e->lpos, pos, sizeof(pos));
        memcpy(e->rpos, pos, sizeof(pos));
        e->injected = 0;
        return;
    }
    if (!e || e->addr != p)
        return;   /* not in the L snapshot (spawned since / table full) */
    if (e->injected) {
        /* Consecutive R-frame: live still holds our previous lerp — rewrite
         * from the stored pair at the new alpha. Never read live as rpos. */
        jpa_write_lerp(st, p, e->lpos, e->rpos, alpha);
        return;
    }
    fw = rd32_fast(st, p + JPA_PTCL_FRAME);
    memcpy(&frame, &fw, 4u);
    if (frame != e->frame + 1.0f)
        return;   /* addr recycled between ticks — not the same particle */
    rd_f32_arr(st, p + JPA_PTCL_GLOBAL, pos, 3u);
    memcpy(e->rpos, pos, sizeof(pos));      /* save live for the L restore  */
    e->frame = frame;
    e->injected = 1;
    ++s_dbg_jpainj;
    jpa_write_lerp(st, p, e->lpos, e->rpos, alpha);
}

static void jpa_walk(CPUState* st, int rframe, float alpha)
{
    const uint32_t mgr = rd32_fast(st, JPA_MGR_EA);
    if (!in_ram(st, mgr, JPA_OFF_GROUPS + 16u * 0x0Cu))
        return;
    static const uint32_t pl_off[2] = { JPA_EMTR_ACT, JPA_EMTR_CHLD };
    uint32_t visited = 0;
    for (uint32_t g = 0; g < 16u; ++g) {
        const uint32_t lbase = mgr + JPA_OFF_GROUPS + g * 0x0Cu;
        uint32_t elink = rd32_fast(st, lbase);
        uint32_t en = rd32_fast(st, lbase + 8u);
        if (en > JPA_MAX_EMTR) en = JPA_MAX_EMTR;
        for (uint32_t ei = 0; ei < en && in_ram(st, elink, JPA_LINK_NEXT + 4u);
             ++ei) {
            const uint32_t emtr = elink - JPA_EMTR_LINK;
            elink = rd32_fast(st, elink + JPA_LINK_NEXT);
            if (!in_ram(st, emtr, JPA_EMTR_CHLD + 0x0Cu))
                continue;   /* emitter must span both particle lists */
            for (int li = 0; li < 2; ++li) {
                uint32_t p = rd32_fast(st, emtr + pl_off[li]);
                uint32_t pn = rd32_fast(st, emtr + pl_off[li] + 8u);
                if (pn > JPA_MAX_PTCL) pn = JPA_MAX_PTCL;
                for (uint32_t pi = 0; pi < pn && visited < JPA_MAX_PTCL &&
                                   in_ram(st, p, JPA_PTCL_SPAN); ++pi) {
                    const uint32_t next = rd32_fast(st, p + JPA_LINK_NEXT);
                    jpa_visit(st, p, rframe, alpha);
                    ++visited;
                    p = next;
                }
            }
        }
    }
}

/* Write every outstanding lerp back to its particle — must run BEFORE the
 * table is wiped (jpa_lframe rebuild, f60_reset_runtime_state) or the live
 * mGlobalPosition fields keep our midpoint on an authoritative L frame. */
static void jpa_restore(CPUState* st)
{
    for (uint32_t i = 0; i < JPA_HASH_SIZE; ++i) {
        JpaEnt* e = &s_jpa[i];
        if (e->injected) {
            e->injected = 0;
            if (in_ram(st, e->addr, JPA_PTCL_SPAN))
                wr_f32_arr(st, e->addr + JPA_PTCL_GLOBAL, e->rpos, 3u);
        }
    }
}

static void jpa_lframe(CPUState* st)
{
    if (!s_jpa_fix) return;
    /* Restore any lerped positions still outstanding BEFORE Painter re-draws
     * the persistent emitter lists (they are consumed twice: R_k then
     * L_{k+1}). Nothing ran between the R inject and now, so the recorded
     * addrs still name the same particles; the in_ram guard covers a torn-
     * down manager anyway. */
    jpa_restore(st);
    memset(s_jpa, 0, sizeof(s_jpa));
    jpa_walk(st, 0, 0.0f);
}

static void jpa_rframe(CPUState* st, float alpha)
{
    if (!s_jpa_fix) return;
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    jpa_walk(st, 1, alpha);
}

/* ---- M1: shared runtime-state reset -----------------------------------------
 * Called on the split engage/disengage edges and from on_unload. Clears every
 * per-frame mutable static so a stale snapshot/lerp/armed-flag can never leak
 * across a mode transition or a mod reload. Does NOT touch config knobs
 * (s_enabled, *_fix, s_j3d_interp, meters config), owned allocations
 * (s_hist buffers, stream bufs — freed in on_unload), or the monotonic
 * s_hist_gen/s_purge_epoch counters used to invalidate J3D slots. With a
 * live CPUState it also seeds s_list_heap from guest mCurrentHeap (H1). */
static void f60_reset_runtime_state(CPUState* st)
{
    /* Undo outstanding guest-memory overrides BEFORE clearing the flags that
     * track them — a disengage edge can land between an R-frame inject and
     * the next L-frame restore, and zeroing the flags alone would leave the
     * lerped matrices / lerped camera / overridden fader colour live. */
    if (st) {
        if (s_fdr_r_pending && s_fdr_self && in_ram(st, s_fdr_self, 0x10u))
            wr32_fast(st, s_fdr_self + 0x0Cu, s_fdr_r_saved);
        if (s_matrices_injected)
            j3d_restore_injected(st);
        /* Only write the camera back if it is still mCurrentView — a swapped
         * view address may already be recycled onto a live object. */
        if (s_cam_injected && s_cam_view && J3D_IN_MEM1(s_cam_view) &&
            rd32_fast(st, GAMEINFO_MCURRVIEW) == s_cam_view)
            camview_restore(st, s_cam_view);
    }
    s_split_mode = 0;
    s_logic_this_frame = 1;
    s_interp_alpha = 0.0f;
    s_acc = 0;
    s_last_delta = 0;
    s_prev_frame_class = 1;
    s_l_delta = 0;
    s_pair_pad = 0;
    s_vi_locked = 0;
    s_first = 0;
    s_jfw_display = 0;
    s_overlap_active = 0;
    s_r_dup = 0;
    s_cam_injected = 0;
    s_cam_cut = 0;
    s_cam_view = 0;
    s_cam_has_prev = 0;
    s_fol_cut = 0;
    s_wptr_prev = 0;
    s_wptr_fsize = 0;
    s_wptr_valid = 0;
    s_pd_bytes = 0;
    s_render_conf = 0;
    s_rd_wptr0 = 0;
    s_rd_bytes = 0;
    s_painter_entry_wptr = 0;
    s_fcdw_entry_wptr = 0;
    s_painter_bytes = 0;
    s_fcdw_bytes = 0;
    s_split_valid = 0;
    s_prev_xfb_dest = 0;
    s_stream_dump_n = 0;
    s_dlt_n = 0;
    s_rng_lcount = 0;
    s_fade_snap = 0;
    s_wipe_snap = 0;
    s_fdr_l_drew = 0;
    s_fdr_l_color = 0;
    s_fdr_r_pending = 0;
    s_fdr_r_saved = 0;
    s_fdr_self = 0;
    s_fdl_self = 0;
    s_fdl_hold = 0;
    s_dbg_fader_calls = 0;
    s_sea_snap = 0;
    s_sea_self = 0;
    s_sea_orig = 0;
    s_light_snap = 0;
    s_light_pt = 0;
    s_matrices_injected = 0;
    s_draw_gen = 0;
    s_dlchk_budget = 0;
    s_dlchk_draws = 0;
    s_trace_prev_drawn = -2;
    s_capture_gen = 0;
    s_owed_injects = 0;
    s_injects_in_pair = 0;
    s_obs_wptr = 0;
    s_obs_valid = 0;
    s_obs_base = 0;
    s_obs_end = 0;
    s_stream_prev_len = 0;
    s_stream_curr_len = 0;
    s_carry_len = 0;
    s_scan_resync = 0;
    s_scan_unknowns = 0;
    s_scan_first_unknown = 0;
    s_scan_xf = 0;
    s_scan_dl = 0;
    s_scan_prim = 0;
    s_scan_bp = 0;
    s_scan_xfmtx = 0;
    s_scan_idx = 0;
    s_light_mask = 0;
    s_fade_flag = s_fade_rate = s_fade_color = 0;
    s_wipe_flag = s_wipe_rate = s_wipe_scrolls = s_wipe_scrollt = 0;
    s_wipe_xlu = 0;
    s_cost_prev_ns = s_cost_prev_tb = 0;
    s_cost_prev_valid = 0;
    s_cost_l_ns = s_cost_r_ns = s_cost_o_ns = 0;
    s_cost_l_tb = s_cost_r_tb = s_cost_o_tb = 0;
    s_cost_l_n = s_cost_r_n = s_cost_o_n = 0;
    s_wl_ns = s_wr_ns = s_wo_ns = 0;
    s_wl_tb = s_wr_tb = s_wo_tb = 0;
    s_wl_n = s_wr_n = s_wo_n = 0;
    s_c_snap_ns = s_c_snap_n = 0;
    s_c_refr_ns = s_c_refr_n = 0;
    s_c_inj_ns = s_c_inj_n = 0;
    s_c_frep_ns = s_c_frep_n = 0;
    s_dbg_jfast = s_dbg_jmiss = 0;
    s_dbg_lframes = s_dbg_rframes = 0;
    s_dbg_snapcall = s_dbg_skipni = 0;
    s_dbg_replays = s_dbg_capbytes = s_dbg_patched = s_dbg_nofifo = 0;
    s_dbg_np = s_dbg_nc = 0;
    s_dbg_purged = 0;
    s_dbg_ovr_dup = s_dbg_w0_dup = s_dbg_camcut = s_dbg_caminj = 0;
    s_dbg_lightgate = 0;
    s_dbg_gp_dup = s_dbg_wnum_dup = s_dbg_cap_dup = 0;
    s_dbg_lightfix = s_dbg_lightbad = 0;
    s_dbg_fader_calls = 0;
    s_dbg_folinj = s_dbg_jpainj = 0;
    s_dbg_entrymd = s_dbg_calcent = s_dbg_calcret = 0;
    s_dbg_vcent = s_dbg_vcret = s_dbg_dtor = 0;
    fol_reset(&s_fol_grass);
    fol_reset(&s_fol_flower);
    fol_reset(&s_fol_tree);
    /* Restore any lerped particle positions before dropping the table —
     * a disengage edge can land between an R inject and the next L entry. */
    if (st) jpa_restore(st);
    memset(s_jpa, 0, sizeof(s_jpa));
    if (st)
        s_list_heap = rd8_fast(st, GINF_MCURRHEAP) & 1u;
    else
        s_list_heap = 0;
}

/* budget constants for TB-14 */
static const uint32_t J3D_BUDGET_TYPICAL_KIB = 140; /* 1500*96≈144000 ≈140.6 rounded */
static const uint32_t J3D_BUDGET_HEAVY_KIB = 300;   /* 3200*96=307200 */

/* Optional: export to let a future 60Hz-logic sweep (Option C) query
 * whether this frame is logic or render-only, or to let QA sample
 * the accumulator at runtime. */
static void frame60_is_logic_frame(CPUState* state)
{
    moderngekko_mod_return_u32(state, s_logic_this_frame);
}

/* B1: J3DModel::entryModelData @0x802ED6C4 — model setup; r3=this, r4=J3DModelData*.
 * Allocates history early (init-only hook, fires once per model). */
static void on_entryModelData(CPUState* state)
{
    if (s_debug && ((++s_dbg_entrymd) & 0xFFu) == 0u) j3d_dbg_counts("entryMD");
    if (!s_j3d_interp) return;
    uint32_t model_ptr = (uint32_t)state->gpr[3];
    uint32_t modelData = (uint32_t)state->gpr[4];
    if (!J3D_IN_MEM1(model_ptr) || !J3D_IN_MEM1(modelData)) return;
    uint32_t jointNum = rd16_fast(state, modelData + 0x28u);
    uint32_t wEvlpNum = rd16_fast(state, modelData + 0x30u);
    (void)j3d_history_ensure(model_ptr, jointNum, wEvlpNum, 0);
}
/* B3: J3DModel::~J3DModel @0x802ED5AC — free */
static void on_j3d_dtor(CPUState* state)
{
    if (s_debug && ((++s_dbg_dtor) & 0xFFu) == 0u) j3d_dbg_counts("dtor");
    if (!s_j3d_interp) return;
    uint32_t model_ptr = (uint32_t)state->gpr[3];
    if (model_ptr) j3d_history_free(model_ptr);
}

/* ---- judge-cluster fast path -------------------------------------------------
 * PC sampling shows the process-list judge cluster owning ~65% of guest
 * cycles in heavy scenes: cNdIt_Judge (0x80244F44) walks a node_class list
 * (next at +0x08) calling pJudge(node,ud) per node; when pJudge is
 * cTgIt_JudgeFilter (0x80245640) each visit costs a second indirect dispatch
 * into filter->mpJudgeFunc(tag->mpTagData, filter->mpUserData). The dominant
 * inner judges are pure-read leaf comparators — fpcSch_JudgeForPName
 * (0x80040050: s16 node->f8 == s16 *ud) and fpcSch_JudgeByID
 * (0x80040068: u32 node->f4 == u32 *ud) — so the whole call reduces to a
 * native walk with identical guest-visible semantics (the walkers/judges
 * write nothing). pNext is fetched BEFORE the judge call exactly like the
 * original, and any unexpected shape (unknown judge, non-MEM1 pointer,
 * cyclic/corrupt list) falls through without touching pc so the original
 * function runs. */
/* NOTE: 0x80244F44's hot callers are intra-chunk `bl`s, which the emitter
 * lowers to `goto`s — a chassis-level hook can never intercept them. The
 * generated tree therefore carries an inline host-call check at
 * label_80244F44 (dol-inline-opt/chunks/chunk_0144_text1_802416E0.c), the
 * same idiom DOLRECOMP_HOST_HOOKS emits. Any module REGEN must include
 * 80244F44 in DOLRECOMP_HOST_HOOKS or this fast path silently goes dead.
 * The libm fast path below needs the same treatment at the sin/cos entries:
 * HOST_HOOKS must also list 8033071C (cos) + 80330C84 (sin) — the current
 * tree has them hand-patched into chunk_0203_text1_8032D6E0.c.
 * REGEN should also pass DOLRECOMP_GPR_STUBS=80328F04:80328F4C,80328F50:80328F98
 * (emitter inlines the savegpr/restgpr run-in bodies; the current tree got the
 * same transform via bench-out/inline_gpr_stubs.py). */
#define CTGIT_JUDGEFILTER 0x80245640u
#define FPCSCH_JUDGE_FOR_PNAME 0x80040050u  /* lha r5,8(r3); lha r0,0(r4); cmpw */
#define FPCSCH_JUDGE_BY_ID     0x80040068u  /* lwz r5,4(r3); lwz r0,0(r4); cmplw */
#define JUDGE_WALK_CAP 8192u   /* lists run ~hundreds; cap guards corrupt cycles */

/* The judge walk runs ~10K node-visits/frame — too hot for the
 * external_read path; uses the shared in_ram/rd*_ram helpers. */
static uint32_t j3d_judge_walk(CPUState* s, uint32_t node, int via_filter,
                             int is_name, uint32_t wanted, uint32_t foff)
{
    uint32_t next;
    if (node && !in_ram(s, node + 8u, 4u)) return 0xFFFFFFFFu;
    next = node ? rd32_ram(s, node + 8u) : 0;
    for (uint32_t it = 0; node; ) {
        if (++it > JUDGE_WALK_CAP) return 0xFFFFFFFFu;
        if (!in_ram(s, node + 0x0Cu, 4u)) return 0xFFFFFFFFu;
        uint32_t cand = node;
        uint32_t field_ea;
        if (via_filter) {
            cand = rd32_ram(s, node + 0x0Cu);   /* create_tag->mpTagData */
            field_ea = cand + foff;
        } else {
            field_ea = node + foff;
        }
        if (!in_ram(s, field_ea, is_name ? 2u : 4u)) return 0xFFFFFFFFu;
        const uint32_t fv = is_name
            ? (uint32_t)(int16_t)rd16_ram(s, field_ea)
            : rd32_ram(s, field_ea);
        if (fv == wanted)
            return cand;
        node = next;
        if (node && !in_ram(s, node + 8u, 4u)) return 0xFFFFFFFFu;
        next = node ? rd32_ram(s, node + 8u) : 0;
    }
    return 0;
}

static void on_cNdIt_Judge(CPUState* state)
{
    if (!s_judge_fast) return;
    const uint32_t node0 = state->gpr[3];
    const uint32_t judge = state->gpr[4];
    const uint32_t ud = state->gpr[5];
    uint32_t inner, iud;
    int via_filter;
    if (judge == CTGIT_JUDGEFILTER) {
        if (!J3D_IN_MEM1(ud)) { ++s_dbg_jmiss; return; }
        inner = rd32_fast(state, ud);            /* filter->mpJudgeFunc */
        iud   = rd32_fast(state, ud + 4u);       /* filter->mpUserData */
        if ((inner != FPCSCH_JUDGE_BY_ID && inner != FPCSCH_JUDGE_FOR_PNAME) ||
            !J3D_IN_MEM1(iud)) { ++s_dbg_jmiss; return; }
        via_filter = 1;
    } else if (judge == FPCSCH_JUDGE_BY_ID || judge == FPCSCH_JUDGE_FOR_PNAME) {
        inner = judge;
        iud = ud;
        if (!J3D_IN_MEM1(iud)) { ++s_dbg_jmiss; return; }
        via_filter = 0;
    } else {
        ++s_dbg_jmiss;
        return;
    }
    /* ForPName: cmpw on lha@+8 vs s16 *ud. ByID: cmplw on lwz@+4 vs u32 *ud.
     * Equality is width-preserving under sign-extension, so comparing the
     * sign-extended s16 (or raw u32) reproduces both branches exactly. */
    const int is_name = (inner == FPCSCH_JUDGE_FOR_PNAME);
    const uint32_t wanted = is_name ? (uint32_t)(int16_t)rd16_fast(state, iud)
                                    : rd32_fast(state, iud);
    const uint32_t foff = is_name ? 8u : 4u;
    const uint32_t r = j3d_judge_walk(state, node0, via_filter, is_name, wanted, foff);
    if (r == 0xFFFFFFFFu) { ++s_dbg_jmiss; return; }   /* anomaly: original runs */
    state->gpr[3] = r;
    /* The original's last CR write is `cmplwi r3,0` (unsigned: EQ or GT). */
    {
        uint32_t cr0 = (r == 0) ? 0x2u : 0x4u;
        cr0 |= (state->xer >> 31) & 1u;
        state->cr = (state->cr & ~(0xFu << 28)) | (cr0 << 28);
    }
    state->pc = state->lr;
    ++s_dbg_jfast;
}

/* L1: guest libm leaf calls (sin 0x80330C84, cos 0x8033071C — ~54-instr
 * double-precision polynomials each, ~1.3% of guest cycles in pcsamp).
 * Cross-chunk `bl` callers dispatch through the chassis, so the hook fires
 * per call: compute with host libm, write f1, return via lr.
 * MEASURED NET-NEGATIVE (2026-09-20): enabling the claim path cost ~8-9%
 * VI Hz — the host-call round-trip exceeds the small guest bodies' cost.
 * Kept behind opt-in MODERNGEKKO_LIBM_FAST=1 for future re-tests; the
 * generated-tree host-call sites were reverted, so this is inert anyway. */
#define GCLIBM_SIN 0x80330C84u
#define GCLIBM_COS 0x8033071Cu
static void on_libm(CPUState* state)
{
    if (!s_libm_fast) return;
    const uint32_t pc = state->pc;
    if (pc == GCLIBM_SIN) state->fpr[1] = sin(state->fpr[1]);
    else if (pc == GCLIBM_COS) state->fpr[1] = cos(state->fpr[1]);
    else return;
    state->pc = state->lr & ~3u;
}

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK(0x80255D34u, on_wait_for_tick),
    RECOMP_HOOK(0x8003E370u, on_execute_gate), /* fpcM_Execute (per-process)    */
    RECOMP_HOOK(0x800231BCu, on_after_gate),   /* fapGm_After (managers)        */
    RECOMP_HOOK(0x80007224u, on_aud_gate),     /* mDoAud_Execute (C2 audio)     */
    RECOMP_HOOK(0x800078C0u, on_cpad_gate),    /* mDoCPd_Read (C3 input)        */
    RECOMP_HOOK(0x8003D314u, on_void_logic_gate), /* fpcDt_Handler (delete queue)  */
    RECOMP_HOOK(0x8003FF00u, on_true_logic_gate), /* fpcPi_Handler (priority queue) */
    RECOMP_HOOK(0x8003D150u, on_true_logic_gate), /* fpcCt_Handler (create queue)   */
    RECOMP_HOOK(0x802449ACu, on_void_logic_gate), /* cCt_Counter (frame counters)   */
    RECOMP_HOOK(0x802C85F0u, on_fader_gate),      /* F1 JUTFader::control -> draw   */
    RECOMP_HOOK(0x802C86F0u, on_fader_draw_seen), /* F1 draw entry: record L colour */
    RECOMP_HOOK_RETURN(0x802C85F0u, on_fader_return), /* F1 ret: post-control fadelog */
    RECOMP_HOOK(0x8015C75Cu, on_sea_draw_entry),  /* F-1 daSea_packet_c::draw ctr   */
    RECOMP_HOOK_RETURN(0x8015C75Cu, on_sea_draw_return),
    RECOMP_HOOK(0x80007FE8u, on_calcfade_entry),  /* F2 calcFade snapshot           */
    RECOMP_HOOK_RETURN(0x80007FE8u, on_calcfade_return),
    RECOMP_HOOK(0x800866F0u, on_calcwipe_entry),  /* F3 calcWipe snapshot           */
    RECOMP_HOOK_RETURN(0x800866F0u, on_calcwipe_return),
    RECOMP_HOOK(0x80008354u, on_void_logic_gate), /* F4 calcMonotone (pure state)   */
    /* F5: mDoGph_gInf_c heap cadence — implemented inside on_painter_skip
     * (pin mCurrentHeap to s_list_heap at entry; flip s_list_heap on
     * L-frames). A dedicated mid-function hook at 0x8000AFE8 was tried and
     * livelocks the guest — only function-entry/call-site hooks are safe. */
    RECOMP_HOOK(0x8000AF2Cu, on_painter_skip),  /* D1 Painter entry -> beginRender  */
    RECOMP_HOOK(0x800404CCu, on_fcdw_gate),       /* D2 fpcDw_Handler (BOOL, ignored) */
    RECOMP_HOOK(0x80255570u, on_void_render_gate), /* D3 exchangeXfb_double */
    RECOMP_HOOK(0x802557C0u, on_void_render_gate), /* D4 endGX */
    /* D5-D11: leaf draw-submission funnels. Duplicate mode: all gated, EFB
     * retains the last logic frame (D3 also gated) and VI re-presents it.
     * Render mode (MODERNGEKKO_RFRAME_RENDER): all pass through so Painter
     * re-draws the persistent packet list — except imageDrawShadow, which
     * stays gated (it calls viewCalc with a shadow viewMtx; gating it keeps
     * the scene matrix buffers' parity intact and saves the shadow re-render). */
    RECOMP_HOOK(0x802ECCE4u, on_drawbuffer_gate),   /* J3DDrawBuffer::draw — chain preflight + gate */
    RECOMP_HOOK(0x80086570u, on_void_render_gate), /* dDlst_list_c::draw (2D/copy) */
    RECOMP_HOOK(0x80084EF0u, on_void_render_gate), /* dDlst_shadowControl_c::draw */
    RECOMP_HOOK(0x80084DECu, on_void_logic_gate),  /* dDlst_shadowControl_c::imageDraw */
    RECOMP_HOOK(0x80082F9Cu, on_true_render_gate), /* dDlst_alphaModel_c::draw (BOOL) */
    RECOMP_HOOK(0x80008880u, on_void_render_gate), /* drawAlphaBuffer */
    RECOMP_HOOK(0x8007D16Cu, on_void_render_gate), /* dPa_control_c::draw (particles) */
    RECOMP_HOOK(0x80244F44u, on_cNdIt_Judge),      /* J1 cNdIt_Judge list-walk fast path */
    RECOMP_HOOK(0x80194BDCu, on_dky_setlight_gate),/* F-2 dKy_setLight: R-frame relight     */
    RECOMP_HOOK_RETURN(0x80194BDCu, on_dky_setlight_return), /* F-2 restore write set    */
};

/* Opt-in hooks: registered only when their feature is armed. A registered
 * address makes every chassis-dispatched call into it take the host-call
 * round-trip even when the callback declines, which is pure overhead for the
 * diagnostics and for sin/cos (called constantly; LIBM_FAST is off by
 * default because that round-trip outweighs their bodies). */
static const ModernGekkoModHook hooks_diag[] = {
    RECOMP_HOOK(0x80323D50u, on_gxcopydisp),     /* trace/XFBLUM/NOCOPYCLEAR: EFB->XFB copy */
    RECOMP_HOOK(0x80255F60u, on_clear_efb),      /* trace: clearEfb          */
};
static const ModernGekkoModHook hooks_vilog[] = {
    RECOMP_HOOK(0x802C7E30u, on_vi_retrace),     /* JUTVideo::preRetraceProc  */
};
static const ModernGekkoModHook hooks_libm[] = {
    RECOMP_HOOK(0x80330C84u, on_libm),             /* L1 sin -> host libm  */
    RECOMP_HOOK(0x8033071Cu, on_libm),             /* L2 cos -> host libm  */
};

/* Stage-B interp hooks — registered only when MODERNGEKKO_J3D_INTERP=1.
 * Entry hooks only: RECOMP_HOOK_RETURN livelocks on hot functions (calc/viewCalc
 * run ~155-3000x/frame) because stale pending_returns saturate the per-block
 * HandlesAddress scan. Registration/snapshot use entry + once-per-frame hooks. */
static const ModernGekkoModHook hooks_interp[] = {
    RECOMP_HOOK(0x802ED6C4u, on_entryModelData),        /* B1 model setup         */
    RECOMP_HOOK(0x802EE8C0u, on_j3d_calc_entry),        /* B2 calc entry (register)*/
    RECOMP_HOOK(0x802ED5ACu, on_j3d_dtor),              /* B3 dtor free           */
    /* D4 foliage packet draws — ~1 call/frame each, so paired return hooks
     * are safe (unlike the calc/viewCalc hot path). Entry captures/lerps,
     * return restores the live composite matrices. */
    RECOMP_HOOK(0x80077CDCu, on_grass_draw_entry),      /* dGrass_packet_c::draw  */
    RECOMP_HOOK_RETURN(0x80077CDCu, on_grass_draw_return),
    RECOMP_HOOK(0x800C05DCu, on_flower_draw_entry),     /* dFlower_packet_c::draw */
    RECOMP_HOOK_RETURN(0x800C05DCu, on_flower_draw_return),
    RECOMP_HOOK(0x8007960Cu, on_tree_draw_entry),       /* dTree_packet_c::draw   */
    RECOMP_HOOK_RETURN(0x8007960Cu, on_tree_draw_return),
};

/* Stale-entry purge: JKR free funnels, split into separately-armable groups.
 * `delete` routes through operator delete -> static JKRHeap::free(ptr,heap)
 * -> member free() — the tail calls are intra-chunk, so each funnel needs
 * its own entry hook. */
static const ModernGekkoModHook hooks_purge_free[] = {
    RECOMP_HOOK(0x802B0D28u, on_jkr_opdel),             /* operator delete        */
    RECOMP_HOOK(0x802B0D4Cu, on_jkr_opdel),             /* operator delete[]      */
    RECOMP_HOOK(0x802B0518u, on_jkr_free_static),       /* JKRHeap::free(ptr,heap)*/
    RECOMP_HOOK(0x802B0560u, on_jkr_free_member),       /* JKRHeap::free(ptr)     */
};
static const ModernGekkoModHook hooks_purge_bulk[] = {
    RECOMP_HOOK(0x802B0634u, on_jkr_bulk),              /* JKRHeap::freeAll       */
    RECOMP_HOOK(0x802B069Cu, on_jkr_bulk),              /* JKRHeap::freeTail      */
    RECOMP_HOOK(0x802B0408u, on_jkr_bulk),              /* JKRHeap::destroy       */
};

static const ModernGekkoModExportEntry exports[] = {
    RECOMP_EXPORT("frame60_is_logic_frame", frame60_is_logic_frame),
};

/* on_unload — free every host-side allocation and reset runtime state so a
 * reload starts from a clean table. ModManager::Unload calls this BEFORE it
 * clears hooks/pending_returns, and no guest code runs between the callback
 * and the clear, so nothing here can be re-entered post-free. The J3D
 * history buffers and the FIFO stream buffers are the only heap state the
 * mod owns; the guest-side arrays they mirror belong to the game and are
 * untouched. Config (s_enabled/s_j3d_interp/…) is env-derived and left for
 * on_load to re-derive on reload. */
static void frame60_accum_on_unload(void)
{
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        J3DHistory* h = &s_hist[i];
        j3d_free_mtx(h->prev); j3d_free_mtx(h->curr); j3d_free_mtx(h->scratch);
        j3d_free_mtx(h->prev_env); j3d_free_mtx(h->curr_env); j3d_free_mtx(h->scratch_env);
        j3d_free_mtx(h->prev_draw); j3d_free_mtx(h->curr_draw); j3d_free_mtx(h->scratch_draw);
    }
    memset(s_hist, 0, sizeof(s_hist));
    s_hist_used = 0;
    s_hist_gen = 1;
    s_purge_epoch = 0;

    free(s_stream_prev); s_stream_prev = 0;
    free(s_stream_curr); s_stream_curr = 0;
    free(s_stream_out);  s_stream_out = 0;
    free(s_carry);       s_carry = 0;
    /* M2: the lazy STREAM_DUMP staging buffer was owned by the mod too —
     * leaving it allocated across unload was a straight leak. */
    free(s_stream_dump_buf); s_stream_dump_buf = 0;

    /* M1: every remaining per-frame static — fader/sea/light snapshots, fifo
     * diagnostics, split trackers, foliage/JPA history — via the shared
     * reset so unload can never drift ahead of the mode-transition path. */
    f60_reset_runtime_state(0);
}

static ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GZLE01",
    "frame60-accum",
    "0.1.0",
    "Wind Waker 60Hz Loop — 30Hz logic accumulator (Option B, EXIT-1 gated) + Stage-B J3D history",
    0, 0u,
    0, 0u,
    hooks, (uint32_t)(sizeof(hooks) / sizeof(hooks[0])),
    exports, 1u,
    0, 0u,
    0, 0u,
    0, 0u,
    frame60_accum_on_load,
    frame60_accum_on_unload,
};

static ModernGekkoModHook hooks_active[
    sizeof(hooks) / sizeof(hooks[0]) +
    sizeof(hooks_diag) / sizeof(hooks_diag[0]) +
    sizeof(hooks_vilog) / sizeof(hooks_vilog[0]) +
    sizeof(hooks_libm) / sizeof(hooks_libm[0]) +
    sizeof(hooks_interp) / sizeof(hooks_interp[0]) +
    sizeof(hooks_purge_free) / sizeof(hooks_purge_free[0]) +
    sizeof(hooks_purge_bulk) / sizeof(hooks_purge_bulk[0])];

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc* moderngekko_get_mod(void)
{
    frame60_accum_on_load(0);
    uint32_t n = 0;
    if (s_enabled) {
        uint32_t i;
        for (i = 0; i < (uint32_t)(sizeof(hooks) / sizeof(hooks[0])); ++i)
            hooks_active[n++] = hooks[i];
        if (s_trace || s_xfb_lum_enabled || s_nocopyclr || s_vilog)
            for (i = 0; i < (uint32_t)(sizeof(hooks_diag) / sizeof(hooks_diag[0])); ++i)
                hooks_active[n++] = hooks_diag[i];
        if (s_vilog)
            for (i = 0; i < (uint32_t)(sizeof(hooks_vilog) / sizeof(hooks_vilog[0])); ++i)
                hooks_active[n++] = hooks_vilog[i];
        if (s_libm_fast)
            for (i = 0; i < (uint32_t)(sizeof(hooks_libm) / sizeof(hooks_libm[0])); ++i)
                hooks_active[n++] = hooks_libm[i];
        if (s_j3d_interp) {
            for (i = 0; i < (uint32_t)(sizeof(hooks_interp) / sizeof(hooks_interp[0])); ++i)
                hooks_active[n++] = hooks_interp[i];
            if (s_purge_free)
                for (i = 0; i < (uint32_t)(sizeof(hooks_purge_free) / sizeof(hooks_purge_free[0])); ++i)
                    hooks_active[n++] = hooks_purge_free[i];
            if (s_purge_bulk)
                for (i = 0; i < (uint32_t)(sizeof(hooks_purge_bulk) / sizeof(hooks_purge_bulk[0])); ++i)
                    hooks_active[n++] = hooks_purge_bulk[i];
        }
    }
    descriptor.hooks = hooks_active;
    descriptor.num_hooks = n;
    return &descriptor;
}
