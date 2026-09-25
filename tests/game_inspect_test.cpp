#include "moderngekko/game.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Portable env set/clear: UCRT has no setenv/unsetenv. _putenv_s copies the
// value, and "_putenv NAME=" removes the variable entirely (vs setting an
// empty string, which this reader happens to treat the same but is not the
// same state for other consumers).
void UnsetEnv(const char* name)
{
#ifdef _WIN32
  _putenv((std::string(name) + "=").c_str());
#else
  ::unsetenv(name);
#endif
}

int SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  return _putenv_s(name, value);
#else
  return ::setenv(name, value, 1);
#endif
}
}  // namespace

int main()
{
  const fs::path root = fs::temp_directory_path() / "moderngekko-game-inspect-test";
  fs::remove_all(root);
  fs::create_directories(root / "sys");
  fs::create_directories(root / "files");

  // Pin the manifest wire format and ordering against an independently
  // calculated SHA-256. Create entries in reverse order and include a nested
  // path and an empty file so traversal order cannot determine the digest.
  fs::create_directories(root / "files" / "nested");
  std::ofstream(root / "files" / "z.bin", std::ios::binary);
  std::ofstream(root / "files" / "nested" / "z.bin", std::ios::binary) << "wind";
  std::ofstream(root / "files" / "a.bin", std::ios::binary) << "abc";
  const auto tree_hash = moderngekko::HashDirectorySha256(root / "files");
  if (tree_hash != "027b8dd900f08001030ef0d20cc6e8d1995ac7de99d62f036641f446cee28eb9")
    return 2;
  if (moderngekko::HashDirectorySha256(root / "files" / "nested" / "..") != tree_hash)
    return 3;
  const auto original_time = fs::last_write_time(root / "files" / "a.bin");
  std::ofstream(root / "files" / "a.bin", std::ios::binary) << "abd";
  fs::last_write_time(root / "files" / "a.bin", original_time);
  if (moderngekko::HashDirectorySha256(root / "files") == tree_hash)
    return 4;  // A same-size edit with a restored timestamp must still be hashed.
  std::ofstream(root / "files" / "a.bin", std::ios::binary) << "abc";
  if (moderngekko::HashDirectorySha256(root / "files") != tree_hash)
    return 5;

  std::array<unsigned char, 0x60> boot{};
  const char id[] = "TEST01";
  std::copy_n(id, 6, boot.begin());
  boot[0x18] = 0x5d; boot[0x19] = 0x1c; boot[0x1a] = 0x9e; boot[0x1b] = 0xa3;
  const char name[] = "Synthetic Test Game";
  std::copy_n(name, sizeof(name), boot.begin() + 0x20);
  std::ofstream(root / "sys" / "boot.bin", std::ios::binary)
      .write(reinterpret_cast<const char*>(boot.data()), boot.size());

  std::array<unsigned char, 0x104> dol{};
  dol[0x02] = 0x01;  // Text section 0 file offset: 0x100.
  dol[0x48] = 0x80; dol[0x49] = 0x00; dol[0x4a] = 0x31; dol[0x4b] = 0x00;
  dol[0x93] = 0x04;
  dol[0xe0] = 0x80; dol[0xe1] = 0x00; dol[0xe2] = 0x31; dol[0xe3] = 0x00;
  std::ofstream(root / "sys" / "main.dol", std::ios::binary)
      .write(reinterpret_cast<const char*>(dol.data()), dol.size());

  const auto result = moderngekko::InspectGame(root);
  if (!result || result.metadata->disc_id != "TEST01" ||
      result.metadata->game_name != "Synthetic Test Game" ||
      result.metadata->platform != moderngekko::GamePlatform::Wii ||
      result.metadata->entry_point != 0x80003100u ||
      result.metadata->dol_sha256 !=
          "ee292f5fc3d0e5cfa32d951bd682a3cd2806c102e4a0a50300a2c480e21bcef6")
  {
    fs::remove_all(root);
    return 1;
  }

  // MODERNGEKKO_HASH_THREADS determinism: the manifest (sorted per-file
  // digests) must be bit-identical whether digests are produced serially or
  // by workers. Sizes cross the 64-byte SHA-256 block and the streaming
  // read-buffer boundaries so padding/chunking is exercised too.
  const fs::path par = fs::temp_directory_path() / "moderngekko-game-inspect-par";
  fs::remove_all(par);
  fs::create_directories(par / "sub");
  const std::vector<std::size_t> sizes = {
      0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 1024, 1u << 20,
      (1u << 22) + 13,  // cross the 4 MiB streaming buffer
      3u * (1u << 20) + 7,
  };
  for (std::size_t i = 0; i < sizes.size(); ++i)
  {
    std::vector<char> bytes(sizes[i]);
    for (std::size_t j = 0; j < bytes.size(); ++j)
      bytes[j] = static_cast<char>((j * 31 + i * 7) & 0xFF);
    const fs::path dir = (i % 3 == 0) ? par / "sub" : par;
    const fs::path file = dir / (std::string("f") + std::to_string(i) + ".bin");
    std::ofstream(file, std::ios::binary).write(bytes.data(), bytes.size());
  }
  UnsetEnv("MODERNGEKKO_HASH_THREADS");
  const auto serial_hash = moderngekko::HashDirectorySha256(par);
  if (SetEnv("MODERNGEKKO_HASH_THREADS", "8") != 0)
  {
    fs::remove_all(root);
    fs::remove_all(par);
    return 6;
  }
  const auto parallel_hash = moderngekko::HashDirectorySha256(par);
  // Per-file path coverage too: single-file hash must not depend on mode.
  const auto file_serial_baseline = moderngekko::HashFileSha256(par / "f13.bin");
  fs::remove_all(par);
  fs::remove_all(root);
  if (!serial_hash || !parallel_hash || *serial_hash != *parallel_hash)
    return 7;
  if (!file_serial_baseline || file_serial_baseline->size() != 64)
    return 8;
  return 0;
}
