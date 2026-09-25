#include "moderngekko/mod_loader.hpp"

#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
#include "Common/DynamicLibrary.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#endif
#include "moderngekko/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moderngekko {
namespace {
constexpr std::uint32_t MAX_ITEMS = 1u << 20;

struct Version {
  std::array<std::uint32_t, 3> parts{};
};

bool ParseVersion(const char *text, Version *version) {
  if (!text || !*text)
    return false;
  const char *current = text;
  for (std::size_t i = 0; i < version->parts.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(*current)))
      return false;
    std::uint64_t value = 0;
    do {
      value = value * 10u + static_cast<unsigned int>(*current - '0');
      if (value > std::numeric_limits<std::uint32_t>::max())
        return false;
      ++current;
    } while (std::isdigit(static_cast<unsigned char>(*current)));
    version->parts[i] = static_cast<std::uint32_t>(value);
    if (i + 1u != version->parts.size()) {
      if (*current != '.')
        return false;
      ++current;
    }
  }
  return *current == '\0' || *current == '-' || *current == '+';
}

bool VersionAtLeast(const char *actual, const char *minimum) {
  if (!minimum || !*minimum)
    return true;
  Version actual_version;
  Version minimum_version;
  if (!ParseVersion(actual, &actual_version) ||
      !ParseVersion(minimum, &minimum_version))
    return false;
  return actual_version.parts >= minimum_version.parts;
}

bool ValidName(const char *text) {
  if (!text || !*text)
    return false;
  for (const unsigned char ch : std::string(text)) {
    if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
      return false;
  }
  return true;
}

bool ValidText(const char *text) { return text && *text; }

template <typename T> bool ValidArray(const T *values, std::uint32_t count) {
  return count <= MAX_ITEMS && (count == 0u || values != nullptr);
}

std::string EventKey(const std::string &provider, const std::string &event) {
  std::string key = provider;
  key.push_back('\0');
  key += event;
  return key;
}

std::string LibrarySuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsPackagedLibrary(const std::filesystem::path &path) {
  // Compare the file name as UTF-8: path::string() would first mangle it
  // through the Windows ANSI code page, where best-fit mapping can alias a
  // look-alike name (e.g. a fullwidth U+FF0E dot) onto the ".mgm.dll" suffix.
  const std::u8string name = path.filename().u8string();
  return EndsWith(
      std::string_view(reinterpret_cast<const char *>(name.data()),
                       name.size()),
      ".mgm" + LibrarySuffix());
}
}

struct ModManager::Impl {
  struct Mod {
    std::string source;
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
    std::unique_ptr<Common::DynamicLibrary> library;
#endif
    const ModernGekkoModDesc *descriptor = nullptr;
    bool loaded = false;
  };

  struct Hooks {
    std::vector<ModernGekkoModFunction> entry;
    std::vector<ModernGekkoModFunction> returning;
  };

  struct PendingReturn {
    std::uint32_t address = 0;
    std::uint32_t stack_pointer = 0;
    std::vector<ModernGekkoModFunction> functions;
  };

  std::vector<std::unique_ptr<Mod>> mods;
  std::vector<LoadedModInfo> loaded;
  std::unordered_map<std::string, Mod *> mods_by_id;
  std::unordered_map<std::string, ModernGekkoModFunction> exports;
  std::unordered_map<std::uint32_t, ModernGekkoModFunction> patches;
  std::unordered_map<std::uint32_t, Hooks> hooks;
  std::unordered_map<std::string, std::vector<ModernGekkoModFunction>>
      callbacks;
  std::vector<ModernGekkoModFunction *> import_slots;
  std::vector<PendingReturn> pending_returns;
  // Sorted unique set of every address carrying a hook or patch. The
  // generated-code dispatch path probes HandlesAddress/Dispatch once per
  // executed block (~millions/sec); a binary search over a contiguous array
  // replaces the two unordered_map probes on the miss (common) path.
  // Rebuilt at the end of Load() — hooks/patches never mutate afterwards —
  // and cleared by Unload(). Pending-return addresses stay dynamic and are
  // still consulted separately.
  std::vector<std::uint32_t> handled_sorted;
  bool runtime_started = false;
  // Bumped on every event that can change a dispatch verdict at runtime:
  // runtime_start's first fire, and each pending_returns push/pop (return
  // observers arm/disarm interception at their return address). The chassis
  // diffs this across a host_call to decide whether the module's pc->native
  // dcache actually needs flushing — claim-only hooks skip the epoch bump.
  std::uint64_t verdict_ops = 0;
  // Parallel ring recording WHICH pc each verdict_ops mutation affects
  // (drained by VerdictPcs): push records the intercepted return address
  // (state->lr), pop records the popped entry's address, cap-evict records
  // the evicted front address. Overflow or a non-pc event (runtime_start)
  // forces the chassis back to a global epoch bump — always conservative.
  std::uint32_t verdict_pcs[16] = {};
  std::uint32_t verdict_pcs_n = 0;
  bool verdict_pcs_overflow = false;
  void RecordVerdictPc(std::uint32_t pc) {
    if (verdict_pcs_n < 16)
      verdict_pcs[verdict_pcs_n++] = pc;
    else
      verdict_pcs_overflow = true;
  }
  ModernGekkoModHostApi host_api{};

  static std::string ExportKey(const std::string &provider,
                               const std::string &name) {
    return EventKey(provider, name);
  }

  static int TriggerEventThunk(void *user_data, const char *provider_id,
                               const char *event_name, CPUState *state) {
    if (!user_data || !provider_id || !event_name || !state)
      return 0;
    return static_cast<ModManager *>(user_data)->TriggerEvent(provider_id,
                                                              event_name, state)
               ? 1
               : 0;
  }

  static ModernGekkoModFunction FindExportThunk(void *user_data,
                                                const char *provider_id,
                                                const char *export_name) {
    if (!user_data || !provider_id || !export_name)
      return nullptr;
    return static_cast<ModManager *>(user_data)->FindExport(provider_id,
                                                            export_name);
  }
};

ModSource ModSource::DynamicPath(std::filesystem::path path) {
  ModSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModSource ModSource::AttachedDescriptor(const ModernGekkoModDesc *descriptor,
                                        std::string label) {
  ModSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  source.label = std::move(label);
  return source;
}

ModManager::ModManager() : m_impl(std::make_unique<Impl>()) {
  m_impl->host_api.abi_version = MODERNGEKKO_MOD_HOST_ABI_VERSION;
  m_impl->host_api.user_data = this;
  m_impl->host_api.trigger_event = Impl::TriggerEventThunk;
  m_impl->host_api.find_export = Impl::FindExportThunk;
}

ModManager::~ModManager() { Unload(); }

ModLoadReport ModManager::Load(const std::vector<ModSource> &sources,
                               const std::string &game_id) {
  Unload();
  ModLoadReport report;
  const auto issue = [&](const std::string &source, std::string message) {
    report.issues.push_back({source, std::move(message)});
  };

  for (std::size_t index = 0; index < sources.size(); ++index) {
    const ModSource &source = sources[index];
    auto mod = std::make_unique<Impl::Mod>();
    mod->source = source.kind == ModSource::Kind::DynamicPath
                      ? PathToUtf8(source.path)
                      : source.label;
    if (mod->source.empty())
      mod->source = "attached:" + std::to_string(index);
    if (source.kind == ModSource::Kind::AttachedDescriptor) {
      mod->descriptor = source.descriptor;
    } else {
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
      mod->library = std::make_unique<Common::DynamicLibrary>();
#if defined(_WIN32)
      // DynamicLibrary::Open is ANSI (LoadLibraryA) and can't represent a
      // mod path outside the code page; load wide and hand over the handle.
      *mod->library = reinterpret_cast<void*>(LoadLibraryW(source.path.c_str()));
      const bool library_opened = mod->library->IsOpen();
#else
      const bool library_opened = mod->library->Open(source.path.c_str());
#endif
      if (!library_opened) {
        issue(mod->source, "could not open the mod library");
        continue;
      }
      ModernGekkoGetModFn get_mod = nullptr;
      if (!mod->library->GetSymbol(MODERNGEKKO_GET_MOD_SYMBOL, &get_mod)) {
        issue(mod->source, "missing moderngekko_get_mod");
        continue;
      }
      mod->descriptor = get_mod();
#else
      issue(mod->source, "dynamic mod loading is unavailable");
      continue;
#endif
    }

    const ModernGekkoModDesc *desc = mod->descriptor;
    if (!desc)
      issue(mod->source, "null mod descriptor");
    else if (desc->abi_version != MODERNGEKKO_MOD_ABI_VERSION)
      issue(mod->source, "unsupported mod ABI");
    else if (desc->cpu_abi_version != MODERNGEKKO_CPU_ABI_VERSION ||
             desc->cpu_state_size != sizeof(CPUState))
      issue(mod->source, "CPU ABI mismatch");
    else if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) ||
             game_id != desc->game_id)
      issue(mod->source, "target game ID mismatch");
    else if (!ValidName(desc->id))
      issue(mod->source, "invalid mod ID");
    else if (!ValidText(desc->display_name))
      issue(mod->source, "missing display name");
    else {
      Version version;
      if (!ParseVersion(desc->version, &version))
        issue(mod->source, "invalid mod version");
      else if (!ValidArray(desc->dependencies, desc->num_dependencies) ||
               !ValidArray(desc->patches, desc->num_patches) ||
               !ValidArray(desc->hooks, desc->num_hooks) ||
               !ValidArray(desc->exports, desc->num_exports) ||
               !ValidArray(desc->imports, desc->num_imports) ||
               !ValidArray(desc->events, desc->num_events) ||
               !ValidArray(desc->callbacks, desc->num_callbacks))
        issue(mod->source, "invalid descriptor arrays");
      else if (m_impl->mods_by_id.contains(desc->id)) {
        // A second copy of the same mod (e.g. the default Mods dir plus an
        // explicit --mods dir, or a stale copy in user-dir) is redundant, not
        // an error: keep the first discovery-order copy and skip this one.
        // Recording this as an issue would tear down the entire mod set.
        std::fprintf(stderr, "mod skipped: %s: duplicate mod ID\n",
                     mod->source.c_str());
      }
      else {
        m_impl->mods_by_id.emplace(desc->id, mod.get());
        m_impl->mods.push_back(std::move(mod));
      }
    }
  }

  if (!report.issues.empty()) {
    Unload();
    return report;
  }

  std::unordered_map<Impl::Mod *, std::size_t> mod_indices;
  for (std::size_t i = 0; i < m_impl->mods.size(); ++i)
    mod_indices.emplace(m_impl->mods[i].get(), i);
  std::vector<std::vector<std::size_t>> edges(m_impl->mods.size());
  std::vector<std::size_t> indegree(m_impl->mods.size());

  for (std::size_t i = 0; i < m_impl->mods.size(); ++i) {
    const auto &mod = m_impl->mods[i];
    const auto *desc = mod->descriptor;
    std::unordered_set<std::string> seen;
    for (std::uint32_t d = 0; d < desc->num_dependencies; ++d) {
      const auto &dependency = desc->dependencies[d];
      if (!ValidName(dependency.id) || !seen.emplace(dependency.id).second) {
        issue(mod->source, "invalid or duplicate dependency");
        continue;
      }
      const auto found = m_impl->mods_by_id.find(dependency.id);
      if (found == m_impl->mods_by_id.end()) {
        if (!dependency.optional)
          issue(mod->source,
                "missing dependency " + std::string(dependency.id));
        continue;
      }
      if (!VersionAtLeast(found->second->descriptor->version,
                          dependency.minimum_version)) {
        issue(mod->source, "dependency version is too old for " +
                               std::string(dependency.id));
        continue;
      }
      const std::size_t dependency_index = mod_indices.at(found->second);
      edges[dependency_index].push_back(i);
      ++indegree[i];
    }
  }

  if (!report.issues.empty()) {
    Unload();
    return report;
  }

  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>>
      ready;
  for (std::size_t i = 0; i < indegree.size(); ++i) {
    if (indegree[i] == 0)
      ready.push(i);
  }
  std::vector<std::size_t> order;
  order.reserve(m_impl->mods.size());
  while (!ready.empty()) {
    const std::size_t index = ready.top();
    ready.pop();
    for (const std::size_t dependent : edges[index]) {
      if (--indegree[dependent] == 0)
        ready.push(dependent);
    }
    order.push_back(index);
  }
  if (order.size() != m_impl->mods.size()) {
    issue("mods", "dependency cycle");
    Unload();
    return report;
  }
  std::vector<std::unique_ptr<Impl::Mod>> ordered;
  ordered.reserve(m_impl->mods.size());
  for (const std::size_t index : order)
    ordered.push_back(std::move(m_impl->mods[index]));
  m_impl->mods = std::move(ordered);
  m_impl->mods_by_id.clear();
  for (const auto &mod : m_impl->mods)
    m_impl->mods_by_id.emplace(mod->descriptor->id, mod.get());

  std::unordered_set<std::string> event_names;
  event_names.emplace(EventKey("*", "runtime_start"));
  for (const auto &mod : m_impl->mods) {
    const auto *desc = mod->descriptor;
    std::unordered_set<std::string> local_exports;
    std::unordered_set<std::string> local_events;
    for (std::uint32_t i = 0; i < desc->num_exports; ++i) {
      const auto &entry = desc->exports[i];
      if (!ValidName(entry.name) || !entry.function ||
          !local_exports.emplace(entry.name).second) {
        issue(mod->source, "invalid or duplicate export");
        continue;
      }
      m_impl->exports.emplace(Impl::ExportKey(desc->id, entry.name),
                              entry.function);
    }
    for (std::uint32_t i = 0; i < desc->num_events; ++i) {
      const auto &event = desc->events[i];
      if (!ValidName(event.name) || !local_events.emplace(event.name).second) {
        issue(mod->source, "invalid or duplicate event");
        continue;
      }
      event_names.emplace(EventKey(desc->id, event.name));
    }
  }

  for (const auto &mod : m_impl->mods) {
    const auto *desc = mod->descriptor;
    for (std::uint32_t i = 0; i < desc->num_imports; ++i) {
      const auto &entry = desc->imports[i];
      if (!ValidName(entry.dependency_id) || !ValidName(entry.name) ||
          !entry.slot) {
        issue(mod->source, "invalid import");
        continue;
      }
      const ModernGekkoModDependency *declared = nullptr;
      for (std::uint32_t d = 0; d < desc->num_dependencies; ++d) {
        if (std::strcmp(desc->dependencies[d].id, entry.dependency_id) == 0) {
          declared = &desc->dependencies[d];
          break;
        }
      }
      if (!declared) {
        issue(mod->source, "import provider is not a declared dependency");
        continue;
      }
      const auto dependency = m_impl->mods_by_id.find(entry.dependency_id);
      const auto function = m_impl->exports.find(
          Impl::ExportKey(entry.dependency_id, entry.name));
      if (dependency == m_impl->mods_by_id.end()) {
        *entry.slot = nullptr;
      } else if (function == m_impl->exports.end()) {
        issue(mod->source, "missing export " +
                               std::string(entry.dependency_id) + ":" +
                               entry.name);
      } else {
        *entry.slot = function->second;
      }
      m_impl->import_slots.push_back(entry.slot);
    }
    for (std::uint32_t i = 0; i < desc->num_patches; ++i) {
      const auto &patch = desc->patches[i];
      if ((patch.address & 3u) != 0u || !patch.function ||
          (patch.flags & ~MODERNGEKKO_MOD_PATCH_FORCE) != 0u) {
        issue(mod->source, "invalid patch");
        continue;
      }
      const auto found = m_impl->patches.find(patch.address);
      if (found != m_impl->patches.end() &&
          (patch.flags & MODERNGEKKO_MOD_PATCH_FORCE) == 0u) {
        issue(mod->source,
              "patch conflict at " + std::to_string(patch.address));
        continue;
      }
      m_impl->patches[patch.address] = patch.function;
    }
    for (std::uint32_t i = 0; i < desc->num_hooks; ++i) {
      const auto &hook = desc->hooks[i];
      if ((hook.address & 3u) != 0u || !hook.function ||
          hook.kind > MODERNGEKKO_MOD_HOOK_RETURN) {
        issue(mod->source, "invalid hook");
        continue;
      }
      auto &hooks = m_impl->hooks[hook.address];
      if (hook.kind == MODERNGEKKO_MOD_HOOK_ENTRY)
        hooks.entry.push_back(hook.function);
      else
        hooks.returning.push_back(hook.function);
    }
    for (std::uint32_t i = 0; i < desc->num_callbacks; ++i) {
      const auto &callback = desc->callbacks[i];
      const std::string provider =
          callback.dependency_id &&
                  std::strcmp(callback.dependency_id, ".") == 0
              ? desc->id
          : callback.dependency_id ? callback.dependency_id
                                   : "";
      if ((!ValidName(provider.c_str()) && provider != "*") ||
          !ValidName(callback.event_name) || !callback.function) {
        issue(mod->source, "invalid callback");
        continue;
      }
      if (provider != desc->id && provider != "*") {
        bool declared = false;
        for (std::uint32_t d = 0; d < desc->num_dependencies; ++d)
          declared |= provider == desc->dependencies[d].id;
        if (!declared) {
          issue(mod->source, "callback provider is not a declared dependency");
          continue;
        }
      }
      const std::string key = EventKey(provider, callback.event_name);
      if (!event_names.contains(key)) {
        issue(mod->source,
              "missing event " + provider + ":" + callback.event_name);
        continue;
      }
      m_impl->callbacks[key].push_back(callback.function);
    }
  }

  if (!report.issues.empty()) {
    Unload();
    return report;
  }

  // Materialize the flat handled-address set now that hooks/patches are
  // final for this load generation.
  m_impl->handled_sorted.clear();
  m_impl->handled_sorted.reserve(m_impl->hooks.size() +
                                 m_impl->patches.size());
  for (const auto &entry : m_impl->hooks)
    m_impl->handled_sorted.push_back(entry.first);
  for (const auto &entry : m_impl->patches)
    m_impl->handled_sorted.push_back(entry.first);
  std::sort(m_impl->handled_sorted.begin(), m_impl->handled_sorted.end());
  m_impl->handled_sorted.erase(
      std::unique(m_impl->handled_sorted.begin(),
                  m_impl->handled_sorted.end()),
      m_impl->handled_sorted.end());
  m_impl->handled_sorted.shrink_to_fit();

  for (const auto &mod : m_impl->mods) {
    const auto *desc = mod->descriptor;
    m_impl->loaded.push_back(
        {desc->id, desc->version, desc->display_name, mod->source});
    if (desc->on_load)
      desc->on_load(&m_impl->host_api);
    mod->loaded = true;
  }
  report.loaded = m_impl->loaded;
  return report;
}

ModLoadReport ModManager::LoadDirectories(
    const std::vector<std::filesystem::path> &directories,
    const std::string &game_id) {
  ModLoadReport discovery;
  std::vector<ModSource> sources =
      DiscoverModSources(directories, &discovery.issues);
  if (!discovery.issues.empty())
    return discovery;
  return Load(sources, game_id);
}

std::vector<ModSource>
DiscoverModSources(const std::vector<std::filesystem::path> &directories,
                   std::vector<ModLoadIssue> *issues) {
  std::vector<std::filesystem::path> paths;
  const auto issue = [&](const std::filesystem::path &path,
                         std::string message) {
    if (issues)
      issues->push_back({PathToUtf8(path), std::move(message)});
  };
  for (const auto &directory : directories) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
      continue;
    if (std::filesystem::is_regular_file(directory, ec)) {
      paths.push_back(directory);
      continue;
    }
    if (!std::filesystem::is_directory(directory, ec)) {
      issue(directory, "mod path is not a directory or library");
      continue;
    }
    if (directory.extension() == ".mgm") {
      const auto library = directory / ("mod" + LibrarySuffix());
      if (std::filesystem::is_regular_file(library, ec))
        paths.push_back(library);
      else
        issue(directory, "package has no platform mod library");
      continue;
    }
    for (std::filesystem::directory_iterator it(directory, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec) && it->path().extension() == ".mgm") {
        const auto library = it->path() / ("mod" + LibrarySuffix());
        if (std::filesystem::is_regular_file(library, ec))
          paths.push_back(library);
        else
          issue(it->path(), "package has no platform mod library");
      } else if (it->is_regular_file(ec) && IsPackagedLibrary(it->path())) {
        paths.push_back(it->path());
      }
    }
    if (ec)
      issue(directory, "could not enumerate mod directory");
  }
  // Dedup on canonical paths, not spelling: the same directory reached via a
  // relative --mods plus the absolute default Mods dir must collapse to one
  // entry, otherwise the same mod.dll registers twice and the duplicate-ID
  // issue tears down the entire mod set at load.
  for (auto &path : paths)
  {
    std::error_code canonical_ec;
    auto canonical = std::filesystem::weakly_canonical(path, canonical_ec);
    if (!canonical_ec)
      path = std::move(canonical);
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  std::vector<ModSource> sources;
  sources.reserve(paths.size());
  for (auto &path : paths)
    sources.push_back(ModSource::DynamicPath(std::move(path)));
  return sources;
}

void ModManager::Unload() {
  for (auto it = m_impl->mods.rbegin(); it != m_impl->mods.rend(); ++it) {
    if ((*it) && (*it)->loaded && (*it)->descriptor &&
        (*it)->descriptor->on_unload)
      (*it)->descriptor->on_unload();
  }
  for (ModernGekkoModFunction *slot : m_impl->import_slots) {
    if (slot)
      *slot = nullptr;
  }
  m_impl->pending_returns.clear();
  m_impl->handled_sorted.clear();
  m_impl->callbacks.clear();
  m_impl->hooks.clear();
  m_impl->patches.clear();
  m_impl->exports.clear();
  m_impl->mods_by_id.clear();
  m_impl->loaded.clear();
  m_impl->import_slots.clear();
  m_impl->mods.clear();
  m_impl->runtime_started = false;
}

bool ModManager::Dispatch(CPUState *state, std::uint32_t address) {
  if (!state)
    return false;
  if (!m_impl->runtime_started) {
    m_impl->runtime_started = true;
    ++m_impl->verdict_ops;
    m_impl->verdict_pcs_overflow = true;  // non-pc event -> global bump
    TriggerEvent("*", "runtime_start", state);
  }
  if (!m_impl->pending_returns.empty() &&
      m_impl->pending_returns.back().address == address &&
      m_impl->pending_returns.back().stack_pointer == state->gpr[1]) {
    auto pending = std::move(m_impl->pending_returns.back());
    m_impl->RecordVerdictPc(pending.address);
    m_impl->pending_returns.pop_back();
    ++m_impl->verdict_ops;
    for (auto it = pending.functions.rbegin(); it != pending.functions.rend();
         ++it) {
      const CPUState saved = *state;
      (*it)(state);
      *state = saved;
    }
  }
  // Flat reject: no hook and no patch at this address. The pending-return
  // block above must stay first — return-observer addresses are tracked
  // dynamically and are NOT part of handled_sorted.
  if (m_impl->handled_sorted.empty() ||
      !std::binary_search(m_impl->handled_sorted.begin(),
                          m_impl->handled_sorted.end(), address)) {
    return false;
  }
  const auto hooks = m_impl->hooks.find(address);
  if (hooks != m_impl->hooks.end()) {
    // Option B accumulator (frame60-impl-plan.md §4, arbitration Q3 D11):
    // Entry hooks are mutating pre-hooks so the 30Hz logic gates
    // (fpcM_Execute 0x8003E370, fapGm_After 0x800231BC) can skip via
    // `state->pc = state->lr`. Return hooks and event callbacks remain
    // observer-only (save/restore). See FINDING A / entry-hook-persist-decision.md.
    for (ModernGekkoModFunction function : hooks->second.entry) {
      function(state);
    }
    if (!hooks->second.returning.empty()) {
      // ROOTCAUSE-FIX (A): dedup pending-return pushes. The pre-fix code
      // pushed {state->lr, state->gpr[1], returning} unconditionally on every
      // Dispatch visit to a returning-hook address. The pop above only ever
      // consumes the BACK of the stack, and only when the guest re-dispatches
      // at the exact {return address, stack pointer} pair, so a re-dispatch
      // of the same guest visit — the hook-funnel livelock re-fires the same
      // visit ~16x with no guest progress in between (FIX-REVIEW "Top
      // residual risk 1") — stacked identical unreachable entries: each twin
      // later fired the return observers again on a matching dispatch (mod
      // state corruption in the render/Present path), the 4096-cap eviction
      // churned verdict_ops/RecordVerdictPc (dispatch-epoch poisoning), and
      // stale twins kept HandlesAddress() true at the dead return address.
      // Invariant restored here: at most one pending entry may exist per
      // live {return address, stack pointer} pair — exactly one per guest
      // return event. A second push at an identical pair while the first is
      // still pending cannot be a new call: a genuine re-entry has a
      // different stack pointer while nested, and a sibling call at the same
      // pair can only occur after the previous entry was popped above.
      // (Diverted/stale returns whose twin is never popped — the frame60
      // "pending_returns saturation" case — now over-fire once instead of
      // twice at the eventual matching dispatch; the mod-side frame reset
      // that clears stale entries is unaffected.) The patch lookup below is
      // intentionally reached in the duplicate case exactly as before.
      const bool duplicate_pending = std::ranges::any_of(
          m_impl->pending_returns, [&](const Impl::PendingReturn &pending) {
            return pending.address == state->lr &&
                   pending.stack_pointer == state->gpr[1];
          });
      if (!duplicate_pending) {
        if (m_impl->pending_returns.size() >= 4096u) {
          m_impl->RecordVerdictPc(m_impl->pending_returns.front().address);
          m_impl->pending_returns.erase(m_impl->pending_returns.begin());
          ++m_impl->verdict_ops;
        }
        m_impl->pending_returns.push_back(
            {state->lr, state->gpr[1], hooks->second.returning});
        m_impl->RecordVerdictPc(state->lr);
        ++m_impl->verdict_ops;
      }
    }
  }
  const auto patch = m_impl->patches.find(address);
  if (patch == m_impl->patches.end())
    return false;
  patch->second(state);
  return true;
}

bool ModManager::TriggerEvent(const std::string &provider_id,
                              const std::string &event_name, CPUState *state) {
  if (!state)
    return false;
  const auto found = m_impl->callbacks.find(EventKey(provider_id, event_name));
  if (found == m_impl->callbacks.end())
    return false;
  const CPUState saved = *state;
  for (ModernGekkoModFunction function : found->second) {
    function(state);
    *state = saved;
  }
  return true;
}

ModernGekkoModFunction
ModManager::FindExport(const std::string &provider_id,
                       const std::string &export_name) const {
  const auto found =
      m_impl->exports.find(Impl::ExportKey(provider_id, export_name));
  return found == m_impl->exports.end() ? nullptr : found->second;
}

const std::vector<LoadedModInfo> &ModManager::GetLoadedMods() const {
  return m_impl->loaded;
}

bool ModManager::HandlesAddress(std::uint32_t address) const {
  // Membership in the sorted set is exactly "hooks.contains || patches
  // .contains"; pending-return addresses remain a dynamic side channel.
  if (std::binary_search(m_impl->handled_sorted.begin(),
                         m_impl->handled_sorted.end(), address))
    return true;
  return std::ranges::any_of(m_impl->pending_returns,
                             [address](const Impl::PendingReturn &pending) {
                               return pending.address == address;
                             });
}

bool ModManager::HandlesRange(std::uint32_t start, std::uint32_t end) const {
  if (start >= end)
    return false;
  // First handled_sorted element >= start is inside [start, end) iff the
  // union set intersects the range — identical to the previous any_of over
  // both hash tables.
  const auto it =
      std::lower_bound(m_impl->handled_sorted.begin(),
                       m_impl->handled_sorted.end(), start);
  if (it != m_impl->handled_sorted.end() && *it < end)
    return true;
  return std::ranges::any_of(
      m_impl->pending_returns, [start, end](const Impl::PendingReturn &pending) {
        return pending.address >= start && pending.address < end;
      });
}

bool ModManager::Empty() const { return m_impl->mods.empty(); }

bool ModManager::HostCall(CPUState *state, std::uint32_t address,
                          void *user_data) {
  return user_data &&
         static_cast<ModManager *>(user_data)->Dispatch(state, address);
}

bool ModManager::HostCallContains(std::uint32_t address, void *user_data) {
  return user_data &&
         static_cast<ModManager *>(user_data)->HandlesAddress(address);
}

std::uint64_t ModManager::VerdictOps(void *user_data) {
  return user_data ? static_cast<ModManager *>(user_data)->m_impl->verdict_ops
                   : 0;
}

std::uint32_t ModManager::VerdictPcs(void *user_data, std::uint32_t *out,
                                     std::uint32_t cap) {
  if (!user_data)
    return ~0u;
  auto *impl = static_cast<ModManager *>(user_data)->m_impl.get();
  if (!impl)
    return ~0u;
  // A caller with cap < n would silently drop tail invalidations while the
  // ring resets — treat it like an overflow so nothing is under-invalidated.
  if (!out || impl->verdict_pcs_overflow || cap < impl->verdict_pcs_n) {
    impl->verdict_pcs_overflow = false;
    impl->verdict_pcs_n = 0;
    return ~0u;
  }
  std::memcpy(out, impl->verdict_pcs,
              impl->verdict_pcs_n * sizeof(std::uint32_t));
  const std::uint32_t n = impl->verdict_pcs_n;
  impl->verdict_pcs_n = 0;
  return n;
}

bool ModManager::HostCallRangeContains(std::uint32_t start, std::uint32_t end,
                                       void *user_data) {
  return user_data &&
         static_cast<ModManager *>(user_data)->HandlesRange(start, end);
}
}
