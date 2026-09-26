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
static uint32_t s_shadow_interp = 1;  /* MODERNGEKKO_F60_SHADOW_INTERP — shadow matrix lerp (D5)  */
static uint32_t s_texanim = 1;        /* MODERNGEKKO_F60_TEXANIM — J3D material-anim DL payload lerp    */
static uint32_t s_jpa_full = 1;       /* MODERNGEKKO_F60_JPA_FULL — full draw-state lerp (rot/scale/
                                       * alpha/color/emitter); =0 keeps position-only behavior        */
static uint32_t s_cloth_interp = 1;   /* MODERNGEKKO_F60_CLOTH_INTERP — CPU-cloth vertex lerp           */
static uint32_t s_fadelog = 0;        /* MODERNGEKKO_F60_FADELOG — fade-state forensics           */
static uint32_t s_rnglog = 0;         /* MODERNGEKKO_F60_RNGLOG — cM_rnd state per L-frame        */
static uint32_t s_rng_lcount = 0;     /* L-frames logged by RNGLOG (capped at 400)                */
static uint32_t s_dltlog = 0;         /* MODERNGEKKO_F60_DLTLOG — per-iter tick delta + class     */
static uint32_t s_dlt_n = 0;          /* iterations logged by DLTLOG (capped at 3000)             */
static uint32_t s_camlog = 0;         /* MODERNGEKKO_F60_CAMLOG — view-matrix cut diagnostics     */
static uint32_t s_purge_epoch_always = 0; /* MODERNGEKKO_F60_PURGE_EPOCH_ALWAYS — legacy bump     */
static uint32_t s_force_else = 0;     /* MODERNGEKKO_F60_FORCE_ELSE — L-frames take else-branch */
static uint32_t s_prio_boost = 1;     /* MODERNGEKKO_F60_PRIORITY — 0=off 1=above-normal 2=highest */
static int      s_prio_applied = 0;   /* emu thread priority currently raised */
static uint32_t s_dirty_exact = 1;    /* MODERNGEKKO_F60_DIRTY_EXACT — endpoint-diff dirty gate */
static uint32_t s_sparse_wr = 1;      /* MODERNGEKKO_F60_SPARSE_WR — write only changed 48B mtx */
static uint32_t s_ovlp_gate_peek = 1; /* MODERNGEKKO_F60_OVLP_PEEK — phase-aware dup narrowing   */
static uint32_t s_mdllog = 0;         /* MODERNGEKKO_F60_MDLLOG — per-model coverage dump        */
static uint32_t s_overlap_active = 0; /* fopOvlpM overlap in flight — R-frames take dup present  */
static uint32_t s_ovlp_peek = 0;      /* overlap request mIsPeek (+0x08) sampled this iteration —
                                       * 1 while the cover/peek (WaitOfFadeout) phase is active */
static uint32_t s_ovlp_phase = 0;     /* request mPhs.id (+0x1C): 0-3 scene live, >=4 teardown    */
static uint32_t s_ovlp_peek_dbg = 0;  /* last peek value the debug line reported                 */
static uint64_t s_dbg_ovr_rep = 0;    /* R-frames repainted while overlap active (peek==0)       */
static uint32_t s_mdllog_l = 0;       /* L-frames since the last MDLLOG coverage dump            */
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
static uint64_t s_dbg_ta_sight = 0, s_dbg_ta_patch = 0, s_dbg_ta_reparse = 0;

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
static uint64_t s_c_jpa_ns  = 0, s_c_jpa_n  = 0;  /* jpa_lframe + jpa_rframe  */
static uint64_t s_c_cam_ns  = 0, s_c_cam_n  = 0;  /* camview L + R frame work */
static uint64_t s_c_fol_ns  = 0, s_c_fol_n  = 0;  /* foliage entry + return   */
static uint64_t s_c_lit_ns  = 0, s_c_lit_n  = 0;  /* dKy_setLight relight     */
static uint64_t s_hook_calls = 0, s_hook_calls_w = 0; /* mod hook invocations */

/* ---- emu-thread priority boost (MODERNGEKKO_F60_PRIORITY) ------------------
 * The measured failure mode of the governor is scheduling starvation, not
 * compute: the L+R pair's real host work is a few ms per 33.3ms window, so
 * only host preemption can stretch an iteration enough to convict a window
 * at GOV_SLOW. One notch of priority lets the emu thread reclaim its bursts
 * against NORMAL-priority contenders (other runners, browsers); the in-
 * process GPU/video thread stays at base priority and never backed up in
 * practice (vs_qd ~ 0 across the bench matrix), so the bump is safe. Applied
 * once per split engagement — mod hooks run on the emu thread — and restored
 * on disengage/unload. 0=off (kill switch), 1=ABOVE_NORMAL (default),
 * 2=HIGHEST for experiments. Raw externs keep <windows.h> out of this TU
 * (same pattern as QueryPerformanceCounter above); constants restated from
 * WinBase.h. */
#if defined(_WIN32)
extern void* GetCurrentThread(void);
extern int   SetThreadPriority(void* hThread, int nPriority);
#define MG_PRIO_ABOVE_NORMAL 1
#define MG_PRIO_HIGHEST      2
#define MG_PRIO_NORMAL       0
static void emu_prio_apply(void)
{
    if (s_prio_applied || !s_prio_boost) return;
    const int lvl = (s_prio_boost >= 2u) ? MG_PRIO_HIGHEST : MG_PRIO_ABOVE_NORMAL;
    if (SetThreadPriority(GetCurrentThread(), lvl)) {
        s_prio_applied = 1;
        if (s_debug)
            fprintf(stderr, "[f60] emu thread priority boosted (lvl=%d)\n", lvl);
    }
}
static void emu_prio_restore(void)
{
    if (!s_prio_applied) return;
    SetThreadPriority(GetCurrentThread(), MG_PRIO_NORMAL);
    s_prio_applied = 0;
}
#else
static void emu_prio_apply(void)   {}
static void emu_prio_restore(void) {}
#endif
static uint64_t s_c_shd_ns  = 0, s_c_shd_n  = 0;  /* shd_ctrl_draw_entry (L+R) */
static uint64_t s_c_ta_ns = 0, s_c_ta_n = 0;      /* on_end_diff scan (L)     */
static uint64_t s_c_ta2_ns = 0, s_c_ta2_n = 0;    /* texanim_rframe     (R)   */
static uint64_t s_c_clo_ns  = 0, s_c_clo_n  = 0;  /* cloth entry+R-frame (L+R) */

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
        const char* si = getenv("MODERNGEKKO_F60_SHADOW_INTERP");
        if (si) s_shadow_interp = (si[0] != '0');
        const char* ta = getenv("MODERNGEKKO_F60_TEXANIM");
        if (ta) s_texanim = (ta[0] != '0');
        const char* jpf = getenv("MODERNGEKKO_F60_JPA_FULL");
        if (jpf) s_jpa_full = (jpf[0] != '0');
        const char* co = getenv("MODERNGEKKO_F60_CLOTH_INTERP");
        if (co) s_cloth_interp = (co[0] != '0');
        const char* fe = getenv("MODERNGEKKO_F60_FORCE_ELSE");
        if (fe && fe[0] == '1')
            s_force_else = 1;
        const char* pr = getenv("MODERNGEKKO_F60_PRIORITY");
        if (pr && pr[0])
            s_prio_boost = (uint32_t)strtoul(pr, 0, 0);
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
        /* characters lane knobs (wave2): DIRTY_EXACT=1 makes the J3D pose
         * history dirty iff any observed endpoint element differs — the old
         * 0.02f whole-model dead-zone (an idle-pose/slow-motion freeze) is
         * kept behind =0 for A/B. SPARSE_WR=1 bounds guest matrix writes to
         * the 48B blocks whose lerp differs from what the live array already
         * holds; =0 restores the full-array write (same visible output).
         * OVLP_PEEK=1 narrows the overlap dup gate to the request's teardown
         * phases (mPhs.id >= 4 — the scene-swap boundary where frees can
         * land); =0 restores dup-for-the-whole-request. */
        const char* de = getenv("MODERNGEKKO_F60_DIRTY_EXACT");
        if (de) s_dirty_exact = (de[0] != '0');
        const char* sw = getenv("MODERNGEKKO_F60_SPARSE_WR");
        if (sw) s_sparse_wr = (sw[0] != '0');
        const char* op = getenv("MODERNGEKKO_F60_OVLP_PEEK");
        if (op) s_ovlp_gate_peek = (op[0] != '0');
        const char* ml = getenv("MODERNGEKKO_F60_MDLLOG");
        if (ml) s_mdllog = (ml[0] == '1');
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
static void j3d_mdl_dump(void);
static void camview_lframe(CPUState* state);
static void camview_rframe(CPUState* state);
static void camview_install(CPUState* state, float alpha);
static void camview_restore(CPUState* state, uint32_t view);
static void jpa_lframe(CPUState* state);
/* D5 shadow interp — defined in the D5 section; on_void_render_gate call-outs. */
#define SHD_CTRL_DRAW    0x80084EF0u   /* dDlst_shadowControl_c::draw      */
#define SHD_CTRL_SETSMPL 0x80085274u   /* dDlst_shadowControl_c::setSimple */
static void shd_ctrl_draw_entry(CPUState* st);
static void on_shd_setsimple(CPUState* st);
static void on_shd_draw_return(CPUState* st);
static void jpa_rframe(CPUState* state, float alpha);
static void texanim_lframe(CPUState* state);
static void texanim_rframe(CPUState* state, float alpha);
static void texanim_restore(CPUState* state);
static void on_end_diff(CPUState* state);
static void cloth_painter_entry(CPUState* state);
static void cloth_rframe(CPUState* state, float alpha);
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
static uint64_t s_dbg_shdinj;         /* shadow entries lerped on R-frames (D5)             */
static uint64_t s_dbg_shdsnap;        /* shadow snapshots taken on L-frames                 */
static uint64_t s_dbg_jpaeinj;        /* JPA emitter states lerped on R-frames              */
static uint64_t s_cloth_dbg_lerp, s_cloth_dbg_skip; /* cloth packets lerped / warp-skipped */
static uint64_t s_cloth_dbg_dol;                  /* DOL draw entries gated OK on R */
static uint64_t s_cloth_dbg_rel;                  /* rel packets classified on R    */
static uint64_t s_cloth_dbg_bas;                  /* resolved-base bitmask (OR'd)   */
static uint64_t s_cloth_dbg_pkt;                  /* drawbuf packets visited on R   */
static uint32_t s_cloth_dbg_vt0;                  /* last unmatched vptr seen on R  */
static uint64_t s_cloth_dbg_dhit;                 /* DOL draw hook raw hits         */
static uint64_t s_cloth_dbg_nodes;                /* DMC nodes walked (diag)        */
static uint64_t s_cloth_dbg_linked;               /* linked nodes seen              */
static uint32_t s_cloth_dbg_dumped;               /* one-shot name dump done        */
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
        emu_prio_restore();
        s_split_mode = 0;
        s_logic_this_frame = 1;
        s_interp_alpha = 0.0f;
        s_acc = 0;
        s_first = 0;
        s_overlap_active = 0;
        s_ovlp_peek = 0;
        s_ovlp_phase = 0;
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
        emu_prio_apply();
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
        const uint32_t req = rd32_fast(state, 0x803F6160u);
        const uint32_t ovr = (req != 0u) ? 1u : 0u;
        /* overlap_request_class layout (decomp-verified):
         *   +0x08 mIsPeek — set every tick in phase_WaitOfFadeout, cleared
         *         when the task's mRq completes in phase_IsWaitOfFadeout;
         *   +0x1C mPhs.id — the request's phase index into phaseMethod[8]:
         *         0 Create, 1 IsCreated, 2 IsComplete, 3 WaitOfFadeout,
         *         4 IsWaitOfFadeout, 5 IsDone, 6 Done.
         * The phases split the request into exactly the two regimes the
         * dup gate cares about. Phases 0-3 run while the OLD scene is
         * still alive and being presented — during 3 (peek) the cover task
         * draws the old scene under the fade, so repainting interpolates
         * real on-screen content (this is the window the whole-request dup
         * used to freeze). Phases 4-6 begin once the task's mRq reports
         * done: the requester proceeds to swap scenes, i.e. teardown
         * frees + scene-init allocates start hitting the persistent
         * packet list / J3D arrays — repaint there walks dangling data
         * (observed: two stalls on ovlphang-d7 / cold-boot with repaint
         * active at peek==0). So the safe criterion is the PHASE, not the
         * peek bit: repaint iff phase <= 3; dup iff phase >= 4 or the
         * request can't be proven readable. The bit/phase only advance
         * inside fapGm_After on L-frames, so the sample is stable across
         * the whole R-frame. */
        uint32_t phase = 7u;
        if (ovr) {
            if (!in_ram(state, req, 0x20u)) {
                phase = 7u;                       /* can't prove safe -> teardown side */
                s_ovlp_peek = 1u;
            } else {
                phase = rd32_fast(state, req + 0x1Cu);
                if (phase > 7u) phase = 7u;
                s_ovlp_peek = rd32_fast(state, req + 0x08u) != 0u ? 1u : 0u;
            }
        } else {
            s_ovlp_peek = 0u;
        }
        if (s_debug && (ovr != s_overlap_active ||
                        (ovr && (s_ovlp_peek != s_ovlp_peek_dbg || phase != s_ovlp_phase))))
            fprintf(stderr, "[f60] overlap %s peek=%u ph=%u — R-frames %s\n",
                    ovr ? "engaged" : "cleared", (unsigned)s_ovlp_peek,
                    (unsigned)phase,
                    (ovr && (!s_ovlp_gate_peek || phase >= 4u))
                        ? "-> dup present" : "-> repaint");
        s_overlap_active = ovr;
        s_ovlp_peek_dbg = s_ovlp_peek;
        s_ovlp_phase = phase;
    }
    if (s_debug) {
        if (s_logic_this_frame) ++s_dbg_lframes; else ++s_dbg_rframes;
        if (((s_dbg_lframes + s_dbg_rframes) & 0x07u) == 0) {
            fprintf(stderr, "[f60] L=%llu R=%llu (%.1f%% L) acc=%llu inj=%llu/%llu res=%llu/%llu snap=%llu trk=%u snapcall=%llu skipni=%llu rep=%llu capKB=%llu pat=%llu nofifo=%llu np=%llu nc=%llu ovr=%llu\n",
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
                    (unsigned long long)s_dbg_nc,
                    (unsigned long long)s_dbg_ovr_rep);
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
                fprintf(stderr, "[f60] jf=%llu jm=%llu prg=%llu ovrR=%llu w0d=%llu capd=%llu gpd=%llu cut=%llu camI=%llu lg=%llu wnum=%u pd=%u rd=%u rc=%u xm=%u xi=%d,%d,%d lst=%u,%u,%u,%u cam=%08X pk=%08X,%08X,%08X pb=%u fb=%u fol=%llu jpa=%llu/%llu ta=%llu/%llu/%llu cl=%llu,%llu\n",
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
                    (unsigned long long)s_dbg_jpainj,
                    (unsigned long long)s_dbg_jpaeinj,
                    (unsigned long long)s_dbg_ta_sight,
                    (unsigned long long)s_dbg_ta_patch,
                    (unsigned long long)s_dbg_ta_reparse,
                    (unsigned long long)s_cloth_dbg_lerp,
                    (unsigned long long)s_cloth_dbg_skip);
                fprintf(stderr, "[f60-cloth] dhit=%llu dol=%llu rel=%llu bas=%llu pkt=%llu vt0=%08X nd=%llu lk=%llu\n",
                    (unsigned long long)s_cloth_dbg_dhit,
                    (unsigned long long)s_cloth_dbg_dol,
                    (unsigned long long)s_cloth_dbg_rel,
                    (unsigned long long)s_cloth_dbg_bas,
                    (unsigned long long)s_cloth_dbg_pkt,
                    (unsigned)s_cloth_dbg_vt0,
                    (unsigned long long)s_cloth_dbg_nodes,
                    (unsigned long long)s_cloth_dbg_linked);
                fprintf(stderr, "[f60] shd_inj=%llu shd_snap=%llu\n",
                    (unsigned long long)s_dbg_shdinj,
                    (unsigned long long)s_dbg_shdsnap);
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
    ++s_hook_calls;
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
    ++s_hook_calls;
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* H3: fapGm_After 0x800231BC — scene/overlay/camera manager gate.
 * Same R-frame skip; keeps managers at retail 30 Hz cadence. */
static void on_after_gate(CPUState* state)
{
    ++s_hook_calls;
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* C2: mDoAud_Execute 0x80007224 — JAI control pump gate (av-sync).
 * Same R-frame skip; prevents double-pump of sequence/stream retarget
 * when MODERNGEKKO_FRAME60_ACCUM=1. Latent under default passthrough. */
static void on_aud_gate(CPUState* state)
{
    ++s_hook_calls;
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* C3: mDoCPd_Read 0x800078C0 — pad sample gate (input polling).
 * Same R-frame skip; prevents 60Hz PADRead while logic runs 30Hz
 * (would double-poll and miss triggers). Latent under default passthrough. */
static void on_cpad_gate(CPUState* state)
{
    ++s_hook_calls;
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

static void on_void_logic_gate(CPUState* state)
{
    ++s_hook_calls;
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
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    ++s_hook_calls;
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
    if (s_cost) { s_c_lit_ns += host_now_ns() - t0; ++s_c_lit_n; }
}

static void on_dky_setlight_return(CPUState* state)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    ++s_hook_calls;
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
    if (s_cost) { s_c_lit_ns += host_now_ns() - t0; ++s_c_lit_n; }
}

static void on_true_logic_gate(CPUState* state)
{
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
        "snap=%.1fus(%llu) refr=%.1fus(%llu) inj=%.1fus(%llu) frep=%.1fus(%llu) shd=%.1fus(%llu) "
        "tadiff=%.1fus(%llu) tapatch=%.1fus(%llu) "
        "jpaw=%.1fus(%llu) "
        "clo=%.1fus(%llu)\n",
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
        (unsigned long long)s_c_frep_n,
        s_c_shd_n ? (double)s_c_shd_ns / (double)s_c_shd_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_shd_n,
        s_c_ta_n ? (double)s_c_ta_ns / (double)s_c_ta_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_ta_n,
        s_c_ta2_n ? (double)s_c_ta2_ns / (double)s_c_ta2_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_ta2_n,
        s_c_jpa_n ? (double)s_c_jpa_ns / (double)s_c_jpa_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_jpa_n,
        s_c_clo_n ? (double)s_c_clo_ns / (double)s_c_clo_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_clo_n);
    /* Second line: the finer R/L mod-work buckets + hook-call volume. The
     * buckets together bound "mod host work"; frame wall minus their sum is
     * guest code + dispatch + fifo/gpu waits (separate attribution). */
    fprintf(stderr,
        "[f60-cost2] jpa=%.1fus(%llu) cam=%.1fus(%llu) fol=%.1fus(%llu) "
        "lit=%.1fus(%llu) hooks=%llu/win\n",
        s_c_jpa_n ? (double)s_c_jpa_ns / (double)s_c_jpa_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_jpa_n,
        s_c_cam_n ? (double)s_c_cam_ns / (double)s_c_cam_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_cam_n,
        s_c_fol_n ? (double)s_c_fol_ns / (double)s_c_fol_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_fol_n,
        s_c_lit_n ? (double)s_c_lit_ns / (double)s_c_lit_n / 1.0e3 : 0.0,
        (unsigned long long)s_c_lit_n,
        (unsigned long long)(s_hook_calls - s_hook_calls_w));
    s_hook_calls_w = s_hook_calls;
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
    if (s_debug)
        fprintf(stderr, "[f60-gov] speed=%.3f dup=%u probe=%u\n",
                speed, (unsigned)g->degraded, (unsigned)g->probing);
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
    ++s_hook_calls;
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
    /* Cloth index-flip restore + sail snapshot must run before ANY Painter
     * body below: an R-frame's flipped mCurArr/m1C3A has to be back at its
     * real value before this iteration's execute samples the "old" buffer
     * (Painter runs before execute inside fpcM_Management), and the DOL
     * draw-hook arm is a leak-safety net. Cheap: zero work unless a flip is
     * outstanding or an L-frame sail packet exists. */
    if (s_cost) {
        const uint64_t t0 = host_now_ns();
        cloth_painter_entry(state);
        s_c_clo_ns += host_now_ns() - t0; ++s_c_clo_n;
    } else {
        cloth_painter_entry(state);
    }
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
    /* Every draw-consuming Painter entry: any DL payload lerp still live from
     * the previous R-frame goes back to its curr endpoint BEFORE this frame's
     * replay reads it (L repaint must be authoritative; R paths re-patch or
     * stay clean below). Runs for dup/noinject paths too — the bytes belong
     * to the mod only until this point. */
    texanim_restore(state);
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
            if (s_cam_interp) {
                const uint64_t tc = s_cost ? host_now_ns() : 0;
                camview_lframe(state);
                if (s_cost) { s_c_cam_ns += host_now_ns() - tc; ++s_c_cam_n; }
            }
            /* JPA: restore any lerped particle positions before Painter
             * re-draws the persistent list (the list is consumed twice —
             * the R-frame's inject must not leak into this authoritative
             * frame), then snapshot live positions as the lerp's L endpoint.
             * Runs at Painter ENTRY, before any particle draw this frame. */
            {
                const uint64_t tj = s_cost ? host_now_ns() : 0;
                jpa_lframe(state);
                if (s_cost) { s_c_jpa_ns += host_now_ns() - tj; ++s_c_jpa_n; }
            }
            texanim_lframe(state);   /* bump the endDiff sighting generation */
            s_fol_cut = 0;
            if (s_mdllog && ++s_mdllog_l >= 300u) {
                s_mdllog_l = 0;
                j3d_mdl_dump();
            }
        }
        if (s_vilog)
            vilog_note_eye(state);
        return;
    }
    /* Overlap request in its teardown phases: take the SAFE R-frame — the
     * duplicate-present tail-call touches neither the persistent packet
     * list nor the J3D arrays while scene teardown may be freeing them
     * (the reason the guard exists). Narrowed (MODERNGEKKO_F60_OVLP_PEEK,
     * default on) from the whole request to its teardown tail: the
     * request's mPhs.id runs 0-3 while the OLD scene is alive and drawn —
     * phases 0-2 are normal scene rendering with the cover task spawning,
     * phase 3 (WaitOfFadeout, mIsPeek==1) is the peek window where the
     * task draws the old scene under the fade — repaint interpolates real
     * on-screen content in all of them. Phases 4-6
     * (IsWaitOfFadeout/IsDone/Done) are the completion boundary where the
     * requester proceeds to swap scenes — the window that can free what
     * repaint would touch — so those dup. An unreadable request also dups
     * (can't prove safety). s_ovlp_gate_peek=0 restores the old
     * whole-request dup. The 30Hz cadence is untouched either way (see
     * frame60_accum_decide), so fades/wipes/timers keep retail timing.
     * FIFO replay also falls back: re-patching a captured stream is
     * unsafe mid-teardown. */
    if (s_overlap_active && (!s_ovlp_gate_peek || s_ovlp_phase >= 4u)) {
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
        /* Overlap request alive but in a scene-live phase (0-3): the
         * scene on screen is intact — repaint/interpolate instead of dup,
         * counted separately. */
        if (s_overlap_active) ++s_dbg_ovr_rep;
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
            uint64_t tc = s_cost ? host_now_ns() : 0;
            if (s_cam_interp) {
                camview_rframe(state);          /* may set s_cam_cut */
                s_c_cam_ns += host_now_ns() - tc; ++s_c_cam_n;
            }
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
                if (s_cam_interp) {
                    tc = host_now_ns();
                    camview_install(state, s_interp_alpha);
                    s_c_cam_ns += host_now_ns() - tc; ++s_c_cam_n;
                }
                texanim_rframe(state, s_interp_alpha);
            }
            s_c_inj_ns += host_now_ns() - t1; ++s_c_inj_n;
        } else {
            j3d_rframe_refresh(state);
            uint64_t tc = s_cost ? host_now_ns() : 0;
            if (s_cam_interp) {
                camview_rframe(state);
                if (s_cost) { s_c_cam_ns += host_now_ns() - tc; ++s_c_cam_n; }
            }
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
                if (s_cam_interp) {
                    tc = s_cost ? host_now_ns() : 0;
                    camview_install(state, s_interp_alpha);
                    if (s_cost) { s_c_cam_ns += host_now_ns() - tc; ++s_c_cam_n; }
                }
                texanim_rframe(state, s_interp_alpha);
            }
        }
        /* JPA particles are world-space — they lerp even across a camera cut
         * (the world did not jump, only the view did). Skipped entirely by the
         * noinject probe like every other inject. */
        if (s_jpa_fix && !s_noinject) {
            const uint64_t tj = s_cost ? host_now_ns() : 0;
            jpa_rframe(state, s_interp_alpha);
            if (s_cost) { s_c_jpa_ns += host_now_ns() - tj; ++s_c_jpa_n; }
        }
        /* Cloth/sail/flag packets: rel-side interpolation (OPA/XLU drawbuf
         * walk — flips restored at next Painter entry). DOL-side cloth is
         * handled inside Painter by the dCloth_packet_c::draw hooks. */
        if (s_cloth_interp && !s_noinject) {
            if (s_cost) {
                const uint64_t t0 = host_now_ns();
                cloth_rframe(state, s_interp_alpha);
                s_c_clo_ns += host_now_ns() - t0; ++s_c_clo_n;
            } else {
                cloth_rframe(state, s_interp_alpha);
            }
        }
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
    ++s_hook_calls;
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
    ++s_hook_calls;
    /* D5: the shadow draw hook shares this gate (no second registration at
     * 0x80084EF0) — snapshot on the L entry, lerp+inject on the R entry.
     * Injected fields are restored by the paired return hook. */
    if (state->pc == SHD_CTRL_DRAW) {
        if (s_cost) {
            const uint64_t t0 = host_now_ns();
            shd_ctrl_draw_entry(state);
            s_c_shd_ns += host_now_ns() - t0; ++s_c_shd_n;
        } else {
            shd_ctrl_draw_entry(state);
        }
    }
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    /* Per-array "live content" mask — which guest array currently holds OUR
     * scratch bytes (bit per array kind below). `injected` is model-level
     * and latches if ANY array was touched; the sparse-writer's ref check
     * needs per-array precision: node vs env vs draw track independently
     * (e.g. an env pair seeded later than the node pair). The bit is set by
     * apply (post-write live==scratch) and by refresh's ours-check, cleared
     * by every observer/restorer that re-establishes live==curr. */
    uint32_t inj_mask;
    J3DMtx* prev_draw;
    J3DMtx* curr_draw;
    J3DMtx* scratch_draw;
    /* Per-model coverage audit (MODERNGEKKO_F60_MDLLOG): observations,
     * dirty-marked observations, inject installs, guest blocks written and
     * teleport reseeds — dumped periodically keyed by guest ptr + counts so
     * cl.bdl (42 joints / 120 env) and friends are identifiable. */
    uint32_t lg_snap;
    uint32_t lg_dirty;
    uint32_t lg_inj;
    uint32_t lg_injb;
    uint32_t lg_tp;
    uint32_t seen_epoch;    /* s_purge_epoch at this slot's last ensure()   */
    int used;
} J3DHistory;

/* inj_mask bits: which guest array live-holds our scratch bytes. */
#define J3D_INJ_NODE 0x1u   /* mpNodeMtx        (0x20 ConcatView models) */
#define J3D_INJ_ENV  0x2u   /* mpWeightEnvMtx   (0x20 ConcatView models) */
#define J3D_INJ_DRAW 0x4u   /* mpDrawMtxBuf[1][viewNo] (buffered models) */

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

/* Per-model coverage audit (MODERNGEKKO_F60_MDLLOG=1): one line per used
 * history slot keyed by guest model ptr + modelData + joint/env counts +
 * flags_f0/draw_mtx_num — enough to identify cl.bdl (Link body: 42 joints /
 * 120 env matrices, flags 0x20), katsura/hands/equipment, NPCs, enemies, the
 * boat and animals. Columns: snap = L-frame observations, dt = observations
 * that set dirty (a real pose change — with the exact gate every moved
 * matrix counts), inj = R-frame inject installs, blk = 48B guest matrices
 * actually written (sparse), tp = teleport reseeds. A model that moves on
 * screen but shows inj==0 or dt==0 is a coverage gap to chase. Debug-only:
 * zero cost while the env is unset. */
static void j3d_mdl_dump(void)
{
    uint32_t n = 0, ni = 0, dead = 0;
    fprintf(stderr, "[f60-mdl] --- coverage dump (trk=%u) ---\n",
            (unsigned)s_hist_used);
    for (uint32_t i = 0; i < J3D_MAX_MODELS; ++i) {
        const J3DHistory* h = &s_hist[i];
        if (!h->used) continue;
        ++n;
        if (h->no_interp) ++ni;
        if (!h->model_data) ++dead;
        fprintf(stderr,
            "[f60-mdl] mdl=%08X md=%08X j=%u e=%u fl=%02X dn=%u%s%s "
            "snap=%u dt=%u inj=%u blk=%u tp=%u\n",
            (unsigned)h->guest_model_ptr, (unsigned)h->model_data,
            (unsigned)h->joint_num, (unsigned)h->wEvlp_num,
            (unsigned)h->flags_f0, (unsigned)h->draw_mtx_num,
            h->no_interp ? " NI" : "",
            (h->joint_num == 42u && h->wEvlp_num == 120u) ? " cl.bdl?" : "",
            (unsigned)h->lg_snap, (unsigned)h->lg_dirty,
            (unsigned)h->lg_inj, (unsigned)h->lg_injb, (unsigned)h->lg_tp);
    }
    fprintf(stderr, "[f60-mdl] --- %u used (%u no_interp, %u unconfigured) ---\n",
            (unsigned)n, (unsigned)ni, (unsigned)dead);
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
        h->model_data = 0; h->flags_f0 = 0; h->injected = 0; h->inj_mask = 0;
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
        h->injected = 0; h->inj_mask = 0;
    }
    if (!h->prev)
    {
        h->joint_num = joint_num; h->wEvlp_num = wEvlp;
        h->node_ptr = h->env_ptr = 0;
        h->model_data = 0;
        h->flags_f0 = 0; h->draw_mtx_num = 0; h->view_no = 0; h->draw_ptr = 0;
        h->draw_buf1 = 0; h->draw_gen = 0;
        h->has_prev_env = 0; h->has_prev_draw = 0; h->dirty_draw = 0;
        h->injected = 0; h->inj_mask = 0;
        h->lg_snap = 0; h->lg_dirty = 0; h->lg_inj = 0; h->lg_injb = 0;
        h->lg_tp = 0;
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
    h->injected = 0; h->inj_mask = 0;
    h->lg_snap = 0; h->lg_dirty = 0; h->lg_inj = 0; h->lg_injb = 0;
    h->lg_tp = 0;
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
    /* dirty iff any joint endpoint changed — same rule as production
     * hist_rotate (eps==0 under MODERNGEKKO_F60_DIRTY_EXACT, the shipped
     * default; =0 restores the legacy 0.02f dead-zone). */
    const float eps = s_dirty_exact ? 0.0f : 0.02f;
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

/* Sparse variant (MODERNGEKKO_F60_SPARSE_WR, default on): store only the 48B
 * matrices whose src bytes differ from ref[j]. ref must describe what the
 * live array already holds — curr after a fresh observation, our scratch on
 * consecutive R-frames — so a skipped block leaves live==ref==scratch bytes
 * intact and the scratch-match restore/refresh protocol stays bit-exact.
 * Bounds the guest writes to the bones that actually moved this step (idle
 * poses touch a handful of joints; a full-model write was ~120x48B anyway).
 * Returns the number of 48B matrices written. Callers hold the "ref==live"
 * invariant via J3DHistory::inj_mask; when unsure they pass curr and accept a
 * possibly-redundant write (never a wrongly-skipped one). */
static uint32_t write_mtx_arr_diff(CPUState* st, uint32_t addr,
                                   const J3DMtx* src, const J3DMtx* ref,
                                   uint32_t n)
{
    uint32_t wr = 0;
    if (!s_sparse_wr) {
        write_mtx_arr(st, addr, src, n);
        return n;
    }
    for (uint32_t j = 0; j < n; ++j) {
        if (memcmp(&src[j], &ref[j], J3D_MTX_BYTES) != 0) {
            write_mtx_arr(st, addr + j * J3D_MTX_BYTES, &src[j], 1u);
            ++wr;
        }
    }
    return wr;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
    if (!s_j3d_interp || !s_hist_used) return;
    const uint32_t ptr = (uint32_t)state->gpr[4];
    if (!J3D_IN_MEM1(ptr)) return;
    j3d_purge_ptr_block(state, ptr);
}
static void on_jkr_bulk(CPUState* state)           /* r3 = this (heap) */
{
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    /* Dirty iff any endpoint element actually changed. The original 0.02f
     * whole-model dead-zone was a cost short-circuit (skip lerping a pose
     * that "didn't move") — but it swallowed exactly the sub-threshold idle /
     * slow-motion deltas this mod exists to show, freezing characters at the
     * L pose on every R-frame. Identical endpoints are already a no-op lerp
     * (and dirty==0 skips even the memcmp/write), so exact comparison is the
     * correct criterion: eps==0 -> `d > 0` == "changed" (NaN stays
     * not-dirty, +0/-0 compare equal). MODERNGEKKO_F60_DIRTY_EXACT=0 restores
     * the old threshold for A/B. */
    const float eps = s_dirty_exact ? 0.0f : 0.02f;
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
    ++s_hook_calls;
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
        /* live==scratch verified — restoring curr need only rewrite the
         * blocks that differ (the ones we actually lerped); skipped blocks
         * already hold curr bytes. */
        s_rest_writes += write_mtx_arr_diff(state, arr_ea, curr, scratch, n);
        memcpy(s_tmp_mtx, curr, (size_t)n * J3D_MTX_BYTES);
        ++s_rest_models;
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
        ++h->lg_snap;
        const uint32_t was_injected = h->injected;
        const uint32_t was_tele = h->teleported;
        /* lg_dirty: the pose-changed flag meaningful at this point is the
         * one left by the LAST observation — the preceding R-frame refresh
         * rotate (execute writes the next pose after the L-Painter, so the
         * refresh is where a moved pose is first seen) or a foreign-write
         * observation. This snapshot's own rotate then re-checks live vs
         * prev — which is always identical right after the scratch restore,
         * so the POST-rotate flag would read 0 even for a walking character
         * (which is why the old dt counts stuck at ~1). Count the pre-rotate
         * flags instead, OR'd with any fresh diff this rotate itself sees. */
        const uint32_t was_dirty = h->dirty;
        const uint32_t was_ddraw = h->dirty_draw;
        h->injected = 0;
        /* Post-snapshot every array holds the observed pose (restored curr or
         * fresh production bytes) — nothing of ours lives anywhere. */
        h->inj_mask = 0;
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
            if (was_dirty || h->dirty) ++h->lg_dirty;
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
            if (was_ddraw || h->dirty_draw) ++h->lg_dirty;
        }
        if (h->teleported && !was_tele) ++h->lg_tp;
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
                /* live content established: ours -> scratch, else curr */
                h->inj_mask = ours ? (h->inj_mask | J3D_INJ_NODE)
                                   : (h->inj_mask & ~J3D_INJ_NODE);
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
                h->inj_mask = ours ? (h->inj_mask | J3D_INJ_ENV)
                                   : (h->inj_mask & ~J3D_INJ_ENV);
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
                memcmp(s_tmp_mtx, h->scratch_draw, (size_t)dn * J3D_MTX_BYTES) == 0) {
                h->inj_mask |= J3D_INJ_DRAW;   /* live still holds our lerp */
                continue;
            }
            h->injected = 0;
        }
        h->inj_mask &= ~J3D_INJ_DRAW;   /* observed or matched — live==curr */
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
            uint32_t ran = 0, wr = 0;
            if (h->joint_num && h->prev && h->curr && h->scratch && J3D_IN_MEM1(h->node_ptr)) {
                /* Sparse install (MODERNGEKKO_F60_SPARSE_WR): lerp into the
                 * staging buffer, write only the 48B matrices that differ
                 * from what the array already holds (inj_mask says scratch
                 * on consecutive R-frames, curr otherwise), then publish the
                 * full lerp into scratch so live==scratch bitwise — the
                 * refresh/restore ours-checks keep working byte-exactly. */
                j3d_lerp_mtx(h->prev, h->curr, alpha, s_tmp_mtx, h->joint_num);
                const J3DMtx* ref = (h->inj_mask & J3D_INJ_NODE) ? h->scratch : h->curr;
                wr += write_mtx_arr_diff(state, h->node_ptr, s_tmp_mtx, ref, h->joint_num);
                memcpy(h->scratch, s_tmp_mtx, (size_t)h->joint_num * J3D_MTX_BYTES);
                h->inj_mask |= J3D_INJ_NODE;
                ran = 1;
            }
            /* has_prev_env required: the env pair seeds on its first real
             * observation — without the gate an unseeded (uninitialized)
             * env buffer would be lerped into a live array. */
            if (h->wEvlp_num && h->has_prev_env && h->prev_env && h->curr_env && h->scratch_env && J3D_IN_MEM1(h->env_ptr)) {
                j3d_lerp_mtx(h->prev_env, h->curr_env, alpha, s_tmp_mtx, h->wEvlp_num);
                const J3DMtx* ref = (h->inj_mask & J3D_INJ_ENV) ? h->scratch_env : h->curr_env;
                wr += write_mtx_arr_diff(state, h->env_ptr, s_tmp_mtx, ref, h->wEvlp_num);
                memcpy(h->scratch_env, s_tmp_mtx, (size_t)h->wEvlp_num * J3D_MTX_BYTES);
                h->inj_mask |= J3D_INJ_ENV;
                ran = 1;
            }
            if (ran) { ++s_inj_models; s_inj_writes += wr; h->injected = 1;
                       s_matrices_injected = 1; ++h->lg_inj; h->lg_injb += wr; }
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
            j3d_lerp_mtx(h->prev_draw, h->curr_draw, alpha, s_tmp_mtx, h->draw_mtx_num);
            const J3DMtx* ref = (h->inj_mask & J3D_INJ_DRAW) ? h->scratch_draw : h->curr_draw;
            const uint32_t wr =
                write_mtx_arr_diff(state, arr, s_tmp_mtx, ref, h->draw_mtx_num);
            memcpy(h->scratch_draw, s_tmp_mtx, (size_t)h->draw_mtx_num * J3D_MTX_BYTES);
            h->inj_mask |= J3D_INJ_DRAW;
            ++s_inj_models; s_inj_writes += wr;
            h->injected = 1; s_matrices_injected = 1;
            ++h->lg_inj; h->lg_injb += wr;
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
        h->inj_mask = 0;   /* after this pass nothing of ours is live */
        if (!h->model_data) continue;
        if (s_model_alive && !j3d_model_alive(state, h)) { j3d_slot_reset(h); continue; }
        if (h->flags_f0 == 0x20u) {
            if (h->joint_num && h->curr && h->scratch && J3D_IN_MEM1(h->node_ptr)) {
                read_mtx_arr(state, h->node_ptr, s_tmp_mtx, h->joint_num);
                if (memcmp(s_tmp_mtx, h->scratch, (size_t)h->joint_num * J3D_MTX_BYTES) == 0) {
                    /* live==scratch proven — only the lerped blocks differ */
                    s_rest_writes +=
                        write_mtx_arr_diff(state, h->node_ptr, h->curr,
                                           h->scratch, h->joint_num);
                    ++s_rest_models;
                }
            }
            if (h->wEvlp_num && h->has_prev_env && h->curr_env && h->scratch_env &&
                J3D_IN_MEM1(h->env_ptr)) {
                read_mtx_arr(state, h->env_ptr, s_tmp_mtx, h->wEvlp_num);
                if (memcmp(s_tmp_mtx, h->scratch_env, (size_t)h->wEvlp_num * J3D_MTX_BYTES) == 0) {
                    s_rest_writes +=
                        write_mtx_arr_diff(state, h->env_ptr, h->curr_env,
                                           h->scratch_env, h->wEvlp_num);
                    ++s_rest_models;
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
                    s_rest_writes +=
                        write_mtx_arr_diff(state, arr, h->curr_draw,
                                           h->scratch_draw, h->draw_mtx_num);
                    ++s_rest_models;
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
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    fol_entry(st, &s_fol_grass, (uint32_t)st->gpr[3]);
    if (s_cost) { s_c_fol_ns += host_now_ns() - t0; ++s_c_fol_n; }
    ++s_hook_calls;
}
static void on_grass_draw_return(CPUState* st)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    fol_return(st, &s_fol_grass);
    if (s_cost) { s_c_fol_ns += host_now_ns() - t0; ++s_c_fol_n; }
    ++s_hook_calls;
}
static void on_flower_draw_entry(CPUState* st)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    fol_entry(st, &s_fol_flower, (uint32_t)st->gpr[3]);
    if (s_cost) { s_c_fol_ns += host_now_ns() - t0; ++s_c_fol_n; }
    ++s_hook_calls;
}
static void on_flower_draw_return(CPUState* st)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    fol_return(st, &s_fol_flower);
    if (s_cost) { s_c_fol_ns += host_now_ns() - t0; ++s_c_fol_n; }
    ++s_hook_calls;
}
static void on_tree_draw_entry(CPUState* st)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    fol_entry(st, &s_fol_tree, (uint32_t)st->gpr[3]);
    if (s_cost) { s_c_fol_ns += host_now_ns() - t0; ++s_c_fol_n; }
    ++s_hook_calls;
}
static void on_tree_draw_return(CPUState* st)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    fol_return(st, &s_fol_tree);
    if (s_cost) { s_c_fol_ns += host_now_ns() - t0; ++s_c_fol_n; }
    ++s_hook_calls;
}

/* ========== D4-b JPA particle state lerp (MODERNGEKKO_F60_JPA_FIX /
 *                                      MODERNGEKKO_F60_JPA_FULL) ===========
 * JPABaseParticle's draw-consumed fields are all recomputed every calc tick
 * (calcPosition / JPADraw::calcParticle — JPAParticle.cpp / JPADraw.cpp) and
 * read by the draw visitors — under the split every animated property steps
 * at 30 Hz. Fix: at each L-frame Painter entry rebuild a host table
 * {addr -> frame, draw state} covering every live particle; at the R-frame
 * entry write lerp(L_state, live_state, alpha) into entries whose live
 * mCurFrame == stored + 1.0 — incFrame() adds exactly 1.0 per calc tick and
 * pooled reuse restarts at -1.0 (JPAParticle.cpp:88/133 initParticle/
 * initChild, :165 incFrame), so frame continuity IS the identity key: a
 * recycled address can only alias when the L snapshot held frame -1.0,
 * which live-list particles never carry (init + incFrame happen inside one
 * calc pass). FULL adds a second identity key, mLifeTime — constant per
 * particle after init — so an exotic re-init at the same address can never
 * alias even if its frame happened to continue.
 * FULL (default on) lerps every draw-read field that animates per tick:
 *   mLocalPosition +0x1C   dirTypePos/PosInv quads (JPADrawVisitor.cpp:369)
 *   mGlobalPosition +0x28  every vertex-bearing exec + dirTypePrevPtcl
 *   mVelocity +0x34        dirTypeVel + ExecLine length (L366, L642)
 *   mDrawParams.mAxis +0x8C  persistent draw-mutated orientation
 *                          (dPa_J3DmodelEmitter_c::draw, d_particle.cpp:95)
 *   mScaleX/mScaleY +0x9C/+0xA0  all quad/line/point sizes
 *   mAlphaOut +0xAC        TEV alpha (RegisterPrm/Env color execs)
 *   mPrmColor/mEnvColor +0xB8/+0xBC   per-channel TEV colors
 *   mRotateAngle +0xC0     rot execs + model emitters — s16-domain shortest-
 *                          angle lerp (0x10000 = 2pi)
 * Discrete fields are NEVER lerped: mTexIdx (+0xC6), mStatus (+0xCC),
 * mCurFrame (the identity key itself), mRotateSpeed/mAlphaWaveRandom/
 * mLoopOffset/mScaleOut (calc-time inputs). MODERNGEKKO_F60_JPA_FULL=0
 * falls back to the original position-only lerp with byte-identical
 * guest writes.
 * Emitter state draws per-emitter, keyed in a second table by emitter
 * address; its identity key is mTick.mFrame +0x164 — incFrame() advances it
 * exactly +1 per calc and create() restarts it at 0, so a pooled emitter
 * slot can never alias a stored tick >= 0:
 *   mEmitterTranslation +0x18 / mEmitterDir +0x2C / mEmitterRot +0x24
 *                          calcEmitterGlobalPosition + calcgReRDirection
 *                          run at DRAW time (dirTypePrevPtcl head anchor,
 *                          dirTypeEmtrDir) — attached effects track the
 *                          interpolated emitter transform
 *   mTick.mFrame +0x164    global tex scroll (GenPrjTexMtx/SetTexMtx read
 *                          pbe->getFrame() at draw)
 *   mDraw.mPrmColor/mEnvColor +0x158/+0x15C   RegisterColorEmitter* execs
 *   mGlobalRotation +0x1A8 / mGlobalDynamicsScale +0x1D8 /
 *   mGlobalTranslation +0x1E4      setGlobalSRTMatrix outputs
 *   mGlobalParticleScale +0x1F0    cb.mGlobalScaleX/Y
 *   mGlobalPrmColor/mGlobalEnvColor +0x1FC/+0x200   cb.mPrmColor/mEnvColor
 *                          + getGlobalAlpha() zDraw alpha compare
 * mDraw.mTexIdx (+0x160) and mTime (+0x168) stay discrete/calc-side.
 * Restore: the next L-entry writes the recorded live values back before
 * Painter draws — the persistent emitter lists are consumed TWICE (R_k and
 * L_{k+1}), so an unrestored lerp would leak into the authoritative L frame
 * AND into the next calc tick's inputs (calcPosition reads mLocalPosition).
 * Traversal: mEmitterMng (dPa_control_c .sbss 0x803F6C28, symbols.txt:18804)
 * -> mEmtrGroup[16] @+0x50 (JPAEmitterManager.h:47; JSUPtrList: mHead@+0,
 * mTail@+4, mLength@+8, JSUList.h:73-75) -> emitters via mLink @+0x90
 * (JPAEmitter.h:393; JSUPtrLink mNext @+0x0C, JSUList.h:31) ->
 * mActiveParticles @+0x17C / mChildParticles @+0x188 (JPAEmitter.h:398-399).
 * Particle link mLink is member +0x00 (JPAParticle.h:91), so link addr ==
 * particle addr; mGlobalPosition @+0x28, mCurFrame @+0x78
 * (JPAParticle.h:92,100). Bounded: 16 groups, <=256 emitters each, <=4096
 * particles per walk; every pointer in_ram-guarded. The tables are rebuilt
 * (memset + reseed) at every L entry and self-heal any stale address. */
#define JPA_MGR_EA      0x803F6C28u   /* dPa_control_c::mEmitterMng           */
#define JPA_OFF_GROUPS  0x50u         /* JPAEmitterManager::mEmtrGroup[16]    */
#define JPA_EMTR_LINK   0x90u         /* JPABaseEmitter::mLink                */
#define JPA_EMTR_ACT    0x17Cu        /* JPABaseEmitter::mActiveParticles     */
#define JPA_EMTR_CHLD   0x188u        /* JPABaseEmitter::mChildParticles      */
#define JPA_EMTR_SPAN   0x210u        /* bytes we touch inside an emitter     */
#define JPA_EMTR_ETRS   0x18u         /* mEmitterTranslation (TVec3<f32>)     */
#define JPA_EMTR_EROT   0x24u         /* mEmitterRot (TVec3<s16>, degrees)    */
#define JPA_EMTR_EDIR   0x2Cu         /* mEmitterDir (TVec3<f32>)             */
#define JPA_EMTR_DCLR   0x158u        /* mDraw.mPrmColor / mEnvColor          */
#define JPA_EMTR_TICK   0x164u        /* mTick.mFrame (f32)                   */
#define JPA_EMTR_GROT   0x1A8u        /* mGlobalRotation (SMatrix34C, 12 f32) */
#define JPA_EMTR_GDYN   0x1D8u        /* mGlobalDynamicsScale (TVec3<f32>)    */
#define JPA_EMTR_GTRN   0x1E4u        /* mGlobalTranslation (TVec3<f32>)      */
#define JPA_EMTR_GPSCL  0x1F0u        /* mGlobalParticleScale (TVec3<f32>)    */
#define JPA_EMTR_GCLR   0x1FCu        /* mGlobalPrmColor / mGlobalEnvColor    */
#define JPA_EMTR_FLAGS  0x20Cu        /* mFlags — JPAEmtrStts_*               */
#define JPA_EMTRFL_STOPCALC 0x02u     /* JPAEmtrStts_StopCalc                 */
#define JPA_PTCL_LOCAL  0x1Cu         /* JPABaseParticle::mLocalPosition      */
#define JPA_PTCL_GLOBAL 0x28u         /* JPABaseParticle::mGlobalPosition     */
#define JPA_PTCL_FRAME  0x78u         /* JPABaseParticle::mCurFrame           */
#define JPA_PTCL_LIFE   0x7Cu         /* JPABaseParticle::mLifeTime           */
#define JPA_PTCL_AXIS   0x8Cu         /* JPADrawParams::mAxis (TVec3<f32>)    */
#define JPA_PTCL_SCL    0x9Cu         /* JPADrawParams::mScaleX/mScaleY       */
#define JPA_PTCL_AOUT   0xACu         /* JPADrawParams::mAlphaOut             */
#define JPA_PTCL_PRM    0xB8u         /* JPADrawParams::mPrmColor (GXColor)   */
#define JPA_PTCL_ENV    0xBCu         /* JPADrawParams::mEnvColor (GXColor)   */
#define JPA_PTCL_ANGLE  0xC0u         /* JPADrawParams::mRotateAngle (u16)    */
#define JPA_LINK_NEXT   0x0Cu         /* JSUPtrLink::mNext                    */
#define JPA_PTCL_SPAN   0xD4u         /* bytes we touch inside a particle     */
#define JPA_MAX_EMTR    256u          /* per-group emitter walk cap           */
#define JPA_MAX_PTCL    4096u         /* total particles per walk             */
#define JPA_HASH_SIZE   8192u
#define JPA_HASH_PROBE  8u
#define JPA_EHASH_SIZE  512u          /* emitter pool is 150 — 512 slots      */
#define JPA_EHASH_PROBE 8u

/* One endpoint of the particle draw-state pair: every field the R-frame
 * temporarily writes into the live object. `l` = what the previous L-frame
 * drew (prev endpoint), `r` = the live value captured at R-entry (next
 * endpoint + restore source). */
typedef struct {
    float    loc[3];    /* mLocalPosition                    +0x1C */
    float    pos[3];    /* mGlobalPosition                   +0x28 */
    float    vel[3];    /* mVelocity                         +0x34 */
    float    axis[3];   /* mDrawParams.mAxis                 +0x8C */
    float    scl[2];    /* mDrawParams.mScaleX/mScaleY   +0x9C/+A0 */
    float    aout;      /* mDrawParams.mAlphaOut             +0xAC */
    uint16_t angle;     /* mDrawParams.mRotateAngle          +0xC0 */
    uint16_t pad0;
    uint32_t prm;       /* mDrawParams.mPrmColor             +0xB8 */
    uint32_t env;       /* mDrawParams.mEnvColor             +0xBC */
} JpaVis;   /* 72B */

typedef struct {
    uint32_t addr;      /* guest particle address; 0 = empty slot           */
    float    frame;     /* mCurFrame observed at the L snapshot             */
    float    lifetime;  /* mLifeTime — second identity key (FULL only)      */
    uint8_t  injected;  /* live draw fields currently hold our lerp         */
    uint8_t  pad[3];
    JpaVis   l;         /* L endpoint (what the last L frame drew)          */
    JpaVis   r;         /* live values read at R entry (restore + endpoint) */
} JpaEnt;
static JpaEnt s_jpa[JPA_HASH_SIZE];

/* Emitter-side endpoint pair — same l/r contract as JpaVis. */
typedef struct {
    float    etrs[3];   /* mEmitterTranslation                +0x18  */
    int16_t  erot[3];   /* mEmitterRot (degrees per axis)     +0x24  */
    uint16_t pad0;
    float    edir[3];   /* mEmitterDir                        +0x2C  */
    uint32_t dprm;      /* mDraw.mPrmColor                    +0x158 */
    uint32_t denv;      /* mDraw.mEnvColor                    +0x15C */
    float    tick;      /* mTick.mFrame                       +0x164 */
    float    rot[12];   /* mGlobalRotation (3x4)              +0x1A8 */
    float    dyn[3];    /* mGlobalDynamicsScale               +0x1D8 */
    float    trn[3];    /* mGlobalTranslation                 +0x1E4 */
    float    pscl[3];   /* mGlobalParticleScale               +0x1F0 */
    uint32_t gprm;      /* mGlobalPrmColor                    +0x1FC */
    uint32_t genv;      /* mGlobalEnvColor                    +0x200 */
} JpaEVis;  /* 136B */

typedef struct {
    uint32_t addr;      /* emitter object address; 0 = empty slot           */
    uint8_t  injected;  /* live emitter fields currently hold our lerp      */
    uint8_t  pad[7];
    JpaEVis  l;         /* L endpoint (l.tick is also the identity key)     */
    JpaEVis  r;         /* live values read at R entry                      */
} JpaEmtrEnt;
static JpaEmtrEnt s_jpae[JPA_EHASH_SIZE];

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

static JpaEmtrEnt* jpa_elookup(uint32_t addr)
{
    const uint32_t h = ((addr >> 8) * 2654435761u) >> (32u - 9u);
    for (uint32_t p = 0; p < JPA_EHASH_PROBE; ++p) {
        JpaEmtrEnt* e = &s_jpae[(h + p) & (JPA_EHASH_SIZE - 1u)];
        if (e->addr == 0u || e->addr == addr)
            return e;
    }
    return 0;
}

static float jpa_lf(float a, float b, float alpha)
{
    const float v = a + alpha * (b - a);
    return isfinite(v) ? v : b;     /* same non-finite policy as J3D */
}

static void jpa_lerp3(float* out, const float* a, const float* b, float alpha)
{
    for (int k = 0; k < 3; ++k)
        out[k] = jpa_lf(a[k], b[k], alpha);
}

/* GXColor packs 4 u8 channels big-endian — per-channel lerp, rounded and
 * clamped (the TEV color math consumes integer channels). */
static uint32_t jpa_lerp_color(uint32_t a, uint32_t b, float alpha)
{
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        const uint32_t sh = (uint32_t)(3 - i) * 8u;
        const int ca = (int)((a >> sh) & 0xFFu);
        const int cb = (int)((b >> sh) & 0xFFu);
        const int d = cb - ca;
        int c = ca + (int)(d * alpha + (d >= 0 ? 0.5f : -0.5f));
        if (c < 0) c = 0; else if (c > 255) c = 255;
        out |= (uint32_t)c << sh;
    }
    return out;
}

/* 16-bit angle domain: 0x10000 = one full turn (JMASSin/JMASCos argument
 * domain — mRotateAngle accumulates mRotateSpeed and wraps mod 2^16). The
 * shortest signed delta is the only correct blend across the wrap point. */
static uint16_t jpa_lerp_angle(uint16_t a, uint16_t b, float alpha)
{
    const int32_t d = (int32_t)(int16_t)(uint16_t)(b - a);
    const int32_t v = (int32_t)a +
        (int32_t)(d * alpha + (d >= 0 ? 0.5f : -0.5f));
    return (uint16_t)v;
}

/* mEmitterRot stores DEGREES per axis (calcgReRDirection maps deg -> the
 * 0x10000-turn domain via (deg << 14) / 90). The physical angle wraps on a
 * 360-degree circle (+179 -> -179 is a 2-degree step), so the shortest path
 * must be taken mod 360 — a plain s16 delta would rotate the long way
 * through 0 across the +-180 boundary. */
static int16_t jpa_lerp_deg(int16_t a, int16_t b, float alpha)
{
    int32_t d = (int32_t)b - (int32_t)a;
    d %= 360;
    if (d > 180) d -= 360;
    else if (d < -180) d += 360;
    return (int16_t)((int32_t)a +
        (int32_t)(d * alpha + (d >= 0 ? 0.5f : -0.5f)));
}

/* Read the full draw-consumed particle field set (FULL mode). */
static void jpa_rd_vis(CPUState* st, uint32_t p, JpaVis* v)
{
    float tmp[9];
    uint32_t w;
    rd_f32_arr(st, p + JPA_PTCL_LOCAL, tmp, 9u);   /* loc+pos+vel contiguous */
    memcpy(v->loc, tmp + 0, 12u);
    memcpy(v->pos, tmp + 3, 12u);
    memcpy(v->vel, tmp + 6, 12u);
    rd_f32_arr(st, p + JPA_PTCL_AXIS, v->axis, 3u);
    rd_f32_arr(st, p + JPA_PTCL_SCL,  v->scl,  2u);
    w = rd32_fast(st, p + JPA_PTCL_AOUT);
    memcpy(&v->aout, &w, 4u);
    v->prm   = rd32_fast(st, p + JPA_PTCL_PRM);
    v->env   = rd32_fast(st, p + JPA_PTCL_ENV);
    v->angle = (uint16_t)rd16_fast(st, p + JPA_PTCL_ANGLE);
    v->pad0  = 0;
}

static void jpa_lerp_vis(const JpaVis* a, const JpaVis* b, float alpha,
                         JpaVis* o)
{
    jpa_lerp3(o->loc, a->loc, b->loc, alpha);
    jpa_lerp3(o->pos, a->pos, b->pos, alpha);
    jpa_lerp3(o->vel, a->vel, b->vel, alpha);
    jpa_lerp3(o->axis, a->axis, b->axis, alpha);
    o->scl[0] = jpa_lf(a->scl[0], b->scl[0], alpha);
    o->scl[1] = jpa_lf(a->scl[1], b->scl[1], alpha);
    o->aout   = jpa_lf(a->aout,  b->aout,  alpha);
    o->angle  = jpa_lerp_angle(a->angle, b->angle, alpha);
    o->prm    = jpa_lerp_color(a->prm, b->prm, alpha);
    o->env    = jpa_lerp_color(a->env, b->env, alpha);
}

static void jpa_wr_vis(CPUState* st, uint32_t p, const JpaVis* v)
{
    float tmp[9];
    uint32_t w;
    memcpy(tmp + 0, v->loc, 12u);
    memcpy(tmp + 3, v->pos, 12u);
    memcpy(tmp + 6, v->vel, 12u);
    wr_f32_arr(st, p + JPA_PTCL_LOCAL, tmp, 9u);
    wr_f32_arr(st, p + JPA_PTCL_AXIS, v->axis, 3u);
    wr_f32_arr(st, p + JPA_PTCL_SCL,  v->scl,  2u);
    memcpy(&w, &v->aout, 4u);
    wr32_fast(st, p + JPA_PTCL_AOUT, w);
    wr32_fast(st, p + JPA_PTCL_PRM, v->prm);
    wr32_fast(st, p + JPA_PTCL_ENV, v->env);
    wr16_fast(st, p + JPA_PTCL_ANGLE, v->angle);
}

/* Emitter draw-state read — every field JPADraw/mDraw consume that also
 * animates per tick. mEmitterRot (+0x24) packs three s16 degrees:
 * x|y in one word at +0x24, z in the high half of the word at +0x28. */
static void jpa_rd_evis(CPUState* st, uint32_t e, JpaEVis* v)
{
    uint32_t w;
    rd_f32_arr(st, e + JPA_EMTR_ETRS, v->etrs, 3u);
    w = rd32_fast(st, e + JPA_EMTR_EROT);
    v->erot[0] = (int16_t)(w >> 16);
    v->erot[1] = (int16_t)(w & 0xFFFFu);
    v->erot[2] = (int16_t)rd16_fast(st, e + JPA_EMTR_EROT + 4u);
    rd_f32_arr(st, e + JPA_EMTR_EDIR, v->edir, 3u);
    v->dprm = rd32_fast(st, e + JPA_EMTR_DCLR);
    v->denv = rd32_fast(st, e + JPA_EMTR_DCLR + 4u);
    w = rd32_fast(st, e + JPA_EMTR_TICK);
    memcpy(&v->tick, &w, 4u);
    rd_f32_arr(st, e + JPA_EMTR_GROT, v->rot, 12u);
    rd_f32_arr(st, e + JPA_EMTR_GDYN, v->dyn, 3u);
    rd_f32_arr(st, e + JPA_EMTR_GTRN, v->trn, 3u);
    rd_f32_arr(st, e + JPA_EMTR_GPSCL, v->pscl, 3u);
    v->gprm = rd32_fast(st, e + JPA_EMTR_GCLR);
    v->genv = rd32_fast(st, e + JPA_EMTR_GCLR + 4u);
}

static void jpa_lerp_evis(const JpaEVis* a, const JpaEVis* b, float alpha,
                          JpaEVis* o)
{
    jpa_lerp3(o->etrs, a->etrs, b->etrs, alpha);
    for (int j = 0; j < 3; ++j)
        o->erot[j] = jpa_lerp_deg(a->erot[j], b->erot[j], alpha);
    jpa_lerp3(o->edir, a->edir, b->edir, alpha);
    o->dprm = jpa_lerp_color(a->dprm, b->dprm, alpha);
    o->denv = jpa_lerp_color(a->denv, b->denv, alpha);
    o->tick = jpa_lf(a->tick, b->tick, alpha);
    for (int j = 0; j < 12; ++j)
        o->rot[j] = jpa_lf(a->rot[j], b->rot[j], alpha);
    jpa_lerp3(o->dyn,  a->dyn,  b->dyn,  alpha);
    jpa_lerp3(o->trn,  a->trn,  b->trn,  alpha);
    jpa_lerp3(o->pscl, a->pscl, b->pscl, alpha);
    o->gprm = jpa_lerp_color(a->gprm, b->gprm, alpha);
    o->genv = jpa_lerp_color(a->genv, b->genv, alpha);
}

static void jpa_wr_evis(CPUState* st, uint32_t e, const JpaEVis* v)
{
    uint32_t w;
    wr_f32_arr(st, e + JPA_EMTR_ETRS, v->etrs, 3u);
    /* both s16 lanes live in the +0x24 word; z alone at +0x28 */
    wr32_fast(st, e + JPA_EMTR_EROT,
              ((uint32_t)(uint16_t)v->erot[0] << 16) |
              (uint32_t)(uint16_t)v->erot[1]);
    wr16_fast(st, e + JPA_EMTR_EROT + 4u, (uint16_t)v->erot[2]);
    wr_f32_arr(st, e + JPA_EMTR_EDIR, v->edir, 3u);
    wr32_fast(st, e + JPA_EMTR_DCLR,     v->dprm);
    wr32_fast(st, e + JPA_EMTR_DCLR + 4u, v->denv);
    memcpy(&w, &v->tick, 4u);
    wr32_fast(st, e + JPA_EMTR_TICK, w);
    wr_f32_arr(st, e + JPA_EMTR_GROT,  v->rot, 12u);
    wr_f32_arr(st, e + JPA_EMTR_GDYN,  v->dyn,  3u);
    wr_f32_arr(st, e + JPA_EMTR_GTRN,  v->trn,  3u);
    wr_f32_arr(st, e + JPA_EMTR_GPSCL, v->pscl, 3u);
    wr32_fast(st, e + JPA_EMTR_GCLR,     v->gprm);
    wr32_fast(st, e + JPA_EMTR_GCLR + 4u, v->genv);
}

static void jpa_emtr_visit(CPUState* st, uint32_t emtr, int rframe,
                           float alpha)
{
    JpaEmtrEnt* e = jpa_elookup(emtr);
    if (!rframe) {
        if (!e) return;
        jpa_rd_evis(st, emtr, &e->l);
        memcpy(&e->r, &e->l, sizeof(JpaEVis));
        e->addr = emtr;
        e->injected = 0;
        return;
    }
    if (!e || e->addr != emtr)
        return;   /* spawned since the L snapshot / table full */
    if (e->injected) {
        /* Consecutive R-frame: rewrite from the stored pair at the new
         * alpha — live fields still hold our previous lerp. */
        JpaEVis o;
        jpa_lerp_evis(&e->l, &e->r, alpha, &o);
        jpa_wr_evis(st, emtr, &o);
        return;
    }
    /* Continuity: calc() runs mTick.incFrame() exactly once per tick; a
     * pooled slot restarts at 0 so reuse can never alias a stored tick.
     * Exception: a StopCalc emitter's tick freezes while its before/after
     * calc callbacks still run each tick (JPAEmitter.cpp:243-247 — attached
     * emitters use this to track a parent). Accept an unchanged tick only
     * when StopCalc is still set; a fresh pool reuse can never carry it
     * (create() re-inits mFlags to FirstEmit|RateStepEmit). */
    {
        uint32_t w = rd32_fast(st, emtr + JPA_EMTR_TICK);
        float tick;
        memcpy(&tick, &w, 4u);
        if (tick != e->l.tick + 1.0f) {
            const uint32_t flags = rd32_fast(st, emtr + JPA_EMTR_FLAGS);
            if (!(flags & JPA_EMTRFL_STOPCALC) || tick != e->l.tick)
                return;   /* recycled — leave live untouched */
        }
    }
    jpa_rd_evis(st, emtr, &e->r);
    {
        JpaEVis o;
        jpa_lerp_evis(&e->l, &e->r, alpha, &o);
        jpa_wr_evis(st, emtr, &o);
    }
    e->injected = 1;
    ++s_dbg_jpaeinj;
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
        /* L walk: (re)seed — the state this frame drew is the prev endpoint */
        if (!e) return;
        if (s_jpa_full) {
            jpa_rd_vis(st, p, &e->l);
            fw = rd32_fast(st, p + JPA_PTCL_LIFE);
            memcpy(&e->lifetime, &fw, 4u);
        } else {
            rd_f32_arr(st, p + JPA_PTCL_GLOBAL, e->l.pos, 3u);
        }
        memcpy(&e->r, &e->l, sizeof(JpaVis));
        fw = rd32_fast(st, p + JPA_PTCL_FRAME);
        memcpy(&frame, &fw, 4u);
        e->addr = p;
        e->frame = frame;
        e->injected = 0;
        return;
    }
    if (!e || e->addr != p)
        return;   /* not in the L snapshot (spawned since / table full) */
    if (e->injected) {
        /* Consecutive R-frame: live still holds our previous lerp — rewrite
         * from the stored pair at the new alpha. Never read live as r-side. */
        if (s_jpa_full) {
            JpaVis o;
            jpa_lerp_vis(&e->l, &e->r, alpha, &o);
            jpa_wr_vis(st, p, &o);
        } else {
            jpa_write_lerp(st, p, e->l.pos, e->r.pos, alpha);
        }
        return;
    }
    fw = rd32_fast(st, p + JPA_PTCL_FRAME);
    memcpy(&frame, &fw, 4u);
    if (frame != e->frame + 1.0f)
        return;   /* addr recycled between ticks — not the same particle */
    if (s_jpa_full) {
        float life;
        JpaVis o;
        fw = rd32_fast(st, p + JPA_PTCL_LIFE);
        memcpy(&life, &fw, 4u);
        if (life != e->lifetime)
            return;   /* slot re-init at the same addr — not our particle */
        jpa_rd_vis(st, p, &e->r);   /* save live for the L restore */
        jpa_lerp_vis(&e->l, &e->r, alpha, &o);
        jpa_wr_vis(st, p, &o);
    } else {
        rd_f32_arr(st, p + JPA_PTCL_GLOBAL, pos, 3u);
        memcpy(e->r.pos, pos, sizeof(pos));  /* save live for the L restore */
        jpa_write_lerp(st, p, e->l.pos, e->r.pos, alpha);
    }
    e->frame = frame;
    e->injected = 1;
    ++s_dbg_jpainj;
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
            if (!in_ram(st, emtr, JPA_EMTR_SPAN))
                continue;   /* emitter must span lists + global state */
            /* Emitter-level draw state lerps in the same walk — once per
             * emitter, before its particle lists, so dirTypePrevPtcl/
             * dirTypeEmtrDir inputs already carry the mid-frame transform
             * when the particles get visited. */
            if (s_jpa_full)
                jpa_emtr_visit(st, emtr, rframe, alpha);
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

/* Write every outstanding lerp back to its owner — must run BEFORE the
 * tables are wiped (jpa_lframe rebuild, f60_reset_runtime_state) or the
 * live fields keep our midpoint on an authoritative L frame (and the next
 * calc tick would consume lerped mLocalPosition/mTick.mFrame inputs). */
static void jpa_restore(CPUState* st)
{
    for (uint32_t i = 0; i < JPA_HASH_SIZE; ++i) {
        JpaEnt* e = &s_jpa[i];
        if (e->injected) {
            e->injected = 0;
            if (in_ram(st, e->addr, JPA_PTCL_SPAN)) {
                if (s_jpa_full)
                    jpa_wr_vis(st, e->addr, &e->r);
                else
                    wr_f32_arr(st, e->addr + JPA_PTCL_GLOBAL, e->r.pos, 3u);
            }
        }
    }
    for (uint32_t i = 0; i < JPA_EHASH_SIZE; ++i) {
        JpaEmtrEnt* e = &s_jpae[i];
        if (e->injected) {
            e->injected = 0;
            if (in_ram(st, e->addr, JPA_EMTR_SPAN))
                jpa_wr_evis(st, e->addr, &e->r);
        }
    }
}

static void jpa_lframe(CPUState* st)
{
    if (!s_jpa_fix) return;
    /* Restore any lerped state still outstanding BEFORE Painter re-draws
     * the persistent emitter lists (they are consumed twice: R_k then
     * L_{k+1}). Nothing ran between the R inject and now, so the recorded
     * addrs still name the same particles; the in_ram guard covers a torn-
     * down manager anyway. Then snapshot live state as the next lerp's L
     * endpoint. */
    jpa_restore(st);
    memset(s_jpa, 0, sizeof(s_jpa));
    memset(s_jpae, 0, sizeof(s_jpae));
    jpa_walk(st, 0, 0.0f);
}

static void jpa_rframe(CPUState* st, float alpha)
{
    if (!s_jpa_fix) return;
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    jpa_walk(st, 1, alpha);
}

/* ========== D5 shadow-matrix interpolation (MODERNGEKKO_F60_SHADOW_INTERP) ==
 * Both shadow kinds step at 30 Hz because every matrix they consume is baked
 * during fpcDw (L-only) and the R-frame Painter reads them raw:
 *
 *   dDlst_shadowSimple_c::set  (0x80084AC8, d_drawlist.cpp:1462-1505) bakes
 *     mVolumeMtx = view*worldVol (:1467) and mMtx = view*worldShadow (:1497)
 *     plus mAlpha = f(pos distance). The view is j3dSys.mViewMtx AT SET TIME
 *     — the PREVIOUS iteration's camera — so on R-frames blobs are pinned to
 *     a stale pose AND a stale camera (double error vs lerped geometry).
 *   dDlst_shadowReal_c::set/set2 (0x8008450C/0x800846C8 via ::setReal/
 *     setReal2/addReal) run setShadowRealMtx (0x800841B0): light-facing view
 *     mViewMtx +0x08, ortho mRenderProjMtx +0x38 (Mtx44), receiver proj
 *     mReceiverProjMtx +0x78 — all LIGHT-space (no camera component), so the
 *     projected blob anchors at the L-tick caster pose while the model is
 *     interpolated.
 *
 * Consumption is FIFO matrix values only (COMMON rule 8 safe — no display-
 * list/array pointer patching): dDlst_shadowSimple_c::draw loads mVolumeMtx/
 * mMtx via GXLoadPosMtxImm (:1423/:1431) and mAlpha into a tev color;
 * dDlst_shadowReal_c::draw concat-loads inv(mViewMtx)*inv(mRenderProjMtx)
 * *live j3dSys view into PNMTX1 (:1156-1165 — the volume transform picks up
 * the R-frame camera for free) and mReceiverProjMtx via GXLoadTexMtxImm
 * (:1180) which steers the blob's UV over the 30-Hz depth texture. mAlpha
 * feeds tev alpha. All injected fields are restored at the return hook.
 *
 * Guest layout (verified include/d/d_drawlist.h:292-303,345-404 +
 * config/GZLE01/symbols.txt):
 *   control: mRealNum +0x00, mSimpleNum +0x01, mSimple[128] +0x04 stride 0x68,
 *            mNextID +0x3404, mReal[8] +0x3408 stride 0x2544 (struct 0x15E28)
 *   simple : mAlpha +0x00, mpTexObj +0x04, mVolumeMtx +0x08, mMtx +0x38
 *   real   : mState +0x00 (1=draw this tick), mAlpha +0x02, mModelNum +0x03,
 *            mKey +0x04 (persistent id from mNextID), mViewMtx +0x08,
 *            mRenderProjMtx +0x38, mReceiverProjMtx +0x78, mpModels +0x24DC
 *   j3dSys.mViewMtx = 0x803EDA58 +0x00 (symbols.txt) — the view set() bakes.
 *
 * Identity: simple shadows are order-only entries in mSimple[]; setSimple's
 * pos arg is the actor's own position field for the common dComIfGd_setShadow
 * wrapper (actor->current.pos — stable per actor). The setSimple hook records
 * slot->posPtr into s_shd_id_live while the build runs; the L-entry snapshot
 * copies it as the prev-build identity. Match = same pos ptr + L1
 * translation delta < SHD_TELEPORT (covers the stack-local &pos reuse in
 * dComIfGd_setShadow where distinct callers share one stack address —
 * different callers sit at different frame depths, and same-depth callers
 * that collide get caught by the distance guard). Slot count/index changes
 * between builds are handled by the ptr-keyed search, not positional match.
 * Real shadows carry a persistent mKey + model pointer — exact identity.
 *
 * Interpolation: simple endpoints are UN-BAKED to world space —
 * W = inv(bakeView) * stored — then lerped and re-baked with the R-frame's
 * actual view (r4 at the draw hook, the same matrix draw() uploads at
 * :1554). This is an exact decomposition, not a view*world lerp: rotation/
 * scale components stay correct because the bake view factors out exactly.
 * Unmatched entries (appeared this tick) get W = live and still gain the
 * camera re-bake. Real matrices are lerped element-wise: between adjacent
 * ticks the light frame varies only by caster motion (rotation is the
 * quasi-static sun direction), so the element-wise mid is a faithful rigid
 * midpoint — same convention as J3D joint lerps and Dusklight's
 * record_final_mtx. Camera cuts / noinject / dup frames skip injection —
 * the live values are the newest endpoint and draw unchanged.
 *
 * Residual: the real-shadow DEPTH texture still renders at 30 Hz — the
 * silhouette pose steps while its projected position tracks. Re-running
 * imageDraw on R-frames stays gated: imageDraw calls J3DModel::viewCalc per
 * shadowed model, which rewrites the very draw-matrix buffers Stage-B
 * injected — its write-set proof is still open (see report). */
/* SHD_CTRL_DRAW / SHD_CTRL_SETSMPL are #defined at the forward-decl block
 * (on_void_render_gate needs SHD_CTRL_DRAW before this section). */
#define J3DSYS_VIEW_EA   0x803EDA58u   /* j3dSys.mViewMtx (+0x00)          */
#define SHDC_SIMPLE_NUM  0x01u
#define SHDC_SIMPLE_ARR  0x04u
#define SHDC_REAL_ARR    0x3408u
#define SHDC_SPAN        0x15E28u      /* sizeof(dDlst_shadowControl_c)    */
#define SHDS_ALPHA  0x00u
#define SHDS_VOL    0x08u
#define SHDS_MTX    0x38u
#define SHDS_SIZE   0x68u
#define SHDR_STATE  0x00u
#define SHDR_ALPHA  0x02u
#define SHDR_KEY    0x04u
#define SHDR_VIEW   0x08u
#define SHDR_PROJ   0x38u
#define SHDR_RECV   0x78u
#define SHDR_MODEL0 0x24DCu
#define SHDR_SIZE   0x2544u
#define SHD_SIMPLE_MAX 128u
#define SHD_REAL_MAX   8u
#define SHD_TELEPORT   300.0f          /* L1 translation cap for a live match */

typedef struct {
    uint32_t key, model0, used;
    uint8_t  alpha;
    J3DMtx   view, recv;
    Mtx44f   proj;
} ShdRealEnt;

static J3DMtx   s_shd_wvol[SHD_SIMPLE_MAX], s_shd_wmtx[SHD_SIMPLE_MAX];
static J3DMtx   s_shd_bvol[SHD_SIMPLE_MAX], s_shd_bmtx[SHD_SIMPLE_MAX];
static uint32_t s_shd_id_snap[SHD_SIMPLE_MAX];
static uint32_t s_shd_id_live[SHD_SIMPLE_MAX]; /* slot->posPtr, current build */
static uint8_t  s_shd_a_snap[SHD_SIMPLE_MAX], s_shd_a_bak[SHD_SIMPLE_MAX];
static uint8_t  s_shd_sinj[SHD_SIMPLE_MAX];
static uint32_t s_shd_snum = 0;             /* entries in the L snapshot     */
static J3DMtx   s_shd_bake;                 /* j3dSys view of the last build */
static uint32_t s_shd_bake_ok = 0;
static ShdRealEnt s_shd_rprev[SHD_REAL_MAX];
static ShdRealEnt s_shd_rbak[SHD_REAL_MAX];
static uint8_t  s_shd_rinj[SHD_REAL_MAX];
static uint32_t s_shd_self = 0, s_shd_armed = 0, s_shd_prev = 0;

/* out = a * b — affine 3x4 concat (mDoMtx_stack_c::concat equivalent). */
static void shd_mtx_concat(const J3DMtx* a, const J3DMtx* b, J3DMtx* out)
{
    float o[3][4];
    int r, c;
    for (r = 0; r < 3; ++r) {
        for (c = 0; c < 3; ++c)
            o[r][c] = a->m[r][0] * b->m[0][c] + a->m[r][1] * b->m[1][c] +
                      a->m[r][2] * b->m[2][c];
        o[r][3] = a->m[r][0] * b->m[0][3] + a->m[r][1] * b->m[1][3] +
                  a->m[r][2] * b->m[2][3] + a->m[r][3];
    }
    memcpy(out->m, o, sizeof(o));
}

/* General affine inverse (mDoMtx_inverse equivalent): 3x3 adjugate over
 * determinant — bake views are rigid but shadow volumes carry scale, and
 * the inverse must round-trip whatever set() produced. 0 = singular. */
static int shd_mtx_inverse(const J3DMtx* in, J3DMtx* out)
{
    const float (*m)[4] = in->m;
    const float det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    float r[3][3], id;
    int i;
    if (!isfinite(det) || fabsf(det) < 1e-12f)
        return 0;
    id = 1.0f / det;
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    for (i = 0; i < 3; ++i) {
        out->m[i][0] = r[i][0];
        out->m[i][1] = r[i][1];
        out->m[i][2] = r[i][2];
        out->m[i][3] = -(r[i][0] * m[0][3] + r[i][1] * m[1][3] +
                         r[i][2] * m[2][3]);
    }
    return 1;
}

/* setSimple entry hook (0x80085274): r3 = control, r4 = cXyz* pos — the
 * identity key — r3+mSimpleNum = the slot the call is about to claim. Runs
 * inside fpcDw, i.e. L-frames only. Also refreshes the pending bake view:
 * every set() bakes the j3dSys view live at call time. */
static void on_shd_setsimple(CPUState* st)
{
    const uint32_t ctrl = (uint32_t)st->gpr[3];
    uint32_t slot;
    if (!(s_enabled && s_split_mode && s_shadow_interp))
        return;
    if (!in_ram(st, ctrl, SHDC_SPAN))
        return;
    slot = rd8_fast(st, ctrl + SHDC_SIMPLE_NUM);
    if (slot < SHD_SIMPLE_MAX)
        s_shd_id_live[slot] = (uint32_t)st->gpr[4];
    if (in_ram(st, J3DSYS_VIEW_EA, 48u)) {
        rd_f32_arr(st, J3DSYS_VIEW_EA, s_shd_bake.m[0], 12u);
        s_shd_bake_ok = 1;
    }
}

/* L-entry snapshot: the entries about to draw are the prev endpoint. Simple
 * matrices are un-baked to world space NOW (inverse work lands on the L
 * frame, off the R-frame's hot path); ids + alpha + real light matrices are
 * captured verbatim. Real matrices are light-space (cMtx_lookAt around the
 * parallel-light vector — the camera view enters only at draw() through
 * j3dSys), so they snapshot/inject fine with no bake view at all; gating
 * the whole snapshot on s_shd_bake_ok would disable real-shadow interp in
 * every scene where no setSimple ran this tick (Link's own shadow is
 * real-type via dComIfGd_setShadow's setReal-first path). */
static void shd_snapshot(CPUState* st, uint32_t ctrl)
{
    J3DMtx ibv, v;
    uint32_t num, i, j;
    if (!(s_enabled && s_split_mode && s_shadow_interp &&
          s_rframe_render && s_j3d_interp))
        return;
    if (!in_ram(st, ctrl, SHDC_SPAN)) {
        s_shd_prev = 0;
        return;
    }
    s_shd_self = ctrl;
    num = rd8_fast(st, ctrl + SHDC_SIMPLE_NUM);
    if (num > SHD_SIMPLE_MAX)
        num = SHD_SIMPLE_MAX;
    s_shd_snum = 0;
    if (num && s_shd_bake_ok && shd_mtx_inverse(&s_shd_bake, &ibv)) {
        s_shd_snum = num;
        for (i = 0; i < num; ++i) {
            const uint32_t e = ctrl + SHDC_SIMPLE_ARR + i * SHDS_SIZE;
            rd_f32_arr(st, e + SHDS_VOL, v.m[0], 12u);
            shd_mtx_concat(&ibv, &v, &s_shd_wvol[i]);
            rd_f32_arr(st, e + SHDS_MTX, v.m[0], 12u);
            shd_mtx_concat(&ibv, &v, &s_shd_wmtx[i]);
            s_shd_a_snap[i] = (uint8_t)rd8_fast(st, e + SHDS_ALPHA);
            s_shd_id_snap[i] = s_shd_id_live[i];
        }
    }
    for (j = 0; j < SHD_REAL_MAX; ++j) {
        const uint32_t e = ctrl + SHDC_REAL_ARR + j * SHDR_SIZE;
        ShdRealEnt* r = &s_shd_rprev[j];
        if (rd8_fast(st, e + SHDR_STATE) != 1u) {
            r->used = 0;
            continue;
        }
        r->used = 1;
        r->key = rd32_fast(st, e + SHDR_KEY);
        r->model0 = rd32_fast(st, e + SHDR_MODEL0);
        r->alpha = (uint8_t)rd8_fast(st, e + SHDR_ALPHA);
        rd_f32_arr(st, e + SHDR_VIEW, r->view.m[0], 12u);
        rd_f32_arr(st, e + SHDR_PROJ, &r->proj.m[0][0], 16u);
        rd_f32_arr(st, e + SHDR_RECV, r->recv.m[0], 12u);
    }
    s_shd_prev = 1;
    ++s_dbg_shdsnap;
}

/* R-entry injection: live entries are the curr endpoint (this slot's fpcDw
 * rebuilt them after the L snapshot). Every written field is backed up for
 * the return-hook restore. viewptr = draw()'s Mtx arg (r4) — the actual
 * camera matrix this pass uploads, already lerped by camview_install. */
static void shd_inject(CPUState* st, uint32_t ctrl, uint32_t viewptr)
{
    J3DMtx vr, ibv;
    uint8_t used[SHD_SIMPLE_MAX];
    uint32_t num, i, j;
    int do_simple = 0;
    const float alpha = s_interp_alpha;
    if (!(s_enabled && s_split_mode && s_shadow_interp &&
          s_rframe_render && s_j3d_interp))
        return;
    if (s_r_dup || s_noinject || s_fol_cut)
        return;
    if (alpha <= 0.0f || alpha >= 1.0f)
        return;
    if (!s_shd_prev)
        return;
    if (!in_ram(st, ctrl, SHDC_SPAN))
        return;
    s_shd_self = ctrl;
    memset(s_shd_sinj, 0, sizeof(s_shd_sinj));
    memset(s_shd_rinj, 0, sizeof(s_shd_rinj));
    num = rd8_fast(st, ctrl + SHDC_SIMPLE_NUM);
    if (num > SHD_SIMPLE_MAX)
        num = SHD_SIMPLE_MAX;
    /* Simples need the bake view (to un-bake live matrices) and this pass's
     * view arg (to re-bake). Reals are light-space and run without either —
     * do not let a simple-free tick disable real-shadow interpolation. */
    if (num && s_shd_bake_ok && in_ram(st, viewptr, 48u)) {
        rd_f32_arr(st, viewptr, vr.m[0], 12u);
        if (shd_mtx_inverse(&s_shd_bake, &ibv))
            do_simple = 1;  /* s_shd_bake = THIS build's bake view */
    }
    memset(used, 0, sizeof(used));
    for (i = 0; do_simple && i < num; ++i) {
        const uint32_t e = ctrl + SHDC_SIMPLE_ARR + i * SHDS_SIZE;
        J3DMtx lv, lm, wv, wm, mv, mm, iv, im;
        uint32_t match = 0xFFFFFFFFu, id;
        rd_f32_arr(st, e + SHDS_VOL, lv.m[0], 12u);
        rd_f32_arr(st, e + SHDS_MTX, lm.m[0], 12u);
        shd_mtx_concat(&ibv, &lv, &wv);   /* live matrices -> world space */
        shd_mtx_concat(&ibv, &lm, &wm);
        s_shd_bvol[i] = lv;               /* backup live values verbatim  */
        s_shd_bmtx[i] = lm;
        s_shd_a_bak[i] = (uint8_t)rd8_fast(st, e + SHDS_ALPHA);
        id = s_shd_id_live[i];
        if (id) {
            /* Nearest-translation match among same-ptr candidates: several
             * actors can share the dComIfGd_setShadow stack-local &pos —
             * pairing each live entry with its closest prev position beats
             * first-fit (wrong pairing shows as a cross-lerped shadow). */
            float best = SHD_TELEPORT;
            for (j = 0; j < s_shd_snum; ++j) {
                float dx, dy, dz, d;
                if (used[j] || s_shd_id_snap[j] != id)
                    continue;
                dx = wv.m[0][3] - s_shd_wvol[j].m[0][3];
                dy = wv.m[1][3] - s_shd_wvol[j].m[1][3];
                dz = wv.m[2][3] - s_shd_wvol[j].m[2][3];
                d = fabsf(dx) + fabsf(dy) + fabsf(dz);
                if (d < best) {
                    best = d;
                    match = j;
                }
            }
            if (match != 0xFFFFFFFFu)
                used[match] = 1;
        }
        if (match != 0xFFFFFFFFu) {
            j3d_lerp_mtx(&s_shd_wvol[match], &wv, alpha, &mv, 1u);
            j3d_lerp_mtx(&s_shd_wmtx[match], &wm, alpha, &mm, 1u);
            {
                const int a = (int)s_shd_a_snap[match] +
                    (int)(alpha * ((float)s_shd_a_bak[i] -
                                   (float)s_shd_a_snap[match]));
                wr8_fast(st, e + SHDS_ALPHA,
                         (uint32_t)(a < 0 ? 0 : (a > 255 ? 255 : a)));
            }
            ++s_dbg_shdinj;
        } else {
            mv = wv;   /* no prev endpoint: latest pose + camera re-bake */
            mm = wm;
        }
        shd_mtx_concat(&vr, &mv, &iv);
        shd_mtx_concat(&vr, &mm, &im);
        wr_f32_arr(st, e + SHDS_VOL, iv.m[0], 12u);
        wr_f32_arr(st, e + SHDS_MTX, im.m[0], 12u);
        s_shd_sinj[i] = 1;
        s_shd_armed = 1;
    }
    for (j = 0; j < SHD_REAL_MAX; ++j) {
        const uint32_t e = ctrl + SHDC_REAL_ARR + j * SHDR_SIZE;
        const ShdRealEnt* p = &s_shd_rprev[j];
        ShdRealEnt* b = &s_shd_rbak[j];
        J3DMtx iv, ir;
        Mtx44f ip;
        float dt;
        int r, c, a;
        if (rd8_fast(st, e + SHDR_STATE) != 1u)
            continue;
        b->key = rd32_fast(st, e + SHDR_KEY);
        b->model0 = rd32_fast(st, e + SHDR_MODEL0);
        b->alpha = (uint8_t)rd8_fast(st, e + SHDR_ALPHA);
        rd_f32_arr(st, e + SHDR_VIEW, b->view.m[0], 12u);
        rd_f32_arr(st, e + SHDR_PROJ, &b->proj.m[0][0], 16u);
        rd_f32_arr(st, e + SHDR_RECV, b->recv.m[0], 12u);
        if (!p->used || p->key != b->key || p->model0 != b->model0)
            continue;   /* slot claimed by a different shadow this tick */
        dt = fabsf(b->view.m[0][3] - p->view.m[0][3]) +
             fabsf(b->view.m[1][3] - p->view.m[1][3]) +
             fabsf(b->view.m[2][3] - p->view.m[2][3]);
        if (dt > SHD_TELEPORT)
            continue;
        j3d_lerp_mtx(&p->view, &b->view, alpha, &iv, 1u);
        j3d_lerp_mtx(&p->recv, &b->recv, alpha, &ir, 1u);
        for (r = 0; r < 4; ++r)
            for (c = 0; c < 4; ++c) {
                const float v = p->proj.m[r][c] +
                    alpha * (b->proj.m[r][c] - p->proj.m[r][c]);
                ip.m[r][c] = isfinite(v) ? v : b->proj.m[r][c];
            }
        wr_f32_arr(st, e + SHDR_VIEW, iv.m[0], 12u);
        wr_f32_arr(st, e + SHDR_PROJ, &ip.m[0][0], 16u);
        wr_f32_arr(st, e + SHDR_RECV, ir.m[0], 12u);
        a = (int)p->alpha + (int)(alpha * ((float)b->alpha - (float)p->alpha));
        wr8_fast(st, e + SHDR_ALPHA,
                 (uint32_t)(a < 0 ? 0 : (a > 255 ? 255 : a)));
        s_shd_rinj[j] = 1;
        s_shd_armed = 1;
        ++s_dbg_shdinj;
    }
}

/* Return hook (0x80084EF0): every field shd_inject wrote goes back to its
 * live value — the same memory is consumed again by the next L Painter, and
 * the real entries persist into imageDraw consumption as well. */
static void on_shd_draw_return(CPUState* st)
{
    const uint32_t ctrl = s_shd_self;
    uint32_t i, j;
    if (!s_shd_armed)
        return;
    s_shd_armed = 0;
    if (in_ram(st, ctrl, SHDC_SPAN)) {
        for (i = 0; i < SHD_SIMPLE_MAX; ++i) {
            const uint32_t e = ctrl + SHDC_SIMPLE_ARR + i * SHDS_SIZE;
            if (!s_shd_sinj[i])
                continue;
            s_shd_sinj[i] = 0;
            wr_f32_arr(st, e + SHDS_VOL, s_shd_bvol[i].m[0], 12u);
            wr_f32_arr(st, e + SHDS_MTX, s_shd_bmtx[i].m[0], 12u);
            wr8_fast(st, e + SHDS_ALPHA, (uint32_t)s_shd_a_bak[i]);
        }
        for (j = 0; j < SHD_REAL_MAX; ++j) {
            const uint32_t e = ctrl + SHDC_REAL_ARR + j * SHDR_SIZE;
            const ShdRealEnt* b = &s_shd_rbak[j];
            if (!s_shd_rinj[j])
                continue;
            s_shd_rinj[j] = 0;
            wr_f32_arr(st, e + SHDR_VIEW, b->view.m[0], 12u);
            wr_f32_arr(st, e + SHDR_PROJ, &b->proj.m[0][0], 16u);
            wr_f32_arr(st, e + SHDR_RECV, b->recv.m[0], 12u);
            wr8_fast(st, e + SHDR_ALPHA, (uint32_t)b->alpha);
        }
    } else {
        memset(s_shd_sinj, 0, sizeof(s_shd_sinj));
        memset(s_shd_rinj, 0, sizeof(s_shd_rinj));
    }
}

/* Call-out inside on_void_render_gate at 0x80084EF0 (the existing render
 * gate already hooks this address — no second registration): L Painter ->
 * snapshot, R Painter -> inject. */
static void shd_ctrl_draw_entry(CPUState* st)
{
    if (s_logic_this_frame)
        shd_snapshot(st, (uint32_t)st->gpr[3]);
    else
        shd_inject(st, (uint32_t)st->gpr[3], (uint32_t)st->gpr[4]);
}

/* ---- animated-material DL payload smoothing (BTK/BRK/BPK; MODERNGEKKO_F60_TEXANIM)
 *
 * Problem: J3DMaterial::diff() re-emits per-frame material state (texture
 * matrices, TEV/konst colors, mat/amb colors) into the init shape packet's
 * differed display list each L-frame. R-frames replay the SAME DL bytes, so
 * texture-SRT / color animation steps at 30 Hz.
 *
 * Design: a J3DMatPacket::endDiff entry hook (0x802DB46C) observes every
 * re-emitted differed DL. At the hook the buffer swap + emission are already
 * done (beginDL ran inside the critical section before us), endDL has not
 * stored mSize yet — but __GDCurrentDL==&sGDLObj and sGDLObj.ptr-start is the
 * exact emitted size, so the payload bytes are final. The hook parses the
 * freshly emitted command stream, picks out the animatable payload slots:
 *   - XF tex-mtx loads     (0x10 hdr, XF addr 0x78+12i, 8 or 12 f32)
 *   - XF mat/amb color prs (0x10 hdr, XF addr 0x100C/0x100A, 2 RGBA32)
 *   - BP TEV-color quads   (0x61 x4: regs eN/eN+1/eN+1/eN+1, 11-bit channels)
 *   - BP konst-color pairs (0x61 x2: regs eN/eN+1 with bit23 set, u8 channels)
 * and snapshots the payload. Discrete texture-pattern commands (GXSetTexImage
 * BP image regs, indirect-mtx BP writes, tex-gen XF config) are never matched,
 * so BTP swaps stay frame-exact by construction.
 *
 * Each R-frame the current slot payloads are lerped against the previous
 * emission (unwrap-on-1.0f for BTK translation columns, raw lerp elsewhere)
 * and written in place. The injected DL bytes live exactly one Painter pair:
 * patched at R-entry, compared+restored at the NEXT Painter entry before any
 * draw consumes them — the same inject/draw/restore discipline as the J3D
 * matrix path. Because the writer region is rewritten wholesale by every
 * emission, patching never fights the double buffer: the lerp lives in
 * mpData[0] while mpData[1] is re-emitted next tick.
 *
 * GPU hazard (COMMON.md rule 8): the write window mirrors the matrix inject —
 * bytes are patched before the FIFO submits the DL pointer and restored one
 * full iteration later; the video thread sees either fully-old or fully-new
 * 4-byte words, never torn commands, and both values are legal payloads. */

/* guest layout (verified against the shipped GZLE01 code, NOT the decomp
 * header comments — those run +4: J3DDrawPacket is really 0x24 bytes, so
 * every J3DMatPacket field sits one word earlier than J3DPacket.h states):
 *   J3DMatPacket      +0x24 mpInitShapePacket   (beginDiff: lwz r3,36(r3))
 *                     +0x28 mpShapePacket +0x2C mpMaterial +0x30 mDiffFlag
 *                     +0x34 mpTexture     +0x38 mpMaterialAnm
 *   J3DDrawPacket     +0x20 mpDisplayListObj    (beginDiff: lwz r3,32(r3))
 *   J3DDisplayListObj +0x00 mpData[0] +0x04 mpData[1] +0x08 mSize +0x0C mCapacity
 *   GDLObj            +0x00 start +0x04 length +0x08 ptr +0x0C top            */
#define MATPKT_OFF_INITSHAPE 0x24u
#define SHPPKT_OFF_DLOBJ     0x20u
#define DLOBJ_OFF_DATA0      0x00u
#define DLOBJ_OFF_SIZE       0x08u
#define DLOBJ_OFF_CAPACITY   0x0Cu
#define GDLOBJ_EA            0x803EDC08u   /* J3DDisplayListObj::sGDLObj        */
#define GDLOBJ_OFF_START     0x00u
#define GDLOBJ_OFF_PTR       0x08u
#define GDCURRENT_EA         0x803F7CA8u   /* __GDCurrentDL                     */
#define J3D_ENDDIFF_EA       0x802DB46Cu   /* J3DMatPacket::endDiff entry       */

/* GX command opcodes recognised by the differed-DL parser. */
#define GXOP_NOP   0x00u
#define GXOP_CP    0x08u
#define GXOP_XF    0x10u
#define GXOP_CALLDL 0x40u
#define GXOP_BP    0x61u

enum { TA_K_TEXMTX = 0, TA_K_XFCOL2 = 1, TA_K_TEV4 = 2, TA_K_KCOL2 = 3 };

#define TA_MAX_RECS   384u
#define TA_HASH_SIZE  1024u                /* power of two                    */
#define TA_MAX_SLOTS  16u
#define TA_SPAN_MAX   48u                  /* largest payload span (texmtx3x4)*/
#define TA_STALE_GENS 6u                   /* unseen-for-N-gens -> evictable  */
#define TA_DL_CAP     0x8000u              /* parse bound sanity clamp        */

typedef struct {
    uint16_t off;      /* byte offset of the slot SPAN inside the DL          */
    uint16_t cmd;      /* XF addr (mtx/col) or BP reg id (tev/kcol) — verify  */
    uint8_t  nbytes;   /* span length: payload for XF kinds, 20/10 for BP     */
    uint8_t  kind;
    uint8_t  has_prev; /* prev[] is a usable endpoint                        */
    uint8_t  dirty;    /* live bytes currently hold lerp[] (restore needed)  */
    uint8_t  prev[TA_SPAN_MAX];
    uint8_t  curr[TA_SPAN_MAX];
    uint8_t  lerp[TA_SPAN_MAX];
} TaSlot;

typedef struct {
    uint32_t dlobj;    /* key: J3DDisplayListObj*                             */
    uint32_t base;     /* mpData[0] at the last sighting                      */
    uint32_t esize;    /* emitted bytes (unpadded) at the last sighting       */
    uint32_t seen_gen; /* s_ta_gen value when last emitted                    */
    uint16_t nslots;
    uint8_t  live;
    uint8_t  injected; /* at least one dirty slot                             */
    TaSlot   slots[TA_MAX_SLOTS];
} TaRec;

static TaRec    s_ta[TA_MAX_RECS];
static uint16_t s_ta_hash[TA_HASH_SIZE];   /* 0=empty, else rec index + 1     */
static uint32_t s_ta_n = 0;                /* high-water live+dead recs       */
static uint32_t s_ta_gen = 0;              /* bumped once per L Painter entry */
static uint32_t ta_hash_of(uint32_t dlobj) { return (dlobj >> 4) & (TA_HASH_SIZE - 1u); }

/* Linear-probe lookup; on miss inserts a live record (reusing a dead or the
 * stalest slot when the table is full — stale hash entries pointing at a
 * recycled rec are harmless because rec->dlobj is the authoritative key). */
static TaRec* ta_find(uint32_t dlobj)
{
    uint32_t h = ta_hash_of(dlobj);
    for (uint32_t p = 0; p < 16u; ++p) {
        const uint32_t slot = (h + p) & (TA_HASH_SIZE - 1u);
        const uint16_t e = s_ta_hash[slot];
        if (e == 0) {
            /* miss — insert here */
            TaRec* r = NULL;
            for (uint32_t i = 0; i < s_ta_n; ++i) {
                if (!s_ta[i].live) { r = &s_ta[i]; break; }
            }
            if (!r && s_ta_n < TA_MAX_RECS) r = &s_ta[s_ta_n++];
            if (!r) {
                /* table full — evict the stalest rec */
                uint32_t best = 0, bs = 0xFFFFFFFFu;
                for (uint32_t i = 0; i < s_ta_n; ++i) {
                    if (s_ta[i].seen_gen < bs) { bs = s_ta[i].seen_gen; best = i; }
                }
                r = &s_ta[best];
                if (bs + TA_STALE_GENS >= s_ta_gen) return NULL;  /* all fresh */
            }
            memset(r, 0, sizeof(*r));
            r->dlobj = dlobj;
            r->live = 1;
            s_ta_hash[slot] = (uint16_t)((r - s_ta) + 1u);
            return r;
        }
        TaRec* r = &s_ta[e - 1u];
        if (r->dlobj == dlobj) return r;
    }
    return NULL;   /* probe chain saturated */
}

static void ta_span_read(CPUState* st, uint32_t addr, uint8_t* dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) dst[i] = (uint8_t)rd8_fast(st, addr + i);
}

static int ta_span_eq(CPUState* st, uint32_t addr, const uint8_t* ref, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (rd8_fast(st, addr + i) != ref[i]) return 0;
    return 1;
}

/* Write a slot span honouring the per-kind payload layout: XF kinds store the
 * whole span, BP kinds write only the u32 after each 0x61 header byte. */
static void ta_span_write(CPUState* st, uint32_t base, const TaSlot* s,
                          const uint8_t* src)
{
    const uint32_t addr = base + s->off;
    if (s->kind == TA_K_TEV4 || s->kind == TA_K_KCOL2) {
        const uint32_t n = (s->kind == TA_K_TEV4) ? 4u : 2u;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t w = ((uint32_t)src[i * 5u + 1u] << 24) |
                               ((uint32_t)src[i * 5u + 2u] << 16) |
                               ((uint32_t)src[i * 5u + 3u] << 8) |
                               (uint32_t)src[i * 5u + 4u];
            wr32_fast(st, addr + i * 5u + 1u, w);
        }
    } else {
        for (uint32_t i = 0; i < s->nbytes; i += 4u) {
            const uint32_t w = ((uint32_t)src[i] << 24) | ((uint32_t)src[i + 1u] << 16) |
                               ((uint32_t)src[i + 2u] << 8) | (uint32_t)src[i + 3u];
            wr32_fast(st, addr + i, w);
        }
    }
}

/* Header signature check: the command at slot.off still has the shape the
 * parser recorded. Verifies before trusting stored offsets across frames. */
static int ta_slot_sig_ok(CPUState* st, uint32_t base, const TaSlot* s)
{
    const uint32_t a = base + s->off;
    if (s->kind == TA_K_TEXMTX || s->kind == TA_K_XFCOL2) {
        const uint32_t h = a - 5u;
        if (rd8_fast(st, h) != GXOP_XF) return 0;
        if (rd16_fast(st, h + 1u) + 1u != s->nbytes / 4u) return 0;
        if (rd16_fast(st, h + 3u) != s->cmd) return 0;
        return 1;
    }
    if (rd8_fast(st, a) != GXOP_BP || rd8_fast(st, a + 1u) != s->cmd) return 0;
    if (s->kind == TA_K_KCOL2)
        return rd8_fast(st, a + 5u) == GXOP_BP && rd8_fast(st, a + 6u) == s->cmd + 1u;
    /* TA_K_TEV4 */
    return rd8_fast(st, a + 5u)  == GXOP_BP && rd8_fast(st, a + 6u)  == s->cmd + 1u &&
           rd8_fast(st, a + 10u) == GXOP_BP && rd8_fast(st, a + 11u) == s->cmd + 1u &&
           rd8_fast(st, a + 15u) == GXOP_BP && rd8_fast(st, a + 16u) == s->cmd + 1u;
}

static int ta_is_texmtx_cmd(uint32_t cmd, uint32_t nwords)
{
    /* J3DGDLoadTexMtxImm: cmd = (GX_TEXMTX0 + i*3)*4 = 0x78+12i (i<8),
     * len = 8 (GX_MTX2x4) or 12 (GX_MTX3x4). */
    if (nwords != 8u && nwords != 12u) return 0;
    return cmd >= 0x78u && cmd <= 0xCCu && ((cmd - 0x78u) % 12u) == 0u;
}

static int ta_is_bp_tev_reg(uint32_t reg)
{
    /* J3DGDSetTevColorS10/J3DGDSetTevKColor base regs: e0,e2,e4,e6. */
    return reg == 0xE0u || reg == 0xE2u || reg == 0xE4u || reg == 0xE6u;
}

/* Walk the freshly emitted command stream and register every animatable
 * payload slot. Unknown opcodes stop the parse — the slots found so far stay
 * valid (they cover complete commands), the tail is simply left unpatched. */
static void ta_parse(CPUState* st, TaRec* r)
{
    const uint32_t base = r->base;
    uint32_t esize = r->esize;
    const uint32_t cap = rd32_fast(st, r->dlobj + DLOBJ_OFF_CAPACITY);
    uint16_t n = 0;
    if (esize > cap) esize = cap;
    if (esize > TA_DL_CAP) esize = TA_DL_CAP;
    r->nslots = 0;
    if (!in_ram(st, base, esize)) return;
    for (uint32_t o = 0; o < esize && n < TA_MAX_SLOTS; ) {
        const uint32_t op = rd8_fast(st, base + o);
        if (op == GXOP_NOP) { ++o; continue; }
        if (op == GXOP_CP) {
            if (o + 6u > esize) break;
            o += 6u;          /* J3DGDWriteCPCmd: u8 + u8 subcmd + u32 — skip */
            continue;
        }
        if (op == GXOP_XF) {
            if (o + 5u > esize) break;
            const uint32_t nwords = rd16_fast(st, base + o + 1u) + 1u;
            const uint32_t cmd = rd16_fast(st, base + o + 3u);
            const uint32_t span = 5u + 4u * nwords;
            if (o + span > esize) break;
            if (ta_is_texmtx_cmd(cmd, nwords)) {
                TaSlot* s = &r->slots[n++];
                memset(s, 0, sizeof(*s));
                s->off = (uint16_t)(o + 5u);
                s->cmd = (uint16_t)cmd;
                s->nbytes = (uint8_t)(4u * nwords);
                s->kind = TA_K_TEXMTX;
            } else if ((cmd == 0x100Cu || cmd == 0x100Au) && nwords == 2u) {
                /* loadMatColors (0x100C) / loadAmbColors (0x100A): 2 RGBA32 */
                TaSlot* s = &r->slots[n++];
                memset(s, 0, sizeof(*s));
                s->off = (uint16_t)(o + 5u);
                s->cmd = (uint16_t)cmd;
                s->nbytes = 8u;
                s->kind = TA_K_XFCOL2;
            }
            o += span;
        } else if (op == GXOP_BP) {
            if (o + 5u > esize) break;
            const uint32_t reg = rd8_fast(st, base + o + 1u);
            if (ta_is_bp_tev_reg(reg)) {
                const uint32_t w0b1 = rd8_fast(st, base + o + 2u);
                if (w0b1 & 0x80u) {
                    /* J3DGDSetTevKColor: 2 cmds {reg, reg+1} with bit23 set */
                    if (o + 10u <= esize &&
                        rd8_fast(st, base + o + 5u) == GXOP_BP &&
                        rd8_fast(st, base + o + 6u) == reg + 1u) {
                        TaSlot* s = &r->slots[n++];
                        memset(s, 0, sizeof(*s));
                        s->off = (uint16_t)o;
                        s->cmd = (uint16_t)reg;
                        s->nbytes = 10u;
                        s->kind = TA_K_KCOL2;
                        o += 10u; continue;
                    }
                } else {
                    /* J3DGDSetTevColorS10: 4 cmds {reg, reg+1, reg+1, reg+1} */
                    if (o + 20u <= esize &&
                        rd8_fast(st, base + o + 5u)  == GXOP_BP && rd8_fast(st, base + o + 6u)  == reg + 1u &&
                        rd8_fast(st, base + o + 10u) == GXOP_BP && rd8_fast(st, base + o + 11u) == reg + 1u &&
                        rd8_fast(st, base + o + 15u) == GXOP_BP && rd8_fast(st, base + o + 16u) == reg + 1u) {
                        TaSlot* s = &r->slots[n++];
                        memset(s, 0, sizeof(*s));
                        s->off = (uint16_t)o;
                        s->cmd = (uint16_t)reg;
                        s->nbytes = 20u;
                        s->kind = TA_K_TEV4;
                        o += 20u; continue;
                    }
                }
            }
            o += 5u;   /* unmatched/other BP command */
        } else {
            break;     /* draw/CP/call/unknown opcodes — not in diff output   */
        }
    }
    r->nslots = n;
    ++s_dbg_ta_reparse;
}

/* Channel lerp helpers. s11: GXColorS10 11-bit signed fields; u8: GXColor. */
static float ta_lerp_f(float p, float c, float a)
{
    if (!(p == p) || !(c == c)) return c;          /* NaN guard -> curr       */
    return p + (c - p) * a;
}
static int32_t ta_lerp_u8(uint32_t p, uint32_t c, float a)
{
    float f = (float)p + ((float)c - (float)p) * a;
    int32_t v = (int32_t)(f + (f >= 0.0f ? 0.5f : -0.5f));
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
static int32_t ta_lerp_s11(uint32_t p, uint32_t c, float a)
{
    const int32_t ps = (int32_t)((p & 0x7FFu) ^ 0x400u) - 0x400;
    const int32_t cs = (int32_t)((c & 0x7FFu) ^ 0x400u) - 0x400;
    float f = (float)ps + ((float)cs - (float)ps) * a;
    int32_t v = (int32_t)(f + (f >= 0.0f ? 0.5f : -0.5f));
    return v < -1024 ? -1024 : (v > 1023 ? 1023 : v);
}

static uint32_t ta_span_u32(const uint8_t* p, uint32_t i)
{
    return ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1u] << 16) |
           ((uint32_t)p[i + 2u] << 8) | (uint32_t)p[i + 3u];
}
static void ta_put_u32(uint8_t* p, uint32_t i, uint32_t w)
{
    p[i] = (uint8_t)(w >> 24); p[i + 1u] = (uint8_t)(w >> 16);
    p[i + 2u] = (uint8_t)(w >> 8); p[i + 3u] = (uint8_t)w;
}

/* Build the lerped span bytes for one slot into slot->lerp. Returns 0 when
 * the result equals curr (static content / edge cases -> leave unpatched). */
static int ta_build_lerp(TaSlot* s, float alpha)
{
    const uint32_t n = s->nbytes;
    memcpy(s->lerp, s->curr, n);
    if (memcmp(s->prev, s->curr, n) == 0) return 0;   /* static emission       */
    switch (s->kind) {
    case TA_K_TEXMTX: {
        const uint32_t nf = n / 4u;
        float maxd = 0.0f;
        for (uint32_t i = 0; i < nf; ++i) {
            const float d = be_f32(&s->curr[i * 4u]) - be_f32(&s->prev[i * 4u]);
            const float ad = d < 0.0f ? -d : d;
            if (ad > maxd) maxd = ad;
        }
        if (maxd > 64.0f) return 0;                 /* teleport scale -> snap  */
        for (uint32_t i = 0; i < nf; ++i) {
            float p = be_f32(&s->prev[i * 4u]);
            float c = be_f32(&s->curr[i * 4u]);
            float d = c - p;
            /* BTK translation wrap: on 2x4 (SRT) matrices the column-3
             * elements carry the UV translation and cyclic BTK tracks wrap
             * there (e.g. +0.98 -> -0.98 continuing the scroll). Any |d|>0.5
             * in a column element is a wrap or a huge one-tick scroll — take
             * the nearest-integer short way either way. 3x4 payloads are
             * view/projection matrices (envmap) whose translation column is
             * real view motion — no unwrap. */
            if (n == 32u && (i == 3u || i == 7u)) {
                const float ad = d < 0.0f ? -d : d;
                if (ad > 0.5f) {
                    const float k = floorf(d + 0.5f);
                    if (k <= 8.0f && k >= -8.0f) d -= k;
                }
            }
            const float v = ta_lerp_f(p, p + d, alpha);
            if (!(v == v)) { wr_be_f32(&s->lerp[i * 4u], c); continue; }
            wr_be_f32(&s->lerp[i * 4u], v);
        }
        return 1;
    }
    case TA_K_XFCOL2:
        for (uint32_t i = 0; i < 8u; ++i)
            s->lerp[i] = (uint8_t)ta_lerp_u8(s->prev[i], s->curr[i], alpha);
        return 1;
    case TA_K_KCOL2: {
        /* 2 cmds; word at rel+1 holds r[0:7]|a[12:19]|bit23|reg<<24,
         * word at rel+6 holds b[0:7]|g[12:19]|bit23|reg+1<<24. */
        const uint32_t w0p = ta_span_u32(s->prev, 1u), w0c = ta_span_u32(s->curr, 1u);
        const uint32_t w1p = ta_span_u32(s->prev, 6u), w1c = ta_span_u32(s->curr, 6u);
        const int32_t r = ta_lerp_u8(w0p & 0xFFu, w0c & 0xFFu, alpha);
        const int32_t a = ta_lerp_u8((w0p >> 12) & 0xFFu, (w0c >> 12) & 0xFFu, alpha);
        const int32_t b = ta_lerp_u8(w1p & 0xFFu, w1c & 0xFFu, alpha);
        const int32_t g = ta_lerp_u8((w1p >> 12) & 0xFFu, (w1c >> 12) & 0xFFu, alpha);
        ta_put_u32(s->lerp, 1u, (w0c & 0xFF000000u) | (1u << 23) |
                                ((uint32_t)a << 12) | (uint32_t)r);
        ta_put_u32(s->lerp, 6u, (w1c & 0xFF000000u) | (1u << 23) |
                                ((uint32_t)g << 12) | (uint32_t)b);
        return 1;
    }
    case TA_K_TEV4: {
        const uint32_t w0p = ta_span_u32(s->prev, 1u), w0c = ta_span_u32(s->curr, 1u);
        const uint32_t w1p = ta_span_u32(s->prev, 6u), w1c = ta_span_u32(s->curr, 6u);
        const int32_t r = ta_lerp_s11(w0p & 0x7FFu, w0c & 0x7FFu, alpha);
        const int32_t a = ta_lerp_s11((w0p >> 12) & 0x7FFu, (w0c >> 12) & 0x7FFu, alpha);
        const int32_t b = ta_lerp_s11(w1p & 0x7FFu, w1c & 0x7FFu, alpha);
        const int32_t g = ta_lerp_s11((w1p >> 12) & 0x7FFu, (w1c >> 12) & 0x7FFu, alpha);
        const uint32_t w0 = (w0c & 0xFF000000u) | (((uint32_t)a & 0x7FFu) << 12) |
                            ((uint32_t)r & 0x7FFu);
        const uint32_t w1 = (w1c & 0xFF000000u) | (((uint32_t)g & 0x7FFu) << 12) |
                            ((uint32_t)b & 0x7FFu);
        ta_put_u32(s->lerp, 1u,  w0);
        ta_put_u32(s->lerp, 6u,  w1);
        ta_put_u32(s->lerp, 11u, w1);
        ta_put_u32(s->lerp, 16u, w1);
        return 1;
    }
    }
    return 0;
}

/* J3DMatPacket::endDiff entry hook (0x802DB46C). r3 = this; runs inside the
 * beginDL/endDL critical section with the emission complete and
 * __GDCurrentDL == &sGDLObj, so sGDLObj.ptr-start is the exact new size and
 * sGDLObj.start == the packet's mpData[0]. Entry hook, not return: endDiff
 * fires per animated material (~300-800x/frame) and RETURN hooks saturate
 * the pending-return scan on hot functions. */
static void on_end_diff(CPUState* st)
{
    if (!s_texanim) return;
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    const uint32_t pkt = (uint32_t)st->gpr[3];
    uint32_t isp, dlobj, base, gstart, gptr, esize;
    TaRec* r;
    int reparse = 0;
    if (!in_ram(st, pkt, 0x40u)) goto out;
    isp = rd32_fast(st, pkt + MATPKT_OFF_INITSHAPE);
    if (!in_ram(st, isp, SHPPKT_OFF_DLOBJ + 4u)) goto out;
    dlobj = rd32_fast(st, isp + SHPPKT_OFF_DLOBJ);
    if (!in_ram(st, dlobj, 0x10u)) goto out;
    /* The critical section guarantees the GDL state belongs to THIS packet:
     * current==sGDLObj and its start==the packet's current buffer. */
    if (rd32_fast(st, GDCURRENT_EA) != GDLOBJ_EA) goto out;
    gstart = rd32_fast(st, GDLOBJ_EA + GDLOBJ_OFF_START);
    gptr = rd32_fast(st, GDLOBJ_EA + GDLOBJ_OFF_PTR);
    base = rd32_fast(st, dlobj + DLOBJ_OFF_DATA0);
    if (gstart != base || gptr < gstart || !in_ram(st, base, 1u)) goto out;
    esize = gptr - gstart;
    r = ta_find(dlobj);
    if (!r) goto out;
    ++s_dbg_ta_sight;
    r->seen_gen = s_ta_gen;
    /* mpData[0] ALTERNATES every emission — a changed base is the normal
     * double-buffer swap, not a shape change: stored offsets are relative to
     * the buffer start and stay valid. Only an esize change or a failed
     * signature check means the emitted command shape actually moved. */
    if (r->esize != esize)
        reparse = 1;
    r->base = base;
    r->esize = esize;
    if (!reparse) {
        /* Verify stored slot signatures on the new emission — a changed
         * diff-mask inserts/removes commands and shifts offsets. */
        for (uint32_t i = 0; i < r->nslots; ++i) {
            const TaSlot* s = &r->slots[i];
            if ((uint32_t)s->off + s->nbytes > esize ||
                !ta_slot_sig_ok(st, base, s)) { reparse = 1; break; }
        }
    }
    if (reparse) {
        ta_parse(st, r);
        /* Fresh parse: current payloads are the first endpoints — no prev. */
        for (uint32_t i = 0; i < r->nslots; ++i) {
            TaSlot* s = &r->slots[i];
            ta_span_read(st, base + s->off, s->curr, s->nbytes);
            s->has_prev = 0;
            s->dirty = 0;
        }
        r->injected = 0;
        goto out;
    }
    /* Same-shape emission: rotate prev <- last emission unconditionally —
     * the endpoint pair is always (previous L emission, current L emission),
     * even when the payload did not change (a static tick must not replay an
     * older pair's motion). The first sighting after a reparse carries
     * has_prev=0 from above; every sighting after that has a real pair. */
    for (uint32_t i = 0; i < r->nslots; ++i) {
        TaSlot* s = &r->slots[i];
        uint8_t tmp[TA_SPAN_MAX];
        ta_span_read(st, base + s->off, tmp, s->nbytes);
        memcpy(s->prev, s->curr, s->nbytes);
        memcpy(s->curr, tmp, s->nbytes);
        s->has_prev = 1;
        s->dirty = 0;   /* emission overwrote any old lerp bytes            */
    }
    r->injected = 0;
out:
    if (s_cost) { s_c_ta_ns += host_now_ns() - t0; ++s_c_ta_n; }
}

/* Per-L-frame Painter entry: advance the sighting generation. Restoration of
 * outstanding patches runs unconditionally at every painter entry (common
 * path), so this only bumps the gen — endDiff sightings during the upcoming
 * fpcDw draw get stamped with it. */
static void texanim_lframe(CPUState* st)
{
    (void)st;
    if (!s_texanim) return;
    ++s_ta_gen;
    /* Reclaim records whose DL stopped re-emitting (material culled/model
     * destroyed) — keeps insert capacity free for new animations. */
    for (uint32_t i = 0; i < s_ta_n; ++i)
        if (s_ta[i].live && s_ta[i].seen_gen + TA_STALE_GENS < s_ta_gen)
            s_ta[i].live = 0;
}

/* Restore any outstanding lerp payloads to their curr endpoints. Runs at
 * every Painter entry (L draw must see authoritative bytes; R noinject/cut
 * paths replay unpatched). The scratch compare makes the write conditional
 * on the bytes still holding exactly what we injected — if the region was
 * recycled or rewritten by something else, leave it alone. */
static void texanim_restore(CPUState* st)
{
    if (!s_texanim) return;
    for (uint32_t i = 0; i < s_ta_n; ++i) {
        TaRec* r = &s_ta[i];
        if (!r->injected) continue;
        r->injected = 0;
        if (!in_ram(st, r->base, r->esize < 1u ? 1u : r->esize)) continue;
        for (uint32_t k = 0; k < r->nslots; ++k) {
            TaSlot* s = &r->slots[k];
            if (!s->dirty) continue;
            s->dirty = 0;
            if (!ta_span_eq(st, r->base + s->off, s->lerp, s->nbytes))
                continue;   /* not our bytes anymore — do not clobber        */
            ta_span_write(st, r->base, s, s->curr);
        }
    }
}

/* R-frame: write lerp(prev,curr,alpha) into each live differed DL still
 * carrying its L emission. Re-resolves mpData[0] per rec — a stale base or
 * an unsighted generation means the record does not describe this frame. */
static void texanim_rframe(CPUState* st, float alpha)
{
    const uint64_t t0 = s_cost ? host_now_ns() : 0;
    if (!s_texanim) return;
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    for (uint32_t i = 0; i < s_ta_n; ++i) {
        TaRec* r = &s_ta[i];
        uint32_t base, esize;
        if (!r->live || r->seen_gen != s_ta_gen) continue;
        if (!in_ram(st, r->dlobj, 0x10u)) continue;
        base = rd32_fast(st, r->dlobj + DLOBJ_OFF_DATA0);
        if (base != r->base) continue;    /* swapped since the sighting      */
        esize = r->esize;
        if (!in_ram(st, base, esize)) continue;
        for (uint32_t k = 0; k < r->nslots; ++k) {
            TaSlot* s = &r->slots[k];
            if (!s->has_prev) continue;
            if (!ta_build_lerp(s, alpha)) continue;   /* static -> leave curr */
            /* Guard: confirm the live payload is still this tick's emission
             * before writing (recycled/recycled buffer -> skip). */
            if (!ta_span_eq(st, base + s->off, s->curr, s->nbytes)) continue;
            ta_span_write(st, base, s, s->lerp);
            s->dirty = 1;
            r->injected = 1;
            ++s_dbg_ta_patch;
        }
    }
    if (s_cost) { s_c_ta2_ns += host_now_ns() - t0; ++s_c_ta2_n; }
}

/* ========== Wave2 cloth: CPU-simulated cloth/sail vertex interpolation ======
 * (MODERNGEKKO_F60_CLOTH_INTERP, default ON; =0 restores retail output
 * exactly — no hooks registered, no writes.)
 *
 * Target: cloth vertex arrays are re-simulated inside fpcM_Execute at 30 Hz,
 * so an R-frame repaint re-issues the same vertices — sails/flags step at
 * 30 Hz while everything interpolated moves at 60 Hz.
 *
 * Every cloth packet in TWW is a double-buffered J3DPacket variant: the sim
 * samples the CURRENT buffer's pointers ("old"), flips an index byte, then
 * writes the NEW pose into the other buffer (e.g. dCloth_packet_c::cloth_move,
 * d_cloth_packet.cpp:135-153 — pPosOld is read before changeCurrentBuff()).
 * So after the L-tick's execute, the stale (index^1) buffer still holds the
 * exact pose the L Painter drew — the perfect lerp prev endpoint, already in
 * guest memory. The R-frame path lerps it IN PLACE toward the current buffer
 * and flips the index, so draw() publishes interpolated arrays. Next L-tick's
 * sim writes into that buffer wholesale before ever reading it — simulation
 * state is never corrupted, and the lerped array stays stable across the
 * async-GPU consume window (no patch-then-restore of pointer-referenced
 * memory within a frame; COMMON.md rule 8 satisfied by construction).
 *
 * Two mechanisms:
 *  (A) DOL cloth: hook draw__15dCloth_packet_c (0x80063728 — the single
 *      non-virtual draw() covering every subclass, incl. packets entered
 *      into the zsort-XLU list by cloth_draw__18dCloth_packetXlu_cFv).
 *      Entry: lerp stale-buffer arrays + flip mCurArr (+0xF8). Return:
 *      restore mCurArr. A per-Painter de-dup list covers packets drawn
 *      twice in one Painter (multi-window) so the stale buffer is lerped
 *      once but drawn correctly each time.
 *  (B) REL cloth packets (sail/goal flag/majuu flag/pirate flag/buoy flag):
 *      rel code is OSLink'd to dynamic guest addresses — cannot be hooked.
 *      Instead a Painter-entry walk scans the dDlst_list_c J3DDrawBuffer
 *      members (mpOpaList/mpXluList/... — every cloth packet entryImm()'d
 *      itself into one during the last fpcDw) and classifies each packet
 *      by vtable:
 *      DynamicModuleControlBase::mFirst -> module name -> OSModuleHeader ->
 *      sectionInfo[5] (.data) base + a baked vtable offset. Matched packets
 *      get the same lerp-into-stale + index flip at Painter ENTRY; the index
 *      is restored at the NEXT Painter entry (before the next execute can
 *      read it) via the s_cloth_flips list.
 *
 * Special cases:
 *  - daSail_packet_c is SINGLE-buffered: m1C3A is written 0 at init and
 *    sail_move rewrites mPos[0] wholesale each tick; slots [1] are dead
 *    pairs we borrow: at each L-entry the walk copies slot0 -> slot1
 *    (prev endpoint), at R the lerp lands in slot1 and m1C3A is set to 1.
 *  - daGFlag_packet_c draws the DERIVED mDPos array (not sim mPos) — the
 *    lerp touches only the three drawn arrays (mDPos/mNrm/mBackNrm).
 *  - Vertex count per packet kind is fixed; every access is in_ram-gated;
 *    a per-vertex displacement check (> CLOTH_WARP_D2) skips lerp on
 *    teleports, sail hoist/lower snaps and other discontinuities.
 *  - Camera cuts (s_fol_cut set by the cam-view path) and s_noinject make
 *    the R-frame a plain repaint: no lerp, no flips.                         */

/* dDlst_list_c inside g_dComIfG_gameInfo (0x803C4C08) at +0x5D1C owns the
 * real J3DDrawBuffer* members (mpOpaListSky @+0x00 .. mpOpaList2D @+0x38).
 * j3dSys.mDrawBuffer[] is NOT walkable here — it aliases whichever list the
 * last set*DrawList installed during fpcDw. Cloth packets enter via
 * j3dSys.getDrawBuffer(0)->entryImm(pkt,0), landing in the window list that
 * was active in their actor's _draw — almost always mpOpaList or mpXluList,
 * but sky/BG/P1 windows can host flag-type packets too, so scan all 15
 * buffer slots; a packet lives in exactly one list's chains. */
#define DDLST_EA           (0x803C4C08u + 0x5D1Cu)
#define DDLST_NBUFS        15u          /* member buf ptrs +0x00..+0x38       */
#define J3DDB_OFF_BUF      0x00u        /* J3DDrawBuffer::mpBuf                 */
#define J3DDB_OFF_BUFSZ    0x04u        /* J3DDrawBuffer::mBufSize              */
#define J3DDB_MAX_SLOTS    1024u        /* sanity cap on the entry table        */

#define DMC_FIRST_EA       0x803F7308u  /* DynamicModuleControlBase::mFirst     */
#define DMC_OFF_LINKCNT    0x00u        /* u16 mLinkCount (isLinked)            */
#define DMC_OFF_NEXT       0x08u        /* mNext                                */
#define DMC_OFF_MODULE     0x10u        /* OSModuleHeader* mModule              */
#define DMC_OFF_NAME       0x1Cu        /* const char* mName ("d_a_sail" ...)   */
#define DMC_WALK_CAP       128u
#define OSM_OFF_SECTBL     0x10u        /* OSModuleInfo::sectionInfoOffset —
                                        * stock OSLink patches it to an
                                        * absolute OSSectionInfo*; this port's
                                        * rel_loader leaves the raw FILE offset
                                        * (observed 0x4C). Both encodings are
                                        * accepted below; same for each entry's
                                        * .offset. */
#define CLOTH_SEC_DATA     5u           /* .data section index (TWW rels are
                                        * uniform: 1=.text 2=.ctors 3=.dtors
                                        * 4=.rodata 5=.data 6=.bss — verified
                                        * against d_a_sail.rel/d_a_goal_flag  */

#define CLOTH_MAX_VERTS    1024u        /* hard cap: sanity for DOL n=w*h       */
#define CLOTH_CHUNK        32u          /* verts per stack-buffer pass          */
#define CLOTH_MAX_FLIPS    32u          /* tracked rel index flips per R frame  */
#define CLOTH_SEEN_MAX     32u          /* DOL per-Painter lerp de-dup          */
/* Teleport/warp guard: if any vertex moved more than this (squared units —
 * a sail whip is a few units/tick, a warp is hundreds) between the two
 * endpoints, the pair is not adjacent-tick-continuous -> plain repaint. */
#define CLOTH_WARP_D2      (50.0f * 50.0f)

#define CLOTH_SPEC_SAIL    0x01u        /* single-buffered (dead slot1 = prev) */

typedef struct {
    const char* rel;        /* DynamicModuleControl mName match               */
    uint32_t    vtab_off;   /* packet vtable offset inside .data              */
    uint32_t    n;          /* vertex count per array                         */
    uint32_t    idx_off;    /* current-buffer index field offset              */
    uint8_t     idx_s32;    /* 1 => s32 index, 0 => u8                        */
    uint8_t     flags;
    uint32_t    pos_off;    /* drawn pos array, slot s at off + s*stride      */
    uint32_t    nrm_off;    /* front normals                                  */
    uint32_t    bck_off;    /* back normals                                   */
    uint32_t    stride;     /* slot stride in bytes                           */
    uint32_t    span;       /* packet bytes probed (in_ram gate)              */
} ClothSpec;

static const ClothSpec s_cloth_specs[] = {
    /* daSail_packet_c (d_a_sail.rel) — the pirate ship's sail.
     * vtab __vt__15daSail_packet_c @ .data+0x1434; draw mPos/mNrm/mBackNrm
     * [m1C3A], 84 verts (12x7); m1C3A u8 @+0x1C3A pinned 0. */
    { "d_a_sail",        0x1434u,  84u, 0x1C3Au, 0, CLOTH_SPEC_SAIL,
      0x00A4u, 0x0C74u, 0x1454u, 84u * 12u, 0x1C40u },
    /* daGFlag_packet_c (d_a_goal_flag.rel) — goal flags.
     * vtab @ .data+0x25F0; drawn arrays mDPos/mNrm/mBackNrm[mCurrArr],
     * 45 verts (9x5); mCurrArr u8 @+0x1384. Sim-only mPos/mVelocity
     * untouched. */
    { "d_a_goal_flag",   0x25F0u,  45u, 0x1384u, 0, 0,
      0x04C0u, 0x08F8u, 0x0D30u, 45u * 12u, 0x1388u },
    /* daMajuu_Flag_packet_c (d_a_majuu_flag.rel) — dragon roost banners.
     * vtab @ .data+0x0F68; mpPosArr/mpNrmArr/mpNrmArrBack[mCurArr],
     * 21 verts (7x3); mCurArr u8 @+0x79A. */
    { "d_a_majuu_flag",  0x0F68u,  21u, 0x079Au, 0, 0,
      0x00A0u, 0x02A0u, 0x04A0u, 21u * 12u, 0x079Cu },
    /* daPirate_Flag_packet_c (d_a_pirate_flag.rel) — small pirate flags.
     * vtab @ .data+0x0384; mPos/mNrm/mBackNrm[m87E], 25 verts (5x5);
     * m87E u8 @+0x87E. */
    { "d_a_pirate_flag", 0x0384u,  25u, 0x087Eu, 0, 0,
      0x0044u, 0x029Cu, 0x04F4u, 25u * 12u, 0x0880u },
    /* daObjBuoyflag::Packet_c (d_a_obj_buoyflag.rel) — buoy flags, common
     * while sailing. vtab @ .data+0x1B40; mDrawVtx[2] slots {pos[35]@0,
     * nrm[35]@+0x1A4, nrm2(back)[35]@+0x348}; mB8C s32 @+0xB8C. */
    { "d_a_obj_buoyflag",0x1B40u,  35u, 0x0B8Cu, 1, 0,
      0x0010u, 0x01B4u, 0x0358u, 0x04ECu, 0x0B90u },
};
#define CLOTH_NSPEC ((uint32_t)(sizeof(s_cloth_specs) / sizeof(s_cloth_specs[0])))

/* dCloth_packet_c::draw hook target (DOL; covers all subclasses). */
#define DCLOTH_DRAW_EA     0x80063728u
#define DCLOTH_OFF_FLY     0x10u        /* mFlyGridSize                       */
#define DCLOTH_OFF_HOIST   0x14u        /* mHoistGridSize                     */
#define DCLOTH_OFF_POS     0x28u        /* cXyz* mpPosArr[2]                  */
#define DCLOTH_OFF_NRM     0x30u        /* cXyz* mpNrmArr[2]                  */
#define DCLOTH_OFF_BCK     0x38u        /* cXyz* mpNrmArrBack[2]              */
#define DCLOTH_OFF_CUR     0xF8u        /* u8 mCurArr                         */
#define DCLOTH_SPAN        0xFCu        /* object probe span                  */

/* Rel flip bookkeeping: one entry per packet whose index byte was flipped
 * at the last R-frame Painter entry — restored at the next entry. */
typedef struct {
    uint32_t pkt;
    uint32_t orig;
    uint8_t  spec;
} ClothFlip;
static ClothFlip s_cloth_flips[CLOTH_MAX_FLIPS];
static uint32_t  s_cloth_nflips = 0;
/* DOL draw-hook arm state (entry flips, return restores). */
static uint32_t  s_cloth_dol_armed = 0;
static uint32_t  s_cloth_dol_self = 0;
static uint8_t   s_cloth_dol_orig = 0;
/* Packets already lerped this Painter pass (multi-window de-dup). */
static uint32_t  s_cloth_seen[CLOTH_SEEN_MAX];
static uint32_t  s_cloth_seen_n = 0;

static int cloth_str_eq(CPUState* st, uint32_t ea, const char* lit)
{
    /* Guest C-string vs literal, byte-wise, bounded + in_ram-gated. */
    uint32_t i;
    for (i = 0; i < 24u; ++i) {
        if (!in_ram(st, ea + i, 1u)) return 0;
        const uint8_t g = (uint8_t)rd8_fast(st, ea + i);
        const uint8_t l = (uint8_t)lit[i];
        if (g != l) return 0;
        if (g == 0u) return 1;
    }
    return 0;
}

/* Resolve every spec's .data runtime base in ONE walk of the
 * DynamicModuleControlBase list. bases[] zeroed by caller. A base is set
 * only when the module is linked (mLinkCount != 0, mModule != 0) and the
 * section table is readable — stale bases never survive a module unload. */
static void cloth_resolve_bases(CPUState* st, uint32_t* bases, uint32_t nb)
{
    uint32_t node = rd32_fast(st, DMC_FIRST_EA);
    uint32_t found = 0;
    uint32_t it;
    for (it = 0; node && it < DMC_WALK_CAP && found < nb; ++it) {
        uint32_t mod, nm, i;
        if (!in_ram(st, node, 0x20u)) return;
        ++s_cloth_dbg_nodes;
        mod = rd32_fast(st, node + DMC_OFF_MODULE);
        nm  = rd32_fast(st, node + DMC_OFF_NAME);
        /* Linked modules only: mLinkCount != 0 keeps a mounted-but-unlinked
         * module (mid scene swap) from handing out a stale base. */
        if (!mod || !nm || rd16_fast(st, node + DMC_OFF_LINKCNT) == 0u) {
            node = rd32_fast(st, node + DMC_OFF_NEXT);
            continue;
        }
        ++s_cloth_dbg_linked;
        if (s_debug && !s_cloth_dbg_dumped && s_cloth_dbg_linked <= 96u) {
            char buf[32]; uint32_t c;
            for (c = 0; c < 31u; ++c) {
                if (!in_ram(st, nm + c, 1u)) break;
                buf[c] = (char)rd8_fast(st, nm + c);
                if (!buf[c]) break;
            }
            buf[c] = 0;
            const uint32_t tbl0 = in_ram(st, mod, 0x14u)
                ? rd32_fast(st, mod + OSM_OFF_SECTBL) : 0u;
            const uint32_t tbl = (tbl0 >= 0x80000000u) ? tbl0 : mod + tbl0;
            uint32_t s5o = 0, s5s = 0;
            if (in_ram(st, tbl, (CLOTH_SEC_DATA + 1u) * 8u)) {
                s5o = rd32_fast(st, tbl + CLOTH_SEC_DATA * 8u);
                s5s = rd32_fast(st, tbl + CLOTH_SEC_DATA * 8u + 4u);
            }
            fprintf(stderr, "[f60-cloth-dmc] %u: %s mod=%08X tbl=%08X->%08X sec5=%08X+%08X\n",
                    (unsigned)s_cloth_dbg_linked, buf, (unsigned)mod,
                    (unsigned)tbl0, (unsigned)tbl, (unsigned)s5o, (unsigned)s5s);
        }
        for (i = 0; i < nb; ++i) {
            if (bases[i] || !cloth_str_eq(st, nm, s_cloth_specs[i].rel))
                continue;
            const uint32_t tbl0 = in_ram(st, mod, 0x14u)
                ? rd32_fast(st, mod + OSM_OFF_SECTBL) : 0u;
            const uint32_t tbl = (tbl0 >= 0x80000000u) ? tbl0 : mod + tbl0;
            if (in_ram(st, tbl, (CLOTH_SEC_DATA + 1u) * 8u) &&
                s_cloth_specs[i].vtab_off <
                    rd32_fast(st, tbl + CLOTH_SEC_DATA * 8u + 4u)) {
                const uint32_t soff = rd32_fast(st, tbl + CLOTH_SEC_DATA * 8u);
                bases[i] = (soff >= 0x80000000u) ? soff : mod + soff;
                s_cloth_dbg_bas |= (1ull << i);
                ++found;
            }
            break;      /* module names are unique — done with this node */
        }
        node = rd32_fast(st, node + DMC_OFF_NEXT);
    }
    /* One-shot: keep the dump open until a walk actually sees a linked
     * module — an early boot-time call can walk an all-unlinked list. */
    if (s_cloth_dbg_linked)
        s_cloth_dbg_dumped = 1;
}

static int cloth_classify(const uint32_t* bases, uint32_t nb, uint32_t vtab)
{
    uint32_t i;
    for (i = 0; i < nb; ++i)
        if (bases[i] && vtab == bases[i] + s_cloth_specs[i].vtab_off)
            return (int)i;
    return -1;
}

static uint32_t cloth_idx_read(CPUState* st, const ClothSpec* sp, uint32_t pkt)
{
    return sp->idx_s32 ? rd32_fast(st, pkt + sp->idx_off)
                       : rd8_fast(st, pkt + sp->idx_off);
}
static void cloth_idx_write(CPUState* st, const ClothSpec* sp, uint32_t pkt,
                            uint32_t v)
{
    if (sp->idx_s32) wr32_fast(st, pkt + sp->idx_off, v);
    else             wr8_fast(st, pkt + sp->idx_off, v);
}

/* Lerp one cXyz array: dst[i] = a + alpha*(b-a); normals (nrm=1) are
 * renormalized after the blend. Chunked through small stack buffers so any
 * vertex count up to CLOTH_MAX_VERTS works. Everything is in_ram-gated;
 * non-finite results fall back to the src value (same policy as J3D). */
static int cloth_lerp_arr(CPUState* st, uint32_t da, uint32_t sa,
                          uint32_t n, float alpha, int nrm)
{
    float pa[CLOTH_CHUNK * 3u], ca[CLOTH_CHUNK * 3u];
    uint32_t done = 0;
    while (done < n) {
        const uint32_t c = (n - done < CLOTH_CHUNK) ? n - done : CLOTH_CHUNK;
        uint32_t i;
        if (!in_ram(st, da + done * 12u, c * 12u) ||
            !in_ram(st, sa + done * 12u, c * 12u))
            return 0;
        rd_f32_arr(st, da + done * 12u, pa, c * 3u);
        rd_f32_arr(st, sa + done * 12u, ca, c * 3u);
        for (i = 0; i < c * 3u; ++i) {
            float v = pa[i] + alpha * (ca[i] - pa[i]);
            if (!isfinite(v)) v = ca[i];
            pa[i] = v;
        }
        if (nrm) {
            for (i = 0; i < c; ++i) {
                float* v = &pa[i * 3u];
                const float l2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
                if (l2 > 1e-12f) {
                    const float s = 1.0f / sqrtf(l2);
                    v[0] *= s; v[1] *= s; v[2] *= s;
                }
            }
        }
        wr_f32_arr(st, da + done * 12u, pa, c * 3u);
        done += c;
    }
    return 1;
}

/* Warp check over positions only: max per-vertex |b-a|^2 > CLOTH_WARP_D2
 * means the endpoints are not consecutive-tick-continuous. */
static int cloth_warped(CPUState* st, uint32_t da, uint32_t sa, uint32_t n)
{
    float pa[CLOTH_CHUNK * 3u], ca[CLOTH_CHUNK * 3u];
    uint32_t done = 0;
    while (done < n) {
        const uint32_t c = (n - done < CLOTH_CHUNK) ? n - done : CLOTH_CHUNK;
        uint32_t i;
        if (!in_ram(st, da + done * 12u, c * 12u) ||
            !in_ram(st, sa + done * 12u, c * 12u))
            return 1;   /* can't verify — treat as unsafe */
        rd_f32_arr(st, da + done * 12u, pa, c * 3u);
        rd_f32_arr(st, sa + done * 12u, ca, c * 3u);
        for (i = 0; i < c; ++i) {
            const float dx = ca[i*3u] - pa[i*3u];
            const float dy = ca[i*3u+1u] - pa[i*3u+1u];
            const float dz = ca[i*3u+2u] - pa[i*3u+2u];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (!isfinite(d2) || d2 > CLOTH_WARP_D2) return 1;
        }
        done += c;
    }
    return 0;
}

/* Whole-packet lerp for the rel spec layout: slot dst <- lerp(dst, src).
 * Covers pos + front nrm + back nrm. Returns 1 when all three arrays were
 * lerped (caller may then flip the index). */
static int cloth_lerp_slot(CPUState* st, const ClothSpec* sp, uint32_t pkt,
                           uint32_t dst, uint32_t src, float alpha)
{
    const uint32_t dp = pkt + sp->pos_off + dst * sp->stride;
    const uint32_t cp = pkt + sp->pos_off + src * sp->stride;
    if (cloth_warped(st, dp, cp, sp->n)) return 0;
    if (!cloth_lerp_arr(st, dp, cp, sp->n, alpha, 0)) return 0;
    if (!cloth_lerp_arr(st, pkt + sp->nrm_off + dst * sp->stride,
                        pkt + sp->nrm_off + src * sp->stride,
                        sp->n, alpha, 1)) return 0;
    if (!cloth_lerp_arr(st, pkt + sp->bck_off + dst * sp->stride,
                        pkt + sp->bck_off + src * sp->stride,
                        sp->n, alpha, 1)) return 0;
    return 1;
}

/* Flat byte copy of one slot triple (sail L-frame snapshot: slot1 <- slot0).
 * Each array is copied verbatim — normals are already unit-length. */
static void cloth_copy_slot(CPUState* st, const ClothSpec* sp, uint32_t pkt,
                            uint32_t dst, uint32_t src)
{
    const uint32_t offs[3] = { sp->pos_off, sp->nrm_off, sp->bck_off };
    uint32_t a;
    for (a = 0; a < 3u; ++a) {
        const uint32_t d = pkt + offs[a] + dst * sp->stride;
        const uint32_t s = pkt + offs[a] + src * sp->stride;
        uint32_t done = 0;
        while (done < sp->n * 12u) {
            uint32_t w = sp->n * 12u - done;
            if (w > 0x100u) w = 0x100u;
            if (!in_ram(st, d + done, w) || !in_ram(st, s + done, w)) break;
            memcpy(st->ram + (d + done - 0x80000000u),
                   st->ram + (s + done - 0x80000000u), w);
            done += w;
        }
    }
}

/* Per-packet work for the Painter-entry walk. On R-render frames: lerp the
 * stale slot toward current + flip the index (recorded for next-entry
 * restore). On L frames: restore nothing here (cloth_restore_flips did it)
 * — just refresh the sail's prev-pose snapshot (slot1 <- slot0, taken while
 * slot0 still holds the pose this L Painter draws, i.e. last tick's). */
static void cloth_visit_packet(CPUState* st, uint32_t pkt, int spec_i,
                               int rframe, float alpha)
{
    const ClothSpec* sp = &s_cloth_specs[spec_i];
    uint32_t idx, dst, src, i;
    if (!in_ram(st, pkt, sp->span)) return;
    idx = cloth_idx_read(st, sp, pkt);
    if (!rframe) {
        if ((sp->flags & CLOTH_SPEC_SAIL) && idx == 0u)
            cloth_copy_slot(st, sp, pkt, 1u, 0u);
        return;
    }
    if (idx > 1u) return;
    if (s_cloth_nflips >= CLOTH_MAX_FLIPS) return; /* unrecorded flips leak:
        * the index would stay flipped past the next execute — for the sail
        * (pinned-0 invariant) that would break it outright. Repaint instead. */
    for (i = 0; i < s_cloth_nflips; ++i)
        if (s_cloth_flips[i].pkt == pkt) return;  /* already flipped this R   */
    if (sp->flags & CLOTH_SPEC_SAIL) {
        if (idx != 0u) return;       /* pinned-0 invariant violated — bail   */
        dst = 1u; src = 0u;
    } else {
        dst = idx ^ 1u; src = idx;
    }
    if (!cloth_lerp_slot(st, sp, pkt, dst, src, alpha)) {
        ++s_cloth_dbg_skip;
        return;
    }
    cloth_idx_write(st, sp, pkt, dst);
    ++s_cloth_dbg_lerp;
    s_cloth_flips[s_cloth_nflips].pkt = pkt;
    s_cloth_flips[s_cloth_nflips].orig = idx;
    s_cloth_flips[s_cloth_nflips].spec = (uint8_t)spec_i;
    ++s_cloth_nflips;
}

/* Restore every index flipped by the last R walk. A flip is only written
 * back if the packet still classifies as the same spec (vtable check) —
 * protects a slot whose packet was freed/reused between frames. bases may
 * be partially zero; unmatched flips are then dropped silently (the index
 * write into reused memory is skipped). */
static void cloth_restore_flips(CPUState* st, const uint32_t* bases, uint32_t nb)
{
    uint32_t i;
    for (i = 0; i < s_cloth_nflips; ++i) {
        ClothFlip* f = &s_cloth_flips[i];
        const ClothSpec* sp = &s_cloth_specs[f->spec];
        if (f->spec < nb && bases[f->spec] &&
            in_ram(st, f->pkt, sp->span) &&
            rd32_fast(st, f->pkt) == bases[f->spec] + sp->vtab_off)
            cloth_idx_write(st, sp, f->pkt, f->orig);
    }
    s_cloth_nflips = 0;
}

#define J3DPKT_OFF_NEXT    0x04u        /* J3DPacket::mpNextPacket (chain)    */
#define J3DPKT_CHAIN_CAP   4096u        /* hard bound vs corrupt next ptrs    */

/* Scan the dDlst_list_c J3DDrawBuffer* members (+0x00..+0x38) -> mpBuf.
 * entryImm pushes each packet onto the mpBuf[index] HEAD and links the old
 * head via mpNextPacket, so every non-null slot is a chain (drawHead walks
 * it the same way). Classify each packet by vtable -> cloth_visit_packet. */
static void cloth_walk_drawbufs(CPUState* st, const uint32_t* bases, uint32_t nb,
                                int rframe, float alpha)
{
    uint32_t bi, i;
    for (bi = 0; bi < DDLST_NBUFS; ++bi) {
        const uint32_t db = rd32_fast(st, DDLST_EA + bi * 4u);
        uint32_t buf, n;
        if (!in_ram(st, db, 8u)) continue;
        buf = rd32_fast(st, db + J3DDB_OFF_BUF);
        n   = rd32_fast(st, db + J3DDB_OFF_BUFSZ);
        if (!in_ram(st, buf, 4u) || n > J3DDB_MAX_SLOTS) continue;
        for (i = 0; i < n && in_ram(st, buf + i * 4u, 4u); ++i) {
            uint32_t pkt = rd32_fast(st, buf + i * 4u);
            uint32_t hops = 0;
            while (pkt && hops < J3DPKT_CHAIN_CAP && in_ram(st, pkt, 4u)) {
                const uint32_t vt = rd32_fast(st, pkt);
                const int spec_i = cloth_classify(bases, nb, vt);
                if (rframe) ++s_cloth_dbg_pkt;
                if (spec_i >= 0) {
                    if (rframe) ++s_cloth_dbg_rel;
                    cloth_visit_packet(st, pkt, spec_i, rframe, alpha);
                } else if (rframe) {
                    s_cloth_dbg_vt0 = vt;
                }
                if (!in_ram(st, pkt + J3DPKT_OFF_NEXT, 4u)) break;
                pkt = rd32_fast(st, pkt + J3DPKT_OFF_NEXT);
                ++hops;
            }
        }
    }
}

/* Every-Painter-entry housekeeping (called from on_painter_skip for ALL
 * frame classes): undo outstanding index flips + a leaked DOL arm, clear
 * the per-Painter de-dup list, and on L frames refresh the sail snapshot.
 * Runs even on dup/unsplit entries — a flip can only be outstanding when
 * an R-render Painter just ran, and the next execute MUST see the real
 * index before it samples the "old" buffer. */
static void cloth_painter_entry(CPUState* st)
{
    uint32_t bases[CLOTH_NSPEC];
    uint32_t need;
    /* Cheap early-out: nothing outstanding and feature off/not in an
     * interp-capable split. The DOL arm is checked unconditionally — a
     * draw-return hook misfire must never leak a flipped mCurArr. */
    if (s_cloth_dol_armed) {
        s_cloth_dol_armed = 0;
        if (in_ram(st, s_cloth_dol_self, DCLOTH_SPAN))
            wr8_fast(st, s_cloth_dol_self + DCLOTH_OFF_CUR, s_cloth_dol_orig);
    }
    s_cloth_seen_n = 0;
    need = s_cloth_nflips ||
           (s_cloth_interp && s_enabled && s_split_mode &&
            s_logic_this_frame && s_rframe_render && s_j3d_interp);
    if (!need) return;
    memset(bases, 0, sizeof(bases));
    cloth_resolve_bases(st, bases, CLOTH_NSPEC);
    cloth_restore_flips(st, bases, CLOTH_NSPEC);
    if (s_cloth_interp && s_logic_this_frame)
        cloth_walk_drawbufs(st, bases, CLOTH_NSPEC, 0, 0.0f);
}

/* R-render path (called inside the s_rframe_render && s_j3d_interp branch
 * of on_painter_skip, next to jpa_rframe): lerp rel packets' stale slots
 * and flip their indices for this Painter. */
static void cloth_rframe(CPUState* st, float alpha)
{
    uint32_t bases[CLOTH_NSPEC];
    if (!s_cloth_interp) return;
    if (s_noinject || s_fol_cut) return;
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    memset(bases, 0, sizeof(bases));
    cloth_resolve_bases(st, bases, CLOTH_NSPEC);
    cloth_walk_drawbufs(st, bases, CLOTH_NSPEC, 1, alpha);
}

/* ---- DOL cloth: dCloth_packet_c::draw entry/return hooks --------------------
 * this = r3. Entry (R-render only): lerp stale buffer (mCurArr^1) arrays in
 * place -> flip mCurArr so draw() publishes the lerped pos/nrm/backnrm.
 * Return: restore mCurArr. The stale buffer is fully rewritten by the next
 * cloth_move before the sim reads it (it only ever reads the CURRENT
 * buffer), so nothing authoritative is disturbed. */
/* De-dup: packets are added to s_cloth_seen ONLY after a successful lerp —
 * a failed first attempt must not mark the packet (a second draw would
 * dedup-flip into an un-lerped stale buffer). */
static int cloth_dol_is_seen(uint32_t pkt)
{
    uint32_t i;
    for (i = 0; i < s_cloth_seen_n; ++i)
        if (s_cloth_seen[i] == pkt) return 1;
    return 0;
}
static void cloth_dol_mark(uint32_t pkt)
{
    if (s_cloth_seen_n < CLOTH_SEEN_MAX)
        s_cloth_seen[s_cloth_seen_n++] = pkt;
}

static void on_cloth_draw_entry(CPUState* st)
{
    float alpha;
    uint32_t self, cur, n, i;
    uint32_t pa, pb, na, nb, ba, bb;
    int dedup, ok = 0;
    ++s_cloth_dbg_dhit;
    if (!s_cloth_interp) return;
    if (s_logic_this_frame || s_r_dup || s_noinject || s_fol_cut) return;
    if (!(s_split_mode && s_rframe_render && s_j3d_interp)) return;
    alpha = s_interp_alpha;
    if (alpha <= 0.0f || alpha >= 1.0f) return;
    self = st->gpr[3];
    if (!in_ram(st, self, DCLOTH_SPAN)) return;
    ++s_cloth_dbg_dol;   /* reached the work gates on an R-render frame */
    cur = rd8_fast(st, self + DCLOTH_OFF_CUR) & 1u;
    n = rd32_fast(st, self + DCLOTH_OFF_FLY) *
        rd32_fast(st, self + DCLOTH_OFF_HOIST);
    if (n == 0u || n > CLOTH_MAX_VERTS) return;
    dedup = cloth_dol_is_seen(self);
    if (!dedup) {
        pa = rd32_fast(st, self + DCLOTH_OFF_POS + cur * 4u);
        pb = rd32_fast(st, self + DCLOTH_OFF_POS + (cur ^ 1u) * 4u);
        na = rd32_fast(st, self + DCLOTH_OFF_NRM + cur * 4u);
        nb = rd32_fast(st, self + DCLOTH_OFF_NRM + (cur ^ 1u) * 4u);
        ba = rd32_fast(st, self + DCLOTH_OFF_BCK + cur * 4u);
        bb = rd32_fast(st, self + DCLOTH_OFF_BCK + (cur ^ 1u) * 4u);
        {
            const uint32_t a[6] = { pa, pb, na, nb, ba, bb };
            ok = 1;
            for (i = 0; i < 6u; ++i)
                if (!a[i] || !in_ram(st, a[i], n * 12u)) ok = 0;
        }
        if (!ok) return;
        if (cloth_warped(st, pb, pa, n)) { ++s_cloth_dbg_skip; return; }
        if (!cloth_lerp_arr(st, pb, pa, n, alpha, 0)) return;
        if (!cloth_lerp_arr(st, nb, na, n, alpha, 1)) return;
        if (!cloth_lerp_arr(st, bb, ba, n, alpha, 1)) return;
        cloth_dol_mark(self);   /* only now: a later draw may safely dedup */
        ++s_cloth_dbg_lerp;
    }
    /* Flip AFTER success (or on a de-duped second draw — the lerped buffer
     * is still in place): draw() then publishes the interpolated arrays;
     * a failed lerp leaves mCurArr alone = plain repaint. */
    wr8_fast(st, self + DCLOTH_OFF_CUR, cur ^ 1u);
    s_cloth_dol_self = self;
    s_cloth_dol_orig = (uint8_t)cur;
    s_cloth_dol_armed = 1;
}

static void on_cloth_draw_return(CPUState* st)
{
    if (!s_cloth_dol_armed) return;
    s_cloth_dol_armed = 0;
    if (in_ram(st, s_cloth_dol_self, DCLOTH_SPAN))
        wr8_fast(st, s_cloth_dol_self + DCLOTH_OFF_CUR, s_cloth_dol_orig);
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
        texanim_restore(st);   /* outstanding DL payload lerps              */
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
    s_ovlp_peek = 0;
    s_ovlp_phase = 0;
    s_ovlp_peek_dbg = 0;
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
    s_c_jpa_ns = s_c_jpa_n = 0;
    s_c_cam_ns = s_c_cam_n = 0;
    s_c_fol_ns = s_c_fol_n = 0;
    s_c_lit_ns = s_c_lit_n = 0;
    s_hook_calls = s_hook_calls_w = 0;
    s_c_clo_ns = s_c_clo_n = 0;
    s_dbg_jfast = s_dbg_jmiss = 0;
    s_dbg_lframes = s_dbg_rframes = 0;
    s_dbg_snapcall = s_dbg_skipni = 0;
    s_dbg_replays = s_dbg_capbytes = s_dbg_patched = s_dbg_nofifo = 0;
    s_dbg_np = s_dbg_nc = 0;
    s_dbg_purged = 0;
    s_dbg_ovr_dup = s_dbg_w0_dup = s_dbg_camcut = s_dbg_caminj = 0;
    s_dbg_ovr_rep = 0;
    s_mdllog_l = 0;
    s_dbg_lightgate = 0;
    s_dbg_gp_dup = s_dbg_wnum_dup = s_dbg_cap_dup = 0;
    s_dbg_lightfix = s_dbg_lightbad = 0;
    s_dbg_fader_calls = 0;
    s_dbg_folinj = s_dbg_jpainj = s_dbg_jpaeinj = 0;
    s_dbg_shdinj = s_dbg_shdsnap = 0;
    /* D5: a disengage edge can land between the R inject and the return
     * restore — replay the restore, then drop the snapshot. */
    if (st)
        on_shd_draw_return(st);
    s_shd_armed = 0;
    s_shd_prev = 0;
    s_shd_bake_ok = 0;
    s_shd_self = 0;
    s_shd_snum = 0;
    s_dbg_entrymd = s_dbg_calcent = s_dbg_calcret = 0;
    s_dbg_vcent = s_dbg_vcret = s_dbg_dtor = 0;
    fol_reset(&s_fol_grass);
    fol_reset(&s_fol_flower);
    fol_reset(&s_fol_tree);
    /* Restore any lerped particle/emitter state before dropping the
     * tables — a disengage edge can land between an R inject and the next
     * L entry. */
    if (st) jpa_restore(st);
    memset(s_jpa, 0, sizeof(s_jpa));
    memset(s_ta, 0, sizeof(s_ta));
    memset(s_ta_hash, 0, sizeof(s_ta_hash));
    s_ta_n = 0;
    s_ta_gen = 0;
    s_dbg_ta_sight = s_dbg_ta_patch = s_dbg_ta_reparse = 0;
    s_c_ta_ns = s_c_ta_n = 0;
    s_c_ta2_ns = s_c_ta2_n = 0;
    memset(s_jpae, 0, sizeof(s_jpae));
    /* Cloth: undo outstanding index flips + a leaked DOL draw-hook arm.
     * The raw restore skips the vtab verify (bases are not resolved here)
     * — writing the recorded original index is correct whenever the packet
     * memory is still mapped, same standard as jpa_restore. */
    if (st) {
        uint32_t ci;
        if (s_cloth_dol_armed && in_ram(st, s_cloth_dol_self, DCLOTH_SPAN))
            wr8_fast(st, s_cloth_dol_self + DCLOTH_OFF_CUR, s_cloth_dol_orig);
        for (ci = 0; ci < s_cloth_nflips; ++ci) {
            ClothFlip* f = &s_cloth_flips[ci];
            const ClothSpec* sp = &s_cloth_specs[f->spec];
            if (in_ram(st, f->pkt, sp->idx_off + 4u))
                cloth_idx_write(st, sp, f->pkt, f->orig);
        }
    }
    s_cloth_nflips = 0;
    s_cloth_dol_armed = 0;
    s_cloth_seen_n = 0;
    s_cloth_dbg_lerp = s_cloth_dbg_skip = 0;
    s_cloth_dbg_dol = s_cloth_dbg_rel = s_cloth_dbg_bas = 0;
    s_cloth_dbg_pkt = 0; s_cloth_dbg_vt0 = 0; s_cloth_dbg_dhit = 0;
    s_cloth_dbg_nodes = 0; s_cloth_dbg_linked = 0; s_cloth_dbg_dumped = 0;
    if (st)
        s_list_heap = rd8_fast(st, GINF_MCURRHEAP) & 1u;
    else
        s_list_heap = 0;
}

/* budget constants for TB-14 (kept for harness documentation; unreferenced) */
static const uint32_t J3D_BUDGET_TYPICAL_KIB __attribute__((unused)) = 140; /* 1500*96≈144000 ≈140.6 rounded */
static const uint32_t J3D_BUDGET_HEAVY_KIB  __attribute__((unused)) = 300;   /* 3200*96=307200 */

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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    ++s_hook_calls;
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
    /* D5 shadows: setSimple records per-slot identity + bake view (fires
     * inside fpcDw only); the draw entry is an on_void_render_gate call-out
     * (already hooked there), the return hook restores injected matrices. */
    RECOMP_HOOK(SHD_CTRL_SETSMPL, on_shd_setsimple),    /* setSimple id/bake rec */
    RECOMP_HOOK_RETURN(SHD_CTRL_DRAW, on_shd_draw_return),
};

/* Animated-material DL tracking — registered only when the R-render path can
 * run (split + Painter replay + J3D interp) AND texanim is on. endDiff fires
 * once per animated-material diff (~hundreds/frame); entry hook only — the
 * emitted DL state is already final at entry (see on_end_diff). */
static const ModernGekkoModHook hooks_texanim[] = {
    RECOMP_HOOK(0x802DB46Cu, on_end_diff),              /* J3DMatPacket::endDiff */
};

/* Cloth hooks — registered only when MODERNGEKKO_F60_CLOTH_INTERP=1 (and
 * the interp block is armed). ~1 call per packet per Painter — paired
 * return hooks are safe at this rate (same class as the foliage draws).
 * Rel-side cloth (sail/flags) needs no hooks: it is driven from the
 * Painter-entry walk inside on_painter_skip. */
static const ModernGekkoModHook hooks_cloth[] = {
    RECOMP_HOOK(DCLOTH_DRAW_EA, on_cloth_draw_entry),   /* dCloth_packet_c::draw  */
    RECOMP_HOOK_RETURN(DCLOTH_DRAW_EA, on_cloth_draw_return),
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
    /* If the mod is unloaded mid-split, drop the emu-thread priority back to
     * NORMAL before the hook plumbing is torn down. */
    emu_prio_restore();
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
    sizeof(hooks_texanim) / sizeof(hooks_texanim[0]) +
    sizeof(hooks_cloth) / sizeof(hooks_cloth[0]) +
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
            if (s_texanim && s_rframe_render)
                for (i = 0; i < (uint32_t)(sizeof(hooks_texanim) / sizeof(hooks_texanim[0])); ++i)
                    hooks_active[n++] = hooks_texanim[i];
            if (s_cloth_interp)
                for (i = 0; i < (uint32_t)(sizeof(hooks_cloth) / sizeof(hooks_cloth[0])); ++i)
                    hooks_active[n++] = hooks_cloth[i];
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
