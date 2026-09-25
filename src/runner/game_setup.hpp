#pragma once

// First-run / recovery flow for the runner's game path. Kept out of
// moderngekko_run.cpp so the dialog orchestration, the structural probe, and
// the default-game.txt persistence each have one reviewable home. The Win32
// leaf (folder picker + task dialog) lives in game_setup_win32.cpp; on other
// platforms the UI calls report unavailability and callers fall back to the
// console error path unchanged.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::runner {

// Result of the fast structural check of a candidate extracted-disc root.
// This is deliberately the light pass: it reads file headers only and never
// hashes the files/ tree (InspectGame still runs once after selection).
// The probe must never dead-end a root the deep check would take — a probe
// rejection is itself always recoverable (stderr reason or picker retry), so
// it may reject inputs InspectGame would mechanically accept but that can
// never boot usefully (an empty files/ hashes fine yet gives the guest zero
// assets; it is rejected here with an actionable message instead). What must
// not happen is the reverse: the probe accepting a root InspectGame rejects
// with no recovery path.
struct GameProbeResult
{
  bool ok = false;
  // Ordered, user-facing reasons the root was rejected. Never empty when
  // ok == false.
  std::vector<std::string> problems;
  // Populated when sys/boot.bin yields a well-formed ID, even if other
  // problems were found — the recovery dialog can still say what it saw.
  std::string disc_id;
  std::string game_name;
};

GameProbeResult ProbeExtractedGame(const std::filesystem::path& root,
                                   const std::filesystem::path& executable_dir);

// Canonical spelling of `root` for the whole pipeline. On Windows a
// \\?\-prefixed input is reduced to its natural form (InspectGame's own
// weakly_canonical strips the prefix anyway, so keeping it would only skew
// the light probe from the deep check); relative, already-natural, and empty
// input passes through unchanged. Other platforms return the input verbatim.
std::filesystem::path
NormalizeGameRootPath(const std::filesystem::path& root);

// The disc ID this build must boot, when one can be determined:
// MODERNGEKKO_REQUIRED_DISC_ID when compiled in, else inferred from a single
// bundled g<ID>_recomp module beside the executable (the release layout).
// Empty means the runner accepts any well-formed disc.
std::optional<std::string>
RequiredDiscId(const std::filesystem::path& executable_dir);

// Where the remembered game path lives. Mirrors the launcher's rules:
// beside the executable in MODERNGEKKO_PORTABLE_DEFAULT_GAME builds, inside
// the user directory otherwise.
std::filesystem::path
DefaultGameFilePath(const std::filesystem::path& user_dir,
                    const std::filesystem::path& executable_dir);

// The saved root, or nullopt when the file is missing or holds no path.
// Relative entries resolve against the executable directory (same rule the
// runner and launcher have always used).
std::optional<std::filesystem::path>
ReadSavedGameRoot(const std::filesystem::path& user_dir,
                  const std::filesystem::path& executable_dir);

// Persists the chosen root in UTF-8 (same format the launcher writes).
// Portable builds store an executable-relative path when possible.
bool WriteSavedGameRoot(const std::filesystem::path& user_dir,
                        const std::filesystem::path& executable_dir,
                        const std::filesystem::path& root, std::string* error);

// "  - problem" lines for console output.
std::string DescribeProbeProblems(const GameProbeResult& probe);

// Full stderr text for a rejected root in a non-interactive run: explains
// where the path came from and how to fix it without a UI.
std::string DescribeUnusableGameRoot(const std::filesystem::path& root,
                                     const GameProbeResult& probe,
                                     bool came_from_saved_file,
                                     bool came_from_cli,
                                     const std::filesystem::path& user_dir,
                                     const std::filesystem::path& executable_dir);

// ---------- platform UI ----------

enum class SetupChoice
{
  Primary,    // the labelled action ("Locate Game Files…", "Try Again", "Continue")
  Secondary,  // the optional alternate action ("Retry" the same path)
  Cancel,
};

struct SetupDialog
{
  std::string title;
  std::string heading;
  std::string details;
  std::string primary_label;
  std::optional<std::string> secondary_label;
};

// Shows the dialog and returns the choice. Returns Cancel when no UI layer
// exists (non-Windows, or every dialog call fails) so callers degrade to a
// plain error instead of hanging.
SetupChoice ShowSetupDialog(const SetupDialog& dialog);

struct FolderPick
{
  enum class Outcome
  {
    Picked,
    Cancelled,
    Unavailable,
  };
  Outcome outcome = Outcome::Cancelled;
  std::filesystem::path path;
};

FolderPick PickGameFolder(std::string_view title);

// Injectable seam: tests and headless harnesses substitute scripted answers
// for the real dialogs, keeping every loop below exercisable without a
// desktop.
struct GameSetupUi
{
  std::function<SetupChoice(const SetupDialog&)> show_dialog = &ShowSetupDialog;
  std::function<FolderPick(std::string_view)> pick_folder = &PickGameFolder;
};

// Interactive dialogs are a Windows feature of this runner and only make
// sense when a human can see them. --headless and MODERNGEKKO_DISABLE_GAME_SETUP
// both force the console path so automation can never block on a modal.
bool GamePromptsAvailable(bool headless);

struct GameSetupRequest
{
  // True when the rejected root came from default-game.txt (the dialog can
  // then offer "Retry" alongside relocating).
  bool previously_configured = false;
  // The configured root being rejected (empty for a first run).
  std::filesystem::path current;
  // Why the current root was rejected — probe problem lines or the deep
  // InspectGame error.
  std::string rejection_reason;
};

// Runs the whole user-visible flow: explanation dialog -> folder picker ->
// probe -> retry/cancel, repeating until the user picks a valid folder or
// cancels. On success writes the pick to default-game.txt (a write failure
// is reported but does not block this run) and returns the root through
// root_out. On cancel/unavailable returns false with *error set.
bool PromptForValidGameRoot(const GameSetupRequest& request,
                            const std::filesystem::path& user_dir,
                            const std::filesystem::path& executable_dir,
                            std::filesystem::path* root_out, std::string* error,
                            const GameSetupUi& ui = {});

}  // namespace moderngekko::runner
