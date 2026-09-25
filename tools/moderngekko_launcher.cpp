#include "frontend_config.hpp"
#include "dol_patch.hpp"
#include "launcher_savestates.hpp"
#include "launcher_module_setup.hpp"
#include "moderngekko/game.hpp"
#include "netplay_session.hpp"

#include "Common/StringUtil.h"
#include "Common/WindowSystemInfo.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "UICommon/UICommon.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
#ifndef MODERNGEKKO_FRONTEND_NAME
#define MODERNGEKKO_FRONTEND_NAME "ModernGekko"
#endif

#ifndef MODERNGEKKO_RUNNER_FILENAME
#define MODERNGEKKO_RUNNER_FILENAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

#ifndef MODERNGEKKO_LOG_FILENAME
#define MODERNGEKKO_LOG_FILENAME "ModernGekko.log"
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr std::string_view CONTROLLER_PROFILE_NAME = "GCPadNew.ini";
#else
constexpr std::string_view CONTROLLER_PROFILE_NAME = "WiimoteNew.ini";
#endif

struct ExtractionState
{
  std::atomic<bool> running{false};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> total{1};
  std::mutex mutex;
  std::string status;
  std::string error;
  std::optional<fs::path> finished_game;
};

struct DialogState
{
  std::mutex mutex;
  std::optional<fs::path> selected;
  std::string error;
};

struct ControllerOption
{
  std::string label;
  std::string device;
};

std::string RequiredGameError(const moderngekko::GameMetadata& metadata)
{
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (metadata.disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return "expected disc ID " MODERNGEKKO_REQUIRED_DISC_ID ", found " + metadata.disc_id;
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (metadata.dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256)
    return "main DOL does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (metadata.rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256)
    return "_Main.rel does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (metadata.assets_sha256 != MODERNGEKKO_REQUIRED_ASSETS_SHA256)
    return "game assets do not match the pinned release";
#endif
  return {};
}

int FindController(const std::vector<ControllerOption>& controllers, std::string_view device)
{
  const auto found = std::ranges::find(controllers, device, &ControllerOption::device);
  return found == controllers.end() ? -1 : static_cast<int>(found - controllers.begin());
}

// The runner binds the controller profile against Dolphin's
// ControllerInterface, so the launcher enumerates and captures through the
// same device layer rather than SDL's joystick list — SDL only sees a
// subset of what a profile can name (DInput pads and keyboard/mouse, XInput
// pads, Pipe devices the QA harness feeds, WGInput pads on MSVC builds).
// Labels are the qualified device strings themselves: that is exactly what
// lands on the "Device =" line, so the UI stays honest about it.
std::vector<ControllerOption> EnumerateControllers()
{
  std::vector<ControllerOption> result;
  for (const auto& device : g_controller_interface.GetAllDevices())
  {
    ciface::Core::DeviceQualifier qualifier;
    qualifier.FromDevice(device.get());
    ControllerOption option;
    option.device = qualifier.ToString();
    option.label = option.device;
    result.emplace_back(std::move(option));
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  // The scripted-input device only exists while a test holds the pipe file
  // open; keep it selectable so a QA binding can be configured ahead of a
  // run instead of only after one.
  if (FindController(result, "Pipe/0/pad0") < 0)
    result.push_back({"Pipe/0/pad0 (scripted test input)", "Pipe/0/pad0"});
#endif
  return result;
}

// Rows like "Main Stick/Calibration" or "Main Stick/Modifier/Range" are
// numeric settings, not controls — press-to-bind is only offered on
// expression rows.
bool IsBindableKey(std::string_view key)
{
  return !(key.starts_with("Options/") || key == "Extension" ||
           key.ends_with("/Calibration") || key.ends_with("/Range") ||
           key.ends_with("/Dead Zone") || key.ends_with("/Response") ||
           key.ends_with("/Rate"));
}

// In-progress "press a control to bind" capture driven from the frame loop.
// InputDetector owns its neutral-state snapshot (no copy/move), so it lives
// behind a unique_ptr; detector == nullptr means no capture is running.
struct CaptureState
{
  std::unique_ptr<ciface::Core::InputDetector> detector;
  int port = -1;
  std::string key;
};

constexpr auto CAPTURE_WINDOW = std::chrono::milliseconds(10000);
constexpr auto CAPTURE_CONFIRM = std::chrono::milliseconds(300);

fs::path DefaultUserDirectory()
{
#ifdef MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
#if defined(_WIN32)
  if (const auto user_profile =
          moderngekko::frontend::EnvironmentPath("USERPROFILE"))
    return *user_profile / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const auto home = moderngekko::frontend::EnvironmentPath("HOME"))
    return *home / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
#if defined(_WIN32)
  if (const auto local_app_data =
          moderngekko::frontend::EnvironmentPath("LOCALAPPDATA"))
    return *local_app_data / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const auto xdg = moderngekko::frontend::EnvironmentPath("XDG_DATA_HOME"))
    return *xdg / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const auto home = moderngekko::frontend::EnvironmentPath("HOME"))
    return *home / ".local/share" / MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

fs::path DocumentsDirectory()
{
#if defined(_WIN32)
  if (const auto user_profile =
          moderngekko::frontend::EnvironmentPath("USERPROFILE"))
    return *user_profile / "Documents";
#endif
  if (const auto home = moderngekko::frontend::EnvironmentPath("HOME"))
    return *home / "Documents";
  return fs::current_path();
}

fs::path ExecutableDirectory(const fs::path& argv0)
{
  std::error_code ec;
#if defined(__linux__)
  const fs::path proc_executable = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const fs::path executable = fs::weakly_canonical(argv0, ec);
  return ec ? fs::current_path() : executable.parent_path();
}

fs::path SiblingExecutable(const fs::path& release_directory, std::string name)
{
#if defined(_WIN32)
  name += ".exe";
#endif
  const fs::path sibling = release_directory / name;
  return fs::is_regular_file(sibling) ? sibling : fs::path(name);
}

bool ApplyBundledDolPatch(const fs::path& game_root, const fs::path& release_directory,
                          bool* changed, std::string* error)
{
#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
  if (game_root.empty() || !fs::is_directory(game_root))
  {
    *changed = false;
    return true;
  }
  return moderngekko::frontend::ApplyDolPatchManifest(
      game_root / "sys" / "main.dol", release_directory / MODERNGEKKO_DOL_PATCH_MANIFEST,
      changed, error);
#else
  *changed = false;
  return true;
#endif
}

fs::path DefaultGameFile(const fs::path& user_directory, const fs::path& release_directory)
{
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  return release_directory / "default-game.txt";
#else
  return user_directory / "default-game.txt";
#endif
}

fs::path ReadDefaultGame(const fs::path& user_directory, const fs::path& release_directory)
{
  std::ifstream file(DefaultGameFile(user_directory, release_directory));
  std::string value;
  std::getline(file, value);
  if (!value.empty() && value.back() == '\r')
    value.pop_back();
  // The file is written UTF-8 by WriteDefaultGame below.
  fs::path game = StringToPath(value);
  if (game.is_relative())
    game = release_directory / game;
  return game;
}

bool WriteDefaultGame(const fs::path& user_directory, const fs::path& release_directory,
                      const fs::path& game, std::string* error)
{
  const fs::path destination = DefaultGameFile(user_directory, release_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  std::ofstream file(destination, std::ios::trunc);
  if (!file)
  {
    if (error)
      *error = "can't save default-game.txt";
    return false;
  }
  fs::path stored = game;
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path relative = fs::relative(game, release_directory, ec);
  if (!ec && !relative.empty())
    stored = relative;
#endif
  file << moderngekko::frontend::PathText(stored) << '\n';
  return true;
}

std::vector<fs::path> FindDiscImages()
{
  std::vector<fs::path> images;
  std::error_code ec;
  const fs::path documents = DocumentsDirectory();
  if (!fs::is_directory(documents, ec))
    return images;
  fs::recursive_directory_iterator iterator(documents,
                                            fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (iterator != end)
  {
    if (iterator.depth() > 4)
      iterator.disable_recursion_pending();
    if (iterator->is_regular_file(ec))
    {
      // Compare the extension as UTF-8: on Windows path::string() runs the
      // name through the ANSI code page, whose best-fit mapping can fold a
      // look-alike extension (e.g. fullwidth ".ｉｓｏ") onto ".iso".
      const std::u8string extension_u8 = iterator->path().extension().u8string();
      std::string extension(reinterpret_cast<const char*>(extension_u8.data()),
                            extension_u8.size());
      std::ranges::transform(extension, extension.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".wbfs" || extension == ".iso" || extension == ".rvz")
        images.push_back(iterator->path());
    }
    iterator.increment(ec);
    if (ec)
      ec.clear();
  }
  std::ranges::sort(images);
  return images;
}

std::optional<fs::path> PrepareDisc(const fs::path& image, const fs::path& user_directory,
                                    const fs::path& release_directory, ExtractionState* state,
                                    std::string* error)
{
  std::unique_ptr<DiscIO::Volume> source =
      DiscIO::CreateVolume(PathToString(image));
  if (!source)
  {
    *error = "Dolphin rejected the selected disc image";
    return std::nullopt;
  }
  const std::string source_id = source->GetGameID(source->GetGamePartition());
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (source_id == MODERNGEKKO_REQUIRED_DISC_ID)
    return image;
#endif
#if defined(MODERNGEKKO_DISC_PREPARER_FILENAME) && \
    defined(MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID) && defined(MODERNGEKKO_REQUIRED_DISC_ID)
  if (source_id != MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID)
  {
    *error = "expected clean " MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID " or patched "
             MODERNGEKKO_REQUIRED_DISC_ID "; selected " + source_id;
    return std::nullopt;
  }
  // The preparer rewrites an accepted source image into the pinned release
  // image; name the output after the required disc ID so the path stays
  // product-neutral. Wind Waker builds configure no preparer, so this branch
  // is compiled out for them.
  const fs::path output =
      user_directory / "Setup" / (MODERNGEKKO_REQUIRED_DISC_ID ".iso");
  std::error_code ec;
  fs::create_directories(output.parent_path(), ec);
  if (ec)
  {
    *error = "can't create setup directory: " + ec.message();
    return std::nullopt;
  }
  {
    std::lock_guard lock(state->mutex);
    state->status = "Preparing disc image " MODERNGEKKO_REQUIRED_DISC_ID;
  }
  const fs::path preparer =
      SiblingExecutable(release_directory, MODERNGEKKO_DISC_PREPARER_FILENAME);
  // SDL_CreateProcess wants UTF-8 argv; PathText (u8string) supplies it on
  // Windows where .string() would emit the ANSI code page.
  const std::array<std::string, 5> storage = {
      moderngekko::frontend::PathText(preparer), "--input",
      moderngekko::frontend::PathText(image), "--output",
      moderngekko::frontend::PathText(output)};
  std::array<const char*, 6> arguments{};
  for (std::size_t i = 0; i < storage.size(); ++i)
    arguments[i] = storage[i].c_str();
  SDL_Process* process = SDL_CreateProcess(arguments.data(), false);
  if (!process)
  {
    *error = "can't start disc preparer: " + std::string(SDL_GetError());
    return std::nullopt;
  }
  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  if (!waited || exit_code != 0)
  {
    *error = "disc preparation failed";
    return std::nullopt;
  }
  return output;
#else
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  *error = "this frontend requires disc ID " MODERNGEKKO_REQUIRED_DISC_ID
           "; selected " + source_id;
#else
  *error = "this build has no disc preparer configured";
#endif
  return std::nullopt;
#endif
}

bool ExtractDisc(const fs::path& image, const fs::path& user_directory,
                 const fs::path& release_directory, ExtractionState* state)
{
  auto fail = [&](std::string message)
  {
    std::lock_guard lock(state->mutex);
    state->error = std::move(message);
    state->running = false;
    return false;
  };

  {
    std::lock_guard lock(state->mutex);
    state->status = "Opening " + moderngekko::frontend::PathText(image.filename());
  }
  std::string preparation_error;
  const std::optional<fs::path> prepared =
      PrepareDisc(image, user_directory, release_directory, state, &preparation_error);
  if (!prepared)
    return fail(std::move(preparation_error));
  std::unique_ptr<DiscIO::Volume> volume =
      DiscIO::CreateVolume(PathToString(*prepared));
  if (!volume)
    return fail("Dolphin rejected the prepared disc image");

  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem* filesystem = volume->GetFileSystem(partition);
  if (!filesystem || !filesystem->IsValid())
    return fail("Dolphin could not read the game partition filesystem");

  std::string disc_id = volume->GetGameID(partition);
  if (disc_id.size() != 6)
    return fail("the selected image has an invalid disc ID");
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return fail("this frontend requires disc ID " MODERNGEKKO_REQUIRED_DISC_ID "; selected " +
                disc_id);
#endif
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path games_directory = release_directory;
  const fs::path output = games_directory / "Game";
#else
  const fs::path games_directory = user_directory / "games";
  const fs::path output = games_directory / disc_id;
#endif
  std::error_code ec;
  if (fs::is_directory(output, ec))
  {
    bool changed = false;
    std::string patch_error;
    if (!ApplyBundledDolPatch(output, release_directory, &changed, &patch_error))
      return fail("existing extraction patching failed: " + patch_error);
  }
  const auto existing = moderngekko::InspectGame(output);
  if (existing && RequiredGameError(*existing.metadata).empty())
  {
    std::string error;
    if (!WriteDefaultGame(user_directory, release_directory, output, &error))
      return fail(error);
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Using existing extraction";
    state->running = false;
    return true;
  }

  const fs::path staging = games_directory / (disc_id + ".extracting");
  fs::remove_all(staging, ec);
  fs::create_directories(staging / "files", ec);
  if (ec)
    return fail("can't create extraction directory: " + ec.message());

  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting system data";
  }
  if (!DiscIO::ExportSystemData(*volume, partition, PathToString(staging)))
  {
    fs::remove_all(staging, ec);
    return fail("Dolphin failed while extracting the disc system data");
  }

  state->total = std::max(1u, filesystem->GetRoot().GetTotalChildren());
  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting game files";
  }
  DiscIO::ExportDirectory(*volume, partition, filesystem->GetRoot(), true, "",
                          PathToString(staging / "files"),
                          [state](const std::string& path)
                          {
                            ++state->completed;
                            std::lock_guard lock(state->mutex);
                            state->status = "Extracting " + path;
                            return false;
                          });

  bool dol_changed = false;
  std::string dol_patch_error;
  if (!ApplyBundledDolPatch(staging, release_directory, &dol_changed, &dol_patch_error))
  {
    fs::remove_all(staging, ec);
    return fail("extracted DOL patching failed: " + dol_patch_error);
  }

  const auto inspected = moderngekko::InspectGame(staging);
  if (!inspected)
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + inspected.error);
  }
  if (const std::string identity_error = RequiredGameError(*inspected.metadata);
      !identity_error.empty())
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + identity_error);
  }

  fs::remove_all(output, ec);
  ec.clear();
  fs::rename(staging, output, ec);
  if (ec)
    return fail("can't publish extracted game: " + ec.message());
  std::string error;
  if (!WriteDefaultGame(user_directory, release_directory, output, &error))
    return fail(error);

  {
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Extraction complete";
  }
  state->running = false;
  return true;
}

void SDLCALL FileDialogCallback(void* userdata, const char* const* filelist, int)
{
  auto* state = static_cast<DialogState*>(userdata);
  std::lock_guard lock(state->mutex);
  if (!filelist)
    state->error = SDL_GetError();
  else if (filelist[0])
    state->selected = StringToPath(filelist[0]);  // SDL paths are UTF-8
}

fs::path SiblingRunner(const fs::path& argv0)
{
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path runner = MODERNGEKKO_RUNNER_FILENAME;
#if defined(_WIN32)
  runner += ".exe";
#endif
  const fs::path sibling = self.parent_path() / runner;
  return fs::is_regular_file(sibling) ? sibling : runner;
}
} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
  // The CRT hands us argv in the ANSI code page; rebuild it as UTF-8 from the
  // wide command line so --user-dir/--extract can carry any profile path, then
  // decode path options with StringToPath. Non-empty check keeps CRT argv as
  // the fallback rather than running with no arguments at all.
  std::vector<std::string> utf8_args =
      Common::CommandLineToUtf8Argv(GetCommandLineW());
  std::vector<char*> utf8_argv;
  utf8_argv.reserve(utf8_args.size() + 1);
  for (std::string& argument : utf8_args)
    utf8_argv.push_back(argument.data());
  utf8_argv.push_back(nullptr);
  if (!utf8_args.empty())
  {
    argc = static_cast<int>(utf8_args.size());
    argv = utf8_argv.data();
  }
#endif

  bool use_wayland = false;
  std::optional<fs::path> extract_only;
  std::optional<fs::path> user_directory_override;
  for (int i = 1; i < argc; ++i)
  {
    if (std::string_view(argv[i]) == "-X11" || std::string_view(argv[i]) == "--x11")
      use_wayland = false;
    else if (std::string_view(argv[i]) == "--wayland")
      use_wayland = true;
    else if (std::string_view(argv[i]) == "--user-dir")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--user-dir requires a path\n";
        return 2;
      }
      user_directory_override = StringToPath(argv[++i]);
    }
    else if (std::string_view(argv[i]) == "--extract" && i + 1 < argc)
      extract_only = StringToPath(argv[++i]);
  }

  const fs::path release_directory = ExecutableDirectory(StringToPath(argv[0]));
  fs::path user_directory;
  if (user_directory_override)
  {
    if (user_directory_override->empty())
    {
      std::cerr << "--user-dir requires a non-empty path\n";
      return 2;
    }
    std::error_code ec;
    user_directory = fs::absolute(*user_directory_override, ec).lexically_normal();
    if (ec)
    {
      std::cerr << "invalid --user-dir: " << ec.message() << '\n';
      return 2;
    }
  }
  else
  {
    // Release-bundle layout: <exe>/assets/user-dir ships the seeded config
    // (Dolphin.ini, controller config, shader caches) and is also what the
    // runner picks by default, so launcher edits land where the game reads
    // them. Outside a bundle, use the roaming profile dir.
    std::error_code bundled_ec;
    const fs::path bundled = release_directory / "assets" / "user-dir";
    user_directory = fs::is_directory(bundled, bundled_ec)
                         ? bundled
                         : DefaultUserDirectory();
  }
  if (extract_only)
  {
    ExtractionState extraction;
    extraction.running = true;
    const bool success =
        ExtractDisc(*extract_only, user_directory, release_directory, &extraction);
    std::lock_guard lock(extraction.mutex);
    if (!success)
      std::cerr << "extraction failed: " << extraction.error << '\n';
    else
      std::cout << extraction.status << ": " << *extraction.finished_game << '\n';
    return success ? 0 : 1;
  }

  auto config = moderngekko::frontend::LoadConfig(user_directory, true);
  if (!config)
  {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Invalid " MODERNGEKKO_FRONTEND_NAME " config.ini",
                             config.error.c_str(), nullptr);
    return 2;
  }

#if defined(__linux__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, use_wayland ? "wayland" : "x11");
#endif
  SDL_setenv_unsafe("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    return 1;

  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  SDL_Window* window = SDL_CreateWindow(MODERNGEKKO_FRONTEND_NAME, static_cast<int>(820 * scale),
                                        static_cast<int>(700 * scale),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
  {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, "vulkan");
  if (!renderer)
    renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer)
  {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  // Configure through the same device layer the runner binds against:
  // ControllerInterface enumerates DInput pads and keyboard/mouse, XInput
  // pads, and Pipe test devices — SDL's own joystick list can't name those.
  // The SDL input backend stays off because this process already owns SDL's
  // joystick subsystem (it would double-list every pad and spawn a second
  // hotplug thread); DSUClient stays off because a settings UI never polls
  // network devices. SetUserDirectory first so the Pipe backend scans this
  // user dir's Pipes folder rather than a default profile path.
  UICommon::SetUserDirectory(PathToString(user_directory));
  g_controller_interface.SetBackendSourceEnabled("SDL", false);
  g_controller_interface.SetBackendSourceEnabled("DSUClient", false);
  {
    WindowSystemInfo input_wsi;
#if defined(_WIN32)
    // DInput and the CM_NOTIFY hotplug watch are window-bound; hand the
    // launcher HWND in so enumeration and capture work like the game's.
    // (Off Windows the launcher stays headless-input for now: only Pipe
    // devices enumerate.)
    input_wsi.type = WindowSystemType::Windows;
    input_wsi.render_window = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr);
    input_wsi.render_surface = input_wsi.render_window;
#endif
    g_controller_interface.Initialize(input_wsi);
  }

  std::vector<fs::path> images = FindDiscImages();
  std::optional<fs::path> selected_image = images.empty() ? std::nullopt : std::optional(images[0]);
  fs::path current_game = ReadDefaultGame(user_directory, release_directory);
  bool current_dol_changed = false;
  std::string current_dol_error;
  auto current_metadata =
      ApplyBundledDolPatch(current_game, release_directory, &current_dol_changed,
                           &current_dol_error) ?
          moderngekko::InspectGame(current_game) :
          moderngekko::GameInspectResult{{}, current_dol_error};
  if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
    current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
  const auto& resolutions = moderngekko::frontend::SupportedResolutions();
  const auto& graphics_backends =
      moderngekko::frontend::SupportedGraphicsBackends();
  const auto& aspect_ratios = moderngekko::frontend::SupportedAspectRatios();
  const auto& anisotropy_options =
      moderngekko::frontend::SupportedAnisotropyOptions();
  const auto& window_modes = moderngekko::frontend::SupportedWindowModes();
  const auto& aa_options =
      moderngekko::frontend::SupportedAntiAliasingOptions();
  bool show_fps_in_title = config.show_fps_in_title;
  bool widescreen_hack = config.widescreen_hack.value_or(false);
  bool background_input = config.background_input.value_or(false);
  std::array<char, 31> netplay_nickname{};
  std::array<char, 256> netplay_address{};
  std::snprintf(netplay_nickname.data(), netplay_nickname.size(), "%s",
                config.netplay_nickname.c_str());
  std::snprintf(netplay_address.data(), netplay_address.size(), "%s",
                config.netplay_address.c_str());
  int netplay_port = config.netplay_port;
  bool automatic_buffer = config.netplay_buffer == "auto";
  int manual_buffer = automatic_buffer ? 5 : std::stoi(config.netplay_buffer);
  int resolution_index = 0;
  for (std::size_t i = 0; i < resolutions.size(); ++i)
  {
    if (config.resolution == resolutions[i].text)
      resolution_index = static_cast<int>(i);
  }
  int graphics_backend_index = 0;
  for (std::size_t i = 0; i < graphics_backends.size(); ++i)
  {
    if (config.graphics_backend == graphics_backends[i].value)
      graphics_backend_index = static_cast<int>(i);
  }
  int aspect_ratio_index = 0;
  bool aspect_ratio_unlisted = false;
  std::array<char, 48> aspect_ratio_unlisted_text{};
  {
    const moderngekko::frontend::AspectRatioValue current =
        config.aspect_ratio.value_or(moderngekko::frontend::AspectRatio::Auto);
    bool matched = false;
    for (std::size_t i = 0; i < aspect_ratios.size(); ++i)
    {
      const auto& option = aspect_ratios[i];
      if (current.mode != option.value)
        continue;
      if (option.value == moderngekko::frontend::AspectRatio::Custom &&
          (current.width != option.width || current.height != option.height))
        continue;
      aspect_ratio_index = static_cast<int>(i);
      matched = true;
      break;
    }
    // config.ini can carry any in-window W:H pair (aspect_ratio=custom
    // 48:10, aspect_ratio=2.39): show it verbatim instead of snapping the
    // combo back to Auto.
    if (!matched && current.mode == moderngekko::frontend::AspectRatio::Custom)
    {
      aspect_ratio_unlisted = true;
      std::snprintf(aspect_ratio_unlisted_text.data(),
                    aspect_ratio_unlisted_text.size(), "custom %d:%d",
                    current.width, current.height);
    }
  }
  int anisotropy_index = 0;
  for (std::size_t i = 0; i < anisotropy_options.size(); ++i)
  {
    if (config.anisotropy.value_or(
            moderngekko::frontend::Anisotropy::Default) ==
        anisotropy_options[i].value)
      anisotropy_index = static_cast<int>(i);
  }
  int window_mode_index = 0;
  for (std::size_t i = 0; i < window_modes.size(); ++i)
  {
    if (config.EffectiveWindowMode() == window_modes[i].value)
      window_mode_index = static_cast<int>(i);
  }
  int aa_index = 0;
  for (std::size_t i = 0; i < aa_options.size(); ++i)
  {
    if (config.aa.value_or(moderngekko::frontend::AntiAliasing::Off) ==
        aa_options[i].value)
      aa_index = static_cast<int>(i);
  }

  // StateSaves is Dolphin's own state directory, so states written in-game land
  // here without any extra configuration and show up on the next launch.
  //
  // Listed once at startup rather than every frame, so the dropdown does not
  // hit the filesystem on each redraw. A state written while the launcher is
  // open therefore appears on the next launch, which is the case that matters:
  // states are written by the running game, not by the launcher.
  const std::vector<fs::path> launcher_savestates =
      moderngekko::frontend::ListLauncherSavestates(user_directory / "StateSaves");
  // Loading is always explicit. StateSaves is shared by games, so selecting the
  // newest file automatically could try to resume a different title.
  int selected_savestate = -1;

  // SDL's open-file dialog runs asynchronously and invokes FileDialogCallback
  // once, whenever it finishes — including after the event loop has stopped if
  // the window is closed while a dialog is outstanding. A stack DialogState
  // would be destroyed on return from main() in that case, leaving the pending
  // callback to write through a dangling userdata pointer. Allocate it for the
  // lifetime of the process instead; the single instance is intentionally not
  // freed so even a callback racing process exit still sees valid storage.
  DialogState& dialog = *new DialogState();
  std::vector<ControllerOption> controllers = EnumerateControllers();
  bool controller_profile_exists =
      moderngekko::frontend::ControllerConfigExists(user_directory);
  // The four [GCPadN] ports as they stand in the profile on disk. The
  // Controllers section edits this copy; every change is persisted
  // immediately through the frontend's atomic writers, so the file is never
  // left half-applied.
  std::array<moderngekko::frontend::PadPortSettings, 4> pad_ports =
      moderngekko::frontend::ReadControllerProfile(user_directory);
  std::string controller_status;
  CaptureState capture;

  // Hotplug arrives on backend worker threads (CM_NOTIFY on Windows);
  // funnel it into a flag the UI loop drains so device enumeration only
  // happens on the main thread.
  std::atomic<bool> devices_changed{false};
  const auto devices_changed_hook =
      g_controller_interface.RegisterDevicesChangedCallback(
          [&devices_changed] { devices_changed.store(true); });

  const auto bound_devices = [&]
  {
    std::vector<std::string> devices;
    for (const auto& port : pad_ports)
      if (!port.device.empty())
        devices.push_back(port.device);
    return devices;
  };

  // The QA pipe only exists while a test holds it open, so a configured
  // Pipe binding must never count as missing; everything else is available
  // iff ControllerInterface currently enumerates it.
  const auto device_available = [&](std::string_view device) -> bool
  {
    if (device.empty())
      return false;
    ciface::Core::DeviceQualifier qualifier;
    qualifier.FromString(std::string(device));
    if (qualifier.source == "Pipe")
      return true;
    return FindController(controllers, device) >= 0;
  };

  // First connected XInput pad, else the keyboard/mouse device, else the
  // first enumerated device of any kind — the automatic pick used when a
  // port has no usable binding. Pipe is never auto-picked: it is a test
  // input the user (or harness) selects explicitly.
  const auto pick_fallback_device = [&]() -> std::string
  {
    for (const auto& option : controllers)
      if (option.device.starts_with("XInput/"))
        return option.device;
    for (const auto& option : controllers)
      if (option.device == "DInput/0/Keyboard Mouse")
        return option.device;
    for (const auto& option : controllers)
      if (!option.device.starts_with("Pipe/"))
        return option.device;
    return {};
  };

  // Every settings save shares this sync: config.ini's controllerN list
  // follows the bound ports (compacted, in port order) so the runner's
  // ensure path and netplay --controller arguments agree with the profile.
  const auto sync_config = [&]
  {
    config.resolution = resolutions[resolution_index].text;
    config.show_fps_in_title = show_fps_in_title;
    config.controllers = bound_devices();
    config.controller = config.controllers.empty() ? std::string{}
                                                   : config.controllers.front();
  };

  // Persist the current port set to the controller profile and mirror it to
  // config.ini. The profile write is atomic (sibling temp + copy); a failed
  // save leaves the on-disk pair untouched.
  const auto persist_controllers = [&](std::string status)
  {
    std::string message;
    if (!moderngekko::frontend::SaveControllerProfile(user_directory, pad_ports,
                                                      &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    controller_profile_exists = true;
    sync_config();
    std::string error;
    if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(error);
      return false;
    }
    controller_status = std::move(status);
    return true;
  };

  // Bind a port to a device, seeding generated defaults when the port has
  // no controls yet. Existing mappings are kept across a device switch —
  // "Reset to defaults" is the explicit way to start over.
  const auto bind_port = [&](int port, std::string_view device)
  {
    auto& pad = pad_ports[port];
    const bool kept_mappings = !pad.controls.empty() && pad.device != device;
    pad.device = std::string(device);
    if (pad.controls.empty())
      pad.controls = moderngekko::frontend::DefaultPadControls(device);
    std::string status =
        "Port " + std::to_string(port + 1) + " bound to " + pad.device;
    if (kept_mappings)
      status += " (mappings kept)";
    persist_controllers(std::move(status));
  };

  // Nothing usable bound yet: honor config.ini's controller1 when it names
  // a detected device, otherwise the automatic pick. Goes through
  // EnsureControllerConfig rather than the profile writer so a hand-edited
  // file's other sections and comments survive untouched.
  const auto bootstrap_port1 = [&]() -> bool
  {
    std::string device;
    if (!config.controllers.empty() && device_available(config.controllers[0]))
      device = config.controllers[0];
    else
      device = pick_fallback_device();
    if (device.empty())
    {
      controller_status = "No controller detected";
      return false;
    }
    std::string message;
    if (!moderngekko::frontend::EnsureControllerConfig(user_directory, device,
                                                       &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    pad_ports = moderngekko::frontend::ReadControllerProfile(user_directory);
    controller_profile_exists = true;
    sync_config();
    std::string error;
    if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(error);
      return false;
    }
    controller_status = "Port 1 bound to " + device;
    return true;
  };

  const auto ensure_controller = [&]
  {
    // Re-read from disk: the QA harness (or a text editor) may have swapped
    // the profile while the launcher was open, and the Play path must act
    // on what is actually there.
    pad_ports = moderngekko::frontend::ReadControllerProfile(user_directory);
    controller_profile_exists =
        moderngekko::frontend::ControllerConfigExists(user_directory);
    const bool any_bound = std::ranges::any_of(
        pad_ports, [](const auto& p) { return !p.device.empty(); });
    if (!controller_profile_exists || !any_bound)
      return bootstrap_port1();
    if (!pad_ports[0].device.empty() && !device_available(pad_ports[0].device))
    {
      // Port 1's device went away — don't launch into a dead binding. Same
      // source keeps the mapping body (our defaults use names shared by
      // every pad of that kind); a different source starts over.
      const std::string missing = pad_ports[0].device;
      const std::string fallback = pick_fallback_device();
      if (!fallback.empty() && fallback != missing)
      {
        ciface::Core::DeviceQualifier old_qualifier, new_qualifier;
        old_qualifier.FromString(missing);
        new_qualifier.FromString(fallback);
        pad_ports[0].device = fallback;
        if (old_qualifier.source != new_qualifier.source)
          pad_ports[0].controls =
              moderngekko::frontend::DefaultPadControls(fallback);
        if (!persist_controllers({}))
          return false;
        controller_status =
            missing + " not detected; bound port 1 to " + fallback;
        return true;
      }
      controller_status = missing + " not detected";
      return true;
    }
    controller_status =
        "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
    return true;
  };

  const auto refresh_controllers = [&]
  {
    controllers = EnumerateControllers();
    // Only auto-bind while nothing usable is on file; a pad appearing
    // mid-session must not rewrite a profile the player (or the QA
    // harness) already set.
    if (!controller_profile_exists)
      bootstrap_port1();
    else if (controller_status.empty())
      controller_status =
          "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
  };
  refresh_controllers();
  ExtractionState extraction;
  std::jthread extraction_thread;
  enum class LaunchMode
  {
    None,
    Solo,
    Host,
    Join,
  };
  bool done = false;
  LaunchMode launch_mode = LaunchMode::None;
  LaunchMode pending_launch = LaunchMode::None;
  moderngekko::frontend::ModuleSetup module_setup;
  bool module_setup_ready = false;
  while (!done)
  {
    if (module_setup.Poll())
    {
      if (!module_setup.Module().empty())
      {
        module_setup_ready = true;
        launch_mode = pending_launch;
        done = true;
      }
      pending_launch = LaunchMode::None;
    }
    bool controllers_changed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        done = true;
      if (event.type == SDL_EVENT_JOYSTICK_ADDED || event.type == SDL_EVENT_JOYSTICK_REMOVED ||
          event.type == SDL_EVENT_GAMEPAD_REMAPPED)
      {
        controllers_changed = true;
      }
    }
    // Keep device state fresh for press-to-bind and let the backends retire
    // removed devices; cheap at this frame rate.
    g_controller_interface.UpdateInput();
    if (controllers_changed || devices_changed.exchange(false))
      refresh_controllers();

    {
      std::lock_guard lock(dialog.mutex);
      if (dialog.selected)
      {
        selected_image = std::move(dialog.selected);
        dialog.selected.reset();
      }
    }
    {
      std::lock_guard lock(extraction.mutex);
      if (extraction.finished_game)
      {
        current_game = *extraction.finished_game;
        current_metadata = moderngekko::InspectGame(current_game);
        if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
          current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
        extraction.finished_game.reset();
        if (ensure_controller())
        {
          launch_mode = LaunchMode::Solo;
          done = true;
        }
      }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Drive a press-to-bind capture: the detector was started on the port's
    // bound device, so results come back as names on that device and
    // BuildExpression emits them unqualified (they resolve against the
    // section's Device line, like the generated defaults).
    if (capture.detector)
    {
      capture.detector->Update(CAPTURE_WINDOW, CAPTURE_CONFIRM, CAPTURE_WINDOW);
      if (capture.detector->IsComplete())
      {
        const auto results = capture.detector->TakeResults();
        capture.detector.reset();
        if (!results.empty() && capture.port >= 0 && capture.port < 4)
        {
          auto& pad = pad_ports[capture.port];
          ciface::Core::DeviceQualifier default_device;
          default_device.FromString(pad.device);
          const std::string expression = ciface::MappingCommon::BuildExpression(
              results, default_device, ciface::MappingCommon::Quote::On);
          for (auto& [key, value] : pad.controls)
          {
            if (key == capture.key)
            {
              value = expression;
              break;
            }
          }
          persist_controllers("Bound " + capture.key + " to " + expression);
        }
        capture.port = -1;
        capture.key.clear();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
      {
        capture.detector.reset();
        capture.port = -1;
        capture.key.clear();
        controller_status = "Binding cancelled";
      }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(MODERNGEKKO_FRONTEND_NAME " Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(MODERNGEKKO_FRONTEND_NAME);
    ImGui::Separator();
    ImGui::BeginDisabled(module_setup.Running());

    if (current_metadata)
    {
      ImGui::Text("Ready: %s [%s]", current_metadata.metadata->game_name.c_str(),
                  current_metadata.metadata->disc_id.c_str());
      if (!launcher_savestates.empty())
      {
        ImGui::TextUnformatted("Start from savestate");
        const std::string savestate_preview =
            selected_savestate >= 0 ?
                moderngekko::frontend::LauncherSavestateLabel(
                    launcher_savestates[static_cast<std::size_t>(selected_savestate)],
                    selected_savestate == 0) :
                "Normal boot";
        if (ImGui::BeginCombo("##launch_savestate", savestate_preview.c_str()))
        {
          const bool normal_boot_selected = selected_savestate < 0;
          if (ImGui::Selectable("Normal boot", normal_boot_selected))
            selected_savestate = -1;
          if (normal_boot_selected)
            ImGui::SetItemDefaultFocus();

          for (std::size_t i = 0; i < launcher_savestates.size(); ++i)
          {
            const std::string label =
                moderngekko::frontend::LauncherSavestateLabel(launcher_savestates[i], i == 0);
            const bool selected = selected_savestate == static_cast<int>(i);
            if (ImGui::Selectable(label.c_str(), selected))
              selected_savestate = static_cast<int>(i);
            if (selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        // Netplay starts every player from the same boot, so a state chosen by
        // one side would desync the session immediately.
        ImGui::TextDisabled("Solo play only.");
        ImGui::Spacing();
      }
      if (ImGui::Button("Play", ImVec2(180 * scale, 42 * scale)))
      {
        if (ensure_controller())
        {
          launch_mode = LaunchMode::Solo;
          done = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Host Netplay", ImVec2(180 * scale, 42 * scale)))
      {
        if (ensure_controller())
        {
          config.netplay_nickname = netplay_nickname.data();
          config.netplay_address = netplay_address.data();
          config.netplay_port = static_cast<std::uint16_t>(netplay_port);
          config.netplay_buffer = automatic_buffer ? "auto" : std::to_string(manual_buffer);
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            launch_mode = LaunchMode::Host;
            done = true;
          }
          else
          {
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Join Netplay", ImVec2(180 * scale, 42 * scale)))
      {
        if (ensure_controller())
        {
          config.netplay_nickname = netplay_nickname.data();
          config.netplay_address = netplay_address.data();
          config.netplay_port = static_cast<std::uint16_t>(netplay_port);
          config.netplay_buffer = automatic_buffer ? "auto" : std::to_string(manual_buffer);
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            launch_mode = LaunchMode::Join;
            done = true;
          }
          else
          {
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
    }
    else
    {
      ImGui::TextUnformatted("No extracted game is configured yet.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Graphics backend");
    if (ImGui::BeginCombo("##graphics_backend",
                          graphics_backends[graphics_backend_index].text))
    {
      for (std::size_t i = 0; i < graphics_backends.size(); ++i)
      {
        const bool selected = graphics_backend_index == static_cast<int>(i);
        if (ImGui::Selectable(graphics_backends[i].text, selected))
        {
          const std::string previous = config.graphics_backend;
          config.graphics_backend = graphics_backends[i].value;
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            graphics_backend_index = static_cast<int>(i);
          }
          else
          {
            config.graphics_backend = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Graphics adapter");
    const std::string adapter_preview =
        config.graphics_adapter
            ? "Adapter " + std::to_string(*config.graphics_adapter)
            : "Automatic";
    if (ImGui::BeginCombo("##graphics_adapter", adapter_preview.c_str()))
    {
      const bool automatic_selected = !config.graphics_adapter;
      if (ImGui::Selectable("Automatic", automatic_selected))
      {
        const std::optional<int> previous = config.graphics_adapter;
        config.graphics_adapter.reset();
        sync_config();
        std::string error;
        if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
        {
          config.graphics_adapter = previous;
          std::lock_guard lock(dialog.mutex);
          dialog.error = std::move(error);
        }
      }
      if (automatic_selected)
        ImGui::SetItemDefaultFocus();
      for (int i = 0; i <= 16; ++i)
      {
        const bool selected = config.graphics_adapter == i;
        const std::string label = "Adapter " + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected))
        {
          const std::optional<int> previous = config.graphics_adapter;
          config.graphics_adapter = i;
          sync_config();
          std::string error;
          if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            config.graphics_adapter = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Window mode");
    if (ImGui::BeginCombo("##window_mode",
                          window_modes[window_mode_index].text))
    {
      for (std::size_t i = 0; i < window_modes.size(); ++i)
      {
        const bool selected = window_mode_index == static_cast<int>(i);
        if (ImGui::Selectable(window_modes[i].text, selected))
        {
          // window_mode is authoritative; fullscreen stays coherent for
          // runners that predate the key.
          const std::optional<moderngekko::frontend::WindowMode> previous =
              config.window_mode;
          const bool previous_fullscreen = config.fullscreen;
          config.window_mode = window_modes[i].value;
          config.fullscreen =
              window_modes[i].value ==
              moderngekko::frontend::WindowMode::Fullscreen;
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config,
                                                &error))
          {
            window_mode_index = static_cast<int>(i);
          }
          else
          {
            config.window_mode = previous;
            config.fullscreen = previous_fullscreen;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Aspect ratio");
    if (ImGui::BeginCombo("##aspect_ratio",
                          aspect_ratio_unlisted
                              ? aspect_ratio_unlisted_text.data()
                              : aspect_ratios[aspect_ratio_index].text))
    {
      for (std::size_t i = 0; i < aspect_ratios.size(); ++i)
      {
        const bool selected =
            !aspect_ratio_unlisted && aspect_ratio_index == static_cast<int>(i);
        if (ImGui::Selectable(aspect_ratios[i].text, selected))
        {
          const std::optional<moderngekko::frontend::AspectRatioValue> previous =
              config.aspect_ratio;
          config.aspect_ratio = moderngekko::frontend::AspectRatioValue{
              aspect_ratios[i].value, aspect_ratios[i].width,
              aspect_ratios[i].height};
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            aspect_ratio_index = static_cast<int>(i);
            aspect_ratio_unlisted = false;
          }
          else
          {
            config.aspect_ratio = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    const bool previous_widescreen_hack = widescreen_hack;
    if (ImGui::Checkbox("Expand 3D view for widescreen", &widescreen_hack))
    {
      const std::optional<bool> previous_optional = config.widescreen_hack;
      config.widescreen_hack = widescreen_hack;
      sync_config();
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
      {
        widescreen_hack = previous_widescreen_hack;
        config.widescreen_hack = previous_optional;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
    }
    ImGui::TextDisabled(
        "Select 16:9 above; experimental scenes may expose culled objects.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Internal resolution (Dolphin EFB upscale)");
    if (ImGui::BeginCombo("##resolution", resolutions[resolution_index].text))
    {
      for (std::size_t i = 0; i < resolutions.size(); ++i)
      {
        const bool selected = resolution_index == static_cast<int>(i);
        if (ImGui::Selectable(resolutions[i].text, selected))
        {
          // Full-config save: the 4-arg SaveConfig overload collapses
          // controllerN down to the single selected pad.
          const int previous_index = resolution_index;
          resolution_index = static_cast<int>(i);
          sync_config();
          std::string error;
          if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            resolution_index = previous_index;
            config.resolution = resolutions[previous_index].text;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Anisotropic filtering");
    if (ImGui::BeginCombo("##anisotropy",
                          anisotropy_options[anisotropy_index].text))
    {
      for (std::size_t i = 0; i < anisotropy_options.size(); ++i)
      {
        const bool selected = anisotropy_index == static_cast<int>(i);
        if (ImGui::Selectable(anisotropy_options[i].text, selected))
        {
          const std::optional<moderngekko::frontend::Anisotropy> previous =
              config.anisotropy;
          config.anisotropy = anisotropy_options[i].value;
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            anisotropy_index = static_cast<int>(i);
          }
          else
          {
            config.anisotropy = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Anti-aliasing");
    if (ImGui::BeginCombo("##anti_aliasing", aa_options[aa_index].text))
    {
      for (std::size_t i = 0; i < aa_options.size(); ++i)
      {
        const bool selected = aa_index == static_cast<int>(i);
        if (ImGui::Selectable(aa_options[i].text, selected))
        {
          const std::optional<moderngekko::frontend::AntiAliasing> previous =
              config.aa;
          config.aa = aa_options[i].value;
          sync_config();
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            aa_index = static_cast<int>(i);
          }
          else
          {
            config.aa = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    const bool previous_show_fps_in_title = show_fps_in_title;
    if (ImGui::Checkbox("Show FPS in window title", &show_fps_in_title))
    {
      sync_config();
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
      {
        show_fps_in_title = previous_show_fps_in_title;
        config.show_fps_in_title = previous_show_fps_in_title;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Controllers");
    if (ImGui::BeginTabBar("##controller_ports"))
    {
      for (int port = 0; port != 4; ++port)
      {
        std::string tab_label = "Port " + std::to_string(port + 1);
        if (pad_ports[port].device.empty())
          tab_label += " (off)";
        if (!ImGui::BeginTabItem(tab_label.c_str()))
          continue;
        auto& pad = pad_ports[port];
        const int pad_device_index = FindController(controllers, pad.device);
        const std::string device_preview =
            pad.device.empty()      ? "Not connected"
            : pad_device_index >= 0 ? controllers[pad_device_index].label
                                    : pad.device + " (not detected)";
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("Device##port", device_preview.c_str()))
        {
          if (ImGui::Selectable("Not connected", pad.device.empty()))
          {
            pad.device.clear();
            pad.controls.clear();
            persist_controllers("Port " + std::to_string(port + 1) +
                                " disconnected");
          }
          if (!pad.device.empty() && pad_device_index < 0)
          {
            // Bound but not enumerated right now (pad unplugged, pipe file
            // not open yet): keep the entry visible so it isn't silently
            // deselected, but only a real pick changes it.
            ImGui::Selectable((pad.device + " (not detected)").c_str(), false,
                              ImGuiSelectableFlags_Disabled);
          }
          for (std::size_t i = 0; i < controllers.size(); ++i)
          {
            const bool selected = pad_device_index == static_cast<int>(i);
            if (ImGui::Selectable(controllers[i].label.c_str(), selected))
              bind_port(port, controllers[i].device);
            if (selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }

        if (!pad.device.empty())
        {
          if (capture.detector && capture.port == port)
          {
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
                               "Press a control on %s — Esc cancels",
                               pad.device.c_str());
          }
          if (ImGui::BeginTable("##pad_bindings", 3,
                                ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp))
          {
            int row = 0;
            for (auto& [key, expression] : pad.controls)
            {
              ImGui::PushID(row++);
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(key.c_str());
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(expression.empty() ? "(unbound)"
                                                        : expression.c_str());
              ImGui::TableNextColumn();
              if (IsBindableKey(key))
              {
                const bool capturing_this =
                    capture.detector && capture.port == port &&
                    capture.key == key;
                if (capturing_this)
                {
                  if (ImGui::SmallButton("cancel"))
                  {
                    capture.detector.reset();
                    capture.port = -1;
                    capture.key.clear();
                    controller_status = "Binding cancelled";
                  }
                }
                else
                {
                  ImGui::BeginDisabled(capture.detector != nullptr);
                  if (ImGui::SmallButton("Bind"))
                  {
                    capture.detector =
                        std::make_unique<ciface::Core::InputDetector>();
                    capture.port = port;
                    capture.key = key;
                    g_controller_interface.UpdateInput();
                    capture.detector->Start(g_controller_interface,
                                            std::array{pad.device});
                  }
                  ImGui::EndDisabled();
                }
              }
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
          if (ImGui::SmallButton("Reset to defaults"))
          {
            pad.controls = moderngekko::frontend::DefaultPadControls(pad.device);
            persist_controllers("Port " + std::to_string(port + 1) +
                                " reset to defaults");
          }
        }
        else
        {
          ImGui::TextDisabled(
              "Pick a device to add a controller on this port.");
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    if (ImGui::SmallButton("Refresh device list"))
    {
      g_controller_interface.RefreshDevices();
      controllers = EnumerateControllers();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Devices are listed as %s writes them (Device = ...).",
        CONTROLLER_PROFILE_NAME.data());
    const bool previous_background_input = background_input;
    if (ImGui::Checkbox("Allow input while game is in background",
                        &background_input))
    {
      const std::optional<bool> previous_optional = config.background_input;
      config.background_input = background_input;
      sync_config();
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
      {
        background_input = previous_background_input;
        config.background_input = previous_optional;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Netplay");
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::InputText("Nickname", netplay_nickname.data(), netplay_nickname.size());
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::InputText("Host / IP", netplay_address.data(), netplay_address.size());
    ImGui::SetNextItemWidth(120 * scale);
    ImGui::InputInt("UDP port", &netplay_port);
    netplay_port = std::clamp(netplay_port, 1, 65535);
    ImGui::Checkbox("Automatic input buffer", &automatic_buffer);
    if (!automatic_buffer)
    {
      ImGui::SetNextItemWidth(180 * scale);
      ImGui::SliderInt("Buffer frames", &manual_buffer, 1, 20);
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Game disc image");
    if (selected_image)
      ImGui::TextWrapped(
          "%s", moderngekko::frontend::PathText(*selected_image).c_str());
    else
      ImGui::TextDisabled("No ISO, WBFS, or RVZ selected");

    if (!extraction.running)
    {
      if (ImGui::Button("Browse for ISO / WBFS / RVZ"))
      {
        static constexpr SDL_DialogFileFilter filters[] = {
            {"Disc images", "iso;wbfs;rvz"},
            {"ISO", "iso"},
            {"WBFS", "wbfs"},
            {"RVZ", "rvz"}};
        const std::string documents =
            moderngekko::frontend::PathText(DocumentsDirectory());
        SDL_ShowOpenFileDialog(FileDialogCallback, &dialog, window, filters,
                               static_cast<int>(std::size(filters)), documents.c_str(), false);
      }
      if (selected_image)
      {
        ImGui::SameLine();
        if (ImGui::Button("Extract and Play"))
        {
          extraction.completed = 0;
          extraction.total = 1;
          extraction.running = true;
          {
            std::lock_guard lock(extraction.mutex);
            extraction.error.clear();
            extraction.finished_game.reset();
          }
          const fs::path image = *selected_image;
          extraction_thread = std::jthread([image, user_directory, release_directory, &extraction]
                                           {
                                             ExtractDisc(image, user_directory,
                                                         release_directory, &extraction);
                                           });
        }
      }
    }
    else
    {
      const float progress =
          std::min(1.0f, static_cast<float>(extraction.completed.load()) / extraction.total.load());
      ImGui::ProgressBar(progress, ImVec2(-1, 0));
    }

    {
      std::lock_guard lock(extraction.mutex);
      if (!extraction.status.empty())
        ImGui::TextWrapped("%s", extraction.status.c_str());
      if (!extraction.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", extraction.error.c_str());
    }
    {
      std::lock_guard lock(dialog.mutex);
      if (!dialog.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", dialog.error.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Controller: %s", controller_status.c_str());
    ImGui::EndDisabled();
    if (module_setup.Running())
    {
      ImGui::Separator();
      ImGui::TextWrapped("First-time game setup. Your compiled game will be saved for future launches.");
      ImGui::TextWrapped("%s", module_setup.Status().c_str());
      ImGui::ProgressBar(module_setup.Progress(), ImVec2(-1, 0));
      if (ImGui::Button("Cancel setup"))
      {
        module_setup.Cancel();
        pending_launch = LaunchMode::None;
      }
    }
    if (!module_setup.Error().empty())
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", module_setup.Error().c_str());
    ImGui::End();

    // Use the same setup path for Play, extraction-and-play and netplay. Keep
    // rendering the launcher while the compiler works in a child process.
    if (done && launch_mode != LaunchMode::None && !module_setup_ready &&
        moderngekko::frontend::ModuleSetup::Available(release_directory))
    {
      pending_launch = launch_mode;
      launch_mode = LaunchMode::None;
      done = false;
      if (!module_setup.Running())
        module_setup.Start(release_directory, user_directory, current_game);
    }

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  if (extraction_thread.joinable())
    extraction_thread.join();
  module_setup.Cancel();

  // Release the input backends before the game process takes the devices
  // (or before exit): the runner initializes its own ControllerInterface.
  g_controller_interface.Shutdown();
  WiimoteReal::Shutdown();

  int result = 0;
  if (launch_mode != LaunchMode::None)
  {
    std::string launch_error;
    if (!WriteDefaultGame(user_directory, release_directory, current_game,
                          &launch_error))
    {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(), window);
      result = 1;
    }
    else
    {
      // SDL_CreateProcess expects UTF-8 arguments on every platform;
      // fs::path::string() would emit the ANSI code page on Windows.
      std::vector<std::string> argument_storage = {
          moderngekko::frontend::PathText(SiblingRunner(StringToPath(argv[0]))),
          "--game", moderngekko::frontend::PathText(current_game), "--user-dir",
          moderngekko::frontend::PathText(user_directory)};
      if (module_setup_ready)
      {
        argument_storage.emplace_back("--module");
        argument_storage.emplace_back(moderngekko::PathToUtf8(module_setup.Module()));
      }
      // Solo only: see the note by the dropdown.
      if (launch_mode == LaunchMode::Solo && selected_savestate >= 0 &&
          static_cast<std::size_t>(selected_savestate) < launcher_savestates.size())
      {
        argument_storage.emplace_back("--load-state");
        argument_storage.emplace_back(
            moderngekko::frontend::PathText(
                launcher_savestates[static_cast<std::size_t>(selected_savestate)]));
      }
      if (launch_mode == LaunchMode::Host)
        argument_storage.emplace_back("--netplay-host");
      else if (launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-join");
        argument_storage.emplace_back(config.netplay_address);
      }
      if (launch_mode == LaunchMode::Host || launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-port");
        argument_storage.emplace_back(std::to_string(config.netplay_port));
        argument_storage.emplace_back("--nickname");
        argument_storage.emplace_back(config.netplay_nickname);
        argument_storage.emplace_back("--buffer");
        argument_storage.emplace_back(config.netplay_buffer);
        for (const std::string& controller : config.controllers)
        {
          argument_storage.emplace_back("--controller");
          argument_storage.emplace_back(controller);
        }
      }
      if (use_wayland)
        argument_storage.emplace_back("--wayland");
#if defined(__linux__)
      else
        argument_storage.emplace_back("-X11");
#endif
      std::vector<const char*> arguments;
      arguments.reserve(argument_storage.size() + 1);
      for (const std::string& argument : argument_storage)
        arguments.push_back(argument.c_str());
      arguments.push_back(nullptr);
      const std::filesystem::path log_path =
          user_directory / "Logs" / MODERNGEKKO_LOG_FILENAME;
      std::error_code log_error;
      std::filesystem::create_directories(log_path.parent_path(), log_error);
      SDL_IOStream* log_stream =
          log_error ? nullptr
                    : SDL_IOFromFile(
                          moderngekko::frontend::PathText(log_path).c_str(),
                          "w");
      SDL_Process* process = nullptr;
      std::string process_error;
      if (log_stream)
      {
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties)
        {
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                 arguments.data());
          SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_REDIRECT);
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_stream);
          SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
                                 true);
          process = SDL_CreateProcessWithProperties(properties);
          SDL_DestroyProperties(properties);
        }
        if (!process)
          process_error = SDL_GetError();
        SDL_CloseIO(log_stream);
      }
      else
      {
        process = SDL_CreateProcess(arguments.data(), false);
        if (!process)
          process_error = SDL_GetError();
      }
      if (!process)
      {
        launch_error = "Could not start " +
                       moderngekko::frontend::PathText(
                           SiblingRunner(StringToPath(argv[0]))) +
                       ": " + process_error;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(),
                                 window);
        result = 1;
      }
      else
      {
        SDL_HideWindow(window);
        int exit_code = 1;
        const bool waited = SDL_WaitProcess(process, true, &exit_code);
        SDL_DestroyProcess(process);
        if (!waited || exit_code != 0)
        {
          SDL_ShowWindow(window);
          if (!waited)
          {
            launch_error =
                "The game process could not be monitored: " + std::string(SDL_GetError());
          }
          else if (launch_mode == LaunchMode::Join)
          {
            switch (static_cast<moderngekko::frontend::NetplayExitCode>(exit_code))
            {
            case moderngekko::frontend::NetplayExitCode::VersionMismatch:
              launch_error = "The host is running an incompatible netplay build. Both "
                             "players must use the same release.";
              break;
            case moderngekko::frontend::NetplayExitCode::CompatibilityMismatch:
              launch_error = "The extracted game or recomp module does not match the "
                             "host. Both players need the same game revision and "
                             "release.";
              break;
            case moderngekko::frontend::NetplayExitCode::RoomFull:
              launch_error = "All four controller slots are already occupied.";
              break;
            case moderngekko::frontend::NetplayExitCode::GameRunning:
              launch_error = "The host has already started the game.";
              break;
            case moderngekko::frontend::NetplayExitCode::ServerFull:
              launch_error = "The netplay server is full.";
              break;
            case moderngekko::frontend::NetplayExitCode::NicknameRejected:
              launch_error = "The nickname was rejected by the host.";
              break;
            default:
              launch_error = "Could not reach the netplay host. Check the host name, UDP "
                             "port, and firewall.";
              break;
            }
          }
          else if (launch_mode == LaunchMode::Host)
          {
            launch_error = "Could not create the netplay session. Check the UDP port "
                           "and firewall settings.";
          }
          else
          {
            launch_error = "The game process exited with code " + std::to_string(exit_code) + ".";
          }
          if (!log_error)
            launch_error +=
                "\n\nDetails were written to:\n" +
                moderngekko::frontend::PathText(log_path);
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Session failed", launch_error.c_str(),
                                   window);
          result = 1;
        }
      }
    }
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
