#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include <cstdlib>
#include "Core/HW/GBACore.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/State.h"
#include "Core/StateFile.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "present_mode_map.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <mutex>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// The AttachedDescriptor path reinterpret_casts ModernGekkoModuleDesc* to
// StaticRecompModuleDesc* below. Two definitions of the StaticRecomp* types
// exist: module_abi.h's aliases (which win whenever that header is included
// first — as here, via runtime.hpp — and suppress the vendored definitions)
// and the vendored copy in StaticRecompABI.h (active in module-side TUs that
// never see module_abi.h). Under the alias regime these asserts compare a
// type to itself — tautological, but they become a live cross-check of the
// two layouts if a TU ever includes StaticRecompABI.h first. The hard wire
// pin is the literal-offset assert block in module_abi.h plus the
// abi_version/cpu_state_size checks at module load.
static_assert(sizeof(ModernGekkoRange) == sizeof(StaticRecompRange));
static_assert(offsetof(ModernGekkoRange, end) == offsetof(StaticRecompRange, end));
static_assert(sizeof(ModernGekkoRelSection) == sizeof(StaticRecompRelSection));
static_assert(offsetof(ModernGekkoRelSection, size) ==
              offsetof(StaticRecompRelSection, size));
static_assert(sizeof(ModernGekkoRelModule) == sizeof(StaticRecompRelModule));
static_assert(offsetof(ModernGekkoRelModule, sections) ==
              offsetof(StaticRecompRelModule, sections));
static_assert(offsetof(ModernGekkoRelModule, num_sections) ==
              offsetof(StaticRecompRelModule, num_sections));
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, dispatch) ==
              offsetof(StaticRecompModuleDesc, dispatch));
static_assert(offsetof(ModernGekkoModuleDesc, code_ranges) ==
              offsetof(StaticRecompModuleDesc, code_ranges));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_ranges) ==
              offsetof(StaticRecompModuleDesc, chunk_ranges));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
static_assert(offsetof(ModernGekkoModuleDesc, rel_modules) ==
              offsetof(StaticRecompModuleDesc, rel_modules));
static_assert(offsetof(ModernGekkoModuleDesc, num_rel_modules) ==
              offsetof(StaticRecompModuleDesc, num_rel_modules));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
// Game-id repair state for direct DOL boots. BootParameters::Executable
// derives the running game id from the file name via SConfig::MakeGameID —
// sys/main.dol becomes "ID-main" — so GameSettings/<id>.ini and
// Load/Textures/<id>/ would never resolve the real disc id (GZLE01).
// Host_TitleChanged swaps it in; armed in Create, cleared in ~Runtime.
std::string s_running_game_id;
std::string s_executable_game_id;
bool s_artifact_title_callback_seen = false;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;

AspectMode ToDolphinAspectRatio(moderngekko::AspectRatio ratio) {
  switch (ratio) {
    case moderngekko::AspectRatio::Auto:
      return AspectMode::Auto;
    case moderngekko::AspectRatio::Wide:
      // The PC frontend promises an exact 16:9 output. ForceWide preserves
      // the analog VI blanking ratio (Wind Waker can end up near 1.67:1),
      // while the native widescreen mod already supplies a 16:9 projection.
      return AspectMode::CustomStretch;
    case moderngekko::AspectRatio::Standard:
      return AspectMode::ForceStandard;
    case moderngekko::AspectRatio::Stretch:
      return AspectMode::Stretch;
    case moderngekko::AspectRatio::Custom:
      return AspectMode::Custom;
  }
  return AspectMode::Auto;
}

AnisotropicFilteringMode
ToDolphinAnisotropy(moderngekko::Anisotropy anisotropy) {
  switch (anisotropy) {
    case moderngekko::Anisotropy::Default:
      return AnisotropicFilteringMode::Default;
    case moderngekko::Anisotropy::X1:
      return AnisotropicFilteringMode::Force1x;
    case moderngekko::Anisotropy::X2:
      return AnisotropicFilteringMode::Force2x;
    case moderngekko::Anisotropy::X4:
      return AnisotropicFilteringMode::Force4x;
    case moderngekko::Anisotropy::X8:
      return AnisotropicFilteringMode::Force8x;
    case moderngekko::Anisotropy::X16:
      return AnisotropicFilteringMode::Force16x;
  }
  return AnisotropicFilteringMode::Default;
}

void PumpShutdownMessages() {
#ifdef _WIN32
  // SetWindowText from the title worker sends a synchronous window message.
  // Keep the owning thread responsive after Platform::MainLoop has returned.
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
#endif
}

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  const auto now = std::chrono::steady_clock::now();
  std::string formatted_title = fmt::format("{} | {:.1f} FPS", title, fps);
  const NetPlay::InputWaitTelemetry telemetry =
      NetPlay::NetPlayClient::GetInputWaitTelemetry();
  if (!telemetry.active) {
    s_previous_net_wait_ns = 0;
    s_net_wait_ms_per_second = 0.0;
    s_previous_net_wait_sample = {};
    return formatted_title;
  }
  if (s_previous_net_wait_sample.time_since_epoch().count() == 0) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  } else if (telemetry.total_wait_ns < s_previous_net_wait_ns) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
    s_net_wait_ms_per_second = 0.0;
  } else if (now - s_previous_net_wait_sample >=
             std::chrono::milliseconds(500)) {
    const double seconds =
        std::chrono::duration<double>(now - s_previous_net_wait_sample).count();
    s_net_wait_ms_per_second =
        static_cast<double>(telemetry.total_wait_ns - s_previous_net_wait_ns) /
        1000000.0 / seconds;
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  }
  return fmt::format("{} | Net wait {:.1f} ms/s | Buffer {}", formatted_title,
                     s_net_wait_ms_per_second, telemetry.buffer_size);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  // s_platform / s_window_title / s_show_fps_in_title are written under
  // s_runtime_mutex (Create, ~Runtime) while this runs on the title thread
  // and Dolphin's own title-change callers. Take the lock so the shutdown
  // path cannot clear the string mid-read. No s_runtime_mutex holder calls
  // back into this function, so the lock cannot self-deadlock.
  std::lock_guard lock(s_runtime_mutex);
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(
        title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {
  // Dolphin invokes this hook synchronously from
  // SConfig::SetRunningGameMetadata — including the call inside BootCore's
  // SetPathsAndGameMetadata, before Core::Init spawns the emu thread — so
  // repairing the executable-derived id here is race-free. It only fires
  // while the current id is exactly the "ID-<filename>" artifact, never
  // stomping a real disc or ES title id.
  //
  // The BootParameters::Executable branch fires this hook twice per boot:
  // once nested inside SetRunningGameMetadata (ConfigManager.cpp), before
  // that call installs the artifact's own GameSettings config layers, and
  // once again after it returns. Config::AddLayer keys layers by LayerType,
  // so repairing on the first call would let the artifact's inert layers
  // replace the real id's. Wait for the second — the repair's own
  // SetRunningGameMetadata then replaces them with GameSettings/<real id>
  // layers.
  std::string repair_id;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (!s_executable_game_id.empty() &&
        SConfig::GetInstance().GetGameID() == s_executable_game_id) {
      if (s_artifact_title_callback_seen)
        repair_id = s_running_game_id;
      else
        s_artifact_title_callback_seen = true;
    }
  }
  // SetRunningGameMetadata re-enters Host_TitleChanged (by then the id no
  // longer matches the artifact, so the nested call is a no-op); it must run
  // outside the lock because s_runtime_mutex is not recursive.
  if (!repair_id.empty()) {
    SConfig::GetInstance().SetRunningGameMetadata(repair_id);
    std::lock_guard lock(s_runtime_mutex);
    s_running_game_id.clear();
    s_executable_game_id.clear();
    s_artifact_title_callback_seen = false;
  }
}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  std::unique_ptr<ModManager> mods;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
};

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  // Reuse a caller-supplied InspectGame result when it names the same
  // canonical root — the runner inspects once up front for the release-pin
  // checks, and the tree hash is the launch-phase hot spot.
  GameInspectResult inspected{};
  if (config.preinspected_metadata)
  {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(config.game_root, ec);
    if (!ec && canonical == config.preinspected_metadata->root)
      inspected = GameInspectResult{config.preinspected_metadata, {}};
  }
  if (!inspected)
    inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(PathToString(config.module.path), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      message += " (load_status=" +
                 std::to_string(static_cast<int>(module_result.status)) + ")";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      "ModernGekko - " + impl->metadata.game_name + " [" +
      impl->metadata.disc_id + "]");
  impl->mods = std::make_unique<ModManager>();
  const ModLoadReport mod_report = impl->mods->LoadDirectories(
      impl->config.mod_directories, impl->metadata.disc_id);
  for (const ModLoadIssue &issue : mod_report.issues)
    std::fprintf(stderr, "mod rejected: %s: %s\n", issue.source.c_str(),
                 issue.message.c_str());
  for (const LoadedModInfo &mod : mod_report.loaded)
    std::fprintf(stderr, "mod loaded: %s %s\n", mod.id.c_str(),
                 mod.version.c_str());
  const char* accum_env = std::getenv("MODERNGEKKO_FRAME60_ACCUM");
  if (accum_env && accum_env[0] == '1') {
    const bool accum_loaded =
        std::any_of(mod_report.loaded.begin(), mod_report.loaded.end(),
                    [](const LoadedModInfo& mod) {
                      return mod.id == "frame60-accum";
                    });
    if (!accum_loaded)
      std::fprintf(stderr,
                   "warning: open render mode requested but frame60-accum is "
                   "not loaded for %s; guest keeps scene cadence\n",
                   impl->metadata.disc_id.c_str());
  }

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(PathToString(impl->config.user_directory));
    UICommon::Init();
    impl->ui_initialized = true;
    // Wind Waker performs a tolerated JAI audio NULL probe during normal
    // startup. With panic handlers enabled (Dolphin's default) the alert
    // lands on the default MsgHandler, which is a modal MessageBox — on
    // headless or unsupervised runs nothing can dismiss it and the emu
    // thread blocks forever mid-JAS-init. Force alerts off (matching the
    // release bundle's UsePanicHandlers=False); MODERNGEKKO_PANIC_ALERTS=1
    // restores the modal path for debugging.
    const char* panic_alerts = std::getenv("MODERNGEKKO_PANIC_ALERTS");
    if (!panic_alerts || panic_alerts[0] != '1') {
      Config::SetBase(Config::MAIN_USE_PANIC_HANDLERS, false);
      Common::SetEnableAlert(false);
    }
  }
  // Texture-pack readiness: replacement textures load from
  // Load/Textures/<GAMEID>/ (GZLE01 for this build) and per-game ini
  // overrides from GameSettings/<GAMEID>.ini. UICommon::Init's
  // CreateDirectories already makes both on the normal path; repeat so the
  // s_external_ui_common case (netplay owns UICommon there) gets them too —
  // CreateFullPath is idempotent.
  File::CreateFullPath(File::GetUserPath(D_HIRESTEXTURES_IDX));
  File::CreateFullPath(File::GetUserPath(D_GAMESETTINGS_IDX));

  Config::SetBase(Config::MAIN_FULLSCREEN,
                  impl->config.window_mode == WindowMode::Fullscreen);

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (impl->platform && impl->config.window_mode == WindowMode::Maximized)
    impl->platform->SetStartMaximized(true);
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  // MODERNGEKKO_HEADLESS_INPUT=1 keeps Pad/Wiimote/adapter init but hands
  // ControllerInterface a Headless WSI so window-bound backends stay off —
  // diagnostic lever for headed-mode host-cost bisection. (Skipping
  // InitControllers entirely stalls boot — do not gate that.)
  const char* headless_input = std::getenv("MODERNGEKKO_HEADLESS_INPUT");
  WindowSystemInfo input_wsi = wsi;
  if (headless_input && headless_input[0] == '1')
    input_wsi.type = WindowSystemType::Headless;
  UICommon::InitControllers(input_wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  // Guest clock budget (guest-clock-budget-spec.md §5): --clock flag or
  // MODERNGEKKO_CYCLES_PER_SECOND env. The env default is read here so the
  // knob works headless without the flag.
  u64 guest_clock = impl->config.guest_clock_hz;
  if (guest_clock == 0)
  {
    const char* env_clock = std::getenv("MODERNGEKKO_CYCLES_PER_SECOND");
    if (env_clock && env_clock[0])
    {
      // Whole-token parse: strtoull would map "-1" to ULLONG_MAX, saturate on
      // overflow, and silently take the numeric prefix of "486M".
      u64 parsed = 0;
      const char* const env_end = env_clock + std::strlen(env_clock);
      const auto parsed_result = std::from_chars(env_clock, env_end, parsed);
      if (parsed_result.ec == std::errc{} && parsed_result.ptr == env_end)
        guest_clock = parsed;
    }
  }
  if (guest_clock != 0)
    Config::SetBase(Config::MAIN_GUEST_CLOCK_HZ, guest_clock);
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.adapter)
    Config::SetBase(Config::GFX_ADAPTER, *impl->config.graphics.adapter);
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE,
                    *impl->config.graphics.internal_resolution_scale);
  if (impl->config.graphics.aspect_ratio) {
    if (*impl->config.graphics.aspect_ratio == moderngekko::AspectRatio::Wide) {
      Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH, 16);
      Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT, 9);
    }
    Config::SetBase(Config::GFX_ASPECT_RATIO,
                    ToDolphinAspectRatio(*impl->config.graphics.aspect_ratio));
  }
  if (impl->config.graphics.custom_aspect) {
    // AspectMode::Custom stretches the ~4:3 source by (W/H)/(4/3); the
    // widescreen16x9 mod gets the same pair via MODERNGEKKO_WIDESCREEN_ASPECT
    // (moderngekko_run) so its widened in-game layout matches.
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH,
                    impl->config.graphics.custom_aspect->first);
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT,
                    impl->config.graphics.custom_aspect->second);
  }
  if (impl->config.graphics.widescreen_hack)
    Config::SetBase(Config::GFX_WIDESCREEN_HACK,
                    *impl->config.graphics.widescreen_hack);
  if (impl->config.graphics.anisotropy)
    Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                    ToDolphinAnisotropy(*impl->config.graphics.anisotropy));
  // Anti-aliasing (runtime.hpp contract): Off pins MSAA=1/SSAA=off and clears
  // any post-processing shader so a stale enhancement cannot leak through;
  // Fxaa only sets the post shader (leaves MSAA/SSAA alone so a user GFX.ini
  // MSAA still composes); Msaa* set MSAA=N with SSAA off.
  if (impl->config.graphics.aa) {
    switch (*impl->config.graphics.aa) {
      case AntiAliasing::Off:
        Config::SetBase(Config::GFX_MSAA, 1u);
        Config::SetBase(Config::GFX_SSAA, false);
        Config::SetBase(Config::GFX_ENHANCE_POST_SHADER, std::string{});
        break;
      case AntiAliasing::Fxaa:
        Config::SetBase(Config::GFX_ENHANCE_POST_SHADER, std::string("FXAA"));
        break;
      case AntiAliasing::Msaa2x:
      case AntiAliasing::Msaa4x:
      case AntiAliasing::Msaa8x: {
        const u32 samples =
            *impl->config.graphics.aa == AntiAliasing::Msaa2x   ? 2u
            : *impl->config.graphics.aa == AntiAliasing::Msaa4x ? 4u
                                                              : 8u;
        Config::SetBase(Config::GFX_MSAA, samples);
        Config::SetBase(Config::GFX_SSAA, false);
        break;
      }
    }
  }
  // Uncapped throttle knob (RunMain tri-state): nullopt leaves the Dolphin
  // default/user-dir EmulationSpeed untouched (default-capped bit-identical);
  // 0.0f = unlimited (open throttle); 1.0f = explicit --capped pin.
  if (impl->config.emulation_speed)
    Config::SetBase(Config::MAIN_EMULATION_SPEED, *impl->config.emulation_speed);
  if (impl->config.graphics.immediate_xfb)
    Config::SetBase(Config::GFX_HACK_IMMEDIATE_XFB,
                    *impl->config.graphics.immediate_xfb);
  if (!impl->config.headless) {
    auto resolve_present = [&]() -> std::optional<Config::PresentMode> {
      if (impl->config.graphics.present_mode)
        return detail::ToDolphinPresentMode(*impl->config.graphics.present_mode);
      return std::nullopt;
    }();
    if (resolve_present) {
      Config::SetBase(Config::GFX_PRESENT_MODE, *resolve_present);
      bool vsync = (*resolve_present != Config::PresentMode::Immediate);
      Config::SetBase(Config::GFX_VSYNC, vsync);
    }
  }
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  // Upstream's default is a single async compile worker, which leaves the
  // uber-fallback window open for tens of seconds on new content (measured
  // async_pending ~1008 in the intro). -1 is the upstream "auto" sentinel
  // (GetNumAutoShaderCompilerThreads). An explicit >1 in GFX.ini still wins.
  if (Config::Get(Config::GFX_SHADER_COMPILER_THREADS) <= 1)
    Config::SetBase(Config::GFX_SHADER_COMPILER_THREADS, -1);
  // Wait-for-shaders has a progress UI: the async-compiler progress lambda
  // calls ImGui::GetIO() (ShaderCache.cpp WaitForAsyncCompiler), but headless
  // skips OnScreenUI/ImGui init (Present.cpp guards on IsHeadless). With a
  // cold NVIDIA shader cache the compile has pending work -> the lambda runs
  // -> `GImGui != __null` crash 2s after module load (soak 12:49 attempt on
  // VK_ICD_FILENAMES=nvidia_icd.json; AMD/radv never hit it because the mesa
  // cache compiles instantly). Only force the wait when there is a UI to
  // report progress to; headless keeps the default (false) and compiles
  // asynchronously without blocking.
  if (!impl->config.headless)
    Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (impl->config.headless) {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  } else if (impl->config.audio.backend.empty() ||
             !std::ranges::contains(audio_backends,
                                    impl->config.audio.backend)) {
    constexpr std::array preferred_backends = {
        BACKEND_CUBEB, BACKEND_PULSEAUDIO, BACKEND_ALSA};
    const auto preferred =
        std::ranges::find_if(preferred_backends, [&](const char *backend) {
          return std::ranges::contains(audio_backends, backend);
        });
    impl->config.audio.backend =
        preferred != preferred_backends.end() ? *preferred : BACKEND_NULLSOUND;
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  // nullopt = untouched: a BackgroundInput already set in the user dir's
  // Dolphin.ini keeps working when the frontend config says nothing.
  if (impl->config.input.background_input)
    Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                    *impl->config.input.background_input);

  auto &jit = Core::System::GetInstance().GetJitInterface();
  StaticRecompModuleSource recomp_source;
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    // UTF-8 per Dolphin's path-string convention; the vendored loader checks
    // it with File::Exists (UTF-8 aware). On Windows its DynamicLibrary::Open
    // still uses LoadLibraryA, so a path outside the ANSI code page degrades
    // to a graceful interpreter fallback rather than loading a wrong file.
    recomp_source =
        StaticRecompModuleSource::Dynamic(
            PathToString(impl->config.module.path));
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    recomp_source = StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor));
  if (!impl->mods->Empty()) {
    recomp_source.host_call = &ModManager::HostCall;
    recomp_source.host_call_contains = &ModManager::HostCallContains;
    recomp_source.host_call_range_contains =
        &ModManager::HostCallRangeContains;
    recomp_source.verdict_ops = &ModManager::VerdictOps;
    recomp_source.verdict_pcs = &ModManager::VerdictPcs;
    recomp_source.host_call_user = impl->mods.get();
  }
  // SMC-mismatch forensics: give the guard the retail main.dol so a failed
  // chunk verification can diff guest RAM against the original bytes. The
  // vendored consumer opens it via std::ifstream(const char*), which wants
  // the native narrow encoding (ANSI on Windows) — path.string() supplies
  // exactly that, unlike the UTF-8 used for Dolphin APIs above.
  recomp_source.retail_dol_path =
      (impl->config.game_root / "sys" / "main.dol").string();
  jit.SetStaticRecompModuleSource(std::move(recomp_source));

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  s_running_game_id = impl->metadata.disc_id;
  // Mirror the Executable branch's derivation (ConfigManager.cpp): the id
  // BootCore will invent for sys/main.dol is "ID-main". Host_TitleChanged
  // replaces exactly this artifact with the real disc id, inside BootCore —
  // before the emu thread's first GameSettings/Textures lookup.
  std::string dol_utf8 = PathToString(impl->metadata.main_dol);
  std::ranges::replace(dol_utf8, '\\', '/');
  s_executable_game_id = SConfig::MakeGameID(PathToFileName(dol_utf8));
  s_artifact_title_callback_seen = false;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_running_game_id.clear();
  s_executable_game_id.clear();
  s_artifact_title_callback_seen = false;
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          PathToString(m_impl->metadata.main_dol), std::move(*s_boot_session_data));
    else if (m_impl->config.load_state_path)
      // DeleteSavestateAfterBoot::No: the state is the player's, not a
      // scratch file this run owns.
      boot = BootParameters::GenerateFromFile(
          PathToString(m_impl->metadata.main_dol),
          BootSessionData(PathToString(*m_impl->config.load_state_path),
                          DeleteSavestateAfterBoot::No));
    else
      boot =
          BootParameters::GenerateFromFile(PathToString(m_impl->metadata.main_dol));
    s_boot_session_data.reset();
  }
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  const std::string &dump_path = m_impl->config.audio.dump_audio_path;
  if (!dump_path.empty()) {
    // Delete any pre-existing dump so WaveFileWriter never shows an
    // interactive overwrite prompt (headless-hostile).
    std::error_code ignore_ec;
    // dump_audio_path carries UTF-8 like the other Dolphin-facing strings.
    std::filesystem::remove(StringToPath(dump_path), ignore_ec);
    // Core::Init spawns the emu thread and returns immediately; the sound
    // stream is created inside the emu thread a moment later. Poll briefly
    // instead of racing the boot.
    SoundStream *sound_stream = nullptr;
    for (int attempt = 0; attempt < 200; ++attempt) {
      sound_stream = Core::System::GetInstance().GetSoundStream();
      if (sound_stream)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (auto *mixer = sound_stream ? sound_stream->GetMixer() : nullptr) {
      mixer->StartLogDSPAudio(dump_path);
      std::fprintf(stderr, "audio dump: writing %s\n", dump_path.c_str());
    } else {
      std::fprintf(stderr,
                   "audio dump: sound stream unavailable, dump disabled\n");
    }
  }
  // MODERNGEKKO_RUN_SECONDS=<float>: bench helper — request a clean stop
  // after N seconds so runs bounded inside this process still produce
  // shutdown stats and the MODERNGEKKO_FTIME_LOG summary (an MSYS `timeout`
  // kill cannot deliver SIGTERM to a native exe, so benches otherwise lose
  // all shutdown-time diagnostics). 0/unset = no limit.
  std::jthread auto_stop_thread;
  {
    const char* run_seconds_env = std::getenv("MODERNGEKKO_RUN_SECONDS");
    // Clamp to a day so an absurd env value can't overflow the
    // duration_cast below; NaN/negative/garbage parses collapse to 0.
    const double run_seconds =
        (run_seconds_env && *run_seconds_env)
            ? std::min(86400.0,
                       std::max(0.0, std::strtod(run_seconds_env, nullptr)))
            : 0.0;
    if (run_seconds > 0.0) {
      auto_stop_thread = std::jthread([this, run_seconds](std::stop_token st) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::duration<double>(run_seconds));
        while (!st.stop_requested() &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!st.stop_requested()) {
          std::fprintf(stderr, "[run-seconds] %.1fs elapsed, requesting stop\n",
                       run_seconds);
          RequestStop();
        }
      });
    }
  }
  std::atomic<bool> title_thread_done{true};
  std::jthread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread_done.store(false);
    title_thread = std::jthread([&title_thread_done](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        Host_UpdateTitle({});
        for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      title_thread_done.store(true);
    });
  }
  m_impl->platform->MainLoop();
  // --save-state-on-exit: mirror of the --load-state restore point. The
  // state must be written while machine state is intact, i.e. before the
  // Core::Stop/Core::Shutdown below tear the emu thread down. SIGTERM /
  // timeout reach this point via RequestStop -> platform->RequestShutdown.
  bool exit_state_dump_ok = true;
  if (m_impl->config.save_state_on_exit_path &&
      Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized) {
    const std::string exit_state_path =
        PathToString(*m_impl->config.save_state_on_exit_path);
    State::SaveAs(Core::System::GetInstance(), exit_state_path);
    {
      // SaveAs only schedules the compress+write worker; hold the exclusive
      // save lock so the atomic tmp+rename has landed when this block exits.
      // The queued save holds a shared task lock and takes another shared
      // lock when handing data to the compression worker. A blocking writer
      // can prevent that handoff on writer-preferring shared mutexes.
      std::unique_lock lk(State::s_state_saves_in_progress, std::defer_lock);
      while (!lk.try_lock()) {
        PumpShutdownMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    std::error_code size_ec;
    const std::uintmax_t written =
        std::filesystem::file_size(exit_state_path, size_ec);
    if (!size_ec && written > 0) {
      std::fprintf(stderr, "[savestate-on-exit] wrote %s (%ju bytes)\n",
                   exit_state_path.c_str(), written);
    } else {
      std::fprintf(stderr, "[savestate-on-exit] FAILED to write %s\n",
                   exit_state_path.c_str());
      exit_state_dump_ok = false;
    }
  }
  title_thread.request_stop();
  if (title_thread.joinable()) {
    while (!title_thread_done.load()) {
      PumpShutdownMessages();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    title_thread.join();
  }
  m_impl->platform->SaveWindowGeometry();
  if (!m_impl->config.audio.dump_audio_path.empty()) {
    if (auto *sound_stream = Core::System::GetInstance().GetSoundStream()) {
      if (auto *mixer = sound_stream->GetMixer())
        mixer->StopLogDSPAudio();
    }
  }
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  if (!exit_state_dump_ok)
    return {RuntimeExitReason::Stopped,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "exit savestate dump failed: " +
                             PathToString(*m_impl->config.save_state_on_exit_path)}};
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko
