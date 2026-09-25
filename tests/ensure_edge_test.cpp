// Regression supplement to tests/frontend_config_test.cpp.
// Covers shipped-file edge cases the main suite doesn't exercise:
//   1. The release's EMPTY GCPadNew.ini through EnsureControllerConfig
//      (must yield a fully mapped binding, not a dead section).
//   2. A bindings-less [GCPad1] section through the merge path
//      (must get the standard mapping body spliced in — a bound Device
//      with zero controls is a "connected but dead" pad).
//   2b. A pre-bound Device section counts as configured and must be left
//      untouched (never clobber a user-bound profile).
//   2c. Sections with existing control lines keep them verbatim (splice
//      only applies to zero-control sections).
//   2d. A UTF-8 BOM must not hide the first section header from the merge
//      (a BOM'd bindings-less [GCPad1] still gets bound and spliced).
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "frontend_config.hpp"

namespace fs = std::filesystem;

static std::string ReadAll(const fs::path& p) {
  std::ifstream in(p);
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "mgk_a4_edge";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "Config", ec);

  const fs::path ini = dir / "Config" / "GCPadNew.ini";
  std::string error;

  // Case 1: the shipped 0-byte file. EnsureControllerConfig must bind the
  // selected pad AND emit the standard mapping body for it.
  { std::ofstream(ini, std::ios::trunc); }
  if (!moderngekko::frontend::EnsureControllerConfig(dir, "SDL/0/Xbox Pad",
                                                     &error)) {
    std::cerr << "case1 ensure failed: " << error << '\n';
    return 1;
  }
  const std::string out1 = ReadAll(ini);
  if (!out1.contains("[GCPad1]\nDevice = SDL/0/Xbox Pad\n") ||
      !out1.contains("Buttons/A = `Button A`\n") ||
      !out1.contains("Main Stick/Up = `Left Y+`\n") ||
      !out1.contains("Rumble/Motor = `Motor L` | `Motor R`\n")) {
    std::cerr << "case1: empty file did not produce full profile:\n" << out1;
    return 2;
  }
  std::cout << "case1 (empty file -> full profile) PASS\n";

  // Case 2: a pad section that exists but has no control expressions at all.
  // The merge binds Device in place and must now splice the standard mapping
  // body in too — otherwise Dolphin loads a bound pad whose every expression
  // is empty ("connected but dead" pad). Regression-checked, not just
  // documented.
  { std::ofstream(ini, std::ios::trunc) << "[GCPad1]\n"; }
  if (!moderngekko::frontend::EnsureControllerConfig(dir, "SDL/0/Merged Pad",
                                                     &error)) {
    std::cerr << "case2 ensure failed: " << error << '\n';
    return 3;
  }
  const std::string out2 = ReadAll(ini);
  const bool bound = out2.contains("Device = SDL/0/Merged Pad");
  const bool has_mapping = out2.contains("Buttons/A =");
  std::cout << "case2 (bindings-less section): bound=" << bound
            << " mapping_body_present=" << has_mapping;
  if (bound && !has_mapping) {
    std::cout << "  <-- DEAD-PAD EDGE: bound but unmapped\n" << out2;
    return 4;
  }
  std::cout << " PASS\n";

  // Case 2b: a section that already binds a Device counts as "configured"
  // (ControllerConfigExists), so Ensure leaves it untouched — a user-bound
  // profile is never rewritten, even a mappings-less one.
  { std::ofstream(ini, std::ios::trunc)
        << "[GCPad1]\nDevice = SDL/0/Old Pad\n"; }
  if (!moderngekko::frontend::EnsureControllerConfig(dir, "SDL/0/Merged Pad",
                                                     &error)) {
    std::cerr << "case2b ensure failed: " << error << '\n';
    return 5;
  }
  const std::string out2b = ReadAll(ini);
  if (out2b != "[GCPad1]\nDevice = SDL/0/Old Pad\n") {
    std::cerr << "case2b: user-bound profile was rewritten:\n" << out2b;
    return 6;
  }
  std::cout << "case2b (pre-bound profile untouched) PASS\n";

  // Case 2c: hand-edited mappings in the section must survive the merge —
  // the body splice only applies to sections with zero control lines.
  { std::ofstream(ini, std::ios::trunc)
        << "[GCPad1]\nButtons/A = `Button X`\n"; }
  if (!moderngekko::frontend::EnsureControllerConfig(dir, "SDL/0/Merged Pad",
                                                     &error)) {
    std::cerr << "case2c ensure failed: " << error << '\n';
    return 7;
  }
  const std::string out2c = ReadAll(ini);
  if (!out2c.contains("Buttons/A = `Button X`") ||
      out2c.contains("Buttons/A = `Button A`")) {
    std::cerr << "case2c: hand-edited mapping was clobbered:\n" << out2c;
    return 8;
  }
  if (!out2c.contains("Device = SDL/0/Merged Pad")) {
    std::cerr << "case2c: controls-only section was never bound:\n" << out2c;
    return 8;
  }
  std::cout << "case2c (existing controls preserved) PASS\n";

  // Case 2d: a UTF-8 BOM at the start of the file must not hide the first
  // section header from the merge — a BOM'd bindings-less [GCPad1] still gets
  // its mapping body spliced (the BOM itself stays in the output).
  {
    std::ofstream out(ini, std::ios::trunc | std::ios::binary);
    out << "\xEF\xBB\xBF[GCPad1]\n";
  }
  if (!moderngekko::frontend::EnsureControllerConfig(dir, "SDL/0/Merged Pad",
                                                     &error)) {
    std::cerr << "case2d ensure failed: " << error << '\n';
    return 9;
  }
  const std::string out2d = ReadAll(ini);
  {
    const auto pad1 = out2d.find("\xEF\xBB\xBF[GCPad1]\n");
    const auto device = out2d.find("Device = SDL/0/Merged Pad\n");
    const auto button_a = out2d.find("Buttons/A = `Button A`\n");
    const auto pad2 = out2d.find("[GCPad2]");
    if (pad1 != 0 || device == std::string::npos ||
        button_a == std::string::npos || pad2 == std::string::npos ||
        !(pad1 < device && device < button_a && button_a < pad2)) {
      std::cerr << "case2d: BOM'd bare section not bound+spliced correctly:\n"
                << out2d;
      return 10;
    }
  }
  std::cout << "case2d (BOM-prefixed bare section bound+spliced) PASS\n";

  fs::remove_all(dir, ec);
  return 0;
}
