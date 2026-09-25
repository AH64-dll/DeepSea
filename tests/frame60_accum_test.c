#include <stdint.h>
#include <string.h>

#include "../mods/frame60-accum/mod.c"

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

static void store_be16(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static uint16_t load_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void store_f32(uint8_t* p, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    store_be32(p, v);
}

/* frame60 bulk-access page mirror: the engine's 0xCC00F000 ops take real host
 * pointers, so the shim can implement them directly. Guest phys offsets index
 * s_memory (MEM1). Canned scan results let tests drive boundary extraction
 * (including adversarial offsets) without the real OpcodeDecoder. */
static uint8_t* s_bulk_dst;
static uint32_t s_bulk_src;
static uint32_t* s_scan_out;
static const uint32_t* s_scan_canned;
static uint32_t s_scan_canned_n;
static uint32_t s_fake_base, s_fake_end, s_fake_wptr; /* PI fifo regs, phys addrs */
static uint32_t s_inject_result = 1;
static int s_force_inject_refuse;   /* model the engine's free-space refusal */

/* Mirror the host external_read/write contract: accesses are masked to the
 * physical window and unmapped addresses read 0 / drop writes. Without the
 * range check a bad guest pointer (e.g. below 0x80000000 wraps the index to
 * ~4 GiB) would make the harness itself read/write far out of bounds and
 * silently corrupt the test instead of surfacing the bad access. */
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
    (void)state;
    /* MMIO intercepts (outside the MEM1 window test_ptr covers). */
    if (address == PI_FIFO_BASE_REG) return s_fake_base;
    if (address == PI_FIFO_END_REG)  return s_fake_end;
    if (address == PI_FIFO_WPTR_REG) return s_fake_wptr;
    if (address == MG_BULK_STAT_EA)  return s_inject_result;
    const uint8_t* p = test_ptr(address, size);
    if (!p)
        return 0;
    if (size == 1u)
        return p[0];
    if (size == 2u)
        return ((uint32_t)p[0] << 8) | p[1];
    if (size == 8u)
        /* Mirror MMU::Read<u64>: the whole word byteswaps — hi u32 lane at
         * addr, lo at addr+4 — NOT two independent u32 reads. */
        return ((uint64_t)load_be32(p) << 32) | load_be32(p + 4);
    return load_be32(p);
}

static void test_write(CPUState* state, uint32_t address, uint64_t value, uint8_t size)
{
    (void)state;
    /* Bulk-page ops (host pointers — honored only inside host calls on the
     * real engine; the shim is always in that context). */
    if (address == MG_BULK_PTR_EA) { s_bulk_dst = (uint8_t*)(uintptr_t)value; return; }
    if (address == MG_BULK_SRC_EA) { s_bulk_src = (uint32_t)value; return; }
    if (address == MG_BULK_READ_EA)
    {
        uint32_t len = (uint32_t)value;
        if (s_bulk_dst && (uint64_t)s_bulk_src + len <= sizeof(s_memory))
            memcpy(s_bulk_dst, s_memory + s_bulk_src, len);
        return;
    }
    if (address == MG_SCAN_OUT_EA) { s_scan_out = (uint32_t*)(uintptr_t)value; return; }
    if (address == MG_SCAN_EA)
    {
        if (s_scan_out && s_scan_canned)
        {
            s_scan_out[0] = s_scan_canned_n;
            memcpy(s_scan_out + 1, s_scan_canned,
                   s_scan_canned_n * sizeof(uint32_t));
            memset(s_scan_out + 257, 0, 8 * sizeof(uint32_t));
        }
        return;
    }
    if (address == MG_BULK_FIFO_EA)
    {
        if (s_force_inject_refuse) { s_inject_result = 0; return; }
        /* Mirror UpdateGatherPipe: bytes land at wptr, wptr advances w/ wrap. */
        uint32_t len = (uint32_t)value;
        uint32_t fsize = s_fake_end - s_fake_base;
        if (s_bulk_dst && len && (uint64_t)s_fake_wptr + len <= sizeof(s_memory) &&
            fsize && len <= fsize)
        {
            uint32_t first = s_fake_end - s_fake_wptr;
            if (first > len) first = len;
            memcpy(s_memory + s_fake_wptr, s_bulk_dst, first);
            if (len > first)
                memcpy(s_memory + s_fake_base, s_bulk_dst + first, len - first);
            s_fake_wptr += len;
            while (s_fake_wptr >= s_fake_end) s_fake_wptr -= fsize;
            s_inject_result = 1;
        }
        else
            s_inject_result = 0;
        return;
    }
    if (address == PI_FIFO_WPTR_REG) { s_fake_wptr = (uint32_t)value; return; }
    uint8_t* p = test_ptr(address, size);
    if (!p)
        return;
    if (size == 1u)
    {
        p[0] = (uint8_t)value;
    }
    else if (size == 2u)
    {
        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)value;
    }
    else if (size == 8u)
    {
        /* Mirror MMU::Write<u64>: hi u32 lane at addr, lo at addr+4. */
        store_be32(p, (uint32_t)(value >> 32));
        store_be32(p + 4, (uint32_t)value);
    }
    else
    {
        store_be32(p, (uint32_t)value);
    }
}

#define VIEW_EA 0x801D0000u
static void put_view(const float* v12, const float* p16)
{
    for (int i = 0; i < 12; ++i)
        store_f32(&s_memory[VIEW_EA + VIEW_OFF_VIEWMTX - 0x80000000u + i * 4u], v12[i]);
    for (int i = 0; i < 16; ++i)
        store_f32(&s_memory[VIEW_EA + VIEW_OFF_PROJMTX - 0x80000000u + i * 4u], p16[i]);
}
static int check_view(const float* want12, const float* want16)
{
    for (int i = 0; i < 12; ++i)
        if (be_f32(&s_memory[VIEW_EA + VIEW_OFF_VIEWMTX - 0x80000000u + i * 4u]) != want12[i])
            return 0;
    for (int i = 0; i < 16; ++i)
        if (be_f32(&s_memory[VIEW_EA + VIEW_OFF_PROJMTX - 0x80000000u + i * 4u]) != want16[i])
            return 0;
    return 1;
}

/* ==== D-pairlock sim harness ===========================================
 * waitForTick semantics (JFWDisplay.cpp:347-356): the hook rewrites p1 at
 * iteration N's beginRender; waitForTick then sets nextTick = exit_N + p1,
 * so the pad floors the interval ending at the NEXT beginRender — measured
 * as field_0x34_{N+1} and consumed by decide_{N+2}. On an R-iteration the
 * armed pad therefore paces the upcoming L-ending slot.
 *   delta_i = max(raw_i, pad_{i-1})
 *   raw_i   = tail(class_{i-1}) + head(class_i)
 * calibrated so the (R,L) slot stays pad-bound and the (L,R) slot is
 * work-bound ~692K — the observed real split ([dlt] logs: L-row deltas p50
 * ~691,721, R-row 675,072, post-slip slot ~695K).
 * VI model: copy_i lands at the iteration boundary t_i; the field at
 * k*675K shows the newest copy with t_i <= k*675K (boundary counts). */
#define PL_MAXN 3200u
static uint8_t  pl_cls[PL_MAXN];   /* class decided for iteration i      */
static uint32_t pl_dlt[PL_MAXN];   /* delta_i = interval ending at i      */
static uint32_t pl_pad[PL_MAXN];   /* pad the hook wrote during iter i    */
static uint64_t pl_tim[PL_MAXN];   /* t_i = copy land time (cum delta)    */

/* worktab: optional cyclic raw-cost table for the work-bound (L->R)
 * slots — the real logged L-column (overrides raw_lr). raw_rl: raw cost
 * of the pad-bound (R->L) slot. Returns LL-slip count. */
static uint32_t pl_run(CPUState* state, uint32_t display,
                       const uint32_t* worktab, uint32_t nwork,
                       uint32_t raw_lr, uint32_t raw_rl, uint32_t niter)
{
    uint64_t prev_delta = 0;
    uint32_t prev_pad = 0u;       /* no floor on the first interval */
    uint32_t prev_class = 1u;
    uint32_t wi = 0, ll = 0;
    uint64_t t = 0;
    uint32_t i;

    s_enabled = 1u; s_split_mode = 0u; s_first = 0u;
    s_acc = 0u; s_logic_this_frame = 1u; s_interp_alpha = 0.0f;
    s_last_delta = 0u; s_prev_frame_class = 1u;
    s_l_delta = 0u; s_pair_pad = 0u; s_overlap_active = 0u;
    s_render_hz = 60u;
    store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], display);
    store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u],
               (uint32_t)TICK_30FPS);
    store_be32(&s_memory[0x803F6160u - 0x80000000u], 0u);
    frame60_accum_decide(state, 0);          /* engage: arms, forces L */

    for (i = 0; i < niter; ++i) {
        uint32_t cls, pad, raw;
        uint64_t delta;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   (uint32_t)prev_delta);
        frame60_accum_decide(state, 0);
        cls = s_logic_this_frame;
        state->lr = JFW_BEGINRENDER_START + 0x40u;
        state->gpr[3] = (uint32_t)TICK_30FPS;
        state->gpr[4] = 7u;
        on_wait_for_tick(state);
        pad = state->gpr[3];
        if (prev_class && cls)       raw = 695000u;   /* post-slip L,L */
        else if (prev_class && !cls) raw = worktab ? worktab[wi++ % nwork]
                                                 : raw_lr; /* work slot */
        else if (cls)                raw = raw_rl;    /* pad-bound slot */
        else                         raw = 50000u;    /* rare R,R */
        delta = (uint64_t)raw > prev_pad ? (uint64_t)raw : prev_pad;
        t += delta;
        pl_cls[i] = (uint8_t)cls; pl_dlt[i] = (uint32_t)delta;
        pl_pad[i] = pad; pl_tim[i] = t;
        if (cls && prev_class) ++ll;
        prev_delta = delta; prev_pad = pad; prev_class = cls;
    }
    return ll;
}

/* VI-field accounting over one pl_run: fields that re-show the previous
 * copy (no new content) and copies that never become the newest at any
 * field start. */
static void pl_vi_stats(uint32_t niter, uint32_t* held_out,
                        uint32_t* dropped_out)
{
    const uint64_t F = F60_FIELD_TICKS;
    uint32_t held = 0, dropped = 0, i;
    uint64_t tN = pl_tim[niter - 1];
    uint32_t ci = 0;
    uint32_t prev_shown = 0xFFFFFFFFu;
    for (uint64_t f = F; f <= tN; f += F) {
        uint32_t shown = prev_shown;
        while (ci < niter && pl_tim[ci] <= f) { shown = ci; ++ci; }
        if (shown != 0xFFFFFFFFu && shown == prev_shown) ++held;
        if (shown != 0xFFFFFFFFu) prev_shown = shown;
    }
    for (i = 0; i + 1 < niter; ++i) {
        uint64_t b0 = ((pl_tim[i] + F - 1ull) / F) * F;  /* first b >= t_i */
        if (pl_tim[i + 1] <= b0) ++dropped;              /* superseded    */
    }
    *held_out = held; *dropped_out = dropped;
}

/* VI-lock simulator. Retraces sit on a 59.94 Hz grid (VI_FIELD_NTSC) and
 * preRetraceProc publishes sVideoLastTick VL_LAT ticks after each one;
 * waitForTick is modelled exactly (exit = max(nextTick, arrival), nextTick =
 * exit + pad) with nextTick/sVideoLastTick/sVideoInterval living in guest
 * memory, so vi_lock_pad sees what the game would. Each iteration's copy
 * lands at its wait exit; a copy is dropped when no preRetraceProc pick
 * falls between it and the next copy. */
#define VL_MAX 4096u
#define VL_LAT 1500ull
static uint64_t vl_exit[VL_MAX];
static uint8_t  vl_cls[VL_MAX];
static uint32_t vl_alpha_off;   /* R-after-L frames whose alpha != 0.5 */
static uint32_t vl_run(CPUState* state, uint32_t display, uint32_t wl,
                       uint32_t wr, uint32_t jitter, uint32_t niter,
                       uint32_t* ll_out, uint32_t* lcount_out)
{
    const uint64_t F = VI_FIELD_NTSC;
    const uint64_t r0 = 1000000ull + 12345ull;   /* arbitrary grid phase */
    uint64_t t = 1000000ull + 300000ull;         /* first Painter entry */
    uint64_t next_tick = 0, prev_exit = 0;
    uint32_t rng = 0x1234567u, i, ll = 0, lc = 0, drops = 0;

    s_enabled = 1u; s_split_mode = 0u; s_first = 0u;
    s_acc = 0u; s_logic_this_frame = 1u; s_interp_alpha = 0.0f;
    s_last_delta = 0u; s_prev_frame_class = 1u;
    s_l_delta = 0u; s_pair_pad = 0u; s_overlap_active = 0u;
    s_render_hz = 60u; s_vi_locked = 0u; vl_alpha_off = 0u;
    store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], display);
    store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u],
               (uint32_t)TICK_30FPS);
    store_be32(&s_memory[0x803F6160u - 0x80000000u], 0u);
    store_be32(&s_memory[JUTVIDEO_INTERVAL - 0x80000000u], (uint32_t)F);
    store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u], 0u);
    frame60_accum_decide(state, 0);             /* engage: arms, forces L */

    for (i = 0; i < niter && i < VL_MAX; ++i) {
        uint64_t a, exit_tb, k, last, w;
        uint32_t cls;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   (uint32_t)(i ? vl_exit[i - 1] - prev_exit : 0u));
        frame60_accum_decide(state, 0);
        cls = s_logic_this_frame;
        if (i > 50u && !cls && vl_cls[i - 1] && s_interp_alpha != 0.5f)
            ++vl_alpha_off;
        a = t;
        /* last published retrace at or before the waitForTick call */
        k = (a - r0) / F;
        last = r0 + k * F + VL_LAT;
        if (last > a) last -= F;
        store_be32(&s_memory[JUTVIDEO_LASTTICK - 0x80000000u], (uint32_t)last);
        store_be32(&s_memory[JFW_NEXTTICK - 0x80000000u], (uint32_t)(next_tick >> 32));
        store_be32(&s_memory[JFW_NEXTTICK + 4u - 0x80000000u], (uint32_t)next_tick);
        state->timebase = a;
        state->lr = JFW_BEGINRENDER_START + 0x40u;
        state->gpr[3] = (uint32_t)TICK_30FPS;
        state->gpr[4] = 7u;
        on_wait_for_tick(state);
        exit_tb = next_tick > a ? next_tick : a;
        next_tick = exit_tb + state->gpr[3];
        prev_exit = i ? vl_exit[i - 1] : exit_tb;
        vl_exit[i] = exit_tb; vl_cls[i] = (uint8_t)cls;
        if (cls) ++lc;
        if (i && cls && vl_cls[i - 1]) ++ll;
        rng = rng * 1103515245u + 12345u;
        w = (uint64_t)(cls ? wl : wr) + (jitter ? (rng >> 8) % jitter : 0u);
        t = exit_tb + w;
    }
    /* skip warm-up while the lock converges */
    for (i = 50u; i + 1 < niter && i + 1 < VL_MAX; ++i) {
        const uint64_t pick = ((vl_exit[i] - r0 - VL_LAT + F - 1ull) / F) * F + r0 + VL_LAT;
        if (pick >= vl_exit[i + 1]) ++drops;
    }
    *ll_out = ll; *lcount_out = lc;
    return drops;
}

int main(void)
{
    CPUState state;
    memset(&state, 0, sizeof(state));
    memset(s_memory, 0, sizeof(s_memory));
    state.external_read = test_read;
    state.external_write = test_write;
    /* Direct-RAM path (in_ram/rd*_fast/wr*_fast): s_memory is the MEM1
     * mirror at guest-phys offsets, so it can back the fast path too. */
    state.ram = s_memory;
    state.ram_size = sizeof(s_memory);

    const uint32_t display = 0x80000100u;
    store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], display);
    store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u], (uint32_t)TICK_30FPS);

    /* The cadence/pair-lock checks below pin the tick-derived pacing — the
     * VI lock's fallback when no retrace grid exists. VL covers the lock.
     * The headroom governor samples the host clock; GOV drives it with
     * synthetic time instead. */
    s_vi_lock = 0u;
    s_auto_degrade = 0u;
    s_enabled = 1u;
    s_first = 0u;
    s_acc = 0u;
    on_painter_skip(&state);
    if (!s_split_mode || !s_logic_this_frame)
        return 1;
    if (load_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u]) != TICK_30FPS)
        return 2;

    on_painter_skip(&state);
    if (!s_split_mode)
        return 3;

    s_render_hz = 200u;
    state.lr = JFW_BEGINRENDER_START + 0x40u;
    state.gpr[3] = (uint32_t)TICK_30FPS;
    state.gpr[4] = 7u;
    on_wait_for_tick(&state);
    if (state.gpr[3] != 202500u || state.gpr[4] != 0u)
        return 4;

    /* waitBlanking's call (lr outside beginRender) must keep scene timing. */
    state.lr = 0x80255CF0u;
    state.gpr[3] = (uint32_t)TICK_30FPS;
    state.gpr[4] = 7u;
    on_wait_for_tick(&state);
    if (state.gpr[3] != (uint32_t)TICK_30FPS || state.gpr[4] != 7u)
        return 10;

    store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u], 202500u);
    for (unsigned int i = 0; i < 6u; ++i)
    {
        on_painter_skip(&state);
        if (s_logic_this_frame)
            return 5;
    }
    on_painter_skip(&state);
    if (!s_logic_this_frame)
        return 6;

    s_logic_this_frame = 0u;
    state.pc = 0x80001000u;
    state.lr = 0x80002000u;
    on_true_logic_gate(&state);
    if (state.pc != state.lr || state.gpr[3] != 1u)
        return 7;

    /* F1: on a render-only frame the fader replays the preceding L-frame's
     * draw decision AND colour — it is not an unconditional draw() tail-call.
     * (a) L-frame's control() never reached draw() (e.g. WaitIn) -> R skips. */
    {
        const uint32_t fader = 0x80123400u;
        const uint32_t saved_rfr = s_rframe_render;
        s_fdr_l_drew = 0u;
        s_fdr_r_pending = 0u;
        s_logic_this_frame = 0u;
        state.gpr[3] = fader;
        state.pc = 0x802C85F0u;
        state.lr = 0x80255D00u;
        on_fader_gate(&state);
        if (state.pc != state.lr)
            return 40;
        /* (b) L-frame control() reached draw() with colour X -> the R-frame
         * redirects to draw() with X overriding the live (post-execute)
         * mColor; the next control() entry restores the live colour. */
        s_logic_this_frame = 1u;
        store_be32(&s_memory[fader + 0x0Cu - 0x80000000u], 0x11223344u);
        on_fader_draw_seen(&state);
        if (!s_fdr_l_drew || s_fdr_l_color != 0x11223344u)
            return 41;
        s_logic_this_frame = 0u;
        store_be32(&s_memory[fader + 0x0Cu - 0x80000000u], 0x000000FFu);
        state.pc = 0x802C85F0u;
        on_fader_gate(&state);
        if (state.pc != JUT_FADER_DRAW)
            return 42;
        if (load_be32(&s_memory[fader + 0x0Cu - 0x80000000u]) != 0x11223344u)
            return 43;
        if (!s_fdr_r_pending || s_fdr_r_saved != 0x000000FFu)
            return 44;
        s_logic_this_frame = 1u;
        state.pc = 0x802C85F0u;
        on_fader_gate(&state);
        if (s_fdr_r_pending)
            return 45;
        if (load_be32(&s_memory[fader + 0x0Cu - 0x80000000u]) != 0x000000FFu)
            return 46;
        s_logic_this_frame = 0u;
        /* F-1 sea counter: R-frame entry pre-decrements mAnimCounter
         * (+0x144) so the body's ++ reproduces the L value; the return hook
         * restores the original (also covering early-return paths). */
        {
            const uint32_t sea = 0x80130000u;
            store_be16(&s_memory[sea + 0x144u - 0x80000000u], 42u);
            s_sea_fix = 1u;
            s_r_dup = 0u;
            state.gpr[3] = sea;
            on_sea_draw_entry(&state);
            if (load_be16(&s_memory[sea + 0x144u - 0x80000000u]) != 41u)
                return 47;
            on_sea_draw_return(&state);
            if (load_be16(&s_memory[sea + 0x144u - 0x80000000u]) != 42u)
                return 48;
            /* kill switch off: the counter is untouched. */
            s_sea_fix = 0u;
            on_sea_draw_entry(&state);
            if (load_be16(&s_memory[sea + 0x144u - 0x80000000u]) != 42u || s_sea_snap)
                return 49;
            on_sea_draw_return(&state);
            s_sea_fix = 1u;
        }

        /* F-2 dKy_setLight: fix on -> the R-frame body runs (no pc=lr) and
         * the return hook restores the write set (RNG r0, lightMask);
         * fix off -> the pre-fix full skip. */
        {
            const uint32_t lst = 0x80132000u;
            store_be32(&s_memory[DKY_LST_PT - 0x80000000u], lst);
            store_be32(&s_memory[DKY_RND_STATE - 0x80000000u], 0xAAAABBBBu);
            store_be16(&s_memory[DKY_LIGHTMASK - 0x80000000u], 0x00C3u);
            s_light_fix = 1u;
            s_rframe_render = 1u;
            state.gpr[3] = 0x80300000u;
            state.pc = 0x80194BDCu;
            state.lr = 0x80255E00u;
            on_dky_setlight_gate(&state);
            if (state.pc == state.lr)
                return 50;
            if (!s_light_snap)
                return 51;
            /* emulate the body: RNG draw + recomputed mask */
            store_be32(&s_memory[DKY_RND_STATE - 0x80000000u], 0xDEADBEEFu);
            store_be16(&s_memory[DKY_LIGHTMASK - 0x80000000u], 0xFFFFu);
            on_dky_setlight_return(&state);
            if (load_be32(&s_memory[DKY_RND_STATE - 0x80000000u]) != 0xAAAABBBBu)
                return 52;
            if (load_be16(&s_memory[DKY_LIGHTMASK - 0x80000000u]) != 0x00C3u)
                return 53;
            s_light_fix = 0u;
            state.pc = 0x80194BDCu;
            on_dky_setlight_gate(&state);
            if (state.pc != state.lr)
                return 54;
            s_light_fix = 1u;
        }
        s_rframe_render = saved_rfr;
    }

    /* F2: calcFade entry snapshots the mutated statics; the return hook
     * restores them so the frame-timed advance never lands on R-frames. */
    s_memory[GINF_MFADE - 0x80000000u] = 0xA5u;
    store_be32(&s_memory[GINF_MFADERATE - 0x80000000u], 0x3F800000u);
    store_be32(&s_memory[GINF_MFADECOLOR - 0x80000000u], 0x11223344u);
    on_calcfade_entry(&state);
    if (!s_fade_snap)
        return 12;
    s_memory[GINF_MFADE - 0x80000000u] = 0u;
    store_be32(&s_memory[GINF_MFADERATE - 0x80000000u], 0x40000000u);
    store_be32(&s_memory[GINF_MFADECOLOR - 0x80000000u], 0x55667788u);
    on_calcfade_return(&state);
    if (s_fade_snap)
        return 13;
    if (s_memory[GINF_MFADE - 0x80000000u] != 0xA5u ||
        load_be32(&s_memory[GINF_MFADERATE - 0x80000000u]) != 0x3F800000u ||
        load_be32(&s_memory[GINF_MFADECOLOR - 0x80000000u]) != 0x11223344u)
        return 14;

    /* F3: calcWipe entry/return snapshot+restore (mWipe, mWipeRate,
     * mWipeDlst.mScrollS/T — the packet is enqueued by pointer so restoring
     * the statics also fixes what the list renders). */
    s_memory[DDLST_MWIPE - 0x80000000u] = 1u;
    store_be32(&s_memory[DDLST_MWIPERATE - 0x80000000u], 0x3F000000u);
    store_be32(&s_memory[DDLST_MWIPESCROLLS - 0x80000000u], 0x3E800000u);
    store_be32(&s_memory[DDLST_MWIPESCROLLT - 0x80000000u], 0x3EA00000u);
    on_calcwipe_entry(&state);
    if (!s_wipe_snap)
        return 15;
    store_be32(&s_memory[DDLST_MWIPERATE - 0x80000000u], 0x3F800000u);
    on_calcwipe_return(&state);
    if (s_wipe_snap)
        return 16;
    if (s_memory[DDLST_MWIPE - 0x80000000u] != 1u ||
        load_be32(&s_memory[DDLST_MWIPERATE - 0x80000000u]) != 0x3F000000u ||
        load_be32(&s_memory[DDLST_MWIPESCROLLS - 0x80000000u]) != 0x3E800000u ||
        load_be32(&s_memory[DDLST_MWIPESCROLLT - 0x80000000u]) != 0x3EA00000u)
        return 17;

    /* F3-cursor: on an R-frame the L-frame's &mWipeDlst entry is still in
     * the XLU list (dComIfGd_reset lives inside the gated fpcDw_Handler).
     * calcWipe's re-append is a duplicate — the return hook must rewind
     * mp2DXlu immediately so this pass's draw2DXlu draws the L-frame list
     * unchanged (1x overlay). A stale entry would blend the wipe twice on
     * every R-frame (30 Hz alpha blink). */
    store_be32(&s_memory[DDLST_MP2DXLU - 0x80000000u], 0x803CAA04u);
    on_calcwipe_entry(&state);
    if (!s_wipe_snap)
        return 21;
    /* emulate the duplicate append: *mp2DXlu = &wipe; mp2DXlu++ */
    store_be32(&s_memory[0x803CAA04u - 0x80000000u], 0x803E22F8u);
    store_be32(&s_memory[DDLST_MP2DXLU - 0x80000000u], 0x803CAA08u);
    on_calcwipe_return(&state);
    /* cursor rewound to entry value — duplicate dropped before draw2DXlu. */
    if (load_be32(&s_memory[DDLST_MP2DXLU - 0x80000000u]) != 0x803CAA04u)
        return 22;

    /* F2b: calcFade mutates mFadeRate before drawing — the entry hook
     * pre-decrements by mFadeSpeed so the R-frame replays the L-frame's
     * alpha. Entry: rate=0.40 speed=0.10 -> pre-dec 0.30; body would then
     * add 0.10 -> 0.40 == the L-frame's post-calc rate. */
    store_be32(&s_memory[GINF_MFADERATE - 0x80000000u], 0x3ECCCCCDu); /* 0.40f */
    store_be32(&s_memory[GINF_MFADESPEED - 0x80000000u], 0x3DCCCCCDu); /* 0.10f */
    s_memory[GINF_MFADE - 0x80000000u] = 1u;
    on_calcfade_entry(&state);
    {
        union { uint32_t u; float f; } rr;
        rr.u = load_be32(&s_memory[GINF_MFADERATE - 0x80000000u]);
        if (rr.f < 0.2999f || rr.f > 0.3001f)
            return 23;
    }
    /* mFade == 0: no pre-decrement (else-branch is already idempotent). */
    store_be32(&s_memory[GINF_MFADERATE - 0x80000000u], 0x3ECCCCCDu);
    s_memory[GINF_MFADE - 0x80000000u] = 0u;
    on_calcfade_entry(&state);
    if (load_be32(&s_memory[GINF_MFADERATE - 0x80000000u]) != 0x3ECCCCCDu)
        return 24;
    on_calcfade_return(&state);   /* restores the 0.40 snapshot */
    if (load_be32(&s_memory[GINF_MFADERATE - 0x80000000u]) != 0x3ECCCCCDu)
        return 25;

    /* F4: calcMonotone is a plain pc=lr gate on render-only frames. */
    state.pc = 0x80008354u;
    state.lr = 0x8000B000u;
    on_void_logic_gate(&state);
    if (state.pc != state.lr)
        return 18;

    /* On logic frames the split gates must not touch pc or memory. */
    s_logic_this_frame = 1u;
    state.pc = 0x802C85F0u;
    on_fader_gate(&state);
    if (state.pc != 0x802C85F0u)
        return 19;
    s_fade_snap = 0;
    on_calcfade_entry(&state);
    if (s_fade_snap)
        return 20;
    s_logic_this_frame = 0u;

    store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u], 675000u);
    on_painter_skip(&state);
    if (s_split_mode || !s_logic_this_frame || s_acc != 0u)
        return 8;
    state.gpr[3] = 675000u;
    state.gpr[4] = 3u;
    on_wait_for_tick(&state);
    if (state.gpr[3] != 675000u || state.gpr[4] != 3u)
        return 9;

    /* ==== read/write_mtx_arr element order ====
     * The u64 path byteswaps the whole word — without the lane rotate the
     * two f32s inside each u64 arrive swapped. Element order must be guest
     * order or absolute-index reads (the teleport check's m[r][3]) silently
     * see the wrong element. */
    {
        J3DMtx dst[2];
        const float* df = (const float*)dst;
        const uint32_t node_ea = 0x80120000u;             /* 8B-aligned -> u64 path */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 100.0f + (float)i);
        read_mtx_arr(&state, node_ea, dst, 2);
        for (uint32_t i = 0; i < 24u; ++i)
            if (df[i] != 100.0f + (float)i)
                return 30;
        /* write path: guest bytes get elements back in order */
        memset(&s_memory[node_ea - 0x80000000u], 0, 96);
        write_mtx_arr(&state, node_ea, dst, 2);
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 100.0f + (float)i)
                return 31;
        /* 4B-aligned fallback: same element order through the u32 path */
        read_mtx_arr(&state, node_ea + 4u, dst, 1);
        if (df[0] != 101.0f || df[11] != 112.0f)
            return 32;
    }

    /* ==== hist_rotate: seed / dirty / teleport ==== */
    {
        J3DMtx p[2], c[2], nm[2];
        uint32_t hp = 0, dr = 0, tp = 0;
        memset(nm, 0, sizeof(nm));
        hist_rotate(p, c, nm, 2, &hp, &dr, &tp);
        if (!hp || dr || tp)
            return 33;                                    /* first seed: prev=curr, clean */
        nm[0].m[0][3] = 10.0f;
        hist_rotate(p, c, nm, 2, &hp, &dr, &tp);
        if (!dr || tp)
            return 34;                                    /* small move: dirty, no teleport */
        nm[0].m[0][3] = 610.0f;
        hist_rotate(p, c, nm, 2, &hp, &dr, &tp);
        if (!tp || dr)
            return 35;                                    /* teleport: snap, clean */
        hist_rotate(p, c, nm, 2, &hp, &dr, &tp);
        if (tp || dr)
            return 36;                                    /* settles back */
    }

    /* ==== be_f32 / wr_be_f32 ==== */
    {
        uint8_t b[4];
        wr_be_f32(b, -12.5f);
        if (be_f32(b) != -12.5f)
            return 37;
    }

    /* ==== scan_xf_loads ==== */
    {
        uint8_t s[80];
        XfLoad xl[4];
        memset(s, 0, sizeof(s));
        s[0] = 0x61;                                       /* head opcode */
        s[5] = 0x10; s[6] = 0x00; s[7] = 0x0B;             /* cnt-1=11 -> cnt=12 */
        s[8] = 0x00; s[9] = 0x40;                          /* addr 0x40 < 0x800 */
        /* a plausible-looking 0x10 INSIDE the payload is never examined —
         * validated candidates step over their payload */
        s[20] = 0x10; s[21] = 0x00; s[22] = 0x02; s[23] = 0x00; s[24] = 0x50;
        s[58] = 0x61;                                      /* follower opcode */
        uint32_t n = scan_xf_loads(s, sizeof(s), xl, 4);
        if (n != 1u || xl[0].addr != 0x40u || xl[0].cnt != 12u || xl[0].off != 10u)
            return 38;
        /* cnt==1 rejected */
        uint8_t bad[16] = {0x10, 0x00, 0x00, 0x00, 0x40, 0, 0, 0, 0, 0x61, 0, 0, 0, 0, 0, 0};
        if (scan_xf_loads(bad, sizeof(bad), xl, 4) != 0u)
            return 39;
        /* addr >= 0x0800 (not matrix file) rejected */
        uint8_t bad2[16] = {0x10, 0x00, 0x01, 0x08, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x61, 0, 0};
        if (scan_xf_loads(bad2, sizeof(bad2), xl, 4) != 0u)
            return 40;
        /* invalid follower opcode rejected (0x41 = mid-command garbage) */
        uint8_t bad3[16] = {0x10, 0x00, 0x01, 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0, 0};
        if (scan_xf_loads(bad3, sizeof(bad3), xl, 4) != 0u)
            return 41;
    }

    /* ==== stream_patch_matrices: addr-matched lerp, NaN guard, verbatim copy ==== */
    {
        uint8_t prev[64], curr[64], out[64];
        memset(prev, 0, sizeof(prev));
        memset(curr, 0, sizeof(curr));
        prev[0] = curr[0] = 0x10;
        prev[1] = curr[1] = 0x00; prev[2] = curr[2] = 0x02;  /* cnt=3 */
        prev[3] = curr[3] = 0x00; prev[4] = curr[4] = 0x40;  /* addr 0x40 */
        wr_be_f32(prev + 5, 1.0f);  wr_be_f32(curr + 5, 5.0f);
        wr_be_f32(prev + 9, 2.0f);  wr_be_f32(curr + 9, 6.0f);
        wr_be_f32(prev + 13, 3.0f); wr_be_f32(curr + 13, 7.0f);
        prev[17] = curr[17] = 0x61;                          /* follower */
        curr[40] = 0x99;                                     /* non-payload byte: curr verbatim */
        uint32_t patched = stream_patch_matrices(prev, 64, curr, 64, out, 0.5f);
        if (patched != 3u)
            return 42;
        if (be_f32(out + 5) != 3.0f || be_f32(out + 9) != 4.0f || be_f32(out + 13) != 5.0f)
            return 43;
        if (out[40] != 0x99u)
            return 44;                                       /* non-matched bytes copy curr */
        /* NaN/garbage prev word -> that word copies curr verbatim */
        prev[5] = 0x7F; prev[6] = 0xC0; prev[7] = 0x00; prev[8] = 0x00;
        patched = stream_patch_matrices(prev, 64, curr, 64, out, 0.5f);
        if (patched != 2u || be_f32(out + 5) != 5.0f)
            return 45;
        /* huge prev word (not really a matrix) also verbatim */
        store_f32(prev + 9, 2.0e9f);
        patched = stream_patch_matrices(prev, 64, curr, 64, out, 0.5f);
        if (patched != 1u || be_f32(out + 9) != 6.0f)
            return 46;
    }

    /* ==== fifo_capture: circular wrap at end->base via bulk-read ==== */
    {
        const uint32_t base = 0x1000u, end = 0x2000u;
        for (uint32_t i = 0; i < 0x1000u; ++i)
            s_memory[base + i] = (uint8_t)(i * 7u);
        uint8_t dst[0x200];
        memset(dst, 0xAA, sizeof(dst));
        fifo_capture(&state, base, end, 0x1F80u, 0x100u, dst);
        for (uint32_t i = 0; i < 0x80u; ++i)
            if (dst[i] != (uint8_t)((0x1F80u - base + i) * 7u))
                return 47;
        for (uint32_t i = 0; i < 0x80u; ++i)
            if (dst[0x80u + i] != (uint8_t)(i * 7u))
                return 48;
    }

    /* ==== J3D live-inject end-to-end (ConcatView path) ====
     * register -> snapshot -> lerp inject -> restore -> teleport suppress.
     * Exercises read/write_mtx_arr through the shim's u64 modeling — the
     * exact path that used to swap f32 lanes. */
    {
        s_j3d_interp = 1u;
        const uint32_t mdl = 0x80100000u, modelData = 0x80110000u;
        const uint32_t node_ea = 0x80120000u, env_ea = 0x80121000u;
        store_be32(&s_memory[mdl + 0x00u - 0x80000000u], 0x8039EA50u); /* __vt__8J3DModel */
        store_be32(&s_memory[mdl + 0x04u - 0x80000000u], modelData);
        store_be32(&s_memory[mdl + 0xC0u - 0x80000000u], 0u);            /* no skin */
        store_be32(&s_memory[mdl + 0x8Cu - 0x80000000u], node_ea);
        store_be32(&s_memory[mdl + 0x90u - 0x80000000u], env_ea);
        store_be16(&s_memory[modelData + 0x28u - 0x80000000u], 2u);      /* jointNum */
        store_be16(&s_memory[modelData + 0x30u - 0x80000000u], 1u);      /* wEvlpNum */
        store_be32(&s_memory[modelData + 0x08u - 0x80000000u], 0x20u);   /* flags &0xF0 = ConcatView */
        store_be16(&s_memory[modelData + 0x44u - 0x80000000u], 2u);
        state.gpr[3] = mdl;
        on_j3d_calc_entry(&state);
        J3DHistory* h = j3d_find(mdl);
        if (!h || !h->used || h->flags_f0 != 0x20u || h->joint_num != 2u)
            return 49;
        /* pose A (node) + Ae (env) */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 1.0f + (float)i);
        for (uint32_t i = 0; i < 12u; ++i)
            store_f32(&s_memory[env_ea - 0x80000000u + i * 4u], 50.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (!h->has_prev || h->dirty || h->teleported)
            return 50;                                   /* first seed: clean */
        /* pose B = A + 2 everywhere */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 3.0f + (float)i);
        for (uint32_t i = 0; i < 12u; ++i)
            store_f32(&s_memory[env_ea - 0x80000000u + i * 4u], 52.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (!h->dirty || h->teleported)
            return 51;
        /* inject alpha=0.5 -> arrays hold (A+B)/2 = A+1 elementwise */
        j3d_rframe_inject(&state, 0.5f);
        if (!s_matrices_injected || !h->injected)
            return 52;
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 2.0f + (float)i)
                return 53;
        for (uint32_t i = 0; i < 12u; ++i)
            if (be_f32(&s_memory[env_ea - 0x80000000u + i * 4u]) != 51.0f + (float)i)
                return 54;
        /* R-frame refresh must NOT collapse the pair: the arrays still hold
         * OUR lerp (scratch match), so the 0x20 refresh branch treats them as
         * "not a new observation" and skips the rotate — history untouched.
         * (Re-observing our own output would rotate prev=curr and disarm
         * injection — the old dead-path bug, now guarded per-array.) */
        j3d_rframe_refresh(&state);
        if (!h->dirty || h->injected == 0u)
            return 70;
        /* L-frame with production skipping the model (arrays still hold our
         * lerp): snapshot's scratch-compare restores curr (pose B) and
         * observes B -> rotate sees no change -> dirty clears. */
        j3d_snapshot_pose(&state);
        if (h->injected)
            return 55;
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 3.0f + (float)i)
                return 56;
        /* Next L-frame produces pose C: snapshot observes it -> pair B->C. */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 9.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (!h->dirty || h->teleported)
            return 71;
        /* R-frame inject: arrays = lerp(B,C,0.5) = 6+i, flagged injected. */
        j3d_rframe_inject(&state, 0.5f);
        if (!h->injected)
            return 72;
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 6.0f + (float)i)
                return 73;
        /* Mid-cycle production rewrite (pose D over our lerp): the L-frame
         * snapshot must NOT restore (that would clobber the fresh pose with
         * stale curr) and must observe D directly. */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 20.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (h->injected)
            return 74;
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 20.0f + (float)i)
                return 75;
        if (!h->dirty || h->teleported)
            return 76;                                   /* observed D -> new pair */
        /* teleport: m[0][3] +600 -> teleported, inject suppressed (snap, no
         * smear). THIS is the lane-swap regression: without element order the
         * check reads m[0][2] and never fires. */
        store_f32(&s_memory[node_ea - 0x80000000u + 3u * 4u], 603.0f);
        j3d_snapshot_pose(&state);
        if (!h->teleported)
            return 57;
        j3d_rframe_inject(&state, 0.5f);
        if (be_f32(&s_memory[node_ea - 0x80000000u + 3u * 4u]) != 603.0f)
            return 58;
        /* env teleport propagates the same suppression via t -> teleported */
        h->teleported = 0u;
        store_f32(&s_memory[env_ea - 0x80000000u + 3u * 4u], 700.0f);
        j3d_snapshot_pose(&state);
        if (!h->teleported)
            return 59;
        /* resize: same model ptr, new joint count -> ALL history reset incl.
         * the draw buffers (stale has_prev_draw would lerp across layouts). */
        h->has_prev_draw = 1u;
        h->prev_draw = j3d_alloc_mtx(2);
        h = j3d_history_ensure(mdl, 3u, 0u, 0);
        if (!h || h->joint_num != 3u || h->has_prev_draw || h->prev_draw ||
            h->has_prev_env || h->has_prev)
            return 60;
        j3d_history_free(mdl);
        s_j3d_interp = 0u;
    }

    /* ==== ConcatView forward-interp: refresh must observe the pose the
     * L-frame's OWN production wrote AFTER its Painter (the real pipeline
     * order: Painter -> queues -> execute -> draw). node/env therefore change
     * between the L-snapshot and the next R-entry; the refresh must rotate
     * them in exactly like the draw-buffer path. The old code skipped 0x20
     * here, leaving the pair stuck on the last DISPLAYED poses — inject then
     * lerped a backward midpoint over the newly produced pose. ==== */
    {
        s_j3d_interp = 1u;
        const uint32_t mdl = 0x80140000u, modelData = 0x80150000u;
        const uint32_t node_ea = 0x80160000u, env_ea = 0x80161000u;
        store_be32(&s_memory[mdl + 0x00u - 0x80000000u], 0x8039EA50u); /* __vt__8J3DModel */
        store_be32(&s_memory[mdl + 0x04u - 0x80000000u], modelData);
        store_be32(&s_memory[mdl + 0xC0u - 0x80000000u], 0u);
        store_be32(&s_memory[mdl + 0x8Cu - 0x80000000u], node_ea);
        store_be32(&s_memory[mdl + 0x90u - 0x80000000u], env_ea);
        store_be16(&s_memory[modelData + 0x28u - 0x80000000u], 2u);      /* jointNum */
        store_be16(&s_memory[modelData + 0x30u - 0x80000000u], 1u);      /* wEvlpNum */
        store_be32(&s_memory[modelData + 0x08u - 0x80000000u], 0x20u);   /* ConcatView */
        store_be16(&s_memory[modelData + 0x44u - 0x80000000u], 2u);
        state.gpr[3] = mdl;
        on_j3d_calc_entry(&state);
        J3DHistory* h = j3d_find(mdl);
        if (!h || h->flags_f0 != 0x20u)
            return 81;
        /* pose A in arrays; L-entry snapshot seeds it (last DISPLAYED pose). */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 1.0f + (float)i);
        for (uint32_t i = 0; i < 12u; ++i)
            store_f32(&s_memory[env_ea - 0x80000000u + i * 4u], 50.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (!h->has_prev || h->dirty)
            return 82;
        /* The L-frame's production (AFTER its Painter) writes pose B. */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 3.0f + (float)i);
        for (uint32_t i = 0; i < 12u; ++i)
            store_f32(&s_memory[env_ea - 0x80000000u + i * 4u], 52.0f + (float)i);
        /* R-entry refresh must observe B and rotate the pair to (A,B). */
        j3d_rframe_refresh(&state);
        if (!h->dirty || !h->has_prev)
            return 83;
        if (((const float*)h->curr)[0] != 3.0f)
            return 84;
        j3d_rframe_inject(&state, 0.5f);
        if (!h->injected)
            return 85;
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 2.0f + (float)i)
                return 86;
        /* Consecutive R-frame: arrays still hold OUR lerp -> refresh skips
         * the rotate (scratch match) and inject re-lerps at the new alpha. */
        j3d_rframe_refresh(&state);
        j3d_rframe_inject(&state, 0.75f);
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 2.5f + (float)i)
                return 87;
        /* Next L-entry: scratch-compare restores curr (B) for the draw and
         * re-observes B -> pair collapses to (B,B), dirty clears. */
        j3d_snapshot_pose(&state);
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 3.0f + (float)i)
                return 88;
        if (h->dirty)
            return 89;
        /* Production writes C; the next R-frame lerps (B,C) forward. */
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 9.0f + (float)i);
        j3d_rframe_refresh(&state);
        if (!h->dirty)
            return 90;
        j3d_rframe_inject(&state, 0.5f);
        for (uint32_t i = 0; i < 24u; ++i)
            if (be_f32(&s_memory[node_ea - 0x80000000u + i * 4u]) != 6.0f + (float)i)
                return 91;
        j3d_history_free(mdl);
        s_j3d_interp = 0u;
    }

    /* ==== calc-entry layout re-derive: a slot reused for a model with a
     * different joint count must fully reset (stale joint_num turns the
     * node/env inject write into an out-of-bounds guest-heap write), and a
     * same-count modelData swap must invalidate the stale pose pair. ==== */
    {
        s_j3d_interp = 1u;
        const uint32_t mdl = 0x80180000u;
        const uint32_t modelDataA = 0x80190000u, modelDataB = 0x80191000u;
        const uint32_t node_ea = 0x801A0000u, env_ea = 0x801B0000u;
        store_be32(&s_memory[mdl + 0x00u - 0x80000000u], 0x8039EA50u); /* __vt__8J3DModel */
        store_be32(&s_memory[mdl + 0x04u - 0x80000000u], modelDataA);
        store_be32(&s_memory[mdl + 0xC0u - 0x80000000u], 0u);
        store_be32(&s_memory[mdl + 0x8Cu - 0x80000000u], node_ea);
        store_be32(&s_memory[mdl + 0x90u - 0x80000000u], env_ea);
        store_be16(&s_memory[modelDataA + 0x28u - 0x80000000u], 2u);
        store_be16(&s_memory[modelDataA + 0x30u - 0x80000000u], 1u);
        store_be32(&s_memory[modelDataA + 0x08u - 0x80000000u], 0x20u);
        store_be16(&s_memory[modelDataA + 0x44u - 0x80000000u], 2u);
        state.gpr[3] = mdl;
        on_j3d_calc_entry(&state);
        J3DHistory* h = j3d_find(mdl);
        if (!h || h->joint_num != 2u)
            return 92;
        for (uint32_t i = 0; i < 24u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 1.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (!h->has_prev)
            return 93;
        /* modelData swap with DIFFERENT joint count (address reuse): slot
         * must fully reset — joint_num follows the new data. */
        store_be32(&s_memory[mdl + 0x04u - 0x80000000u], modelDataB);
        store_be16(&s_memory[modelDataB + 0x28u - 0x80000000u], 1u);
        store_be16(&s_memory[modelDataB + 0x30u - 0x80000000u], 0u);
        store_be32(&s_memory[modelDataB + 0x08u - 0x80000000u], 0x20u);
        store_be16(&s_memory[modelDataB + 0x44u - 0x80000000u], 1u);
        on_j3d_calc_entry(&state);
        h = j3d_find(mdl);
        if (!h || h->joint_num != 1u || h->wEvlp_num != 0u ||
            h->has_prev || h->model_data != modelDataB)
            return 94;
        /* same-count modelData swap: layout kept, but the stale pose pair
         * must be invalidated so we don't lerp the old pose into it. Seed
         * the pair while the slot is still self-consistent — the merged
         * liveness check drops a slot whose mModelData word no longer
         * matches the registered data — then repoint the model. */
        store_be16(&s_memory[modelDataA + 0x28u - 0x80000000u], 1u);
        store_be16(&s_memory[modelDataA + 0x30u - 0x80000000u], 0u);
        for (uint32_t i = 0; i < 12u; ++i)
            store_f32(&s_memory[node_ea - 0x80000000u + i * 4u], 7.0f + (float)i);
        j3d_snapshot_pose(&state);
        if (!h->has_prev)
            return 95;
        store_be32(&s_memory[mdl + 0x04u - 0x80000000u], modelDataA);
        on_j3d_calc_entry(&state);
        h = j3d_find(mdl);
        if (!h || h->has_prev || h->dirty || h->model_data != modelDataA)
            return 96;
        j3d_history_free(mdl);
        s_j3d_interp = 0u;
    }

    /* ==== on_unload frees host-side state ==== */
    {
        s_j3d_interp = 1u;
        (void)j3d_history_ensure(0x801C0000u, 2u, 0u, 0);
        if (s_hist_used == 0u)
            return 97;
        frame60_accum_on_unload();
        if (s_hist_used != 0u)
            return 98;
        if (j3d_find(0x801C0000u) != 0)
            return 99;
        s_j3d_interp = 0u;
    }

    /* ==== fifo_replay_frame end-to-end ====
     * Delta capture -> boundary extraction -> owed-pool inject, all through
     * the fake bulk page. The adversarial boundary check exercises the skip
     * that used to underflow the carry memmove. */
    {
        s_obs_valid = 0; s_carry_len = 0; s_capture_gen = 0;
        s_owed_injects = 0; s_injects_in_pair = 0;
        s_stream_prev_len = s_stream_curr_len = 0;
        s_dbg_replays = 0; s_fifo_verify = 0; s_fifo_multi = 1u;
        s_fake_base = 0x1000u; s_fake_end = 0x3000u; s_fake_wptr = 0x1800u;
        s_inject_result = 1;
        /* fake frame = 0x400B: head 0x61, one XF matrix load at off 5 (cnt=12,
         * addr 0x40), follower at 58, NOP fill; boundary lands 0x3FB (+5=end) */
        memset(&s_memory[0x1800u], 0, 0x400u);
        s_memory[0x1800u] = 0x61u;
        s_memory[0x1805u] = 0x10u; s_memory[0x1806u] = 0x00; s_memory[0x1807u] = 0x0Bu;
        s_memory[0x1808u] = 0x00; s_memory[0x1809u] = 0x40u;
        for (uint32_t k = 0; k < 12u; ++k)
            store_f32(&s_memory[0x180Au + k * 4u], 100.0f + (float)k);
        s_memory[0x183Au] = 0x61u;                       /* follower for scan_xf */
        static const uint32_t bounds1[] = { 0x3FBu };
        s_scan_canned = bounds1; s_scan_canned_n = 1u;
        fifo_replay_frame(&state);                        /* arms obs point only */
        s_fake_wptr = 0x1C00u;                            /* game produced 0x400B */
        fifo_replay_frame(&state);
        if (s_stream_curr_len != 0x400u || s_owed_injects != 1u || s_capture_gen != 1u)
            return 61;
        /* second produced frame: same layout, payload = 200+k -> pair forms */
        memset(&s_memory[0x1C00u], 0, 0x400u);
        s_memory[0x1C00u] = 0x61u;
        s_memory[0x1C05u] = 0x10u; s_memory[0x1C06u] = 0x00; s_memory[0x1C07u] = 0x0Bu;
        s_memory[0x1C08u] = 0x00; s_memory[0x1C09u] = 0x40u;
        for (uint32_t k = 0; k < 12u; ++k)
            store_f32(&s_memory[0x1C0Au + k * 4u], 200.0f + (float)k);
        s_memory[0x1C3Au] = 0x61u;
        s_fake_wptr = 0x2000u;
        fifo_replay_frame(&state);
        if (s_capture_gen != 2u || s_stream_prev_len != 0x400u)
            return 62;
        /* one inject drained (multi=1) at alpha=0.25: patched stream of curr
         * lands at the fake wptr; payload = prev + .25*(curr-prev) = 125+k */
        if (s_dbg_replays != 1u || s_owed_injects != 1u)
            return 63;
        for (uint32_t k = 0; k < 12u; ++k)
            if (be_f32(&s_memory[0x2000u + 0x0Au + k * 4u]) != 125.0f + (float)k)
                return 64;
        if (s_memory[0x2000u] != 0x61u)
            return 65;                                   /* head = curr verbatim */
        if (s_fake_wptr != 0x2400u)
            return 66;                                   /* wptr advanced past inject */
        /* adversarial boundary: fend beyond carry -> skipped WITHOUT the old
         * fstart jump that underflowed (s_carry_len - fstart) into memmove */
        s_fake_wptr = 0x2440u;                             /* delta = 0x40 */
        static const uint32_t bad_bounds[] = { 0xFFFFFFF0u };
        s_scan_canned = bad_bounds; s_scan_canned_n = 1u;
        fifo_replay_frame(&state);
        if (s_carry_len != 0x40u)
            return 67;                                   /* nothing extracted/lost */
        /* remaining owed inject drains at alpha=0.5 -> 150+k at 0x2440 */
        if (s_dbg_replays != 2u || s_owed_injects != 0u)
            return 68;
        for (uint32_t k = 0; k < 12u; ++k)
            if (be_f32(&s_memory[0x2440u + 0x0Au + k * 4u]) != 150.0f + (float)k)
                return 69;
        /* inject refusal: len that can't fit the fifo -> mod-side pre-check
         * returns 0 without touching the fake fifo */
        if (fifo_inject(&state, s_stream_curr, 0x3000u) != 0)
            return 70;
        /* engine-side refusal (stat reads back 0) also returns 0 and leaves
         * the fifo untouched */
        s_force_inject_refuse = 1;
        if (fifo_inject(&state, s_stream_curr, 0x100u) != 0)
            return 71;
        s_force_inject_refuse = 0;
    }

    /* ==== RFRAME_RENDER without J3D_INTERP falls through to dup redirect ====
     * Without the s_j3d_interp co-gate the render branch returned early and
     * Painter re-ran an identical frame at full traversal cost. */
    {
        const uint32_t disp = 0x80000100u;
        s_enabled = 1u;
        s_split_mode = 0u; s_first = 0u; s_acc = 0u; s_logic_this_frame = 1u;
        s_rframe_render = 1u; s_j3d_interp = 0u; s_fifo_replay = 0u;
        s_painter_skip_off = 0u; s_matrices_injected = 0u;
        /* windowed scene: windowNum!=0 is the precondition for the R-frame
         * render branch (windowNum==0 scenes are steered to dup-present). */
        s_memory[GAMEINFO_WNUM - 0x80000000u] = 1u;
        store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], disp);
        store_be32(&s_memory[disp + JFW_OFF_TICKRATE - 0x80000000u],
                   (uint32_t)TICK_30FPS);
        store_be32(&s_memory[disp + JFW_OFF_TICKDELTA - 0x80000000u],
                   (uint32_t)(TICK_30FPS / 4u));
        on_painter_skip(&state);              /* engages split -> L-frame */
        if (!s_split_mode || !s_logic_this_frame)
            return 72;
        state.pc = 0x8000AF2Cu; state.gpr[3] = 0xDEADBEEFu;
        on_painter_skip(&state);              /* R-frame: interp off -> dup */
        if (s_logic_this_frame || state.pc != JFW_BEGIN_RENDER ||
            state.gpr[3] != s_jfw_display)
            return 73;
        /* interp enabled: render branch takes the early return (Painter runs) */
        s_j3d_interp = 1u;
        state.pc = 0x8000AF2Cu;
        on_painter_skip(&state);              /* still R-frame territory */
        if (!s_logic_this_frame && state.pc == JFW_BEGIN_RENDER)
            return 74;
        s_rframe_render = 0u; s_j3d_interp = 0u; s_split_mode = 0u;
        s_logic_this_frame = 1u; s_first = 0u; s_acc = 0u;
    }

    /* ==== L1/L2 libm fast path: sin/cos claim via f1 and pc=lr ==== */
    {
        s_libm_fast = 1u;
        state.pc = GCLIBM_SIN;
        state.lr = 0x80012340u;
        state.fpr[1] = 0.5;
        on_libm(&state);
        if (state.pc != 0x80012340u)
            return 75;
        if (fabs(state.fpr[1] - sin(0.5)) > 1e-12)
            return 76;
        state.pc = GCLIBM_COS;
        state.lr = 0x80012444u;
        state.fpr[1] = -0.25;
        on_libm(&state);
        if (state.pc != 0x80012444u)
            return 77;
        if (fabs(state.fpr[1] - cos(-0.25)) > 1e-12)
            return 78;
        /* unrelated pc must fall through untouched */
        state.pc = 0x80000000u;
        state.lr = 0x80012500u;
        state.fpr[1] = 1.0;
        on_libm(&state);
        if (state.pc != 0x80000000u || state.fpr[1] != 1.0)
            return 79;
        /* disabled gate falls through untouched */
        s_libm_fast = 0u;
        state.pc = GCLIBM_SIN;
        state.fpr[1] = 0.5;
        on_libm(&state);
        if (state.pc != GCLIBM_SIN || state.fpr[1] != 0.5)
            return 80;
        s_libm_fast = 1u;
    }

    /* ==== overlap -> cadence kept, R-frames dup ==========================
     * delta = TICK/4 -> pattern L R R R L R R R ... (acc ticks every 4th).
     * The overlap guard keeps cadence and steers R-frames to the dup
     * tail-call even in render+interp mode. */
    {
        const uint32_t disp = 0x80000100u;
        s_enabled = 1u; s_split_mode = 0u; s_first = 0u; s_acc = 0u;
        s_logic_this_frame = 1u;
        s_rframe_render = 1u; s_j3d_interp = 1u; s_fifo_replay = 0u;
        s_painter_skip_off = 0u; s_cam_interp = 1u;
        store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], disp);
        store_be32(&s_memory[disp + JFW_OFF_TICKRATE - 0x80000000u], (uint32_t)TICK_30FPS);
        store_be32(&s_memory[disp + JFW_OFF_TICKDELTA - 0x80000000u],
                   (uint32_t)(TICK_30FPS / 4u));
        /* overlap flag ON */
        store_be32(&s_memory[0x803F6160u - 0x80000000u], 1u);

        uint32_t l_n = 0, r_n = 0, dup_n = 0, rep_n = 0;
        for (int i = 0; i < 8; ++i)
        {
            state.pc = 0x8000AF2Cu; state.gpr[3] = 0xDEADBEEFu;
            on_painter_skip(&state);
            if (s_logic_this_frame) ++l_n;
            else {
                ++r_n;
                if (state.pc == JFW_BEGIN_RENDER && state.gpr[3] == disp) ++dup_n;
                else ++rep_n;
            }
        }
        /* cadence kept (l_n==2, r_n==6) and every R dups. */
        if (l_n != 2u || r_n != 6u)
            return 101;
        if (dup_n != 6u || rep_n != 0u)
            return 102;
        if (s_dbg_ovr_dup != 6u)
            return 103;
        if (!s_overlap_active)
            return 104;

        /* overlap clears -> R-frames go back to the repaint path (no dup). */
        store_be32(&s_memory[0x803F6160u - 0x80000000u], 0u);
        rep_n = 0; dup_n = 0;
        for (int i = 0; i < 8; ++i)
        {
            state.pc = 0x8000AF2Cu; state.gpr[3] = 0xDEADBEEFu;
            on_painter_skip(&state);
            if (!s_logic_this_frame)
            {
                if (state.pc == JFW_BEGIN_RENDER) ++dup_n; else ++rep_n;
            }
        }
        if (rep_n != 6u || dup_n != 0u)
            return 105;
        if (s_overlap_active)
            return 106;

        s_rframe_render = 0u; s_j3d_interp = 0u;
    }

    /* ==== camera view lerp install + restore =============================
     * near-identity views: rot block fixed, translation column drifts
     * (2,1,0.5) between A and B — a normal pan, NOT a cut. */
    {
        float va[12] = {1,0,0, 0, 0,1,0, 0, 0,0,1, 0};
        float vb[12] = {1,0,0, 2, 0,1,0, 1, 0,0,1, 0.5f};
        float mid[12] = {1,0,0, 1, 0,1,0, 0.5f, 0,0,1, 0.25f};
        float pa[16], pb[16], midp[16];
        for (int i = 0; i < 16; ++i)
        {
            pa[i] = 100.0f + (float)i; pb[i] = 200.0f + (float)i;
            midp[i] = 150.0f + (float)i;
        }
        store_be32(&s_memory[GAMEINFO_MCURRVIEW - 0x80000000u], VIEW_EA);
        put_view(va, pa);
        s_cam_view = 0; s_cam_injected = 0; s_cam_has_prev = 0; s_cam_cut = 0;

        camview_lframe(&state);                 /* obs A -> curr=A */
        if (!s_cam_has_prev || s_cam_cut)
            return 110;
        put_view(vb, pb);                       /* production writes B */
        camview_rframe(&state);                 /* obs B -> pair (A,B) */
        if (s_cam_cut)
            return 111;
        camview_install(&state, 0.5f);          /* field <- lerp(A,B,.5) */
        if (!s_cam_injected)
            return 112;
        if (!check_view(mid, midp))
            return 113;
        /* consecutive R: field==scratch -> refresh must NOT re-observe */
        camview_rframe(&state);
        camview_install(&state, 0.75f);         /* field <- lerp(A,B,.75) */
        {
            float q[12];
            for (int i = 0; i < 12; ++i)
                q[i] = va[i] + 0.75f * (vb[i] - va[i]);
            for (int i = 0; i < 12; ++i)
                if (be_f32(&s_memory[VIEW_EA + VIEW_OFF_VIEWMTX - 0x80000000u + i * 4u]) != q[i])
                    return 114;
        }
        /* L-entry: field still ours -> restore curr (B), observe B */
        camview_lframe(&state);
        if (s_cam_injected)
            return 115;
        if (!check_view(vb, pb))
            return 116;
        if (s_cam_cut)
            return 117;
    }

    /* ==== camera cut -> collapsed history ================================
     * A cut mid-production surfaces at the R-refresh: delta vs curr huge ->
     * s_cam_cut set, prev collapses to obs so a later lerp == endpoint. */
    {
        float vc[12], pc2[16];
        memcpy(pc2, &s_cam_curr_p, sizeof(pc2));
        memcpy(vc, &s_cam_curr, sizeof(vc));
        vc[3] += 900.0f; vc[7] += 900.0f;    /* +900 X/Y translation jump */
        put_view(vc, pc2);
        camview_rframe(&state);
        if (!s_cam_cut)
            return 120;
        /* install under collapsed history must write the endpoint itself */
        camview_install(&state, 0.5f);
        if (!check_view(vc, pc2))
            return 121;
        /* the on_painter_skip decision path consumes the flag */
        if (s_cam_injected) { camview_restore(&state, s_cam_view); s_cam_injected = 0; }
        s_cam_cut = 0;
        /* next normal observation lerps again — keep the rot block fixed,
         * drift only the translation column (a pan, not another cut) */
        float vd[12], pd[16], mid2[12], midp2[16];
        memcpy(vd, vc, sizeof(vd)); memcpy(mid2, vc, sizeof(mid2));
        vd[3] += 2.0f; vd[7] += 1.0f; vd[11] += 0.5f;
        mid2[3] += 1.0f; mid2[7] += 0.5f; mid2[11] += 0.25f;
        for (int i = 0; i < 16; ++i) { pd[i] = pc2[i]; midp2[i] = pc2[i]; }
        camview_lframe(&state);                 /* obs == curr (restored vc) */
        put_view(vd, pd);
        camview_rframe(&state);
        if (s_cam_cut)
            return 122;
        camview_install(&state, 0.5f);
        if (!check_view(mid2, midp2))
            return 123;
        camview_lframe(&state);                 /* restore for next block */
    }

    /* ==== dKy_setLight R-frame relight ===================================
     * fix on: the body runs on R-frames (pc untouched) while the return
     * hook rewinds the write set; fix off: the pre-fix full skip. */
    {
        s_enabled = 1u; s_split_mode = 1u; s_logic_this_frame = 0u;
        s_rframe_render = 1u;
        store_be32(&s_memory[DKY_LST_PT - 0x80000000u], 0x80132000u);
        state.pc = 0x80194BDCu; state.lr = 0x8000C000u;
        on_dky_setlight_gate(&state);
        if (state.pc != 0x80194BDCu || !s_light_snap)
            return 130;
        on_dky_setlight_return(&state);              /* unwind the snapshot */
        s_light_fix = 0u;
        state.pc = 0x80194BDCu;
        on_dky_setlight_gate(&state);
        if (state.pc != state.lr)
            return 131;
        s_light_fix = 1u;
        s_logic_this_frame = 1u;                     /* L-frame: inert */
        state.pc = 0x80194BDCu;
        on_dky_setlight_gate(&state);
        if (state.pc != 0x80194BDCu)
            return 132;
        s_logic_this_frame = 0u; s_rframe_render = 0u;   /* dup mode -> inert */
        state.pc = 0x80194BDCu;
        on_dky_setlight_gate(&state);
        if (state.pc != 0x80194BDCu)
            return 133;
        s_rframe_render = 1u;
    }

    /* ==== purge-epoch invalidation =======================================
     * Slot seeded with a pose; an unrelated slot drop bumps s_purge_epoch;
     * next ensure() on the SAME addr+counts must invalidate has_prev. */
    {
        s_j3d_interp = 1u;
        const uint32_t mdl = 0x801E0000u, other = 0x801F0000u;
        J3DMtx M[2]; memset(M, 0, sizeof(M));
        M[0].m[0][0] = 1.0f; M[0].m[1][1] = 1.0f; M[0].m[2][2] = 1.0f;
        M[1] = M[0];
        J3DHistory* h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h)
            return 140;
        j3d_history_rotate(mdl, M, 2u);
        if (!h->has_prev)
            return 141;
        /* no purge in between: ensure keeps the pair */
        h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h->has_prev)
            return 142;
        /* drop an unrelated slot -> epoch bump -> same-addr ensure invalidates */
        (void)j3d_history_ensure(other, 2u, 0u, 0);
        j3d_history_free(other);
        h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h || h->has_prev || h->has_prev_draw || h->has_prev_env)
            return 143;
        j3d_history_free(mdl);
        s_j3d_interp = 0u;
    }

    /* ==== purge_range epoch rule =========================================
     * gInf freeAll runs every iteration — the epoch must bump only when a
     * tracked slot actually drops (j3d_slot_reset), or every ensure() sees
     * seen_epoch != epoch and the interp never engages. */
    {
        s_j3d_interp = 1u;
        const uint32_t mdl = 0x801E0000u;
        J3DMtx M[2]; memset(M, 0, sizeof(M));
        M[0].m[0][0] = 1.0f; M[0].m[1][1] = 1.0f; M[0].m[2][2] = 1.0f;
        M[1] = M[0];
        J3DHistory* h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h)
            return 160;
        j3d_history_rotate(mdl, M, 2u);
        if (!h->has_prev)
            return 161;
        const uint32_t epoch0 = s_purge_epoch;
        /* range covering no tracked pointer: no drop, no bump, pair kept */
        j3d_purge_range(&state, 0x80500000u, 0x80501000u);
        if (s_purge_epoch != epoch0)
            return 162;
        h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h || !h->has_prev)
            return 163;
        /* range covering guest_model_ptr: slot reset AND epoch bumped */
        j3d_purge_range(&state, mdl, mdl + 0x100u);
        if (s_purge_epoch == epoch0 || h->used)
            return 164;
        s_j3d_interp = 0u;
    }

    /* ==== j3d_restore_injected (camera-cut cleanup) ======================
     * A ConcatView slot whose node array holds OUR lerp gets curr back;
     * a foreign-touched array is left alone. */
    {
        s_j3d_interp = 1u; s_model_alive = 0u;   /* fake model: no vtable */
        const uint32_t mdl = 0x80200000u, node = 0x80210000u;
        J3DHistory* h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h || !h->curr || !h->scratch)
            return 150;
        h->model_data = 0x80220000u;           /* nonzero: configured */
        h->flags_f0 = 0x20u;                   /* ConcatView -> node/env path */
        h->node_ptr = node;
        /* curr = C, scratch/live = S (our lerp bits) */
        for (uint32_t j = 0; j < 2u; ++j)
        {
            memset(&h->curr[j], 0, sizeof(J3DMtx));
            h->curr[j].m[0][0] = 7.0f + (float)j;
            memset(&h->scratch[j], 0, sizeof(J3DMtx));
            h->scratch[j].m[0][0] = 4.0f + (float)j;
        }
        write_mtx_arr(&state, node, h->scratch, 2u);
        h->injected = 1u; s_matrices_injected = 1u;
        j3d_restore_injected(&state);
        if (h->injected)
            return 151;
        if (s_matrices_injected)
            return 152;
        {
            J3DMtx got[2];
            read_mtx_arr(&state, node, got, 2u);
            if (got[0].m[0][0] != 7.0f || got[1].m[0][0] != 8.0f)
                return 153;
        }
        /* foreign writer after our inject: restore must NOT clobber it */
        h->injected = 1u; s_matrices_injected = 1u;
        {
            J3DMtx foreign[2]; memset(foreign, 0, sizeof(foreign));
            foreign[0].m[0][0] = 99.0f;
            write_mtx_arr(&state, node, foreign, 2u);
        }
        j3d_restore_injected(&state);
        {
            J3DMtx got[2];
            read_mtx_arr(&state, node, got, 2u);
            if (got[0].m[0][0] != 99.0f)
                return 154;
        }
        j3d_history_free(mdl);
        s_j3d_interp = 0u;
        s_model_alive = 1u;
    }

    /* ==== D-pairlock (judder): L+R pair phase-lock ====================== */
    {
        /* Real work-bound slot costs — the L-column of
         * batch2/P3-outset/deltas.txt (post-warmup rows), replayed
         * verbatim as the (L->R) interval work. */
        static const uint32_t real_ws[] = {
            691109u, 687864u, 681426u, 683999u, 683999u, 678852u,
            688031u, 687273u, 679271u, 683999u, 687859u, 685202u,
            680139u, 685331u, 689033u, 685285u, 685286u, 685453u,
            688972u, 686582u, 681426u, 690433u, 687861u, 690743u,
            684075u, 691658u, 688064u, 693182u, 688968u, 689150u,
            689148u, 694295u, 689150u, 690435u, 695582u, 685519u,
            693655u,
        };
        const uint32_t nws = (uint32_t)(sizeof(real_ws) / sizeof(real_ws[0]));
        uint32_t ll0, ll1, held0, drop0, held1, drop1, i;

        /* PL0: unit-level pad math — inject preconditions, let the real
         * accumulator emit the class. field_0x34=100K keeps acc under the
         * slot so decide() emits R. */
        s_enabled = 1u; s_split_mode = 1u; s_first = 1u;
        s_acc = 0u; s_pair_lock = 1u; s_overlap_active = 0u;
        s_render_hz = 60u;
        store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], display);
        store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u],
                   (uint32_t)TICK_30FPS);
        store_be32(&s_memory[0x803F6160u - 0x80000000u], 0u);

        s_logic_this_frame = 1u; s_l_delta = 692000ull;   /* prev L, d_L */
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        if (s_logic_this_frame) return 210;              /* R emitted */
        if (s_pair_pad != 658000u) return 211;           /* 1350K-692K  */

        /* heavy L: clamps to MIN_PAD — never negative, never too short */
        s_logic_this_frame = 1u; s_l_delta = 1200000ull; s_acc = 0u;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        if (s_pair_pad != (uint32_t)F60_PAIR_MIN_PAD) return 212;
        s_logic_this_frame = 1u; s_l_delta = 1400000ull; s_acc = 0u;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        if (s_pair_pad != (uint32_t)F60_PAIR_MIN_PAD) return 213;

        /* d_L <= one field: pair-lock stays off — byte-identical pad */
        s_logic_this_frame = 1u; s_l_delta = 675000ull; s_acc = 0u;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        if (s_pair_pad != 0u) return 214;
        s_logic_this_frame = 1u; s_l_delta = 670000ull; s_acc = 0u;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        if (s_pair_pad != 0u) return 215;
        /* prev not L (R,R): no arm */
        s_logic_this_frame = 0u; s_l_delta = 692000ull; s_acc = 0u;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        if (s_pair_pad != 0u) return 216;

        /* armed pad reaches waitForTick only on the 675K field path */
        s_logic_this_frame = 1u; s_l_delta = 692000ull; s_acc = 0u;
        store_be32(&s_memory[display + JFW_OFF_TICKDELTA - 0x80000000u],
                   100000u);
        frame60_accum_decide(&state, 0);
        state.lr = JFW_BEGINRENDER_START + 0x40u;
        state.gpr[3] = (uint32_t)TICK_30FPS; state.gpr[4] = 7u;
        on_wait_for_tick(&state);
        if (state.gpr[3] != 658000u || state.gpr[4] != 0u) return 217;
        s_render_hz = 200u;                              /* other rates */
        state.gpr[3] = (uint32_t)TICK_30FPS;
        on_wait_for_tick(&state);
        if (state.gpr[3] != 202500u) return 218;         /* untouched */
        s_render_hz = 60u;
        s_pair_pad = 0u; s_l_delta = 0u; s_prev_frame_class = 1u;

        /* PL1: baseline defect replay — fix OFF, real jitter */
        s_pair_lock = 0u;
        ll0 = pl_run(&state, display, real_ws, nws, 692000u, 620000u, 2000u);
        if (ll0 == 0u) return 220;             /* defect must reproduce */
        pl_vi_stats(2000u, &held0, &drop0);
        /* copies arrive ~683K apart > the 675K field — the drift symptom
         * is held (no-new-copy) fields; copies are never superseded here */
        if (held0 == 0u) return 221;

        /* PL2: fix ON, same real jitter — locked */
        s_pair_lock = 1u;
        ll1 = pl_run(&state, display, real_ws, nws, 692000u, 620000u, 3000u);
        if (ll1 != 0u) return 222;
        pl_vi_stats(3000u, &held1, &drop1);
        if (held1 > 4u || drop1 > 4u) return 223;   /* warmup-scale only */
        /* R pads = 1350K - d_L (d_L = work slot ending at iter i-2);
         * L pads stay the full field. */
        for (i = 4; i < 3000u; ++i) {
            uint32_t want;
            if (pl_cls[i]) {
                if (pl_pad[i] != (uint32_t)F60_FIELD_TICKS) return 224;
                continue;
            }
            if (!pl_cls[i - 1]) continue;      /* only L->R arms */
            want = (uint32_t)(TICK_30FPS - pl_dlt[i - 2]);
            if (want < (uint32_t)F60_PAIR_MIN_PAD)
                want = (uint32_t)F60_PAIR_MIN_PAD;
            if (pl_pad[i] != want) return 225;
        }

        /* PL3: synthetic steady work — exact 1.35M pair sums, strict LRLR */
        ll1 = pl_run(&state, display, NULL, 0u, 692000u, 620000u, 3000u);
        if (ll1 != 0u) return 226;
        for (i = 8; i + 1 < 3000u; ++i)
            if ((uint64_t)pl_dlt[i] + pl_dlt[i + 1] != TICK_30FPS)
                return 227;
        for (i = 8; i < 3000u; ++i)
            if (pl_cls[i] == pl_cls[i - 1]) return 228;   /* no LL/RR */

        /* PL4: d_L <= field => byte-identical to fix-off */
        {
            static uint8_t  c0[PL_MAXN], c1[PL_MAXN];
            static uint32_t p0[PL_MAXN], p1_[PL_MAXN], d0[PL_MAXN], d1_[PL_MAXN];
            s_pair_lock = 0u;
            pl_run(&state, display, NULL, 0u, 675000u, 620000u, 2000u);
            memcpy(c0, pl_cls, 2000u); memcpy(p0, pl_pad, sizeof(p0));
            memcpy(d0, pl_dlt, sizeof(d0));
            s_pair_lock = 1u;
            pl_run(&state, display, NULL, 0u, 675000u, 620000u, 2000u);
            memcpy(c1, pl_cls, 2000u); memcpy(p1_, pl_pad, sizeof(p1_));
            memcpy(d1_, pl_dlt, sizeof(d1_));
            if (memcmp(c0, c1, 2000u) || memcmp(p0, p1_, sizeof(p0)) ||
                memcmp(d0, d1_, sizeof(d0)))
                return 229;
        }

        /* PL5: pad-bound slot work exceeding the pair pad degrades
         * gracefully — interval runs over, no clamp explosion */
        ll1 = pl_run(&state, display, NULL, 0u, 692000u, 700000u, 400u);
        for (i = 4; i < 400u; ++i)
            if (pl_pad[i] < (uint32_t)F60_PAIR_MIN_PAD) return 230;
        for (i = 4; i < 400u; ++i)
            if (pl_cls[i] && !pl_cls[i - 1] &&
                pl_dlt[i] != 700000u) return 231;

        /* PL6: env kill switch — one-way like the other F60_* fixes */
        {
            uint32_t save = s_pair_lock;
#if defined(_WIN32)
            _putenv("MODERNGEKKO_F60_PAIR_LOCK=0");
#else
            setenv("MODERNGEKKO_F60_PAIR_LOCK", "0", 1);
#endif
            frame60_accum_on_load(0);
            if (s_pair_lock != 0u) return 232;
#if defined(_WIN32)
            _putenv("MODERNGEKKO_F60_PAIR_LOCK=");
#else
            unsetenv("MODERNGEKKO_F60_PAIR_LOCK");
#endif
            s_pair_lock = save;
        }

        s_pair_pad = 0u; s_l_delta = 0u; s_prev_frame_class = 1u;
    }

    /* ================= D4 foliage packet lerp =============================
     * Fake dGrass_packet_c: element array inside the packet at this+0x14,
     * stride 0x44, matrix at elem+0x10. Drives the real entry/return hooks
     * (r3 = packet this). */
    {
        const uint32_t pkt = 0x80080000u;
        const uint32_t base = pkt + 0x14u;
        float f;
        int i;
        s_enabled = 1u; s_split_mode = 1u; s_foliage_fix = 1u;
        s_rframe_render = 1u; s_j3d_interp = 1u;
        s_r_dup = 0u; s_noinject = 0u; s_fol_cut = 0u;
        fol_reset(&s_fol_grass);
        state.gpr[3] = pkt;

        /* elem0: valid (mState=2, no clip), anim 3, item 1 */
        s_memory[base + 0x00u - 0x80000000u] = 2u;
        s_memory[base + 0x01u - 0x80000000u] = 0u;
        s_memory[base + 0x02u - 0x80000000u] = 3u;
        s_memory[base + 0x03u - 0x80000000u] = 1u;
        for (i = 0; i < 12; ++i)
            store_f32(&s_memory[base + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      10.0f + (float)i);
        /* elem1: valid now, will be clipped between snapshots */
        s_memory[base + 0x44u + 0x00u - 0x80000000u] = 2u;
        s_memory[base + 0x44u + 0x01u - 0x80000000u] = 0u;
        s_memory[base + 0x44u + 0x02u - 0x80000000u] = 3u;
        s_memory[base + 0x44u + 0x03u - 0x80000000u] = 1u;
        for (i = 0; i < 12; ++i)
            store_f32(&s_memory[base + 0x44u + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      100.0f + (float)i);
        /* elem2: dead (mState==0) */
        for (i = 0; i < 12; ++i)
            store_f32(&s_memory[base + 0x88u + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      200.0f + (float)i);
        /* elem3: valid, will change anim tag between snapshots */
        s_memory[base + 0xCCu + 0x00u - 0x80000000u] = 2u;
        s_memory[base + 0xCCu + 0x01u - 0x80000000u] = 0u;
        s_memory[base + 0xCCu + 0x02u - 0x80000000u] = 3u;
        s_memory[base + 0xCCu + 0x03u - 0x80000000u] = 1u;
        for (i = 0; i < 12; ++i)
            store_f32(&s_memory[base + 0xCCu + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      300.0f + (float)i);

        /* L draw entry: snapshot pose A */
        s_logic_this_frame = 1u;
        on_grass_draw_entry(&state);
        if (!s_fol_grass.ok_curr[0] || !s_fol_grass.ok_curr[1] ||
            s_fol_grass.ok_curr[2] || !s_fol_grass.ok_curr[3])
            return 240;

        /* production writes pose B into every element */
        for (i = 0; i < 12; ++i) {
            store_f32(&s_memory[base + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      20.0f + (float)i);
            store_f32(&s_memory[base + 0x44u + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      110.0f + (float)i);
            store_f32(&s_memory[base + 0x88u + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      210.0f + (float)i);
            store_f32(&s_memory[base + 0xCCu + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      310.0f + (float)i);
        }
        /* elem1 got clipped, elem2 revived, elem3's owner changed */
        s_memory[base + 0x44u + 0x01u - 0x80000000u] = 2u;   /* mInitFlags|=0x02 */
        s_memory[base + 0x88u + 0x00u - 0x80000000u] = 2u;   /* mState: alive    */
        s_memory[base + 0xCCu + 0x02u - 0x80000000u] = 4u;   /* mAnimIdx changed */

        /* R draw entry: lerp valid-in-both elements only */
        s_logic_this_frame = 0u;
        s_interp_alpha = 0.5f;
        on_grass_draw_entry(&state);
        /* elem0: lerp(10,20,0.5) = 15 on every matrix element */
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0x10u - 0x80000000u])}, 4u);
        if (f != 15.0f) return 241;
        /* elem1 clipped -> live pose untouched (110) */
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0x44u + 0x10u - 0x80000000u])}, 4u);
        if (f != 110.0f) return 242;
        /* elem2 dead at L -> untouched (210) */
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0x88u + 0x10u - 0x80000000u])}, 4u);
        if (f != 210.0f) return 243;
        /* elem3 tag mismatch -> untouched (310) */
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0xCCu + 0x10u - 0x80000000u])}, 4u);
        if (f != 310.0f) return 244;
        if (!s_fol_grass.inj[0] || s_fol_grass.inj[1] ||
            s_fol_grass.inj[2] || s_fol_grass.inj[3])
            return 245;

        /* return hook restores the live (curr) pose */
        on_grass_draw_return(&state);
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0x10u - 0x80000000u])}, 4u);
        if (f != 20.0f) return 246;
        if (s_fol_grass.armed) return 247;

        /* camera-cut R-frame: no lerp at all */
        fol_reset(&s_fol_grass);
        s_logic_this_frame = 1u;
        s_memory[base + 0x44u + 0x01u - 0x80000000u] = 0u;   /* elem1 valid again */
        on_grass_draw_entry(&state);                        /* snapshot B        */
        for (i = 0; i < 12; ++i)
            store_f32(&s_memory[base + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                      30.0f + (float)i);                    /* pose C            */
        s_logic_this_frame = 0u;
        s_fol_cut = 1u;
        on_grass_draw_entry(&state);
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0x10u - 0x80000000u])}, 4u);
        if (f != 30.0f) return 248;                         /* plain repaint     */
        if (s_fol_grass.armed) return 249;
        s_fol_cut = 0u;

        /* alpha edge: 0.0 disables the lerp */
        fol_reset(&s_fol_grass);
        s_logic_this_frame = 1u;
        on_grass_draw_entry(&state);
        s_logic_this_frame = 0u;
        s_interp_alpha = 0.0f;
        on_grass_draw_entry(&state);
        memcpy(&f, &(uint32_t){load_be32(&s_memory[base + 0x10u - 0x80000000u])}, 4u);
        if (f != 30.0f) return 250;
        s_interp_alpha = 0.5f;

        /* flower packet: same flow through its own history */
        {
            const uint32_t fpkt = 0x80100000u;
            const uint32_t fbase = fpkt + 0x14u;
            fol_reset(&s_fol_flower);
            state.gpr[3] = fpkt;
            s_memory[fbase + 0x00u - 0x80000000u] = 2u;   /* alive, unclipped */
            s_memory[fbase + 0x01u - 0x80000000u] = 7u;
            s_memory[fbase + 0x02u - 0x80000000u] = 9u;
            for (i = 0; i < 12; ++i)
                store_f32(&s_memory[fbase + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                          40.0f + (float)i);
            s_logic_this_frame = 1u;
            on_flower_draw_entry(&state);
            for (i = 0; i < 12; ++i)
                store_f32(&s_memory[fbase + 0x10u + (uint32_t)i * 4u - 0x80000000u],
                          50.0f + (float)i);
            s_logic_this_frame = 0u;
            on_flower_draw_entry(&state);
            memcpy(&f, &(uint32_t){load_be32(&s_memory[fbase + 0x10u - 0x80000000u])}, 4u);
            if (f != 45.0f) return 251;
            on_flower_draw_return(&state);
            memcpy(&f, &(uint32_t){load_be32(&s_memory[fbase + 0x10u - 0x80000000u])}, 4u);
            if (f != 50.0f) return 252;
        }

        /* tree packet: all three matrices (+0x10/+0x40/+0xD0) lerp+restore */
        {
            const uint32_t tpkt = 0x80180000u;
            const uint32_t tbase = tpkt + 0x14u;
            fol_reset(&s_fol_tree);
            state.gpr[3] = tpkt;
            s_memory[tbase + 0x00u - 0x80000000u] = 2u;   /* alive, unclipped */
            s_memory[tbase + 0x01u - 0x80000000u] = 5u;   /* shadow idx       */
            s_memory[tbase + 0x02u - 0x80000000u] = 6u;   /* anim idx         */
            for (i = 0; i < 12; ++i) {
                store_f32(&s_memory[tbase + 0x10u + (uint32_t)i * 4u - 0x80000000u], 60.0f + (float)i);
                store_f32(&s_memory[tbase + 0x40u + (uint32_t)i * 4u - 0x80000000u], 80.0f + (float)i);
                store_f32(&s_memory[tbase + 0xD0u + (uint32_t)i * 4u - 0x80000000u], 90.0f + (float)i);
            }
            s_logic_this_frame = 1u;
            on_tree_draw_entry(&state);
            for (i = 0; i < 12; ++i) {
                store_f32(&s_memory[tbase + 0x10u + (uint32_t)i * 4u - 0x80000000u], 70.0f + (float)i);
                store_f32(&s_memory[tbase + 0x40u + (uint32_t)i * 4u - 0x80000000u], 82.0f + (float)i);
                store_f32(&s_memory[tbase + 0xD0u + (uint32_t)i * 4u - 0x80000000u], 92.0f + (float)i);
            }
            s_logic_this_frame = 0u;
            on_tree_draw_entry(&state);
            memcpy(&f, &(uint32_t){load_be32(&s_memory[tbase + 0x10u - 0x80000000u])}, 4u);
            if (f != 65.0f) return 253;
            memcpy(&f, &(uint32_t){load_be32(&s_memory[tbase + 0x40u - 0x80000000u])}, 4u);
            if (f != 81.0f) return 254;
            memcpy(&f, &(uint32_t){load_be32(&s_memory[tbase + 0xD0u - 0x80000000u])}, 4u);
            if (f != 91.0f) return 255;
            on_tree_draw_return(&state);
            memcpy(&f, &(uint32_t){load_be32(&s_memory[tbase + 0xD0u - 0x80000000u])}, 4u);
            if (f != 92.0f) return 256;
        }
        s_logic_this_frame = 1u;
    }

    /* ================= D4 JPA particle lerp ===============================
     * Fake JPAEmitterManager: mEmtrGroup[16] @+0x50 (JSUList h/t/len), one
     * emitter linked via mLink @+0x90 (mNext @+0x0C), particle lists at
     * +0x17C/+0x188; particle link @+0 with mNext @+0x0C, mGlobalPosition
     * @+0x28, mCurFrame @+0x78. */
    {
        const uint32_t mgr  = 0x80300000u;
        const uint32_t emtr = 0x80310000u;
        const uint32_t p0   = 0x80320000u;
        const uint32_t p1   = 0x80320200u;
        const uint32_t p2   = 0x80320400u;
        const uint32_t grp  = mgr + 0x50u + 3u * 0x0Cu;   /* group 3 list */
        float pos[3];
        int k;
        s_jpa_fix = 1u;
        memset(s_jpa, 0, sizeof(s_jpa));

        store_be32(&s_memory[JPA_MGR_EA - 0x80000000u], mgr);
        /* group 3: one emitter; all other groups empty (len 0, head 0) */
        store_be32(&s_memory[grp + 0x00u - 0x80000000u], emtr + JPA_EMTR_LINK);
        store_be32(&s_memory[grp + 0x04u - 0x80000000u], emtr + JPA_EMTR_LINK);
        store_be32(&s_memory[grp + 0x08u - 0x80000000u], 1u);
        /* emitter link + particle lists */
        store_be32(&s_memory[emtr + JPA_EMTR_LINK + 0x0Cu - 0x80000000u], 0u); /* mNext=0 */
        store_be32(&s_memory[emtr + JPA_EMTR_ACT + 0x00u - 0x80000000u], p0);
        store_be32(&s_memory[emtr + JPA_EMTR_ACT + 0x04u - 0x80000000u], p1);
        store_be32(&s_memory[emtr + JPA_EMTR_ACT + 0x08u - 0x80000000u], 2u);
        /* child list empty */
        /* particle links (link is member 0): p0->p1, p1->0 */
        store_be32(&s_memory[p0 + JPA_LINK_NEXT - 0x80000000u], p1);
        store_be32(&s_memory[p1 + JPA_LINK_NEXT - 0x80000000u], 0u);
        /* frames + positions */
        store_f32(&s_memory[p0 + JPA_PTCL_FRAME - 0x80000000u], 5.0f);
        store_f32(&s_memory[p1 + JPA_PTCL_FRAME - 0x80000000u], 10.0f);
        for (k = 0; k < 3; ++k) {
            store_f32(&s_memory[p0 + JPA_PTCL_GLOBAL + (uint32_t)k * 4u - 0x80000000u], 1.0f + (float)k);
            store_f32(&s_memory[p1 + JPA_PTCL_GLOBAL + (uint32_t)k * 4u - 0x80000000u], 4.0f + (float)k);
        }

        /* L-frame walk: seeds the table with the drawn positions */
        jpa_lframe(&state);
        /* calc tick: both particles advance one frame and move */
        store_f32(&s_memory[p0 + JPA_PTCL_FRAME - 0x80000000u], 6.0f);
        store_f32(&s_memory[p1 + JPA_PTCL_FRAME - 0x80000000u], 11.0f);
        for (k = 0; k < 3; ++k) {
            store_f32(&s_memory[p0 + JPA_PTCL_GLOBAL + (uint32_t)k * 4u - 0x80000000u], 2.0f + (float)k);
            store_f32(&s_memory[p1 + JPA_PTCL_GLOBAL + (uint32_t)k * 4u - 0x80000000u], 7.0f + (float)k);
        }
        /* a brand-new particle spawned mid-tick (frame 0) joins the list */
        store_be32(&s_memory[p1 + JPA_LINK_NEXT - 0x80000000u], p2);
        store_be32(&s_memory[p2 + JPA_LINK_NEXT - 0x80000000u], 0u);
        store_be32(&s_memory[emtr + JPA_EMTR_ACT + 0x08u - 0x80000000u], 3u);
        store_f32(&s_memory[p2 + JPA_PTCL_FRAME - 0x80000000u], 0.0f);
        for (k = 0; k < 3; ++k)
            store_f32(&s_memory[p2 + JPA_PTCL_GLOBAL + (uint32_t)k * 4u - 0x80000000u], 9.0f + (float)k);

        /* R-frame walk at alpha 0.5 */
        jpa_rframe(&state, 0.5f);
        rd_f32_arr(&state, p0 + JPA_PTCL_GLOBAL, pos, 3u);
        if (pos[0] != 1.5f || pos[1] != 2.5f || pos[2] != 3.5f) return 260;
        rd_f32_arr(&state, p1 + JPA_PTCL_GLOBAL, pos, 3u);
        if (pos[0] != 5.5f || pos[1] != 6.5f || pos[2] != 7.5f) return 261;
        /* new particle (never snapshotted) keeps its live position */
        rd_f32_arr(&state, p2 + JPA_PTCL_GLOBAL, pos, 3u);
        if (pos[0] != 9.0f || pos[1] != 10.0f || pos[2] != 11.0f) return 262;

        /* consecutive R-frame at a different alpha rewrites from the stored
         * pair — the live (already lerped) value is NOT re-read as an input */
        jpa_rframe(&state, 0.25f);
        rd_f32_arr(&state, p0 + JPA_PTCL_GLOBAL, pos, 3u);
        if (pos[0] != 1.25f || pos[1] != 2.25f || pos[2] != 3.25f) return 263;

        /* next L entry restores live positions before Painter draws */
        jpa_lframe(&state);
        rd_f32_arr(&state, p0 + JPA_PTCL_GLOBAL, pos, 3u);
        if (pos[0] != 2.0f || pos[1] != 3.0f || pos[2] != 4.0f) return 264;

        /* recycled address: same addr, non-continuous frame -> never lerp */
        store_f32(&s_memory[p0 + JPA_PTCL_FRAME - 0x80000000u], 50.0f);
        for (k = 0; k < 3; ++k)
            store_f32(&s_memory[p0 + JPA_PTCL_GLOBAL + (uint32_t)k * 4u - 0x80000000u], 50.0f + (float)k);
        jpa_rframe(&state, 0.5f);
        rd_f32_arr(&state, p0 + JPA_PTCL_GLOBAL, pos, 3u);
        if (pos[0] != 50.0f || pos[1] != 51.0f || pos[2] != 52.0f) return 265;

        /* corrupt manager pointer: the walk must bail, not touch memory */
        store_be32(&s_memory[JPA_MGR_EA - 0x80000000u], 0x80400000u);
        jpa_rframe(&state, 0.5f);   /* must simply not crash */
        jpa_lframe(&state);
        store_be32(&s_memory[JPA_MGR_EA - 0x80000000u], mgr);
    }

    /* ================= H1: list-heap seeding ==============================*/
    {
        uint32_t saved_tick = load_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u]);
        /* Unsplit path mirrors the live mCurrentHeap every iteration. */
        store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u], 450000u);
        s_memory[GINF_MCURRHEAP - 0x80000000u] = 1u;
        s_split_mode = 0u;
        frame60_accum_decide(&state, 0);
        if (s_list_heap != 1u) return 270;
        /* Engagement edge seeds from the live heap instead of guessing. */
        store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u], (uint32_t)TICK_30FPS);
        s_memory[GINF_MCURRHEAP - 0x80000000u] = 0u;
        s_list_heap = 1u;                       /* stale parity from before  */
        frame60_accum_decide(&state, 0);
        if (!s_split_mode) return 271;
        if (s_list_heap != 0u) return 272;      /* reseeded, not flipped     */
        /* While split, the L-frame flip still tracks the swap. */
        s_logic_this_frame = 1u; s_heap_pin = 1u;
        /* (flip lives in on_painter_skip — drive the flag through decide's
         * caller path is covered by the engagement check above) */
        store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u], saved_tick);
        s_memory[GINF_MCURRHEAP - 0x80000000u] = 0u;
    }

    /* ================= M1/M2: runtime reset + stream_dump free ============*/
    {
        /* Dirty every family of runtime static the review flagged. */
        s_fdr_l_drew = 1u; s_fdr_l_color = 0xDEADBEEFu;
        s_fdr_r_pending = 1u; s_fdr_r_saved = 0x12345678u;
        s_sea_snap = 1; s_sea_self = 0x80123456u; s_sea_orig = 77u;
        s_light_snap = 1; s_light_pt = 0x80123456u;
        s_pair_pad = 9u; s_list_heap = 1u;
        s_wptr_valid = 1; s_split_valid = 1; s_render_conf = 3u;
        s_painter_entry_wptr = 0xAAu; s_fcdw_entry_wptr = 0xBBu;
        s_rd_wptr0 = 0xCCu; s_fdl_self = 0x80123456u; s_fdl_hold = 5u;
        s_prev_xfb_dest = 0x80123456u; s_stream_dump_n = 12u;
        s_dlt_n = 7u; s_rng_lcount = 3u;
        s_fade_snap = 1; s_wipe_snap = 1;
        s_matrices_injected = 1u; s_draw_gen = 4u;
        s_cam_injected = 1u; s_cam_cut = 1u; s_cam_view = 0x80123456u;
        s_cam_has_prev = 1u; s_fol_cut = 1u;
        s_fol_grass.armed = 1u; s_fol_grass.self = 0x80123456u;
        s_jpa[0].addr = 0x80320000u; s_jpa[0].injected = 1u;
        s_trace_prev_drawn = 5;
        s_overlap_active = 1u; s_r_dup = 1u;
        /* Disengage edge must clear them all. */
        s_split_mode = 1u;
        store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u], 450000u);
        frame60_accum_decide(&state, 0);
        if (s_fdr_l_drew || s_fdr_l_color || s_fdr_r_pending || s_fdr_r_saved)
            return 280;
        if (s_sea_snap || s_sea_self || s_sea_orig) return 281;
        if (s_light_snap || s_light_pt) return 282;
        if (s_pair_pad || s_wptr_valid || s_split_valid || s_render_conf)
            return 283;
        if (s_painter_entry_wptr || s_fcdw_entry_wptr || s_rd_wptr0) return 284;
        if (s_fdl_self || s_fdl_hold) return 285;
        if (s_prev_xfb_dest || s_stream_dump_n || s_dlt_n || s_rng_lcount)
            return 286;
        if (s_fade_snap || s_wipe_snap || s_matrices_injected || s_draw_gen)
            return 287;
        if (s_cam_injected || s_cam_cut || s_cam_view || s_cam_has_prev)
            return 288;
        if (s_fol_cut || s_fol_grass.armed || s_fol_grass.self) return 289;
        if (s_jpa[0].addr || s_jpa[0].injected) return 290;
        if (s_trace_prev_drawn != -2) return 291;
        if (s_overlap_active || s_r_dup || s_split_mode) return 292;
        if (s_list_heap != (s_memory[GINF_MCURRHEAP - 0x80000000u] & 1u))
            return 293;

        /* Unload frees the lazy stream-dump buffer too (M2). */
        s_stream_dump_buf = (uint8_t*)malloc(16u);
        s_stream_dump_n = 9u;
        frame60_accum_on_unload();
        if (s_stream_dump_buf != 0) return 294;
        if (s_stream_dump_n != 0u) return 295;
        if (s_fdr_r_pending || s_sea_snap || s_list_heap != 0u) return 296;
    }

    /* ================= reset must UNDO injections, not just clear flags ====
     * A disengage edge after an R-frame inject must write the true values
     * back to guest memory: the fader colour, the J3D matrix arrays and the
     * camera view/proj fields. */
    {
        /* Fader: live mColor was overridden; saved word must go back. */
        const uint32_t fdr = 0x80240000u;
        store_be32(&s_memory[fdr + 0x0Cu - 0x80000000u], 0xDEADBEEFu);
        s_fdr_r_pending = 1u; s_fdr_r_saved = 0x12345678u; s_fdr_self = fdr;

        /* J3D: node array holds our lerp (scratch); curr must go back. */
        s_j3d_interp = 1u; s_model_alive = 0u;
        const uint32_t mdl = 0x80200000u, node = 0x80210000u;
        J3DHistory* h = j3d_history_ensure(mdl, 2u, 0u, 0);
        if (!h || !h->curr || !h->scratch)
            return 300;
        h->model_data = 0x80220000u;
        h->flags_f0 = 0x20u;
        h->node_ptr = node;
        for (uint32_t j = 0; j < 2u; ++j) {
            memset(&h->curr[j], 0, sizeof(J3DMtx));
            h->curr[j].m[0][0] = 7.0f + (float)j;
            memset(&h->scratch[j], 0, sizeof(J3DMtx));
            h->scratch[j].m[0][0] = 4.0f + (float)j;
        }
        write_mtx_arr(&state, node, h->scratch, 2u);
        h->injected = 1u; s_matrices_injected = 1u;

        /* Camera: view fields hold the lerp; curr must go back. */
        const uint32_t view = 0x80250000u;
        store_be32(&s_memory[GAMEINFO_MCURRVIEW - 0x80000000u], view);
        s_cam_view = view; s_cam_injected = 1u;
        memset(&s_cam_curr, 0, sizeof(s_cam_curr));
        s_cam_curr.m[0][0] = 11.0f;
        memset(&s_cam_curr_p, 0, sizeof(s_cam_curr_p));
        s_cam_curr_p.m[0][0] = 22.0f;
        store_f32(&s_memory[view + VIEW_OFF_VIEWMTX - 0x80000000u], 99.0f);
        store_f32(&s_memory[view + VIEW_OFF_PROJMTX - 0x80000000u], 88.0f);

        /* Disengage edge. */
        s_split_mode = 1u;
        store_be32(&s_memory[display + JFW_OFF_TICKRATE - 0x80000000u],
                   450000u);
        frame60_accum_decide(&state, 0);
        if (load_be32(&s_memory[fdr + 0x0Cu - 0x80000000u]) != 0x12345678u)
            return 301;   /* fader colour not restored */
        {
            J3DMtx got[2];
            read_mtx_arr(&state, node, got, 2u);
            if (got[0].m[0][0] != 7.0f || got[1].m[0][0] != 8.0f)
                return 302;   /* J3D matrices not restored */
        }
        if (load_be32(&s_memory[view + VIEW_OFF_VIEWMTX - 0x80000000u]) !=
                0x41300000u /* 11.0f */)
            return 303;   /* camera view not restored */
        if (load_be32(&s_memory[view + VIEW_OFF_PROJMTX - 0x80000000u]) !=
                0x41B00000u /* 22.0f */)
            return 304;   /* camera proj not restored */
        if (s_fdr_r_pending || s_fdr_self || s_matrices_injected ||
            s_cam_injected)
            return 305;   /* tracking flags not cleared */
        j3d_history_free(mdl);
        s_j3d_interp = 0u;
    }

    /* ==== VL: VI phase lock ====
     * Tick-exact pacing drifts against the 59.94 Hz retrace grid; with a
     * work-bound L-slot longer than a field the L-image window then misses
     * retraces in bursts. The lock must leave no dropped copy for light and
     * heavy scenes, keep logic at ~50% of iterations (30 Hz), and the
     * unlocked path must reproduce the drops. */
    {
        uint32_t ll, lc, drops;
        const uint32_t n = 3000u;
        s_split_mode = 0u; s_first = 0u; s_pair_lock = 1u;

        s_vi_lock = 1u;
        drops = vl_run(&state, display, 500000u, 150000u, 20000u, n, &ll, &lc);
        if (drops != 0u) return 400;                /* light scene     */
        if (lc < n / 2u - n / 50u || lc > n / 2u + n / 50u) return 401;
        drops = vl_run(&state, display, 740000u, 170000u, 40000u, n, &ll, &lc);
        if (drops != 0u) return 402;                /* sailing-heavy L */
        if (vl_alpha_off != 0u) return 408;         /* display-time alpha */
        if (lc < n / 2u - n / 50u || lc > n / 2u + n / 50u) return 403;
        if (ll > n / 400u + 2u) return 404;         /* slip ~1 per 500 pairs */
        drops = vl_run(&state, display, 1100000u, 150000u, 30000u, n, &ll, &lc);
        if (drops != 0u) return 405;                /* L near two fields */

        s_vi_lock = 0u;                             /* defect reproduces */
        drops = vl_run(&state, display, 740000u, 170000u, 40000u, n, &ll, &lc);
        if (drops == 0u) return 406;
        if (vl_alpha_off == 0u) return 409;         /* residual alpha when unlocked */

        /* no retrace grid (VI stalled): lock yields to the tick pad */
        s_vi_lock = 1u;
        store_be32(&s_memory[JUTVIDEO_LASTTICK - 0x80000000u], 0u);
        store_be32(&s_memory[JFW_NEXTTICK - 0x80000000u], 0u);
        store_be32(&s_memory[JFW_NEXTTICK + 4u - 0x80000000u], 90000000u);
        state.timebase = 90000000ull;
        s_split_mode = 1u; s_pair_pad = 0u;
        state.lr = JFW_BEGINRENDER_START + 0x40u;
        state.gpr[3] = (uint32_t)TICK_30FPS;
        on_wait_for_tick(&state);
        if (state.gpr[3] != (uint32_t)F60_FIELD_TICKS) return 407;
        s_vi_lock = 0u;
    }

    /* ==== GOV: headroom governor ====
     * Full speed must never degrade; sustained slow motion must switch
     * R-frames to dup-present, retry after the hold, double the hold on a
     * failed retry, skip retries while even dup is slow, and ignore pauses
     * and single hitches the throttle catches up on. */
    {
        F60Gov g;
        const uint64_t slot = TICK_30FPS;           /* guest ticks per L->L */
        const uint64_t rt = 33333333ull;            /* wall ns per slot at 1.0x */
        const uint64_t slow = 39215686ull;          /* 0.85x */
        uint64_t tb = 1000000ull, ns = 1000000000ull;
        uint32_t i, n;
#define GOV_STEP(dns) do { tb += slot; ns += (dns); gov_on_lframe(&g, tb, ns); } while (0)
        memset(&g, 0, sizeof(g));
        g.hold_ns = GOV_HOLD_MIN_NS;
        g.last_speed = 1.0;
        for (i = 0; i < 20000u; ++i) GOV_STEP(rt);
        if (g.degraded || g.probing) return 410;

        /* pause (5 s gap) and a hitch the throttle catches up on */
        ns += 5000000000ull; GOV_STEP(rt);
        GOV_STEP(120000000ull);
        for (i = 0; i < 7u; ++i) GOV_STEP(21000000ull);
        for (i = 0; i < 600u; ++i) GOV_STEP(rt);
        if (g.degraded || g.probing) return 411;
        /* shader-compile stutter: every 6th pair 80 ms, no catch-up. Mean
         * speed is 0.81x, but dropping R-frames cannot cure spikes, and the
         * median pair is at full speed. */
        for (i = 0; i < 20u * GOV_WINDOW; ++i) GOV_STEP((i % 6u) == 5u ? 80000000ull : rt);
        if (g.degraded || g.probing) return 425;
        for (i = 0; i < 2u * GOV_WINDOW; ++i) GOV_STEP(rt);

        /* sustained 0.85x: degrade within three windows */
        for (n = 0; n < 3u * GOV_WINDOW && !g.degraded; ++n) GOV_STEP(slow);
        if (!g.degraded || n < GOV_WINDOW) return 412;
        if (g.hold_ns != GOV_HOLD_MIN_NS) return 413;

        /* dup keeps full speed: no retry before the hold, one right after */
        for (i = 0; i < 300u; ++i) GOV_STEP(rt);            /* 10 s */
        if (!g.degraded) return 414;
        for (n = 0; n < 300u && g.degraded; ++n) GOV_STEP(rt);
        if (g.degraded || !g.probing) return 415;

        /* the retry is still slow: back to dup, hold doubled */
        for (n = 0; n < GOV_WINDOW + 1u && !g.degraded; ++n) GOV_STEP(slow);
        if (!g.degraded || g.probing) return 416;
        if (g.hold_ns != 2u * GOV_HOLD_MIN_NS) return 417;

        /* even dup is short of full speed: never retry */
        for (i = 0; i < 3000u; ++i) GOV_STEP(36000000ull);  /* 0.93x, 100 s */
        if (!g.degraded || g.probing) return 418;

        /* host recovers: retry succeeds, 60 fps stays, hold decays */
        for (n = 0; n < 3000u && g.degraded; ++n) GOV_STEP(rt);
        if (g.degraded || !g.probing) return 419;
        for (i = 0; i < GOV_WINDOW; ++i) GOV_STEP(rt);
        if (g.degraded || g.probing) return 420;
        for (i = 0; i < GOV_CLEAN_WINDOWS * GOV_WINDOW; ++i) GOV_STEP(rt);
        if (g.hold_ns != GOV_HOLD_MIN_NS || g.degraded) return 421;

        /* the hold never exceeds its cap */
        for (i = 0; i < 12u; ++i) {
            for (n = 0; n < 3u * GOV_WINDOW && !g.degraded; ++n) GOV_STEP(slow);
            for (n = 0; n < 20000u && g.degraded; ++n) GOV_STEP(rt);
        }
        if (g.hold_ns != GOV_HOLD_MAX_NS) return 422;
#undef GOV_STEP

        /* a degraded governor steers R-frames to dup-present */
        {
            const uint32_t disp = 0x80000100u;
            uint32_t r_n = 0, dup_n = 0;
            s_enabled = 1u; s_split_mode = 0u; s_first = 0u; s_acc = 0u;
            s_logic_this_frame = 1u;
            s_rframe_render = 1u; s_j3d_interp = 1u; s_fifo_replay = 0u;
            s_painter_skip_off = 0u; s_cam_interp = 1u; s_wnum_gate = 0u;
            store_be32(&s_memory[JFW_SINGLETON - 0x80000000u], disp);
            store_be32(&s_memory[disp + JFW_OFF_TICKRATE - 0x80000000u], (uint32_t)TICK_30FPS);
            store_be32(&s_memory[disp + JFW_OFF_TICKDELTA - 0x80000000u],
                       (uint32_t)(TICK_30FPS / 2u));
            store_be32(&s_memory[0x803F6160u - 0x80000000u], 0u);
            s_auto_degrade = 1u;
            memset(&s_gov, 0, sizeof(s_gov));
            s_gov.hold_ns = GOV_HOLD_MIN_NS;
            s_gov.degraded = 1u;
            s_gov.retry_at_ns = ~0ull;
            for (i = 0; i < 8u; ++i) {
                state.pc = 0x8000AF2Cu; state.gpr[3] = 0xDEADBEEFu;
                on_painter_skip(&state);
                if (!s_logic_this_frame) {
                    ++r_n;
                    if (state.pc == JFW_BEGIN_RENDER && state.gpr[3] == disp) ++dup_n;
                }
            }
            if (r_n == 0u || dup_n != r_n || s_gov.dup_frames != r_n) return 423;
            /* and back to the re-render once it clears */
            s_gov.degraded = 0u;
            dup_n = 0u; r_n = 0u;
            for (i = 0; i < 8u; ++i) {
                state.pc = 0x8000AF2Cu; state.gpr[3] = 0xDEADBEEFu;
                on_painter_skip(&state);
                if (!s_logic_this_frame) {
                    ++r_n;
                    if (state.pc == JFW_BEGIN_RENDER) ++dup_n;
                }
            }
            if (r_n == 0u || dup_n != 0u) return 424;
            s_auto_degrade = 0u; s_wnum_gate = 1u;
            s_rframe_render = 0u; s_j3d_interp = 0u;
            f60_reset_runtime_state(&state);
        }
    }

    return 0;
}
