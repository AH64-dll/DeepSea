#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
// After windows.h: shellapi.h uses EXTERN_C/DECLSPEC_IMPORT/DECLARE_HANDLE.
#include <windows.h>
#include <shellapi.h>
#endif

namespace moderngekko
{

#if defined(_WIN32)
// Wide form of a UTF-8 string for the *W Win32 APIs (CreateProcessW,
// LoadLibraryW, ...). Empty on failure, matching Utf8ToPath below.
inline std::wstring Utf8ToWide(std::string_view utf8)
{
  if (utf8.empty())
    return {};
  const int input_size = static_cast<int>(utf8.size());
  const int wide_size =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), input_size, nullptr, 0);
  if (wide_size <= 0)
    return {};
  std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), input_size, wide.data(),
                          wide_size) != wide_size)
    return {};
  return wide;
}

// Inverse of Utf8ToWide. Empty on failure.
inline std::string WideToUtf8(std::wstring_view wide)
{
  if (wide.empty())
    return {};
  const int input_size = static_cast<int>(wide.size());
  const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), input_size,
                                          nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0)
    return {};
  std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), input_size, utf8.data(),
                          utf8_size, nullptr, nullptr) != utf8_size)
    return {};
  return utf8;
}
#endif

// Converts a UTF-8 path string to the native filesystem encoding. On Windows
// the narrow fs::path constructor would decode through the ANSI code page and
// could mangle a UTF-8 path onto a different existing file, so convert
// through UTF-16 instead. Everywhere else the native narrow encoding is
// already UTF-8. Mirrors the vendored Dolphin StringToPath semantics
// (permissive flags, empty result on failure) so targets that deliberately
// do not link the Dolphin core lib can share one implementation.
inline std::filesystem::path Utf8ToPath(std::string_view utf8)
{
#if defined(_WIN32)
  return std::filesystem::path(Utf8ToWide(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

// Inverse of Utf8ToPath: the path's UTF-8 form for storing and displaying.
inline std::string PathToUtf8(const std::filesystem::path& path)
{
  const std::u8string encoded = path.u8string();
  return {encoded.begin(), encoded.end()};
}

// Reads an environment variable as a filesystem path. POSIX getenv bytes are
// already the native path encoding, but on Windows the CRT's getenv returns
// the ANSI code page, which cannot represent every profile name -- this goes
// through _wgetenv and the wide path constructor there instead.
inline std::optional<std::filesystem::path> GetEnvPath(const char* name)
{
#if defined(_WIN32)
  // Variable names are ASCII; widen them for _wgetenv.
  std::wstring wide;
  for (const char* p = name; *p; ++p)
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
  if (const wchar_t* value = _wgetenv(wide.c_str()))
    return std::filesystem::path(value);
  return std::nullopt;
#else
  if (const char* value = std::getenv(name))
    return std::filesystem::path(value);
  return std::nullopt;
#endif
}

#if defined(_WIN32)
// Rebuilds argv as UTF-8: the CRT's narrow argv is encoded in the ANSI code
// page, which cannot represent paths outside it. Returns empty on failure;
// callers should fall back to the CRT argv rather than lose the process.
// Requires linking shell32 for CommandLineToArgvW.
inline std::vector<std::string> CommandLineToUtf8Argv()
{
  int wide_argc = 0;
  LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
  std::vector<std::string> out;
  if (wide_argv == nullptr)
    return out;
  out.reserve(static_cast<std::size_t>(wide_argc));
  for (int i = 0; i < wide_argc; ++i)
    out.push_back(WideToUtf8(wide_argv[i]));
  LocalFree(wide_argv);
  return out;
}
#endif

}  // namespace moderngekko
