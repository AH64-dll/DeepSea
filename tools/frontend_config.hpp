#pragma once

#include "moderngekko/utf8_path.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend {
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
  // Arbitrary W:H target (21:9, 32:9, "2.37"...): drives Dolphin's
  // AspectMode::Custom stretch and, through MODERNGEKKO_WIDESCREEN_ASPECT,
  // the widescreen16x9 mod's generalized layout. The pair lives in
  // AspectRatioValue::width/height.
  Custom,
};

// [Video] aspect_ratio value: the mode plus, for AspectRatio::Custom, the
// W:H pair. Parsed forms: auto | wide/16:9 | standard/4:3 | stretch for the
// fixed modes; "W:H" ("21:9"), "custom W:H", or a bare decimal ("2.37")
// produce Custom. Custom ratios are restricted to the widescreen mod's
// supported window [4:3, 32:9] (compared as exact rationals).
struct AspectRatioValue
{
  AspectRatio mode = AspectRatio::Auto;
  int width = 0;   // Custom only: CustomAspectRatioWidth
  int height = 0;  // Custom only: CustomAspectRatioHeight

  AspectRatioValue() = default;
  /*implicit*/ AspectRatioValue(AspectRatio m) : mode(m) {}
  AspectRatioValue(AspectRatio m, int w, int h) : mode(m), width(w), height(h) {}
  friend bool operator==(const AspectRatioValue&,
                         const AspectRatioValue&) = default;
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

// [Video] window_mode: how the game window opens. Fullscreen is the
// borderless path MAIN_FULLSCREEN already drives (PlatformWin32
// ToggleFullscreen); Maximized is a normal maximized window; Windowed is
// the legacy behavior.
enum class WindowMode
{
  Windowed,
  Maximized,
  Fullscreen,
};

// [Video] aa: anti-aliasing applied through Dolphin's GFX settings.
// Off is an explicit "pin MSAA 1x / SSAA off" (distinct from an unset key,
// which leaves GFX.ini untouched); Fxaa only sets PostProcessingShader;
// Msaa2x/4x/8x set MSAA=N with SSAA off.
enum class AntiAliasing
{
  Off,
  Fxaa,
  Msaa2x,
  Msaa4x,
  Msaa8x,
};

struct WindowModeOption
{
  const char* text;
  WindowMode value;
};

struct AntiAliasingOption
{
  const char* text;
  AntiAliasing value;
};

struct PresentModeOption
{
  const char* text;
  PresentMode value;
};

struct AspectRatioOption
{
  const char* text;
  AspectRatio value;
  // Custom options carry the W:H pair stored into AspectRatioValue; 0:0 for
  // the fixed modes.
  int width = 0;
  int height = 0;
};

struct AnisotropyOption
{
  const char* text;
  Anisotropy value;
};

struct ResolutionOption {
  const char *text;
  int dolphin_scale;
};

struct GraphicsBackendOption {
  const char *text;
  const char *value;
};

struct ConfigResult {
  int dolphin_scale = 0;
  std::string resolution;
  std::string graphics_backend = "Vulkan";
  std::optional<int> graphics_adapter;
  std::optional<AspectRatioValue> aspect_ratio;
  std::optional<bool> widescreen_hack;
  std::optional<Anisotropy> anisotropy;
  std::string controller;
  std::vector<std::string> controllers;
  // Tri-state like the other optional settings: unset leaves Dolphin's own
  // Input/BackgroundInput (Dolphin.ini) untouched; an explicit true/false in
  // config.ini overrides it.
  std::optional<bool> background_input;
  bool show_fps_in_title = true;
  // Borderless fullscreen is the shipped default. `fullscreen` remains the
  // legacy serialized key; `window_mode` ([Video] window_mode=windowed|
  // maximized|fullscreen) overrides it when present.
  bool fullscreen = true;
  std::optional<WindowMode> window_mode;
  // [Video] aa=off|fxaa|2x|4x|8x. Unset leaves GFX.ini MSAA/SSAA and the
  // post-processing shader untouched; Off actively pins MSAA=1/SSAA=off so
  // a stale enhancement cannot leak through.
  std::optional<AntiAliasing> aa;
  std::optional<PresentMode> present_mode;
  // Open-frame-rate request (FrameUncap): `--uncapped` / `--open-frame-rate`,
  // env MODERNGEKKO_UNCAPPED, or config `frame_rate=uncapped`. Unset (the
  // default) means capped: current behavior, bit-identical. Set only fills
  // the default — an explicit present mode (CLI or config) always wins.
  std::optional<bool> uncapped;
  std::string netplay_nickname = "Player";
  std::string netplay_address = "127.0.0.1";
  std::uint16_t netplay_port = 2626;
  std::string netplay_buffer = "auto";
  std::string error;

  // The mode the window actually opens in: an explicit window_mode wins;
  // otherwise the legacy fullscreen bool maps to Fullscreen/Windowed.
  WindowMode EffectiveWindowMode() const {
    return window_mode.value_or(fullscreen ? WindowMode::Fullscreen
                                           : WindowMode::Windowed);
  }

  explicit operator bool() const { return error.empty(); }
};

const std::vector<ResolutionOption> &SupportedResolutions();
const std::vector<GraphicsBackendOption> &SupportedGraphicsBackends();
const std::vector<PresentModeOption> &SupportedPresentModes();
const std::vector<AspectRatioOption> &SupportedAspectRatios();
const std::vector<AnisotropyOption> &SupportedAnisotropyOptions();
std::optional<PresentMode> ParsePresentMode(std::string_view raw);
std::string PresentModeToString(PresentMode mode);
// Parses `aspect_ratio` values: auto | wide/16:9 | standard/4:3 | stretch,
// or a custom W:H ratio — "21:9", "32:9", "custom W:H", or a bare decimal
// ("2.37"). Custom ratios must land inside the widescreen mod's window
// [4:3, 32:9] (the mod itself clamps, but config rejects out-of-window
// values so the written file can't drift from what gets applied).
std::optional<AspectRatioValue> ParseAspectRatio(std::string_view raw);
std::string AspectRatioToString(AspectRatioValue ratio);
std::optional<Anisotropy> ParseAnisotropy(std::string_view raw);
std::string AnisotropyToString(Anisotropy mode);
const std::vector<WindowModeOption> &SupportedWindowModes();
const std::vector<AntiAliasingOption> &SupportedAntiAliasingOptions();
std::optional<WindowMode> ParseWindowMode(std::string_view raw);
std::string WindowModeToString(WindowMode mode);
// Parses `aa` values: off/none/disabled -> Off, fxaa -> Fxaa,
// 2x/4x/8x (with or without an msaa prefix) -> the MSAA modes, else nullopt.
std::optional<AntiAliasing> ParseAntiAliasing(std::string_view raw);
std::string AntiAliasingToString(AntiAliasing mode);
// Parses `frame_rate` values: uncapped/open/unlimited/true... -> true,
// capped/closed/limited/vsync/false... -> false, else nullopt (invalid).
std::optional<bool> ParseFrameRate(std::string_view raw);
// Reads an environment variable as a filesystem path. POSIX getenv bytes are
// already the native path encoding, but on Windows the CRT's getenv returns
// the ANSI code page, which cannot represent every profile name -- the shared
// GetEnvPath goes through _wgetenv and the wide path constructor there.
inline std::optional<std::filesystem::path> EnvironmentPath(const char *name) {
  return moderngekko::GetEnvPath(name);
}
ConfigResult LoadConfig(const std::filesystem::path &user_directory,
                        bool create_if_missing);
bool SaveConfig(const std::filesystem::path &user_directory,
                const ConfigResult &config, std::string *error);
bool SaveConfig(const std::filesystem::path &user_directory,
                std::string_view resolution, bool show_fps_in_title,
                std::string_view controller, std::string *error);
std::string
ReadConfiguredController(const std::filesystem::path &user_directory);
std::vector<std::string>
ReadConfiguredControllers(const std::filesystem::path &user_directory);
bool ControllerConfigExists(const std::filesystem::path &user_directory);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::string_view controller,
                              std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::string_view controller, std::string *message);

// One [GCPadN]/[WiimoteN] section's editable state: the bound device string
// plus the raw "Group/Control = expression" lines beneath it (e.g.
// "Buttons/A" -> "`Button A`"). Order is preserved so a hand-edited profile
// round-trips through the launcher's remap screen untouched except for the
// controls the user actually changed.
struct PadPortSettings {
  std::string device;
  std::vector<std::pair<std::string, std::string>> controls;
};

// All four pad sections of the controller profile (GCPadNew.ini /
// WiimoteNew.ini), in port order. Unbound or missing sections come back
// with an empty device and no controls.
std::array<PadPortSettings, 4>
ReadControllerProfile(const std::filesystem::path &user_directory);

// The generated default bindings for a device as "Group/Control" ->
// expression pairs: the keyboard layout for "DInput/0/Keyboard Mouse" and
// the XInput-style gamepad layout for everything else. This is the same
// body GenerateControllerConfig/EnsureControllerConfig write; exposed so
// the launcher's remap screen can offer it as "Reset to defaults".
std::vector<std::pair<std::string, std::string>>
DefaultPadControls(std::string_view device);

// Writes the four pad sections: header, `Device = <device>` (only when the
// port is bound) and each port's control lines verbatim. Atomic via a
// sibling temp file + copy, like the ensure-merge. Rejects anything that
// would corrupt the ini (newlines, '=' in keys, >4 ports).
bool SaveControllerProfile(const std::filesystem::path &user_directory,
                           std::span<const PadPortSettings> ports,
                           std::string *message);
} // namespace moderngekko::frontend
