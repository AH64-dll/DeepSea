// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// (diag note, lreloc-p5): MODERNGEKKO_DIAG_DI env-gated DI lifecycle trace,
// default OFF and zero-cost when unset.

#include "moderngekko/disc_interface.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace moderngekko
{
namespace
{
constexpr std::uint32_t TransferCompleteMask = 1u << 3;
constexpr std::uint32_t TransferComplete = 1u << 4;
const bool di_diag = std::getenv("MODERNGEKKO_DIAG_DI") != nullptr;
}  // namespace

DiscInterface::DiscInterface(AddressSpace& memory, EventScheduler& scheduler,
                             ProcessorInterface& processor_interface)
    : m_memory(memory), m_scheduler(scheduler), m_processor_interface(processor_interface)
{
}

DiscInterface::~DiscInterface()
{
  if (m_transfer_event != 0)
    m_scheduler.Cancel(m_transfer_event);
}

void DiscInterface::SetSource(DiscSource* source)
{
  m_source = source;
  m_cover = source == nullptr ? 1u : 0u;
}

void DiscInterface::RegisterMmio(MmioBus& bus, bool is_wii)
{
  const std::uint32_t base = is_wii ? WiiMmioBase : GameCubeMmioBase;
  for (std::uint32_t offset = 0; offset <= 0x24u; offset += 4u)
  {
    bus.Register(base + offset, 4u,
                 [this, offset](std::uint32_t, std::uint8_t) { return ReadRegister(offset); },
                 [this, offset](std::uint32_t, std::uint64_t value, std::uint8_t) {
                   WriteRegister(offset, static_cast<std::uint32_t>(value));
                 });
  }
  if (di_diag)
    std::fprintf(stderr, "[diag-di] registered mmio\n");
}

void DiscInterface::Reset()
{
  // A transfer in flight at reset time is aborted: without cancelling the
  // event, FinishTransfer would still run and raise a completion interrupt
  // on the freshly reset registers.
  if (m_transfer_event != 0)
  {
    m_scheduler.Cancel(m_transfer_event);
    m_transfer_event = 0;
  }
  m_status = 0;
  m_cover = m_source == nullptr ? 1u : 0u;
  std::ranges::fill(m_command, 0u);
  m_dma_address = 0;
  m_dma_length = 0;
  m_dma_control = 0;
  m_immediate = 0;
  m_last_irq_level = false;
  UpdateInterrupt();
}

std::uint32_t DiscInterface::ReadRegister(std::uint32_t offset) const
{
  switch (offset)
  {
  case 0x00: return m_status;
  case 0x04: return m_cover;
  case 0x08: return m_command[0];
  case 0x0C: return m_command[1];
  case 0x10: return m_command[2];
  case 0x14: return m_dma_address;
  case 0x18: return m_dma_length;
  case 0x1C: return m_dma_control;
  case 0x20: return m_immediate;
  case 0x24: return 1u;
  default: return 0;
  }
}

void DiscInterface::WriteRegister(std::uint32_t offset, std::uint32_t value)
{
  if (di_diag && ((offset >= 0x08u && offset <= 0x1Cu) || offset == 0x00u))
    std::fprintf(stderr, "[diag-di] w reg=0x%02X val=0x%08X\n", offset, value);
  switch (offset)
  {
  case 0x00:
    m_status = (m_status & ~(value & 0x54u)) | (value & 0x2Au);
    UpdateInterrupt();
    break;
  case 0x08: m_command[0] = value; break;
  case 0x0C: m_command[1] = value; break;
  case 0x10: m_command[2] = value; break;
  case 0x14: m_dma_address = value & 0x03FFFFE0u; break;
  case 0x18: m_dma_length = value & ~0x1Fu; break;
  case 0x1C:
    m_dma_control = value & 7u;
    if ((m_dma_control & 1u) != 0u)
      StartTransfer();
    break;
  case 0x20: m_immediate = value; break;
  default: break;
  }
}

void DiscInterface::StartTransfer()
{
  if (di_diag)
    std::fprintf(stderr, "[diag-di] start cmd=%08X %08X %08X dma=0x%08X len=%u\n",
                 m_command[0], m_command[1], m_command[2], m_dma_address, m_dma_length);
  // A second start while a transfer is pending aborts the first: without the
  // cancel the stale event still fires FinishTransfer mid-transfer and raises
  // a completion IRQ for work that was superseded.
  if (m_transfer_event != 0)
    m_scheduler.Cancel(m_transfer_event);
  m_transfer_event =
      m_scheduler.ScheduleAfter(1u, [this](std::uint64_t) { FinishTransfer(); });
}

void DiscInterface::FinishTransfer()
{
  m_transfer_event = 0;
  const std::uint8_t command = static_cast<std::uint8_t>(m_command[0] >> 24);
  bool success = false;
  if (command == 0xA8u && m_source != nullptr)
  {
    const std::uint64_t offset = static_cast<std::uint64_t>(m_command[1]) << 2;
    std::uint8_t* output = m_memory.Resolve(m_dma_address, m_dma_length);
    success = output != nullptr && offset <= m_source->GetSize() &&
              m_dma_length <= m_source->GetSize() - offset &&
              m_source->Read(offset, {output, m_dma_length});
    if (di_diag)
      std::fprintf(stderr, "[diag-di] finish a8 ok=%d out=0x%08X len=%u off=%llu\n", success ? 1 : 0,
                   m_dma_address, m_dma_length, (unsigned long long)offset);
  }
  else if (di_diag)
  {
    std::fprintf(stderr, "[diag-di] finish cmd=%02X success-path-skipped\n", command);
  }

  m_dma_control &= ~1u;
  m_dma_length = 0;
  m_status |= success ? TransferComplete : 0x4u;
  UpdateInterrupt();
}

void DiscInterface::UpdateInterrupt()
{
  const bool level = (m_status & TransferCompleteMask) != 0u &&
                     (m_status & TransferComplete) != 0u;
  // Member, not function-static: a static would share edge state across
  // instances and survive Reset(), misreporting the first post-reset edge.
  if (di_diag && level != m_last_irq_level)
    std::fprintf(stderr, "[diag-di] irq level=%d\n", level ? 1 : 0);
  m_last_irq_level = level;
  m_processor_interface.SetInterrupt(ProcessorInterface::DiscInterface, level);
}
}  // namespace moderngekko
