#include "frontend_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace moderngekko::frontend {
namespace {
std::string Trim(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

// Paths surface in error text as UTF-8 (the launcher shows them in an ImGui
// dialog); fs::path::string() would emit the ANSI code page on Windows.
std::string PathUtf8(const fs::path &path) {
  const std::u8string encoded = path.u8string();
  return {encoded.begin(), encoded.end()};
}

bool ValidNetplayAddress(std::string_view value) {
  if (value.empty() || value.size() > 253)
    return false;
  return std::ranges::all_of(value, [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_';
  });
}

std::string NormalizeGraphicsBackend(std::string value) {
  const std::string lower = Lower(Trim(std::move(value)));
  if (lower == "vulkan")
    return "Vulkan";
  if (lower == "opengl" || lower == "ogl")
    return "OGL";
  return {};
}

bool ParseBoolean(const std::string &value, bool *result) {
  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    *result = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    *result = false;
    return true;
  }
  return false;
}

fs::path ControllerConfigPath(const fs::path &user_directory) {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  return user_directory / "Config" / "GCPadNew.ini";
#else
  return user_directory / "Config" / "WiimoteNew.ini";
#endif
}

std::string_view ControllerSectionPrefix() {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  return "[GCPad";
#else
  return "[Wiimote";
#endif
}

// Dolphin's IniFile compares section names and keys case-insensitively
// (Common::CaseInsensitiveEquals / CaseInsensitiveLess); match that here so
// our view of a hand-edited profile agrees with what the emulator will read.
bool AsciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y));
  });
}

// IniFile::Load skips a UTF-8 BOM at the very start of the file before
// parsing. Every pass that re-examines raw lines must hand the section/key
// checks the same stripped view of the first line, or a BOM'd "[GCPad1]"
// parses as a header in one pass and as plain text in the next.
std::string_view StripUtf8Bom(std::string_view line, bool first_line) {
  if (first_line && line.starts_with("\xEF\xBB\xBF"))
    line.remove_prefix(3);
  return line;
}

// IniFile::Load treats a line as a section header when its FIRST byte is '['
// (leading whitespace disqualifies it) and a ']' appears anywhere after; the
// name runs to that first ']' and trailing text is ignored. The UTF-8 BOM is
// skipped on the first line before this check there, as it is here.
bool IsSectionHeader(std::string_view line) {
  return line.starts_with('[') && line.find(']') != std::string_view::npos;
}

// IniFile::ParseLine strips surrounding quotes off values (StripQuotes).
std::string StripQuoted(std::string value) {
  if (!value.empty() && value.front() == '"' && value.back() == '"')
    return value.substr(1, value.size() - 2);
  return value;
}

// Maps a "[GCPad2]"/"[Wiimote3]" style header line to a 0-based pad index; 4
// means "not a pad section". `header` must satisfy IsSectionHeader. Shared by
// the profile reader and the ensure-merge.
std::size_t PadSectionIndex(std::string_view header) {
  const std::string_view prefix = ControllerSectionPrefix();
  const std::string_view name = header.substr(1, header.find(']') - 1);
  const std::string_view base = prefix.substr(1); // prefix includes '['
  if (name.size() == base.size() + 1 && name.back() >= '1' &&
      name.back() <= '4' &&
      AsciiEqualsIgnoreCase(name.substr(0, base.size()), base))
    return static_cast<std::size_t>(name.back() - '1');
  return 4;
}

// The Device key under a pad section, matched like IniFile does.
bool IsDeviceKey(std::string_view key) {
  return AsciiEqualsIgnoreCase(Trim(std::string(key)), "Device");
}

bool ValidControllerList(std::span<const std::string> controllers,
                         std::string *message) {
  if (controllers.empty() || controllers.size() > 4) {
    if (message)
      *message = "select between one and four connected SDL gamepads";
    return false;
  }
  for (const std::string &controller : controllers) {
    if (controller.empty() ||
        controller.find_first_of("\r\n") != std::string_view::npos) {
      if (message)
        *message = "select connected SDL gamepads";
      return false;
    }
  }
  return true;
}

// One side of a "W:H" aspect ratio: plain ASCII digits, fully consumed,
// value in [1, 9999] (2560:1080 fits; keeps the range-check products tiny).
bool ParseAspectInt(std::string_view s, std::int64_t *out) {
  if (s.empty() || !std::ranges::all_of(s, [](char c) {
        return c >= '0' && c <= '9';
      }))
    return false;
  std::int64_t v = 0;
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), v);
  if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() ||
      v < 1 || v > 9999)
    return false;
  *out = v;
  return true;
}

// A bare decimal aspect like "2.37": digits with at most one '.' (trailing
// digits required after it), up to 6 fraction digits. Returns the exact
// integer ratio (2.37 -> 237:100) so the [4:3, 32:9] window check and the
// stored CustomAspectRatio pair carry no binary rounding.
bool ParseAspectDecimal(std::string_view s, std::int64_t *w,
                        std::int64_t *h) {
  const std::size_t dot = s.find('.');
  if (dot != std::string_view::npos && s.find('.', dot + 1) != std::string_view::npos)
    return false;  // at most one decimal point
  const std::string_view ip = s.substr(0, dot);
  const std::string_view fp =
      dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1);
  if (dot != std::string_view::npos && (fp.empty() || fp.size() > 6))
    return false;
  if (ip.empty() || ip.size() > 7 || !std::ranges::all_of(ip, [](char c) {
        return c >= '0' && c <= '9';
      }))
    return false;
  if (!fp.empty() && !std::ranges::all_of(fp, [](char c) {
        return c >= '0' && c <= '9';
      }))
    return false;
  std::int64_t iv = 0, fv = 0;
  const auto parsed = std::from_chars(ip.data(), ip.data() + ip.size(), iv);
  if (parsed.ec != std::errc{} || parsed.ptr != ip.data() + ip.size())
    return false;
  if (!fp.empty()) {
    const auto frac = std::from_chars(fp.data(), fp.data() + fp.size(), fv);
    if (frac.ec != std::errc{} || frac.ptr != fp.data() + fp.size())
      return false;
  }
  std::int64_t den = 1;
  for (std::size_t i = 0; i < fp.size(); ++i)
    den *= 10;
  *w = iv * den + fv;
  *h = den;
  return *w > 0;
}

} // namespace

const std::vector<PresentModeOption> &SupportedPresentModes() {
  static const std::vector<PresentModeOption> modes = {
      {"fifo", PresentMode::Fifo},
      {"mailbox", PresentMode::Mailbox},
      {"immediate", PresentMode::Immediate},
  };
  return modes;
}

const std::vector<AspectRatioOption> &SupportedAspectRatios() {
  // Custom entries carry their W:H pair; arbitrary ratios can still be set
  // via aspect_ratio= in config.ini ("custom 48:10", "2.39"...) - the combo
  // then shows the configured value verbatim.
  static const std::vector<AspectRatioOption> ratios = {
      {"Auto (game native)", AspectRatio::Auto, 0, 0},
      {"16:9", AspectRatio::Wide, 0, 0},
      {"4:3", AspectRatio::Standard, 0, 0},
      {"21:9 (ultrawide)", AspectRatio::Custom, 21, 9},
      {"32:9 (super ultrawide)", AspectRatio::Custom, 32, 9},
      {"Stretch to window", AspectRatio::Stretch, 0, 0},
  };
  return ratios;
}

const std::vector<AnisotropyOption> &SupportedAnisotropyOptions() {
  static const std::vector<AnisotropyOption> options = {
      {"Game default", Anisotropy::Default}, {"1x", Anisotropy::X1},
      {"2x", Anisotropy::X2}, {"4x", Anisotropy::X4},
      {"8x", Anisotropy::X8}, {"16x", Anisotropy::X16},
  };
  return options;
}

const std::vector<WindowModeOption> &SupportedWindowModes() {
  static const std::vector<WindowModeOption> modes = {
      {"Windowed", WindowMode::Windowed},
      {"Maximized", WindowMode::Maximized},
      {"Fullscreen (borderless)", WindowMode::Fullscreen},
  };
  return modes;
}

const std::vector<AntiAliasingOption> &SupportedAntiAliasingOptions() {
  // The MSAA entries carry the lens-flare caveat because Wind Waker's glow
  // reads back EFB contents that multi-sample resolve changes; FXAA is a
  // pure post-process and leaves them alone.
  static const std::vector<AntiAliasingOption> options = {
      {"Off", AntiAliasing::Off},
      {"FXAA", AntiAliasing::Fxaa},
      {"MSAA 2x (may affect lens-flare effects)", AntiAliasing::Msaa2x},
      {"MSAA 4x (may affect lens-flare effects)", AntiAliasing::Msaa4x},
      {"MSAA 8x (may affect lens-flare effects)", AntiAliasing::Msaa8x},
  };
  return options;
}

std::optional<PresentMode> ParsePresentMode(std::string_view raw) {
  std::string s(raw);
  s = Lower(Trim(std::move(s)));
  if (s == "fifo")
    return PresentMode::Fifo;
  if (s == "mailbox")
    return PresentMode::Mailbox;
  if (s == "immediate")
    return PresentMode::Immediate;
  return std::nullopt;
}

std::string PresentModeToString(PresentMode mode) {
  switch (mode) {
    case PresentMode::Fifo:
      return "fifo";
    case PresentMode::Mailbox:
      return "mailbox";
    case PresentMode::Immediate:
      return "immediate";
  }
  return {};
}

std::optional<AspectRatioValue> ParseAspectRatio(std::string_view raw) {
  std::string s = Lower(Trim(std::string(raw)));
  if (s == "auto")
    return AspectRatio::Auto;
  if (s == "wide" || s == "16:9" || s == "16x9")
    return AspectRatio::Wide;
  if (s == "standard" || s == "4:3" || s == "4x3")
    return AspectRatio::Standard;
  if (s == "stretch" || s == "window")
    return AspectRatio::Stretch;
  // Custom target ratio: "custom W:H", bare "W:H" ("21:9", "48:10"), or a
  // bare decimal ("2.37"). The decimal form is split digit-exact into an
  // integer ratio ("2.37" -> 237:100) so no binary rounding sneaks into the
  // range check or the serialized pair.
  if (s.starts_with("custom ") || s.starts_with("custom\t")) {
    s = Trim(std::string(std::string_view(s).substr(6)));
    if (s.empty())
      return std::nullopt;
  }
  std::int64_t w = 0, h = 0;
  const std::size_t colon = s.find(':');
  if (colon != std::string::npos) {
    if (!ParseAspectInt(std::string_view(s).substr(0, colon), &w) ||
        !ParseAspectInt(std::string_view(s).substr(colon + 1), &h))
      return std::nullopt;
  }
  else {
    if (!ParseAspectDecimal(s, &w, &h))
      return std::nullopt;
  }
  // The widescreen mod clamps its layout to [4:3, 32:9]; accept exactly
  // that window, compared as exact rationals (3w >= 4h and 9w <= 32h).
  if (w * 3 < h * 4 || w * 9 > h * 32)
    return std::nullopt;
  return AspectRatioValue{AspectRatio::Custom, static_cast<int>(w),
                          static_cast<int>(h)};
}

std::string AspectRatioToString(AspectRatioValue ratio) {
  switch (ratio.mode) {
    case AspectRatio::Auto:
      return "auto";
    case AspectRatio::Wide:
      return "16:9";
    case AspectRatio::Standard:
      return "4:3";
    case AspectRatio::Stretch:
      return "stretch";
    case AspectRatio::Custom:
      // Must satisfy the same window ParseAspectRatio accepts, or the
      // written file would fail to reload (SaveConfig treats "" as invalid).
      if (ratio.width <= 0 || ratio.height <= 0 ||
          static_cast<std::int64_t>(ratio.width) * 3 <
              static_cast<std::int64_t>(ratio.height) * 4 ||
          static_cast<std::int64_t>(ratio.width) * 9 >
              static_cast<std::int64_t>(ratio.height) * 32)
        return {};
      return "custom " + std::to_string(ratio.width) + ":" +
             std::to_string(ratio.height);
  }
  return {};
}

std::optional<Anisotropy> ParseAnisotropy(std::string_view raw) {
  const std::string s = Lower(Trim(std::string(raw)));
  if (s == "default" || s == "game")
    return Anisotropy::Default;
  if (s == "1" || s == "1x")
    return Anisotropy::X1;
  if (s == "2" || s == "2x")
    return Anisotropy::X2;
  if (s == "4" || s == "4x")
    return Anisotropy::X4;
  if (s == "8" || s == "8x")
    return Anisotropy::X8;
  if (s == "16" || s == "16x")
    return Anisotropy::X16;
  return std::nullopt;
}

std::string AnisotropyToString(Anisotropy mode) {
  switch (mode) {
    case Anisotropy::Default:
      return "default";
    case Anisotropy::X1:
      return "1x";
    case Anisotropy::X2:
      return "2x";
    case Anisotropy::X4:
      return "4x";
    case Anisotropy::X8:
      return "8x";
    case Anisotropy::X16:
      return "16x";
  }
  return {};
}

std::optional<WindowMode> ParseWindowMode(std::string_view raw) {
  const std::string s = Lower(Trim(std::string(raw)));
  if (s == "windowed" || s == "window" || s == "normal")
    return WindowMode::Windowed;
  if (s == "maximized" || s == "maximize" || s == "max")
    return WindowMode::Maximized;
  if (s == "fullscreen" || s == "borderless" || s == "full")
    return WindowMode::Fullscreen;
  return std::nullopt;
}

std::string WindowModeToString(WindowMode mode) {
  switch (mode) {
    case WindowMode::Windowed:
      return "windowed";
    case WindowMode::Maximized:
      return "maximized";
    case WindowMode::Fullscreen:
      return "fullscreen";
  }
  return {};
}

std::optional<AntiAliasing> ParseAntiAliasing(std::string_view raw) {
  const std::string s = Lower(Trim(std::string(raw)));
  if (s == "off" || s == "none" || s == "disabled" || s == "false" ||
      s == "0" || s == "1x" || s == "msaa1x" || s == "msaa 1x")
    return AntiAliasing::Off;
  if (s == "fxaa")
    return AntiAliasing::Fxaa;
  if (s == "2" || s == "2x" || s == "msaa2" || s == "msaa2x" || s == "msaa 2x")
    return AntiAliasing::Msaa2x;
  if (s == "4" || s == "4x" || s == "msaa4" || s == "msaa4x" || s == "msaa 4x")
    return AntiAliasing::Msaa4x;
  if (s == "8" || s == "8x" || s == "msaa8" || s == "msaa8x" || s == "msaa 8x")
    return AntiAliasing::Msaa8x;
  return std::nullopt;
}

std::string AntiAliasingToString(AntiAliasing mode) {
  switch (mode) {
    case AntiAliasing::Off:
      return "off";
    case AntiAliasing::Fxaa:
      return "fxaa";
    case AntiAliasing::Msaa2x:
      return "2x";
    case AntiAliasing::Msaa4x:
      return "4x";
    case AntiAliasing::Msaa8x:
      return "8x";
  }
  return {};
}

std::optional<bool> ParseFrameRate(std::string_view raw) {
  const std::string s = Lower(Trim(std::string(raw)));
  if (s == "uncapped" || s == "open" || s == "open-frame-rate" ||
      s == "open_frame_rate" || s == "unlimited" || s == "true" ||
      s == "1" || s == "yes" || s == "on")
    return true;
  if (s == "capped" || s == "closed" || s == "limited" || s == "vsync" ||
      s == "false" || s == "0" || s == "no" || s == "off")
    return false;
  return std::nullopt;
}

const std::vector<ResolutionOption> &SupportedResolutions() {
  // These are the output-resolution labels used by Dolphin's integer EFB
  // scales.
  static const std::vector<ResolutionOption> resolutions = {
      {"640x528", 1},   {"1280x720", 2},  {"1920x1080", 3},  {"2560x1440", 4},
      {"3840x2160", 6}, {"5120x2880", 8}, {"7680x4320", 12},
  };
  return resolutions;
}

const std::vector<GraphicsBackendOption> &SupportedGraphicsBackends() {
  static const std::vector<GraphicsBackendOption> backends = {
      {"Vulkan", "Vulkan"},
      {"OpenGL", "OGL"},
  };
  return backends;
}

ConfigResult LoadConfig(const fs::path &user_directory,
                        bool create_if_missing) {
  const fs::path path = user_directory / "config.ini";
  if (!fs::exists(path) && create_if_missing) {
    std::string error;
    if (!SaveConfig(user_directory, "1920x1080", true, {}, &error))
      return {.error = std::move(error)};
  }

  std::ifstream file(path);
  if (!file)
    return {.error = "can't open " + PathUtf8(path)};

  ConfigResult config;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' ||
        trimmed[0] == '[')
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos)
      return {.error = "invalid config.ini line: " + trimmed};
    const std::string key = Lower(Trim(trimmed.substr(0, separator)));
    const std::string raw_value = Trim(trimmed.substr(separator + 1));
    const std::string value = Lower(raw_value);
    if (key == "resolution")
      config.resolution = value;
    else if (key == "backend" || key == "graphics_backend") {
      config.graphics_backend = NormalizeGraphicsBackend(raw_value);
      if (config.graphics_backend.empty())
        return {.error = "graphics backend must be Vulkan or OpenGL"};
    }
    else if (key == "adapter" || key == "graphics_adapter") {
      if (value == "auto")
        config.graphics_adapter.reset();
      else {
        int adapter = 0;
        const auto parsed =
            std::from_chars(raw_value.data(),
                            raw_value.data() + raw_value.size(), adapter);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != raw_value.data() + raw_value.size() ||
            adapter < 0 || adapter > 16)
          return {.error =
                      "graphics adapter must be auto or an index from 0 to 16"};
        config.graphics_adapter = adapter;
      }
    }
    else if (key == "aspect" || key == "aspect_ratio") {
      const auto ratio = ParseAspectRatio(raw_value);
      if (!ratio)
        return {.error = "aspect_ratio must be auto, 16:9, 4:3, stretch, or "
                         "a custom ratio between 4:3 and 32:9 "
                         "(21:9, custom 48:10, 2.37...)"};
      config.aspect_ratio = *ratio;
    }
    else if (key == "widescreen" || key == "widescreen_hack") {
      bool enabled = false;
      if (!ParseBoolean(value, &enabled))
        return {.error = "widescreen_hack must be true or false"};
      config.widescreen_hack = enabled;
    }
    else if (key == "anisotropy" || key == "anisotropic_filtering") {
      const auto anisotropy = ParseAnisotropy(raw_value);
      if (!anisotropy)
        return {.error = "anisotropy must be default, 1x, 2x, 4x, 8x, or 16x"};
      config.anisotropy = *anisotropy;
    }
    else if (key == "present_mode" || key == "present") {
      auto m = ParsePresentMode(raw_value);
      if (!m)
        return {.error = "present_mode must be fifo, mailbox, or immediate"};
      config.present_mode = *m;
    } else if (key == "frame_rate" || key == "framerate") {
      auto u = ParseFrameRate(raw_value);
      if (!u)
        return {.error = "frame_rate must be capped or uncapped"};
      config.uncapped = *u;
    } else if (key == "controller")
      config.controller = raw_value;
    else if (key.starts_with("controller") && key.size() == 11 &&
             key.back() >= '1' && key.back() <= '4') {
      const std::size_t index = static_cast<std::size_t>(key.back() - '1');
      if (config.controllers.size() <= index)
        config.controllers.resize(index + 1);
      config.controllers[index] = raw_value;
    } else if (key == "background_input") {
      bool enabled = false;
      if (!ParseBoolean(value, &enabled))
        return {.error = "background_input must be true or false"};
      config.background_input = enabled;
    } else if (key == "show_fps_in_title") {
      if (!ParseBoolean(value, &config.show_fps_in_title))
        return {.error = "show_fps_in_title must be true or false"};
    } else if (key == "fullscreen") {
      if (!ParseBoolean(value, &config.fullscreen))
        return {.error = "fullscreen must be true or false"};
    } else if (key == "window_mode" || key == "windowmode") {
      const auto mode = ParseWindowMode(raw_value);
      if (!mode)
        return {.error =
                    "window_mode must be windowed, maximized, or fullscreen"};
      config.window_mode = *mode;
    } else if (key == "aa" || key == "anti_aliasing" ||
               key == "antialiasing") {
      const auto aa = ParseAntiAliasing(raw_value);
      if (!aa)
        return {.error = "aa must be off, fxaa, 2x, 4x, or 8x"};
      config.aa = *aa;
    } else if (key == "nickname")
      config.netplay_nickname = raw_value;
    else if (key == "address")
      config.netplay_address = raw_value;
    else if (key == "port") {
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          raw_value.data(), raw_value.data() + raw_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != raw_value.data() + raw_value.size() || port == 0 ||
          port > 65535)
        return {.error = "netplay port must be between 1 and 65535"};
      config.netplay_port = static_cast<std::uint16_t>(port);
    } else if (key == "buffer") {
      if (value != "auto") {
        unsigned int frames = 0;
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), frames);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || frames < 1 ||
            frames > 20)
          return {.error =
                      "netplay buffer must be auto or a value from 1 to 20"};
      }
      config.netplay_buffer = value;
    }
  }
  if (config.resolution.empty())
    return {.error = "config.ini is missing resolution=<width>x<height>"};

  std::erase(config.controllers, std::string{});
  if (config.controllers.empty() && !config.controller.empty())
    config.controllers.push_back(config.controller);
  if (config.controller.empty() && !config.controllers.empty())
    config.controller = config.controllers.front();
  if (config.netplay_nickname.empty())
    return {.error = "netplay nickname cannot be empty"};
  if (config.netplay_nickname.size() > 30)
    return {.error = "netplay nickname cannot exceed 30 characters"};
  if (!ValidNetplayAddress(config.netplay_address))
    return {.error = "netplay address must be an IPv4 address or hostname"};

  for (const ResolutionOption &option : SupportedResolutions()) {
    if (config.resolution == option.text) {
      config.dolphin_scale = option.dolphin_scale;
      return config;
    }
  }

  // Dolphin also accepts exact raw EFB multiples even when they do not have a
  // common display label.
  for (int scale = 1; scale <= 12; ++scale) {
    const std::string raw =
        std::to_string(640 * scale) + "x" + std::to_string(528 * scale);
    if (config.resolution == raw) {
      config.dolphin_scale = scale;
      return config;
    }
  }

  return {.error = "unsupported Dolphin internal resolution '" +
                   config.resolution +
                   "'; use a listed display resolution or an exact 640x528 "
                   "multiple up to 12x"};
}

bool SaveConfig(const fs::path &user_directory, const ConfigResult &config,
                std::string *error) {
  const std::string graphics_backend =
      NormalizeGraphicsBackend(config.graphics_backend);
  if (config.resolution.empty() || graphics_backend.empty() ||
      (config.graphics_adapter &&
       (*config.graphics_adapter < 0 || *config.graphics_adapter > 16)) ||
      (config.aspect_ratio &&
       AspectRatioToString(*config.aspect_ratio).empty()) ||
      (config.anisotropy &&
       AnisotropyToString(*config.anisotropy).empty()) ||
      (config.window_mode && WindowModeToString(*config.window_mode).empty()) ||
      (config.aa && AntiAliasingToString(*config.aa).empty()) ||
      config.netplay_nickname.empty() ||
      config.netplay_nickname.size() > 30 ||
      config.netplay_nickname.find_first_of("\r\n") != std::string::npos ||
      !ValidNetplayAddress(config.netplay_address) ||
      config.netplay_address.find_first_of("\r\n") != std::string::npos ||
      config.netplay_port == 0) {
    if (error)
      *error = "invalid frontend settings";
    return false;
  }
  if (config.netplay_buffer != "auto") {
    unsigned int frames = 0;
    const auto parsed = std::from_chars(
        config.netplay_buffer.data(),
        config.netplay_buffer.data() + config.netplay_buffer.size(), frames);
    if (parsed.ec != std::errc{} ||
        parsed.ptr !=
            config.netplay_buffer.data() + config.netplay_buffer.size() ||
        frames < 1 || frames > 20) {
      if (error)
        *error = "netplay buffer must be auto or a value from 1 to 20";
      return false;
    }
  }
  // Validate everything before opening with trunc: a controller name with a
  // newline found mid-write would otherwise leave a truncated config.ini
  // behind, and LoadConfig then reports the file corrupt.
  for (std::size_t i = 0; i < config.controllers.size() && i < 4; ++i) {
    if (config.controllers[i].find_first_of("\r\n") != std::string::npos) {
      if (error)
        *error = "controller device cannot contain a newline";
      return false;
    }
  }
  std::error_code ec;
  fs::create_directories(user_directory, ec);
  if (ec) {
    if (error)
      *error = "can't create user directory: " + ec.message();
    return false;
  }
  std::ofstream file(user_directory / "config.ini", std::ios::trunc);
  if (!file) {
    if (error)
      *error = "can't write " + PathUtf8(user_directory / "config.ini");
    return false;
  }
  file << "# ModernGekko frontend settings\n"
          "# This is Dolphin's internal render target, not the window size.\n"
          "[Video]\n"
          "resolution="
       << config.resolution << '\n'
       << "backend=" << graphics_backend << '\n'
       << "adapter="
       << (config.graphics_adapter ? std::to_string(*config.graphics_adapter)
                                   : "auto")
       << '\n';
  if (config.aspect_ratio)
    file << "aspect_ratio=" << AspectRatioToString(*config.aspect_ratio)
         << '\n';
  if (config.widescreen_hack)
    file << "widescreen_hack=" << (*config.widescreen_hack ? "true" : "false")
         << '\n';
  if (config.anisotropy)
    file << "anisotropy=" << AnisotropyToString(*config.anisotropy) << '\n';
  if (config.aa)
    file << "aa=" << AntiAliasingToString(*config.aa) << '\n';
  // window_mode is the authoritative key; fullscreen= stays written from the
  // effective mode (not the raw bool) so runners that predate window_mode
  // still honor the resolved choice.
  file << "fullscreen="
       << (config.EffectiveWindowMode() == WindowMode::Fullscreen ? "true"
                                                                : "false")
       << '\n'
       << "window_mode=" << WindowModeToString(config.EffectiveWindowMode())
       << '\n'
       << "show_fps_in_title=" << (config.show_fps_in_title ? "true" : "false")
       << '\n';
  if (config.present_mode)
    file << "present_mode=" << PresentModeToString(*config.present_mode) << '\n';
  if (config.uncapped)
    file << "frame_rate=" << (*config.uncapped ? "uncapped" : "capped") << '\n';
  file << "[Input]\n";
  if (config.background_input)
    file << "background_input=" << (*config.background_input ? "true" : "false")
         << '\n';
  for (std::size_t i = 0; i < config.controllers.size() && i < 4; ++i)
    file << "controller" << i + 1 << '=' << config.controllers[i] << '\n';
  file << "[Netplay]\n"
       << "nickname=" << config.netplay_nickname << '\n'
       << "address=" << config.netplay_address << '\n'
       << "port=" << config.netplay_port << '\n'
       << "buffer=" << config.netplay_buffer << '\n';
  file.flush();
  if (!file) {
    if (error)
      *error = "can't write " + PathUtf8(user_directory / "config.ini");
    return false;
  }
  return true;
}

bool SaveConfig(const fs::path &user_directory, std::string_view resolution,
                bool show_fps_in_title, std::string_view controller,
                std::string *error) {
  ConfigResult config = LoadConfig(user_directory, false);
  if (!config) {
    // A missing file is the normal first-run case; a present-but-unparseable
    // one must not be silently replaced by defaults — that would discard the
    // user's netplay/graphics settings on an unrelated option save.
    std::error_code exists_ec;
    if (fs::exists(user_directory / "config.ini", exists_ec)) {
      if (error)
        *error = std::move(config.error);
      return false;
    }
    config = {};
  }
  config.resolution = resolution;
  config.show_fps_in_title = show_fps_in_title;
  config.controller = controller;
  config.controllers.clear();
  if (!controller.empty())
    config.controllers.emplace_back(controller);
  return SaveConfig(user_directory, config, error);
}

std::string ReadConfiguredController(const fs::path &user_directory) {
  const std::vector<std::string> controllers =
      ReadConfiguredControllers(user_directory);
  return controllers.empty() ? std::string{} : controllers.front();
}

std::vector<std::string>
ReadConfiguredControllers(const fs::path &user_directory) {
  std::ifstream input(ControllerConfigPath(user_directory));
  std::vector<std::string> controllers;
  std::string line;
  std::size_t controller_index = 4;
  bool first_line = true;
  while (std::getline(input, line)) {
    const std::string_view view = StripUtf8Bom(line, first_line);
    first_line = false;
    if (IsSectionHeader(view)) {
      controller_index = PadSectionIndex(view);
      continue;
    }
    if (controller_index >= 4)
      continue;
    const std::string trimmed = Trim(std::string(view));
    const std::size_t separator = trimmed.find('=');
    if (separator != std::string::npos &&
        IsDeviceKey(trimmed.substr(0, separator))) {
      const std::string device =
          StripQuoted(Trim(trimmed.substr(separator + 1)));
      if (!device.empty()) {
        if (controllers.size() <= controller_index)
          controllers.resize(controller_index + 1);
        controllers[controller_index] = device;
      }
    }
  }
  std::erase(controllers, std::string{});
  return controllers;
}

bool ControllerConfigExists(const fs::path &user_directory) {
  return !ReadConfiguredControllers(user_directory).empty();
}

namespace {
// The "Group/Control" -> expression pairs WritePadMappingBody emits. This is
// the single source of truth for generated bindings: the ensure-merge
// serializes it and the launcher's remap screen reads the same set back via
// ReadControllerProfile to offer "Reset to defaults".
std::vector<std::pair<std::string, std::string>>
DefaultPadControlsImpl(std::string_view device) {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  if (device == "DInput/0/Keyboard Mouse")
  {
    return {
        {"Buttons/A", "X"},
        {"Buttons/B", "Z"},
        {"Buttons/X", "C"},
        {"Buttons/Y", "S"},
        {"Buttons/Z", "D"},
        {"Buttons/Start", "RETURN"},
        {"Main Stick/Up", "UP"},
        {"Main Stick/Down", "DOWN"},
        {"Main Stick/Left", "LEFT"},
        {"Main Stick/Right", "RIGHT"},
        {"Main Stick/Modifier", "Shift"},
        {"Main Stick/Modifier/Range", "50.00"},
        {"Main Stick/Calibration", "100.00"},
        {"C-Stick/Up", "I"},
        {"C-Stick/Down", "K"},
        {"C-Stick/Left", "J"},
        {"C-Stick/Right", "L"},
        {"C-Stick/Modifier", "Ctrl"},
        {"C-Stick/Modifier/Range", "50.00"},
        {"C-Stick/Calibration", "100.00"},
        {"Triggers/L", "Q"},
        {"Triggers/R", "W"},
        {"Triggers/L-Analog", "Q"},
        {"Triggers/R-Analog", "W"},
        {"D-Pad/Up", "T"},
        {"D-Pad/Down", "G"},
        {"D-Pad/Left", "F"},
        {"D-Pad/Right", "H"},
    };
  }
  if (device.starts_with("Pipe/"))
  {
    // Scripted QA input: Pipe controls are named "Button A"/"Button START"
    // and "Axis MAIN X +"-style split directions, so the generic pad body
    // would be mostly dead. Mirror the layout the QA profile ships with.
    return {
        {"Buttons/A", "`Button A`"},
        {"Buttons/B", "`Button B`"},
        {"Buttons/X", "`Button X`"},
        {"Buttons/Y", "`Button Y`"},
        {"Buttons/Z", "`Button Z`"},
        {"Buttons/L", "`Button L`"},
        {"Buttons/R", "`Button R`"},
        {"Buttons/Start", "`Button START`"},
        {"Main Stick/Up", "`Axis MAIN Y +`"},
        {"Main Stick/Down", "`Axis MAIN Y -`"},
        {"Main Stick/Left", "`Axis MAIN X -`"},
        {"Main Stick/Right", "`Axis MAIN X +`"},
        {"Main Stick/Calibration", "100.00"},
        {"C-Stick/Up", "`Axis C Y +`"},
        {"C-Stick/Down", "`Axis C Y -`"},
        {"C-Stick/Left", "`Axis C X -`"},
        {"C-Stick/Right", "`Axis C X +`"},
        {"C-Stick/Calibration", "100.00"},
        {"Triggers/L", "`Button L`"},
        {"Triggers/R", "`Button R`"},
        {"D-Pad/Up", "`Button D_UP`"},
        {"D-Pad/Down", "`Button D_DOWN`"},
        {"D-Pad/Left", "`Button D_LEFT`"},
        {"D-Pad/Right", "`Button D_RIGHT`"},
    };
  }
  return {
      {"Buttons/A", "`Button A`"},
      {"Buttons/B", "`Button B`"},
      {"Buttons/X", "`Button X`"},
      {"Buttons/Y", "`Button Y`"},
      {"Buttons/Z", "`Shoulder R`"},
      {"Buttons/Start", "Start"},
      {"Main Stick/Up", "`Left Y+`"},
      {"Main Stick/Down", "`Left Y-`"},
      {"Main Stick/Left", "`Left X-`"},
      {"Main Stick/Right", "`Left X+`"},
      {"Main Stick/Calibration", "100.00"},
      // Camera stick Y is inverted from the console by default. Dolphin names
      // SDL axes the XInput way (`Right Y+` is stick up), so the same pair
      // fits XInput and SDL pads.
      {"C-Stick/Up", "`Right Y-`"},
      {"C-Stick/Down", "`Right Y+`"},
      {"C-Stick/Left", "`Right X-`"},
      {"C-Stick/Right", "`Right X+`"},
      {"C-Stick/Calibration", "100.00"},
      {"Triggers/L", "`Trigger L`"},
      {"Triggers/R", "`Trigger R`"},
      {"Triggers/L-Analog", "`Trigger L`"},
      {"Triggers/R-Analog", "`Trigger R`"},
      {"D-Pad/Up", "`Pad N`"},
      {"D-Pad/Down", "`Pad S`"},
      {"D-Pad/Left", "`Pad W`"},
      {"D-Pad/Right", "`Pad E`"},
      {"Rumble/Motor", "`Motor L` | `Motor R`"},
  };
#else
  return {
      {"Buttons/A", "`Shoulder L`"},
      {"Buttons/B", "`Shoulder R`"},
      {"Buttons/1", "`Button W`"},
      {"Buttons/2", "`Button S`"},
      {"Buttons/-", "Back"},
      {"Buttons/+", "Start"},
      {"Buttons/Home", "Guide"},
      {"D-Pad/Up", "`Pad N` | `Left Y+`"},
      {"D-Pad/Down", "`Pad S` | `Left Y-`"},
      {"D-Pad/Left", "`Pad W` | `Left X-`"},
      {"D-Pad/Right", "`Pad E` | `Left X+`"},
      {"IR/Up", "`Cursor Y-`"},
      {"IR/Down", "`Cursor Y+`"},
      {"IR/Left", "`Cursor X-`"},
      {"IR/Right", "`Cursor X+`"},
      {"Shake/X", "`Trigger L`"},
      {"Shake/Y", "`Trigger R`"},
      {"Shake/Z", "`Trigger L`"},
      {"IRPassthrough/Object 1 X", "`IR Object 1 X`"},
      {"IRPassthrough/Object 1 Y", "`IR Object 1 Y`"},
      {"IRPassthrough/Object 1 Size", "`IR Object 1 Size`"},
      {"IRPassthrough/Object 2 X", "`IR Object 2 X`"},
      {"IRPassthrough/Object 2 Y", "`IR Object 2 Y`"},
      {"IRPassthrough/Object 2 Size", "`IR Object 2 Size`"},
      {"IRPassthrough/Object 3 X", "`IR Object 3 X`"},
      {"IRPassthrough/Object 3 Y", "`IR Object 3 Y`"},
      {"IRPassthrough/Object 3 Size", "`IR Object 3 Size`"},
      {"IRPassthrough/Object 4 X", "`IR Object 4 X`"},
      {"IRPassthrough/Object 4 Y", "`IR Object 4 Y`"},
      {"IRPassthrough/Object 4 Size", "`IR Object 4 Size`"},
      {"IMUAccelerometer/Up", "`Accel Up`"},
      {"IMUAccelerometer/Down", "`Accel Down`"},
      {"IMUAccelerometer/Left", "`Accel Left`"},
      {"IMUAccelerometer/Right", "`Accel Right`"},
      {"IMUAccelerometer/Forward", "`Accel Forward`"},
      {"IMUAccelerometer/Backward", "`Accel Backward`"},
      {"IMUGyroscope/Pitch Up", "`Gyro Pitch Up`"},
      {"IMUGyroscope/Pitch Down", "`Gyro Pitch Down`"},
      {"IMUGyroscope/Roll Left", "`Gyro Roll Left`"},
      {"IMUGyroscope/Roll Right", "`Gyro Roll Right`"},
      {"IMUGyroscope/Yaw Left", "`Gyro Yaw Left`"},
      {"IMUGyroscope/Yaw Right", "`Gyro Yaw Right`"},
      {"Rumble/Motor", "Motor"},
      {"Extension", "None"},
      {"Options/Sideways Wiimote", "True"},
  };
#endif
}
} // namespace

// Public copy of the generated-binding table for the launcher remap screen.
std::vector<std::pair<std::string, std::string>>
DefaultPadControls(std::string_view device) {
  return DefaultPadControlsImpl(device);
}

namespace {
// Writes the default mapping body (everything after the Device line) for a
// bound device. Kept separate from WritePadSection so the ensure-merge can
// splice the body into a bound-but-empty pad section of an existing profile.
void WritePadMappingBody(std::ostream &output, std::string_view device) {
  for (const auto &[key, expression] : DefaultPadControlsImpl(device))
    output << key << " = " << expression << "\n";
}

// Writes one pad section: header, the Device binding, and the default mapping
// body for that device. An empty device emits the bare header, matching the
// unused slots in a freshly generated profile.
void WritePadSection(std::ostream &output, std::size_t index,
                     std::string_view device) {
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  output << "[GCPad" << index + 1 << "]\n";
#else
  output << "[Wiimote" << index + 1 << "]\n";
#endif
  if (device.empty())
    return;
  output << "Device = " << device << '\n';
  WritePadMappingBody(output, device);
}
} // namespace

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message) {
  if (!ValidControllerList(controllers, message))
    return false;

  const fs::path destination = ControllerConfigPath(user_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + PathUtf8(destination);
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i)
    WritePadSection(output, i,
                    i < controllers.size() ? std::string_view(controllers[i])
                                           : std::string_view{});
#ifndef MODERNGEKKO_GAMECUBE_CONTROLLERS
  output << "[BalanceBoard]\n";
#endif
  if (!output) {
    if (message)
      *message = "can't write " + PathUtf8(destination);
    return false;
  }
  if (message)
    *message = std::to_string(controllers.size()) +
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
               " GameCube controller" +
#else
               " sideways Wii Remote" +
#endif
               (controllers.size() == 1 ? " mapped" : "s mapped");
  return true;
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::string_view controller,
                              std::string *message) {
  const std::string value(controller);
  return GenerateControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message) {
  if (ControllerConfigExists(user_directory)) {
    if (message)
      *message = "using existing controller profile";
    return true;
  }
  const fs::path destination = ControllerConfigPath(user_directory);
  std::error_code ec;
  const bool file_present = fs::is_regular_file(destination, ec) && !ec;
  if (!file_present)
    return GenerateControllerConfig(user_directory, controllers, message);

  // The file exists but no pad section binds a Device, so it gives the game
  // no input — the case ControllerConfigExists now reports as absent. It can
  // still carry hand-edited mappings though, so bind the selected devices in
  // place instead of truncating the profile the way GenerateControllerConfig
  // (the explicit "Replace" action) does.
  if (!ValidControllerList(controllers, message))
    return false;
  std::ifstream input(destination);
  if (!input) {
    if (message)
      *message = "can't read " + PathUtf8(destination);
    return false;
  }
  std::vector<std::string> lines;
  std::string line;
  std::size_t section = 4;
  std::array<bool, 4> section_seen{};
  std::array<bool, 4> device_bound{};
  std::array<bool, 4> section_has_controls{};
  bool first_line = true;
  while (std::getline(input, line)) {
    const std::string_view view = StripUtf8Bom(line, first_line);
    first_line = false;
    if (IsSectionHeader(view)) {
      section = PadSectionIndex(view);
      if (section < 4)
        section_seen[section] = true;
      lines.push_back(std::move(line));
      // Bind right under the header; any Device line the section already had
      // is dropped below (keys compare case-insensitively in Dolphin's
      // IniFile, so a lowercase `device` must go too or it would win), which
      // also dedupes a repeated section.
      if (section < controllers.size() && !device_bound[section]) {
        lines.push_back("Device = " + controllers[section]);
        device_bound[section] = true;
      }
      continue;
    }
    if (section < 4) {
      const std::string trimmed = Trim(std::string(view));
      const std::size_t separator = trimmed.find('=');
      if (separator != std::string::npos && !trimmed.starts_with(';')) {
        // Keys compare case-insensitively in Dolphin's IniFile, so a
        // pre-existing `device` line must go once we bound our own.
        if (device_bound[section] &&
            IsDeviceKey(trimmed.substr(0, separator)))
          continue;
        section_has_controls[section] = true;
      }
    }
    lines.push_back(std::move(line));
  }
  if (input.bad()) {
    if (message)
      *message = "can't read " + PathUtf8(destination);
    return false;
  }

  // A pad section we just bound a Device to but that carried no control
  // expressions is a connected-but-dead pad: the game would load a device
  // with zero mappings. Splice the standard mapping body in at the end of
  // the section — sections that already carried key=value lines keep their
  // contents untouched.
  std::vector<std::string> merged;
  merged.reserve(lines.size() + 64);
  std::size_t open_section = 4;
  const auto close_open_section = [&] {
    if (open_section < 4 && device_bound[open_section] &&
        !section_has_controls[open_section]) {
      std::ostringstream body;
      WritePadMappingBody(body, controllers[open_section]);
      std::istringstream body_lines(body.str());
      for (std::string body_line; std::getline(body_lines, body_line);) {
        if (!body_line.empty())
          merged.push_back(std::move(body_line));
      }
    }
  };
  bool first_kept_line = true;
  for (std::string &kept : lines) {
    // `kept` still carries the original bytes (a first-line BOM is preserved
    // in the output), so apply the same strip the read pass did before the
    // header check — otherwise a BOM'd "[GCPad1]" never opens the section
    // and its close-splice never runs.
    const std::string_view view = StripUtf8Bom(kept, first_kept_line);
    first_kept_line = false;
    if (IsSectionHeader(view)) {
      close_open_section();
      open_section = PadSectionIndex(view);
    }
    merged.push_back(std::move(kept));
  }
  close_open_section();

  // Rewrite through a sibling temp + copy so a failed write can't leave the
  // user's profile half-merged; rename() can't overwrite on Windows. Build
  // the staging name with operator+= so a non-ASCII file name stays in the
  // native encoding rather than round-tripping through path::string().
  fs::path staging = destination;
  staging += ".mgktmp";
  {
    std::ofstream output(staging, std::ios::trunc);
    if (!output) {
      if (message)
        *message = "can't write " + PathUtf8(staging);
      return false;
    }
    for (const std::string &kept : merged)
      output << kept << '\n';
    for (std::size_t i = 0; i < 4; ++i) {
      if (!section_seen[i])
        WritePadSection(output, i,
                        i < controllers.size()
                            ? std::string_view(controllers[i])
                            : std::string_view{});
    }
    output.flush();
    if (!output) {
      // A partial staging file would otherwise linger in the user's Config
      // directory; the merge target is untouched either way. Close the
      // handle first — Windows cannot remove an open file.
      output.close();
      std::error_code staging_ec;
      fs::remove(staging, staging_ec);
      if (message)
        *message = "can't write " + PathUtf8(staging);
      return false;
    }
  }
  ec.clear();
  fs::copy_file(staging, destination, fs::copy_options::overwrite_existing, ec);
  std::error_code remove_ec;
  fs::remove(staging, remove_ec);
  if (ec) {
    if (message)
      *message = "can't write " + PathUtf8(destination) + ": " + ec.message();
    return false;
  }
  if (message)
    *message = "bound " + std::to_string(controllers.size()) +
               " controller device" + (controllers.size() == 1 ? "" : "s") +
               " in the existing profile";
  return true;
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::string_view controller, std::string *message) {
  const std::string value(controller);
  return EnsureControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}

std::array<PadPortSettings, 4>
ReadControllerProfile(const fs::path &user_directory) {
  std::array<PadPortSettings, 4> ports;
  std::ifstream input(ControllerConfigPath(user_directory));
  if (!input)
    return ports;

  // Parse like IniFile::Load: a pad section header opens the port, `Device`
  // (case-insensitive) is the binding and every other key=value line is a
  // control expression kept verbatim. `#`/`;` comment lines and blank lines
  // never reach the key test. A repeated section header just re-opens the
  // same port, matching Dolphin's unordered_map-by-name sections.
  std::string line;
  std::size_t section = 4;
  bool first_line = true;
  while (std::getline(input, line)) {
    const std::string_view view = StripUtf8Bom(line, first_line);
    first_line = false;
    if (IsSectionHeader(view)) {
      section = PadSectionIndex(view);
      continue;
    }
    if (section >= 4)
      continue;
    const std::string trimmed = Trim(std::string(view));
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos || trimmed.starts_with('#') ||
        trimmed.starts_with(';'))
      continue;
    const std::string key = Trim(trimmed.substr(0, separator));
    if (key.empty())
      continue;
    if (IsDeviceKey(key)) {
      ports[section].device =
          StripQuoted(Trim(trimmed.substr(separator + 1)));
      continue;
    }
    ports[section].controls.emplace_back(
        std::move(key), StripQuoted(Trim(trimmed.substr(separator + 1))));
  }
  return ports;
}

bool SaveControllerProfile(const fs::path &user_directory,
                           std::span<const PadPortSettings> ports,
                           std::string *message) {
  if (ports.size() > 4) {
    if (message)
      *message = "can't save more than four controller ports";
    return false;
  }
  for (const PadPortSettings &port : ports) {
    if (port.device.find_first_of("\r\n") != std::string::npos) {
      if (message)
        *message = "invalid controller device";
      return false;
    }
    for (const auto &[key, value] : port.controls) {
      if (key.empty() || key.find('=') != std::string::npos ||
          key.find_first_of("\r\n") != std::string::npos ||
          value.find_first_of("\r\n") != std::string::npos) {
        if (message)
          *message = "invalid controller mapping";
        return false;
      }
    }
  }

  const fs::path destination = ControllerConfigPath(user_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }

  // Same staging + copy dance as the ensure-merge: a failed write must not
  // leave the user's profile half-saved, and rename() can't overwrite on
  // Windows.
  fs::path staging = destination;
  staging += ".mgktmp";
  {
    std::ofstream output(staging, std::ios::trunc);
    if (!output) {
      if (message)
        *message = "can't write " + PathUtf8(staging);
      return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      output << ControllerSectionPrefix() << i + 1 << "]\n";
      if (i >= ports.size() || ports[i].device.empty())
        continue;
      output << "Device = " << ports[i].device << '\n';
      for (const auto &[key, value] : ports[i].controls)
        output << key << " = " << value << '\n';
    }
#ifndef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[BalanceBoard]\n";
#endif
    output.flush();
    if (!output) {
      output.close();
      std::error_code staging_ec;
      fs::remove(staging, staging_ec);
      if (message)
        *message = "can't write " + PathUtf8(staging);
      return false;
    }
  }
  ec.clear();
  fs::copy_file(staging, destination, fs::copy_options::overwrite_existing,
                ec);
  std::error_code remove_ec;
  fs::remove(staging, remove_ec);
  if (ec) {
    if (message)
      *message = "can't write " + PathUtf8(destination) + ": " + ec.message();
    return false;
  }
  if (message)
    *message = "controller settings saved";
  return true;
}
} // namespace moderngekko::frontend
