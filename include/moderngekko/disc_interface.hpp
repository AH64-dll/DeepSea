#pragma once

#include "moderngekko/address_space.hpp"
#include "moderngekko/event_scheduler.hpp"
#include "moderngekko/mmio_bus.hpp"
#include "moderngekko/processor_interface.hpp"

#include <cstdint>
#include <span>

namespace moderngekko
{
class DiscSource
{
public:
  virtual ~DiscSource() = default;
  virtual std::uint64_t GetSize() const = 0;
  virtual bool Read(std::uint64_t offset, std::span<std::uint8_t> output) = 0;
};

class DiscInterface final
{
public:
  static constexpr std::uint32_t GameCubeMmioBase = 0x0C006000u;
  static constexpr std::uint32_t WiiMmioBase = 0x0D006000u;

  DiscInterface(AddressSpace& memory, EventScheduler& scheduler,
                ProcessorInterface& processor_interface);
  ~DiscInterface();

  void SetSource(DiscSource* source);
  void RegisterMmio(MmioBus& bus, bool is_wii);
  void Reset();

private:
  std::uint32_t ReadRegister(std::uint32_t offset) const;
  void WriteRegister(std::uint32_t offset, std::uint32_t value);
  void StartTransfer();
  void FinishTransfer();
  void UpdateInterrupt();

  AddressSpace& m_memory;
  EventScheduler& m_scheduler;
  ProcessorInterface& m_processor_interface;
  DiscSource* m_source = nullptr;
  std::uint32_t m_status = 0;
  std::uint32_t m_cover = 1;
  std::uint32_t m_command[3]{};
  std::uint32_t m_dma_address = 0;
  std::uint32_t m_dma_length = 0;
  std::uint32_t m_dma_control = 0;
  std::uint32_t m_immediate = 0;
  // Outstanding FinishTransfer event; Reset() and teardown must cancel it so a
  // phantom completion cannot set status/IRQ after the interface is reset or
  // destroyed. 0 = none pending.
  EventScheduler::EventId m_transfer_event = 0;
  bool m_last_irq_level = false;  // MODERNGEKKO_DIAG_DI edge detection
};
}
