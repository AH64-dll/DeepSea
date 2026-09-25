#include "moderngekko/audio_system.hpp"

#include <array>
#include <cstdint>
#include <vector>

int main()
{
  moderngekko::AddressSpace memory;
  CPUState cpu{};
  moderngekko::EventScheduler scheduler;
  moderngekko::ProcessorInterface processor_interface(cpu);
  moderngekko::MmioBus bus;
  processor_interface.RegisterMmio(bus);
  moderngekko::AudioSystem audio(memory, scheduler, processor_interface);
  audio.RegisterMmio(bus, false);

  for (std::uint32_t i = 0; i < 32u; ++i)
    memory.Write8(0x1000u + i, static_cast<std::uint8_t>(i));
  std::vector<std::int16_t> captured;
  audio.SetDmaSink([&](std::span<const std::int16_t> samples) {
    captured.assign(samples.begin(), samples.end());
  });

  bus.Write(moderngekko::ProcessorInterface::MmioBase + 4u,
            moderngekko::ProcessorInterface::DspInterface, 4u);
  bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x0Au, 0x10u, 2u);
  bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x30u, 0u, 2u);
  bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x32u, 0x1000u, 2u);
  bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x36u, 0x8001u, 2u);
  scheduler.Advance(200u);

  if (captured.size() != 16u || captured.front() != 0x0001 || captured.back() != 0x1E1F ||
      (cpu.exception & moderngekko::ProcessorInterface::ExternalInterrupt) == 0u)
  {
    return 1;
  }

  std::uint64_t blocks_left = 1;
  if (!bus.Read(moderngekko::AudioSystem::DspMmioBase + 0x3Au, 2u, &blocks_left) ||
      blocks_left != 0u)
  {
    return 2;
  }

  bus.Write(moderngekko::ProcessorInterface::MmioBase + 4u,
            moderngekko::ProcessorInterface::AudioInterface, 4u);
  bus.Write(moderngekko::AudioSystem::AiMmioBase + 0x0Cu, 2u, 4u);
  bus.Write(moderngekko::AudioSystem::AiMmioBase, 0x5u, 4u);
  scheduler.Advance(20232u);
  std::uint64_t control = 0;
  std::uint64_t counter = 0;
  bus.Read(moderngekko::AudioSystem::AiMmioBase, 4u, &control);
  bus.Read(moderngekko::AudioSystem::AiMmioBase + 8u, 4u, &counter);
  if ((control & 8u) == 0u || counter < 2u ||
      (cpu.exception & moderngekko::ProcessorInterface::ExternalInterrupt) == 0u)
  {
    return 3;
  }

  // Events scheduled by an AudioSystem must not outlive it: destroy one with
  // both the DMA and AI callbacks still pending, then drive the scheduler
  // far past their deadlines. A stale callback on the dead object would
  // still push samples to the sink and raise the PI exception — the scoped
  // system's destructor must have cancelled both events instead. The main
  // system is Reset first: its DMA block self-reschedules and would
  // legitimately keep firing during the advance.
  audio.Reset();
  cpu.exception &= ~moderngekko::ProcessorInterface::ExternalInterrupt;
  captured.clear();
  {
    moderngekko::MmioBus scoped_bus;
    {
      moderngekko::AudioSystem scoped_audio(memory, scheduler, processor_interface);
      scoped_audio.RegisterMmio(scoped_bus, false);
      scoped_audio.SetDmaSink([&](std::span<const std::int16_t> samples) {
        captured.assign(samples.begin(), samples.end());
      });
      scoped_bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x0Au, 0x10u, 2u);
      scoped_bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x30u, 0u, 2u);
      scoped_bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x32u, 0x1000u, 2u);
      scoped_bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x36u, 0x8001u, 2u);
      scoped_bus.Write(moderngekko::AudioSystem::AiMmioBase + 0x0Cu, 2u, 4u);
      scoped_bus.Write(moderngekko::AudioSystem::AiMmioBase, 0x5u, 4u);
      // ~AudioSystem runs here while both events are still pending.
    }
    scheduler.Advance(200000u);
  }
  if (!captured.empty() ||
      (cpu.exception & moderngekko::ProcessorInterface::ExternalInterrupt) != 0u)
  {
    return 4;
  }

  return 0;
}
