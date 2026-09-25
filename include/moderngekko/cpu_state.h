#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODERNGEKKO_CPU_ABI_VERSION 3u
#define GXRUNTIME_CPU_ABI_VERSION MODERNGEKKO_CPU_ABI_VERSION

typedef struct CPUState CPUState;

typedef uint64_t (*PPCExternalRead)(CPUState* cpu, uint32_t address, uint8_t size);
typedef void (*PPCExternalWrite)(CPUState* cpu, uint32_t address, uint64_t value, uint8_t size);
typedef uint32_t (*PPCExternalRead32)(CPUState* cpu, uint32_t address, uint8_t region);
typedef void (*PPCExternalWrite32)(CPUState* cpu, uint32_t address, uint32_t value,
                                   uint8_t region);
typedef void* (*PPCExternalPointer)(CPUState* cpu, uint32_t address, uint32_t size);
typedef void (*PPCInstructionFallback)(CPUState* cpu, uint32_t instruction, uint32_t address);
typedef bool (*PPCHostCall)(CPUState* cpu, uint32_t address);
typedef uint32_t (*PPCSPRRead)(CPUState* cpu, uint16_t spr, uint32_t address);
typedef void (*PPCSPRWrite)(CPUState* cpu, uint16_t spr, uint32_t value, uint32_t address);
typedef void (*PPCCacheControl)(CPUState* cpu, uint8_t operation, uint32_t address,
                                uint32_t instruction_address);

struct CPUState
{
    uint32_t gpr[32];
    double fpr[32];
    double ps1[32];
    uint32_t pc;
    uint32_t lr;
    uint32_t ctr;
    uint32_t cr;
    uint32_t xer;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t dar;
    uint32_t dsisr;
    uint32_t ear;
    uint32_t hid2;
    uint64_t timebase;
    uint32_t sr[16];
    uint32_t gqr[8];
    uint32_t exception;
    uint32_t program_exception;
    uint32_t tlb_last_vps;
    uint32_t tlb_last_index;
    uint32_t tlb_invalidate_count;
    uint32_t external_addr;
    uint32_t external_value;
    uint8_t external_rid;
    uint8_t external_read_count;
    uint8_t external_write_count;
    uint32_t reserve_addr;
    bool reserve_valid;
    uint32_t locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;
    uint8_t* ram;
    uint32_t ram_size;
    PPCExternalPointer external_pointer;
    int64_t downcount;
    uint8_t* exram;
    uint32_t exram_size;
    PPCSPRRead spr_read;
    PPCSPRWrite spr_write;
    PPCCacheControl cache_control;
};

/* Wire-layout pins. GXRuntime's core/cpu.h ships a second, textual copy of
 * CPUState (the shared DOLRECOMP_CPU_H guard keeps exactly one definition
 * per TU); the module side compiles whichever it sees, so both must agree.
 * cpu_state_size catches size drift at module load but not a same-size field
 * reorder — these asserts do. Offsets through hid2 are absolute (no
 * alignment-sensitive member precedes them, so they hold on 32- and 64-bit
 * hosts); the tail is pinned by deltas so i64/function-pointer alignment
 * differences can't false-trip a legit port. Function pointers are assumed
 * pointer-sized on all supported targets — if that ever breaks, failing this
 * compile is the desired loud signal. */
#if defined(__cplusplus)
#define MODERNGEKKO_CPU_LAYOUT_ASSERT(name, expr) \
    static_assert(expr, "CPUState ABI layout drift: " #name)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MODERNGEKKO_CPU_LAYOUT_ASSERT(name, expr) \
    _Static_assert(expr, "CPUState ABI layout drift: " #name)
#else
#define MODERNGEKKO_CPU_LAYOUT_ASSERT(name, expr) \
    typedef char moderngekko_cpu_layout_##name[(expr) ? 1 : -1]
#endif

#define MODERNGEKKO_OFF(member) offsetof(CPUState, member)

MODERNGEKKO_CPU_LAYOUT_ASSERT(gpr_at_0, MODERNGEKKO_OFF(gpr) == 0u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(fpr_at_128, MODERNGEKKO_OFF(fpr) == 128u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(ps1_at_384, MODERNGEKKO_OFF(ps1) == 384u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(pc_at_640, MODERNGEKKO_OFF(pc) == 640u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(lr_at_644, MODERNGEKKO_OFF(lr) == 644u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(ctr_at_648, MODERNGEKKO_OFF(ctr) == 648u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(cr_at_652, MODERNGEKKO_OFF(cr) == 652u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(xer_at_656, MODERNGEKKO_OFF(xer) == 656u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(fpscr_at_660, MODERNGEKKO_OFF(fpscr) == 660u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(msr_at_664, MODERNGEKKO_OFF(msr) == 664u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(srr0_at_668, MODERNGEKKO_OFF(srr0) == 668u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(srr1_at_672, MODERNGEKKO_OFF(srr1) == 672u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(dar_at_676, MODERNGEKKO_OFF(dar) == 676u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(dsisr_at_680, MODERNGEKKO_OFF(dsisr) == 680u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(ear_at_684, MODERNGEKKO_OFF(ear) == 684u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(hid2_at_688, MODERNGEKKO_OFF(hid2) == 688u);

/* timebase is the first member whose alignment differs across hosts
 * (u64 wants 8 under 64-bit/MSVC-32, 4 under i386 SysV): window-assert it. */
MODERNGEKKO_CPU_LAYOUT_ASSERT(timebase_pad,
    MODERNGEKKO_OFF(timebase) - MODERNGEKKO_OFF(hid2) >= 4u &&
    MODERNGEKKO_OFF(timebase) - MODERNGEKKO_OFF(hid2) <= 8u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(sr_delta,
    MODERNGEKKO_OFF(sr) - MODERNGEKKO_OFF(timebase) == 8u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(gqr_delta,
    MODERNGEKKO_OFF(gqr) - MODERNGEKKO_OFF(sr) == 64u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(exception_delta,
    MODERNGEKKO_OFF(exception) - MODERNGEKKO_OFF(gqr) == 32u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(program_exception_delta,
    MODERNGEKKO_OFF(program_exception) - MODERNGEKKO_OFF(exception) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(tlb_last_vps_delta,
    MODERNGEKKO_OFF(tlb_last_vps) - MODERNGEKKO_OFF(program_exception) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(tlb_last_index_delta,
    MODERNGEKKO_OFF(tlb_last_index) - MODERNGEKKO_OFF(tlb_last_vps) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(tlb_invalidate_count_delta,
    MODERNGEKKO_OFF(tlb_invalidate_count) - MODERNGEKKO_OFF(tlb_last_index) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_addr_delta,
    MODERNGEKKO_OFF(external_addr) - MODERNGEKKO_OFF(tlb_invalidate_count) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_value_delta,
    MODERNGEKKO_OFF(external_value) - MODERNGEKKO_OFF(external_addr) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_rid_delta,
    MODERNGEKKO_OFF(external_rid) - MODERNGEKKO_OFF(external_value) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_read_count_delta,
    MODERNGEKKO_OFF(external_read_count) - MODERNGEKKO_OFF(external_rid) == 1u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_write_count_delta,
    MODERNGEKKO_OFF(external_write_count) - MODERNGEKKO_OFF(external_read_count) == 1u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(reserve_addr_pad,
    MODERNGEKKO_OFF(reserve_addr) - MODERNGEKKO_OFF(external_write_count) >= 1u &&
    MODERNGEKKO_OFF(reserve_addr) - MODERNGEKKO_OFF(external_write_count) <= 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(reserve_valid_delta,
    MODERNGEKKO_OFF(reserve_valid) - MODERNGEKKO_OFF(reserve_addr) == 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(locked_cache_tag_pad,
    MODERNGEKKO_OFF(locked_cache_tag) - MODERNGEKKO_OFF(reserve_valid) >= 1u &&
    MODERNGEKKO_OFF(locked_cache_tag) - MODERNGEKKO_OFF(reserve_valid) <= 4u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(locked_cache_valid_delta,
    MODERNGEKKO_OFF(locked_cache_valid) - MODERNGEKKO_OFF(locked_cache_tag) == 2048u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_read_pad,
    MODERNGEKKO_OFF(external_read) - MODERNGEKKO_OFF(locked_cache_valid) >= 512u &&
    MODERNGEKKO_OFF(external_read) - MODERNGEKKO_OFF(locked_cache_valid) <
        512u + sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_write_delta,
    MODERNGEKKO_OFF(external_write) - MODERNGEKKO_OFF(external_read) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_read32_delta,
    MODERNGEKKO_OFF(external_read32) - MODERNGEKKO_OFF(external_write) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_write32_delta,
    MODERNGEKKO_OFF(external_write32) - MODERNGEKKO_OFF(external_read32) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(instruction_fallback_delta,
    MODERNGEKKO_OFF(instruction_fallback) - MODERNGEKKO_OFF(external_write32) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(host_call_delta,
    MODERNGEKKO_OFF(host_call) - MODERNGEKKO_OFF(instruction_fallback) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_user_data_delta,
    MODERNGEKKO_OFF(external_user_data) - MODERNGEKKO_OFF(host_call) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(ram_delta,
    MODERNGEKKO_OFF(ram) - MODERNGEKKO_OFF(external_user_data) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(ram_size_delta,
    MODERNGEKKO_OFF(ram_size) - MODERNGEKKO_OFF(ram) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(external_pointer_pad,
    MODERNGEKKO_OFF(external_pointer) - MODERNGEKKO_OFF(ram_size) >= 4u &&
    MODERNGEKKO_OFF(external_pointer) - MODERNGEKKO_OFF(ram_size) <=
        4u + sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(downcount_pad,
    MODERNGEKKO_OFF(downcount) - MODERNGEKKO_OFF(external_pointer) >= sizeof(void*) &&
    MODERNGEKKO_OFF(downcount) - MODERNGEKKO_OFF(external_pointer) <=
        sizeof(void*) + 8u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(exram_delta,
    MODERNGEKKO_OFF(exram) - MODERNGEKKO_OFF(downcount) == 8u);
MODERNGEKKO_CPU_LAYOUT_ASSERT(exram_size_delta,
    MODERNGEKKO_OFF(exram_size) - MODERNGEKKO_OFF(exram) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(spr_read_pad,
    MODERNGEKKO_OFF(spr_read) - MODERNGEKKO_OFF(exram_size) >= 4u &&
    MODERNGEKKO_OFF(spr_read) - MODERNGEKKO_OFF(exram_size) <=
        4u + sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(spr_write_delta,
    MODERNGEKKO_OFF(spr_write) - MODERNGEKKO_OFF(spr_read) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(cache_control_delta,
    MODERNGEKKO_OFF(cache_control) - MODERNGEKKO_OFF(spr_write) == sizeof(void*));
MODERNGEKKO_CPU_LAYOUT_ASSERT(struct_tail,
    sizeof(CPUState) - MODERNGEKKO_OFF(cache_control) >= sizeof(void*) &&
    sizeof(CPUState) - MODERNGEKKO_OFF(cache_control) <= sizeof(void*) + 8u);

#undef MODERNGEKKO_OFF
#undef MODERNGEKKO_CPU_LAYOUT_ASSERT

#ifdef __cplusplus
}
#endif

#endif
