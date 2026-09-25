#pragma once

#include "moderngekko/utf8_path.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <picojson.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace moderngekko::frontend
{
// The builder owns validation and cache selection. The GUI never treats the mere
// presence of an old DLL as a successful build for the selected disc and recipe.
class ModuleSetup
{
public:
  ~ModuleSetup() { Cancel(); }
  bool Running() const { return m_process != nullptr; }
  const std::string& Status() const { return m_status; }
  const std::string& Error() const { return m_error; }
  const std::filesystem::path& Module() const { return m_module; }
  float Progress() const { return m_progress; }

  static bool Available(const std::filesystem::path& release)
  {
    return std::filesystem::is_regular_file(release / "disc-builder/build.py");
  }

  bool Start(const std::filesystem::path& release, const std::filesystem::path& user,
             const std::filesystem::path& game)
  {
    Cancel();
    m_error.clear();
    m_module.clear();
    m_status = "Checking your game";
    m_progress = 0;
    const auto setup = user / "Setup";
    std::error_code ec;
    std::filesystem::create_directories(setup, ec);
    if (ec)
    {
      m_error = "Cannot create the game setup folder: " + ec.message();
      return false;
    }
    // Status files and logs from earlier launches; a day old is long past any
    // build another launcher instance could still be running.
    for (const auto& entry : std::filesystem::directory_iterator(setup, ec))
    {
      if (entry.path().filename().string().rfind("module-", 0) != 0)
        continue;
      std::error_code age_ec;
      const auto written = entry.last_write_time(age_ec);
      if (!age_ec && std::filesystem::file_time_type::clock::now() - written > std::chrono::hours(24))
        std::filesystem::remove(entry.path(), age_ec);
    }
    // A unique status path prevents another launcher instance's result from
    // being mistaken for this build. The builder serializes its module cache.
    const auto token = std::to_string(SDL_GetTicksNS());
    m_status_file = setup / ("module-" + token + ".json");
    const std::vector<std::string> storage = {
        PathToUtf8(release / "disc-builder/python/python.exe"), "-I",
        PathToUtf8(release / "disc-builder/build.py"), PathToUtf8(game),
        "--output", PathToUtf8(user / "modules"), "--status", PathToUtf8(m_status_file)};
    std::vector<const char*> arguments;
    for (const auto& arg : storage)
      arguments.push_back(arg.c_str());
    arguments.push_back(nullptr);
    SDL_IOStream* log = SDL_IOFromFile(PathToUtf8(setup / ("module-" + token + ".log")).c_str(), "w");
    const auto props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, arguments.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          log ? SDL_PROCESS_STDIO_REDIRECT : SDL_PROCESS_STDIO_NULL);
    if (log)
      SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    m_process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (log)
      SDL_CloseIO(log);
    if (!m_process)
      m_error = std::string("Could not start game setup: ") + SDL_GetError();
    return Running();
  }

  // Returns true once when the child exits. JSON is published atomically by the
  // builder, but a sharing violation during replacement is harmless: retry.
  bool Poll()
  {
    if (!m_process)
      return false;
    int code = 0;
    const bool finished = SDL_WaitProcess(m_process, false, &code);
    std::ifstream input(m_status_file);
    picojson::value value;
    if (input && picojson::parse(value, input).empty() && value.is<picojson::object>())
    {
      if (value.get("stage").is<std::string>())
        m_status = value.get("stage").get<std::string>();
      if (value.get("completed").is<double>() && value.get("total").is<double>() &&
          value.get("total").get<double>() > 0)
        m_progress = static_cast<float>(value.get("completed").get<double>() /
                                        value.get("total").get<double>());
      if (value.get("error").is<std::string>())
        m_error = value.get("error").get<std::string>();
      if (finished && code == 0 && value.get("module").is<std::string>())
        m_module = Utf8ToPath(value.get("module").get<std::string>());
    }
    if (!finished)
      return false;
    SDL_DestroyProcess(m_process);
    m_process = nullptr;
    if (code != 0 || m_module.empty() || !std::filesystem::is_regular_file(m_module))
    {
      m_module.clear();
      if (m_error.empty())
        m_error = "Game setup did not finish. See the module log in the Setup folder, then retry.";
    }
    return true;
  }

  void Cancel()
  {
    if (m_process)
    {
      SDL_KillProcess(m_process, true);
      SDL_WaitProcess(m_process, true, nullptr);
      SDL_DestroyProcess(m_process);
      m_process = nullptr;
    }
  }

private:
  SDL_Process* m_process = nullptr;
  std::filesystem::path m_status_file;
  std::filesystem::path m_module;
  std::string m_status;
  std::string m_error;
  float m_progress = 0;
};
} // namespace moderngekko::frontend
