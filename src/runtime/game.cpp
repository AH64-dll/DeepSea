#include "moderngekko/game.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace moderngekko
{
namespace
{
constexpr std::array<std::uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t ReadBE32(const std::uint8_t* p)
{
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
         (std::uint32_t{p[2]} << 8) | p[3];
}

// Incremental SHA-256: identical digest to the original all-at-once
// Sha256Portable (the game_inspect_test manifest hash pins the format).
// Streaming lets large files hash through a fixed-size buffer instead of a
// whole-file allocation, and lets several files hash concurrently.
class Sha256Ctx
{
public:
  void Update(const std::uint8_t* data, std::size_t size)
  {
    m_total += size;
    if (m_block_len != 0)
    {
      const std::size_t take = std::min<std::size_t>(64 - m_block_len, size);
      std::memcpy(m_block.data() + m_block_len, data, take);
      m_block_len += take;
      data += take;
      size -= take;
      if (m_block_len == 64)
      {
        Process(m_block.data());
        m_block_len = 0;
      }
    }
    while (size >= 64)
    {
      Process(data);
      data += 64;
      size -= 64;
    }
    if (size != 0)
    {
      std::memcpy(m_block.data(), data, size);
      m_block_len = size;
    }
  }

  std::string Final()
  {
    const std::uint64_t bit_size = m_total * 8;
    const std::uint8_t one = 0x80;
    Update(&one, 1);
    const std::uint8_t zero = 0;
    while (m_block_len != 56)
      Update(&zero, 1);
    std::uint8_t length[8];
    for (int shift = 56, i = 0; shift >= 0; shift -= 8, ++i)
      length[i] = static_cast<std::uint8_t>(bit_size >> shift);
    Update(length, sizeof(length));

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto word : m_h)
      out << std::setw(8) << word;
    return out.str();
  }

private:
  void Process(const std::uint8_t* data)
  {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
      w[i] = ReadBE32(data + i * 4);
    for (std::size_t i = 16; i < 64; ++i)
    {
      const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, hh] = m_h;
    for (std::size_t i = 0; i < 64; ++i)
    {
      const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto ch = (e & f) ^ (~e & g);
      const auto t1 = hh + s1 + ch + K[i] + w[i];
      const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + s0 + maj;
    }
    m_h[0] += a; m_h[1] += b; m_h[2] += c; m_h[3] += d;
    m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += hh;
  }

  std::array<std::uint32_t, 8> m_h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> m_block{};
  std::size_t m_block_len = 0;
  std::uint64_t m_total = 0;
};

std::string Sha256Portable(std::vector<std::uint8_t> data)
{
  Sha256Ctx ctx;
  ctx.Update(data.data(), data.size());
  return ctx.Final();
}

#ifdef _WIN32
// Keep BCrypt optional: the release runner has no new import/library
// dependency, and a provider/API failure falls back to the exact portable
// implementation above.
namespace
{
using BcryptHandle = void*;
using OpenAlgorithmProviderFn = long(WINAPI*)(BcryptHandle*, const wchar_t*, const wchar_t*, unsigned long);
using CloseAlgorithmProviderFn = long(WINAPI*)(BcryptHandle, unsigned long);
using GetPropertyFn = long(WINAPI*)(BcryptHandle, const wchar_t*, unsigned char*, unsigned long,
                                    unsigned long*, unsigned long);
using CreateHashFn = long(WINAPI*)(BcryptHandle, BcryptHandle*, unsigned char*, unsigned long,
                                   unsigned char*, unsigned long, unsigned long);
using HashDataFn = long(WINAPI*)(BcryptHandle, unsigned char*, unsigned long, unsigned long);
using FinishHashFn = long(WINAPI*)(BcryptHandle, unsigned char*, unsigned long, unsigned long);
using DestroyHashFn = long(WINAPI*)(BcryptHandle);

struct BcryptApi
{
  HMODULE library = nullptr;
  OpenAlgorithmProviderFn open = nullptr;
  CloseAlgorithmProviderFn close = nullptr;
  GetPropertyFn get_property = nullptr;
  CreateHashFn create_hash = nullptr;
  HashDataFn hash_data = nullptr;
  FinishHashFn finish_hash = nullptr;
  DestroyHashFn destroy_hash = nullptr;

  BcryptApi()
  {
    library = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
      return;
    open = reinterpret_cast<OpenAlgorithmProviderFn>(GetProcAddress(library, "BCryptOpenAlgorithmProvider"));
    close = reinterpret_cast<CloseAlgorithmProviderFn>(GetProcAddress(library, "BCryptCloseAlgorithmProvider"));
    get_property = reinterpret_cast<GetPropertyFn>(GetProcAddress(library, "BCryptGetProperty"));
    create_hash = reinterpret_cast<CreateHashFn>(GetProcAddress(library, "BCryptCreateHash"));
    hash_data = reinterpret_cast<HashDataFn>(GetProcAddress(library, "BCryptHashData"));
    finish_hash = reinterpret_cast<FinishHashFn>(GetProcAddress(library, "BCryptFinishHash"));
    destroy_hash = reinterpret_cast<DestroyHashFn>(GetProcAddress(library, "BCryptDestroyHash"));
  }

  bool usable() const
  {
    return library && open && close && get_property && create_hash && hash_data && finish_hash && destroy_hash;
  }

  ~BcryptApi() { if (library) FreeLibrary(library); }
};

const BcryptApi& GetBcryptApi()
{
  static const BcryptApi api;
  return api;
}

struct BcryptAlgorithmGuard
{
  const BcryptApi* api = nullptr;
  BcryptHandle handle = nullptr;
  ~BcryptAlgorithmGuard() { if (handle) api->close(handle, 0); }
};

struct BcryptHashGuard
{
  const BcryptApi* api = nullptr;
  BcryptHandle handle = nullptr;
  ~BcryptHashGuard() { if (handle) api->destroy_hash(handle); }
};

// Shared bcrypt hash-object setup + hex digest. `feed` supplies successive
// chunks (set *chunk/*len, return true) and returns false at end-of-input.
template <typename Feed>
std::optional<std::string> Sha256BcryptFeed(Feed&& feed)
{
  const auto& api = GetBcryptApi();
  if (!api.usable())
    return std::nullopt;
  constexpr long kSuccess = 0;
  BcryptAlgorithmGuard algorithm{&api};
  if (api.open(&algorithm.handle, L"SHA256", nullptr, 0) != kSuccess)
    return std::nullopt;

  unsigned long object_length = 0;
  unsigned long result_length = 0;
  if (api.get_property(algorithm.handle, L"ObjectLength", reinterpret_cast<unsigned char*>(&object_length),
                       sizeof(object_length), &result_length, 0) != kSuccess ||
      object_length == 0) {
    return std::nullopt;
  }
  std::vector<unsigned char> object(object_length);
  BcryptHashGuard hash{&api};
  if (api.create_hash(algorithm.handle, &hash.handle, object.data(), object_length, nullptr, 0, 0) != kSuccess) {
    return std::nullopt;
  }
  bool ok = true;
  unsigned char* chunk = nullptr;
  unsigned long chunk_len = 0;
  while (ok && feed(&chunk, &chunk_len))
  {
    if (chunk_len != 0)
      ok = api.hash_data(hash.handle, chunk, chunk_len, 0) == kSuccess;
  }
  std::array<unsigned char, 32> digest{};
  if (ok)
    ok = api.finish_hash(hash.handle, digest.data(), static_cast<unsigned long>(digest.size()), 0) == kSuccess;
  if (!ok)
    return std::nullopt;
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest)
    out << std::setw(2) << static_cast<unsigned int>(byte);
  return out.str();
}

std::optional<std::string> Sha256Bcrypt(const std::vector<std::uint8_t>& data)
{
  std::size_t offset = 0;
  constexpr std::size_t kChunk = 1u << 20;
  return Sha256BcryptFeed([&](unsigned char** chunk, unsigned long* len) {
    if (offset >= data.size())
      return false;
    const auto count = static_cast<unsigned long>(std::min(kChunk, data.size() - offset));
    *chunk = const_cast<unsigned char*>(data.data() + offset);
    *len = count;
    offset += count;
    return true;
  });
}

std::optional<std::string> Sha256FileBcrypt(std::ifstream& file)
{
  std::vector<char> buffer(1 << 20);
  bool stream_ok = true;
  const auto result = Sha256BcryptFeed([&](unsigned char** chunk, unsigned long* len) {
    if (!file)
      return false;
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<unsigned long>(file.gcount());
    if (file.bad())
      stream_ok = false;
    if (count == 0)
      return false;
    *chunk = reinterpret_cast<unsigned char*>(buffer.data());
    *len = count;
    return true;
  });
  if (!stream_ok)
    return std::nullopt;
  return result;
}
}  // namespace
#endif

std::string Sha256(std::vector<std::uint8_t> data)
{
#ifdef _WIN32
  if (const auto digest = Sha256Bcrypt(data))
    return *digest;
#endif
  return Sha256Portable(std::move(data));
}

std::optional<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return std::nullopt;
  const auto size = file.tellg();
  if (size < 0)
    return std::nullopt;
  // A giant or corrupt file must fail as nullopt, not escape as bad_alloc —
  // InspectGame callers treat inspection as a structured result, not a
  // throwing API.
  std::vector<std::uint8_t> bytes;
  try
  {
    bytes.resize(static_cast<std::size_t>(size));
  }
  catch (const std::exception&)
  {
    return std::nullopt;
  }
  file.seekg(0);
  if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size))
    return std::nullopt;
  return bytes;
}

// MODERNGEKKO_HASH_THREADS: per-file hashing workers for
// HashDirectorySha256. Unset/0/1 = serial (legacy behavior); >=2 = that many
// worker threads, capped by hardware_concurrency and file count. The file
// list is fixed (walked + sorted before workers spawn) and each worker owns
// disjoint digest slots, so the manifest — and therefore the resulting hash —
// is bit-identical to the serial pass regardless of scheduling.
unsigned int DirectoryHashThreads(std::size_t file_count)
{
  const char* env = std::getenv("MODERNGEKKO_HASH_THREADS");
  if (env == nullptr || env[0] == '\0')
    return 1;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(env, &end, 10);
  // Whole-token parse only: "2x" or "-1" (which strtoul wraps to ULONG_MAX)
  // must not silently arm a thread pool.
  if (end == env || *end != '\0' || env[0] == '-' || parsed <= 1)
    return 1;
  const unsigned int hw = std::thread::hardware_concurrency();
  const unsigned long cap = hw != 0 ? hw : 4;
  const unsigned long wanted = std::min<unsigned long>({parsed, cap, 64ul});
  return static_cast<unsigned int>(
      std::min<unsigned long>(wanted, file_count != 0 ? file_count : 1));
}
}  // namespace

std::optional<std::string> HashFileSha256(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
#ifdef _WIN32
  if (const auto digest = Sha256FileBcrypt(file))
    return digest;
  file.clear();
  file.seekg(0, std::ios::beg);
  if (!file)
    return std::nullopt;
#endif
  // Stream through a fixed buffer: a whole-file vector is a ~550 MB
  // transient on the largest game assets, and with parallel workers those
  // allocations would multiply.
  Sha256Ctx ctx;
  std::vector<char> buffer(1 << 22);
  while (file)
  {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(file.gcount());
    if (count != 0)
      ctx.Update(reinterpret_cast<const std::uint8_t*>(buffer.data()), count);
  }
  if (file.bad())
    return std::nullopt;
  return ctx.Final();
}

std::optional<std::string> HashDirectorySha256(const std::filesystem::path& root)
{
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec)
    return std::nullopt;
  struct ManifestFile
  {
    std::filesystem::path path;
    std::string relative;
  };
  std::vector<ManifestFile> files;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  while (!ec && iterator != end)
  {
    if (iterator->is_regular_file(ec) && !ec)
    {
      // relative() canonicalizes paths and can query the filesystem. Compute
      // it once per file rather than twice for every comparison during sort.
      // Keep the canonical relative spelling used by existing release hashes.
      // UTF-8, not .generic_string(): on Windows the latter emits the ANSI
      // code page, so two netplay peers on different code pages would hash
      // different manifest bytes for a non-ASCII asset name (the digest feeds
      // the compatibility fingerprint). ASCII names — all supported game
      // files — encode identically, so pinned hashes are unchanged.
      const auto relative_u8 =
          std::filesystem::relative(iterator->path(), root, ec).generic_u8string();
      if (ec)
        return std::nullopt;
      std::string relative(reinterpret_cast<const char*>(relative_u8.data()),
                           relative_u8.size());
      files.push_back({iterator->path(), std::move(relative)});
    }
    if (ec)
      return std::nullopt;
    iterator.increment(ec);
  }
  if (ec)
    return std::nullopt;
  std::ranges::sort(files, [&](const auto& left, const auto& right) {
    return left.relative < right.relative;
  });
  // Per-file digests: each file's hash is a pure function of its bytes, and
  // the manifest below consumes digests[i] in sorted order, so workers may
  // fill disjoint slots in any schedule. Digest collection happens before
  // the guest even exists — nothing here is guest-visible.
  std::vector<std::optional<std::string>> digests(files.size());
  const unsigned int workers = DirectoryHashThreads(files.size());
  if (workers > 1)
  {
    std::atomic<std::size_t> next{0};
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (unsigned int w = 0; w < workers; ++w)
    {
      pool.emplace_back([&]() {
        for (;;)
        {
          const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= files.size())
            break;
          // A throwing alloc inside a jthread is std::terminate; a failed
          // digest slot below is the graceful version of the same failure.
          try
          {
            digests[i] = HashFileSha256(files[i].path);
          }
          catch (const std::exception&)
          {
            digests[i] = std::nullopt;
          }
        }
      });
    }
    pool.clear();  // join all workers before manifest assembly
  }
  else
  {
    for (std::size_t i = 0; i < files.size(); ++i)
    {
      try
      {
        digests[i] = HashFileSha256(files[i].path);
      }
      catch (const std::exception&)
      {
        digests[i] = std::nullopt;
      }
    }
  }
  std::vector<std::uint8_t> manifest;
  for (std::size_t i = 0; i < files.size(); ++i)
  {
    const auto& [path, relative] = files[i];
    const auto size = std::filesystem::file_size(path, ec);
    const auto& hash = digests[i];
    if (ec || !hash)
      return std::nullopt;
    const auto append_u64 = [&](std::uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8)
        manifest.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    append_u64(relative.size());
    manifest.insert(manifest.end(), relative.begin(), relative.end());
    append_u64(size);
    manifest.insert(manifest.end(), hash->begin(), hash->end());
  }
  return Sha256(std::move(manifest));
}

GameInspectResult InspectGame(const std::filesystem::path& input_root)
{
  std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(input_root, ec);
  if (ec || !std::filesystem::is_directory(root, ec) || ec)
    return {{}, "game root is not a readable directory"};
  const auto dol_path = root / "sys" / "main.dol";
  const auto boot_path = root / "sys" / "boot.bin";
  const auto rel_path = root / "files" / "_Main.rel";
  // ec-overloads: this function reports failures as structured errors, so a
  // throwing fs:: call here would bypass the error path (and crash callers
  // that do not expect exceptions from inspection).
  if (!std::filesystem::is_regular_file(dol_path, ec))
    return {{}, "missing sys/main.dol"};
  if (!std::filesystem::is_directory(root / "files", ec))
    return {{}, "missing files directory"};
  const auto boot = ReadFile(boot_path);
  const auto dol = ReadFile(dol_path);
  if (!boot || boot->size() < 0x60)
    return {{}, "missing or malformed sys/boot.bin"};
  if (!dol || dol->size() < 0x100)
    return {{}, "malformed sys/main.dol"};

  const std::uint32_t entry_point = ReadBE32(dol->data() + 0xe0);
  bool entry_is_executable = false;
  for (std::size_t section = 0; section < 18; ++section)
  {
    const std::uint32_t offset = ReadBE32(dol->data() + section * 4);
    const std::uint32_t address = ReadBE32(dol->data() + 0x48 + section * 4);
    const std::uint32_t size = ReadBE32(dol->data() + 0x90 + section * 4);
    if (size == 0)
      continue;
    if (offset == 0 || address == 0 || static_cast<std::uint64_t>(offset) + size > dol->size() ||
        static_cast<std::uint64_t>(address) + size > 0x100000000ULL)
      return {{}, "malformed DOL section table"};
    if (section < 7 && entry_point >= address &&
        static_cast<std::uint64_t>(entry_point) < static_cast<std::uint64_t>(address) + size)
      entry_is_executable = true;
  }
  if (!entry_is_executable)
    return {{}, "DOL entry point is outside its text sections"};

  std::string id(reinterpret_cast<const char*>(boot->data()), 6);
  // SECURITY: this alnum-only check is load-bearing — moderngekko_port.cpp
  // embeds disc_id UNQUOTED into generator/cmake command lines and uses it
  // as a path component. Relaxing it (e.g. to allow '-') would turn a
  // hostile boot.bin into shell injection / path traversal.
  if (id.size() != 6 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) != 0;
      }))
    return {{}, "invalid six-character disc ID in boot.bin"};

  std::string name(reinterpret_cast<const char*>(boot->data() + 0x20), 0x40);
  if (const auto terminator = name.find('\0'); terminator != std::string::npos)
    name.resize(terminator);
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    name.pop_back();
  if (name.empty())
    name = id;
  const auto wii_magic = ReadBE32(boot->data() + 0x18);
  const auto gc_magic = ReadBE32(boot->data() + 0x1c);
  if (wii_magic != 0x5d1c9ea3 && gc_magic != 0xc2339f3d)
    return {{}, "boot.bin has neither Wii nor GameCube disc magic"};

  GameMetadata metadata;
  metadata.root = root;
  metadata.main_dol = dol_path;
  if (std::filesystem::is_regular_file(rel_path, ec))
    metadata.main_rel = rel_path;
  metadata.game_name = std::move(name);
  metadata.disc_id = std::move(id);
  metadata.platform = wii_magic == 0x5d1c9ea3 ? GamePlatform::Wii : GamePlatform::GameCube;
  metadata.entry_point = entry_point;
  metadata.dol_sha256 = Sha256(std::move(*dol));
  if (!metadata.main_rel.empty())
  {
    const auto rel_hash = HashFileSha256(metadata.main_rel);
    if (!rel_hash)
      return {{}, "can't hash files/_Main.rel"};
    metadata.rel_sha256 = *rel_hash;
  }
  const auto assets_hash = HashDirectorySha256(root / "files");
  if (!assets_hash)
    return {{}, "can't hash the files directory"};
  metadata.assets_sha256 = *assets_hash;
  return {std::move(metadata), {}};
}
}  // namespace moderngekko
