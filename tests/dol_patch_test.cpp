#include "dol_patch.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {
void WriteBE32(std::uint8_t *data, std::uint32_t value) {
  data[0] = static_cast<std::uint8_t>(value >> 24);
  data[1] = static_cast<std::uint8_t>(value >> 16);
  data[2] = static_cast<std::uint8_t>(value >> 8);
  data[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadBE32(const std::uint8_t *data) {
  return (std::uint32_t{data[0]} << 24) | (std::uint32_t{data[1]} << 16) |
         (std::uint32_t{data[2]} << 8) | data[3];
}

bool Write(const fs::path &path, const auto &bytes) {
  std::ofstream output(path, std::ios::binary);
  return output && output.write(reinterpret_cast<const char *>(bytes.data()),
                                bytes.size());
}
} // namespace

int main() {
  const fs::path root =
      fs::temp_directory_path() /
      ("moderngekko-dol-patch-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  const fs::path dol = root / "main.dol";
  const fs::path manifest = root / "patches.csv";
  std::array<std::uint8_t, 0x140> bytes{};
  WriteBE32(bytes.data() + 0x00, 0x100);
  WriteBE32(bytes.data() + 0x48, 0x80004000);
  WriteBE32(bytes.data() + 0x90, 0x40);
  WriteBE32(bytes.data() + 0x100, 0x4092000C);
  WriteBE32(bytes.data() + 0x104, 0x7C00F850);
  if (!Write(dol, bytes))
    return 1;
  {
    std::ofstream output(manifest);
    output << "address,expected,replacement\n"
              "80004000,4092000C,4800000C\n"
              "80004004,7C00F850,7C000050\n";
  }

  bool changed = false;
  std::string error;
  if (!moderngekko::frontend::ApplyDolPatchManifest(dol, manifest, &changed,
                                                    &error) ||
      !changed) {
    std::cerr << error << '\n';
    return 1;
  }
  // Scoped: Windows will not delete a file that is still open, so an ifstream
  // left open here makes the remove_all below throw on that platform only.
  {
    std::ifstream input(dol, std::ios::binary);
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  }
  if (ReadBE32(bytes.data() + 0x100) != 0x4800000C ||
      ReadBE32(bytes.data() + 0x104) != 0x7C000050)
    return 1;
  if (!moderngekko::frontend::ApplyDolPatchManifest(dol, manifest, &changed,
                                                    &error) ||
      changed)
    return 1;

  WriteBE32(bytes.data() + 0x100, 0xDEADBEEF);
  if (!Write(dol, bytes))
    return 1;
  if (moderngekko::frontend::ApplyDolPatchManifest(dol, manifest, &changed,
                                                   &error) ||
      error.find("0x80004000") == std::string::npos)
    return 1;

  // A patch may touch the last word of a section: address+4 == section end is
  // inside the mapped window, not past it.
  {
    std::ofstream output(manifest);
    output << "address,expected,replacement\n8000403C,00000000,60000000\n";
  }
  if (!moderngekko::frontend::ApplyDolPatchManifest(dol, manifest, &changed,
                                                    &error) ||
      !changed) {
    std::cerr << error << '\n';
    return 50;
  }
  {
    std::ifstream input(dol, std::ios::binary);
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  }
  if (ReadBE32(bytes.data() + 0x13C) != 0x60000000)
    return 51;

  // Manifest reject cases. Binary mode so CRLF/BOM survive on every host.
  const auto write_manifest = [&](const char *text) {
    std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
    output << text;
  };
  const auto expect_reject = [&](const fs::path &target, const char *needle) {
    bool sub_changed = true; // the call must clear it
    std::string sub_error;
    if (moderngekko::frontend::ApplyDolPatchManifest(target, manifest,
                                                     &sub_changed, &sub_error) ||
        sub_changed ||
        sub_error.find(needle) == std::string::npos) {
      std::cerr << "expected rejection containing \"" << needle << "\", got \""
                << sub_error << "\"\n";
      return false;
    }
    return true;
  };

  // Ordering: strictly increasing 4-aligned addresses are required, so two
  // patches can never overlap. Descending and duplicate rows both reject.
  write_manifest("address,expected,replacement\n"
                 "80004004,7C000050,7C00F850\n"
                 "80004000,4092000C,4800000C\n");
  if (!expect_reject(dol, "line 3"))
    return 52;
  write_manifest("address,expected,replacement\n"
                 "80004000,4092000C,4800000C\n"
                 "80004000,4092000C,48000010\n");
  if (!expect_reject(dol, "line 3"))
    return 53;
  write_manifest("address,expected,replacement\n80004002,4092000C,4800000C\n");
  if (!expect_reject(dol, "line 2"))
    return 54;
  write_manifest("address,expected,replacement\n80004000,4092000C,4092000C\n");
  if (!expect_reject(dol, "line 2"))
    return 55;
  write_manifest("address,expected,replacement\n80004000,4092000C\n");
  if (!expect_reject(dol, "line 2"))
    return 56;
  write_manifest("address,expected,replacement\n80004000,4092000C,4800000C,00\n");
  if (!expect_reject(dol, "line 2"))
    return 57;
  write_manifest("address,expected,replacement\n8000400Z,4092000C,4800000C\n");
  if (!expect_reject(dol, "line 2"))
    return 58;
  write_manifest("address,expected,replacement\n");
  if (!expect_reject(dol, "no patches"))
    return 59;
  write_manifest("addr,expected,replacement\n80004000,4092000C,4800000C\n");
  if (!expect_reject(dol, "manifest header"))
    return 60;
  // A trailing newline is fine; a blank line is a malformed row, not ignored.
  write_manifest("address,expected,replacement\n80004004,7C000050,7C00F850\n\n");
  if (!expect_reject(dol, "line 3"))
    return 61;
  // Unmapped patch addresses reject before the expected-value check.
  write_manifest("address,expected,replacement\n80003000,00000000,60000000\n");
  if (!expect_reject(dol, "outside executable code"))
    return 62;
  // 0xFFFFFFFC + 4 must not wrap back into the section window (u64 math).
  write_manifest("address,expected,replacement\nFFFFFFFC,00000000,60000000\n");
  if (!expect_reject(dol, "outside executable code"))
    return 63;

  // Tolerated input: a UTF-8 BOM on the header line and CRLF endings. The
  // patch is already applied, so this must succeed with changed == false.
  write_manifest("\xEF\xBB\xBF"
                 "address,expected,replacement\n80004004,7C00F850,7C000050\n");
  if (!moderngekko::frontend::ApplyDolPatchManifest(dol, manifest, &changed,
                                                    &error) ||
      changed)
    return 64;
  write_manifest("address,expected,replacement\r\n80004004,7C00F850,7C000050\r\n");
  if (!moderngekko::frontend::ApplyDolPatchManifest(dol, manifest, &changed,
                                                    &error) ||
      changed)
    return 65;

  // A patch whose four bytes straddle the section end is unmapped, even
  // though its start address is inside the section.
  {
    std::array<std::uint8_t, 0x140> straddle{};
    WriteBE32(straddle.data() + 0x00, 0x100);
    WriteBE32(straddle.data() + 0x48, 0x80004000);
    WriteBE32(straddle.data() + 0x90, 0x3E); // size not a multiple of 4
    const fs::path straddle_dol = root / "straddle.dol";
    if (!Write(straddle_dol, straddle))
      return 66;
    write_manifest("address,expected,replacement\n8000403C,00000000,60000000\n");
    if (!expect_reject(straddle_dol, "outside executable code"))
      return 66;
  }

  // Two text sections covering the same address window make the patch
  // ambiguous: it must refuse rather than guess an offset.
  {
    std::array<std::uint8_t, 0x200> ambiguous{};
    WriteBE32(ambiguous.data() + 0x00, 0x100);
    WriteBE32(ambiguous.data() + 0x48, 0x80004000);
    WriteBE32(ambiguous.data() + 0x90, 0x40);
    WriteBE32(ambiguous.data() + 0x04, 0x140);
    WriteBE32(ambiguous.data() + 0x4C, 0x80004000);
    WriteBE32(ambiguous.data() + 0x94, 0x40);
    const fs::path ambiguous_dol = root / "ambiguous.dol";
    if (!Write(ambiguous_dol, ambiguous))
      return 67;
    write_manifest("address,expected,replacement\n80004000,00000000,60000000\n");
    if (!expect_reject(ambiguous_dol, "ambiguously mapped"))
      return 67;
  }

  // Section table sanity: an offset inside the 0x100 header or a section
  // running past EOF rejects as a malformed DOL before any patch is tried.
  {
    std::array<std::uint8_t, 0x140> low_offset{};
    WriteBE32(low_offset.data() + 0x00, 0x80);
    WriteBE32(low_offset.data() + 0x48, 0x80004000);
    WriteBE32(low_offset.data() + 0x90, 0x40);
    const fs::path low_dol = root / "low_offset.dol";
    if (!Write(low_dol, low_offset))
      return 68;
    write_manifest("address,expected,replacement\n80004000,00000000,60000000\n");
    if (!expect_reject(low_dol, "outside the file"))
      return 68;
  }
  {
    std::array<std::uint8_t, 0x140> over_eof{};
    WriteBE32(over_eof.data() + 0x00, 0x100);
    WriteBE32(over_eof.data() + 0x48, 0x80004000);
    WriteBE32(over_eof.data() + 0x90, 0x100); // 0x100 + 0x100 > file size
    const fs::path over_dol = root / "over_eof.dol";
    if (!Write(over_dol, over_eof))
      return 69;
    write_manifest("address,expected,replacement\n80004000,00000000,60000000\n");
    if (!expect_reject(over_dol, "outside the file"))
      return 69;
  }
  {
    const std::array<std::uint8_t, 0x80> tiny{};
    const fs::path tiny_dol = root / "tiny.dol";
    if (!Write(tiny_dol, tiny))
      return 70;
    write_manifest("address,expected,replacement\n80004000,00000000,60000000\n");
    if (!expect_reject(tiny_dol, "header is truncated"))
      return 70;
  }

  fs::remove_all(root);
  return 0;
}
