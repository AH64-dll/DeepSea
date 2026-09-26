#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mods/input-script/mod.c"

/* Parser unit tests for mods/input-script. Pure-C coverage: line grammar,
 * button names, analog ranges, quit, monotonic-tick enforcement in
 * is_load_script, script activation via on_padread, and the PADStatus write
 * via on_padread_return against a fake MEM1 window. */
static uint8_t s_memory[0x400000];

static CPUState test_state(void)
{
    CPUState st;
    memset(&st, 0, sizeof(st));
    st.ram = s_memory;
    st.ram_size = sizeof(s_memory);
    return st;
}

static uint16_t ld_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static int t_grammar(void)
{
    InputCmd c;
    if (is_parse_line("\n", &c) != 1) return 10;
    if (is_parse_line("   \n", &c) != 1) return 11;
    if (is_parse_line("# comment\n", &c) != 1) return 12;
    if (is_parse_line("   # indented comment\n", &c) != 1) return 13;
    if (is_parse_line("", &c) != 1) return 14;

    if (is_parse_line("0 -\n", &c) != 2 || c.tick != 0 || c.buttons != 0 ||
        c.sx || c.sy || c.cx || c.cy || c.l || c.r || c.a || c.b || c.quit)
        return 15;
    if (is_parse_line("30 A\n", &c) != 2 || c.buttons != BTN_A) return 16;
    if (is_parse_line("60 A+B+START\n", &c) != 2 ||
        c.buttons != (BTN_A | BTN_B | BTN_START)) return 17;
    if (is_parse_line("90 DUP+DDOWN+DLEFT+DRIGHT+Z+L+R+X+Y\n", &c) != 2 ||
        c.buttons != (BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT | BTN_Z |
                      BTN_L | BTN_R | BTN_X | BTN_Y)) return 18;
    /* all six optional analogs + two more for analogA/B */
    if (is_parse_line("120 - 100 -100 127 -128 200 128 64 32\n", &c) != 2 ||
        c.sx != 100 || c.sy != -100 || c.cx != 127 || c.cy != -128 ||
        c.l != 200 || c.r != 128 || c.a != 64 || c.b != 32) return 19;
    /* partial analogs default the rest to 0 */
    if (is_parse_line("150 - 80 90\n", &c) != 2 || c.sx != 80 || c.sy != 90 ||
        c.cx != 0 || c.l != 0) return 20;
    /* trailing comment tolerated */
    if (is_parse_line("180 A # press\n", &c) != 2 || c.buttons != BTN_A)
        return 21;
    return 0;
}

static int t_malformed(void)
{
    InputCmd c;
    if (is_parse_line("abc A\n", &c) != 0) return 30;          /* bad tick   */
    if (is_parse_line("-1 A\n", &c) != 0) return 31;           /* neg tick   */
    if (is_parse_line("10\n", &c) != 0) return 32;             /* missing op */
    if (is_parse_line("10 Q\n", &c) != 0) return 33;           /* bad button */
    if (is_parse_line("10 A+Bogus\n", &c) != 0) return 34;     /* bad combo  */
    if (is_parse_line("10 A 128\n", &c) != 0) return 35;       /* stick >127 */
    if (is_parse_line("10 A -129\n", &c) != 0) return 36;      /* stick <-128*/
    if (is_parse_line("10 A 0 0 0 0 256\n", &c) != 0) return 37; /* trig>255 */
    if (is_parse_line("10 A x\n", &c) != 0) return 38;         /* non-int    */
    if (is_parse_line("10 quit extra\n", &c) != 0) return 39;  /* quit args  */
    if (is_parse_line("10 A 1 2 3 4 5 6 7 8 9\n", &c) != 0) return 40;
    if (is_parse_line("5 quit\n", &c) != 2 || !c.quit) return 41;
    return 0;
}

static int t_load_and_activate(void)
{
    const char* path = "input_script_test_tmp.txt";
    FILE* f = fopen(path, "wb");
    if (!f) return 50;
    fputs("# test script\n"
          "0 -\n"
          "10 START\n"
          "20 A 0 90\n"
          "40 B+A\n"
          "60 quit\n", f);
    fclose(f);
    if (!is_load_script(path)) return 51;
    if (s_ncmds != 5) return 52;
    s_idx = 0; s_tick = 0; memset(&s_cur, 0, sizeof(s_cur));
    CPUState st = test_state();

    for (int i = 0; i < 10; ++i) on_padread(&st);   /* ticks 0..9  */
    if (s_cur.buttons != 0) return 53;              /* neutral     */
    for (int i = 0; i < 10; ++i) on_padread(&st);   /* ticks 10..19*/
    if (s_cur.buttons != BTN_START) return 54;
    if (s_tick != 20) return 55;
    for (int i = 0; i < 20; ++i) on_padread(&st);   /* ticks 20..39*/
    if (s_cur.buttons != BTN_A || s_cur.sy != 90) return 56;

    /* PADStatus[0] write: BE layout at PADSTATUS0 (checked while the
     * analog-bearing command is the held state). */
    memset(s_memory, 0xAA, sizeof(s_memory));
    on_padread_return(&st);
    const uint32_t off = PADSTATUS0 - 0x80000000u;
    if (ld_be16(s_memory + off) != BTN_A) return 58;
    if (s_memory[off + 3] != 90) return 59;

    for (int i = 0; i < 20; ++i) on_padread(&st);   /* ticks 40..59*/
    if (s_cur.buttons != (BTN_B | BTN_A)) return 57;
    memset(s_memory, 0xAA, sizeof(s_memory));
    on_padread_return(&st);
    if (ld_be16(s_memory + off) != (BTN_B | BTN_A)) return 58;
    if (s_memory[off + 10] != 0) return 60;   /* err = 0 */
    if (s_memory[off + 11] != 0xAA) return 61;/* pad byte untouched */

    /* backwards ticks rejected */
    f = fopen(path, "wb");
    fputs("30 A\n20 B\n", f);
    fclose(f);
    if (is_load_script(path)) return 62;
    /* empty script rejected */
    f = fopen(path, "wb");
    fputs("# nothing\n", f);
    fclose(f);
    if (is_load_script(path)) return 63;
    /* missing file rejected */
    if (is_load_script("definitely-not-here.txt")) return 64;
    remove(path);
    return 0;
}

int main(void)
{
    int rc;
    if ((rc = t_grammar())) return rc;
    if ((rc = t_malformed())) return rc;
    if ((rc = t_load_and_activate())) return rc;
    fprintf(stderr, "input_script_test: all tests passed\n");
    return 0;
}
