// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "moderngekko/gx_state_backend.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace moderngekko
{
namespace
{
bool PacingStatsEnabled()
{
  static const bool enabled = [] {
    const char* raw = std::getenv("MODERNGEKKO_PACING_STATS");
    return raw != nullptr && raw[0] != '\0' && !(raw[0] == '0' && raw[1] == '\0');
  }();
  return enabled;
}
}  // namespace

GxStateBackend::GxStateBackend(AddressSpace& memory) : m_memory(memory), m_vertex_loader(memory)
{
}

void GxStateBackend::SetRenderDevice(GxRenderDevice* device)
{
  m_device = device;
}

void GxStateBackend::Reset()
{
  m_cp = {};
  m_xf = {};
  m_bp = {};
  m_bp_mask = 0xFFFFFFu;
}

void GxStateBackend::LoadCpRegister(std::uint8_t command, std::uint32_t value)
{
  m_cp[command] = value;
}

void GxStateBackend::LoadXfRegisters(std::uint16_t address,
                                     std::span<const std::uint32_t> values)
{
  if (address >= m_xf.size())
    return;
  const std::size_t count = std::min(values.size(), m_xf.size() - address);
  std::ranges::copy(values.first(count), m_xf.begin() + address);
}

void GxStateBackend::LoadBpRegister(std::uint8_t command, std::uint32_t value)
{
  value &= 0xFFFFFFu;
  if (command == 0xFEu)
  {
    m_bp_mask = value;
    m_bp[command] = value;
    return;
  }

  m_bp[command] = (m_bp[command] & ~m_bp_mask) | (value & m_bp_mask);
  m_bp_mask = 0xFFFFFFu;
  if (command == 0x52u && m_device)
  {
    const std::uint32_t source = m_bp[0x49u];
    const std::uint32_t dimensions = m_bp[0x4Au];
    const std::uint32_t control = m_bp[0x52u];
    const std::uint64_t filter = static_cast<std::uint64_t>(m_bp[0x53u]) |
                                 (static_cast<std::uint64_t>(m_bp[0x54u]) << 24u);
    std::array<std::uint8_t, 7> coefficients{};
    for (std::size_t i = 0; i < coefficients.size(); ++i)
      coefficients[i] = static_cast<std::uint8_t>((filter >> (i * 6u)) & 0x3Fu);
    const std::uint32_t clear_color = ((m_bp[0x4Fu] >> 8u) & 0xFFu) << 24u |
                                      (m_bp[0x50u] & 0xFFu) << 16u |
                                      ((m_bp[0x50u] >> 8u) & 0xFFu) << 8u |
                                      (m_bp[0x4Fu] & 0xFFu);
    m_device->CopyEfb({
        static_cast<std::uint16_t>((source >> 10) & 0x3FFu),
        static_cast<std::uint16_t>(source & 0x3FFu),
        static_cast<std::uint16_t>(((dimensions >> 10) & 0x3FFu) + 1u),
        static_cast<std::uint16_t>((dimensions & 0x3FFu) + 1u),
        (m_bp[0x4Bu] & 0xFFFFFFu) << 5,
        (m_bp[0x4Du] & 0xFFFFFFu) << 5,
        m_bp[0x4Eu] & 0xFFFFFFu,
        static_cast<std::uint8_t>((control >> 3) & 0xFu),
        static_cast<std::uint8_t>((control >> 7) & 3u),
        static_cast<std::uint8_t>((control >> 12) & 3u),
        coefficients,
        clear_color,
        m_bp[0x51u] & 0xFFFFFFu,
        (control & (1u << 0)) != 0u,
        (control & (1u << 1)) != 0u,
        (control & (1u << 9)) != 0u,
        (control & (1u << 10)) != 0u,
        (control & (1u << 11)) != 0u,
        (control & (1u << 14)) != 0u,
        (control & (1u << 15)) != 0u,
        (control & (1u << 16)) != 0u,
    });
  }
}

void GxStateBackend::LoadIndexedXf(std::uint8_t array, std::uint16_t index,
                                   std::uint16_t address, std::uint8_t size)
{
  if (array >= 16u || address >= m_xf.size())
    return;
  const std::uint32_t base = m_cp[0xA0u + array];
  const std::uint32_t stride = m_cp[0xB0u + array] & 0xFFu;
  const std::uint64_t source_address = static_cast<std::uint64_t>(base) +
                                       static_cast<std::uint64_t>(stride) * index;
  if (source_address > 0xFFFFFFFFu)
    return;
  const std::uint8_t count = static_cast<std::uint8_t>(
      std::min<std::size_t>(size, m_xf.size() - address));
  for (std::uint8_t i = 0; i < count; ++i)
  {
    // u64 math: source_address near 0xFFFFFFFF plus i*4 would wrap a u32
    // and silently read a low address instead of failing.
    const std::uint64_t read_address =
        source_address + static_cast<std::uint64_t>(i) * 4u;
    std::uint32_t value = 0;
    if (read_address > 0xFFFFFFFFu ||
        !m_memory.Read32(static_cast<std::uint32_t>(read_address), &value))
      break;
    m_xf[address + i] = value;
  }
}

void GxStateBackend::Draw(const GxDrawPacket& packet)
{
  if (m_device)
  {
    GxDecodedDraw decoded;
    if (m_vertex_loader.Decode(packet, m_cp, &decoded))
      m_device->SubmitDecodedDraw(packet, decoded, GetState());
    else
      m_device->SubmitDraw(packet, GetState());
  }
}

void GxStateBackend::InvalidateVertexCache()
{
  if (m_device)
    m_device->InvalidateVertexCache();
}

void GxStateBackend::PresentXfb(const GxXfbPresent& present)
{
  // Present-pacing diagnostics. Default OFF: enable with
  // MODERNGEKKO_PACING_STATS=1 (any other value, including "0" or unset,
  // keeps this a single predictable branch). When on, emits one stderr
  // line per 120 guest XFB presents with the running count and the latest
  // geometry so Windows runs can correlate guest present rate against the
  // swapchain/window FPS sampled in process.csv.
  //
  // Measurement note (VI Hz vs window FPS): VI Hz is the guest
  // VideoInterface interrupt rate (guest-time progress); window FPS is the
  // rate at which presents reach the OS swapchain. Wind Waker renders one
  // frame per 2 VIs, so at full present throughput window FPS ~= VI Hz / 2
  // (20-25 VI Hz -> 10-12.5 FPS is the game cadence showing through, not a
  // second bottleneck). Record both counters separately; never derive FPS
  // from VI (frame-skip, duplicate presents, compositor coalescing, and the
  // headless Null backend all break the conversion).
  if (PacingStatsEnabled())
  {
    // Emulation-thread only (same thread that issues Draw calls).
    static std::uint64_t s_presents = 0;
    if (++s_presents % 120 == 0)
    {
      std::fprintf(stderr, "pacing: %llu xfb presents (last %ux%u stride %u)\n",
                   static_cast<unsigned long long>(s_presents), present.width,
                   present.height, present.stride);
    }
  }
  if (m_device)
    m_device->PresentXfb(present);
}

GxStateView GxStateBackend::GetState() const
{
  return {m_cp, m_xf, m_bp};
}
}
