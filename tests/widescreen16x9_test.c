/* widescreen16x9 mod regression test - exercises the Gecko-helper folds
 * against the semantics decoded from vendor/dolphin's GZLE01 $16:9 code and
 * the retail main.dol (see mods/widescreen16x9/tools/).
 *
 * Covers the helpers where the port previously misdecoded instructions:
 *   helperE (setAlpha): [r12+0x48] = [r12+0xC] + 114.0   (fadds, not fmuls)
 *   helperB stanza 1/2: f0 = 767.0; [r3+0x14] = f0       (f0, not f2/leftover)
 *   041FBA20: fadds f1,f31,f0 -> fadds f1,f30,f0 with f0 = [r27+r29] (the
 *             0x801FBA1C lfsx value - f0 is reloaded before the bl)
 *   041F9BD4: lfs f1,[0x803FB25C]=0.0 then helperG -> f1 = -114.0
 *   041F0678/694/700: f1 = 114.0, f2 = [0x803FB25C] = 0.0 (not -131072)
 * plus the verified-correct folds (helperG/F/A, helperC mirror, scissor
 * post-fix, noteAppear snapshot/restore, margin-bar trampolines) and the
 * MODERNGEKKO_WIDESCREEN_ASPECT generalization: the 16:9 constants must
 * reproduce the stock Gecko values bit-for-bit while 21:9/32:9/decimal
 * targets follow the derived layout formulas. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../mods/widescreen16x9/mod.c"

static uint8_t s_memory[0x400000];

static uint32_t load_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void store_be32(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void store_f32(uint8_t* p, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    store_be32(p, v);
}

static float load_f32(const uint8_t* p)
{
    uint32_t v = load_be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Same contract as the frame60 harness: accesses masked to the MEM1 window,
 * unmapped reads return 0 / writes drop. */
static uint8_t* test_ptr(uint32_t address, uint8_t size)
{
    const uint32_t offset = address - 0x80000000u;
    if (size == 0u || offset >= sizeof(s_memory) ||
        (uint32_t)size > sizeof(s_memory) - offset)
        return NULL;
    return &s_memory[offset];
}

static uint64_t test_read(CPUState* state, uint32_t address, uint8_t size)
{
    const uint8_t* p;
    (void)state;
    p = test_ptr(address, size);
    if (!p)
        return 0;
    if (size == 1u)
        return p[0];
    if (size == 2u)
        return ((uint32_t)p[0] << 8) | p[1];
    return load_be32(p);
}

static void test_write(CPUState* state, uint32_t address, uint64_t value,
                       uint8_t size)
{
    uint8_t* p;
    (void)state;
    p = test_ptr(address, size);
    if (!p)
        return;
    if (size == 1u)
        p[0] = (uint8_t)value;
    else if (size == 2u)
    {
        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)value;
    }
    else
        store_be32(p, (uint32_t)value);
}

static CPUState s_state;

static void reset(void)
{
    memset(s_memory, 0, sizeof(s_memory));
    memset(&s_state, 0, sizeof(s_state));
    s_state.external_read = test_read;
    s_state.external_write = test_write;
}

static void put_f32(uint32_t ea, float v) { store_f32(&s_memory[ea - 0x80000000u], v); }
static void put_u32(uint32_t ea, uint32_t v) { store_be32(&s_memory[ea - 0x80000000u], v); }
static void put_s16(uint32_t ea, int v)
{
    const uint16_t u = (uint16_t)(int16_t)v;
    uint8_t* p = &s_memory[ea - 0x80000000u];
    p[0] = (uint8_t)(u >> 8); p[1] = (uint8_t)u;
}
static float get_f32(uint32_t ea) { return load_f32(&s_memory[ea - 0x80000000u]); }
static uint32_t get_u32(uint32_t ea) { return load_be32(&s_memory[ea - 0x80000000u]); }

static int s_fails = 0;
static void check_f(const char* name, float got, float want)
{
    if (got != want) {
        ++s_fails;
        fprintf(stderr, "FAIL: %s: got %f want %f\n", name, got, want);
    }
}
static void check_u32(const char* name, uint32_t got, uint32_t want)
{
    if (got != want) {
        ++s_fails;
        fprintf(stderr, "FAIL: %s: got 0x%08X want 0x%08X\n", name, got, want);
    }
}

/* Seed the const-pool slots the Gecko patches load (values read back from
 * retail main.dol: D_HIDE_F=0.0, D_ORTHO_R=650->767 after apply_data_writes,
 * D_TAB_6958=121 after writes). The 114.0 lfs const is now the computed
 * s_shift, so only the genuinely-independent slots are seeded. */
static void seed_consts(void)
{
    put_f32(D_HIDE_F, 0.0f);
}

/* Cross-runtime env setter for the ws_on_load env-parse checks. */
static void ws_setenv(const char* name, const char* value)
{
#ifdef _WIN32
    static char buf[128];
    snprintf(buf, sizeof buf, "%s=%s", name, value);
    _putenv(buf);
#else
    if (value && *value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

int main(void)
{
    /* ---- per-frame data writes ----------------------------------------- */
    reset();
    seed_consts();
    apply_data_writes(&s_state);
    check_f("ortho L",   get_f32(D_ORTHO_L), -123.0f);
    check_f("ortho R",   get_f32(D_ORTHO_R), 767.0f);
    check_f("uirect x0", get_f32(D_UIRECT), -123.0f);
    check_f("uirect x2", get_f32(D_UIRECT + 8u), 890.0f);
    check_f("aspect",    get_f32(D_ASPECT), 1.7777778f);
    check_f("2d scale",  get_f32(D_2DT_SCALE), 1.390625f);
    check_f("2d x",      get_f32(D_2DT_X), -123.0f);
    check_u32("tab6958", get_u32(D_TAB_6958) >> 16, 0x79u);

    /* ---- helper E: setAlpha ---------------------------------------------
     * r12 = [r3]; [r12+0x48] = [r12+0xC] + 114.0 - must ADD (Gecko fadds),
     * and leave r12/f0/f1 holding the helper's results. */
    reset();
    seed_consts();
    {
        const uint32_t pane = 0x80100000u, sub = 0x80200000u;
        put_u32(pane, sub);          /* [r3] = r12 target   */
        put_f32(sub + 0x0Cu, 300.0f);
        s_state.gpr[3] = pane;
        s_state.lr = 0x80204650u;
        on_setAlpha(&s_state);
        check_f("helperE store", get_f32(sub + 0x48u), 414.0f);
        check_f("helperE f0", (float)s_state.fpr[0], 114.0f);
        check_f("helperE f1", (float)s_state.fpr[1], 414.0f);
        check_u32("helperE r12", s_state.gpr[12], sub);
        /* lr not in the gate list -> no write */
        put_f32(sub + 0x0Cu, 10.0f);
        s_state.lr = 0x80204651u;
        on_setAlpha(&s_state);
        check_f("helperE ungated", get_f32(sub + 0x48u), 414.0f);
    }

    /* ---- helper B stanzas -------------------------------------------------
     * both are lfs f0,[D_ORTHO_R]; stfs f0,20(r3): f0 (not f2) gets 767.0
     * and the store lands f0, not a caller leftover. */
    reset();
    seed_consts();
    apply_data_writes(&s_state);
    {
        const uint32_t pane = 0x80110000u;
        s_state.gpr[3] = pane;
        s_state.fpr[0] = -55.5;    /* caller leftover - must be overwritten */
        s_state.fpr[2] = 1234.0;   /* must NOT be touched */
        on_fmap_ret(&s_state);
        check_f("stanza1 store", get_f32(pane + 0x14u), 767.0f);
        check_f("stanza1 f0", (float)s_state.fpr[0], 767.0f);
        check_f("stanza1 f2", (float)s_state.fpr[2], 1234.0f);

        put_f32(pane + 0x14u, 0.0f);
        s_state.fpr[0] = -55.5;
        s_state.fpr[2] = 1234.0;
        on_fmap2_ret(&s_state);
        check_f("stanza2 store", get_f32(pane + 0x14u), 767.0f);
        check_f("stanza2 f0", (float)s_state.fpr[0], 767.0f);
        check_f("stanza2 f2", (float)s_state.fpr[2], 1234.0f);
    }

    /* ---- paneTrans folds -------------------------------------------------- */
    reset();
    seed_consts();
    apply_data_writes(&s_state);
    /* helperG plain site: f1 -= 114 (and f0 = 114 like the helper's lfs) */
    s_state.lr = 0x801F1030u;
    s_state.fpr[1] = 320.0;
    s_state.fpr[0] = 777.0;
    on_paneTrans(&s_state);
    check_f("helperG f1", (float)s_state.fpr[1], 206.0f);
    check_f("helperG f0", (float)s_state.fpr[0], 114.0f);

    /* 041F9BD4: lfs f1,[0x803FB25C]=0.0 then helperG -> f1 = -114.0 */
    s_state.lr = 0x801F9BE4u;
    s_state.fpr[1] = 999.0;   /* stale - fully replaced by the lfs */
    s_state.fpr[0] = 0.0;
    on_paneTrans(&s_state);
    check_f("9BD4 f1", (float)s_state.fpr[1], -114.0f);
    check_f("9BD4 f0", (float)s_state.fpr[0], 114.0f);

    /* 041FBA20: fadds f1,f31,f0 -> fadds f1,f30,f0 where f0 is the
     * 0x801FBA1C lfsx value [r27+r29]; f0 gets reloaded by the
     * 0x801FBA24 lfsx before the bl, so live fpr[0] must NOT be used. */
    s_state.lr = 0x801FBA30u;
    s_state.gpr[27] = 0x80300020u;   /* r27 = r1+20 region   */
    s_state.gpr[29] = 8u;            /* r29 = element index  */
    put_f32(0x80300028u, 40.0f);     /* [r27+r29] = vecA[i]  */
    s_state.fpr[30] = 200.0;
    s_state.fpr[31] = -1.0;
    s_state.fpr[0] = 7777.0;         /* reloaded f0 - trap value */
    s_state.fpr[1] = 0.0;
    on_paneTrans(&s_state);
    check_f("FBA20 f1", (float)s_state.fpr[1], 240.0f);

    /* lfs-const sites: f1 = -123.0 */
    s_state.lr = 0x801B6978u;
    s_state.fpr[1] = 5.0;
    on_paneTrans(&s_state);
    check_f("1B6968 f1", (float)s_state.fpr[1], -123.0f);
    s_state.lr = 0x801C5188u;
    on_paneTrans(&s_state);
    check_f("1C5174 f1", (float)s_state.fpr[1], -123.0f);

    /* ---- helper F: setInitAlpha ---------------------------------------- */
    reset();
    seed_consts();
    {
        const uint32_t pane = 0x80120000u;
        put_f32(pane + 4u, 500.0f);
        s_state.gpr[3] = pane;
        s_state.lr = 0x801F8F04u;
        on_setInitAlpha(&s_state);
        check_f("helperF store", get_f32(pane + 4u), 386.0f);
        check_f("helperF f0", (float)s_state.fpr[0], 114.0f);
        check_f("helperF f1", (float)s_state.fpr[1], 386.0f);

        /* NOP'd bl site: pc = lr skips the call */
        s_state.pc = A_SETINITALPHA;
        s_state.lr = 0x8019E90Cu;
        on_setInitAlpha(&s_state);
        check_u32("setInitAlpha skip", s_state.pc, 0x8019E90Cu);
    }

    /* ---- setNowAlpha NOP sites ----------------------------------------- */
    reset();
    s_state.pc = A_SETNOWALPHA;
    s_state.lr = 0x801AB614u;
    on_setNowAlpha(&s_state);
    check_u32("setNowAlpha skip", s_state.pc, 0x801AB614u);
    s_state.pc = A_SETNOWALPHA;
    s_state.lr = 0x801AB615u;         /* not gated -> runs */
    on_setNowAlpha(&s_state);
    check_u32("setNowAlpha ungated", s_state.pc, A_SETNOWALPHA);

    /* ---- helper A: setPaneData ------------------------------------------ */
    reset();
    seed_consts();
    {
        const uint32_t ptr = 0x80130000u;
        s_state.gpr[4] = ptr;
        s_state.lr = 0x80161048u;
        on_setPaneData(&s_state);
        check_f("helperA", get_f32(ptr + 0x0Cu), -123.0f);
    }

    /* ---- helper C: psmtxtrans mirror ---------------------------------------
     * The hook is PSMTXTrans-entry (lr == 0x800E1640) standing in for the
     * 0x800E1630 `lfs f1,8(r1)` -> b helperC patch. Like the helper it reads
     * x from [r1+8] and the 320/4-3 pool consts via r2-relative lfs:
     *   r2 = 0x803FFD00 -> [r2-29068] = 0x803F8B74 = 320.0
     *                      [r2-30556] = 0x803F85A4 = 1.3333333730697632
     * and computes in single precision (fsubs/fmuls), so the expected value is
     * the f32 result, not the double-precision equivalent. */
    reset();
    seed_consts();
    s_state.gpr[1] = 0x80200000u;                 /* caller frame -> [r1+8] = x */
    s_state.gpr[2] = 0x803FFD00u;                 /* SDA2 base */
    put_f32(0x803F8B74u, 320.0f);                 /* c320 = [r2-29068] */
    put_f32(0x803F85A4u, 1.3333333730697632f);    /* c43  = [r2-30556] */
    s_state.lr = 0x800E1640u;
    put_f32(0x80200000u + 8u, 320.0f);            /* x at screen centre -> stays */
    on_psmtxtrans(&s_state);
    check_f("helperC centre f1", (float)s_state.fpr[1], 320.0f);
    check_f("helperC centre f0", (float)s_state.fpr[0], 0.0f);
    put_f32(0x80200000u + 8u, 0.0f);              /* left edge -> 320-426.667 */
    on_psmtxtrans(&s_state);
    check_f("helperC left f1", (float)s_state.fpr[1], -106.66668701171875f);
    check_f("helperC left f0", (float)s_state.fpr[0], 426.66668701171875f);
    s_state.lr = 0x800E1641u;                     /* ungated -> no write */
    s_state.fpr[1] = 7.0;
    on_psmtxtrans(&s_state);
    check_f("helperC ungated", (float)s_state.fpr[1], 7.0f);

    /* ---- J2DScreen::draw folds ------------------------------------------ */
    reset();
    seed_consts();
    s_state.lr = 0x801F0688u;
    s_state.fpr[1] = -9.0;
    s_state.fpr[2] = -21.0;
    on_j2d_draw(&s_state);
    check_f("draw f1", (float)s_state.fpr[1], 114.0f);
    check_f("draw f2", (float)s_state.fpr[2], 0.0f);
    /* or r4,r31 -> li r4,0 */
    s_state.lr = 0x8018F5D4u;
    s_state.gpr[4] = 0xDEADBEEFu;
    s_state.fpr[1] = 1.0;
    on_j2d_draw(&s_state);
    check_u32("draw null ctx", s_state.gpr[4], 0u);
    check_f("draw null ctx f1", (float)s_state.fpr[1], 1.0f);

    /* ---- paneScaleXY: [r3+0x1C] += 110 - s16(live table) ------------------ */
    reset();
    seed_consts();
    apply_data_writes(&s_state);     /* writes 121 to D_TAB_6958 */
    {
        const uint32_t pane = 0x80140000u;
        put_f32(pane + 0x1Cu, 200.0f);
        s_state.gpr[3] = pane;
        s_state.lr = 0x801F0BC4u;
        on_paneScaleXY(&s_state);
        check_f("scaleXY corr", get_f32(pane + 0x1Cu), 189.0f);
    }

    /* ---- dPa_control_c::set: [r6+0] += 114 -------------------------------- */
    reset();
    seed_consts();
    {
        const uint32_t vec = 0x80150000u;
        put_f32(vec, 10.0f);
        s_state.gpr[6] = vec;
        s_state.lr = 0x801FC2E0u;
        on_dpa_set(&s_state);
        check_f("dpa +114", get_f32(vec), 124.0f);
    }

    /* ---- GXSetViewport: f1=79 f2=0 f3=480 f5=79 --------------------------- */
    reset();
    s_state.lr = 0x80227A34u;
    s_state.fpr[1] = s_state.fpr[2] = s_state.fpr[3] = s_state.fpr[5] = -1.0;
    on_gxsetviewport(&s_state);
    check_f("vp f1", (float)s_state.fpr[1], 79.0f);
    check_f("vp f2", (float)s_state.fpr[2], 0.0f);
    check_f("vp f3", (float)s_state.fpr[3], 480.0f);
    check_f("vp f5", (float)s_state.fpr[5], 79.0f);

    /* ---- setPos folds ------------------------------------------------------ */
    reset();
    {
        const uint32_t r23 = 0x80160000u, r25 = 0x80170000u;
        /* clock: r4 = (int)([r23+6484] + [r23+6196]) */
        put_f32(r23 + 6484u, 130.7f);
        put_f32(r23 + 6196u, 21.2f);
        s_state.gpr[23] = r23;
        s_state.lr = 0x801FF06Cu;
        on_setPos(&s_state);
        check_u32("setPos clock", s_state.gpr[4], 151u);
        /* swim: 04201D8C's f4 feeds both x edges of the marker rect:
         *   r4 = (int)(114 + [r25+11420] - [r25+11436]*0.5)   (x0, r29)
         *   r6 = (int)(114 + [r25+11420] + [r25+11436]*0.5)   (x1, r27) */
        put_f32(r25 + 11420u, 200.0f);
        put_f32(r25 + 11436u, 40.0f);
        s_state.gpr[25] = r25;
        s_state.gpr[6] = 0u;
        s_state.lr = 0x80201F1Cu;
        on_setPos(&s_state);
        check_u32("setPos swim x0", s_state.gpr[4], 294u);
        check_u32("setPos swim x1", s_state.gpr[6], 334u);
        /* kill switch: MODERNGEKKO_WS_SWIM_R6=0 leaves r6 untouched */
        s_swim_r6 = 0u;
        s_state.gpr[6] = 0u;
        on_setPos(&s_state);
        check_u32("setPos swim x1 off", s_state.gpr[6], 0u);
        s_swim_r6 = 1u;
    }

    /* ---- margin-bar trampolines ------------------------------------------- */
    reset();
    s_state.lr = 0x80001234u;
    on_draw_jle_entry(&s_state);
    check_u32("jle lr redirect", s_state.lr, B_JLE_BLR);
    s_state.pc = B_JLE_BLR;
    on_jle_blr(&s_state);
    check_u32("jle blr pc", s_state.pc, A_J2D_FILLBOX);
    check_u32("jle blr lr", s_state.lr, B_JLE_SCRATCH);
    check_f("jle box1 x", (float)s_state.fpr[1], -130.0f);
    check_f("jle box1 h", (float)s_state.fpr[4], 640.0f);
    check_u32("jle box1 col", s_state.gpr[3], D_FILLBOX_COL);
    s_state.pc = B_JLE_SCRATCH;
    on_jle_scratch(&s_state);
    check_u32("jle scr pc", s_state.pc, A_J2D_FILLBOX);
    check_u32("jle scr lr", s_state.lr, 0x80001234u);
    check_f("jle box2 x", (float)s_state.fpr[1], 640.0f);
    /* stage guard: re-fire without entry is a no-op */
    s_state.pc = B_JLE_BLR;
    s_state.fpr[1] = 0.0;
    on_jle_blr(&s_state);
    check_u32("jle stage guard", s_state.pc, B_JLE_BLR);
    /* 2DSCP path */
    s_state.lr = 0x80005678u;
    on_draw_2dscp_entry(&s_state);
    on_2dscp_blr(&s_state);
    check_u32("2dscp blr pc", s_state.pc, A_J2D_FILLBOX);
    on_2dscp_scratch(&s_state);
    check_u32("2dscp resume", s_state.lr, 0x80005678u);

    /* ---- noteAppear stb-NOP snapshot/restore ------------------------------- */
    reset();
    {
        const uint32_t obj = 0x80180000u;
        /* collect path: entry snapshot, mid restores via r31, ret restores
         * via saved this */
        s_state.gpr[3] = obj;
        s_memory[obj + 2469u - 0x80000000u] = 0xAB;
        on_noteAppear_col(&s_state);
        s_memory[obj + 2469u - 0x80000000u] = 0x55;  /* the NOP'd stb lands */
        s_state.gpr[31] = obj;
        on_noteAppear_col_mid(&s_state);
        check_u32("collect mid restore",
                  s_memory[obj + 2469u - 0x80000000u], 0xABu);
        s_memory[obj + 2469u - 0x80000000u] = 0x55;
        on_noteAppear_col_ret(&s_state);
        check_u32("collect ret restore",
                  s_memory[obj + 2469u - 0x80000000u], 0xABu);
        /* item path */
        s_state.gpr[3] = obj;
        s_memory[obj + 2469u - 0x80000000u] = 0xCD;
        on_noteAppear_itm(&s_state);
        s_memory[obj + 2469u - 0x80000000u] = 0x77;
        on_noteAppear_itm_ret(&s_state);
        check_u32("item ret restore",
                  s_memory[obj + 2469u - 0x80000000u], 0xCDu);
    }

    /* ---- calcScissor post-fix ----------------------------------------------
     * The hook re-derives the patched block from the still-live input:
     *   mDispPosLeftUpX = [r13-30216] = 0x803F6AD8  (s16)
     *   x = trunc((posX + 9) * 640/659); width = s_sciss_w;
     *   if (x < 0) { width += x; if (width < 0) width = 0; x = s_sciss_x; }
     *   mScissorWidth  = [r13-30200] = 0x803F6AE8   (0404A428 li r3,116->153)
     *   mScissorOrigX  = [r13-30204] = 0x803F6AE4   (0404A444 li r4,0->32)
     * Seed r13 to the retail SDA base and drive posX. At 16:9 with the map
     * shown (posX = -79): x = -67 -> width 153-67 = 86, origX = 32 - the
     * same results the Gecko immediates produce, and the same results the
     * L7 linearized +37/+32 post-fix produced for this input. */
    reset();
    s_state.gpr[13] = 0x803FE0E0u;
    put_s16(0x803F6AD8u, -79);
    on_calcScissor_ret(&s_state);
    check_u32("scissor width shown",  get_u32(0x803F6AE8u), 86u);
    check_u32("scissor origX shown",  get_u32(0x803F6AE4u), 32u);
    /* posX = 0: x = 8 >= 0, no clamp - width is the plain 153, origX the
     * computed 8 (not 32 - the x==0/x>0 paths keep the true value). */
    put_s16(0x803F6AD8u, 0);
    on_calcScissor_ret(&s_state);
    check_u32("scissor width unclamped", get_u32(0x803F6AE8u), 153u);
    check_u32("scissor origX unclamped", get_u32(0x803F6AE4u), 8u);
    /* posX = -150: the L7 linearization collapsed here (unpatched width is
     * already -20 -> stored 0 -> stays 0); the verbatim recompute yields the
     * Gecko result 153-136 = 17. */
    put_s16(0x803F6AD8u, -150);
    on_calcScissor_ret(&s_state);
    check_u32("scissor width deep", get_u32(0x803F6AE8u), 17u);
    check_u32("scissor origX deep", get_u32(0x803F6AE4u), 32u);
    /* posX = -9: x == 0 exactly - Gecko stores origX = 0 (the L7 post-fix
     * wrote 32; off by the margin for that frame). */
    put_s16(0x803F6AD8u, -9);
    on_calcScissor_ret(&s_state);
    check_u32("scissor origX zero", get_u32(0x803F6AE4u), 0u);

    /* ---- aspect model ------------------------------------------------------
     * Unset/invalid -> 16:9 and every derived constant must equal the stock
     * Gecko value bit-for-bit; wider aspects follow the layout formulas. */
    ws_apply_aspect(NULL);
    check_f("A16 aspect", s_aspect, 1.7777778f);         /* 0x3FE38E39 */
    check_f("A16 W",      s_ws_w, 890.0f);
    check_f("A16 S",      s_shift, 114.0f);
    check_f("A16 xL",     s_left, -123.0f);
    check_f("A16 xR",     s_right, 767.0f);
    check_f("A16 scale",  s_scale2dt, 1.390625f);
    check_f("A16 spread", s_spread, 1.3333333730697632f);/* 4/3 f32 */
    check_f("A16 bar",    s_bar, 130.0f);
    check_f("A16 vp x",   s_vp_x, 79.0f);
    check_f("A16 vp w",   s_vp_w, 480.0f);
    check_f("A16 pane",   s_pane_imm, 110.0f);
    check_u32("A16 tab0", (uint32_t)s_tab[0], (uint32_t)-79);
    check_u32("A16 tab1", (uint32_t)s_tab[1], (uint32_t)-256);
    check_u32("A16 tab2", (uint32_t)s_tab[2], 704u);
    check_u32("A16 tab3", (uint32_t)s_tab[3], 121u);
    check_u32("A16 tab4", (uint32_t)s_tab[4], (uint32_t)-114);
    check_u32("A16 scisW", (uint32_t)s_sciss_w, 153u);
    check_u32("A16 scisX", (uint32_t)s_sciss_x, 32u);
    /* "16:9" spelled out and invalid/junk inputs all land on the same
     * stock values. */
    ws_apply_aspect("16:9");
    check_f("A16 explicit", s_aspect, 1.7777778f);
    ws_apply_aspect("bogus");
    check_f("A16 bogus", s_aspect, 1.7777778f);
    check_f("A16 bogus S", s_shift, 114.0f);
    ws_apply_aspect("21:9junk");
    check_f("A16 junk", s_aspect, 1.7777778f);
    ws_apply_aspect("0");
    check_f("A16 zero", s_aspect, 1.7777778f);
    ws_apply_aspect("-2");
    check_f("A16 neg", s_aspect, 1.7777778f);
    ws_apply_aspect("1e99");
    check_f("A16 inf", s_aspect, 1.7777778f);
    /* the serialized "custom W:H" form is accepted (case-insensitive) so a
     * config.ini value can be pasted into the env verbatim */
    ws_apply_aspect("custom 21:9");
    check_f("custom pfx", s_aspect, 2.3333333f);
    ws_apply_aspect("CUSTOM 21:9");
    check_f("CUSTOM pfx", s_aspect, 2.3333333f);
    ws_apply_aspect("customize");          /* prefix-like junk -> 16:9 */
    check_f("pfx junk", s_aspect, 1.7777778f);

    /* 21:9: W = 500.625*7/3 = 1168.125, S = 253.0625, xL = -262.0625,
     * xR = 906.0625, spread = 0.75*7/3 = 1.75, bars = 269.0625,
     * vp = 640*(4/3)/A = 365.714.. at x = (640-w)/2-1 = 136.143. */
    ws_apply_aspect("21:9");
    check_f("A21 aspect", s_aspect, 2.3333333f);         /* f32(7/3)   */
    check_f("A21 W",      s_ws_w, 1168.125f);
    check_f("A21 S",      s_shift, 253.0625f);
    check_f("A21 xL",     s_left, -262.0625f);
    check_f("A21 xR",     s_right, 906.0625f);
    check_f("A21 scale",  s_scale2dt, 1.8251953125f);
    check_f("A21 spread", s_spread, 1.75f);
    check_f("A21 bar",    s_bar, 269.0625f);
    check_f("A21 vp w",   s_vp_w, (float)(2560.0 / 7.0));
    check_f("A21 vp x",   s_vp_x, (float)((640.0 - 2560.0 / 7.0) * 0.5 - 1.0));
    check_f("A21 pane",   s_pane_imm, 249.0f);   /* li imm: round(S-4)   */
    check_u32("A21 tab0", (uint32_t)s_tab[0], (uint32_t)-218);
    check_u32("A21 tab1", (uint32_t)s_tab[1], (uint32_t)-395);
    check_u32("A21 tab2", (uint32_t)s_tab[2], 843u);
    check_u32("A21 tab3", (uint32_t)s_tab[3], 260u);
    check_u32("A21 tab4", (uint32_t)s_tab[4], (uint32_t)-253);
    check_u32("A21 scisW", (uint32_t)s_sciss_w, 267u);
    check_u32("A21 scisX", (uint32_t)s_sciss_x, 24u);
    /* data writes + hooks follow the aspect too */
    reset();
    apply_data_writes(&s_state);
    check_f("A21 ortho L", get_f32(D_ORTHO_L), -262.0625f);
    check_f("A21 ortho R", get_f32(D_ORTHO_R), 906.0625f);
    check_f("A21 aspect w", get_f32(D_ASPECT), 2.3333333f);
    check_f("A21 scale w", get_f32(D_2DT_SCALE), 1.8251953125f);
    check_u32("A21 tab6958", get_u32(D_TAB_6958) >> 16, 260u);
    {
        const uint32_t pane = 0x80100000u, sub = 0x80200000u;
        put_u32(pane, sub);
        put_f32(sub + 0x0Cu, 300.0f);
        s_state.gpr[3] = pane;
        s_state.lr = 0x80204650u;
        on_setAlpha(&s_state);
        check_f("A21 helperE", get_f32(sub + 0x48u), 553.0625f);
        put_f32(sub + 0x0Cu, 10.0f);
        /* swim marker: r4 = (int)(S + a - b), r6 = (int)(S + a + b) */
        put_f32(0x80170000u + 11420u, 200.0f);
        put_f32(0x80170000u + 11436u, 40.0f);
        s_state.gpr[25] = 0x80170000u;
        s_state.gpr[6] = 0u;
        s_state.lr = 0x80201F1Cu;
        on_setPos(&s_state);
        check_u32("A21 swim x0", s_state.gpr[4], 433u);   /* 253.0625+180 */
        check_u32("A21 swim x1", s_state.gpr[6], 473u);   /* 253.0625+220 */
        /* viewport follows the computed capture region */
        s_state.lr = 0x80227A34u;
        s_state.fpr[1] = s_state.fpr[3] = s_state.fpr[5] = -1.0;
        on_gxsetviewport(&s_state);
        check_f("A21 vp f1", (float)s_state.fpr[1], s_vp_x);
        check_f("A21 vp f3", (float)s_state.fpr[3], s_vp_w);
        /* scissor at the 21:9 shown map pos (-218): x = -202 ->
         * width 267-202 = 65, origX = 24 */
        s_state.gpr[13] = 0x803FE0E0u;
        put_s16(0x803F6AD8u, -218);
        on_calcScissor_ret(&s_state);
        check_u32("A21 scissor w", get_u32(0x803F6AE8u), 65u);
        check_u32("A21 scissor x", get_u32(0x803F6AE4u), 24u);
        /* margin bars track m+5 = 269.0625 */
        s_state.lr = 0x80001234u;
        on_draw_jle_entry(&s_state);
        s_state.pc = B_JLE_BLR;
        on_jle_blr(&s_state);
        check_f("A21 bar x0", (float)s_state.fpr[1], -269.0625f);
        check_f("A21 bar w",  (float)s_state.fpr[3], 269.0625f);
        s_bar_stage_a = 0u;
    }

    /* 32:9 clamp boundary and the 4:3 floor. */
    ws_apply_aspect("32:9");
    check_f("A32 aspect", s_aspect, 3.5555556f);         /* f32(32/9)  */
    check_f("A32 W",      s_ws_w, 1780.0f);
    check_f("A32 S",      s_shift, 559.0f);
    check_f("A32 xL",     s_left, -568.0f);
    check_f("A32 vp w",   s_vp_w, 240.0f);
    check_f("A32 vp x",   s_vp_x, 199.0f);
    check_u32("A32 scisW", (uint32_t)s_sciss_w, 543u);
    ws_apply_aspect("64:9");                              /* clamp hi */
    check_f("A64 clamp",  s_aspect, 3.5555556f);
    ws_apply_aspect("1:1");                               /* clamp lo */
    check_f("A11 clamp",  s_aspect, 1.3333334f);         /* f32(4/3)   */
    check_f("A11 W",      s_ws_w, 667.5f);
    check_f("A11 vp w",   s_vp_w, 640.0f);               /* full frame */
    check_f("A11 vp x",   s_vp_x, 0.0f);
    /* decimal form "2.37" = 237/100 */
    ws_apply_aspect("2.37");
    check_f("A237 aspect", s_aspect, 2.37f);
    check_f("A237 W",      s_ws_w, (float)(500.625 * 2.37));
    /* env path: ws_on_load reads MODERNGEKKO_WIDESCREEN_ASPECT */
    ws_setenv("MODERNGEKKO_WIDESCREEN_ASPECT", "21:9");
    ws_on_load(0);
    check_f("env aspect", s_aspect, 2.3333333f);
    ws_setenv("MODERNGEKKO_WIDESCREEN_ASPECT", "");
    ws_on_load(0);
    check_f("env reset", s_aspect, 1.7777778f);
    /* restore stock state for any later additions */
    ws_apply_aspect(NULL);

    if (s_fails) {
        fprintf(stderr, "%d widescreen16x9 checks FAILED\n", s_fails);
        return 1;
    }
    printf("widescreen16x9 checks OK\n");
    return 0;
}
