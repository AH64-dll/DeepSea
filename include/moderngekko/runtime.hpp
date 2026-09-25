#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace moderngekko
{
struct ModuleSource
{
  enum class Kind
  {
    None,
    DynamicPath,
    AttachedDescriptor,
  };

  static ModuleSource DynamicPath(std::filesystem::path path);
  static ModuleSource AttachedDescriptor(const ModernGekkoModuleDesc* descriptor);

  Kind kind = Kind::None;
  std::filesystem::path path;
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

enum class PresentMode
{
  Fifo,
  Mailbox,
  Immediate,
};

enum class AspectRatio
{
  Auto,
  Wide,
  Standard,
  Stretch,
  // Arbitrary target aspect (GraphicsSettings::custom_aspect carries the
  // W:H pair): maps to Dolphin's AspectMode::Custom, which stretches the
  // ~4:3 source by custom/(4/3) - the same shape as ForceWide at 16:9.
  Custom,
};

enum class Anisotropy
{
  Default,
  X1,
  X2,
  X4,
  X8,
  X16,
};

// How the render window opens. Fullscreen is the borderless path driven by
// MAIN_FULLSCREEN (PlatformWin32 ToggleFullscreen); Maximized is a normal
// maximized window (Windows only — other platforms fall back to Windowed);
// Windowed is the plain Dolphin.ini-sized window.
enum class WindowMode
{
  Windowed,
  Maximized,
  Fullscreen,
};

// Anti-aliasing applied via Dolphin's GFX settings (config.ini [Video] aa=
// or env MODERNGEKKO_AA). Off pins MSAA=1/SSAA=off explicitly; Fxaa only
// sets the post-processing shader; Msaa* set MSAA=N with SSAA off.
enum class AntiAliasing
{
  Off,
  Fxaa,
  Msaa2x,
  Msaa4x,
  Msaa8x,
};

struct GraphicsSettings
{
  std::string backend;
  std::optional<int> internal_resolution_scale;
  // Vulkan physical-device index (g_Config.iAdapter). 0 = first device the
  // loader enumerates (on this hybrid host: the AMD iGPU via RADV, which
  // hard-recovered under the demo-movie THP load); 1 = the discrete GPU.
  // See phase2-vk-device-lost-dig.md. Env override still works:
  // VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
  std::optional<int> adapter;
  std::optional<AspectRatio> aspect_ratio;
  // The W:H pair behind aspect_ratio == AspectRatio::Custom: forwarded to
  // Dolphin's GFX CustomAspectRatioWidth/Height. moderngekko_run also feeds
  // it to the widescreen16x9 mod via MODERNGEKKO_WIDESCREEN_ASPECT so the
  // game's widened layout matches the presentation stretch.
  std::optional<std::pair<int, int>> custom_aspect;
  std::optional<bool> widescreen_hack;
  std::optional<Anisotropy> anisotropy;
  std::optional<AntiAliasing> aa;
  std::optional<PresentMode> present_mode;
  std::optional<bool> immediate_xfb;
};

struct AudioSettings
{
  std::string backend;
  // Stage 0: capture the post-DSP mix to a WAV file at this path (empty =
  // disabled). The capture point is Dolphin's DSP mixer (Mixer::StartLogDSPAudio),
  // the same stream the AU-1 oracle dump records, so both sides share one path.
  std::string dump_audio_path;
};

struct InputSettings
{
  // Tri-state: nullopt leaves the user dir's Dolphin.ini BackgroundInput value
  // alone; an explicit bool overrides it (config.ini background_input= or
  // --background-input).
  std::optional<bool> background_input;
};

enum class WindowSystem
{
  Default,
  Wayland,
  X11,
};

struct RuntimeConfig
{
  std::filesystem::path game_root;
  std::filesystem::path user_directory;
  ModuleSource module;
  std::vector<std::filesystem::path> mod_directories;
  GraphicsSettings graphics;
  AudioSettings audio;
  InputSettings input;
  // Guest clock budget (guest-clock-budget-spec.md §5): declared CPU clock
  // in Hz; 0 = nominal (486M GC). Set to the delivered cycle rate (254M) for
  // 30 fps @ 1.0x guest time in the wait-bound regime. Env:
  // MODERNGEKKO_CYCLES_PER_SECOND.
  std::uint64_t guest_clock_hz = 0;
  // Wall-clock throttle override (MAIN_EMULATION_SPEED): nullopt = do not
  // touch (Dolphin default 1.0x / user-dir value stays bit-identical);
  // 0.0f = unlimited (open throttle, Throttle() returns without sleeping);
  // 1.0f = explicit 1.0x pin over any stale user-dir setting.
  std::optional<float> emulation_speed;
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  // Windowed is the API-safe default; the runner always resolves the
  // frontend config (window_mode= or legacy fullscreen=) into this field.
  WindowMode window_mode = WindowMode::Windowed;
  bool allow_interpreter = false;
  bool show_fps_in_title = true;
  std::optional<std::string> window_title;
  // Boot straight into a savestate instead of from the title screen.
  std::optional<std::filesystem::path> load_state_path;
  // Dump a Dolphin-format savestate to this path on graceful shutdown
  // (--save-state-on-exit), before teardown destroys machine state.
  std::optional<std::filesystem::path> save_state_on_exit_path;
  // Optional InspectGame result the caller already computed for game_root.
  // Create() reuses it when its canonical root matches game_root's, which
  // skips a second full-tree SHA-256 pass over the extracted assets (~1-2 GB
  // — the dominant startup cost). Empty = Create inspects on its own.
  std::optional<GameMetadata> preinspected_metadata;
};

enum class RuntimeErrorCode
{
  AlreadyActive,
  InvalidGame,
  ModuleRequired,
  ModuleRejected,
  PlatformUnavailable,
  InitializationFailed,
  BootFailed,
  InvalidState,
};

struct RuntimeError
{
  RuntimeErrorCode code = RuntimeErrorCode::InitializationFailed;
  std::string message;
};

enum class RuntimeExitReason
{
  Stopped,
  BootFailed,
};

struct RuntimeRunResult
{
  RuntimeExitReason reason = RuntimeExitReason::Stopped;
  std::optional<RuntimeError> error;
};

class Runtime;

struct RuntimeCreateResult
{
  std::unique_ptr<Runtime> runtime;
  std::optional<RuntimeError> error;

  explicit operator bool() const { return runtime != nullptr; }
};

class Runtime final
{
public:
  static RuntimeCreateResult Create(RuntimeConfig config);

  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  RuntimeRunResult Run();
  void RequestStop();
  std::optional<RuntimeError> Pause();
  std::optional<RuntimeError> Resume();

  const RuntimeConfig& GetConfig() const;
  const GameMetadata& GetGameMetadata() const;
  const std::string& GetWindowTitle() const;

private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko

namespace ModernGekko = moderngekko;
