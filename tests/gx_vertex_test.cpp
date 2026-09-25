#include "moderngekko/gx_vertex_loader.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <vector>

int main()
{
  moderngekko::AddressSpace memory;
  moderngekko::GxVertexLoader loader(memory);
  std::array<std::uint32_t, 256> cp{};
  cp[0x50] = 1u << 9;
  cp[0x70] = 9u;

  std::array<std::uint8_t, 48> data{};
  const std::array<float, 12> positions = {
      0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  for (std::size_t i = 0; i < positions.size(); ++i)
  {
    const std::uint32_t value = std::bit_cast<std::uint32_t>(positions[i]);
    data[i * 4] = static_cast<std::uint8_t>(value >> 24);
    data[i * 4 + 1] = static_cast<std::uint8_t>(value >> 16);
    data[i * 4 + 2] = static_cast<std::uint8_t>(value >> 8);
    data[i * 4 + 3] = static_cast<std::uint8_t>(value);
  }

  moderngekko::GxDecodedDraw decoded;
  const moderngekko::GxDrawPacket packet = {
      moderngekko::GxPrimitive::Quads, 0u, 12u, 4u, data};
  if (!loader.Decode(packet, cp, &decoded) || decoded.vertices.size() != 4u ||
      decoded.indices != std::vector<std::uint32_t>({0, 1, 2, 0, 2, 3}) ||
      std::abs(decoded.vertices[2].position[1] - 1.0f) > 0.0001f)
  {
    return 1;
  }

  cp[0x50] = 2u << 9;
  cp[0xA0] = 0x80001000u;
  cp[0xB0] = 12u;
  for (std::size_t i = 0; i < data.size(); ++i)
    memory.Write8(0x80001000u + static_cast<std::uint32_t>(i), data[i]);
  const std::array<std::uint8_t, 3> indices = {0u, 2u, 3u};
  const moderngekko::GxDrawPacket indexed = {
      moderngekko::GxPrimitive::Triangles, 0u, 1u, 3u, indices};
  if (!loader.Decode(indexed, cp, &decoded) || decoded.vertices.size() != 3u ||
      std::abs(decoded.vertices[1].position[0] - 1.0f) > 0.0001f ||
      decoded.indices != std::vector<std::uint32_t>({0, 1, 2}))
  {
    return 2;
  }

  cp = {};
  cp[0x50] = 2u << 11;
  cp[0x70] = (1u << 9) | (1u << 10) | (1u << 31);
  cp[0xA1] = 0x80002000u;
  cp[0xB1] = 9u;
  const std::array<std::uint8_t, 9> ntb = {64u, 0u, 0u, 0u, 64u, 0u, 0u, 0u, 64u};
  for (std::size_t i = 0; i < ntb.size(); ++i)
    memory.Write8(0x80002000u + static_cast<std::uint32_t>(i), ntb[i]);
  const std::array<std::uint8_t, 3> normal_indices{};
  const moderngekko::GxDrawPacket indexed_normals = {
      moderngekko::GxPrimitive::Points, 0u, 3u, 1u, normal_indices};
  if (!loader.Decode(indexed_normals, cp, &decoded) || decoded.vertices.size() != 1u ||
      std::abs(decoded.vertices[0].normal[0] - 1.0f) > 0.0001f ||
      std::abs(decoded.vertices[0].tangent[1] - 1.0f) > 0.0001f ||
      std::abs(decoded.vertices[0].binormal[2] - 1.0f) > 0.0001f)
  {
    return 3;
  }

  cp = {};
  cp[0x50] = (1u << 0) | (2u << 9);
  cp[0x70] = 9u;
  cp[0xA0] = 0x80001000u;
  cp[0xB0] = 12u;
  const std::array<std::uint8_t, 8> skipped = {
      0xFFu, 0u, 0x7Fu, 0xFFu, 0xFFu, 2u, 0xFFu, 3u};
  const moderngekko::GxDrawPacket skipped_packet = {
      moderngekko::GxPrimitive::Quads, 0u, 2u, 4u, skipped};
  if (!loader.Decode(skipped_packet, cp, &decoded) || decoded.vertices.size() != 3u ||
      decoded.vertices[0].position_matrix != 0x3Fu ||
      decoded.indices != std::vector<std::uint32_t>({0, 1, 2}))
  {
    return 4;
  }

  // A vertex discarded by a -1 position index must not fail the draw when a
  // trailing indexed attribute resolves to unmapped memory — the vertex is
  // dropped, so its tail data is dead either way (Dolphin's compiled loaders
  // skip straight to the next vertex stride without touching the arrays).
  cp = {};
  cp[0x50] = (2u << 9) | (2u << 13);  // indexed8 position + indexed8 color0
  cp[0x70] = 9u | (5u << 14);         // float xyz position, RGBA8 color
  cp[0xA0] = 0x80001000u;
  cp[0xB0] = 12u;
  // Color array base sits at the top of MEM1 so index 0 resolves but index
  // 0xFF (on the skipped vertex) walks past 0x01800000 into the gap.
  cp[0xA2] = 0x817FFF00u;
  cp[0xB2] = 4u;
  memory.Write32(0x817FFF00u, 0x11223344u);
  const std::array<std::uint8_t, 4> skip_tail = {0xFFu, 0xFFu, 0x01u, 0x00u};
  const moderngekko::GxDrawPacket skip_tail_packet = {
      moderngekko::GxPrimitive::Points, 0u, 2u, 2u, skip_tail};
  if (!loader.Decode(skip_tail_packet, cp, &decoded) ||
      decoded.vertices.size() != 1u ||
      decoded.vertices[0].color[0] != 0x11223344u)
  {
    return 8;
  }

  // Same unmapped color index on a non-skipped vertex still fails the draw.
  const std::array<std::uint8_t, 4> bad_tail = {0x01u, 0xFFu, 0x02u, 0x00u};
  const moderngekko::GxDrawPacket bad_tail_packet = {
      moderngekko::GxPrimitive::Points, 0u, 2u, 2u, bad_tail};
  if (loader.Decode(bad_tail_packet, cp, &decoded))
    return 9;

  // vat is a free u8 on the public packet but VAT state has only 8 groups:
  // 8 and 255 must be rejected before the cp[0x70+vat] window read.
  const moderngekko::GxDrawPacket vat8_packet = {
      moderngekko::GxPrimitive::Points, 8u, 2u, 2u, bad_tail};
  if (loader.Decode(vat8_packet, cp, &decoded))
    return 10;
  const moderngekko::GxDrawPacket vat255_packet = {
      moderngekko::GxPrimitive::Points, 255u, 2u, 2u, bad_tail};
  if (loader.Decode(vat255_packet, cp, &decoded))
    return 11;

  // vat == 7 is the boundary: group regs live at cp[0x77]/cp[0x87]/cp[0x97],
  // so a valid direct draw must still decode.
  cp = {};
  cp[0x50] = 1u << 9;
  cp[0x77] = 9u;
  const moderngekko::GxDrawPacket vat7_packet = {
      moderngekko::GxPrimitive::Quads, 7u, 12u, 4u, data};
  if (!loader.Decode(vat7_packet, cp, &decoded) || decoded.vertices.size() != 4u ||
      std::abs(decoded.vertices[2].position[1] - 1.0f) > 0.0001f)
  {
    return 12;
  }

  // cp must carry the full 256-entry register file.
  std::array<std::uint32_t, 128> short_cp{};
  const moderngekko::GxDrawPacket direct_packet = {
      moderngekko::GxPrimitive::Quads, 0u, 12u, 4u, data};
  if (loader.Decode(direct_packet, short_cp, &decoded))
    return 13;

  // A null output sink is a contract violation, not a crash.
  cp = {};
  cp[0x50] = 1u << 9;
  cp[0x70] = 9u;
  if (loader.Decode(direct_packet, cp, nullptr))
    return 14;

  // A legal-but-huge vertex count over a 1-byte stream fails closed on the
  // first ReadIndex/ReadU8 underrun — it must neither hang nor throw.
  const std::array<std::uint8_t, 1> tiny = {0x00u};
  const moderngekko::GxDrawPacket huge_packet = {
      moderngekko::GxPrimitive::Points, 0u, 12u, 0xFFFFu, tiny};
  if (loader.Decode(huge_packet, cp, &decoded))
    return 15;

  // The stream must be consumed exactly: one trailing byte is a protocol
  // error (strict AtEnd), not a successful decode.
  const std::array<std::uint8_t, 49> padded{};
  const moderngekko::GxDrawPacket padded_packet = {
      moderngekko::GxPrimitive::Quads, 0u, 12u, 4u, padded};
  if (loader.Decode(padded_packet, cp, &decoded))
    return 16;

  // A vertex skipped by the indexed8 -1 sentinel still consumes the full
  // vertex stride, including DIRECT attributes — vertex 1's color must land
  // on the bytes after the skipped vertex's color, not at its offset.
  cp = {};
  cp[0x50] = (2u << 9) | (1u << 13);  // indexed8 position + direct color0
  cp[0x70] = 9u | (5u << 14);         // float xyz position, RGBA8 color
  cp[0xA0] = 0x80001000u;
  cp[0xB0] = 12u;
  for (std::size_t i = 0; i < data.size(); ++i)
    memory.Write8(0x80001000u + static_cast<std::uint32_t>(i), data[i]);
  const std::array<std::uint8_t, 10> skip_direct = {
      0xFFu, 0xDEu, 0xADu, 0xBEu, 0xEFu,   // skipped vertex + its color
      0x01u, 0x11u, 0x22u, 0x33u, 0x44u};  // kept vertex: index 1 + color
  const moderngekko::GxDrawPacket skip_direct_packet = {
      moderngekko::GxPrimitive::Points, 0u, 5u, 2u, skip_direct};
  if (!loader.Decode(skip_direct_packet, cp, &decoded) ||
      decoded.vertices.size() != 1u ||
      decoded.vertices[0].color[0] != 0x11223344u)
  {
    return 17;
  }

  // indexed16 (desc 3) uses the 0xFFFF skip sentinel, not 0xFF: a 0xFFFF
  // position index skips the vertex while 0x00FF is a live index.
  cp = {};
  cp[0x50] = 3u << 9;  // indexed16 position
  cp[0x70] = 9u;
  cp[0xA0] = 0x80001000u;
  cp[0xB0] = 12u;
  const std::array<std::uint8_t, 4> skip16 = {0xFFu, 0xFFu, 0x00u, 0x01u};
  const moderngekko::GxDrawPacket skip16_packet = {
      moderngekko::GxPrimitive::Points, 0u, 4u, 2u, skip16};
  if (!loader.Decode(skip16_packet, cp, &decoded) ||
      decoded.vertices.size() != 1u ||
      std::abs(decoded.vertices[0].position[0] - 1.0f) > 0.0001f)
  {
    return 18;
  }

  struct ColorCase
  {
    std::uint32_t format;
    std::vector<std::uint8_t> bytes;
    std::uint32_t expected;
  };
  const std::array<ColorCase, 6> colors = {{
      {0u, {0xF8u, 0x00u}, 0xFF0000FFu},
      {1u, {0x01u, 0x02u, 0x03u}, 0x010203FFu},
      {2u, {0x01u, 0x02u, 0x03u, 0x99u}, 0x010203FFu},
      {3u, {0xF1u, 0x2Eu}, 0xFF1122EEu},
      {4u, {0xFFu, 0xFFu, 0xFFu}, 0xFFFFFFFFu},
      {5u, {0x01u, 0x02u, 0x03u, 0x04u}, 0x01020304u},
  }};
  for (const ColorCase& color : colors)
  {
    cp = {};
    cp[0x50] = 1u << 13;
    cp[0x70] = color.format << 14;
    const moderngekko::GxDrawPacket color_packet = {
        moderngekko::GxPrimitive::Points, 0u,
        static_cast<std::uint32_t>(color.bytes.size()), 1u, color.bytes};
    if (!loader.Decode(color_packet, cp, &decoded) || decoded.vertices.size() != 1u ||
        decoded.vertices[0].color[0] != color.expected)
    {
      return 5;
    }
  }

  cp = {};
  cp[0x50] = 1u << 11;
  cp[0x70] = 1u << 10;
  const std::array<std::uint8_t, 3> signed_normal = {0xC0u, 0x00u, 0x40u};
  const moderngekko::GxDrawPacket normal_packet = {
      moderngekko::GxPrimitive::Points, 0u, 3u, 1u, signed_normal};
  if (!loader.Decode(normal_packet, cp, &decoded) ||
      std::abs(decoded.vertices[0].normal[0] + 1.0f) > 0.0001f ||
      std::abs(decoded.vertices[0].normal[2] - 1.0f) > 0.0001f)
  {
    return 6;
  }

  cp = {};
  cp[0x60] = 1u;
  cp[0x70] = (1u << 21) | (3u << 22) | (1u << 25);
  const std::array<std::uint8_t, 4> texture = {0x00u, 0x02u, 0xFFu, 0xFEu};
  const moderngekko::GxDrawPacket texture_packet = {
      moderngekko::GxPrimitive::Points, 0u, 4u, 1u, texture};
  if (!loader.Decode(texture_packet, cp, &decoded) ||
      std::abs(decoded.vertices[0].texcoord[0][0] - 1.0f) > 0.0001f ||
      std::abs(decoded.vertices[0].texcoord[0][1] + 1.0f) > 0.0001f)
  {
    return 7;
  }
  return 0;
}
