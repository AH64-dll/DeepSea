#include "moderngekko/module_loader.hpp"

#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
#include "Common/DynamicLibrary.h"
#endif
#if defined(_WIN32)
#include "moderngekko/utf8_path.hpp"
#include <windows.h>
#endif

namespace moderngekko
{
struct ModuleLibrary::Impl
{
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
  Common::DynamicLibrary library;
#endif
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

ModuleLibrary::ModuleLibrary() : m_impl(std::make_unique<Impl>())
{
}
ModuleLibrary::~ModuleLibrary() = default;

ModuleLoadResult ModuleLibrary::Open(
    const std::string& path, const ModernGekkoModuleRequirements& requirements)
{
  Close();

#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
  // `path` is UTF-8, matching the convention used for every path string in
  // the tree. Common::DynamicLibrary::Open goes through LoadLibraryA on
  // Windows, which would misdecode it, so load via the wide API there and
  // hand the handle over (DynamicLibrary accepts a raw handle). Utf8ToPath
  // is a local helper: this target deliberately does not link the vendored
  // Dolphin core lib just for StringToPath.
#if defined(_WIN32)
  m_impl->library =
      reinterpret_cast<void*>(LoadLibraryW(Utf8ToPath(path).c_str()));
  const bool opened = m_impl->library.IsOpen();
#else
  const bool opened = m_impl->library.Open(path.c_str());
#endif
  if (!opened)
    return {ModuleLoadStatus::LibraryOpenFailed, MODERNGEKKO_MODULE_OK};

  ModernGekkoGetModuleFn get_module = nullptr;
  if (!m_impl->library.GetSymbol(MODERNGEKKO_GET_MODULE_SYMBOL, &get_module))
  {
    Close();
    return {ModuleLoadStatus::EntryPointMissing, MODERNGEKKO_MODULE_OK};
  }

  const ModernGekkoModuleDesc* descriptor = get_module();
  const ModernGekkoModuleStatus validation =
      moderngekko_validate_module(descriptor, &requirements);
  if (validation != MODERNGEKKO_MODULE_OK)
  {
    Close();
    return {ModuleLoadStatus::DescriptorRejected, validation};
  }

  m_impl->descriptor = descriptor;
  return {ModuleLoadStatus::Ok, MODERNGEKKO_MODULE_OK};
#else
  (void)path;
  (void)requirements;
  return {ModuleLoadStatus::DynamicLoadingUnavailable, MODERNGEKKO_MODULE_OK};
#endif
}

ModuleLoadResult ModuleLibrary::Attach(
    const ModernGekkoModuleDesc* descriptor,
    const ModernGekkoModuleRequirements& requirements)
{
  Close();
  const ModernGekkoModuleStatus validation =
      moderngekko_validate_module(descriptor, &requirements);
  if (validation != MODERNGEKKO_MODULE_OK)
    return {ModuleLoadStatus::DescriptorRejected, validation};
  m_impl->descriptor = descriptor;
  return {ModuleLoadStatus::Ok, MODERNGEKKO_MODULE_OK};
}

void ModuleLibrary::Close()
{
  m_impl->descriptor = nullptr;
#if defined(MODERNGEKKO_ENABLE_DYNAMIC_MODULES)
  m_impl->library.Close();
#endif
}

bool ModuleLibrary::IsOpen() const
{
  return m_impl->descriptor != nullptr;
}

const ModernGekkoModuleDesc* ModuleLibrary::GetDescriptor() const
{
  return m_impl->descriptor;
}
}
