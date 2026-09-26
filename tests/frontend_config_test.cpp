#include "frontend_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr const char *CONTROLLER_CONFIG_NAME = "GCPadNew.ini";
#else
constexpr const char *CONTROLLER_CONFIG_NAME = "WiimoteNew.ini";
#endif

int main() {
  namespace fs = std::filesystem;
  const fs::path directory =
      fs::temp_directory_path() /
      ("moderngekko-frontend-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string error;
  const std::string controller = "SDL/0/Test Controller";
  if (!moderngekko::frontend::SaveConfig(directory, "1920x1080", false,
                                         controller, &error))
    return 1;

  const auto loaded = moderngekko::frontend::LoadConfig(directory, false);
  if (!loaded || loaded.dolphin_scale != 3 || loaded.show_fps_in_title ||
      loaded.controller != controller ||
      loaded.graphics_backend != "Vulkan") {
    return 2;
  }
  if (loaded.aspect_ratio || loaded.widescreen_hack || loaded.anisotropy ||
      loaded.background_input)
    return 24;

  if (moderngekko::frontend::ParseAspectRatio(" 16:9 ") !=
          moderngekko::frontend::AspectRatio::Wide ||
      moderngekko::frontend::ParseAspectRatio("bogus").has_value() ||
      moderngekko::frontend::ParseAnisotropy("16x") !=
          moderngekko::frontend::Anisotropy::X16 ||
      moderngekko::frontend::ParseAnisotropy("32x").has_value()) {
    return 19;
  }

  // Custom aspect ratios: bare "W:H", "custom W:H", and decimal forms all
  // produce AspectRatio::Custom carrying the exact integer pair. Values
  // outside the widescreen mod's [4:3, 32:9] window and malformed input are
  // rejected so a written config can't drift from what gets applied.
  {
    using moderngekko::frontend::AspectRatio;
    using moderngekko::frontend::AspectRatioValue;
    const auto ar = [](const char* s) {
      return moderngekko::frontend::ParseAspectRatio(s);
    };
    const auto c21 = ar("21:9");
    const auto cc48 = ar("custom 48:20");
    const auto c237 = ar("2.37");
    const auto c32 = ar("32:9");
    const auto c169 = ar("custom 16:9");
    const auto c43 = ar("4:3 custom");
    if (!c21 || c21->mode != AspectRatio::Custom || c21->width != 21 ||
        c21->height != 9 || !cc48 || cc48->width != 48 ||
        cc48->height != 20 || !c237 || c237->width != 237 ||
        c237->height != 100 || !c32 || c32->width != 32 ||
        c32->height != 9 || !c169 || c169->width != 16 ||
        c169->height != 9 || c43) {
      return 29;
    }
    // Out-of-window / malformed customs must fail (the mod clamps, config
    // rejects): below 4:3, above 32:9, zero/negative sides, junk.
    if (ar("5:4") || ar("1:1") || ar("64:9") || ar("33:9") || ar("0:9") ||
        ar("21:0") || ar("-21:9") || ar("21:9x") || ar("2.37.5") ||
        ar("2.") || ar(".5") || ar("custom") || ar("custom x:y") ||
        ar("1e2:9") || ar("2.3333333") /* 7 frac digits */) {
      return 30;
    }
    // Serializer: fixed modes unchanged; Custom emits "custom W:H" and
    // refuses pairs outside the window (SaveConfig treats "" as invalid).
    if (moderngekko::frontend::AspectRatioToString(AspectRatio::Wide) !=
            "16:9" ||
        moderngekko::frontend::AspectRatioToString(
            AspectRatioValue{AspectRatio::Custom, 21, 9}) != "custom 21:9" ||
        !moderngekko::frontend::AspectRatioToString(
             AspectRatioValue{AspectRatio::Custom, 64, 9})
             .empty() ||
        !moderngekko::frontend::AspectRatioToString(
             AspectRatioValue{AspectRatio::Custom, 0, 9})
             .empty()) {
      return 31;
    }
  }

  moderngekko::frontend::ConfigResult netplay_config = loaded;
  netplay_config.graphics_backend = "OpenGL";
  netplay_config.fullscreen = true;
  netplay_config.controllers = {controller, "SDL/1/Second Controller"};
  netplay_config.controller = controller;
  netplay_config.netplay_nickname = "Kirby";
  netplay_config.netplay_address = "192.168.1.50";
  netplay_config.netplay_port = 34567;
  netplay_config.netplay_buffer = "auto";
  netplay_config.graphics_adapter = 1;
  netplay_config.aspect_ratio = moderngekko::frontend::AspectRatio::Wide;
  netplay_config.widescreen_hack = true;
  netplay_config.anisotropy = moderngekko::frontend::Anisotropy::X16;
  netplay_config.background_input = true;
  if (!moderngekko::frontend::SaveConfig(directory, netplay_config, &error))
    return 6;
  const auto netplay_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!netplay_loaded ||
      netplay_loaded.controllers != netplay_config.controllers ||
      netplay_loaded.netplay_nickname != "Kirby" ||
      netplay_loaded.netplay_address != "192.168.1.50" ||
      netplay_loaded.netplay_port != 34567 ||
      netplay_loaded.netplay_buffer != "auto" ||
      netplay_loaded.graphics_backend != "OGL" ||
      netplay_loaded.graphics_adapter != 1 ||
      netplay_loaded.aspect_ratio != moderngekko::frontend::AspectRatio::Wide ||
      netplay_loaded.widescreen_hack != true ||
      netplay_loaded.anisotropy != moderngekko::frontend::Anisotropy::X16 ||
      netplay_loaded.background_input != true ||
      !netplay_loaded.fullscreen) {
    return 7;
  }

  std::string serialized;
  {
    std::ifstream config_input(directory / "config.ini");
    serialized.assign(std::istreambuf_iterator<char>(config_input),
                      std::istreambuf_iterator<char>());
  }
  if (!serialized.contains("adapter=1\n") ||
      !serialized.contains("aspect_ratio=16:9\n") ||
      !serialized.contains("widescreen_hack=true\n") ||
      !serialized.contains("anisotropy=16x\n") ||
      !serialized.contains("background_input=true\n")) {
    return 20;
  }

  // Hand-written file: "auto" maps back to no override, explicit false parses
  // as an engaged false (distinct from unset), and aliases resolve.
  {
    std::ofstream raw(directory / "config.ini", std::ios::trunc);
    raw << "[Video]\nresolution=1920x1080\nbackend=Vulkan\nadapter=auto\n"
           "aspect_ratio=stretch\nwidescreen_hack=false\n"
           "anisotropic_filtering=8x\nfullscreen=false\n"
           "show_fps_in_title=true\n"
           "[Input]\nbackground_input=false\n"
           "[Netplay]\nnickname=P\naddress=127.0.0.1\nport=2626\nbuffer=auto\n";
  }
  const auto raw_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!raw_loaded || raw_loaded.graphics_adapter ||
      raw_loaded.aspect_ratio != moderngekko::frontend::AspectRatio::Stretch ||
      raw_loaded.widescreen_hack != false ||
      raw_loaded.anisotropy != moderngekko::frontend::Anisotropy::X8 ||
      raw_loaded.background_input != false) {
    return 25;
  }

  // A hand-written custom ratio round-trips through LoadConfig/SaveConfig
  // with the W:H pair intact and serializes back to "custom W:H".
  {
    std::ofstream raw(directory / "config.ini", std::ios::trunc);
    raw << "[Video]\nresolution=1920x1080\nbackend=Vulkan\nadapter=auto\n"
           "aspect_ratio=custom 32:9\nfullscreen=false\n"
           "show_fps_in_title=true\n"
           "[Netplay]\nnickname=P\naddress=127.0.0.1\nport=2626\nbuffer=auto\n";
  }
  auto custom_loaded = moderngekko::frontend::LoadConfig(directory, false);
  if (!custom_loaded || !custom_loaded.aspect_ratio ||
      custom_loaded.aspect_ratio->mode !=
          moderngekko::frontend::AspectRatio::Custom ||
      custom_loaded.aspect_ratio->width != 32 ||
      custom_loaded.aspect_ratio->height != 9) {
    return 32;
  }
  if (!moderngekko::frontend::SaveConfig(directory, custom_loaded, &error))
    return 33;
  {
    std::ifstream config_input(directory / "config.ini");
    serialized.assign(std::istreambuf_iterator<char>(config_input),
                      std::istreambuf_iterator<char>());
  }
  if (!serialized.contains("aspect_ratio=custom 32:9\n"))
    return 34;

  const auto write_bad = [&](const char *line) {
    std::ofstream raw(directory / "config.ini", std::ios::trunc);
    raw << "[Video]\nresolution=1920x1080\nbackend=Vulkan\n"
        << line << "\n[Netplay]\nnickname=P\naddress=127.0.0.1\n"
                    "port=2626\nbuffer=auto\n";
  };
  write_bad("adapter=17");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 26;
  write_bad("adapter=+2");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 26;
  write_bad("aspect_ratio=64:9");      /* beyond the 32:9 clamp window   */
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 27;
  write_bad("aspect_ratio=1:1");       /* below the 4:3 window           */
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 27;
  write_bad("aspect_ratio=21:x");      /* malformed custom ratio         */
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 27;
  write_bad("anisotropy=32x");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 27;
  write_bad("widescreen_hack=maybe");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 28;
  write_bad("background_input=maybe");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 28;

  auto invalid_netplay = netplay_config;
  invalid_netplay.netplay_address = "not a host";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 8;
  invalid_netplay = netplay_config;
  invalid_netplay.netplay_nickname = std::string(31, 'K');
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 9;
  invalid_netplay = netplay_config;
  invalid_netplay.graphics_backend = "Direct3D 9";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 13;
  invalid_netplay = netplay_config;
  invalid_netplay.graphics_adapter = 17;
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 21;
  invalid_netplay = netplay_config;
  invalid_netplay.aspect_ratio = moderngekko::frontend::AspectRatioValue{
      static_cast<moderngekko::frontend::AspectRatio>(99), 0, 0};
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 22;
  invalid_netplay = netplay_config;
  invalid_netplay.anisotropy =
      static_cast<moderngekko::frontend::Anisotropy>(99);
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 23;
  {
    std::error_code ec;
    fs::create_directories(directory / "Config", ec);
    std::ofstream empty(directory / "Config" / CONTROLLER_CONFIG_NAME);
  }
  if (moderngekko::frontend::ControllerConfigExists(directory))
    return 14;

  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, netplay_config.controllers, &error))
    return 3;
  if (moderngekko::frontend::ReadConfiguredController(directory) != controller)
    return 4;
  if (moderngekko::frontend::ReadConfiguredControllers(directory) !=
      netplay_config.controllers)
    return 10;

  // Scoped so the handle is closed before the cleanup below. Windows refuses
  // to delete a file that is still open, where POSIX allows it, so leaving
  // these open makes remove_all throw there and only there.
  std::string generated;
  {
    std::ifstream input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    generated.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  if (!generated.contains("Buttons/A = `Button A`\n") ||
      !generated.contains("Buttons/Z = `Shoulder R`\n") ||
      !generated.contains("Main Stick/Up = `Left Y+`\n") ||
      !generated.contains("C-Stick/Up = `Right Y-`\n") ||
      !generated.contains("C-Stick/Down = `Right Y+`\n") ||
      !generated.contains("Triggers/L-Analog = `Trigger L`\n") ||
      !generated.contains("Rumble/Motor = `Motor L` | `Motor R`\n") ||
      !generated.contains("[GCPad2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("[Wiimote") || generated.contains("[BalanceBoard]")) {
    return 5;
  }
#else
  if (!generated.contains("Buttons/A = `Shoulder L`\n") ||
      !generated.contains("Buttons/1 = `Button W`\n") ||
      !generated.contains("Buttons/2 = `Button S`\n") ||
      !generated.contains("Shake/X = `Trigger L`\n") ||
      !generated.contains("D-Pad/Up = `Pad N` | `Left Y+`\n") ||
      !generated.contains("D-Pad/Right = `Pad E` | `Left X+`\n") ||
      !generated.contains("Extension = None\n") ||
      !generated.contains("Options/Sideways Wiimote = True\n") ||
      !generated.contains("[Wiimote2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("Nunchuk/")) {
    return 5;
  }
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, "DInput/0/Keyboard Mouse", &error))
    return 15;
  {
    std::ifstream input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    generated.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
  }
  if (!generated.contains("Device = DInput/0/Keyboard Mouse\n") ||
      !generated.contains("Buttons/A = X\n") ||
      !generated.contains("Buttons/Start = RETURN\n") ||
      !generated.contains("Main Stick/Up = UP\n") ||
      !generated.contains("Main Stick/Modifier = Shift\n") ||
      !generated.contains("C-Stick/Up = I\n") ||
      !generated.contains("C-Stick/Modifier = Ctrl\n") ||
      !generated.contains("Triggers/L-Analog = Q\n") ||
      !generated.contains("D-Pad/Right = H\n")) {
    return 16;
  }
  if (generated.contains("`Button A`") || generated.contains("`Motor L`"))
    return 17;
  if (moderngekko::frontend::ReadConfiguredController(directory) !=
      "DInput/0/Keyboard Mouse")
    return 18;
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const std::string custom =
      "[GCPad1]\nDevice = SDL/9/Custom Controller\nButtons/A = Custom\n";
#else
  const std::string custom =
      "[Wiimote1]\nDevice = SDL/9/Custom Controller\nButtons/1 = Custom\n";
#endif
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
    output << custom;
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, netplay_config.controllers, &error))
    return 11;
  std::string preserved;
  {
    std::ifstream custom_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    preserved.assign(std::istreambuf_iterator<char>(custom_input),
                     std::istreambuf_iterator<char>());
  }
  if (preserved != custom || moderngekko::frontend::ReadConfiguredController(
                                 directory) != "SDL/9/Custom Controller")
    return 12;

  // A hand-edited profile with sections but no Device bindings must be filled
  // in, not truncated: unrecognized key/value pairs survive the merge.
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad1]\nButtons/A = Custom\n[GCPad2]\nDevice = \nButtons/B = Y\n";
#else
    output << "[Wiimote1]\nButtons/1 = Custom\n[Wiimote2]\nDevice = \nButtons/2 = Y\n";
#endif
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, "SDL/0/Merge Pad", &error))
    return 29;
  std::string merged;
  {
    std::ifstream merged_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    merged.assign(std::istreambuf_iterator<char>(merged_input),
                  std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const bool merged_ok =
      merged.contains("[GCPad1]\nDevice = SDL/0/Merge Pad\n") &&
      merged.contains("Buttons/A = Custom\n") &&
      merged.contains("Buttons/B = Y\n");
#else
  const bool merged_ok =
      merged.contains("[Wiimote1]\nDevice = SDL/0/Merge Pad\n") &&
      merged.contains("Buttons/1 = Custom\n") &&
      merged.contains("Buttons/2 = Y\n");
#endif
  if (!merged_ok || moderngekko::frontend::ReadConfiguredController(
                        directory) != "SDL/0/Merge Pad")
    return 30;

  // A profile file with no pad sections at all gets sections appended while
  // its own content is left alone.
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
    output << "[General]\nNothing = Related\n";
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, "SDL/0/Appended Pad", &error))
    return 31;
  merged.clear();
  {
    std::ifstream merged_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    merged.assign(std::istreambuf_iterator<char>(merged_input),
                  std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  if (!merged.contains("Nothing = Related\n") ||
      !merged.contains("[GCPad1]\nDevice = SDL/0/Appended Pad\n"))
    return 32;
#else
  if (!merged.contains("Nothing = Related\n") ||
      !merged.contains("[Wiimote1]\nDevice = SDL/0/Appended Pad\n"))
    return 32;
#endif

  // Dolphin's IniFile compares section names and keys case-insensitively, so
  // a lowercase `[gcpad1]`/`device` pair is a bound pad to the emulator; the
  // reader must report it as such rather than treating the file as unbound.
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[gcpad1]\ndevice = SDL/9/Lower Pad\nButtons/A = Custom\n";
#else
    output << "[wiimote1]\ndevice = SDL/9/Lower Pad\nButtons/1 = Custom\n";
#endif
  }
  if (!moderngekko::frontend::ControllerConfigExists(directory) ||
      moderngekko::frontend::ReadConfiguredController(directory) !=
          "SDL/9/Lower Pad")
    return 33;

  // The same parity applies inside the merge: a stale case-variant `device`
  // line must be dropped once the section is bound, or it would override the
  // fresh Device (IniFile keeps the last value seen for a key).
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad1]\ndevice = \nButtons/A = Custom\n";
#else
    output << "[Wiimote1]\ndevice = \nButtons/1 = Custom\n";
#endif
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, "SDL/0/Merge Pad", &error))
    return 34;
  merged.clear();
  {
    std::ifstream merged_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    merged.assign(std::istreambuf_iterator<char>(merged_input),
                  std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const bool case_merge_ok =
      merged.contains("[GCPad1]\nDevice = SDL/0/Merge Pad\n") &&
      !merged.contains("device =") && merged.contains("Buttons/A = Custom\n");
#else
  const bool case_merge_ok =
      merged.contains("[Wiimote1]\nDevice = SDL/0/Merge Pad\n") &&
      !merged.contains("device =") && merged.contains("Buttons/1 = Custom\n");
#endif
  if (!case_merge_ok ||
      moderngekko::frontend::ReadConfiguredController(directory) !=
          "SDL/0/Merge Pad")
    return 35;

  // A header's name runs to the first ']' under IniFile — "[GCPad1] note" is
  // still the pad section. A leading space disqualifies it there instead.
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad1] player one\nDevice = SDL/8/Trailing Pad\n";
#else
    output << "[Wiimote1] player one\nDevice = SDL/8/Trailing Pad\n";
#endif
  }
  if (moderngekko::frontend::ReadConfiguredController(directory) !=
      "SDL/8/Trailing Pad")
    return 36;
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "  [GCPad1]\nDevice = SDL/7/Indented Pad\n";
#else
    output << "  [Wiimote1]\nDevice = SDL/7/Indented Pad\n";
#endif
  }
  if (moderngekko::frontend::ControllerConfigExists(directory))
    return 37;

  // A UTF-8 BOM on the first line is skipped before header detection, and
  // Device values have surrounding quotes stripped (IniFile::StripQuotes).
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc | std::ios::binary);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "\xEF\xBB\xBF[GCPad1]\nDevice = \"SDL/6/Quoted Pad\"\n";
#else
    output << "\xEF\xBB\xBF[Wiimote1]\nDevice = \"SDL/6/Quoted Pad\"\n";
#endif
  }
  if (moderngekko::frontend::ReadConfiguredController(directory) !=
      "SDL/6/Quoted Pad")
    return 38;

  // CRLF line endings: std::getline leaves '\r' on POSIX builds, while a
  // text-mode stream strips it on Windows. Either way the merge must still
  // recognise the section header, drop the empty stale Device, and bind.
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc | std::ios::binary);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad1]\r\nDevice = \r\nButtons/A = Custom\r\n";
#else
    output << "[Wiimote1]\r\nDevice = \r\nButtons/1 = Custom\r\n";
#endif
  }
  if (moderngekko::frontend::ControllerConfigExists(directory))
    return 39;
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, "SDL/0/CRLF Pad", &error))
    return 40;
  if (moderngekko::frontend::ReadConfiguredController(directory) !=
      "SDL/0/CRLF Pad")
    return 41;
  merged.clear();
  {
    std::ifstream merged_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    merged.assign(std::istreambuf_iterator<char>(merged_input),
                  std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const bool crlf_ok = merged.contains("Device = SDL/0/CRLF Pad\n") &&
                       merged.contains("Buttons/A = Custom");
#else
  const bool crlf_ok = merged.contains("Device = SDL/0/CRLF Pad\n") &&
                       merged.contains("Buttons/1 = Custom");
#endif
  if (!crlf_ok)
    return 42;

  // A duplicate section header must not get a second Device insert, and the
  // stale Device under the repeat must be dropped (the sections merge into
  // one in IniFile, so the last surviving Device wins there).
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad1]\nButtons/A = Custom\n[GCPad2]\n"
              "[GCPad1]\nDevice = \nButtons/B = Y\n";
#else
    output << "[Wiimote1]\nButtons/1 = Custom\n[Wiimote2]\n"
              "[Wiimote1]\nDevice = \nButtons/2 = Y\n";
#endif
  }
  const std::string two_pads[] = {"SDL/0/First Pad", "SDL/1/Second Pad"};
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, two_pads, &error))
    return 43;
  merged.clear();
  {
    std::ifstream merged_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    merged.assign(std::istreambuf_iterator<char>(merged_input),
                  std::istreambuf_iterator<char>());
  }
  // Exactly one Device line may exist for pad 1: the merged binding.
  {
    std::size_t hits = 0;
    std::size_t pos = 0;
    while ((pos = merged.find("Device = ", pos)) != std::string::npos) {
      ++hits;
      ++pos;
    }
    if (hits != 2) // one per bound pad section
      return 44;
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const bool dup_ok =
      merged.contains("[GCPad1]\nDevice = SDL/0/First Pad\n") &&
      merged.contains("[GCPad2]\nDevice = SDL/1/Second Pad\n") &&
      merged.contains("Buttons/B = Y\n");
#else
  const bool dup_ok =
      merged.contains("[Wiimote1]\nDevice = SDL/0/First Pad\n") &&
      merged.contains("[Wiimote2]\nDevice = SDL/1/Second Pad\n") &&
      merged.contains("Buttons/2 = Y\n");
#endif
  if (!dup_ok ||
      moderngekko::frontend::ReadConfiguredController(directory) !=
          "SDL/0/First Pad")
    return 45;

  // The launcher remap screen round-trips pad sections through
  // ReadControllerProfile/SaveControllerProfile without touching the
  // expression text, including quoted Device values and empty sections.
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
    output << "[GCPad1]\nDevice = Pipe/0/pad0\nButtons/A = `Button A`\n"
              "Buttons/Z = `Button Z`\nMain Stick/Up = `Axis MAIN Y +`\n"
              "[GCPad2]\nDevice = \"XInput/1/Gamepad\"\nButtons/B = Custom\n"
              "[GCPad4]\n";
#else
    output << "[Wiimote1]\nDevice = Pipe/0/pad0\nButtons/A = `Button A`\n"
              "Buttons/Z = `Button Z`\nMain Stick/Up = `Axis MAIN Y +`\n"
              "[Wiimote2]\nDevice = \"XInput/1/Gamepad\"\nButtons/B = Custom\n"
              "[Wiimote4]\n";
#endif
  }
  auto pad_ports = moderngekko::frontend::ReadControllerProfile(directory);
  if (pad_ports[0].device != "Pipe/0/pad0" ||
      pad_ports[1].device != "XInput/1/Gamepad" ||
      !pad_ports[2].device.empty() || !pad_ports[3].device.empty() ||
      pad_ports[0].controls.size() != 3 ||
      pad_ports[0].controls.front().first != "Buttons/A" ||
      pad_ports[0].controls.front().second != "`Button A`" ||
      pad_ports[0].controls.back().second != "`Axis MAIN Y +`" ||
      !pad_ports[3].controls.empty())
    return 46;

  // A remap + unbind + late-bind round-trip: edit port 1's Buttons/A, clear
  // port 2, bind port 4 to keyboard defaults, and check the file and the
  // reader agree.
  pad_ports[0].controls.front().second = "X";
  pad_ports[1].device.clear();
  pad_ports[1].controls.clear();
  pad_ports[3].device = "DInput/0/Keyboard Mouse";
  pad_ports[3].controls =
      moderngekko::frontend::DefaultPadControls(pad_ports[3].device);
  if (!moderngekko::frontend::SaveControllerProfile(directory, pad_ports,
                                                    &error))
    return 47;
  const auto reloaded =
      moderngekko::frontend::ReadControllerProfile(directory);
  if (reloaded[0].device != "Pipe/0/pad0" ||
      reloaded[0].controls.front().first != "Buttons/A" ||
      reloaded[0].controls.front().second != "X" ||
      !reloaded[1].device.empty() || !reloaded[2].device.empty() ||
      reloaded[3].device != "DInput/0/Keyboard Mouse" ||
      reloaded[3].controls.empty())
    return 48;
  merged.clear();
  {
    std::ifstream saved_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
    merged.assign(std::istreambuf_iterator<char>(saved_input),
                  std::istreambuf_iterator<char>());
  }
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const bool saved_ok =
      merged.contains("[GCPad1]\nDevice = Pipe/0/pad0\nButtons/A = X\n") &&
      merged.contains("[GCPad4]\nDevice = DInput/0/Keyboard Mouse\n") &&
      merged.contains("[GCPad2]\n[GCPad3]\n") &&
      merged.contains("Buttons/Z = `Button Z`\n") &&
      merged.contains("Main Stick/Up = UP\n");
#else
  const bool saved_ok =
      merged.contains("[Wiimote1]\nDevice = Pipe/0/pad0\nButtons/A = X\n") &&
      merged.contains("[Wiimote4]\nDevice = DInput/0/Keyboard Mouse\n") &&
      merged.contains("[Wiimote2]\n[Wiimote3]\n") &&
      merged.contains("Buttons/Z = `Button Z`\n");
#endif
  if (!saved_ok)
    return 49;

  // Validation: >4 ports and ini-corrupting values are refused.
  std::vector<moderngekko::frontend::PadPortSettings> five_ports(5);
  if (moderngekko::frontend::SaveControllerProfile(directory, five_ports,
                                                   &error))
    return 50;
  auto bad_ports = pad_ports;
  bad_ports[0].device = "XInput/0/Game\npad";
  if (moderngekko::frontend::SaveControllerProfile(directory, bad_ports,
                                                   &error))
    return 51;
  bad_ports = pad_ports;
  bad_ports[0].controls.emplace_back("Buttons/A", "`Button A`\nDevice = x");
  if (moderngekko::frontend::SaveControllerProfile(directory, bad_ports,
                                                   &error))
    return 52;

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  // The generated defaults are the same table the UI offers as "Reset":
  // keyboard and Pipe get their own bodies, everything else the pad body.
  const auto find_default = [](const auto &controls, std::string_view key) {
    for (const auto &[k, v] : controls)
      if (k == key)
        return v;
    return std::string{};
  };
  const auto pad_defaults =
      moderngekko::frontend::DefaultPadControls("XInput/0/Gamepad");
  if (find_default(pad_defaults, "Buttons/A") != "`Button A`" ||
      find_default(pad_defaults, "Buttons/Z") != "`Shoulder R`" ||
      find_default(pad_defaults, "Main Stick/Up") != "`Left Y+`" ||
      find_default(pad_defaults, "C-Stick/Up") != "`Right Y-`" ||
      find_default(pad_defaults, "C-Stick/Down") != "`Right Y+`" ||
      find_default(pad_defaults, "Rumble/Motor").empty())
    return 53;
  const auto sdl_defaults =
      moderngekko::frontend::DefaultPadControls("SDL/0/Gamepad");
  if (find_default(sdl_defaults, "C-Stick/Up") != "`Right Y-`" ||
      find_default(sdl_defaults, "C-Stick/Down") != "`Right Y+`")
    return 56;
  const auto keyboard_defaults =
      moderngekko::frontend::DefaultPadControls("DInput/0/Keyboard Mouse");
  if (keyboard_defaults.empty() ||
      keyboard_defaults.front().first != "Buttons/A" ||
      keyboard_defaults.front().second != "X")
    return 54;
  const auto pipe_defaults =
      moderngekko::frontend::DefaultPadControls("Pipe/0/pad0");
  if (find_default(pipe_defaults, "Buttons/Z") != "`Button Z`" ||
      find_default(pipe_defaults, "Main Stick/Up") != "`Axis MAIN Y +`" ||
      find_default(pipe_defaults, "D-Pad/Up") != "`Button D_UP`")
    return 55;
#endif

  // --- [Video] window_mode / aa ------------------------------------------
  using moderngekko::frontend::AntiAliasing;
  using moderngekko::frontend::WindowMode;
  if (moderngekko::frontend::ParseWindowMode("windowed") !=
          WindowMode::Windowed ||
      moderngekko::frontend::ParseWindowMode(" Maximized ") !=
          WindowMode::Maximized ||
      moderngekko::frontend::ParseWindowMode("FULLSCREEN") !=
          WindowMode::Fullscreen ||
      moderngekko::frontend::ParseWindowMode("borderless") !=
          WindowMode::Fullscreen ||
      moderngekko::frontend::ParseWindowMode("bogus").has_value() ||
      moderngekko::frontend::WindowModeToString(WindowMode::Maximized) !=
          "maximized")
    return 56;

  if (moderngekko::frontend::ParseAntiAliasing("off") != AntiAliasing::Off ||
      moderngekko::frontend::ParseAntiAliasing(" FXAA ") !=
          AntiAliasing::Fxaa ||
      moderngekko::frontend::ParseAntiAliasing("2x") != AntiAliasing::Msaa2x ||
      moderngekko::frontend::ParseAntiAliasing("msaa 4x") !=
          AntiAliasing::Msaa4x ||
      moderngekko::frontend::ParseAntiAliasing("8x") != AntiAliasing::Msaa8x ||
      moderngekko::frontend::ParseAntiAliasing("16x").has_value() ||
      moderngekko::frontend::ParseAntiAliasing("msaa").has_value() ||
      moderngekko::frontend::AntiAliasingToString(AntiAliasing::Msaa4x) != "4x")
    return 57;

  // Precedence: fullscreen defaults true (shipped borderless), the legacy
  // bool maps to Windowed/Fullscreen, and an explicit window_mode wins over
  // the bool in either direction.
  {
    moderngekko::frontend::ConfigResult probe;
    if (probe.EffectiveWindowMode() != WindowMode::Fullscreen)
      return 58;
    probe.fullscreen = false;
    if (probe.EffectiveWindowMode() != WindowMode::Windowed)
      return 58;
    probe.window_mode = WindowMode::Maximized;
    if (probe.EffectiveWindowMode() != WindowMode::Maximized)
      return 58;
    probe.fullscreen = true;
    if (probe.EffectiveWindowMode() != WindowMode::Maximized)
      return 58;
  }

  // window_mode overrides fullscreen= at load; aa= parses from [Video].
  {
    std::ofstream raw(directory / "config.ini", std::ios::trunc);
    raw << "[Video]\nresolution=1920x1080\nbackend=Vulkan\n"
           "fullscreen=false\nwindow_mode=maximized\naa=msaa4x\n"
           "[Netplay]\nnickname=P\naddress=127.0.0.1\nport=2626\nbuffer=auto\n";
  }
  const auto wm_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!wm_loaded || wm_loaded.window_mode != WindowMode::Maximized ||
      wm_loaded.aa != AntiAliasing::Msaa4x ||
      wm_loaded.EffectiveWindowMode() != WindowMode::Maximized)
    return 59;

  // A legacy config with only fullscreen=false still opens windowed, and the
  // new keys stay unset when absent.
  {
    std::ofstream raw(directory / "config.ini", std::ios::trunc);
    raw << "[Video]\nresolution=1920x1080\nbackend=Vulkan\nfullscreen=false\n"
           "[Netplay]\nnickname=P\naddress=127.0.0.1\nport=2626\nbuffer=auto\n";
  }
  const auto legacy_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!legacy_loaded || legacy_loaded.window_mode ||
      legacy_loaded.EffectiveWindowMode() != WindowMode::Windowed ||
      legacy_loaded.aa)
    return 60;

  // Persistence: both keys are written, and fullscreen= mirrors the
  // effective mode rather than a stale bool.
  moderngekko::frontend::ConfigResult wm_config = loaded;
  wm_config.window_mode = WindowMode::Maximized;
  wm_config.fullscreen = true;
  wm_config.aa = AntiAliasing::Fxaa;
  if (!moderngekko::frontend::SaveConfig(directory, wm_config, &error))
    return 61;
  {
    std::ifstream config_input(directory / "config.ini");
    serialized.assign(std::istreambuf_iterator<char>(config_input),
                      std::istreambuf_iterator<char>());
  }
  if (!serialized.contains("window_mode=maximized\n") ||
      !serialized.contains("aa=fxaa\n") ||
      !serialized.contains("fullscreen=false\n"))
    return 62;
  const auto wm_reloaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!wm_reloaded ||
      wm_reloaded.EffectiveWindowMode() != WindowMode::Maximized ||
      wm_reloaded.aa != AntiAliasing::Fxaa)
    return 63;

  write_bad("window_mode=fullscrn");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 64;
  write_bad("aa=16x");
  if (moderngekko::frontend::LoadConfig(directory, false))
    return 64;

  invalid_netplay = netplay_config;
  invalid_netplay.window_mode = static_cast<WindowMode>(99);
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 65;
  invalid_netplay = netplay_config;
  invalid_netplay.aa = static_cast<AntiAliasing>(99);
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 65;

  // The launcher-facing option tables: MSAA entries carry the lens-flare
  // caveat, FXAA and Off do not.
  {
    int msaa_caveat_labels = 0;
    bool saw_fxaa = false;
    for (const auto &option :
         moderngekko::frontend::SupportedAntiAliasingOptions()) {
      if (option.value == AntiAliasing::Fxaa)
        saw_fxaa = true;
      if ((option.value == AntiAliasing::Msaa2x ||
           option.value == AntiAliasing::Msaa4x ||
           option.value == AntiAliasing::Msaa8x) &&
          std::string_view(option.text).contains("lens-flare"))
        ++msaa_caveat_labels;
    }
    if (!saw_fxaa || msaa_caveat_labels != 3 ||
        moderngekko::frontend::SupportedWindowModes().size() != 3)
      return 66;
  }

  fs::remove_all(directory);
  return 0;
}
