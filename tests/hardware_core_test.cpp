#include "moderngekko/disc_interface.hpp"
#include "moderngekko/event_scheduler.hpp"
#include "moderngekko/mmio_bus.hpp"
#include "moderngekko/processor_interface.hpp"

#include <cstdint>
#include <vector>

int main()
{
  moderngekko::EventScheduler scheduler;
  std::vector<std::uint32_t> order;
  const auto cancelled = scheduler.ScheduleAfter(4u, [&](std::uint64_t) { order.push_back(9u); });
  scheduler.ScheduleAfter(5u, [&](std::uint64_t late) { order.push_back(1u + late); });
  scheduler.ScheduleAfter(5u, [&](std::uint64_t) { order.push_back(2u); });
  if (!scheduler.Cancel(cancelled))
    return 1;
  scheduler.Advance(7u);
  if (order != std::vector<std::uint32_t>{3u, 2u} || scheduler.GetTicks() != 7u)
    return 2;

  moderngekko::MmioBus bus;
  std::uint64_t storage = 0;
  if (!bus.Register(0x1000u, 8u,
                    [&](std::uint32_t, std::uint8_t) { return storage; },
                    [&](std::uint32_t, std::uint64_t value, std::uint8_t) { storage = value; }) ||
      bus.Register(0x1004u, 4u, {}, {}))
  {
    return 3;
  }
  if (!bus.Write(0x1000u, 0x12345678u, 4u))
    return 4;
  std::uint64_t read = 0;
  if (!bus.Read(0x1000u, 4u, &read) || read != 0x12345678u || bus.Read(0x2000u, 4u, &read))
    return 5;

  CPUState cpu{};
  moderngekko::MmioBus interrupt_bus;
  moderngekko::ProcessorInterface processor_interface(cpu);
  processor_interface.RegisterMmio(interrupt_bus);
  processor_interface.SetInterrupt(moderngekko::ProcessorInterface::AudioInterface);
  if (!interrupt_bus.Write(moderngekko::ProcessorInterface::MmioBase + 4u,
                           moderngekko::ProcessorInterface::AudioInterface, 4u) ||
      (cpu.exception & moderngekko::ProcessorInterface::ExternalInterrupt) == 0u)
  {
    return 6;
  }
  if (!interrupt_bus.Write(moderngekko::ProcessorInterface::MmioBase,
                           moderngekko::ProcessorInterface::AudioInterface, 4u) ||
      (cpu.exception & moderngekko::ProcessorInterface::ExternalInterrupt) != 0u)
  {
    return 7;
  }

  // DiscInterface has the same destructor-cancel contract: a pending
  // FinishTransfer event must never run on the dead object — it would write
  // status and raise the completion IRQ through the dangling `this`.
  cpu.exception &= ~moderngekko::ProcessorInterface::ExternalInterrupt;
  // Mask in the DI cause so a stale completion IRQ would raise the
  // external-interrupt exception (the earlier write left only AI masked).
  interrupt_bus.Write(moderngekko::ProcessorInterface::MmioBase + 4u,
                      moderngekko::ProcessorInterface::AudioInterface |
                          moderngekko::ProcessorInterface::DiscInterface,
                      4u);
  {
    struct ZeroDisc final : moderngekko::DiscSource
    {
      std::uint64_t GetSize() const override { return 0x1000u; }
      bool Read(std::uint64_t, std::span<std::uint8_t> output) override
      {
        for (std::uint8_t& byte : output)
          byte = 0;
        return true;
      }
    } zero_disc;
    moderngekko::AddressSpace memory;
    moderngekko::MmioBus disc_bus;
    {
      moderngekko::DiscInterface disc(memory, scheduler, processor_interface);
      disc.SetSource(&zero_disc);
      disc.RegisterMmio(disc_bus, false);
      // Enable the completion IRQ then arm an 0xA8 read — this schedules
      // FinishTransfer, which ~DiscInterface must cancel on scope exit.
      disc_bus.Write(moderngekko::DiscInterface::GameCubeMmioBase, 0x08u, 4u);
      disc_bus.Write(moderngekko::DiscInterface::GameCubeMmioBase + 0x08u, 0xA8000000u, 4u);
      disc_bus.Write(moderngekko::DiscInterface::GameCubeMmioBase + 0x0Cu, 0u, 4u);
      disc_bus.Write(moderngekko::DiscInterface::GameCubeMmioBase + 0x14u, 0x80000100u, 4u);
      disc_bus.Write(moderngekko::DiscInterface::GameCubeMmioBase + 0x18u, 0x20u, 4u);
      disc_bus.Write(moderngekko::DiscInterface::GameCubeMmioBase + 0x1Cu, 1u, 4u);
      // ~DiscInterface runs here with FinishTransfer still pending.
    }
    scheduler.Advance(64u);
    if ((cpu.exception & moderngekko::ProcessorInterface::ExternalInterrupt) != 0u)
      return 8;
  }

  return 0;
}
