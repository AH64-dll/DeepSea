/* rel_loader_bankwave_test.c — spec test for the initOnCodeWs carve inside
 * rel_loader.c's wsLoadStatus store hook (scripts/disc_builder/module/
 * rel_loader.c, offset==0x3F758C case, ~lines 1226-1282).
 *
 * The carve block is inline in a guest-store hook and depends on generated
 * headers, so this test replicates its copy loops byte-for-byte over a fake
 * guest-RAM image and asserts the contract the fix depends on:
 *
 *   1. initOnCodeWs[i].field_0x0 is a POINTER into the live JaiInit.aaf
 *      buffer, not a string. The verbatim byte copy must preserve it even
 *      though the pointed-to WSYS blob begins "WSYS\0" (NUL at byte 4) —
 *      the historical bug treated it as a name string and truncated every
 *      blob at that NUL, producing empty wave banks and silent sequenced
 *      audio.
 *   2. The 4-byte zero sentinel transInitDataFile appended is copied too;
 *      guest loops bound themselves on entry[i].field_0x0 != 0.
 *   3. The three statics are repointed into the loader-reserved block at
 *      0x80408000 in entry/group/status order with 32-byte alignment.
 *   4. Bounds guards: nothing copies unless wsmax2 in (0,256), the
 *      footprint fits kWsBase..+0x2000, and all three source pointers sit
 *      in guest RAM — an oversized or garbage table must be left alone.
 *
 * The replicated loops are kept textually identical to rel_loader.c
 * 1254-1267; if the source loop changes, update this test in step.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define RAM_BASE 0x80000000u
#define RAM_END  0x81800000u
static u8 s_ram[0x1800000];         /* fake MEM1 0x80000000-0x81800000      */
static u32 s_ram_size = sizeof s_ram;

/* Statics the hook reads/writes (guest addresses). */
#define A_IOCWS 0x803F7584u
#define A_WSGRP 0x803F7588u
#define A_WSLP  0x803F758Cu
#define A_WSMAX 0x803F7590u

static u32 rel_rd32(u32 a)
{
    const u8* p = &s_ram[a - RAM_BASE];
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void rel_wr32(u32 a, u32 v)
{
    u8* p = &s_ram[a - RAM_BASE];
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* ---- verbatim carve body (rel_loader.c 1245-1280 minus diagnostics) ---- */
static void bankwave_carve(void)
{
    static const u32 kWsBase = 0x80408000u;
    u32 iocws = rel_rd32(A_IOCWS);
    u32 wsgrp = rel_rd32(A_WSGRP);
    u32 wslp  = rel_rd32(A_WSLP);
    u32 wsmax2 = rel_rd32(A_WSMAX);
    u32 ws_need = ((wsmax2 * 12u + 4u + 31u) & ~31u) +
                  ((wsmax2 * 4u + 31u) & ~31u) + wsmax2 * 4u;
    if (!(wslp >= kWsBase && wslp < kWsBase + 0x2000u) &&
        wsmax2 > 0u && wsmax2 < 256u && ws_need <= 0x2000u &&
        wsgrp >= 0x80000000u && wsgrp < 0x81800000u &&
        iocws >= 0x80000000u && iocws < 0x81800000u &&
        wslp >= 0x80000000u && wslp < 0x81800000u) {
        u32 cur = kWsBase;
        u32 j;
        for (j = 0; j < wsmax2 * 12u + 4u && iocws - 0x80000000u + j < s_ram_size &&
                    cur - 0x80000000u + j < s_ram_size; j++)
            s_ram[cur - 0x80000000u + j] = s_ram[iocws - 0x80000000u + j];
        rel_wr32(A_IOCWS, cur);
        cur += (wsmax2 * 12u + 4u + 31u) & ~31u;
        for (j = 0; j < wsmax2 * 4u && wsgrp - 0x80000000u + j < s_ram_size &&
                    cur - 0x80000000u + j < s_ram_size; j++)
            s_ram[cur - 0x80000000u + j] = s_ram[wsgrp - 0x80000000u + j];
        rel_wr32(A_WSGRP, cur);
        cur += (wsmax2 * 4u + 31u) & ~31u;
        for (j = 0; j < wsmax2 * 4u && wslp - 0x80000000u + j < s_ram_size &&
                    cur - 0x80000000u + j < s_ram_size; j++)
            s_ram[cur - 0x80000000u + j] = s_ram[wslp - 0x80000000u + j];
        rel_wr32(A_WSLP, cur);
    }
}
/* ---- end verbatim block ---- */

static int s_fail;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)

#define NWS 65u
#define AAF_BASE 0x814E0000u        /* fake live aaf buffer band */
#define TBL_IOCWS 0x806F6C60u       /* matches observed runtime values */
#define TBL_WSGRP 0x806F7F40u
#define TBL_WSLP  0x806F8060u

static void build_world(u32 n)
{
    u32 i;
    memset(s_ram, 0, sizeof s_ram);
    /* WSYS blobs: 'WSYS' + NUL at byte 4 (the truncation tripwire), then
     * archive-bank offset at +0x10 and control-group offset at +0x14. */
    for (i = 0; i < n; i++) {
        u32 blob = AAF_BASE + i * 0x1700u;
        u8* b = &s_ram[blob - RAM_BASE];
        memcpy(b, "WSYS", 4); b[4] = 0;
        rel_wr32(blob + 0x10u, 0x00000A40u + i);   /* archOff */
        rel_wr32(blob + 0x14u, 0x000016E0u + i);   /* ctrlOff */
    }
    /* initOnCodeWs table: 12-byte entries { field_0x0=blob ptr, ... } and a
     * zero sentinel word right after entry n-1. */
    for (i = 0; i < n; i++) {
        rel_wr32(TBL_IOCWS + i * 12u, AAF_BASE + i * 0x1700u);
        rel_wr32(TBL_IOCWS + i * 12u + 4u, 0x11223300u + i);
        rel_wr32(TBL_IOCWS + i * 12u + 8u, 0xAABB0000u + i);
    }
    /* sentinel already zeroed by memset */
    for (i = 0; i < n; i++) {
        rel_wr32(TBL_WSGRP + i * 4u, 0xBEEF0000u + i);
        rel_wr32(TBL_WSLP  + i * 4u, 0xCAFE0000u + i);
    }
    rel_wr32(A_IOCWS, TBL_IOCWS);
    rel_wr32(A_WSGRP, TBL_WSGRP);
    rel_wr32(A_WSLP,  TBL_WSLP);
    rel_wr32(A_WSMAX, n);
}

int main(void)
{
    u32 i, base;

    /* --- 1. happy path: 65 entries, live aaf pointers survive verbatim --- */
    build_world(NWS);
    bankwave_carve();
    base = 0x80408000u;
    CHECK(rel_rd32(A_IOCWS) == base);
    CHECK(rel_rd32(A_WSGRP) == base + ((NWS * 12u + 4u + 31u) & ~31u));
    CHECK(rel_rd32(A_WSLP)  == base + ((NWS * 12u + 4u + 31u) & ~31u) +
                                    ((NWS * 4u + 31u) & ~31u));
    /* footprint stays inside the 0x2000 reserved window */
    CHECK(rel_rd32(A_WSLP) + NWS * 4u <= base + 0x2000u);
    for (i = 0; i < NWS; i++) {
        u32 e = base + i * 12u;
        /* field_0x0 kept verbatim = still points at the real WSYS blob */
        CHECK(rel_rd32(e) == AAF_BASE + i * 0x1700u);
        /* and the blob's offsets are therefore intact for WSParser */
        CHECK(rel_rd32(rel_rd32(e) + 0x10u) == 0x00000A40u + i);
        CHECK(rel_rd32(rel_rd32(e) + 0x14u) == 0x000016E0u + i);
        /* rest of the entry bytes copied too */
        CHECK(rel_rd32(e + 4u) == 0x11223300u + i);
        CHECK(rel_rd32(e + 8u) == 0xAABB0000u + i);
    }
    /* sentinel: the terminating word after the last entry is copied as 0 —
     * guest loops read initOnCodeWs[i].field_0x0 until NULL */
    CHECK(rel_rd32(base + NWS * 12u) == 0u);
    /* group/status arrays verbatim */
    for (i = 0; i < NWS; i++) {
        CHECK(rel_rd32(rel_rd32(A_WSGRP) + i * 4u) == 0xBEEF0000u + i);
        CHECK(rel_rd32(rel_rd32(A_WSLP)  + i * 4u) == 0xCAFE0000u + i);
    }

    /* --- 2. regression pin: the old "copy the name string" semantics would
     * have repointed field_0x0 at a truncated stub whose WSYS header reads
     * archOff=0/ctrlOff=0 — exactly the empty-bank failure. Prove our world
     * actually contains the NUL that used to trigger it. */
    CHECK(s_ram[AAF_BASE - RAM_BASE + 4] == 0);

    /* --- 3. re-arm: carve a second time (audio re-init fires the hook
     * again). wslp now already sits inside the reserved window, so the
     * guard must skip the recopy and the statics must NOT be repointed. */
    {
        u32 keep_iocws = rel_rd32(A_IOCWS);
        u32 keep_wsgrp = rel_rd32(A_WSGRP);
        u32 keep_wslp  = rel_rd32(A_WSLP);
        bankwave_carve();
        CHECK(rel_rd32(A_IOCWS) == keep_iocws);
        CHECK(rel_rd32(A_WSGRP) == keep_wsgrp);
        CHECK(rel_rd32(A_WSLP)  == keep_wslp);
    }

    /* --- 4. bounds: wsmax2 at/past the <256 gate -> no carve. (With
     * wsmax2<256 the 0x2000 footprint check can never actually fail —
     * 255 entries need only 5,148 B — so the count gate is what fires.) --- */
    build_world(200);
    bankwave_carve();
    CHECK(rel_rd32(A_IOCWS) == base);        /* 200 entries still carve */
    build_world(NWS);
    rel_wr32(A_WSMAX, 256u);
    bankwave_carve();
    CHECK(rel_rd32(A_IOCWS) == TBL_IOCWS);   /* untouched */
    CHECK(rel_rd32(A_WSGRP) == TBL_WSGRP);
    CHECK(rel_rd32(A_WSLP)  == TBL_WSLP);

    /* --- 5. bounds: wsmax2
 == 0 or garbage -> no carve --- */
    build_world(NWS);
    rel_wr32(A_WSMAX, 0u);
    bankwave_carve();
    CHECK(rel_rd32(A_IOCWS) == TBL_IOCWS);
    build_world(NWS);
    rel_wr32(A_WSMAX, 0xFFFFFFFFu);
    bankwave_carve();
    CHECK(rel_rd32(A_IOCWS) == TBL_IOCWS);

    /* --- 6. bounds: source table outside guest RAM -> no carve --- */
    build_world(NWS);
    rel_wr32(A_IOCWS, 0x70000000u);
    bankwave_carve();
    CHECK(rel_rd32(A_IOCWS) == 0x70000000u); /* left alone */

    if (s_fail == 0) {
        printf("rel_loader_bankwave_test: all checks passed\n");
        return 0;
    }
    printf("rel_loader_bankwave_test: %d check(s) FAILED\n", s_fail);
    return 1;
}
