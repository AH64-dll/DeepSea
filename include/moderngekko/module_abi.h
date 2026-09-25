#ifndef MODERNGEKKO_MODULE_ABI_H
#define MODERNGEKKO_MODULE_ABI_H

#include "moderngekko/cpu_state.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODERNGEKKO_MODULE_ABI_VERSION 3u
#define MODERNGEKKO_GET_MODULE_SYMBOL "staticrecomp_get_module"

#if defined(_WIN32)
#define MODERNGEKKO_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define MODERNGEKKO_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define MODERNGEKKO_MODULE_EXPORT
#endif

typedef struct ModernGekkoRange
{
    uint32_t start;
    uint32_t end;
} ModernGekkoRange;

typedef struct ModernGekkoRelSection
{
    uint32_t module_id;
    uint32_t section_index;
    uint32_t linked_start;
    uint32_t size;
} ModernGekkoRelSection;

typedef struct ModernGekkoRelModule
{
    uint32_t module_id;
    uint32_t version;
    uint32_t section_count;
    uint32_t section_info_offset;
    uint32_t file_size;
    const ModernGekkoRelSection* sections;
    uint32_t num_sections;
} ModernGekkoRelModule;

typedef struct ModernGekkoModuleDesc
{
    uint32_t abi_version;
    uint32_t cpu_abi_version;
    uint32_t cpu_state_size;
    char game_id[8];
    uint32_t entry_point;

    int (*dispatch)(CPUState* state, uint32_t address);
    void (*on_state_loaded)(CPUState* state);

    const ModernGekkoRange* code_ranges;
    uint32_t num_code_ranges;
    const ModernGekkoRange* smc_ranges;
    uint32_t num_smc_ranges;
    const ModernGekkoRange* chunk_ranges;
    uint32_t num_chunk_ranges;
    const uint64_t* chunk_hashes;
    const ModernGekkoRelModule* rel_modules;
    uint32_t num_rel_modules;
} ModernGekkoModuleDesc;

typedef const ModernGekkoModuleDesc* (*ModernGekkoGetModuleFn)(void);

/* Wire-layout pins. The vendored StaticRecompABI.h ships a second, textual
 * copy of these structs for module-side builds that never see this header;
 * the two definitions must agree exactly. Any layout change here is an ABI
 * break — bump MODERNGEKKO_MODULE_ABI_VERSION. Offsets through entry_point
 * are absolute (only u32/char members precede them, so they hold on every
 * host); the pointer-bearing tail is expressed in sizeof(void*) so the same
 * constants hold on 32- and 64-bit builds (function-pointer size equals
 * sizeof(void*) on every supported target — a platform where it does not
 * fails these asserts, which is the desired fail-loud behavior). */
#if defined(__cplusplus)
#define MODERNGEKKO_ABI_LAYOUT_ASSERT(name, expr) \
    static_assert(expr, "ModernGekko module ABI layout drift: " #name)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MODERNGEKKO_ABI_LAYOUT_ASSERT(name, expr) \
    _Static_assert(expr, "ModernGekko module ABI layout drift: " #name)
#else
#define MODERNGEKKO_ABI_LAYOUT_ASSERT(name, expr) \
    typedef char moderngekko_abi_layout_##name[(expr) ? 1 : -1]
#endif

MODERNGEKKO_ABI_LAYOUT_ASSERT(range_size, sizeof(ModernGekkoRange) == 8u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(range_start, offsetof(ModernGekkoRange, start) == 0u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(range_end, offsetof(ModernGekkoRange, end) == 4u);

MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_section_size, sizeof(ModernGekkoRelSection) == 16u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_section_module_id,
    offsetof(ModernGekkoRelSection, module_id) == 0u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_section_index,
    offsetof(ModernGekkoRelSection, section_index) == 4u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_section_linked_start,
    offsetof(ModernGekkoRelSection, linked_start) == 8u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_section_size_field,
    offsetof(ModernGekkoRelSection, size) == 12u);

MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_module_id,
    offsetof(ModernGekkoRelModule, module_id) == 0u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_version,
    offsetof(ModernGekkoRelModule, version) == 4u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_section_count,
    offsetof(ModernGekkoRelModule, section_count) == 8u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_section_info_offset,
    offsetof(ModernGekkoRelModule, section_info_offset) == 12u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_file_size,
    offsetof(ModernGekkoRelModule, file_size) == 16u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_sections,
    offsetof(ModernGekkoRelModule, sections) == 16u + sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_num_sections,
    offsetof(ModernGekkoRelModule, num_sections) == 16u + 2u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(rel_module_size,
    sizeof(ModernGekkoRelModule) == 16u + 3u * sizeof(void*));

MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_abi_version,
    offsetof(ModernGekkoModuleDesc, abi_version) == 0u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_cpu_abi_version,
    offsetof(ModernGekkoModuleDesc, cpu_abi_version) == 4u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_cpu_state_size,
    offsetof(ModernGekkoModuleDesc, cpu_state_size) == 8u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_game_id,
    offsetof(ModernGekkoModuleDesc, game_id) == 12u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_entry_point,
    offsetof(ModernGekkoModuleDesc, entry_point) == 20u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_dispatch,
    offsetof(ModernGekkoModuleDesc, dispatch) == 24u);
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_on_state_loaded,
    offsetof(ModernGekkoModuleDesc, on_state_loaded) == 24u + sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_code_ranges,
    offsetof(ModernGekkoModuleDesc, code_ranges) == 24u + 2u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_num_code_ranges,
    offsetof(ModernGekkoModuleDesc, num_code_ranges) == 24u + 3u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_smc_ranges,
    offsetof(ModernGekkoModuleDesc, smc_ranges) == 24u + 4u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_num_smc_ranges,
    offsetof(ModernGekkoModuleDesc, num_smc_ranges) == 24u + 5u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_chunk_ranges,
    offsetof(ModernGekkoModuleDesc, chunk_ranges) == 24u + 6u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_num_chunk_ranges,
    offsetof(ModernGekkoModuleDesc, num_chunk_ranges) == 24u + 7u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_chunk_hashes,
    offsetof(ModernGekkoModuleDesc, chunk_hashes) == 24u + 8u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_rel_modules,
    offsetof(ModernGekkoModuleDesc, rel_modules) == 24u + 9u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_num_rel_modules,
    offsetof(ModernGekkoModuleDesc, num_rel_modules) == 24u + 10u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(desc_size,
    sizeof(ModernGekkoModuleDesc) == 24u + 11u * sizeof(void*));
MODERNGEKKO_ABI_LAYOUT_ASSERT(abi_version_is_v3,
    MODERNGEKKO_MODULE_ABI_VERSION == 3u);

#undef MODERNGEKKO_ABI_LAYOUT_ASSERT

/* If the vendored StaticRecompABI.h was included first it already defined
 * these names with its own (identical-layout) structs; aliasing them again
 * here would be a typedef redefinition. Both inclusion orders then work:
 * this header first -> aliases below shadow the vendored block out; vendored
 * first -> ModernGekko* and StaticRecomp* stay distinct types and any
 * reinterpret_cast between them is checked by the layout pins above. */
#ifndef STATICRECOMP_ABI_H
typedef ModernGekkoRange StaticRecompRange;
typedef ModernGekkoRelSection StaticRecompRelSection;
typedef ModernGekkoRelModule StaticRecompRelModule;
typedef ModernGekkoModuleDesc StaticRecompModuleDesc;
typedef ModernGekkoGetModuleFn StaticRecompGetModuleFn;

#define STATICRECOMP_ABI_VERSION MODERNGEKKO_MODULE_ABI_VERSION
#define STATICRECOMP_GET_MODULE_SYMBOL MODERNGEKKO_GET_MODULE_SYMBOL
#endif

#ifdef __cplusplus
}
#endif

#endif
