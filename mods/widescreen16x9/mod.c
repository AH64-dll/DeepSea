/*
 * mods/widescreen16x9/mod.c — native port of Dolphin's "$16:9 Widescreen"
 * Gecko code for Wind Waker (GZLE01), generalized to arbitrary target
 * aspect ratios (true ultrawide, not pillarboxed 16:9).
 *
 * Source: vendor/dolphin/Data/Sys/GameSettings/GZLE01.ini lines 347-452.
 * Built as a ModernGekko .mgm mod (MODERNGEKKO_MOD_ABI_VERSION 1, CPU ABI 3,
 * game_id "GZLE01"). Enabled by env MODERNGEKKO_WIDESCREEN=1.
 * Target aspect: env MODERNGEKKO_WIDESCREEN_ASPECT ("16:9" default;
 * "21:9"/"32:9"/"W:H" or a bare decimal like "2.37"; clamped [4:3,32:9];
 * invalid -> 16:9). Unset or 16:9 reproduces the stock Gecko values
 * bit-for-bit; see "aspect model" below for how every constant derives
 * from A. The frontend maps `aspect_ratio=<custom>` to Dolphin's
 * AspectMode::Custom + this env, so the stretched presentation and the
 * game's layout always agree.
 *
 * WHY THIS EXISTS
 * ---------------
 * The launcher's `aspect_ratio=16:9` only stretches the 4:3 framebuffer, and
 * `widescreen_hack` scales vertex positions (fat HUD/text/fades + edge
 * pop-in). Dolphin's Gecko code instead makes the GAME render widescreen:
 * it widens the camera/projection aspect constant, widens the 2D ortho space
 * to [-123, 767] (from [0, 640]), and relocates HUD elements inside that
 * space. Rendered 4:3 -> stretched to 16:9 by the presentation layer, the
 * result is true widescreen with correct HUD proportions. The same holds
 * at any A: e.g. 21:9 widens the ortho space to ~[-262, 906] and the
 * presentation stretch matches, so HUD/world stay proportional.
 *
 * GECKO WRITE -> IMPLEMENTATION MAP
 * -------------------------------
 * The Gecko block contains three kinds of writes:
 *
 *  A) Pure DATA writes (02/04/06 to non-.text) - applied verbatim to guest
 *     memory once at runtime_start and re-asserted every frame at
 *     mDoGph_Painter (Gecko handlers re-run each frame; the dMeter table is
 *     in .bss and is re-initialised by the game, so re-apply is required):
 *       043F7D68 C2F60000   f32  -9.0    -> -123.0   ortho/UI left margin
 *       043F7D6C 443FC000   f32  650.0   ->  767.0   ortho/UI right margin
 *       063F89B8 (16 bytes) {-9,-21,659,524} -> {-123,-118,890,716}  UI rect
 *       043FA998 3FE38E39   f32  1.33333 -> 1.77778  dCamera aspect const
 *            (@17778 in d_camera.cpp sdata2; read by camera_process_class::
 *            preparation 0x8017BE5C -> projection widening)
 *       043FB77C 3FB20000   f32  1.02969 -> 1.39063  dDlst_2Dt_Sp x-scale
 *       043FB78C C2F60000   f32  -9.0    -> -123.0   dDlst_2Dt_Sp x-pos
 *       023E68E4 FFB1  s16 -79   | 023E68E8 FF00 s16 -256
 *       023E68F0 02C0  s16 704  | 023E6958 0079 s16  121
 *       023E69A4 FF8E  s16 -114          (dMeter layout table, .bss)
 *
 *  B) Instruction patches replaced by CALL-SITE (callee-entry, lr-gated)
 *     hooks. The recompiler never dispatches mid-block PCs, but every
 *     cross-chunk `bl` produces two dispatch boundaries: the callee entry
 *     and the return address (site+4). At callee entry `lr` equals the
 *     return address, so a hook on the callee keyed by lr reproduces the
 *     patch exactly. Gecko helper blocks:
 *       helperA @800037E0: lwz r4,4(r28); [r4+0xC] = -123.0f
 *            site 0x8016103C -> hook setPaneData entry, lr==0x80161048:
 *            write f32 -123.0 to [r4+0xC] (r4 still = [r28+4]).
 *       helperC @80004058: f1 = 320 - (320 - x) * 1.3333333  (boomerang
 *            sight x mirror/spread; constants [0x803F8B74]=320,
 *            [0x803F85A4]=4/3)
 *            site 0x800E1630 -> hook PSMTXTrans entry, lr==0x800E1640.
 *       helperG @800040EC: f1 -= 114.0 before fopMsgM_paneTrans
 *            sites: bl paneTrans at 0x801F102C/103C/9BE0/9C5C/9C6C/9C7C/
 *            9C98/FD610/FD620/FD630 -> hook paneTrans, gate on lr.
 *            (0x801F9BE4 also carries 041F9BD4 lfs f1,-19108(r2) =
 *            [0x803FB25C]=0.0; net f1 = 0.0 - 114.0 = -114.0 - handled in
 *            the same gate.)
 *       helperF @800040D8: [r3+4] -= 114.0 before fopMsgM_setInitAlpha
 *            sites 0x801F8F00/08/10 -> hook setInitAlpha, gate on lr.
 *       helperE @800040C0: r12=[r3]; [r12+0x48] = [r12+0xC] + 114.0 before
 *            fopMsgM_setAlpha; sites 0x8020464C/54/68/70/88 -> hook
 *            setAlpha, gate on lr.
 *     Plain NOP'd `bl` sites (15): hook callee, gate on lr, `pc = lr`
 *     skips the call. setInitAlpha: 0x8019E908 0x801AC400 0x801CA9A8
 *     0x801CC338; setNowAlpha: 0x8019E954 0x8019EC70 0x801AB610 0x801AC44C
 *     0x801AC770 0x801B955C 0x801CC384 0x801CC778 0x801D37FC 0x801DAA38
 *     0x801DAAE8.
 *     Immediate/constant patches consumed by a subsequent `bl` are also
 *     folded into callee-entry hooks:
 *       041B6968/041C5174 lfs f1,->-123.0   -> paneTrans  f1=-123.0
 *       041F0678/694/700  lfs f1,->114.0 + lfs f2,->[0x803FB25C]=0.0
 *            -> J2DScreen::draw
 *       0418F5CC/21E6E8/234528 or r4,r31 -> li r4,0       -> J2DScreen::draw
 *       041FBA20  fadds f1,f31,f0 -> fadds f1,f30,f0       -> paneTrans
 *            (f0 = the 0x801FBA1C lfsx value [r27+r29]; f0 is reloaded at
 *            0x801FBA24 before the bl, so the fold reads the stack slot)
 *       041FC28C  lfs f0,->206.0 (was -320.0 const)        -> dPa set: [r6]+=114
 *       04201D8C  lfs f4,->114.0 -> setPos r4 recompute (swim tekari)
 *       041FEFB4  lfs f1,6476(r23) -> 0x1954(r23)          -> setPos r4 (clock)
 *       041F0B70  lha r0,80(r5) -> li r0,110  ([0x803E6958] -> 110 const)
 *            -> paneScaleXY entry: [r3+0x1C] += 110 - s16(live table)
 *       04227A18/1C/20  lfs f1/f2/f3 -> 79.0/0.0/480.0     -> GXSetViewport
 *
 *  C) Instruction patches at dispatch boundaries - hooked directly at the
 *     patched PC (fires when the preceding cross-chunk `bl` returns):
 *       041B6978 -> helperB stanza1: f0 = 767.0f; [r3+0x14] = f0
 *            (then addi runs)
 *       041C5188 -> helperB stanza2: f0 = 767.0f; [r3+0x14] = f0
 *            (then cmpwi runs)
 *
 *  D) `blr` patches 0x8022B9E4 / 0x802375E4 -> helperD @80004074: draw two
 *     opaque-black J2DFillBox bars over the new margins at function end:
 *       J2DFillBox(-130, -32, 130, 640, &0x000000FF)   left  x in [-130,0]
 *       J2DFillBox( 640, -32, 130, 640, &0x000000FF)   right x in [640,770]
 *     A mid-function `blr` is not a dispatch boundary, so the mod redirects
 *     lr at the two draw() entries to the blr PC (a real, dispatchable
 *     instruction address). When the function returns, dispatch lands on the
 *     blr PC; the hook there arms the J2DFillBox args, points pc at
 *     J2DFillBox (0x802CDF3C) and lr at a scratch PC (the addi just above
 *     the blr - never naturally dispatched); that scratch hook arms box 2,
 *     points pc at J2DFillBox again and lr at the saved real return
 *     address. Net effect: two extra draw calls appended at function end,
 *     identical to the Gecko helper.
 *
 *  E) `stb` NOPs 0x8019E66C / 0x801CC208 (keep old byte at obj+2469):
 *     mid-block stores can't be intercepted, so the mod snapshots the byte
 *     at function entry and restores it after the store would have run.
 *     Collect: entry snapshot + restore at the 0x8019E6CC boundary (r31=this
 *     still live) and again at function return (belt-and-suspenders for the
 *     bc path that skips the bl). Item: entry snapshot + restore at
 *     function return (HOOK_RETURN tracks lr+r1, covers all exit paths).
 *     Semantic gap vs Gecko: a read of obj+2469 between the would-be store
 *     and the restore point sees the written value - no such read exists in
 *     either function.
 *
 *  F) calcScissor 0x8004A3A4 (li r3,116->153; li r4,0->32): the patched
 *     constants feed an inline clamp whose results are stored to the
 *     r13-relative .sbss globals mScissorWidth [r13-30200] and mScissorOrigX
 *     [r13-30204] (r13 = 0x803FE0E0). Mid-block immediates can't be hooked,
 *     so a HOOK_RETURN post-fixes the stored globals: mScissorWidth += 37
 *     (unless the clamp floored it to 0) and mScissorOrigX = 32 when the
 *     clamped x was <= 0. Approximation: when the input delta is exactly 0
 *     the patched value would stay 0 - off-by-32 for one degenerate frame.
 *
 *  NOT PORTED (inert under static recompilation):
 *   The `06` blocks at 0x800037E0/0x80004038 install helper CODE into guest
 *     .text scratch space - never executed here; their semantics are
 *     reimplemented by the hooks above instead.
 *   Every `04` write into .text is a guest instruction patch - likewise
 *     reimplemented semantically, never as a memory write.
 *
 * Env: MODERNGEKKO_WIDESCREEN=1 enables; =0 / unset disables (mod still
 * loads but registers no hooks). Pair with the launcher's 16:9 output
 * (aspect_ratio=16:9) - the mod produces anamorphic 16:9 content in the
 * 4:3 frame; the stretch to 16:9 restores correct proportions.
 */
#include "moderngekko/mod_abi.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- guest function addresses (retail GZLE01 .text) ---------------------- */
#define A_PANETRANS      0x8003BC88u /* fopMsgM_paneTrans(pane*,f,f)       */
#define A_SETALPHA       0x8003BE14u /* fopMsgM_setAlpha(pane*)            */
#define A_SETINITALPHA   0x8003BE24u /* fopMsgM_setInitAlpha(pane*)        */
#define A_SETNOWALPHA    0x8003BE30u /* fopMsgM_setNowAlpha(pane*,f)       */
#define A_SETPANEDATA    0x8003BB78u /* fopMsgM_setPaneData(pane*,scr,u32) */
#define A_PANESCALEXY    0x8003BD50u /* fopMsgM_paneScaleXY(pane*,f)       */
#define A_PSMTXTRANS     0x8030D618u /* PSMTXTrans(x,y,z)                  */
#define A_J2D_DRAW       0x802D0F2Cu /* J2DScreen::draw(x,y,grafCtx*)      */
#define A_J2D_FILLBOX    0x802CDF3Cu /* J2DFillBox(x,y,w,h,TColor*)        */
#define A_DPA_SET        0x8007D1DCu /* dPa_control_c::set(...)            */
#define A_GXSETVIEWPORT  0x803271C8u /* GXSetViewport(x,y,w,h,n,f)         */
#define A_SETPOS         0x80081850u /* dDlst_2Dm_c::setPos(s16 x4)        */
#define A_PAINTER        0x8000AF2Cu /* mDoGph_Painter (once per frame)    */
#define A_DRAW_JLE_PB    0x8022B320u /* dJle_Pb_c::draw                    */
#define A_DRAW_2DSCP     0x80237568u /* dDlst_2DSCP_c::draw                */
#define A_CALCSCISSOR    0x8004A3A4u /* dMap_c::calcScissor                */
#define A_NOTEAPPEAR_COL 0x8019E624u /* dMenu_Collect_c::noteAppear        */
#define A_NOTEAPPEAR_ITM 0x801CC1ACu /* dMenu_Item_c::noteAppear           */

/* boundary PCs that receive direct hooks (blr/return-address sites) */
#define B_JLE_BLR        0x8022B9E4u /* end of dJle_Pb_c::draw             */
#define B_JLE_SCRATCH    0x8022B9E0u /* addi r1,r1,80 just above the blr   */
#define B_2DSCP_BLR      0x802375E4u /* end of dDlst_2DSCP_c::draw         */
#define B_2DSCP_SCRATCH  0x802375E0u /* addi r1,r1,16 just above the blr   */
#define B_FMAP_RET       0x801B6978u /* return of bl paneTrans @0x801B6974 */
#define B_FMAP2_RET      0x801C5188u /* return of bl paneTrans @0x801C5184 */
#define B_COLLECT_MID    0x8019E6CCu /* return of bl 0x80031C38 @0x8019E6C8*/

/* ---- guest data addresses ------------------------------------------------ */
#define D_ORTHO_L     0x803F7D68u /* f32 -9.0   -> -123.0                  */
#define D_ORTHO_R     0x803F7D6Cu /* f32 650.0  ->  767.0                  */
#define D_UIRECT      0x803F89B8u /* f32[4] {-9,-21,659,524} -> {-123,-118,890,716} */
#define D_ASPECT      0x803FA998u /* f32 1.3333 -> 1.77778 (d_camera @17778) */
#define D_2DT_SCALE   0x803FB77Cu /* f32 1.02969 -> 1.390625               */
#define D_2DT_X       0x803FB78Cu /* f32 -9.0   -> -123.0                  */
#define D_FILLBOX_COL 0x803F891Cu /* GXColor 0x000000FF (opaque black)     */
#define D_TAB_6958    0x803E6958u /* s16 dMeter table entry (Gecko: 121)   */
/* calcScissor stores land in r13-relative .sbss globals (r13 = 0x803FE0E0,
 * set by addis/ori @0x80003288): stw r3,-30200 -> mScissorWidth,
 * stw r4,-30204 -> mScissorOrigX. Resolve off the live r13. */
#define SCISS_WIDTH_OFF  30200u /* r13-30200 = 0x803F6AE8 : mScissorWidth  */
#define SCISS_ORIGX_OFF  30204u /* r13-30204 = 0x803F6AE4 : mScissorOrigX  */
/* const-pool slots the Gecko patches load (r2 = 0x803FFD00): */
#define D_HIDE_F      0x803FB25Cu /* f32 0.0    = lfs -19108(r2)           */
/* mDispPosLeftUpX/Y live in r13-relative .sbss next to the scissor globals
 * (0x803F6AD8/0x803F6ADA; symbols.txt): read live by the calcScissor fix. */
#define SCISS_POSX_OFF   30216u /* r13-30216 = 0x803F6AD8 : mDispPosLeftUpX */

/* ---- aspect model ---------------------------------------------------------
 * MODERNGEKKO_WIDESCREEN_ASPECT selects the target display aspect A:
 *   "16:9" (default), "21:9", "32:9", "W:H" ratios, or a bare decimal
 *   ("2.37").  Clamped to [4:3, 32:9]; unset/invalid -> 16:9, reproducing
 *   the stock Gecko constants bit-for-bit.
 *
 * The Gecko numbers are hand-tuned for 16:9 but follow one consistent
 * layout, anchored by the game's own UI->pixel mapping (dMap_c::
 * calcScissor: px = (ui_x + 9) * 640 / 659 -- the [-9,650] x [-21,503] box
 * spans the 640x480 buffer):
 *
 *   W(A) = 640 * (A/(4/3)) * (267/256) = 500.625*A   widened ortho width
 *     The pure anamorphic width is 640*A/(4/3) = 853.33 at 16:9; the Gecko
 *     author used 890 (= *267/256), a ~4.3% cushion.  W proportional to A
 *     keeps HUD elements the same on-screen size at every aspect.
 *   m(A) = (W-640)/2          margin per side                 (125 @16:9)
 *   S(A) = m - 11             HUD anchor shift                (114 @16:9)
 *     = margin minus the 9-unit original overshoot kept inside the new
 *       edge and the box's +2 rightward center bias: [-123,767] is the
 *       window [0,640] plus [-(m-2), +(m+2)].
 *   xL = -9 - S = 2 - m       box left edge                   (-123)
 *   xR = xL + W = 642 + m     box right edge                  ( 767)
 *
 * Everything else derives from W/m/S:
 *   ortho L/R, UIRECT x/w, 2Dt x     = xL / xR / xL / W  (y/h stay fixed:
 *     the wipe quad over-covers the unchanged [-21,503] vertical box)
 *   2Dt scale                        = W/640  (stock 659/640 = 1.02969)
 *   camera aspect                    = A
 *   dMeter table                     = {35-S, shown-177, 590+S, 7+S, -S}
 *     (game ctor defaults {35, -180, 590, 7, 0} +/- S; the -256 hidden map
 *      position keeps the 177-unit slide distance off the shown pos)
 *   helperE/F/G, J2D draw, dPa, swim = +/- S
 *   paneScaleXY immediate            = S - 4                  (110 @16:9)
 *   helperC sight spread             = 0.75*A  (the 4/3 pool const the
 *                                      Gecko helper reuses, at 16:9)
 *   helperD margin bars              = +/- (m+5)              (130 @16:9)
 *   pictograph viewport              = w = 640*(4/3)/A keeps the capture a
 *     4:3 region on the stretched display; x = (640-w)/2 - 1 (79 @16:9)
 *   calcScissor immediates           = see ws_apply_aspect
 *
 * Derived state - the initializers ARE the 16:9 Gecko values so behavior
 * is exact even if hooks somehow run before ws_on_load. */
static float  s_aspect    = 1.7777778f;         /* A -> camera const      */
static float  s_ws_w      = 890.0f;             /* W(A)                   */
static float  s_shift     = 114.0f;             /* S(A)                   */
static float  s_left      = -123.0f;            /* xL                     */
static float  s_right     = 767.0f;             /* xR                     */
static float  s_scale2dt  = 1.390625f;          /* W/640                  */
static float  s_spread    = 1.3333333730697632f;/* 0.75*A (== 4/3 f32)    */
static float  s_bar       = 130.0f;             /* m + 5                  */
static float  s_vp_x      = 79.0f;              /* pictograph viewport    */
static float  s_vp_w      = 480.0f;
static int    s_tab[5]    = { -79, -256, 704, 121, -114 };
static float  s_pane_imm  = 110.0f;             /* S - 4                  */
static int    s_sciss_w   = 153;                /* calcScissor li r3      */
static int    s_sciss_x   = 32;                 /* calcScissor li r4      */

static int ws_iround(double v)        /* round half away from zero */
{
    return (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* MODERNGEKKO_WIDESCREEN_ASPECT value -> A, or <=0 when unparseable.
 * Accepts "W:H" ("21:9"), the same "custom W:H" form the config parser
 * emits, and bare decimals ("2.37").  Rejects empty, junk-suffixed,
 * non-finite, or non-positive input so a malformed value can never
 * produce NaN/Inf constants. */
static double ws_parse_aspect(const char* raw)
{
    char* end;
    double w, h, a;
    static const char pfx[] = "custom";
    int i;
    if (!raw)
        return -1.0;
    while (*raw == ' ' || *raw == '\t')
        ++raw;
    /* optional "custom" prefix (case-insensitive, whitespace-separated -
     * same grammar the config parser uses) so aspect_ratio=custom W:H can
     * be pasted into the env verbatim */
    for (i = 0; pfx[i]; ++i) {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != pfx[i])
            break;
    }
    if (!pfx[i] && (raw[6] == ' ' || raw[6] == '\t' || raw[6] == '\0')) {
        raw += 6;
        while (*raw == ' ' || *raw == '\t')
            ++raw;
    }
    if (!*raw)
        return -1.0;
    w = strtod(raw, &end);
    if (end == raw)
        return -1.0;
    if (*end == ':') {
        const char* p = end + 1;
        h = strtod(p, &end);
        if (end == p || !(h > 0.0))
            return -1.0;
        a = w / h;
    } else {
        a = w;
    }
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end || !(a == a) || !(a > 0.0) || a > 1e30)  /* NaN/inf-safe */
        return -1.0;
    return a;
}

/* Parse + clamp + compute every derived constant.  raw==NULL or invalid
 * restores the stock 16:9 values. */
static void ws_apply_aspect(const char* raw)
{
    double a = ws_parse_aspect(raw);
    double W, m, S, xL, xR, vp_w, vp_x;
    int posX, x0, w_px;
    if (!(a > 0.0))
        a = 16.0 / 9.0;
    if (a < 4.0 / 3.0)
        a = 4.0 / 3.0;
    if (a > 32.0 / 9.0)
        a = 32.0 / 9.0;
    W  = 500.625 * a;                 /* 640*(A/(4/3))*(267/256) */
    m  = (W - 640.0) * 0.5;
    S  = m - 11.0;
    xL = -9.0 - S;
    xR = xL + W;
    vp_w = (2560.0 / 3.0) / a;        /* 640*(4/3)/A */
    if (vp_w > 640.0)
        vp_w = 640.0;
    vp_x = (640.0 - vp_w) * 0.5 - 1.0;
    if (vp_x < 0.0)
        vp_x = 0.0;
    /* calcScissor: the Gecko li immediates 153 (width) / 32 (x floor) make
     * the stored scissor track the map pane under the NEW UI->px mapping
     * ((ui - xL) * 640/W) while the game still computes the OLD one
     * ((ui + 9) * 640/659).  At the shown map pos posX = 35-S the true px
     * origin is (posX - xL)*640/W and the map's px width is
     * 119.44*640/W (116 px stock = 119.44 UI units).  width_imm then makes
     * the patched clamp (width += x when x < 0) land on that width. */
    posX = ws_iround(35.0 - S);
    x0   = (int)(((float)posX + 9.0f) * 640.0f / 659.0f); /* f32 + trunc */
    w_px = ws_iround(76444.0 / W);
    s_aspect   = (float)a;
    s_ws_w     = (float)W;
    s_shift    = (float)S;
    s_left     = (float)xL;
    s_right    = (float)xR;
    s_scale2dt = (float)(W / 640.0);
    s_spread   = (float)(0.75 * a);
    s_bar      = (float)(m + 5.0);
    s_vp_w     = (float)vp_w;
    s_vp_x     = (float)vp_x;
    s_tab[0]   = ws_iround(35.0 - S);    /* map shown pos  (0x803E68E4) */
    s_tab[1]   = s_tab[0] - 177;         /* map hidden pos (0x803E68E8) */
    s_tab[2]   = ws_iround(590.0 + S);   /* icon-free X    (0x803E68F0) */
    s_tab[3]   = ws_iround(7.0 + S);     /* pane x offset  (0x803E6958) */
    s_tab[4]   = ws_iround(-S);          /* meter x offset (0x803E69A4) */
    /* the Gecko site's li immediate is an integer: S-4 rounded (110 @16:9)
     * - i.e. the original table value 7 plus the same +S shift net of the
     * 11-unit anchoring bias (7 + (S-11) = S-4). */
    s_pane_imm = (float)ws_iround(S - 4.0);
    s_sciss_w  = w_px - (x0 < 0 ? x0 : 0);
    s_sciss_x  = ws_iround((posX - xL) * 640.0 / W);
}

/* ---- enable gate ---------------------------------------------------------- */
static uint32_t s_enabled = 0;
static uint32_t s_debug = 0;
/* Gecko-faithful swim-tekari right-edge fold (see on_setPos).  On by default;
 * MODERNGEKKO_WS_SWIM_R6=0 restores the pre-fix behavior for A/B bisection. */
static uint32_t s_swim_r6 = 1;

/* ---- guest-memory helpers (direct-RAM, bounds-checked, like frame60) ------ */
static int in_ram(const CPUState* s, uint32_t ea, uint32_t size)
{
    return s->ram && s->ram_size >= size && (ea - 0x80000000u) <= s->ram_size - size;
}
static uint32_t rd32(CPUState* s, uint32_t a)
{
    if (in_ram(s, a, 4u)) {
        const uint8_t* p = s->ram + (a - 0x80000000u);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    return (uint32_t)moderngekko_mod_read(s, a, 4u);
}
static uint32_t rd16(CPUState* s, uint32_t a)
{
    if (in_ram(s, a, 2u)) {
        const uint8_t* p = s->ram + (a - 0x80000000u);
        return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
    }
    return (uint32_t)moderngekko_mod_read(s, a, 2u);
}
static uint8_t rd8(CPUState* s, uint32_t a)
{
    if (in_ram(s, a, 1u))
        return s->ram[a - 0x80000000u];
    return (uint8_t)moderngekko_mod_read(s, a, 1u);
}
static void wr32(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 4u)) {
        uint8_t* p = s->ram + (a - 0x80000000u);
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
        return;
    }
    moderngekko_mod_write(s, a, v, 4u);
}
static void wr16(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 2u)) {
        uint8_t* p = s->ram + (a - 0x80000000u);
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
        return;
    }
    moderngekko_mod_write(s, a, v, 2u);
}
static void wr8(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 1u)) {
        s->ram[a - 0x80000000u] = (uint8_t)v;
        return;
    }
    moderngekko_mod_write(s, a, v, 1u);
}
static float rd32f(CPUState* s, uint32_t a)
{
    union { uint32_t u; float f; } c;
    c.u = rd32(s, a);
    return c.f;
}
static void wr32f(CPUState* s, uint32_t a, float v)
{
    union { uint32_t u; float f; } c;
    c.f = v;
    wr32(s, a, c.u);
}

/* scalar single-precision result write: Gekko SP ops (lfs/fadds/fsubs/fmuls)
 * broadcast ps0 into ps1 as well, so mirror both halves. */
static void setf(CPUState* s, int i, float v)
{
    s->fpr[i] = (double)v;
    s->ps1[i] = (double)v;
}

/* ---- A: per-frame data writes (Gecko 02/04/06-data equivalents) ---------- */
static void apply_data_writes(CPUState* s)
{
    wr32f(s, D_ORTHO_L, s_left);           /* 043F7D68 C2F60000 (-123.0) */
    wr32f(s, D_ORTHO_R, s_right);          /* 043F7D6C 443FC000 ( 767.0) */
    wr32f(s, D_UIRECT + 0x0u, s_left);     /* 063F89B8 C2F60000          */
    wr32f(s, D_UIRECT + 0x4u, -118.0f);    /*          C2EC0000 (fixed v)*/
    wr32f(s, D_UIRECT + 0x8u,  s_ws_w);    /*          445E8000 ( 890.0) */
    wr32f(s, D_UIRECT + 0xCu,  716.0f);    /*          44330000 (fixed v)*/
    wr32f(s, D_ASPECT,   s_aspect);        /* 043FA998 3FE38E39 (1.7778) */
    wr32f(s, D_2DT_SCALE, s_scale2dt);     /* 043FB77C 3FB20000 (1.3906) */
    wr32f(s, D_2DT_X,    s_left);          /* 043FB78C C2F60000          */
    wr16 (s, 0x803E68E4u, (uint16_t)(int16_t)s_tab[0]);   /* -79  @16:9 */
    wr16 (s, 0x803E68E8u, (uint16_t)(int16_t)s_tab[1]);   /* -256       */
    wr16 (s, 0x803E68F0u, (uint16_t)(int16_t)s_tab[2]);   /*  704       */
    wr16 (s, D_TAB_6958,  (uint16_t)(int16_t)s_tab[3]);   /*  121       */
    wr16 (s, 0x803E69A4u, (uint16_t)(int16_t)s_tab[4]);   /* -114       */
}

/* once at first dispatch (DOL image already loaded), then every frame */
static void on_runtime_start(CPUState* state)
{
    apply_data_writes(state);
}
static void on_painter(CPUState* state)
{
    apply_data_writes(state);
}

/* ---- B: callee-entry, lr-gated hooks ------------------------------------- */

/* fopMsgM_paneTrans(pane*, f1, f2) - Gecko helper G (f1 -= S) plus the
 * lfs-constant sites whose loads feed the call. */
static void on_paneTrans(CPUState* s)
{
    switch (s->lr) {
    case 0x801B6978u:  /* 041B6968: lfs f1,-20072(r2) -> lfs f1,xL */
    case 0x801C5188u:  /* 041C5174: lfs f1,-19904(r2) -> lfs f1,xL */
        setf(s, 1, s_left);
        break;
    case 0x801F9BE4u: { /* 041F9BD4: lfs f1,-19108(r2) = [D_HIDE_F]=0.0,
                         * then helperG f1 -= S (parks the pane x at -S,
                         * under the left margin bar) */
        setf(s, 0, s_shift);
        setf(s, 1, rd32f(s, D_HIDE_F) - s_shift);
        break;
    }
    case 0x801FBA30u:  /* 041FBA20: fadds f1,f31,f0 -> fadds f1,f30,f0.
                        * f0 there is the 0x801FBA1C lfsx load [r27+r29];
                        * 0x801FBA24 reloads f0 before the bl, so the live
                        * fpr[0] is the wrong operand - read the stack slot
                        * via the same registers the lfsx used. */
        setf(s, 1, (float)(s->fpr[30] + (double)rd32f(s, s->gpr[27] + s->gpr[29])));
        break;
    case 0x801F1030u: case 0x801F1040u:  /* 041F102C / 041F103C */
    case 0x801F9C60u: case 0x801F9C70u:  /* 041F9C5C / 041F9C6C */
    case 0x801F9C80u: case 0x801F9C9Cu:  /* 041F9C7C / 041F9C98 */
    case 0x801FD614u: case 0x801FD624u:  /* 041FD610 / 041FD620 */
    case 0x801FD634u: {                  /* 041FD630 */
        /* helper G: lfs f0,[114.0]->S; fsubs f1,f1,f0; b paneTrans */
        setf(s, 0, s_shift);
        setf(s, 1, (float)(s->fpr[1] - (double)s_shift));
        break;
    }
    default:
        break;
    }
}

/* fopMsgM_setInitAlpha(pane*) - NOP'd call sites skip via pc=lr; helper F
 * sites subtract 114 from [pane+4] then let the callee run. */
static void on_setInitAlpha(CPUState* s)
{
    switch (s->lr) {
    case 0x8019E90Cu: case 0x801AC404u:
    case 0x801CA9ACu: case 0x801CC33Cu:
        s->pc = s->lr;                   /* 04..60000000: skip the call   */
        return;
    case 0x801F8F04u: case 0x801F8F0Cu: case 0x801F8F14u: {
        /* helper F: f0 = S; f1 = [r3+4] - f0; [r3+4] = f1 */
        float f1 = rd32f(s, s->gpr[3] + 4u) - s_shift;
        wr32f(s, s->gpr[3] + 4u, f1);
        setf(s, 0, s_shift);
        setf(s, 1, f1);
        break;
    }
    default:
        break;
    }
}

/* fopMsgM_setNowAlpha(pane*, f) - all listed call sites are NOP'd. */
static void on_setNowAlpha(CPUState* s)
{
    switch (s->lr) {
    case 0x8019E958u: case 0x8019EC74u: case 0x801AB614u:
    case 0x801AC450u: case 0x801AC774u: case 0x801B9560u:
    case 0x801CC388u: case 0x801CC77Cu: case 0x801D3800u:
    case 0x801DAA3Cu: case 0x801DAAECu:
        s->pc = s->lr;                   /* 04..60000000: skip the call   */
        return;
    default:
        break;
    }
}

/* fopMsgM_setAlpha(pane*) - helper E: r12 = [r3]; f0 = S;
 * f1 = [r12+0xC] + f0; [r12+0x48] = f1, then the callee runs normally. */
static void on_setAlpha(CPUState* s)
{
    switch (s->lr) {
    case 0x80204650u: case 0x80204658u: case 0x8020466Cu:
    case 0x80204674u: case 0x8020468Cu: {
        uint32_t r12 = rd32(s, s->gpr[3]);
        float f1 = rd32f(s, r12 + 0x0Cu) + s_shift;
        wr32f(s, r12 + 0x48u, f1);
        s->gpr[12] = r12;
        setf(s, 0, s_shift);
        setf(s, 1, f1);
        break;
    }
    default:
        break;
    }
}

/* fopMsgM_setPaneData(pane*, screen*, idx) - helper A: [[r28+4]+0xC] =
 * xL; at callee entry r4 is already that pointer. */
static void on_setPaneData(CPUState* s)
{
    if (s->lr == 0x80161048u)            /* site 0x8016103C */
        wr32f(s, s->gpr[4] + 0x0Cu, s_left);
}

/* PSMTXTrans(f1,f2,f3) - helper C mirrors x around 320 scaled by the pool
 * 4/3 constant: f1' = 320 - (320 - x) * 1.3333334 (boomerang sight spread).
 * The helper reads x fresh via `lfs f0,8(r1)`; the unpatched `lfs f1,8(r1)`
 * at the site leaves the same value in f1 (f2/f3 loads don't touch it), but
 * read [r1+8] like the helper does so the input can't be a stale register. */
static void on_psmtxtrans(CPUState* s)
{
    if (s->lr == 0x800E1640u) {          /* site 0x800E1630 -> helperC */
        const float c320 = rd32f(s, s->gpr[2] - 29068u);  /* lfs f2,-29068(r2) */
        const float x    = rd32f(s, s->gpr[1] + 8u);      /* lfs f0,8(r1)      */
        /* the helper's lfs f3,-30556(r2) grabs the game's 4/3 pool const -
         * numerically the anamorphic stretch 0.75*A at 16:9; the computed
         * value keeps the sight on the pure-stretched window at any A. */
        const float f0   = s_spread * (c320 - x);         /* fsubs + fmuls     */
        setf(s, 0, f0);
        setf(s, 1, c320 - f0);                            /* fsubs f1,f2,f0    */
    }
}

/* J2DScreen::draw(f1 x, f2 y, r4 grafCtx*) - meter pane draws get
 * f1 = S (the lfs -27188(r2) patch, shifting them right into the 4:3 frame) and
 * f2 = [D_HIDE_F] (the [r2-19108] const, 0.0); the or r4,r31 -> li r4,0
 * sites pass NULL ctx. */
static void on_j2d_draw(CPUState* s)
{
    switch (s->lr) {
    case 0x801F0688u: case 0x801F06A4u: case 0x801F0710u:
        /* 041F0678/694/700: lfs f1,-27188(r2)->S ; lfs f2,-19108(r2) */
        setf(s, 1, s_shift);
        setf(s, 2, rd32f(s, D_HIDE_F));
        break;
    case 0x8018F5D4u:                    /* 0418F5CC GameOverScrnDraw */
    case 0x8021E6F0u:                    /* 0421E6E8 Ow_mask          */
    case 0x80234530u:                    /* 04234528 proc_draw        */
        s->gpr[4] = 0u;                  /* or r4,r31,r31 -> li r4,0  */
        break;
    default:
        break;
    }
}

/* fopMsgM_paneScaleXY(pane*, f) - site 0x801F0B70 replaced a table lookup
 * lha r0,80(r5) ([0x803E6958]) with li r0,S-4: the pane's +0x1C position
 * field must end at f1 + (S-4) instead of f1 + s16(table). Fix the stored
 * value using the live table so the result matches either way. */
static void on_paneScaleXY(CPUState* s)
{
    if (s->lr == 0x801F0BC4u) {
        float corr = s_pane_imm - (float)(int16_t)rd16(s, D_TAB_6958);
        wr32f(s, s->gpr[3] + 0x1Cu, rd32f(s, s->gpr[3] + 0x1Cu) + corr);
    }
}

/* dPa_control_c::set(...) - site 0x801FC28C changes the stack-vec x
 * subtractor 320.0 -> 320.0-S (net +S on [r6+0], the emitter position). */
static void on_dpa_set(CPUState* s)
{
    if (s->lr == 0x801FC2E0u)
        wr32f(s, s->gpr[6], rd32f(s, s->gpr[6]) + s_shift);
}

/* GXSetViewport(f1..f6) - pictureDraw sites: f1 0->vp_x, f2 ->0,
 * f3 640->vp_w (f5 mirrors f1).  The pictograph keeps a 4:3-shaped capture
 * centered on the stretched display: w = 640*(4/3)/A, x = (640-w)/2 - 1. */
static void on_gxsetviewport(CPUState* s)
{
    if (s->lr == 0x80227A34u) {
        setf(s, 1, s_vp_x);  /* x: 04227A18 -> lfs f1,[r2-17768]=79.0  */
        setf(s, 2, 0.0f);    /* y: 04227A1C -> lfs f2,[r2-17724]=0.0   */
        setf(s, 3, s_vp_w);  /* w: 04227A20 -> lfs f3,[r2-17640]=480.0 */
        setf(s, 5, s_vp_x);  /* fmr f5,f1 follows the patched f1       */
    }
}

/* dDlst_2Dm_c::setPos(s16 r4..r7) - two int-position recomputes:
 *  lr 0x801FF06C (clockMultiMove): 041FEFB4 reads field 0x1954 not 0x194C:
 *      r4 = (int)([r23+6484] + [r23+6196])          (fadds + fctiwz trunc)
 *  lr 0x80201F1C (swimTekariScroll): 04201D8C loads f4 = 114.0.  That one
 *  constant feeds BOTH x edges of the marker rect:
 *      r29 -> r4 = (int)(114.0 + [r25+11420] - [r25+11436] * 0.5)   (x0)
 *      r27 -> r6 = (int)(114.0 + [r25+11420] + [r25+11436] * 0.5)   (x1)
 *  (verified in main.dol: 0x80201D90-0x80201DEC fadds/fsubs pairs feeding the
 *  or r4,r29 / or r6,r27 arg setup at 0x80201F08-0x80201F14).  Only fixing r4
 *  leaves x1 at the unshifted origin -> marker pane squashed by the shift.
 *  Residual Gecko-parity gap: the same f4 also drives an fcmpu segment chain
 *  (0x80201DF4-0x80201EDC) producing r30 for the follow-up 0x80081870 call -
 *  no dispatch boundary exists between the lfs and the branch tree, the
 *  chain's integer inputs are dead at callee entry, and helper 0x8003BEC4
 *  can't be invoked from a hook, so r30 keeps the unshifted-origin result.
 *  Cosmetic only (trail element offset on the swim meter). */
static void on_setPos(CPUState* s)
{
    if (s->lr == 0x801FF06Cu) {
        float v = rd32f(s, s->gpr[23] + 6484u) + rd32f(s, s->gpr[23] + 6196u);
        s->gpr[4] = (uint32_t)(int32_t)v;
    } else if (s->lr == 0x80201F1Cu) {
        float b  = rd32f(s, s->gpr[25] + 11436u) * 0.5f;
        float a  = rd32f(s, s->gpr[25] + 11420u);
        s->gpr[4] = (uint32_t)(int32_t)(s_shift + a - b);
        if (s_swim_r6)
            s->gpr[6] = (uint32_t)(int32_t)(s_shift + a + b);
    }
}

/* ---- C: boundary hooks (helper B stanzas - the patched PC is a bl-return
 * dispatch point; hook runs, then the ORIGINAL instruction at the PC
 * executes normally - matching the trampoline's fall-through). ---------- */

/* 0x801B6978 (orig addi r3,r27,10380): helper B stanza 1 -
 * lfs f0,[r2-32660] (the patched 767.0 ortho-right constant), then
 * stfs f0,20(r3). Read the live global like the helper does rather than
 * baking 767.0, and leave f0 holding it — both stanzas set f0 too. */
static void on_fmap_ret(CPUState* s)
{
    const float w = rd32f(s, D_ORTHO_R);
    setf(s, 0, w);
    wr32f(s, s->gpr[3] + 0x14u, w);
}

/* 0x801C5188 (orig cmpwi r28,2): helper B stanza 2 - same
 * lfs f0,[r2-32660] ; stfs f0,20(r3) pair. f0 is overwritten with the
 * loaded constant BEFORE the store, so [r3+0x14] gets 767.0, not
 * paneTrans's leftover. */
static void on_fmap2_ret(CPUState* s)
{
    const float w = rd32f(s, D_ORTHO_R);
    setf(s, 0, w);
    wr32f(s, s->gpr[3] + 0x14u, w);
}

/* ---- D: helper D margin bars via lr-redirect trampolines ---------------
 * Entry hook rewrites lr to the function's patched blr PC; the function's
 * own mtlr restore preserves it, so every return path lands on our blr
 * hook. There we arm J2DFillBox args, point pc at J2DFillBox and lr at the
 * scratch PC; the scratch hook arms box 2 and restores the real return. */
static uint32_t s_bar_resume_a = 0;    /* real return addr, dJle_Pb   */
static uint32_t s_bar_resume_b = 0;    /* real return addr, 2DSCP     */
static uint32_t s_bar_stage_a = 0;     /* trampoline stage guards     */
static uint32_t s_bar_stage_b = 0;

static void on_draw_jle_entry(CPUState* s)
{
    s_bar_resume_a = s->lr;
    s_bar_stage_a = 1u;
    s->lr = B_JLE_BLR;
}
static void on_draw_2dscp_entry(CPUState* s)
{
    s_bar_resume_b = s->lr;
    s_bar_stage_b = 1u;
    s->lr = B_2DSCP_BLR;
}
/* helper D box 1: J2DFillBox(-bar, -32, bar, 640, &black) - left margin;
 * bar = m+5 (130 at 16:9) so the covers track the widened window. */
static void on_jle_blr(CPUState* s)
{
    if (s_bar_stage_a != 1u) return;
    s_bar_stage_a = 2u;
    s->gpr[3] = D_FILLBOX_COL;
    setf(s, 1, -s_bar);
    setf(s, 2, -32.0f);
    setf(s, 3, s_bar);
    setf(s, 4, 640.0f);
    s->lr = B_JLE_SCRATCH;
    s->pc = A_J2D_FILLBOX;
}
static void on_jle_scratch(CPUState* s)
{
    if (s_bar_stage_a != 2u) return;
    s_bar_stage_a = 0u;
    s->gpr[3] = D_FILLBOX_COL;
    setf(s, 1, 640.0f);
    setf(s, 2, -32.0f);
    setf(s, 3, s_bar);
    setf(s, 4, 640.0f);
    s->lr = s_bar_resume_a;
    s->pc = A_J2D_FILLBOX;
}
static void on_2dscp_blr(CPUState* s)
{
    if (s_bar_stage_b != 1u) return;
    s_bar_stage_b = 2u;
    s->gpr[3] = D_FILLBOX_COL;
    setf(s, 1, -s_bar);
    setf(s, 2, -32.0f);
    setf(s, 3, s_bar);
    setf(s, 4, 640.0f);
    s->lr = B_2DSCP_SCRATCH;
    s->pc = A_J2D_FILLBOX;
}
static void on_2dscp_scratch(CPUState* s)
{
    if (s_bar_stage_b != 2u) return;
    s_bar_stage_b = 0u;
    s->gpr[3] = D_FILLBOX_COL;
    setf(s, 1, 640.0f);
    setf(s, 2, -32.0f);
    setf(s, 3, s_bar);
    setf(s, 4, 640.0f);
    s->lr = s_bar_resume_b;
    s->pc = A_J2D_FILLBOX;
}

/* ---- E: stb-NOP emulation (snapshot + restore) ------------------------- */
static uint32_t s_collect_this = 0;
static uint32_t s_collect_byte = 0;
static uint32_t s_item_this = 0;
static uint32_t s_item_byte = 0;

/* dMenu_Collect_c::noteAppear entry: snapshot [this+2469]. */
static void on_noteAppear_col(CPUState* s)
{
    s_collect_this = s->gpr[3];
    s_collect_byte = rd8(s, s->gpr[3] + 2469u);
}
/* after the bl 0x80031C38 inside the store path (r31 = this still live):
 * the Gecko NOP'd the store, so put the pre-store byte back. */
static void on_noteAppear_col_mid(CPUState* s)
{
    wr8(s, s->gpr[31] + 2469u, s_collect_byte);
}
/* function return - covers the bc path that skips the mid boundary. */
static void on_noteAppear_col_ret(CPUState* s)
{
    if (s_collect_this)
        wr8(s, s_collect_this + 2469u, s_collect_byte);
}
/* dMenu_Item_c::noteAppear entry/return - r3 = this is dead by the
 * epilogue, so restore via the saved pointer. */
static void on_noteAppear_itm(CPUState* s)
{
    s_item_this = s->gpr[3];
    s_item_byte = rd8(s, s->gpr[3] + 2469u);
}
static void on_noteAppear_itm_ret(CPUState* s)
{
    if (s_item_this)
        wr8(s, s_item_this + 2469u, s_item_byte);
}

/* ---- F: dMap_c::calcScissor post-fix ------------------------------------
 * The Gecko immediates inside calcScissor (li r3,116->width_imm and
 * li r4,0->x_floor) sit mid-block with no dispatch boundary, so the hook
 * re-derives the stored globals from the still-live input.  The patched
 * block is:
 *   int x = (int)((posX - -9.0f) * 640.0f / 659.0f);   // f32 -> fctiwz
 *   int width = width_imm;                             // li r3
 *   if (x < 0) { width += x; if (width < 0) width = 0; x = x_floor; }
 * Reproducing it verbatim generalizes to every aspect AND fixes the L7
 * linearization's two leaks: the x == 0 edge (Gecko stores origX 0, not
 * 32) and the width collapse once the unclamped width is already
 * negative (posX < -128, i.e. past ~20:9 at the shown map pos). */
static void on_calcScissor_ret(CPUState* s)
{
    const int posX = (int)(int16_t)rd16(s, s->gpr[13] - SCISS_POSX_OFF);
    int x = (int)(((float)posX + 9.0f) * 640.0f / 659.0f);
    int width = s_sciss_w;
    if (x < 0) {
        width += x;
        if (width < 0)
            width = 0;
        x = s_sciss_x;
    }
    wr32(s, s->gpr[13] - SCISS_WIDTH_OFF, (uint32_t)width);
    wr32(s, s->gpr[13] - SCISS_ORIGX_OFF, (uint32_t)x);
}

/* ---- registration -------------------------------------------------------- */
static void ws_on_load(const ModernGekkoModHostApi* api)
{
    const char* v;
    const char* d;
    (void)api;
    v = getenv("MODERNGEKKO_WIDESCREEN");
    s_enabled = (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' ||
                       v[0] == 't' || v[0] == 'T')) ? 1u : 0u;
    d = getenv("MODERNGEKKO_WIDESCREEN_DEBUG");
    s_debug = (d && d[0] == '1') ? 1u : 0u;
    v = getenv("MODERNGEKKO_WS_SWIM_R6");
    s_swim_r6 = (v && v[0] == '0') ? 0u : 1u;
    /* MODERNGEKKO_WIDESCREEN_ASPECT: "16:9" default; "21:9"/"32:9"/"W:H"
     * ratios or a bare decimal; clamped [4:3, 32:9]; invalid -> 16:9. */
    ws_apply_aspect(getenv("MODERNGEKKO_WIDESCREEN_ASPECT"));
    if (s_debug)
        fprintf(stderr, "[widescreen16x9] enabled=%u swim_r6=%u aspect=%.6f"
                " W=%.4f S=%.4f xL=%.4f xR=%.4f sciss=%d/%d\n",
                s_enabled, s_swim_r6, (double)s_aspect, (double)s_ws_w,
                (double)s_shift, (double)s_left, (double)s_right,
                s_sciss_w, s_sciss_x);
}

static void ws_on_unload(void)
{
    s_bar_stage_a = s_bar_stage_b = 0;
    s_collect_this = s_item_this = 0;
}

static const ModernGekkoModHook hooks[] = {
    /* per-frame data re-assert (Gecko handlers re-apply each frame) */
    RECOMP_HOOK(A_PAINTER, on_painter),
    /* B: callee-entry, lr-gated */
    RECOMP_HOOK(A_PANETRANS,     on_paneTrans),
    RECOMP_HOOK(A_SETINITALPHA,  on_setInitAlpha),
    RECOMP_HOOK(A_SETNOWALPHA,   on_setNowAlpha),
    RECOMP_HOOK(A_SETALPHA,      on_setAlpha),
    RECOMP_HOOK(A_SETPANEDATA,   on_setPaneData),
    RECOMP_HOOK(A_PSMTXTRANS,    on_psmtxtrans),
    RECOMP_HOOK(A_J2D_DRAW,      on_j2d_draw),
    RECOMP_HOOK(A_PANESCALEXY,   on_paneScaleXY),
    RECOMP_HOOK(A_DPA_SET,       on_dpa_set),
    RECOMP_HOOK(A_GXSETVIEWPORT, on_gxsetviewport),
    RECOMP_HOOK(A_SETPOS,        on_setPos),
    /* C: dispatch-boundary sites (helper B stanzas) */
    RECOMP_HOOK(B_FMAP_RET,  on_fmap_ret),
    RECOMP_HOOK(B_FMAP2_RET, on_fmap2_ret),
    /* D: margin-bar trampolines */
    RECOMP_HOOK(A_DRAW_JLE_PB,   on_draw_jle_entry),
    RECOMP_HOOK(B_JLE_BLR,       on_jle_blr),
    RECOMP_HOOK(B_JLE_SCRATCH,   on_jle_scratch),
    RECOMP_HOOK(A_DRAW_2DSCP,    on_draw_2dscp_entry),
    RECOMP_HOOK(B_2DSCP_BLR,     on_2dscp_blr),
    RECOMP_HOOK(B_2DSCP_SCRATCH, on_2dscp_scratch),
    /* E: stb-NOP snapshot/restore */
    RECOMP_HOOK(A_NOTEAPPEAR_COL,        on_noteAppear_col),
    RECOMP_HOOK(B_COLLECT_MID,           on_noteAppear_col_mid),
    RECOMP_HOOK_RETURN(A_NOTEAPPEAR_COL, on_noteAppear_col_ret),
    RECOMP_HOOK(A_NOTEAPPEAR_ITM,        on_noteAppear_itm),
    RECOMP_HOOK_RETURN(A_NOTEAPPEAR_ITM, on_noteAppear_itm_ret),
    /* F: scissor post-fix */
    RECOMP_HOOK_RETURN(A_CALCSCISSOR, on_calcScissor_ret),
};

static const ModernGekkoModCallback callbacks[] = {
    RECOMP_CALLBACK("*", "runtime_start", on_runtime_start),
};

static ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GZLE01",
    "widescreen16x9",
    "0.1.0",
    "Wind Waker true widescreen - native port of Dolphin's $16:9 Widescreen Gecko code, generalized to arbitrary aspects via MODERNGEKKO_WIDESCREEN_ASPECT (widened projection+ortho, relocated HUD)",
    0, 0u,
    0, 0u,
    hooks, (uint32_t)(sizeof(hooks) / sizeof(hooks[0])),
    0, 0u,
    0, 0u,
    0, 0u,
    callbacks, (uint32_t)(sizeof(callbacks) / sizeof(callbacks[0])),
    ws_on_load,
    ws_on_unload,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc* moderngekko_get_mod(void)
{
    ws_on_load(0);
    if (!s_enabled) {
        descriptor.hooks = 0;
        descriptor.num_hooks = 0u;
        descriptor.callbacks = 0;
        descriptor.num_callbacks = 0u;
    }
    return &descriptor;
}
