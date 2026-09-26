/*
 * mods/input-script/mod.c — scripted controller input for Wind Waker (GZLE01).
 *
 * Built as a ModernGekko .mgm mod (MODERNGEKKO_MOD_ABI_VERSION 1, CPU ABI 3).
 * Completely inert unless MODERNGEKKO_INPUT_SCRIPT=<file> is set: the
 * descriptor then reports zero hooks, so an unset env is a byte-exact no-op.
 *
 * Mechanism
 * ---------
 * PADRead (0x80315A20) fills JUTGamePad::mPadStatus[4] once per 30 Hz logic
 * tick (frame60-accum gates the whole mDoCPd_Read on R-frames, so even at
 * 60 fps the read happens on L-frames only). We hook PADRead's ENTRY to count
 * ticks and advance the script, and its RETURN to overwrite mPadStatus[0]
 * with the scripted state — before PADClamp and JUTGamePad's button/stick
 * update consume it, so the game's own edge detection (mButtonTrig),
 * trigger locks and JUTGamePadButton repeat see the injected state exactly
 * as if it came from hardware. The real controller is fully replaced on
 * port 0 while the script is active (ports 1-3 untouched).
 *
 * Script format (text, one command per line, '#' comments, blank lines ok):
 *   <tick> <buttons> [sx sy cx cy l r [a b]]
 *     buttons = A|B|X|Y|Z|L|R|START|DUP|DDOWN|DLEFT|DRIGHT joined by '+',
 *               or '-' for none.
 *     sticks  = -128..127 (default 0), triggers/analog = 0..255 (default 0).
 *     The state holds from <tick> until the next line activates.
 *   <tick> quit
 *     raises SIGTERM at that tick — the runner's HandleStopSignal path then
 *     shuts the emu down cleanly and --save-state-on-exit writes its state.
 * Ticks are pad-read ticks (30 Hz). Before the first command line, port 0 is
 * held neutral (script owns the port from tick 0 so real input can't race
 * the script's opening).
 *
 * Logs: "[input] tick=N" every 150 ticks, one "[input] t=<tick> ..." line per
 * command activation, "[input] quit" when the quit line fires.
 */
#include "moderngekko/mod_abi.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PADREAD_ADDR   0x80315A20u /* PADRead (SDK) — once per logic tick    */
#define PADSTATUS0     0x803ED818u /* JUTGamePad::mPadStatus[0] (.bss)       */
#define PADSTATUS_SIZE 12u

/* dolphin PAD_BUTTON_* bits */
#define BTN_DLEFT  0x0001u
#define BTN_DRIGHT 0x0002u
#define BTN_DDOWN  0x0004u
#define BTN_DUP    0x0008u
#define BTN_Z      0x0010u
#define BTN_R      0x0020u
#define BTN_L      0x0040u
#define BTN_A      0x0100u
#define BTN_B      0x0200u
#define BTN_X      0x0400u
#define BTN_Y      0x0800u
#define BTN_START  0x1000u

#define INPUT_MAX_CMDS 2048

typedef struct InputCmd {
    uint32_t tick;
    uint32_t buttons;
    int8_t  sx, sy, cx, cy;
    uint8_t l, r, a, b;
    uint8_t quit;
} InputCmd;

static InputCmd s_cmds[INPUT_MAX_CMDS];
static uint32_t s_ncmds;
static uint32_t s_idx;        /* next command index to activate            */
static InputCmd s_cur;        /* currently held state                      */
static uint32_t s_tick;       /* pad-read ticks since boot                 */
static int      s_enabled;    /* env set + script parsed                   */
static int      s_quit_fired;

/* ---- memory helpers (same pattern as frame60-accum: direct BE MEM1) ---- */
static int in_ram(const CPUState* s, uint32_t ea, uint32_t size)
{
    return s->ram && (ea - 0x80000000u) <= s->ram_size - size;
}
static void wr8_mem(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 1u)) s->ram[a - 0x80000000u] = (uint8_t)v;
    else moderngekko_mod_write(s, a, v, 1u);
}
static void wr16_mem(CPUState* s, uint32_t a, uint32_t v)
{
    if (in_ram(s, a, 2u)) {
        uint8_t* p = s->ram + (a - 0x80000000u);
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else moderngekko_mod_write(s, a, v, 2u);
}

/* ---- parser (pure C; exercised by tests/input_script_test.c) ----------- */
typedef struct InputNameBit {
    const char* name;
    uint32_t bit;
} InputNameBit;
static const InputNameBit s_button_names[] = {
    { "A", BTN_A }, { "B", BTN_B }, { "X", BTN_X }, { "Y", BTN_Y },
    { "Z", BTN_Z }, { "L", BTN_L }, { "R", BTN_R }, { "START", BTN_START },
    { "DUP", BTN_DUP }, { "DDOWN", BTN_DDOWN },
    { "DLEFT", BTN_DLEFT }, { "DRIGHT", BTN_DRIGHT },
};

/* Parse a '+'-joined button list (or "-"). Returns 1 on success. */
static int is_parse_buttons(const char* tok, uint32_t* out)
{
    uint32_t bits = 0;
    if (!tok || !*tok) return 0;
    if (tok[0] == '-' && tok[1] == '\0') { *out = 0; return 1; }
    char buf[128];
    if (strlen(tok) >= sizeof(buf)) return 0;
    strcpy(buf, tok);
    for (char* p = strtok(buf, "+"); p; p = strtok(NULL, "+")) {
        int found = 0;
        for (size_t i = 0; i < sizeof(s_button_names) / sizeof(s_button_names[0]); ++i) {
            if (!strcmp(p, s_button_names[i].name)) {
                bits |= s_button_names[i].bit;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    *out = bits;
    return 1;
}

static int is_parse_int(const char* tok, long lo, long hi, long* out)
{
    if (!tok || !*tok) return 0;
    char* end = NULL;
    long v = strtol(tok, &end, 10);
    if (!end || end == tok || *end != '\0') return 0;
    if (v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

/* Parse one script line. Returns:
 *   2 = command parsed, 1 = blank/comment (skip), 0 = malformed. */
static int is_parse_line(const char* line, InputCmd* out)
{
    char buf[256];
    while (*line == ' ' || *line == '\t') ++line;
    if (!*line || *line == '#' || *line == '\r' || *line == '\n') return 1;
    if (strlen(line) >= sizeof(buf)) return 0;
    strcpy(buf, line);
    /* strip trailing comment / newline */
    for (char* p = buf; *p; ++p) {
        if (*p == '#' || *p == '\r' || *p == '\n') { *p = '\0'; break; }
    }
    char* tok[16];
    int n = 0;
    for (char* p = strtok(buf, " \t"); p && n < 16; p = strtok(NULL, " \t"))
        tok[n++] = p;
    if (n == 0) return 1;
    if (n < 2) return 0;

    memset(out, 0, sizeof(*out));
    long t;
    if (!is_parse_int(tok[0], 0, 0x7FFFFFFFL, &t)) return 0;
    out->tick = (uint32_t)t;

    if (!strcmp(tok[1], "quit")) {
        if (n != 2) return 0;
        out->quit = 1;
        return 2;
    }
    if (!is_parse_buttons(tok[1], &out->buttons)) return 0;
    /* optional analogs: sx sy cx cy l r [a b] */
    static const long lo[8] = { -128, -128, -128, -128, 0, 0, 0, 0 };
    static const long hi[8] = { 127, 127, 127, 127, 255, 255, 255, 255 };
    long v[8] = {0};
    const int given = n - 2;
    if (given > 8) return 0;
    for (int i = 0; i < given; ++i)
        if (!is_parse_int(tok[2 + i], lo[i], hi[i], &v[i])) return 0;
    out->sx = (int8_t)v[0]; out->sy = (int8_t)v[1];
    out->cx = (int8_t)v[2]; out->cy = (int8_t)v[3];
    out->l = (uint8_t)v[4]; out->r = (uint8_t)v[5];
    out->a = (uint8_t)v[6]; out->b = (uint8_t)v[7];
    return 2;
}

/* Load + parse the whole script. Returns 1 on success. */
static int is_load_script(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[input] cannot open script '%s'\n", path);
        return 0;
    }
    char line[512];
    uint32_t lineno = 0, prev_tick = 0;
    int seen = 0;
    s_ncmds = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        InputCmd c;
        const int r = is_parse_line(line, &c);
        if (r == 1) continue;
        if (r == 0 || s_ncmds >= INPUT_MAX_CMDS) {
            fprintf(stderr, "[input] FATAL malformed line %u: %s", lineno, line);
            fclose(f);
            return 0;
        }
        if (seen && c.tick < prev_tick) {
            fprintf(stderr, "[input] FATAL line %u: tick %u goes backwards\n",
                    lineno, c.tick);
            fclose(f);
            return 0;
        }
        prev_tick = c.tick;
        seen = 1;
        s_cmds[s_ncmds++] = c;
    }
    fclose(f);
    if (s_ncmds == 0) {
        fprintf(stderr, "[input] FATAL script '%s' has no commands\n", path);
        return 0;
    }
    fprintf(stderr, "[input] loaded %u command(s) from '%s'\n", s_ncmds, path);
    return 1;
}

/* ---- hook bodies -------------------------------------------------------- */
static void on_padread(CPUState* state)
{
    (void)state;
    /* activate every command whose tick has arrived (in-order script) */
    while (s_idx < s_ncmds && s_cmds[s_idx].tick <= s_tick) {
        const InputCmd* c = &s_cmds[s_idx++];
        if (c->quit) {
            if (!s_quit_fired) {
                s_quit_fired = 1;
                fprintf(stderr, "[input] quit at tick=%u — raising SIGTERM\n",
                        s_tick);
                raise(SIGTERM);
            }
            continue;
        }
        s_cur = *c;
        fprintf(stderr,
                "[input] t=%u buttons=0x%04X s=(%d,%d) c=(%d,%d) l=%u r=%u\n",
                s_tick, s_cur.buttons, s_cur.sx, s_cur.sy, s_cur.cx, s_cur.cy,
                s_cur.l, s_cur.r);
    }
    if (s_tick % 150u == 0u)
        fprintf(stderr, "[input] tick=%u\n", s_tick);
    ++s_tick;
}

static void on_padread_return(CPUState* state)
{
    /* PADRead just filled mPadStatus; overwrite port 0 before PADClamp and
     * JUTGamePad's button update consume it. */
    if (!in_ram(state, PADSTATUS0, PADSTATUS_SIZE))
        return;
    wr16_mem(state, PADSTATUS0 + 0u, s_cur.buttons);
    wr8_mem(state, PADSTATUS0 + 2u, (uint32_t)(uint8_t)s_cur.sx);
    wr8_mem(state, PADSTATUS0 + 3u, (uint32_t)(uint8_t)s_cur.sy);
    wr8_mem(state, PADSTATUS0 + 4u, (uint32_t)(uint8_t)s_cur.cx);
    wr8_mem(state, PADSTATUS0 + 5u, (uint32_t)(uint8_t)s_cur.cy);
    wr8_mem(state, PADSTATUS0 + 6u, s_cur.l);
    wr8_mem(state, PADSTATUS0 + 7u, s_cur.r);
    wr8_mem(state, PADSTATUS0 + 8u, s_cur.a);
    wr8_mem(state, PADSTATUS0 + 9u, s_cur.b);
    wr8_mem(state, PADSTATUS0 + 10u, 0u); /* err = 0: pad present */
}

static void input_script_on_load(const ModernGekkoModHostApi* api)
{
    (void)api;
    s_enabled = 0;
    s_ncmds = s_idx = s_tick = 0;
    s_quit_fired = 0;
    memset(&s_cur, 0, sizeof(s_cur));
    const char* path = getenv("MODERNGEKKO_INPUT_SCRIPT");
    if (!path || !*path)
        return; /* complete no-op */
    if (!is_load_script(path))
        return;
    s_enabled = 1;
}

static void input_script_on_unload(void)
{
    s_enabled = 0;
    s_ncmds = s_idx = s_tick = 0;
    s_quit_fired = 0;
    memset(&s_cur, 0, sizeof(s_cur));
}

static const ModernGekkoModHook hooks[] = {
    RECOMP_HOOK(PADREAD_ADDR, on_padread),
    RECOMP_HOOK_RETURN(PADREAD_ADDR, on_padread_return),
};

static ModernGekkoModDesc descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "GZLE01",
    "input-script",
    "0.1.0",
    "Scripted pad input: MODERNGEKKO_INPUT_SCRIPT=<file> drives controller port 0",
    0, 0u,
    0, 0u,
    hooks, (uint32_t)(sizeof(hooks) / sizeof(hooks[0])),
    0, 0u,
    0, 0u,
    0, 0u,
    0, 0u,
    input_script_on_load,
    input_script_on_unload,
};

MODERNGEKKO_MOD_EXPORT const ModernGekkoModDesc* moderngekko_get_mod(void)
{
    input_script_on_load(0);
    if (!s_enabled) {
        descriptor.hooks = 0;
        descriptor.num_hooks = 0u;
    }
    return &descriptor;
}
