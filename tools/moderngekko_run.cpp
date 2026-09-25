#include "cache_affinity.hpp"
#include "frontend_config.hpp"
#include "dol_patch.hpp"
#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"
#include "netplay_session.hpp"
#include "runner/game_setup.hpp"

#include "Common/StringUtil.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// For the affinity/priority setup in main(). WIN32_LEAN_AND_MEAN keeps the
// winsock and GDI surface out of a translation unit that only needs the
// processor-topology and process calls.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace {
#ifndef MODERNGEKKO_RUNNER_NAME
#define MODERNGEKKO_RUNNER_NAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

volatile std::sig_atomic_t s_stop_requested = 0;

void HandleStopSignal(int) { s_stop_requested = 1; }

void Usage() {
  std::cerr << "usage: " MODERNGEKKO_RUNNER_NAME
               " [--game <extracted-root>] [--module <path>]\n"
               "       [--user-dir <path>] [--title <text>] [--load-state <path>]\n"
               "       [--save-state-on-exit <path>]\n"
               "       [--graphics <backend>] [--adapter <n>] [--clock <hz>] [--audio <backend>]\n"
               "       [--present-mode <fifo|mailbox|immediate>]\n"
               "       [--uncapped | --open-frame-rate | --max-fps <30-1000>] [--capped]\n"
               "       [--dump-audio <path>] [--background-input]\n"
               "       [--mods <directory>] [--no-mods]\n"
               "       [--wayland] [-X11] [--headless] [--allow-interpreter]\n"
               "       [--netplay-host | --netplay-join <host>] "
               "[--netplay-port <port>]\n"
               "       [--nickname <name>] [--buffer <auto|1-20>] "
               "[--controller <device>]...\n"
               "       With no --game, boots the path in "
               "<user-dir>/default-game.txt.\n"
               "       --uncapped (alias --open-frame-rate, env MODERNGEKKO_UNCAPPED=1,\n"
               "       config frame_rate=uncapped) opens rendering while guest time stays\n"
               "       at 1.0x. --max-fps selects a render cap; 0 means unlimited through\n"
               "       MODERNGEKKO_RENDER_HZ. Logic remains fixed at 30 Hz. Present defaults\n"
               "       to immediate; an explicit --present-mode always wins. --capped restores\n"
               "       the retail cadence and pins guest time to 1.0x.\n";
}

// default-game.txt read/write moved to src/runner/game_setup.* so the
// first-run/recovery flow and this file share one implementation
// (ReadSavedGameRoot / WriteSavedGameRoot).

std::filesystem::path DefaultUserDirectory() {
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
    return *home / ".local" / "share" / MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
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

std::filesystem::path ExecutableDirectory(const std::filesystem::path &argv0) {
  std::error_code ec;
#if defined(__linux__)
  const std::filesystem::path proc_executable =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const std::filesystem::path executable =
      std::filesystem::weakly_canonical(argv0, ec);
  return ec ? std::filesystem::current_path() : executable.parent_path();
}

// moderngekko::frontend::PresentMode and moderngekko::PresentMode share the
// {Fifo, Mailbox, Immediate} ordering today; pin it so a silent reorder can
// never reintroduce the enum-permutation bug (phase3-presentmode-g2g4.md).
moderngekko::PresentMode ToRuntimePresentMode(moderngekko::frontend::PresentMode mode)
{
  static_assert(static_cast<int>(moderngekko::frontend::PresentMode::Fifo) == 0 &&
                    static_cast<int>(moderngekko::frontend::PresentMode::Mailbox) == 1 &&
                    static_cast<int>(moderngekko::frontend::PresentMode::Immediate) == 2,
                "frontend PresentMode ordering changed; revisit mapping");
  switch (mode)
  {
  case moderngekko::frontend::PresentMode::Fifo:
    return moderngekko::PresentMode::Fifo;
  case moderngekko::frontend::PresentMode::Mailbox:
    return moderngekko::PresentMode::Mailbox;
  case moderngekko::frontend::PresentMode::Immediate:
    return moderngekko::PresentMode::Immediate;
  }
  return moderngekko::PresentMode::Fifo;
}

moderngekko::AspectRatio ToRuntimeAspectRatio(moderngekko::frontend::AspectRatio mode)
{
  switch (mode)
  {
  case moderngekko::frontend::AspectRatio::Auto:
    return moderngekko::AspectRatio::Auto;
  case moderngekko::frontend::AspectRatio::Wide:
    return moderngekko::AspectRatio::Wide;
  case moderngekko::frontend::AspectRatio::Standard:
    return moderngekko::AspectRatio::Standard;
  case moderngekko::frontend::AspectRatio::Stretch:
    return moderngekko::AspectRatio::Stretch;
  case moderngekko::frontend::AspectRatio::Custom:
    return moderngekko::AspectRatio::Custom;
  }
  return moderngekko::AspectRatio::Auto;
}

moderngekko::Anisotropy ToRuntimeAnisotropy(moderngekko::frontend::Anisotropy mode)
{
  switch (mode)
  {
  case moderngekko::frontend::Anisotropy::Default:
    return moderngekko::Anisotropy::Default;
  case moderngekko::frontend::Anisotropy::X1:
    return moderngekko::Anisotropy::X1;
  case moderngekko::frontend::Anisotropy::X2:
    return moderngekko::Anisotropy::X2;
  case moderngekko::frontend::Anisotropy::X4:
    return moderngekko::Anisotropy::X4;
  case moderngekko::frontend::Anisotropy::X8:
    return moderngekko::Anisotropy::X8;
  case moderngekko::frontend::Anisotropy::X16:
    return moderngekko::Anisotropy::X16;
  }
  return moderngekko::Anisotropy::Default;
}

moderngekko::WindowMode ToRuntimeWindowMode(moderngekko::frontend::WindowMode mode)
{
  switch (mode)
  {
  case moderngekko::frontend::WindowMode::Windowed:
    return moderngekko::WindowMode::Windowed;
  case moderngekko::frontend::WindowMode::Maximized:
    return moderngekko::WindowMode::Maximized;
  case moderngekko::frontend::WindowMode::Fullscreen:
    return moderngekko::WindowMode::Fullscreen;
  }
  return moderngekko::WindowMode::Windowed;
}

moderngekko::AntiAliasing ToRuntimeAntiAliasing(moderngekko::frontend::AntiAliasing mode)
{
  switch (mode)
  {
  case moderngekko::frontend::AntiAliasing::Off:
    return moderngekko::AntiAliasing::Off;
  case moderngekko::frontend::AntiAliasing::Fxaa:
    return moderngekko::AntiAliasing::Fxaa;
  case moderngekko::frontend::AntiAliasing::Msaa2x:
    return moderngekko::AntiAliasing::Msaa2x;
  case moderngekko::frontend::AntiAliasing::Msaa4x:
    return moderngekko::AntiAliasing::Msaa4x;
  case moderngekko::frontend::AntiAliasing::Msaa8x:
    return moderngekko::AntiAliasing::Msaa8x;
  }
  return moderngekko::AntiAliasing::Off;
}

// FrameUncap: tri-state open-rate request. CLI > env > config file; default
// capped (nullopt everywhere) preserves exact current behavior.
std::optional<bool> UncappedFromEnv()
{
  const char* raw = std::getenv("MODERNGEKKO_UNCAPPED");
  if (!raw || !raw[0])
    return std::nullopt;
  // Reuse the config vocabulary so env/config/CLI spell values one way.
  return moderngekko::frontend::ParseFrameRate(raw);
}

std::optional<unsigned int> RenderHzFromEnv()
{
  const char* raw = std::getenv("MODERNGEKKO_RENDER_HZ");
  if (!raw || !raw[0])
    return std::nullopt;
  unsigned int hz = 0;
  const char* end = raw + std::char_traits<char>::length(raw);
  const auto parsed = std::from_chars(raw, end, hz);
  if (parsed.ec != std::errc{} || parsed.ptr != end || hz > 1000u ||
      (hz != 0u && hz < 30u))
    return std::nullopt;
  return hz;
}
} // namespace

int RunMain(int argc, char **argv) {
#if defined(_WIN32)
  if (!std::getenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"))
    _putenv_s("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1");
#else
  setenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
#endif
  // The shipped release engages frame60-accum by default (60 fps accumulator
  // + J3D interpolation + painter-on-R-frames) regardless of which entry
  // point launched us -- the .cmd/.ps1 wrappers used to be the only place
  // these were set, so launching the ImGui UI or the exe directly silently
  // dropped back to 30 Hz scene cadence. The mod reads them in on_load
  // during Runtime::Create, so they must exist before it. Precedence is
  // preserved: an explicit MODERNGEKKO_FRAME60_ACCUM=0 still opts out, and
  // these vars are inert when no frame60 mod is loaded.
  const auto default_env = [](const char *name, const char *value) {
    if (std::getenv(name))
      return;
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
  };
  default_env("MODERNGEKKO_FRAME60_ACCUM", "1");
  default_env("MODERNGEKKO_J3D_INTERP", "1");
  default_env("MODERNGEKKO_RFRAME_RENDER", "1");
  // GZLE01's idle-loop PC hint for the recompiler's throttle detection;
  // inert for other games (a non-matching pc is simply never hit).
  default_env("STATICRECOMP_IDLE_PCS", "80307ef4@-0x66b0");
  moderngekko::RuntimeConfig config;
  const std::filesystem::path executable_directory =
      ExecutableDirectory(StringToPath(argv[0]));
  {
    // Release-bundle layout: a package ships its seeded user dir at
    // <exe>/assets/user-dir (Dolphin.ini defaults, shader caches, memcard).
    // Prefer it over the roaming profile dir when present so the packaged
    // configuration applies no matter which entry point launched us; absent
    // (build tree, bare exe), fall back to the profile location.
    std::error_code bundled_ec;
    const std::filesystem::path bundled_user_dir =
        executable_directory / "assets" / "user-dir";
    config.user_directory =
        std::filesystem::is_directory(bundled_user_dir, bundled_ec)
            ? bundled_user_dir
            : DefaultUserDirectory();
  }
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  config.window_title = MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#endif
  std::filesystem::path module_path;
  bool use_default_mods = true;
  std::optional<bool> cli_uncapped;
  std::optional<unsigned int> cli_render_hz;
  bool present_mode_explicit = false;
  std::optional<moderngekko::frontend::NetplayRole> netplay_role;
  std::string netplay_address;
  std::optional<std::uint16_t> netplay_port;
  std::string netplay_nickname;
  std::string netplay_buffer;
  std::vector<std::string> netplay_controllers;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char *option) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << option << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--game")
      config.game_root = StringToPath(value("--game"));
    else if (arg == "--module")
      module_path = StringToPath(value("--module"));
    else if (arg == "--user-dir")
      config.user_directory = StringToPath(value("--user-dir"));
    else if (arg == "--title")
      config.window_title = value("--title");
    else if (arg == "--load-state")
    {
      // Checked here rather than left to the boot path: a state that is not
      // there would otherwise boot to the title screen and look like it worked.
      std::filesystem::path state = StringToPath(value("--load-state"));
      if (!std::filesystem::is_regular_file(state))
      {
        std::cerr << "savestate not found: " << PathToString(state) << '\n';
        return 2;
      }
      config.load_state_path = std::move(state);
    }
    else if (arg == "--save-state-on-exit")
    {
      config.save_state_on_exit_path = StringToPath(value("--save-state-on-exit"));
      // Anchor capture writes into fresh per-run directories; create the
      // parent eagerly so a typo surfaces as a loud runtime dump failure
      // rather than a missing-directory surprise at exit.
      const std::filesystem::path parent =
          config.save_state_on_exit_path->parent_path();
      if (!parent.empty())
      {
        std::error_code mkdir_ec;
        std::filesystem::create_directories(parent, mkdir_ec);
      }
    }
    else if (arg == "--graphics")
      config.graphics.backend = value("--graphics");
    else if (arg == "--adapter") {
      const std::string adapter_value = value("--adapter");
      int adapter = -1;
      const auto parsed =
          std::from_chars(adapter_value.data(),
                          adapter_value.data() + adapter_value.size(), adapter);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != adapter_value.data() + adapter_value.size() ||
          adapter < 0 || adapter > 16) {
        std::cerr << "--adapter must be an integer 0..16\n";
        return 2;
      }
      config.graphics.adapter = adapter;
    } else if (arg == "--present-mode") {
      const std::string raw = value("--present-mode");
      auto m = moderngekko::frontend::ParsePresentMode(raw);
      if (!m) {
        std::cerr << "--present-mode must be fifo, mailbox, or immediate\n";
        Usage();
        return 2;
      }
      config.graphics.present_mode = ToRuntimePresentMode(*m);
      present_mode_explicit = true;
    } else if (arg == "--uncapped" || arg == "--open-frame-rate") {
      cli_uncapped = true;
      cli_render_hz = 0u;
    } else if (arg == "--max-fps") {
      const std::string raw = value("--max-fps");
      unsigned int hz = 0;
      const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), hz);
      if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() ||
          hz < 30u || hz > 1000u) {
        std::cerr << "--max-fps must be between 30 and 1000\n";
        return 2;
      }
      cli_uncapped = true;
      cli_render_hz = hz;
    } else if (arg == "--capped") {
      cli_uncapped = false;
      cli_render_hz.reset();
    } else if (arg == "--clock") {
      const std::string clock_value = value("--clock");
      u64 clock_hz = 0;
      const auto parsed =
          std::from_chars(clock_value.data(),
                          clock_value.data() + clock_value.size(), clock_hz);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != clock_value.data() + clock_value.size()) {
        std::cerr << "--clock must be a non-negative integer in Hz\n";
        return 2;
      }
      config.guest_clock_hz = clock_hz;
    }
    else if (arg == "--audio")
      config.audio.backend = value("--audio");
    else if (arg == "--dump-audio")
      config.audio.dump_audio_path = value("--dump-audio");
    else if (arg == "--background-input")
      config.input.background_input = true;
    else if (arg == "--mods")
      config.mod_directories.emplace_back(StringToPath(value("--mods")));
    else if (arg == "--no-mods")
      use_default_mods = false;
    else if (arg == "-X11" || arg == "--x11")
      config.window_system = moderngekko::WindowSystem::X11;
    else if (arg == "--wayland")
      config.window_system = moderngekko::WindowSystem::Wayland;
    else if (arg == "--headless")
      config.headless = true;
    else if (arg == "--allow-interpreter")
      config.allow_interpreter = true;
    else if (arg == "--netplay-host")
      netplay_role = moderngekko::frontend::NetplayRole::Host;
    else if (arg == "--netplay-join") {
      netplay_role = moderngekko::frontend::NetplayRole::Join;
      netplay_address = value("--netplay-join");
    } else if (arg == "--netplay-port") {
      const std::string port_value = value("--netplay-port");
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          port_value.data(), port_value.data() + port_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != port_value.data() + port_value.size() || port == 0 ||
          port > 65535) {
        std::cerr << "--netplay-port must be between 1 and 65535\n";
        return 2;
      }
      netplay_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--nickname")
      netplay_nickname = value("--nickname");
    else if (arg == "--buffer")
      netplay_buffer = value("--buffer");
    else if (arg == "--controller")
      netplay_controllers.emplace_back(value("--controller"));
    else if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      Usage();
      return 2;
    }
  }
  const bool game_from_cli = !config.game_root.empty();
  std::optional<std::filesystem::path> saved_game;
  if (config.game_root.empty()) {
    saved_game = moderngekko::runner::ReadSavedGameRoot(config.user_directory,
                                                       executable_directory);
    if (saved_game)
      config.game_root = *saved_game;
  }
  // One spelling for every later stat: reduce a hand-typed or saved
  // \\?\-prefixed root to its natural form — InspectGame canonicalizes to
  // natural anyway, and the probe must judge the same path InspectGame sees.
  config.game_root =
      moderngekko::runner::NormalizeGameRootPath(config.game_root);
  {
    // A configured path that fails the light probe either gets the interactive
    // explain/browse/recover flow (Windows, non-headless, not an explicit
    // --game) or a structured stderr rejection. An explicit --game stays
    // strict so automation can never block on a modal dialog.
    const auto probe = moderngekko::runner::ProbeExtractedGame(
        config.game_root, executable_directory);
    if (!probe.ok) {
      if (game_from_cli ||
          !moderngekko::runner::GamePromptsAvailable(config.headless)) {
        std::cerr << moderngekko::runner::DescribeUnusableGameRoot(
            config.game_root, probe, saved_game.has_value(), game_from_cli,
            config.user_directory, executable_directory);
        if (config.game_root.empty())
          Usage();
        return 2;
      }
      moderngekko::runner::GameSetupRequest request;
      request.previously_configured = saved_game.has_value();
      request.current = config.game_root;
      request.rejection_reason =
          moderngekko::runner::DescribeProbeProblems(probe);
      std::string setup_error;
      if (!moderngekko::runner::PromptForValidGameRoot(
              request, config.user_directory, executable_directory,
              &config.game_root, &setup_error)) {
        std::cerr << setup_error << '\n';
        return 2;
      }
    }
  }

  auto frontend_config =
      moderngekko::frontend::LoadConfig(config.user_directory, true);
  if (!frontend_config) {
    std::cerr << "invalid config.ini: " << frontend_config.error << '\n';
    return 2;
  }
  config.graphics.internal_resolution_scale = frontend_config.dolphin_scale;
  if (!config.graphics.adapter && frontend_config.graphics_adapter)
    config.graphics.adapter = frontend_config.graphics_adapter;
  if (frontend_config.aspect_ratio) {
    config.graphics.aspect_ratio =
        ToRuntimeAspectRatio(frontend_config.aspect_ratio->mode);
    if (frontend_config.aspect_ratio->mode ==
        moderngekko::frontend::AspectRatio::Custom)
      config.graphics.custom_aspect = {frontend_config.aspect_ratio->width,
                                       frontend_config.aspect_ratio->height};
  }
  if (frontend_config.widescreen_hack)
    config.graphics.widescreen_hack = *frontend_config.widescreen_hack;
  // With 16:9 output (or a custom target aspect), the widescreen16x9 mod
  // (native port of Dolphin's GZLE01 Gecko code) is the correct widescreen
  // path: it widens the game's projection and relocates HUD panes, while the
  // generic vertex hack also fat-scales 2D elements. Default the mod on — an
  // explicit MODERNGEKKO_WIDESCREEN=0 still opts out — and force the hack off
  // whenever the mod is active, since stacking them would widen twice. The
  // mod's layout target must match Dolphin's presentation stretch, so feed
  // it the same W:H pair through MODERNGEKKO_WIDESCREEN_ASPECT ("16:9" for
  // Wide — identical to the mod's default); an explicit env value wins like
  // the other default_env overrides.
  if (config.graphics.aspect_ratio &&
      (*config.graphics.aspect_ratio == moderngekko::AspectRatio::Wide ||
       *config.graphics.aspect_ratio == moderngekko::AspectRatio::Custom)) {
    const std::string aspect_env =
        config.graphics.custom_aspect
            ? std::to_string(config.graphics.custom_aspect->first) + ":" +
                  std::to_string(config.graphics.custom_aspect->second)
            : "16:9";
    default_env("MODERNGEKKO_WIDESCREEN", "1");
    default_env("MODERNGEKKO_WIDESCREEN_ASPECT", aspect_env.c_str());
  }
  if (const char* ws_env = std::getenv("MODERNGEKKO_WIDESCREEN");
      ws_env && (ws_env[0] == '1' || ws_env[0] == 'y' || ws_env[0] == 'Y' ||
                 ws_env[0] == 't' || ws_env[0] == 'T'))
    config.graphics.widescreen_hack = false;
  if (frontend_config.anisotropy)
    config.graphics.anisotropy = ToRuntimeAnisotropy(*frontend_config.anisotropy);
  if (frontend_config.aa)
    config.graphics.aa = ToRuntimeAntiAliasing(*frontend_config.aa);
  // MODERNGEKKO_AA (off|fxaa|2x|4x|8x) overrides config.ini's [Video] aa=.
  // Same env philosophy as MODERNGEKKO_UNCAPPED/MODERNGEKKO_CYCLES_PER_SECOND:
  // an invalid value warns and falls back to the config file rather than
  // refusing to boot.
  if (const char* aa_env = std::getenv("MODERNGEKKO_AA");
      aa_env && aa_env[0])
  {
    if (const auto aa = moderngekko::frontend::ParseAntiAliasing(aa_env))
      config.graphics.aa = ToRuntimeAntiAliasing(*aa);
    else
      std::cerr << "warning: ignoring invalid MODERNGEKKO_AA='" << aa_env
                << "' (expected off, fxaa, 2x, 4x, or 8x)\n";
  }
  // CLI --background-input wins over config.ini; absent on both leaves
  // Dolphin's own Input/BackgroundInput untouched (InputSettings is tri-state).
  if (!config.input.background_input && frontend_config.background_input)
    config.input.background_input = *frontend_config.background_input;
  if (config.graphics.backend.empty())
    config.graphics.backend = frontend_config.graphics_backend;
  if (!config.graphics.present_mode && frontend_config.present_mode)
    config.graphics.present_mode = ToRuntimePresentMode(*frontend_config.present_mode);
  if (config.headless && (config.graphics.present_mode || frontend_config.present_mode)) {
    std::cerr << "warning: --present-mode ignored in --headless (Null backend has no swapchain)\n";
  }
  // FrameUncap: resolve open-rate request (CLI > env > config; default capped
  // = exact current behavior). Rendering is decoupled through the bundled
  // accumulator while the emulated machine remains wall-clock locked at 1.0x.
  // An explicit --present-mode (CLI or config) always wins.
  {
    bool uncapped = false;
    const char* uncapped_source = nullptr;
    std::optional<unsigned int> render_hz;
    if (frontend_config.uncapped) {
      uncapped = *frontend_config.uncapped;
      render_hz = 0u;
      uncapped_source = "config";
    }
    if (const auto env = UncappedFromEnv()) {
      uncapped = *env;
      render_hz = 0u;
      uncapped_source = "env";
    }
    if (const auto env_hz = RenderHzFromEnv()) {
      uncapped = true;
      render_hz = *env_hz;
      uncapped_source = "env";
    }
    if (cli_uncapped) {
      uncapped = *cli_uncapped;
      render_hz = cli_render_hz.value_or(0u);
      uncapped_source = "cli";
    }
    if (uncapped) {
      if (!present_mode_explicit && !config.graphics.present_mode &&
          !frontend_config.present_mode) {
        config.graphics.present_mode = moderngekko::PresentMode::Immediate;
      }
      config.emulation_speed = 1.0f;
      config.graphics.immediate_xfb = true;
      const std::string render_hz_value = std::to_string(render_hz.value_or(0u));
#if defined(_WIN32)
      _putenv_s("MODERNGEKKO_FRAME60_ACCUM", "1");
      _putenv_s("MODERNGEKKO_RENDER_HZ", render_hz_value.c_str());
#else
      setenv("MODERNGEKKO_FRAME60_ACCUM", "1", 1);
      setenv("MODERNGEKKO_RENDER_HZ", render_hz_value.c_str(), 1);
#endif
      std::cout << "[frame-uncap] render mode (from " << uncapped_source << "): "
                << (render_hz.value_or(0u) == 0u ? "unlimited" : render_hz_value + " FPS")
                << ", 30 Hz logic, 1.0x guest time\n";
    } else if (uncapped_source != nullptr) {
      // Explicit --capped pins 1.0x over any stale user-dir EmulationSpeed;
      // the default-capped path (nothing requested) touches nothing.
      config.emulation_speed = 1.0f;
    }
  }
  config.window_mode =
      ToRuntimeWindowMode(frontend_config.EffectiveWindowMode());
  config.show_fps_in_title = frontend_config.show_fps_in_title;
  if (use_default_mods) {
    config.mod_directories.push_back(executable_directory / "Mods");
    config.mod_directories.push_back(config.user_directory / "Mods");
  }

  if (!netplay_role && !frontend_config.controller.empty()) {
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, frontend_config.controller,
            &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::cout << "controller configuration: " << controller_message << '\n';
  }

  moderngekko::GameInspectResult inspected;
  for (;;) {
#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
    // Inside the loop so a folder picked after a relocation prompt gets the
    // same patch-then-inspect order as the original path.
    bool dol_changed = false;
    std::string dol_patch_error;
    if (!moderngekko::frontend::ApplyDolPatchManifest(
            config.game_root / "sys" / "main.dol",
            executable_directory / MODERNGEKKO_DOL_PATCH_MANIFEST, &dol_changed,
            &dol_patch_error)) {
      std::cerr << "DOL patching failed: " << dol_patch_error << '\n';
      return 2;
    }
    if (dol_changed)
      std::cout << "Applied native DOL patches\n";
#endif
    inspected = moderngekko::InspectGame(config.game_root);
    if (inspected)
      break;
    // Deep-check failure on an explicit --game or in a non-interactive run
    // keeps the strict stderr path. Anything the user configured through the
    // picker or default-game.txt can be relocated instead of dead-ending.
    if (game_from_cli ||
        !moderngekko::runner::GamePromptsAvailable(config.headless))
      break;
    moderngekko::runner::GameSetupRequest request;
    request.previously_configured = true;
    request.current = config.game_root;
    request.rejection_reason = inspected.error + "\n";
    std::string setup_error;
    if (!moderngekko::runner::PromptForValidGameRoot(
            request, config.user_directory, executable_directory,
            &config.game_root, &setup_error)) {
      std::cerr << setup_error << '\n';
      return 2;
    }
  }
  if (!inspected) {
    std::cerr << "invalid game: " << inspected.error << '\n';
    return 2;
  }

#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (inspected.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID) {
    std::cerr << "unsupported disc ID: expected "
              << MODERNGEKKO_REQUIRED_DISC_ID << ", got "
              << inspected.metadata->disc_id << '\n';
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (inspected.metadata->dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256) {
    std::cerr << "unsupported main DOL: this release requires its pinned game build\n";
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (inspected.metadata->rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256) {
    std::cerr << "unsupported _Main.rel: this release requires its pinned game build\n";
    return 2;
  }
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (inspected.metadata->assets_sha256 !=
      MODERNGEKKO_REQUIRED_ASSETS_SHA256) {
    std::cerr << "unsupported game assets: this release requires its pinned game build\n";
    return 2;
  }
#endif

  // Compatibility discovery belongs to the runner, never the runtime library.
  if (module_path.empty()) {
    if (const auto env =
            moderngekko::frontend::EnvironmentPath("STATICRECOMP_MODULE"))
      module_path = *env;
    else {
      const std::string module_name =
          "g" + inspected.metadata->disc_id + "_recomp" + LibrarySuffix();
      const auto bundled = executable_directory / module_name;
      const auto user_module =
          config.user_directory / "StaticRecompModules" / module_name;
      if (std::filesystem::is_regular_file(bundled))
        module_path = bundled;
      else if (std::filesystem::is_regular_file(user_module))
        module_path = user_module;
    }
  }
  if (!module_path.empty())
    config.module =
        moderngekko::ModuleSource::DynamicPath(std::move(module_path));

#if defined(__linux__) || defined(_WIN32)
  if (!config.headless && config.graphics.backend.empty())
    config.graphics.backend = "Vulkan";
#endif

  if (netplay_role) {
    moderngekko::frontend::NetplayOptions options;
    options.role = *netplay_role;
    options.address = netplay_address.empty() ? frontend_config.netplay_address
                                              : netplay_address;
    options.port = netplay_port.value_or(frontend_config.netplay_port);
    options.nickname = netplay_nickname.empty()
                           ? frontend_config.netplay_nickname
                           : netplay_nickname;
    options.buffer = netplay_buffer.empty() ? frontend_config.netplay_buffer
                                            : netplay_buffer;
    const std::vector<std::string> configured_controllers =
        moderngekko::frontend::ReadConfiguredControllers(config.user_directory);
    options.controllers =
        netplay_controllers.empty()
            ? (configured_controllers.empty() ? frontend_config.controllers
                                              : configured_controllers)
            : netplay_controllers;
    if (options.controllers.empty() && !frontend_config.controller.empty())
      options.controllers.push_back(frontend_config.controller);
    if (options.controllers.empty()) {
      std::cerr << "netplay requires at least one selected controller\n";
      return 2;
    }
    frontend_config.netplay_address = options.address;
    frontend_config.netplay_port = options.port;
    frontend_config.netplay_nickname = options.nickname;
    frontend_config.netplay_buffer = options.buffer;
    frontend_config.controllers = options.controllers;
    frontend_config.controller = options.controllers.front();
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, options.controllers, &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::string save_error;
    if (!moderngekko::frontend::SaveConfig(config.user_directory,
                                           frontend_config, &save_error)) {
      std::cerr << "configuration: " << save_error << '\n';
      return 2;
    }
    std::cerr << "netplay: runner started\n";
    return moderngekko::frontend::RunNetplayLobby(
        std::move(config), std::move(frontend_config), std::move(options));
  }

  // Hand the inspection we already paid for to Create() — otherwise it
  // re-hashes the whole files/ tree a second time inside Runtime::Create.
  config.preinspected_metadata = inspected.metadata;
  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created) {
    std::cerr << "initialization failed: " << created.error->message << '\n';
    return 1;
  }
  std::cout << "audio backend: " << created.runtime->GetConfig().audio.backend
            << '\n';

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  std::jthread signal_watcher([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (s_stop_requested) {
        s_stop_requested = 0;
        created.runtime->RequestStop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  const moderngekko::RuntimeRunResult result = created.runtime->Run();
  signal_watcher.request_stop();
  if (result.error) {
    std::cerr << "runtime failed: " << result.error->message << '\n';
    return 1;
  }
  return 0;
}

#if defined(_WIN32)
// Last-chance crash reporting. A native access violation in generated guest
// code or a runtime bug otherwise exits the process silently -- in launcher
// logs that is indistinguishable from a clean quit (the "kind of crashes"
// class of report). The filter writes logs\crash-<pid>-<ticks>.dmp and a
// matching .txt next to the executable (falling back to
// %LOCALAPPDATA%\ModernGekko\logs, then %TEMP%), prints one stderr line, and
// returns EXCEPTION_CONTINUE_SEARCH so WER/debuggers still see the fault.
// Registered before any threads start; only fires when nothing else handled
// the exception, so it never runs on handled fastmem first-chance traps.
void InstallCrashReporter() {
  SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *ep) -> LONG {
    wchar_t base_dir[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, base_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
      len = GetCurrentDirectoryW(MAX_PATH, base_dir);
    std::wstring dir(base_dir);
    const std::wstring::size_type slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
      dir.erase(slash);
    dir += L"\\logs";
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      wchar_t fallback[MAX_PATH]{};
      const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
      if (local && *local)
        dir = std::wstring(local) + L"\\ModernGekko\\logs";
      else if (GetTempPathW(MAX_PATH, fallback))
        dir = fallback;
      CreateDirectoryW(dir.c_str(), nullptr);
    }
    wchar_t stem[MAX_PATH]{};
    swprintf_s(stem, L"\\crash-%lu-%llu", GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));
    const std::wstring dmp_path = dir + stem + L".dmp";
    const std::wstring txt_path = dir + stem + L".txt";

    HANDLE dmp = CreateFileW(dmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dmp != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION mei{};
      mei.ThreadId = GetCurrentThreadId();
      mei.ExceptionPointers = ep;
      mei.ClientPointers = FALSE;
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dmp,
                        static_cast<MINIDUMP_TYPE>(MiniDumpNormal |
                                                   MiniDumpWithThreadInfo),
                        &mei, nullptr, nullptr);
      CloseHandle(dmp);
    }

    HANDLE txt = CreateFileW(txt_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (txt != INVALID_HANDLE_VALUE) {
      const EXCEPTION_RECORD &rec = *ep->ExceptionRecord;
      wchar_t module[MAX_PATH] = L"?";
      HMODULE fault_module = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(rec.ExceptionAddress),
                             &fault_module))
        GetModuleFileNameW(fault_module, module, MAX_PATH);
      char line[1024];
      const int n = std::snprintf(
          line, sizeof(line),
          "moderngekko-run crash\nexception=0x%08lX address=%p module=%ls\n"
          "code_flags=0x%08lX params=%llu thread=%lu pid=%lu\n",
          rec.ExceptionCode, rec.ExceptionAddress, module,
          rec.ExceptionFlags, static_cast<unsigned long long>(rec.NumberParameters),
          GetCurrentThreadId(), GetCurrentProcessId());
      DWORD written = 0;
      WriteFile(txt, line, static_cast<DWORD>(n > 0 ? n : 0), &written, nullptr);
      for (DWORD i = 0; i < rec.NumberParameters && i < 8; ++i) {
        const int m = std::snprintf(line, sizeof(line), "param[%lu]=0x%llX\n", i,
                                    static_cast<unsigned long long>(
                                        rec.ExceptionInformation[i]));
        WriteFile(txt, line, static_cast<DWORD>(m > 0 ? m : 0), &written,
                  nullptr);
      }
      CloseHandle(txt);
    }

    std::fprintf(stderr, "[crash] unhandled exception 0x%08lX at %p; dump: %ls\n",
                 ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress, dmp_path.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
  });
}
#endif

#if defined(_WIN32)
// Optionally confine the process to the cores that share the largest L3.
//
// The emulated CPU is a single serial instruction stream, so nothing here is
// about parallelism -- core usage stays at 1.00 either way. It is about cache
// residency. A recompiled module is one dispatch switch spanning the whole
// game, which lives or dies on staying in L3, and on a chip with 3D V-Cache on
// one CCD only, Windows migrating the thread between dies makes it repeatedly
// lose its working set. Measured on a 9950X3D: 55.41 -> 70.42 fps, +27%.
//
// This applies to every Dolphin thread, not only the emulated CPU thread, so it
// is on by default but reversible. Set MODERNGEKKO_CACHE_AFFINITY=0 to disable
// it after benchmarking the target machine.
void PinProcessToLargestCache() {
  if (!moderngekko::frontend::AffinityEnabled(
          std::getenv("MODERNGEKKO_CACHE_AFFINITY")))
    return;

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
  if (length == 0)
    return;
  std::vector<char> buffer(length);
  if (!GetLogicalProcessorInformationEx(
          RelationCache,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length))
    return;

  const moderngekko::frontend::CacheDomain domain =
      moderngekko::frontend::LargestSharedCache(buffer.data(), length);
  if (!domain)
    return;

  if (moderngekko::frontend::ApplyCacheDomain(GetCurrentProcess(), domain).affinity_set) {
    std::cout << "[perf] pinned to the cores sharing the largest L3 (" << (domain.size >> 20)
              << " MB), mask 0x" << std::hex << static_cast<unsigned long long>(domain.mask)
              << std::dec << '\n';
  }
}
#endif

int main(int argc, char **argv) {
  try {
#if defined(_WIN32)
    InstallCrashReporter();
    PinProcessToLargestCache();
    // The CRT encodes argv in the ANSI code page, which cannot represent
    // paths outside it (a user dir under e.g. C:\Users\宮本). Rebuild argv
    // from the wide command line as UTF-8; the launcher sends UTF-8 through
    // SDL_CreateProcess already, and every path option decodes via
    // StringToPath. On failure keep the CRT argv rather than lose the run.
    std::vector<std::string> utf8_args =
        Common::CommandLineToUtf8Argv(GetCommandLineW());
    std::vector<char *> utf8_argv;
    utf8_argv.reserve(utf8_args.size() + 1);
    for (std::string &argument : utf8_args)
      utf8_argv.push_back(argument.data());
    utf8_argv.push_back(nullptr);
    if (!utf8_args.empty()) {
      argc = static_cast<int>(utf8_args.size());
      argv = utf8_argv.data();
    }
#endif
    return RunMain(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "fatal error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "fatal error: unknown exception\n";
  }
  return 1;
}
