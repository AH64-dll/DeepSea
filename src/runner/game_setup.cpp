#include "runner/game_setup.hpp"

#include "moderngekko/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <system_error>

namespace moderngekko::runner {
namespace {

std::string PathText(const std::filesystem::path& path)
{
  const std::u8string encoded = path.u8string();
  return {encoded.begin(), encoded.end()};
}

std::uint32_t ReadBE32(const std::uint8_t* p)
{
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
         (std::uint32_t{p[2]} << 8) | p[3];
}

// Reads up to `limit` bytes; nullopt only when the file cannot be opened.
// A short read still returns the bytes it got — callers size-check.
std::optional<std::vector<std::uint8_t>> ReadFilePrefix(const std::filesystem::path& path,
                                                      std::size_t limit)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
  std::vector<std::uint8_t> data(limit);
  file.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(limit));
  data.resize(static_cast<std::size_t>(file.gcount()));
  return data;
}

// A file selected instead of a folder: name the common disc-image
// extensions so the message can say "extract it" rather than just "no".
bool LooksLikeDiscImage(const std::filesystem::path& path)
{
  const std::u8string extension_u8 = path.extension().u8string();
  std::string extension(reinterpret_cast<const char*>(extension_u8.data()),
                        extension_u8.size());
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return extension == ".iso" || extension == ".gcm" || extension == ".rvz" ||
         extension == ".wbfs" || extension == ".ciso" || extension == ".nkit";
}

bool EnvFlagEnabled(const char* name)
{
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0')
    return false;
  std::string value{raw};
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

#if defined(_WIN32)
// \\?\ extended-length form of an absolute path: preferred separators, dot
// components collapsed (the OS performs no normalization past the prefix, so
// we do it here). Empty when the path cannot take one (relative, empty, or
// already device-prefixed).
std::filesystem::path ExtendedLengthForm(const std::filesystem::path& path)
{
  if (path.empty() || !path.is_absolute())
    return {};
  const std::wstring native =
      std::filesystem::path(path).make_preferred().lexically_normal().native();
  if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\"))
    return {};
  if (native.starts_with(L"\\\\"))
    return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2));
  return std::filesystem::path(L"\\\\?\\" + native);
}
#else
std::filesystem::path ExtendedLengthForm(const std::filesystem::path&)
{
  return {};
}
#endif

}  // namespace

std::filesystem::path
NormalizeGameRootPath(const std::filesystem::path& root)
{
#if defined(_WIN32)
  const std::wstring& native = root.native();
  std::wstring stripped;
  if (native.starts_with(L"\\\\?\\UNC\\"))
    stripped = L"\\\\" + native.substr(8);
  else if (native.starts_with(L"\\\\?\\"))
    stripped = native.substr(4);
  else
    return root;
  // "D:" alone through the \\?\ prefix means the drive root — spell it with
  // the separator so the result is absolute rather than drive-relative.
  if (stripped.size() == 2 && stripped[1] == L':')
    stripped += L'\\';
  return std::filesystem::path(stripped);
#else
  return root;
#endif
}

std::optional<std::string> RequiredDiscId(const std::filesystem::path& executable_dir)
{
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  (void)executable_dir;
  return std::string(MODERNGEKKO_REQUIRED_DISC_ID);
#else
  // Unbranded build: if exactly one g<ID>_recomp module sits beside the
  // executable (the shipped release layout), that module's ID is still the
  // only bootable disc — enforcing it here turns a wrong-region pick into a
  // clear picker rejection instead of a later "no native module" failure.
  std::error_code ec;
  if (!std::filesystem::is_directory(executable_dir, ec))
    return std::nullopt;
  std::set<std::string> ids;
  for (const auto& entry : std::filesystem::directory_iterator(executable_dir, ec))
  {
    if (ec || !entry.is_regular_file(ec))
      continue;
    const std::u8string name_u8 = entry.path().filename().u8string();
    const std::string name(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());
    // g + 6-char ID + _recomp + .dll/.so/.dylib
    constexpr std::string_view kSuffix = "_recomp";
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || name.size() < 1 + 6 + kSuffix.size() + 1)
      continue;
    const std::string ext = name.substr(dot);
    std::string lowered = ext;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (lowered != ".dll" && lowered != ".so" && lowered != ".dylib")
      continue;
    const std::string stem = name.substr(0, dot);
    if (stem.size() != 1 + 6 + kSuffix.size() || stem.front() != 'g' ||
        stem.compare(1 + 6, kSuffix.size(), kSuffix) != 0)
      continue;
    const std::string id = stem.substr(1, 6);
    if (!std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) != 0; }))
      continue;
    ids.insert(id);
  }
  if (ids.size() == 1)
    return *ids.begin();
  return std::nullopt;
#endif
}

GameProbeResult ProbeExtractedGame(const std::filesystem::path& root,
                                   const std::filesystem::path& executable_dir)
{
  GameProbeResult result;
  auto reject = [&result](std::string problem) {
    result.problems.push_back(std::move(problem));
  };

  if (root.empty())
  {
    reject("no game folder was selected");
    return result;
  }

  std::error_code ec;
  const bool exists = std::filesystem::exists(root, ec);
  if (ec)
  {
    reject("the path could not be read (" + ec.message() +
           ") — check that the drive is connected and the path is usable");
    return result;
  }
  if (!exists)
  {
    // A path that resolves only through the \\?\ extended form — past the
    // ~260-char limit, or a component ending in a space/dot that the plain
    // form strips — reports as missing to a plain stat. Callers normalize
    // their roots beforehand, so landing here means an un-normalized input;
    // say what is actually wrong instead of "does not exist".
    const std::filesystem::path extended = ExtendedLengthForm(root);
    std::error_code extended_ec;
    if (!extended.empty() && extended != root &&
        std::filesystem::exists(extended, extended_ec))
    {
      reject("the folder exists, but this path can't be used directly — it is "
             "too long or has a name ending in a space or dot; move or rename "
             "the folder and try again");
      return result;
    }
    reject("the folder does not exist — it may have been moved or deleted");
    return result;
  }
  if (!std::filesystem::is_directory(root, ec))
  {
    if (LooksLikeDiscImage(root))
    {
      reject("this is a disc image file, not an extracted folder — extract it "
             "first, then select the folder that was extracted from it");
    }
    else
    {
      reject("this is a file, not a folder — select the extracted disc folder "
             "(the one containing \"sys\" and \"files\")");
    }
    return result;
  }

  const std::filesystem::path dol_path = root / "sys" / "main.dol";
  if (!std::filesystem::is_regular_file(dol_path, ec))
  {
    if (std::filesystem::is_directory(dol_path, ec))
      reject("\"sys\\main.dol\" is a folder, not the game executable file — "
             "the disc was extracted incorrectly");
    else
      reject("missing \"sys\\main.dol\" — this is not the extracted disc root "
             "(select the folder that contains \"sys\" and \"files\")");
  }
  else
  {
    const auto dol_size = std::filesystem::file_size(dol_path, ec);
    if (ec)
      reject("\"sys\\main.dol\" could not be read (" + ec.message() + ")");
    else if (dol_size < 0x100)
      reject("\"sys\\main.dol\" is too small to be a game executable");
    else
    {
      const auto header = ReadFilePrefix(dol_path, 0x100);
      if (!header || header->size() < 0xe4)
      {
        reject("\"sys\\main.dol\" could not be read");
      }
      else
      {
        // Same structural rules InspectGame applies: sane section table and an
        // entry point inside a text section. Only the hashing is deferred.
        const std::uint32_t entry_point = ReadBE32(header->data() + 0xe0);
        bool table_bad = false;
        bool entry_is_executable = false;
        for (std::size_t section = 0; section < 18 && !table_bad; ++section)
        {
          const std::uint32_t offset = ReadBE32(header->data() + section * 4);
          const std::uint32_t address = ReadBE32(header->data() + 0x48 + section * 4);
          const std::uint32_t size = ReadBE32(header->data() + 0x90 + section * 4);
          if (size == 0)
            continue;
          if (offset == 0 || address == 0 ||
              static_cast<std::uint64_t>(offset) + size > dol_size ||
              static_cast<std::uint64_t>(address) + size > 0x100000000ULL)
            table_bad = true;
          else if (section < 7 && entry_point >= address &&
                   static_cast<std::uint64_t>(entry_point) <
                       static_cast<std::uint64_t>(address) + size)
            entry_is_executable = true;
        }
        if (table_bad)
          reject("\"sys\\main.dol\" has a corrupt section table");
        else if (!entry_is_executable)
          reject("\"sys\\main.dol\" does not look like a game executable");
      }
    }
  }

  const std::filesystem::path boot_path = root / "sys" / "boot.bin";
  if (!std::filesystem::is_regular_file(boot_path, ec))
  {
    if (std::filesystem::is_directory(boot_path, ec))
      reject("\"sys\\boot.bin\" is a folder, not a file — the disc was "
             "extracted incorrectly");
    else
      reject("missing \"sys\\boot.bin\"");
  }
  else
  {
    const auto boot = ReadFilePrefix(boot_path, 0x60);
    if (!boot || boot->size() < 0x60)
    {
      reject("\"sys\\boot.bin\" is missing or truncated");
    }
    else
    {
      std::string id(reinterpret_cast<const char*>(boot->data()), 6);
      if (!std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) != 0; }))
        reject("\"sys\\boot.bin\" does not contain a valid disc ID");
      else
        result.disc_id = id;

      std::string name(reinterpret_cast<const char*>(boot->data() + 0x20), 0x40);
      if (const auto end = name.find('\0'); end != std::string::npos)
        name.resize(end);
      while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
      result.game_name = name.empty() ? result.disc_id : name;

      const bool wii_magic = ReadBE32(boot->data() + 0x18) == 0x5d1c9ea3;
      const bool gc_magic = ReadBE32(boot->data() + 0x1c) == 0xc2339f3d;
      if (!wii_magic && !gc_magic)
        reject("\"sys\\boot.bin\" is not a GameCube/Wii disc header");
    }
  }

  // files/ presence mirrors InspectGame's contract — no deeper content check
  // here: _Main.rel is optional there (hashed only when present) and the
  // assets manifest hash is what actually judges completeness. The one
  // exception: a files/ tree with NO regular files hashes fine but gives the
  // guest nothing to run (verified: zero guest cycles, silent exit 0), so it
  // is rejected here where the message can say what went wrong.
  const std::filesystem::path files_path = root / "files";
  if (!std::filesystem::is_directory(files_path, ec))
  {
    if (std::filesystem::exists(files_path, ec))
      reject("\"files\" is a file, not a folder — the disc was extracted "
             "incorrectly");
    else
      reject("missing \"files\" folder");
  }
  else
  {
    // Any regular file anywhere under files/ counts as content. The walk is
    // capped so a hand-made junction/symlink cycle can never hang setup.
    std::error_code list_ec;
    std::filesystem::recursive_directory_iterator it(
        files_path, std::filesystem::directory_options::skip_permission_denied,
        list_ec);
    const std::filesystem::recursive_directory_iterator end;
    bool any_file = false;
    std::size_t walked = 0;
    while (!list_ec && !any_file && it != end && walked++ < 100000)
    {
      if (it->is_regular_file(list_ec) && !list_ec)
        any_file = true;
      it.increment(list_ec);
    }
    if (list_ec)
      reject("the \"files\" folder could not be listed (" + list_ec.message() +
             ") — check its permissions");
    else if (!any_file)
      reject("the \"files\" folder is empty — the disc contents were not fully "
             "extracted; re-extract the whole disc");
  }

  if (!result.disc_id.empty())
  {
    if (const auto required = RequiredDiscId(executable_dir);
        required && result.disc_id != *required)
    {
      reject("the selected folder is disc \"" + result.disc_id +
             "\", but this release requires \"" + *required + "\"");
    }
  }

  result.ok = result.problems.empty();
  return result;
}

std::filesystem::path DefaultGameFilePath(const std::filesystem::path& user_dir,
                                          const std::filesystem::path& executable_dir)
{
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  (void)user_dir;
  return executable_dir / "default-game.txt";
#else
  (void)executable_dir;
  return user_dir / "default-game.txt";
#endif
}

std::optional<std::filesystem::path>
ReadSavedGameRoot(const std::filesystem::path& user_dir,
                  const std::filesystem::path& executable_dir)
{
  std::ifstream file(DefaultGameFilePath(user_dir, executable_dir));
  std::string line;
  std::getline(file, line);
  // Tolerate a UTF-8 BOM (hand-edited or externally written files): Utf8ToPath
  // would keep U+FEFF as a leading path character and reject a valid root.
  if (line.starts_with("\xEF\xBB\xBF"))
    line.erase(0, 3);
  // Trim surrounding whitespace (covers \r\n endings and hand-edited files).
  while (!line.empty() &&
         std::isspace(static_cast<unsigned char>(line.back())))
    line.pop_back();
  const auto first = line.find_first_not_of(" \t");
  if (first == std::string::npos)
    return std::nullopt;
  if (first != 0)
    line.erase(0, first);
  if (line.empty())
    return std::nullopt;
  // default-game.txt is UTF-8 (WriteSavedGameRoot and the launcher's
  // PathText write it that way). A \\?\ spelling is accepted on read and
  // reduced to the natural form the rest of the pipeline expects.
  std::filesystem::path root = NormalizeGameRootPath(Utf8ToPath(line));
  if (root.empty())
    return std::nullopt;
  if (root.is_relative())
    root = executable_dir / root;
  return root;
}

bool WriteSavedGameRoot(const std::filesystem::path& user_dir,
                        const std::filesystem::path& executable_dir,
                        const std::filesystem::path& root, std::string* error)
{
  const std::filesystem::path destination =
      DefaultGameFilePath(user_dir, executable_dir);
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  std::ofstream file(destination, std::ios::trunc);
  if (!file)
  {
    if (error)
      *error = "could not write " + PathText(destination);
    return false;
  }
  // Persist the natural spelling (no \\?\ prefix) — every reader of this
  // file handles that form; some (PowerShell 5.1) cannot parse the prefix.
  std::filesystem::path stored = NormalizeGameRootPath(root);
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const std::filesystem::path relative =
      std::filesystem::relative(root, executable_dir, ec);
  if (!ec && !relative.empty())
    stored = relative;
#endif
  file << PathText(stored) << '\n';
  if (!file)
  {
    if (error)
      *error = "could not write " + PathText(destination);
    return false;
  }
  return true;
}

std::string DescribeProbeProblems(const GameProbeResult& probe)
{
  std::string text;
  for (const std::string& problem : probe.problems)
  {
    text += "  - ";
    text += problem;
    text += '\n';
  }
  return text;
}

std::string DescribeUnusableGameRoot(const std::filesystem::path& root,
                                     const GameProbeResult& probe,
                                     bool came_from_saved_file,
                                     bool came_from_cli,
                                     const std::filesystem::path& user_dir,
                                     const std::filesystem::path& executable_dir)
{
  std::string text;
  if (came_from_cli)
  {
    text = "the --game folder \"";
    text += PathText(root);
    text += "\" is not a usable extracted disc:\n";
  }
  else if (came_from_saved_file)
  {
    text = "the saved game folder \"";
    text += PathText(root);
    text += "\" is no longer usable:\n";
  }
  else if (!root.empty())
  {
    text = "the game folder \"";
    text += PathText(root);
    text += "\" is not a usable extracted disc:\n";
  }
  else
  {
    text = "no game folder is configured — pass --game <extracted-disc-folder>\n"
           "or write the path to ";
    text += PathText(DefaultGameFilePath(user_dir, executable_dir));
    text += '\n';
  }
  text += DescribeProbeProblems(probe);
  if (!came_from_cli && !root.empty())
  {
    text += "run without --headless to choose a different folder, or edit ";
    text += PathText(DefaultGameFilePath(user_dir, executable_dir));
    text += '\n';
  }
#if defined(_WIN32)
  // On Windows a non-headless run only lands here when the env flag forced
  // prompts off — "run without --headless" alone would send the user in
  // circles, so name the actual blocker.
  if (EnvFlagEnabled("MODERNGEKKO_DISABLE_GAME_SETUP"))
    text += "(MODERNGEKKO_DISABLE_GAME_SETUP is set — remove it for the "
            "interactive folder picker)\n";
#endif
  return text;
}

bool GamePromptsAvailable(bool headless)
{
#if !defined(_WIN32)
  (void)headless;
  return false;
#else
  if (headless)
    return false;
  return !EnvFlagEnabled("MODERNGEKKO_DISABLE_GAME_SETUP");
#endif
}

namespace {

std::string DialogTitle()
{
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  return MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#else
  return "ModernGekko";
#endif
}

}  // namespace

bool PromptForValidGameRoot(const GameSetupRequest& request,
                            const std::filesystem::path& user_dir,
                            const std::filesystem::path& executable_dir,
                            std::filesystem::path* root_out, std::string* error,
                            const GameSetupUi& ui)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };

  std::string reason = request.rejection_reason;
  // Normalize once up front: Retry re-probes this path, so it must carry the
  // same spelling the deep check will see (\\?\ input reduces to natural).
  const std::filesystem::path current = NormalizeGameRootPath(request.current);
  // Outer loop: re-shows the "why" dialog. After a secondary "Retry" the
  // same path is re-probed; after each picker round a rejected folder comes
  // back through the inner loop with its own problem list.
  for (;;)
  {
    SetupDialog intro;
    intro.title = DialogTitle();
    if (request.previously_configured && !current.empty())
    {
      intro.heading = "The saved game folder can't be used";
      intro.details = "\"" + PathText(request.current) + "\"\n\n" + reason +
                      "\nRetry re-checks the same folder (for example after "
                      "reconnecting a drive).\n\"Locate Game Files\" lets you "
                      "select a different folder. Cancel quits.";
      intro.primary_label = "Locate Game Files...";
      intro.secondary_label = "Retry";
    }
    else
    {
      intro.heading = "Game data required";
      intro.details =
          "This program needs files extracted from your own copy of the game "
          "disc - it does not include or download the game itself.\n\n"
          "Choose \"Locate Game Files\" and select the extracted disc folder "
          "- the one containing the \"sys\" and \"files\" folders (for "
          "example, the output of extracting the disc in Dolphin). Cancel "
          "quits.";
      intro.primary_label = "Locate Game Files...";
    }

    const SetupChoice choice = ui.show_dialog(intro);
    if (choice == SetupChoice::Cancel)
      return fail("game setup was cancelled");

    if (choice == SetupChoice::Secondary)
    {
      const auto probe = ProbeExtractedGame(current, executable_dir);
      if (probe.ok)
      {
        *root_out = current;
        return true;
      }
      reason = DescribeProbeProblems(probe);
      continue;
    }

    // Primary: folder picker loop. Each pick is probed immediately so the
    // rejection can say exactly what is wrong with that folder.
    for (;;)
    {
      const FolderPick pick =
          ui.pick_folder("Select the extracted disc folder");
      if (pick.outcome == FolderPick::Outcome::Cancelled)
        return fail("no game folder was selected");
      if (pick.outcome == FolderPick::Outcome::Unavailable)
        return fail("the folder picker could not be shown");

      // Normalize before probing so the result names the spelling InspectGame
      // will see (a typed-in \\?\ pick reduces to natural here).
      const std::filesystem::path picked = NormalizeGameRootPath(pick.path);
      const auto probe = ProbeExtractedGame(picked, executable_dir);
      if (probe.ok)
      {
        std::string write_error;
        if (!WriteSavedGameRoot(user_dir, executable_dir, picked, &write_error))
        {
          // Losing persistence must not block this launch — the game still
          // starts; next time the picker simply reappears.
          SetupDialog notice;
          notice.title = DialogTitle();
          notice.heading = "The game folder could not be saved";
          notice.details = write_error +
                           "\n\nThe game will still start now, but you will "
                           "be asked for the folder again next time.";
          notice.primary_label = "Continue";
          ui.show_dialog(notice);
        }
        *root_out = picked;
        return true;
      }

      SetupDialog rejected;
      rejected.title = DialogTitle();
      rejected.heading = "That folder can't be used";
      rejected.details = "\"" + PathText(pick.path) + "\"\n\n" +
                         DescribeProbeProblems(probe) +
                         "\n\"Try Again\" opens the picker again. Cancel quits.";
      rejected.primary_label = "Try Again";
      if (ui.show_dialog(rejected) != SetupChoice::Primary)
        return fail("game setup was cancelled");
    }
  }
}

#if !defined(_WIN32)
// Non-Windows builds keep the console error path: no picker, no dialogs.
SetupChoice ShowSetupDialog(const SetupDialog&)
{
  return SetupChoice::Cancel;
}

FolderPick PickGameFolder(std::string_view)
{
  return {FolderPick::Outcome::Unavailable, {}};
}
#endif

}  // namespace moderngekko::runner
