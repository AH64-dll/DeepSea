#ifndef MODERNGEKKO_MOD_ABI_H
#define MODERNGEKKO_MOD_ABI_H

#include "moderngekko/cpu_state.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODERNGEKKO_MOD_ABI_VERSION 1u
#define MODERNGEKKO_MOD_HOST_ABI_VERSION 1u
#define MODERNGEKKO_GET_MOD_SYMBOL "moderngekko_get_mod"

#if defined(_WIN32)
#define MODERNGEKKO_MOD_VISIBILITY __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define MODERNGEKKO_MOD_VISIBILITY __attribute__((visibility("default")))
#else
#define MODERNGEKKO_MOD_VISIBILITY
#endif

#if defined(__cplusplus)
#define MODERNGEKKO_MOD_EXPORT extern "C" MODERNGEKKO_MOD_VISIBILITY
#else
#define MODERNGEKKO_MOD_EXPORT MODERNGEKKO_MOD_VISIBILITY
#endif

typedef void (*ModernGekkoModFunction)(CPUState* state);

typedef enum ModernGekkoModPatchFlags
{
    MODERNGEKKO_MOD_PATCH_NONE = 0,
    MODERNGEKKO_MOD_PATCH_FORCE = 1
} ModernGekkoModPatchFlags;

typedef enum ModernGekkoModHookKind
{
    MODERNGEKKO_MOD_HOOK_ENTRY = 0,
    MODERNGEKKO_MOD_HOOK_RETURN = 1
} ModernGekkoModHookKind;

typedef struct ModernGekkoModDependency
{
    const char* id;
    const char* minimum_version;
    uint32_t optional;
} ModernGekkoModDependency;

typedef struct ModernGekkoModPatch
{
    uint32_t address;
    ModernGekkoModFunction function;
    uint32_t flags;
} ModernGekkoModPatch;

typedef struct ModernGekkoModHook
{
    uint32_t address;
    ModernGekkoModFunction function;
    uint32_t kind;
} ModernGekkoModHook;

typedef struct ModernGekkoModExportEntry
{
    const char* name;
    ModernGekkoModFunction function;
} ModernGekkoModExportEntry;

typedef struct ModernGekkoModImportEntry
{
    const char* dependency_id;
    const char* name;
    ModernGekkoModFunction* slot;
} ModernGekkoModImportEntry;

typedef struct ModernGekkoModEvent
{
    const char* name;
} ModernGekkoModEvent;

typedef struct ModernGekkoModCallback
{
    const char* dependency_id;
    const char* event_name;
    ModernGekkoModFunction function;
} ModernGekkoModCallback;

typedef struct ModernGekkoModHostApi
{
    uint32_t abi_version;
    void* user_data;
    int (*trigger_event)(void* user_data, const char* provider_id,
                         const char* event_name, CPUState* state);
    ModernGekkoModFunction (*find_export)(void* user_data,
                                          const char* provider_id,
                                          const char* export_name);
} ModernGekkoModHostApi;

typedef struct ModernGekkoModDesc
{
    uint32_t abi_version;
    uint32_t cpu_abi_version;
    uint32_t cpu_state_size;
    char game_id[8];
    const char* id;
    const char* version;
    const char* display_name;
    const ModernGekkoModDependency* dependencies;
    uint32_t num_dependencies;
    const ModernGekkoModPatch* patches;
    uint32_t num_patches;
    const ModernGekkoModHook* hooks;
    uint32_t num_hooks;
    const ModernGekkoModExportEntry* exports;
    uint32_t num_exports;
    const ModernGekkoModImportEntry* imports;
    uint32_t num_imports;
    const ModernGekkoModEvent* events;
    uint32_t num_events;
    const ModernGekkoModCallback* callbacks;
    uint32_t num_callbacks;
    void (*on_load)(const ModernGekkoModHostApi* api);
    void (*on_unload)(void);
} ModernGekkoModDesc;

typedef const ModernGekkoModDesc* (*ModernGekkoGetModFn)(void);

/* Wire-layout pins. A .mgm mod and the chassis each compile this header;
 * abi_version guards intentional changes at load, but a field edit without
 * a version bump on EITHER side must fail to compile somewhere — these
 * asserts are that somewhere. Offsets are expressed in sizeof(void*) so the
 * same constants hold on 32- and 64-bit hosts (function pointers are
 * pointer-sized on every supported target). */
#if defined(__cplusplus)
#define MODERNGEKKO_MOD_LAYOUT_ASSERT(name, expr) \
    static_assert(expr, "ModernGekko mod ABI layout drift: " #name)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MODERNGEKKO_MOD_LAYOUT_ASSERT(name, expr) \
    _Static_assert(expr, "ModernGekko mod ABI layout drift: " #name)
#else
#define MODERNGEKKO_MOD_LAYOUT_ASSERT(name, expr) \
    typedef char moderngekko_mod_layout_##name[(expr) ? 1 : -1]
#endif

MODERNGEKKO_MOD_LAYOUT_ASSERT(dependency_id,
    offsetof(ModernGekkoModDependency, id) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(dependency_minimum_version,
    offsetof(ModernGekkoModDependency, minimum_version) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(dependency_optional,
    offsetof(ModernGekkoModDependency, optional) == 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(dependency_size,
    sizeof(ModernGekkoModDependency) == 3u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(patch_address,
    offsetof(ModernGekkoModPatch, address) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(patch_function,
    offsetof(ModernGekkoModPatch, function) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(patch_flags,
    offsetof(ModernGekkoModPatch, flags) == 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(patch_size,
    sizeof(ModernGekkoModPatch) == 3u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(hook_address,
    offsetof(ModernGekkoModHook, address) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(hook_function,
    offsetof(ModernGekkoModHook, function) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(hook_kind,
    offsetof(ModernGekkoModHook, kind) == 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(hook_size,
    sizeof(ModernGekkoModHook) == 3u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(export_name,
    offsetof(ModernGekkoModExportEntry, name) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(export_function,
    offsetof(ModernGekkoModExportEntry, function) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(export_size,
    sizeof(ModernGekkoModExportEntry) == 2u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(import_dependency_id,
    offsetof(ModernGekkoModImportEntry, dependency_id) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(import_name,
    offsetof(ModernGekkoModImportEntry, name) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(import_slot,
    offsetof(ModernGekkoModImportEntry, slot) == 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(import_size,
    sizeof(ModernGekkoModImportEntry) == 3u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(event_name,
    offsetof(ModernGekkoModEvent, name) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(event_size,
    sizeof(ModernGekkoModEvent) == sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(callback_dependency_id,
    offsetof(ModernGekkoModCallback, dependency_id) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(callback_event_name,
    offsetof(ModernGekkoModCallback, event_name) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(callback_function,
    offsetof(ModernGekkoModCallback, function) == 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(callback_size,
    sizeof(ModernGekkoModCallback) == 3u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(host_api_abi_version,
    offsetof(ModernGekkoModHostApi, abi_version) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(host_api_user_data,
    offsetof(ModernGekkoModHostApi, user_data) == sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(host_api_trigger_event,
    offsetof(ModernGekkoModHostApi, trigger_event) == 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(host_api_find_export,
    offsetof(ModernGekkoModHostApi, find_export) == 3u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(host_api_size,
    sizeof(ModernGekkoModHostApi) == 4u * sizeof(void*));

MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_abi_version,
    offsetof(ModernGekkoModDesc, abi_version) == 0u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_cpu_abi_version,
    offsetof(ModernGekkoModDesc, cpu_abi_version) == 4u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_cpu_state_size,
    offsetof(ModernGekkoModDesc, cpu_state_size) == 8u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_game_id,
    offsetof(ModernGekkoModDesc, game_id) == 12u);
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_id,
    offsetof(ModernGekkoModDesc, id) == 16u + sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_version,
    offsetof(ModernGekkoModDesc, version) == 16u + 2u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_display_name,
    offsetof(ModernGekkoModDesc, display_name) == 16u + 3u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_dependencies,
    offsetof(ModernGekkoModDesc, dependencies) == 16u + 4u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_dependencies,
    offsetof(ModernGekkoModDesc, num_dependencies) == 16u + 5u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_patches,
    offsetof(ModernGekkoModDesc, patches) == 16u + 6u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_patches,
    offsetof(ModernGekkoModDesc, num_patches) == 16u + 7u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_hooks,
    offsetof(ModernGekkoModDesc, hooks) == 16u + 8u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_hooks,
    offsetof(ModernGekkoModDesc, num_hooks) == 16u + 9u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_exports,
    offsetof(ModernGekkoModDesc, exports) == 16u + 10u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_exports,
    offsetof(ModernGekkoModDesc, num_exports) == 16u + 11u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_imports,
    offsetof(ModernGekkoModDesc, imports) == 16u + 12u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_imports,
    offsetof(ModernGekkoModDesc, num_imports) == 16u + 13u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_events,
    offsetof(ModernGekkoModDesc, events) == 16u + 14u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_events,
    offsetof(ModernGekkoModDesc, num_events) == 16u + 15u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_callbacks,
    offsetof(ModernGekkoModDesc, callbacks) == 16u + 16u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_num_callbacks,
    offsetof(ModernGekkoModDesc, num_callbacks) == 16u + 17u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_on_load,
    offsetof(ModernGekkoModDesc, on_load) == 16u + 18u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_on_unload,
    offsetof(ModernGekkoModDesc, on_unload) == 16u + 19u * sizeof(void*));
MODERNGEKKO_MOD_LAYOUT_ASSERT(desc_size,
    sizeof(ModernGekkoModDesc) == 16u + 20u * sizeof(void*));

#undef MODERNGEKKO_MOD_LAYOUT_ASSERT

#define RECOMP_PATCH(address, function) \
    { (address), (function), MODERNGEKKO_MOD_PATCH_NONE }
#define RECOMP_FORCE_PATCH(address, function) \
    { (address), (function), MODERNGEKKO_MOD_PATCH_FORCE }
#define RECOMP_HOOK(address, function) \
    { (address), (function), MODERNGEKKO_MOD_HOOK_ENTRY }
#define RECOMP_HOOK_RETURN(address, function) \
    { (address), (function), MODERNGEKKO_MOD_HOOK_RETURN }
#define RECOMP_EXPORT(name, function) { (name), (function) }
#define RECOMP_IMPORT(mod, name, slot) { (mod), (name), (slot) }
#define RECOMP_DECLARE_EVENT(name) { (name) }
#define RECOMP_CALLBACK(mod, event, function) { (mod), (event), (function) }

static inline uint32_t moderngekko_mod_arg_u32(const CPUState* state,
                                                uint32_t index)
{
    return index < 8u ? state->gpr[3u + index] : 0u;
}

static inline void moderngekko_mod_return_u32(CPUState* state, uint32_t value)
{
    state->gpr[3] = value;
    state->pc = state->lr;
}

static inline uint64_t moderngekko_mod_read(CPUState* state, uint32_t address,
                                            uint8_t size)
{
    return state->external_read ? state->external_read(state, address, size) : 0u;
}

static inline void moderngekko_mod_write(CPUState* state, uint32_t address,
                                         uint64_t value, uint8_t size)
{
    if (state->external_write)
        state->external_write(state, address, value, size);
}

#ifdef __cplusplus
}
#endif

#endif
