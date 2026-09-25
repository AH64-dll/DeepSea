// Regression harness for src/runner/game_setup.cpp — exercises the probe and
// the whole PromptForValidGameRoot loop with scripted UI hooks, no desktop
// needed.
//
// Exit code = number of failed checks (0 = all green).

#include "runner/game_setup.hpp"

#include "moderngekko/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using moderngekko::runner::FolderPick;
using moderngekko::runner::GameProbeResult;
using moderngekko::runner::GameSetupRequest;
using moderngekko::runner::GameSetupUi;
using moderngekko::runner::SetupChoice;
using moderngekko::runner::SetupDialog;

namespace {

int g_failures = 0;
int g_checks = 0;

std::string U8(const std::u8string& s)
{
  return {reinterpret_cast<const char*>(s.data()), s.size()};
}

void Check(bool cond, const std::string& label)
{
  ++g_checks;
  if (!cond)
  {
    ++g_failures;
    std::cout << "  FAIL: " << label << '\n';
  }
}

void WriteFile(const fs::path& path, const void* data, std::size_t size)
{
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

bool Contains(const std::vector<std::string>& lines, const std::string& needle)
{
  return std::ranges::any_of(lines, [&](const std::string& line) {
    return line.find(needle) != std::string::npos;
  });
}

// Extended-length (\\?\) spelling of an absolute path, used to create and
// probe fixtures whose natural form exceeds MAX_PATH. Empty input stays empty.
fs::path ExtendedForm(const fs::path& path)
{
#if defined(_WIN32)
  if (path.empty() || !path.is_absolute())
    return path;
  const std::wstring native =
      fs::path(path).make_preferred().lexically_normal().native();
  if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\"))
    return path;
  if (native.starts_with(L"\\\\"))
    return fs::path(L"\\\\?\\UNC\\" + native.substr(2));
  return fs::path(L"\\\\?\\" + native);
#else
  return path;
#endif
}

// Minimal boot.bin: 0x60 bytes, disc id at 0, GC magic at 0x1c, name at 0x20.
void WriteBootBin(const fs::path& sys, const std::string& id, bool good_magic = true,
                  std::size_t size = 0x60)
{
  std::array<unsigned char, 0x60> boot{};
  std::copy_n(id.c_str(), std::min<std::size_t>(id.size(), 6), boot.begin());
  if (good_magic)
  {
    boot[0x1c] = 0xc2; boot[0x1d] = 0x33; boot[0x1e] = 0x9f; boot[0x1f] = 0x3d;
  }
  const char name[] = "THE LEGEND OF ZELDA The Wind Waker";
  std::copy_n(name, sizeof(name) - 1, boot.begin() + 0x20);
  WriteFile(sys / "boot.bin", boot.data(), std::min(size, boot.size()));
}

// Minimal main.dol: 0x100+ bytes, text section 0 at file offset 0x100,
// address 0x80003100, size 4, entry 0x80003100.
void WriteMainDol(const fs::path& sys, std::size_t size = 0x200, bool bad_entry = false,
                  bool bad_table = false)
{
  std::vector<unsigned char> dol(size, 0);
  dol[0x02] = 0x01;  // text[0] file offset = 0x100
  dol[0x48] = 0x80; dol[0x49] = 0x00; dol[0x4a] = 0x31; dol[0x4b] = 0x00;
  dol[0x93] = 0x04;  // text[0] size = 4
  if (bad_table)
    dol[0x92] = 0xff;  // size 0x0000ff04: section runs past EOF
  dol[0xe0] = 0x80; dol[0xe1] = 0x00; dol[0xe2] = bad_entry ? 0x50 : 0x31; dol[0xe3] = 0x00;
  WriteFile(sys / "main.dol", dol.data(), dol.size());
}

// Builds a synthetically-valid extracted-disc root.
void MakeGameTree(const fs::path& root, const std::string& id, bool main_rel = true)
{
  fs::create_directories(root / "sys");
  fs::create_directories(root / "files");
  WriteBootBin(root / "sys", id);
  WriteMainDol(root / "sys");
  if (main_rel)
    WriteFile(root / "files" / "_Main.rel", "rel", 3);
}

void Show(const GameProbeResult& probe, const fs::path& path)
{
  std::cout << "  probe(" << U8(path.u8string()) << ") ok=" << probe.ok
            << " id=" << (probe.disc_id.empty() ? "-" : probe.disc_id) << '\n';
  for (const auto& p : probe.problems)
    std::cout << "      - " << p << '\n';
}

// Scripted UI: queued dialog answers and queued picker results; records what
// the user would have seen.
struct ScriptedUi
{
  std::deque<SetupChoice> dialog_answers;
  std::deque<FolderPick> picks;
  std::vector<std::string> transcript;

  GameSetupUi Hooks()
  {
    GameSetupUi ui;
    ui.show_dialog = [this](const SetupDialog& d) {
      transcript.push_back("[dialog] " + d.heading);
      for (const auto& line : SplitLines(d.details))
        transcript.push_back("         | " + line);
      if (dialog_answers.empty())
      {
        transcript.push_back("         -> (script exhausted: Cancel)");
        return SetupChoice::Cancel;
      }
      const SetupChoice c = dialog_answers.front();
      dialog_answers.pop_front();
      transcript.push_back(std::string("         -> ") + ChoiceName(c));
      return c;
    };
    ui.pick_folder = [this](std::string_view title) {
      transcript.push_back("[picker] " + std::string(title));
      if (picks.empty())
        return FolderPick{FolderPick::Outcome::Cancelled, {}};
      FolderPick p = picks.front();
      picks.pop_front();
      transcript.push_back("         -> pick: " + U8(p.path.u8string()));
      return p;
    };
    return ui;
  }

  static std::vector<std::string> SplitLines(const std::string& s)
  {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
      if (c == '\n') { out.push_back(cur); cur.clear(); }
      else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
  }
  static const char* ChoiceName(SetupChoice c)
  {
    switch (c)
    {
    case SetupChoice::Primary: return "Primary";
    case SetupChoice::Secondary: return "Secondary";
    case SetupChoice::Cancel: return "Cancel";
    }
    return "?";
  }
};

}  // namespace

int main()
{
  const fs::path base = fs::temp_directory_path() / "agent3-game-setup-test";
  fs::remove_all(base);
  fs::create_directories(base);

  // Fake release dir beside the "executable": one bundled module lets the
  // probe infer the required disc id (branded-build behavior).
  const fs::path exe_dir = base / "release";
  fs::create_directories(exe_dir);
  WriteFile(exe_dir / "gGZLE01_recomp.dll", "x", 1);

  const fs::path user_dir = base / "user-dir";

  std::cout << "== RequiredDiscId ==\n";
  const auto req = moderngekko::runner::RequiredDiscId(exe_dir);
  Check(req.has_value() && *req == "GZLE01", "infer GZLE01 from bundled module");
  const fs::path empty_dir = base / "empty-release";
  fs::create_directories(empty_dir);
  Check(!moderngekko::runner::RequiredDiscId(empty_dir).has_value(),
        "permissive when no module bundled");

  std::cout << "== Probe matrix ==\n";
  struct Case { std::string name; fs::path path; bool expect_ok; };
  const fs::path good = base / "games" / "Wind Waker (USA)";
  MakeGameTree(good, "GZLE01");
  const fs::path unicode = base / "ゲーム" / "José—ディスク";
  MakeGameTree(unicode, "GZLE01");
  const fs::path no_dol = base / "broken-no-dol";
  MakeGameTree(no_dol, "GZLE01");
  fs::remove(no_dol / "sys" / "main.dol");
  const fs::path no_files = base / "broken-no-files";
  fs::create_directories(no_files / "sys");
  WriteBootBin(no_files / "sys", "GZLE01");
  WriteMainDol(no_files / "sys");
  const fs::path no_rel = base / "no-rel-optional";
  MakeGameTree(no_rel, "GZLE01", false);
  // _Main.rel is optional, but files/ still needs some content — an empty
  // files/ is its own rejection (a dump with no assets can never boot).
  WriteFile(no_rel / "files" / "other.bin", "x", 1);
  const fs::path pal = base / "pal-disc";
  MakeGameTree(pal, "GZLP01");
  const fs::path bad_magic = base / "bad-magic";
  MakeGameTree(bad_magic, "GZLE01");
  WriteBootBin(bad_magic / "sys", "GZLE01", false);
  const fs::path short_boot = base / "short-boot";
  MakeGameTree(short_boot, "GZLE01");
  WriteBootBin(short_boot / "sys", "GZLE01", true, 0x10);
  const fs::path iso_file = base / "Wind Waker.iso";
  WriteFile(iso_file, "not-a-dir", 9);
  const fs::path missing = base / "does-not-exist";
  const fs::path deep = base / "games" / "Wind Waker (USA)" / "sys";  // picks sys itself
  const fs::path trailing = base / "games" / "Wind Waker (USA)" / "";
  const fs::path no_boot = base / "broken-no-boot";
  fs::create_directories(no_boot / "sys");
  fs::create_directories(no_boot / "files");
  WriteMainDol(no_boot / "sys");
  const fs::path dol_bad_entry = base / "dol-bad-entry";
  MakeGameTree(dol_bad_entry, "GZLE01");
  WriteMainDol(dol_bad_entry / "sys", 0x200, true);
  const fs::path dol_bad_table = base / "dol-bad-table";
  MakeGameTree(dol_bad_table, "GZLE01");
  WriteMainDol(dol_bad_table / "sys", 0x200, false, true);
  const fs::path tiny_dol = base / "dol-tiny";
  MakeGameTree(tiny_dol, "GZLE01");
  WriteFile(tiny_dol / "sys" / "main.dol", "x", 1);
  const fs::path junk_id = base / "junk-id";
  MakeGameTree(junk_id, "GZL-!1");
  const fs::path long_path = base / ("very-long-" + std::string(180, 'x')) /
                             ("deep-" + std::string(120, 'y'));
  const fs::path boot_is_dir = base / "bootbin-is-dir";
  MakeGameTree(boot_is_dir, "GZLE01");
  fs::remove(boot_is_dir / "sys" / "boot.bin");
  fs::create_directories(boot_is_dir / "sys" / "boot.bin");
  const fs::path dol_is_dir = base / "dol-is-dir";
  MakeGameTree(dol_is_dir, "GZLE01");
  fs::remove(dol_is_dir / "sys" / "main.dol");
  fs::create_directories(dol_is_dir / "sys" / "main.dol");
  const fs::path sys_is_file = base / "sys-is-file";
  fs::create_directories(sys_is_file / "files");
  WriteFile(sys_is_file / "sys", "x", 1);
  const fs::path files_is_file = base / "files-is-file";
  fs::create_directories(files_is_file / "sys");
  WriteBootBin(files_is_file / "sys", "GZLE01");
  WriteMainDol(files_is_file / "sys");
  WriteFile(files_is_file / "files", "x", 1);
  const fs::path empty_files = base / "empty-files";
  MakeGameTree(empty_files, "GZLE01");
  fs::remove(empty_files / "files" / "_Main.rel");  // files/ now empty
  const fs::path dir_only_files = base / "dir-only-files";
  MakeGameTree(dir_only_files, "GZLE01");
  fs::remove(dir_only_files / "files" / "_Main.rel");
  fs::create_directories(dir_only_files / "files" / "subdir");
  const fs::path plain_file = base / "readme.txt";
  WriteFile(plain_file, "not a folder", 12);
  const fs::path drive_root = fs::path(base).root_path();

  const std::vector<Case> cases = {
      {"valid GZLE01 (spaces)", good, true},
      {"valid GZLE01 (unicode)", unicode, true},
      {"trailing backslash", trailing, true},
      {"missing sys\\main.dol", no_dol, false},
      {"missing files\\", no_files, false},
      {"no files\\_Main.rel (optional)", no_rel, true},
      {"wrong region GZLP01", pal, false},
      {"bad boot magic", bad_magic, false},
      {"truncated boot.bin", short_boot, false},
      {"iso file not folder", iso_file, false},
      {"plain file not folder", plain_file, false},
      {"nonexistent folder", missing, false},
      {"picked sys subdir", deep, false},
      {"missing sys\\boot.bin", no_boot, false},
      {"dol entry outside text", dol_bad_entry, false},
      {"dol corrupt table", dol_bad_table, false},
      {"dol 1 byte", tiny_dol, false},
      {"non-alnum disc id", junk_id, false},
      {"overlong path", long_path, false},
      {"boot.bin is a directory", boot_is_dir, false},
      {"main.dol is a directory", dol_is_dir, false},
      {"sys is a file", sys_is_file, false},
      {"files is a file", files_is_file, false},
      {"files empty", empty_files, false},
      {"files has only a subdir", dir_only_files, false},
      {"drive root", drive_root, false},
      {"empty path", {}, false},
  };
  for (const auto& c : cases)
  {
    const auto probe = moderngekko::runner::ProbeExtractedGame(c.path, exe_dir);
    Show(probe, c.path);
    Check(probe.ok == c.expect_ok, "probe " + c.name);
    Check(probe.ok || !probe.problems.empty(), "problems listed for " + c.name);
  }

  // Message precision for the new structural cases.
  {
    const auto probe = moderngekko::runner::ProbeExtractedGame(boot_is_dir, exe_dir);
    Check(Contains(probe.problems, "is a folder"), "boot.bin dir named as folder");
  }
  {
    const auto probe = moderngekko::runner::ProbeExtractedGame(empty_files, exe_dir);
    Check(Contains(probe.problems, "empty"), "empty files says incomplete dump");
  }
  {
    const auto probe = moderngekko::runner::ProbeExtractedGame(files_is_file, exe_dir);
    Check(Contains(probe.problems, "not a folder"), "files file named as file");
  }

  // Relative saved path resolves against exe dir.
  std::cout << "== Persistence ==\n";
  const fs::path rel_game = exe_dir / "Game" / "ww";
  MakeGameTree(rel_game, "GZLE01");
  {
    std::string err;
    Check(moderngekko::runner::WriteSavedGameRoot(user_dir, exe_dir, rel_game, &err),
          "write saved root");
    const auto back = moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
    Check(back.has_value(), "read saved root present");
    if (back)
      Check(fs::weakly_canonical(*back) == fs::weakly_canonical(rel_game),
            "saved root roundtrip");
    std::ifstream f(moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir));
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    std::cout << "  default-game.txt: " << raw;
  }
  // Unicode path roundtrip.
  {
    std::string err;
    Check(moderngekko::runner::WriteSavedGameRoot(user_dir, exe_dir, unicode, &err),
          "write unicode root");
    const auto back = moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
    Check(back && fs::weakly_canonical(*back) == fs::weakly_canonical(unicode),
          "unicode saved root roundtrip");
  }
  // Missing file -> nullopt.
  fs::remove(moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir));
  Check(!moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir).has_value(),
        "missing file -> nullopt");
  // Empty file -> nullopt.
  WriteFile(moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir), "", 0);
  Check(!moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir).has_value(),
        "empty file -> nullopt");
  // CRLF line ending tolerated.
  WriteFile(moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir),
            "D:\\nowhere\r\n", 13);
  {
    const auto back = moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
    Check(back && U8(back->u8string()).find('\r') == std::string::npos,
          "CRLF trimmed");
  }
  // UTF-8 BOM tolerated (hand-edited or externally written file).
  {
    const std::string bom_path = "\xEF\xBB\xBF" + U8(good.u8string());
    WriteFile(moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir),
              bom_path.data(), bom_path.size());
    const auto back = moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
    Check(back && fs::weakly_canonical(*back) == fs::weakly_canonical(good),
          "UTF-8 BOM stripped");
  }
  // Whitespace-only file -> nullopt (same as missing).
  WriteFile(moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir),
            "   \r\n\t\n", 6);
  Check(!moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir).has_value(),
        "whitespace-only file -> nullopt");
  // default-game.txt itself being a directory: read -> nullopt, write -> fail.
  {
    const fs::path cfg =
        moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir);
    fs::remove(cfg);
    fs::create_directories(cfg);
    Check(!moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir).has_value(),
          "default-game.txt as dir -> nullopt");
    std::string err;
    Check(!moderngekko::runner::WriteSavedGameRoot(user_dir, exe_dir, good, &err),
          "write onto a directory fails cleanly");
    Check(!err.empty(), "write onto a directory reports an error");
    fs::remove_all(cfg);
  }
  // Read-only default-game.txt: a re-save must fail with an error, not crash.
  {
    const fs::path cfg =
        moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir);
    WriteFile(cfg, "placeholder", 11);
    std::error_code perm_ec;
    fs::permissions(cfg, fs::perms::owner_read, fs::perm_options::replace,
                    perm_ec);
    std::string err;
    const bool wrote =
        moderngekko::runner::WriteSavedGameRoot(user_dir, exe_dir, good, &err);
    // Restore writability so cleanup and later tests are unaffected.
    fs::permissions(cfg, fs::perms::owner_all, fs::perm_options::replace, perm_ec);
    if (perm_ec)
      std::cout << "  (read-only attr unsupported here; skipping)\n";
    else
      Check(!wrote && !err.empty(), "read-only default-game.txt write fails cleanly");
    fs::remove(cfg);
  }

  std::cout << "== NormalizeGameRootPath ==\n";
  {
    using moderngekko::runner::NormalizeGameRootPath;
    Check(NormalizeGameRootPath({}).empty(), "normalize empty stays empty");
    Check(NormalizeGameRootPath(good) == good,
          "normalize natural path unchanged");
    const fs::path rel = fs::path("relative") / "dir";
    Check(NormalizeGameRootPath(rel) == rel, "normalize relative unchanged");
#if defined(_WIN32)
    const fs::path prefixed =
        moderngekko::Utf8ToPath("\\\\?\\" + U8(good.u8string()));
    Check(NormalizeGameRootPath(prefixed) == good,
          "normalize \\\\?\\ input reduces to natural");
    const fs::path unc_prefixed =
        moderngekko::Utf8ToPath("\\\\?\\UNC\\server\\share\\dir");
    Check(U8(NormalizeGameRootPath(unc_prefixed).u8string()) ==
              "\\\\server\\share\\dir",
          "normalize \\\\?\\UNC reduces to \\\\share");
    const fs::path drv = moderngekko::Utf8ToPath("\\\\?\\D:");
    Check(U8(NormalizeGameRootPath(drv).u8string()) == "D:\\",
          "normalize \\\\?\\D: gains separator");
    const fs::path dev = moderngekko::Utf8ToPath("\\\\.\\D:\\x");
    Check(NormalizeGameRootPath(dev) == dev, "normalize device path unchanged");
    // Deep fixture that only resolves through the \\?\ form: create it via
    // the extended spelling (plain CreateDirectory can't reach past MAX_PATH).
    const fs::path deep_natural = base / ("long-" + std::string(230, 'x'));
    const fs::path deep_ext = ExtendedForm(deep_natural);
    std::error_code ec;
    fs::create_directories(deep_ext / "sys", ec);
    fs::create_directories(deep_ext / "files", ec);
    if (!ec)
    {
      WriteBootBin(deep_ext / "sys", "GZLE01");
      WriteMainDol(deep_ext / "sys");
      WriteFile(deep_ext / "files" / "dummy.bin", "x", 1);
      // The probe rejects it — but with the real reason, not "does not
      // exist". (InspectGame canonicalizes back to the natural spelling, so
      // the runner can never actually boot a >MAX_PATH root; the honest
      // failure here IS the correct end state.)
      const auto natural_probe =
          moderngekko::runner::ProbeExtractedGame(deep_natural, exe_dir);
      Check(!natural_probe.ok, "probe >MAX_PATH path rejects");
      Check(Contains(natural_probe.problems, "too long"),
            ">MAX_PATH rejection names the real reason");
      // The picker loop must show that same rejection (not a silent loop).
      {
        ScriptedUi s;
        s.dialog_answers.push_back(SetupChoice::Primary);  // Locate
        s.picks.push_back({FolderPick::Outcome::Picked, deep_natural});
        s.dialog_answers.push_back(SetupChoice::Primary);  // Try Again
        s.picks.push_back({FolderPick::Outcome::Picked, good});
        GameSetupRequest req;
        fs::path out;
        std::string err;
        const bool ok = moderngekko::runner::PromptForValidGameRoot(
            req, user_dir, exe_dir, &out, &err, s.Hooks());
        Check(ok && out == good, "deep pick rejected, good pick accepted");
        bool saw_reason = false;
        for (const auto& line : s.transcript)
          saw_reason = saw_reason || line.find("too long") != std::string::npos;
        Check(saw_reason, "deep pick rejection showed the real reason");
      }
      fs::remove_all(deep_ext);
    }
    else
    {
      std::cout << "  (cannot create >MAX_PATH fixture here; skipped)\n";
    }
    // A saved line already carrying \\?\ is canonicalized to natural on read.
    {
      const fs::path cfg =
          moderngekko::runner::DefaultGameFilePath(user_dir, exe_dir);
      const std::string ext_line = "\\\\?\\" + U8(good.u8string());
      WriteFile(cfg, ext_line.data(), ext_line.size());
      const auto back =
          moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
      Check(back && *back == good, "saved \\\\?\\ line read as natural path");
      fs::remove(cfg);
    }
#endif
  }

  std::cout << "== Flow: first run, cancel picker ==\n";
  {
    ScriptedUi s;
    s.picks.push_back({FolderPick::Outcome::Cancelled, {}});
    s.dialog_answers.push_back(SetupChoice::Primary);  // Locate
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(!ok && !err.empty(), "cancel yields clean failure");
  }

  std::cout << "== Flow: first run, bad pick then good pick, persists ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Primary);  // Locate
    s.picks.push_back({FolderPick::Outcome::Picked, no_dol});
    s.dialog_answers.push_back(SetupChoice::Primary);  // Try Again
    s.picks.push_back({FolderPick::Outcome::Picked, good});
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(ok && out == good, "good pick accepted");
    const auto back = moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
    Check(back && fs::weakly_canonical(*back) == fs::weakly_canonical(good),
          "pick persisted to default-game.txt");
  }

  std::cout << "== Flow: saved broken -> Retry -> Locate -> good ==\n";
  {
    // Point the saved file at a folder that doesn't exist, then create it
    // mid-script? No: Retry re-probes the SAME bad folder (still bad), then
    // user picks Locate and selects the good one.
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Secondary);  // Retry (re-probe)
    s.dialog_answers.push_back(SetupChoice::Primary);    // Locate
    s.picks.push_back({FolderPick::Outcome::Picked, good});
    GameSetupRequest req;
    req.previously_configured = true;
    req.current = missing;
    req.rejection_reason = "  - the folder does not exist\n";
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(ok && out == good, "locate after retry works");
  }

  std::cout << "== Flow: saved broken -> Retry succeeds (drive back) ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Secondary);  // Retry
    GameSetupRequest req;
    req.previously_configured = true;
    req.current = good;  // exists again
    req.rejection_reason = "  - earlier failure\n";
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(ok && out == good, "retry accepts revived folder");
  }

  std::cout << "== Flow: bad pick then cancel ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Primary);  // Locate
    s.picks.push_back({FolderPick::Outcome::Picked, pal});
    s.dialog_answers.push_back(SetupChoice::Cancel);
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(!ok && !err.empty(), "cancel after rejection clean");
  }

  std::cout << "== Flow: picker unavailable ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Primary);
    s.picks.push_back({FolderPick::Outcome::Unavailable, {}});
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(!ok && !err.empty(), "unavailable picker clean");
  }

  std::cout << "== Flow: first dialog cancelled ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Cancel);
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    Check(!ok && !err.empty(), "cancel at intro clean");
  }

  std::cout << "== Flow: unicode pick persists ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Primary);
    s.picks.push_back({FolderPick::Outcome::Picked, unicode});
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    Check(ok, "unicode pick accepted");
    const auto back = moderngekko::runner::ReadSavedGameRoot(user_dir, exe_dir);
    Check(back && fs::weakly_canonical(*back) == fs::weakly_canonical(unicode),
          "unicode pick persisted");
  }

  std::cout << "== Flow: picker drive-root pick (trailing separator) ==\n";
  {
    // IFileOpenDialog returns drive roots with a trailing backslash ("D:\");
    // the pick must still be probe-validated and reject cleanly.
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Primary);  // Locate
    s.picks.push_back({FolderPick::Outcome::Picked, drive_root});
    s.dialog_answers.push_back(SetupChoice::Cancel);   // give up at rejection
    GameSetupRequest req;
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(!ok && !err.empty(), "drive-root pick rejected then cancelled");
  }

  std::cout << "== Flow: saved path is a file -> Locate -> good ==\n";
  {
    ScriptedUi s;
    s.dialog_answers.push_back(SetupChoice::Primary);  // Locate
    s.picks.push_back({FolderPick::Outcome::Picked, good});
    GameSetupRequest req;
    req.previously_configured = true;
    req.current = plain_file;  // saved a file path, not a folder
    req.rejection_reason = "  - this is a file, not a folder\n";
    fs::path out;
    std::string err;
    const bool ok = moderngekko::runner::PromptForValidGameRoot(
        req, user_dir, exe_dir, &out, &err, s.Hooks());
    for (const auto& l : s.transcript) std::cout << l << '\n';
    Check(ok && out == good, "relocate after file-as-root works");
  }

  std::cout << "== GamePromptsAvailable ==\n";
  Check(!moderngekko::runner::GamePromptsAvailable(true), "headless blocks prompts");

  fs::remove_all(base);
  std::cout << "== " << (g_checks - g_failures) << "/" << g_checks
            << " checks passed ==\n";
  return g_failures;
}
