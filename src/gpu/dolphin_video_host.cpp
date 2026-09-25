#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VertexLoaderManager.h"

#include "moderngekko/utf8_path.hpp"

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

VideoConfig g_Config;
VideoConfig g_ActiveConfig;
BackendInfo g_backend_info;
std::unique_ptr<BoundingBox> g_bounding_box;

namespace
{
std::mutex s_cache_path_mutex;
// One entry per Set call, never removed: GetUserPath must return a
// const std::string& (the vendored FileUtil.h signature), and callers copy
// it after our lock is released. Publishing a fresh deque element keeps
// every returned reference permanently valid and immutable, so a concurrent
// Set can never tear a reader's copy. Set is a startup-time call, so the
// retained strings are bounded and trivial.
std::deque<std::string> s_cache_path_history{std::string{}};
}

namespace moderngekko
{
void SetDolphinShaderCacheDirectory(std::string directory)
{
  std::lock_guard lock{s_cache_path_mutex};
  if (!directory.empty() && directory.back() != '/' && directory.back() != '\\')
    directory.push_back(std::filesystem::path::preferred_separator);
  s_cache_path_history.emplace_back(std::move(directory));
}
}

GraphicsModGroupConfig::GraphicsModGroupConfig(std::string game_id)
    : m_game_id(std::move(game_id))
{
}
GraphicsModGroupConfig::~GraphicsModGroupConfig() = default;
GraphicsModGroupConfig::GraphicsModGroupConfig(const GraphicsModGroupConfig&) = default;
GraphicsModGroupConfig::GraphicsModGroupConfig(GraphicsModGroupConfig&&) noexcept = default;
GraphicsModGroupConfig& GraphicsModGroupConfig::operator=(const GraphicsModGroupConfig&) = default;
GraphicsModGroupConfig&
GraphicsModGroupConfig::operator=(GraphicsModGroupConfig&&) noexcept = default;

namespace Common::Log
{
void GenericLogFmtImpl(LogLevel, LogType, const char*, int, fmt::string_view,
                       const fmt::format_args&)
{
}
}

namespace Common
{
bool MsgAlertFmtImpl(bool, MsgType, Log::LogType, const char*, int, fmt::string_view,
                     const fmt::format_args&)
{
  return false;
}
}

namespace File
{
// Dolphin hands these shims UTF-8 path strings. fs::path's narrow-string
// constructor would decode them as the ANSI code page on Windows, which can
// mangle a UTF-8 path onto a *different* existing file — so decode through
// UTF-16 instead. Utf8ToPath is local to this project: this TU exists to
// shim Dolphin symbols precisely so the target does not link the core lib.
bool Exists(const std::string& path)
{
  std::error_code error;
  return std::filesystem::exists(moderngekko::Utf8ToPath(path), error);
}

bool CreateDir(const std::string& path)
{
  std::error_code error;
  const std::filesystem::path native = moderngekko::Utf8ToPath(path);
  return std::filesystem::create_directories(native, error) ||
         std::filesystem::is_directory(native, error);
}

const std::string& GetUserPath(unsigned int)
{
  // The reference is copied by callers after this returns, so lock only to
  // pick the newest published element; the element itself is immutable and
  // never freed (see s_cache_path_history), making the copy race-free.
  std::lock_guard lock{s_cache_path_mutex};
  return s_cache_path_history.back();
}
}

namespace VertexLoaderManager
{
u32 g_current_components = 0;
}
