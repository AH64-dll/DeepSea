// ModuleSetup drives the disc builder as a child process. The test binary
// doubles as that child: copied to <release>/disc-builder/python/python.exe
// and started with the builder's arguments, it behaves like build.py
// (progress, success, failure, or a slow build that gets cancelled).
#include "launcher_module_setup.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;
using moderngekko::PathToUtf8;
using moderngekko::Utf8ToPath;
using moderngekko::frontend::ModuleSetup;

namespace
{
int failures = 0;

void Check(bool ok, const char* what)
{
  if (!ok)
  {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void WriteText(const fs::path& path, const std::string& text)
{
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string JsonString(const std::string& text)
{
  std::string out = "\"";
  for (char c : text)
  {
    if (c == '\\' || c == '"')
      out += '\\';
    out += c;
  }
  return out + "\"";
}

// The child's arguments as paths. On Windows main()'s argv is in the ANSI code
// page, so read the wide command line the way python.exe does.
std::vector<fs::path> Arguments(int argc, char** argv)
{
  std::vector<fs::path> out;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
  for (int i = 0; i < count; ++i)
    out.emplace_back(std::wstring(wide[i]));
  LocalFree(wide);
#else
  for (int i = 0; i < argc; ++i)
    out.push_back(Utf8ToPath(argv[i]));
#endif
  return out;
}

// Fake builder: python.exe -I build.py <game> --output <dir> --status <file>
int FakeBuilder(int argc, char** argv)
{
  const std::vector<fs::path> args = Arguments(argc, argv);
  if (args.size() != 8 || args[1] != "-I" || args[2].filename() != "build.py" ||
      args[4] != "--output" || args[6] != "--status")
    return 3;
  const std::string game = args[3].filename().string();
  const fs::path output = args[5];
  const fs::path status = args[7];
  WriteText(status, R"({"stage": "Compiling game", "completed": 3, "total": 4})");
  if (game == "slow")
  {
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
  }
  if (game == "fail")
  {
    WriteText(status, R"({"stage": "Setup failed", "error": "Unsupported or damaged game", "finished": true})");
    return 1;
  }
  const fs::path module = output / "GZLE01" / "abc" / "gGZLE01_recomp.dll";
  WriteText(module, "module");
  WriteText(status, "{\"stage\": \"Ready to play\", \"completed\": 1, \"total\": 1, \"module\": " +
                        JsonString(PathToUtf8(module)) + ", \"finished\": true}");
  return 0;
}

fs::path OwnExecutable(const char* argv0)
{
#ifdef _WIN32
  wchar_t buffer[32768];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
  return fs::path(std::wstring(buffer, length));
#else
  (void)argv0;
  return fs::read_symlink("/proc/self/exe");
#endif
}

bool RunToEnd(ModuleSetup& setup)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (setup.Poll())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc > 1)
    return FakeBuilder(argc, argv);

  // Non-ASCII on purpose: the arguments cross the process boundary as UTF-8.
  const fs::path root =
      fs::temp_directory_path() / Utf8ToPath("mg-module-setup-\xC3\xA9-" + std::to_string(std::rand()));
  const fs::path release = root / "release";
  const fs::path user = root / "user";
  fs::create_directories(release / "disc-builder" / "python");
  WriteText(release / "disc-builder" / "build.py", "");
  fs::copy_file(OwnExecutable(argv[0]), release / "disc-builder" / "python" / "python.exe",
                fs::copy_options::overwrite_existing);

  Check(ModuleSetup::Available(release), "kit detected");
  Check(!ModuleSetup::Available(root), "no kit, no setup");

  // Stale status files from earlier launches are pruned; recent ones stay.
  const fs::path old_status = user / "Setup" / "module-1.json";
  const fs::path recent_status = user / "Setup" / "module-2.json";
  WriteText(old_status, "{}");
  WriteText(recent_status, "{}");
  fs::last_write_time(old_status, fs::file_time_type::clock::now() - std::chrono::hours(48));

  {
    ModuleSetup setup;
    Check(setup.Start(release, user, root / "game"), "setup starts");
    Check(setup.Running(), "running after start");
    Check(RunToEnd(setup), "setup finishes");
    Check(!setup.Running(), "not running after finish");
    const fs::path expected = user / "modules" / "GZLE01" / "abc" / "gGZLE01_recomp.dll";
    Check(setup.Module() == expected, "module path comes from the builder");
    Check(setup.Error().empty(), "no error on success");
    Check(setup.Status() == "Ready to play", "final stage");
    Check(!fs::exists(old_status), "day-old status file pruned");
    Check(fs::exists(recent_status), "recent status file kept");
  }
  {
    ModuleSetup setup;
    Check(setup.Start(release, user, root / "fail"), "failing setup starts");
    Check(RunToEnd(setup), "failing setup finishes");
    Check(setup.Module().empty(), "no module after failure");
    Check(setup.Error() == "Unsupported or damaged game", "builder error is shown");
  }
  {
    ModuleSetup setup;
    Check(setup.Start(release, user, root / "slow"), "slow setup starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto started = std::chrono::steady_clock::now();
    setup.Cancel();
    Check(!setup.Running(), "cancel stops the builder");
    Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(10), "cancel is prompt");
    Check(setup.Module().empty(), "no module after cancel");
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  if (failures)
    return 1;
  std::puts("launcher_module_setup_test: PASS");
  return 0;
}
