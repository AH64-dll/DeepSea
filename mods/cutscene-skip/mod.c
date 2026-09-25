/*
 * mods/cutscene-skip/mod.c — user-facing cutscene skip for Wind Waker (GZLE01).
 *
 * Built as a ModernGekko .mgm mod (MODERNGEKKO_MOD_ABI_VERSION 1, CPU ABI 3).
 * Loads beside frame60-accum but shares no state with it.
 *
 * VERIFIED mechanism (live runs e5..e14)
 * ------------------------------------
 *  Demo/event cutscenes run as a dEvt_control_c event in dEvtMode_DEMO_e
 *  (mMode==2) inside PLAY_SCENE. dScnPly_Execute — the play scene's execute
 *  callback — runs unconditionally every logic frame while PLAY_SCENE is
 *  active, so it is the reliable per-frame hook.
 *
 *  The demo-skip must ride the game's OWN path whenever one exists, because
 *  killing the dEvt event directly is wrong for scene-driven demos:
 *
 *   * Opening storybook (Demo51 over Outset): NATIVELY skippable via a CLEAN
 *     START press — the demo/scene answers hold==START with a scripted scene
 *     change to Name/file-select (e9: plain START ended the book ~field 1085
 *     vs natural ~2220 and loaded Name/Stage.arc). But Z+START held does NOT
 *     trip it (e11: combo's extra Z bit fails the clean-START check -> book
 *     played to ~field 2100). And forcing dEvt_control_c::endProc() is
 *     actively harmful: the scene re-fires the killed demo and the intro
 *     loops mode 2->0->2->0 forever on a black screen, never reaching
 *     Name/Stage (e8/e10).
 *
 *   * Unskippable in-game demo (e7/e14): no native skip at all. We tail-call
 *     the engine's own teardown and control returns to gameplay cleanly:
 *        dEvt_control_c::endProc() -> demoEnd()
 *            -> dComIfGp_getPEvtManager()->endProc(mEventId,1)
 *            -> cancelStaff("ALL")
 *            -> mMode = dEvtMode_NONE_e
 *     Verified: the dialogue overlay vanished and stable gameplay remained.
 *
 *  So while Z+START is held during a demo we do BOTH, in order (e13/e14):
 *   1. Inject a clean START into g_mDoCPd_cpadInfo[0] (hold=trig=START) at
 *      dScnPly_Execute entry — before that frame's executeEvtManager/dDemo
 *      update reads the pad. Any natively-skippable demo takes its own skip
 *      path: the intro -> scene change to Name, a skippable in-game event ->
 *      its scripted end. During a demo START is consumed by the event
 *      handler (pause is inhibited), so the injection cannot open the menu.
 *   2. If the demo ignores the injected START (genuinely unskippable), the
 *      grace+fade gate (force_skip_now) forces endProc() once it has run
 *      un-faded past SKIP_GRACE_FRAMES — the same native teardown the
 *      engine's check()/reset() uses, no state hacking.
 *
 *  Because endProc is tail-called in place of dScnPly_Execute for the one
 *  frame it fires, exactly one play-scene update is skipped; endProc's BOOL
 *  return lands in the process manager just like dScnPly_Execute's, and
 *  mMode drops to NONE the same call, so it fires once.
 *
 *  Redundant/alternate paths (same gate, same native teardown):
 *   - H3 dEvt_control_c::check() sets mEventFlag bit 0x0008 -> its own
 *     endProc (for demos where check() is actively pumped).
 *   - H6 dEvent_manager_c::mainProc tail-calls evmgr endProc(idx,1) for
 *     events ordered straight into the event manager.
 *
 *  dScnOpen path (H1/H2) — a *different* storybook variant that can run as
 *  fpcNm_OPEN_SCENE_e / OPEN2_SCENE_e rather than a PLAY_SCENE demo: writes
 *  dScnOpen_proc_c::mState = 44 so execute() runs changeGameScene(), and
 *  injects JAIZelBasic::bgmStop(20,0) for the prologue stream. In the tested
 *  build dScnOpen_c::execute never fired during the intro (it is a
 *  PLAY_SCENE demo here), so this path is dormant coverage only.
 *
 * Bindings (read from the game's own pad state, g_mDoCPd_cpadInfo[0].
 * mButtonHold is used, NOT mButtonTrig — under frame60-accum logic runs at
 * 30Hz while the trig edge is one update wide, so trig is observed ==0 and
 * any trig-based binding silently never fires):
 *   Skip a cutscene : hold Z + START together (hold=0x0810). One binding for
 *                     every case — the mod injects a clean START so any
 *                     natively-skippable demo (the storybook -> file-select,
 *                     skippable events -> their end) takes its own path, and
 *                     forces endProc() only for events that have none. The
 *                     combo is deliberate (can't fire accidentally) and does
 *                     not collide with the lone-START pause binding — which
 *                     is untouched: a plain START press still pauses.
 *   Storybook (dScnOpen variant only): same Z+START hold required — a
 *                     stray A/B/START must not skip the new-game prologue.
 *
 * Caveat: events whose remaining script cuts grant items/flags will not run
 * those tail actions — inherent to any early abort (same as retail reset()).
 *
 * Env: MODERNGEKKO_CUTSCENE_SKIP=0 disables everything;
 *      MODERNGEKKO_CUTSCENE_SKIP_EVENTS=0 disables only the in-game part;
 *      MODERNGEKKO_CUTSCENE_SKIP_DEBUG=1 logs transitions to stderr.
 */
#include "moderngekko/mod_abi.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- guest function addresses (retail GZLE01 .text) ---------------------- */
#define ADDR_OPEN_EXECUTE   0x80232E8Cu /* dScnOpen_Execute (wrapper)    */
#define ADDR_OPEN_PROC_EXEC 0x80233BE4u /* dScnOpen_proc_c::proc_execute */
#define ADDR_EVT_CHECK      0x80071048u /* dEvt_control_c::check         */
#define ADDR_BGM_STOP       0x802A4658u /* JAIZelBasic::bgmStop(u32,s32) */
#define ADDR_MSGSET0        0x8002B8A4u /* fopMsgM_messageSet(u32)       */
#define ADDR_MSGSET1        0x8002B634u /* fopMsgM_messageSet(u32,act*)  */
#define ADDR_MSGSET2        0x8002B778u /* fopMsgM_messageSet(u32,cXyz*) */
#define ADDR_MSG_EXEC       0x80214560u /* dMsg_Execute(sub_msg_class*)  */
#define ADDR_EVMGR_MAINPROC 0x800741D4u /* dEvent_manager_c::mainProc    */
#define ADDR_EVMGR_ENDPROC  0x80074114u /* dEvent_manager_c::endProc     */
#define ADDR_PLY_EXECUTE    0x80234FD0u /* dScnPly_Execute(scene wrapper) */
#define ADDR_EVT_ENDPROC    0x80070D1Cu /* dEvt_control_c::endProc        */

/* g_dComIfG_gameInfo (0x803C4C08) + .play (0x12A0) + .mEvtCtrl (0x3F38):
 * the live dEvt_control_c the whole game reads — lets us drive endProc()
 * even while dEvt_control_c::check() is not being pumped. */
#define ADDR_EVT_CTRL       0x803C9DE0u

/* ---- guest data addresses (retail GZLE01 .bss/.sbss) --------------------- */
#define ADDR_ZEL_BASIC      0x803F7710u /* JAIZelBasic::zel_basic ptr    */
#define ADDR_ZEL_AUDIO      0x803A2CE8u /* g_mDoAud_zelAudio (fallback)  */
#define ADDR_STREAM_BUF_PTR 0x803F6880u /* mDoAud_StreamBufferPointer    */
#define ADDR_CPAD_INFO      0x803A4DF0u /* g_mDoCPd_cpadInfo[0]          */
#define ADDR_GINF_MFADE     0x803F68ABu /* mDoGph_gInf_c::mFade (u8)     */

/* ---- guest struct offsets ------------------------------------------------ */
/* base_process_class */
#define OFF_PROC_NAME    0x08u  /* s16 mProcName (fpcM_GetName)          */
/* dScnOpen_c (scene_class base, d_s_open.h) */
#define OFF_MP_PROC      0x1D0u /* dScnOpen_proc_c* mpProc               */
/* dScnOpen_proc_c */
#define OFF_PROC_MSTATE  0x2B0u /* s32 mState                            */
/* dEvt_control_c (d_event.h) */
#define OFF_EVT_MODE     0xC2u  /* u8  mMode  (dEvtMode_DEMO_e == 2)     */
#define OFF_EVT_FLAG     0xE8u  /* u16 mEventFlag — bit 0x0008 = end req */
#define OFF_MSG_NO       0xECu  /* u32 msg_class::mMsgNo                 */
#define OFF_MSG_STATUS   0xF8u  /* u16 msg_class::mStatus                */
#define OFF_MSG_TBTYPE   0x10Cu /* u8  mMesgEntry.mTextboxType (+0xC)    */
#define OFF_MSG_NEXT     0x108u /* u16 mMesgEntry.mNextMsgNo   (+0x8)    */
/* interface_of_controller_pad (c_API_controller_pad.h), stride 0x3C */
#define OFF_PAD_HOLD     0x30u  /* u16 mButtonHold                       */
#define OFF_PAD_TRIG     0x32u  /* u16 mButtonTrig                       */
/* JAIZelBasic */
#define OFF_STREAM_SND   0x70u  /* JAISound* mpStreamBgmSound            */

/* ---- controller_pad_buttons u16 bits (BE, verified vs execute asm):
 * byte0: left right down up z r l a ; byte1: b x y start — execute()
 * tests +0x32 byte0 bit0x01 (A), +0x33 bit0x80 (B), +0x33 bit0x10 (START). */
#define BTN_A     0x0100u
#define BTN_B     0x0080u
#define BTN_X     0x0040u
#define BTN_Y     0x0020u
#define BTN_START 0x0010u
#define BTN_Z     0x0800u
#define BTN_R     0x0400u
#define BTN_L     0x0200u

/* fpcNm ids */
#define PROC_OPEN_SCENE  0x000Eu
#define PROC_OPEN2_SCENE 0x000Fu

#define EVT_MODE_DEMO    0x02u
#define EVT_FLAG_END     0x0008u

/* dScnOpen_proc_c terminal state -> execute() runs changeGameScene */
#define OPEN_STATE_DONE  44u

/* Forced-skip gate tuning. The intro storybook (and many in-game demos) are
 * natively skipped by the game's own START handler — a scripted scene change
 * or event end that runs a screen fade. endProc()ing those early makes the
 * scene re-fire the demo (the intro loops on a black screen: observed
 * mode 2->0->2->0... with Name/Stage never loading). So a held Z+START is
 * first given a grace window in which the game's own skip can act; only if
 * the demo is still running, un-faded, past the grace do we force teardown. */
#define SKIP_GRACE_FRAMES 90u   /* ~3s at 30Hz: window for the native skip   */
#define SKIP_FADE_MAX    180u   /* cap on suppressing skip during a fade      */
#define SKIP_REARM       45u    /* cooldown after one forced endProc          */

/* Skip-state machine for the storybook */
#define OPEN_SKIP_IDLE    0u
#define OPEN_SKIP_DONE    2u  /* mState=44 written; execute() handles rest  */

/* ---- host-side state ------------------------------------------------------ */
static uint32_t s_enabled = 1;
static uint32_t s_events_enabled = 1;
static uint32_t s_debug = 0;

static uint32_t s_open_stage = OPEN_SKIP_IDLE;
static uint32_t s_open_proc = 0;     /* mpProc we are skipping            */
static uint32_t s_bgm_stop = 0;      /* proc_execute -> bgmStop redirect  */
static uint32_t s_evtctrl = 0;       /* live dEvt_control_c* (from check) */
static uint32_t s_dbg_last_mode = 0xFFu; /* last logged evt mMode         */
static uint32_t s_ply_last_mode = 0xFFu; /* last logged mode (ply path)   */
static uint32_t s_skip_hold = 0;         /* un-faded frames combo held    */
static uint32_t s_skip_fade = 0;         /* consecutive fading frames     */
static uint32_t s_skip_cd   = 0;         /* re-arm cooldown after endProc */
static uint32_t s_inj       = 0;         /* logged START-inject this demo */

/* ---- guest-memory helpers (same contract as frame60-accum: bounds-checked
 * direct-RAM for MEM1, external accessors elsewhere) ------------------------ */
static int in_ram(const CPUState* s, uint32_t ea, uint32_t size)
{
    return s->ram && (ea - 0x80000000u) <= s->ram_size - size;
}
static uint32_t rd32_fast(CPUState* s, uint32_t a)
{
    if (in_ram(s, a, 4u)) {
        const uint8_t* p = s->ram + (a - 0x80000000u);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    return (uint32_t)moderngekko_mod_read(s, a, 4u);
}
static uint32_t rd16_fast(CPUState* s, uint32_t a)
{
    if (in_ram(s, a, 2u)) {
        const uint8_t* p = s->ram + (a - 0x80000000u);
        return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
    }
    return (uint32_t)moderngekko_mod_read(s, a, 2u) & 0xFFFFu;
}
static uint32_t rd8_fast(CPUState* s, uint32_t a)
{
    if (in_ram(s, a, 1u))
        return (uint32_t)s->ram[a - 0x80000000u];
    return (uint32_t)moderngekko_mod_read(s, a, 1u) & 0xFFu;
}
static void wr32_fast(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 4u)) {
        uint8_t* p = s->ram + (a - 0x80000000u);
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
    } else {
        moderngekko_mod_write(s, a, v, 4u);
    }
}
static void wr16_fast(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 2u)) {
        uint8_t* p = s->ram + (a - 0x80000000u);
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
    } else {
        moderngekko_mod_write(s, a, v, 2u);
    }
}
static int in_mem1(uint32_t a)
{
    return (a - 0x80000000u) < 0x01800000u;
}

/* ---- pad state ------------------------------------------------------------ */
static uint32_t pad_hold(CPUState* s)
{
    return rd16_fast(s, ADDR_CPAD_INFO + OFF_PAD_HOLD);
}

/*
 * Input is read from mButtonHold (+0x30), NOT mButtonTrig (+0x32).
 * Under frame60-accum the game runs logic at 30Hz while the pad trigger
 * edge is a single update wide — observed trig == 0 for every sample, so
 * any trig-based binding silently never fires. mButtonHold is stable
 * across the whole press, so we detect edges ourselves / use level state.
 */

/* In-game / storybook skip combo: Z + START held together — a deliberate
 * two-button combination that cannot fire accidentally and does not collide
 * with the lone-START pause binding. Level-based (mButtonHold) so it is
 * caught even on the sparse dEvt_control_c::check rate. */
static int event_skip_pressed(CPUState* s)
{
    uint32_t h = pad_hold(s);
    return (h & BTN_Z) && (h & BTN_START);
}
/* Storybook skip input: the same Z+START hold used for in-game events —
 * a stray A/B/START press must not skip the new-game prologue. */
static int open_skip_pressed(CPUState* s)
{
    return event_skip_pressed(s);
}

/* ---- shared forced-skip gate ---------------------------------------------
 * Decides WHEN a held skip combo is allowed to force the engine's event
 * teardown. Many demos are already skipped by the game's own START handler:
 * the intro storybook's START -> a scripted scene change to Name/file-select,
 * and skippable in-game events -> an event end. Those run a screen fade and
 * must NOT be endProc()'d — killing the dEvt event out from under the scene
 * makes it re-fire (verified: the intro loops mode 2->0->2->0 forever on a
 * black screen and never reaches Name/Stage).
 *
 * So while the combo is held we give the game a grace window to act on its
 * own; only once the demo has run past it with no fade in flight do we force
 * endProc(). This makes the binding uniform — native teardown whenever the
 * game offers it, forced teardown only for genuinely unskippable events. */
static void skip_gate_reset(void)
{
    s_skip_hold = 0;
    s_skip_fade = 0;
    s_inj = 0;
}
static int force_skip_now(CPUState* s)
{
    if (s_skip_cd) {
        s_skip_cd--;
        return 0;
    }
    if (rd8_fast(s, ADDR_GINF_MFADE)) {
        /* A fade is in flight — almost certainly the game's own skip / scene
         * change already running (intro -> Name, or a scripted transition).
         * Stay out of its way; the cap keeps a stuck fade from ever making a
         * demo permanently unskippable. */
        if (s_skip_fade < SKIP_FADE_MAX) {
            s_skip_fade++;
            return 0;
        }
    } else {
        s_skip_fade = 0;
    }
    if (++s_skip_hold < SKIP_GRACE_FRAMES)
        return 0;
    s_skip_hold = 0;
    s_skip_cd = SKIP_REARM;
    return 1;
}

/* ---- H1: dScnOpen_Execute entry — storybook skip state machine ------------
 * r3 = this (dScnOpen_c*). Runs once per logic frame while an open scene
 * exists (scene execute is a process update under fpcM_Execute). The hook
 * must sit on the scene-method wrapper: the process manager dispatches the
 * method-table entry dScnOpen_Execute (0x80232E8C), which then calls
 * dScnOpen_c::execute (0x80232BC4) via an intra-chunk `bl` that recompiles
 * to a direct goto — a hook on the member address never re-dispatches. */
static void on_open_execute(CPUState* state)
{
    uint32_t self = state->gpr[3];
    if (!in_mem1(self))
        return;
    uint32_t proc_name = rd16_fast(state, self + OFF_PROC_NAME) & 0xFFFFu;
    {
        static uint32_t s_last_proc = 0xFFFFu;
        if (s_debug && proc_name != s_last_proc) {
            /* Debug-only: distinguishes the open-scene variant the
             * storybook runs as (14=OPEN vs 15=OPEN2). */
            fprintf(stderr, "[cskip] dScnOpen execute proc_name=%u\n",
                    proc_name);
            s_last_proc = proc_name;
        }
    }
    if (proc_name != PROC_OPEN_SCENE && proc_name != PROC_OPEN2_SCENE) {
        /* Some other scene — reset the skip state machine. */
        if (s_open_stage != OPEN_SKIP_IDLE) {
            s_open_stage = OPEN_SKIP_IDLE;
            s_bgm_stop = 0;
        }
        return;
    }

    uint32_t proc = rd32_fast(state, self + OFF_MP_PROC);
    if (!in_mem1(proc)) {
        s_open_stage = OPEN_SKIP_IDLE;
        s_bgm_stop = 0;
        return;
    }
    if (proc != s_open_proc && s_open_stage != OPEN_SKIP_IDLE) {
        /* Scene/proc object changed underneath us — restart cleanly. */
        s_open_stage = OPEN_SKIP_IDLE;
        s_bgm_stop = 0;
    }

    uint32_t mstate = rd32_fast(state, proc + OFF_PROC_MSTATE);
    s_open_proc = proc;
    {
        static uint32_t s_dbg_frames = 0;
        if (s_debug && (++s_dbg_frames % 60) == 0) {
            uint32_t ovlp = rd32_fast(state, 0x803F6160u);
            fprintf(stderr,
                    "[cskip] open hb mState=%u ovlp=%08X ovlp08=%08X f1D4=%u hold=%04X\n",
                    mstate, ovlp,
                    in_mem1(ovlp) ? rd32_fast(state, ovlp + 8) : 0u,
                    rd8_fast(state, self + 0x1D4u),
                    pad_hold(state));
        }
    }

    /*
     * Skip == jump straight to the terminal state. execute() checks
     * mpProc->mState == 44 *after* proc_execute() and runs changeGameScene():
     *   OPEN_SCENE  -> fopScnM_ChangeReq(PLAY_SCENE)
     *   OPEN2_SCENE -> dComIfG_changeOpeningScene(OPENING2_SCENE)
     * Neither path waits on mDoAud_isUsedHeapForStreamBuffer() — unlike the
     * native OPEN2 field_0x1d4 skip, which can stall forever if the port
     * never releases the stream buffer. So we write mState=44 immediately on
     * the skip press (the book auto-plays to 44 anyway; we just shortcut it),
     * and arm s_bgm_stop so this same frame's proc_execute() call is replaced
     * by JAIZelBasic::bgmStop(20,0) to fade the prologue stream.
     */
    if (mstate < OPEN_STATE_DONE && mstate >= 1 && open_skip_pressed(state)) {
        wr32_fast(state, proc + OFF_PROC_MSTATE, OPEN_STATE_DONE);
        s_bgm_stop = 1;
        s_open_stage = OPEN_SKIP_DONE;
        if (s_debug)
            fprintf(stderr,
                    "[cskip] storybook skip -> done (mState=%u proc=%u)\n",
                    mstate, proc_name);
        /* Stop the prologue narration stream now: once the scene change is
         * requested the incoming scene's load waits on
         * mDoAud_isUsedHeapForStreamBuffer(), which never frees while the
         * stream keeps playing — the classic stall. The proc_execute hook
         * cannot deliver bgmStop (execute calls it via an intra-chunk bl
         * that never re-dispatches), so tail-call JAIZelBasic::bgmStop(20,0)
         * in place of this frame's execute call instead. */
        uint32_t zel = rd32_fast(state, ADDR_ZEL_BASIC);
        if (!in_mem1(zel))
            zel = ADDR_ZEL_AUDIO;
        state->gpr[3] = zel;
        state->gpr[4] = 20u;
        state->gpr[5] = 0u;
        state->pc = ADDR_BGM_STOP;
    }
}

/* ---- H2: dScnOpen_proc_c::proc_execute entry — bgmStop injection ----------
 * Only armed while OPEN_SKIP_WAIT: the call is replaced by a tail call to
 * JAIZelBasic::bgmStop(this = zel_basic, 20, 0), which returns straight into
 * dScnOpen_c::execute. proc_execute is void — its discarded result is never
 * observed, so the fake return can't corrupt execute's TRUE return value. */
static void on_open_proc_execute(CPUState* state)
{
    if (!s_bgm_stop)
        return;
    uint32_t zel = rd32_fast(state, ADDR_ZEL_BASIC);
    if (!in_mem1(zel))
        zel = ADDR_ZEL_AUDIO; /* getInterface() fallback: the singleton itself */
    state->gpr[3] = zel;
    state->gpr[4] = 20u;
    state->gpr[5] = 0u;
    state->pc = ADDR_BGM_STOP;
}

/* ---- H3: dEvt_control_c::check entry — in-game demo event skip ------------
 * r3 = this (dEvt_control_c*, lives in g_dComIfG_gameInfo.mEvtCtrl @+0x3F38).
 * check() reads mEventFlag bit 0x0008 at its top: setting it at entry makes
 * THIS call run mbEndProc -> endProc() -> demoEnd() — the engine's own
 * event-teardown path (same one dComIfGp_event_reset() triggers). */
static void on_evt_check(CPUState* state)
{
    if (!s_events_enabled)
        return;
    uint32_t self = state->gpr[3];
    if (!in_mem1(self))
        return;
    /* Capture the live dEvt_control_c* — check() is only pumped pre-demo, so
     * this records the authoritative object for on_ply_execute to drive
     * endProc() directly once check() stops running mid-demo. */
    s_evtctrl = self;
    uint32_t mode = rd8_fast(state, self + OFF_EVT_MODE);
    if (s_debug && mode != s_dbg_last_mode) {
        fprintf(stderr, "[cskip] evt mode %u->%u flag=%04X\n",
                s_dbg_last_mode, mode, rd16_fast(state, self + OFF_EVT_FLAG));
        s_dbg_last_mode = mode;
    }
    if (s_debug) {
        /* Heartbeat: count check() calls so we can prove whether the evt
         * manager even runs during the storybook window. */
        static uint32_t s_hb = 0;
        if (++s_hb % 600 == 0)
            fprintf(stderr, "[cskip] evt check hb=%u mode=%u\n", s_hb, mode);
    }
    if (mode != EVT_MODE_DEMO)
        return;
    if (!event_skip_pressed(state))
        return;
    if (!force_skip_now(state))
        return;
    uint32_t flags = rd16_fast(state, self + OFF_EVT_FLAG);
    if (!(flags & EVT_FLAG_END)) {
        wr16_fast(state, self + OFF_EVT_FLAG, flags | EVT_FLAG_END);
        if (s_debug)
            fprintf(stderr, "[cskip] demo event end requested (flags=%04X)\n",
                    flags);
    }
}

/* ---- H4 (debug): fopMsgM_messageSet entry — identify storybook driver -----
 * Logs every messageSet call's msg_num + return address so we can tell which
 * system issues the prologue pages (and whether they are messages at all). */
static void on_msg_set(CPUState* state)
{
    if (!s_debug)
        return;
    fprintf(stderr, "[cskip] msgSet %08X lr=%08X\n",
            state->gpr[3], state->lr);
}

/* ---- H5 (debug): dMsg_Execute(sub_msg_class*) — storybook page state ------
 * sub_msg_class is the MSG process that renders the sepia prologue book.
 * r3 = this. Log msg_no / status / textbox-type on each distinct state so the
 * capture log reveals the book's pagination and confirms this proc owns it. */
static void on_msg_exec(CPUState* state)
{
    if (!s_debug)
        return;
    uint32_t self = state->gpr[3];
    if (!in_mem1(self))
        return;
    uint32_t msg_no = rd32_fast(state, self + OFF_MSG_NO);
    uint32_t status = rd16_fast(state, self + OFF_MSG_STATUS);
    uint32_t tbtype = rd8_fast(state, self + OFF_MSG_TBTYPE);
    uint32_t next = rd16_fast(state, self + OFF_MSG_NEXT);
    static uint32_t s_last_key = 0xFFFFFFFFu;
    uint32_t key = (msg_no << 8) | (status & 0xFF);
    if (key == s_last_key)
        return;
    s_last_key = key;
    fprintf(stderr,
            "[cskip] msgExec self=%08X no=%04X st=%02X tb=%02X next=%04X lr=%08X\n",
            self, msg_no, status, tbtype, next, state->lr);
}

/* ---- H6: dEvent_manager_c::mainProc — storybook event detection + skip ----
 * The prologue book is a dEvDt event whose "Message" staff calls
 * fopMsgM_messageSet per page. It is ordered straight into dEvent_manager_c
 * (evmng_order), bypassing dEvt_control_c — so the mMode-based demoEnd path
 * never sees it. Instead we end the event natively: at mainProc entry, while
 * the skip combo is held and a PLAY_e event is live, we tail-call
 * endProc(idx, 1) — the engine's own closeProc/cancelStaff teardown.
 *
 * dEvent_manager_c layout: mList @+0x00 { mHeaderP @+0x00 (eventNum @+0x04),
 * mEventP @+0x04 }; dEvDtEvent_c is 0xB0 bytes { mName[0x20] @+0x00,
 * mEventState @+0xA4 (PLAY_e==2) }. */
#define OFF_MGR_HEADERP   0x00u
#define OFF_MGR_EVENTP    0x04u
#define OFF_HDR_EVENTNUM  0x04u
#define SZ_DEVDT_EVENT    0xB0u
#define OFF_EV_STATE      0xA4u
#define EVSTATE_PLAY      2

static uint32_t ev_find_play_event(CPUState* state, uint32_t mgr, char* name_out)
{
    uint32_t hdr = rd32_fast(state, mgr + OFF_MGR_HEADERP);
    uint32_t evp = rd32_fast(state, mgr + OFF_MGR_EVENTP);
    if (!in_mem1(hdr) || !in_mem1(evp))
        return 0xFFFFFFFFu;
    int num = (int)rd32_fast(state, hdr + OFF_HDR_EVENTNUM);
    if (num <= 0 || num > 64)
        num = 64;
    for (int i = 0; i < num; i++) {
        uint32_t ev = evp + (uint32_t)i * SZ_DEVDT_EVENT;
        if (rd32_fast(state, ev + OFF_EV_STATE) == EVSTATE_PLAY) {
            if (name_out) {
                for (int k = 0; k < 31; k++) {
                    char c = (char)rd8_fast(state, ev + k);
                    name_out[k] = c;
                    if (c == '\0')
                        break;
                }
                name_out[31] = '\0';
            }
            return (uint32_t)i;
        }
    }
    return 0xFFFFFFFFu;
}

static void on_evmgr_mainproc(CPUState* state)
{
    uint32_t mgr = state->gpr[3];
    if (!in_mem1(mgr))
        return;
    char name[32];
    uint32_t idx = ev_find_play_event(state, mgr, s_debug ? name : NULL);
    if (s_debug) {
        static uint32_t s_last_idx = 0xFFFFFFFEu;
        if (idx != s_last_idx) {
            if (idx != 0xFFFFFFFFu)
                fprintf(stderr, "[cskip] evmgr PLAY[%u]=\"%s\"\n", idx, name);
            else
                fprintf(stderr, "[cskip] evmgr PLAY none\n");
            s_last_idx = idx;
        }
    }
    if (idx == 0xFFFFFFFFu)
        return;
    if (!event_skip_pressed(state))
        return;
    if (!force_skip_now(state))
        return;
    /* Tail-call endProc(this=mgr, eventIdx=idx, act=1): the engine's own
     * closeProc + cancelStaff teardown, returns into runProc's caller. */
    state->gpr[4] = idx;
    state->gpr[5] = 1u;
    state->pc = ADDR_EVMGR_ENDPROC;
    if (s_debug)
        fprintf(stderr, "[cskip] evmgr endProc(%u) requested\n", idx);
}

/* ---- H7: dScnPly_Execute — reliable per-frame demo teardown ---------------
 * dScnPly_Execute runs every logic frame while PLAY_SCENE is active —
 * including throughout the intro/storybook demo (Demo51.arc). Critically,
 * dEvt_control_c::check() is NOT pumped during that window, so the H3
 * mEventFlag route can sit unread and H6's mainProc may not run either.
 *
 * Here we invoke the engine's own teardown directly: when mMode ==
 * dEvtMode_DEMO_e and Z+START is held, tail-call dEvt_control_c::endProc()
 * with this = the live dEvt_control_c (captured in on_evt_check, falling
 * back to the static gameInfo address). endProc() -> demoEnd() ->
 * endProc(mEventId,1) + cancelStaff("ALL") -> mMode = NONE.
 *
 * Because endProc is tail-called in place of dScnPly_Execute, exactly one
 * frame of the play-scene update is skipped; endProc's BOOL return lands in
 * the process manager just like dScnPly_Execute's would, and the next frame
 * runs normally (mode is already NONE, so this fires exactly once). */
static void on_ply_execute(CPUState* state)
{
    if (!s_events_enabled)
        return;
    uint32_t ec = s_evtctrl ? s_evtctrl : ADDR_EVT_CTRL;
    if (!in_mem1(ec))
        return;
    uint32_t mode = rd8_fast(state, ec + OFF_EVT_MODE);
    if (s_debug && mode != s_ply_last_mode) {
        fprintf(stderr, "[cskip] ply evt mode %u->%u ec=%08X\n",
                s_ply_last_mode, mode, ec);
        s_ply_last_mode = mode;
    }
    if (s_debug) {
        /* Heartbeat: prove dScnPly_Execute runs during the demo window and
         * that we see the live mMode (mirrors the on_evt_check hb). */
        static uint32_t s_hb = 0;
        if (++s_hb % 600 == 0)
            fprintf(stderr, "[cskip] ply hb=%u mode=%u hold=%04X fade=%u\n",
                    s_hb, mode, pad_hold(state),
                    rd8_fast(state, ADDR_GINF_MFADE));
    }
    if (mode != EVT_MODE_DEMO) {
        skip_gate_reset();
        return;
    }
    if (!event_skip_pressed(state)) {
        skip_gate_reset();
        return;
    }

    /*
     * Z+START held during a demo. First present the game's own skip input a
     * CLEAN START: the intro storybook's skip needs hold==START (a plain
     * START with no other button), which the Z+START combo's extra Z bit
     * fails. Injecting hold=trig=START lets ANY natively-skippable demo —
     * the intro (-> scene change to Name) or a skippable in-game event
     * (-> event end) — take its own teardown path, which is always the
     * cleanest. This write happens at dScnPly_Execute entry, before that
     * frame's executeEvtManager / dDemo update reads the pad.
     *
     * If the demo has no native skip (a genuinely unskippable in-game event),
     * the injected START is ignored and the demo persists; the grace+fade
     * gate then forces the engine's own endProc() teardown (verified e7).
     */
    wr16_fast(state, ADDR_CPAD_INFO + OFF_PAD_HOLD, BTN_START);
    wr16_fast(state, ADDR_CPAD_INFO + OFF_PAD_TRIG, BTN_START);
    if (s_debug && !s_inj) {
        fprintf(stderr, "[cskip] demo: injected clean START (native skip)\n");
        s_inj = 1;
    }
    if (!force_skip_now(state))
        return;
    state->gpr[3] = ec;
    state->pc = ADDR_EVT_ENDPROC;
    if (s_debug)
        fprintf(stderr, "[cskip] demo endProc() invoked ec=%08X\n", ec);
}

/* ---- mod boilerplate ------------------------------------------------------ */
static void cutscene_skip_on_load(const ModernGekkoModHostApi* api)
{
    (void)api;
    const char* v = getenv("MODERNGEKKO_CUTSCENE_SKIP");
    if (v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F'))
        s_enabled = 0;
    const char* e = getenv("MODERNGEKKO_CUTSCENE_SKIP_EVENTS");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
        s_events_enabled = 0;
    const char* d = getenv("MODERNGEKKO_CUTSCENE_SKIP_DEBUG");
    if (d && d[0] == '1')
        s_debug = 1;
}

static void cutscene_skip_on_unload(void)
{
    s_open_stage = OPEN_SKIP_IDLE;
    s_open_proc = 0;
    s_bgm_stop = 0;
    s_skip_hold = 0;
    s_skip_fade = 0;
    s_skip_cd = 0;
}

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK(ADDR_OPEN_EXECUTE, on_open_execute),
    RECOMP_HOOK(ADDR_OPEN_PROC_EXEC, on_open_proc_execute),
    RECOMP_HOOK(ADDR_EVT_CHECK, on_evt_check),
    RECOMP_HOOK(ADDR_EVMGR_MAINPROC, on_evmgr_mainproc),
    RECOMP_HOOK(ADDR_PLY_EXECUTE, on_ply_execute),
    RECOMP_HOOK(ADDR_MSGSET0, on_msg_set),
    RECOMP_HOOK(ADDR_MSGSET1, on_msg_set),
    RECOMP_HOOK(ADDR_MSGSET2, on_msg_set),
    RECOMP_HOOK(ADDR_MSG_EXEC, on_msg_exec),
};

static ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GZLE01",
    "cutscene-skip",
    "0.1.0",
    "Wind Waker cutscene skip: Z+START -> inject clean START (native skip; storybook->file-select) else engine endProc for unskippable demos",
    0, 0u,
    0, 0u,
    hooks, (uint32_t)(sizeof(hooks) / sizeof(hooks[0])),
    0, 0u,
    0, 0u,
    0, 0u,
    0, 0u,
    cutscene_skip_on_load,
    cutscene_skip_on_unload,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc* moderngekko_get_mod(void)
{
    cutscene_skip_on_load(0);
    if (!s_enabled) {
        descriptor.hooks = 0;
        descriptor.num_hooks = 0u;
    }
    return &descriptor;
}
