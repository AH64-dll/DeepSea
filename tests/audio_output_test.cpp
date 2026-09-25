#include "moderngekko/audio_output.hpp"
#include "moderngekko/spsc_audio_ring.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace
{
int TestRingContract()
{
  moderngekko::SpscStereoRing<8> ring;

  // Empty ring pops nothing.
  std::array<std::int16_t, 8> out{};
  if (ring.Pop(out.data(), 4) != 0 || ring.StoredFrames() != 0)
    return 1;

  // Push fills; Pop returns data in order across the wrap boundary.
  std::vector<std::int16_t> block(2 * 6);
  for (std::size_t i = 0; i < block.size(); ++i)
    block[i] = static_cast<std::int16_t>(i);
  if (ring.Push(block) != 6 || ring.StoredFrames() != 6)
    return 2;
  if (ring.Pop(out.data(), 3) != 3)
    return 3;
  if (out[0] != 0 || out[1] != 1 || out[2] != 2 || out[3] != 3 || out[4] != 4 ||
      out[5] != 5)
    return 4;
  if (ring.StoredFrames() != 3)
    return 5;

  // Wraparound: indices pass capacity while ordering is preserved.
  // Drain the three queued frames (FIFO) before the wraparound phase.
  if (ring.Pop(out.data(), 3) != 3 || out[0] != 6 || out[1] != 7 || out[2] != 8 ||
      out[3] != 9 || out[4] != 10 || out[5] != 11)
    return 5;

  std::int16_t pattern = 100;
  for (int round = 0; round < 7; ++round)
  {
    std::vector<std::int16_t> push(2 * 1, pattern);
    if (ring.Push(push) != 1)
      return 6;
    std::array<std::int16_t, 2> one{};
    if (ring.Pop(one.data(), 1) != 1 || one[0] != pattern || one[1] != pattern)
      return 7;
    pattern = static_cast<std::int16_t>(pattern + 2);
  }

  // Overflow drops the excess instead of blocking or wrapping into live data.
  std::vector<std::int16_t> big(2 * 10, 7);
  const std::size_t stored = ring.Push(big);
  if (stored > 8 || ring.StoredFrames() != stored)
    return 8;

  return 0;
}

int TestBackendWiring()
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

  auto output = moderngekko::CreateDefaultAudioOutput();
  if (!output)
    return 1;

  // A missing device must degrade gracefully: Start returns false and the
  // counters stay at zero, but nothing may crash or throw.
  const bool started = output->Start(audio);
  if (!started && (output->ProducedFrames() != 0 || output->ConsumedFrames() != 0))
    return 2;

  if (started)
  {
    // Drive one DMA block through the DSP MMIO path exactly like
    // audio_system_test does; it must land in the backend's sample clock.
    bus.Write(moderngekko::ProcessorInterface::MmioBase + 4u,
              moderngekko::ProcessorInterface::DspInterface, 4u);
    bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x0Au, 0x10u, 2u);
    bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x30u, 0u, 2u);
    bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x32u, 0x1000u, 2u);
    bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x36u, 0x8001u, 2u);
    scheduler.Advance(200u);

    if (output->ProducedFrames() != 8u)
    {
      output->Stop();
      return 3;
    }
  }

  output->Stop();

  // After Stop the sink is detached: another block must not be counted.
  bus.Write(moderngekko::AudioSystem::DspMmioBase + 0x36u, 0x8001u, 2u);
  scheduler.Advance(200u);
  if (started && output->ProducedFrames() > 8u)
    return 4;


  return 0;
}
}  // namespace

int main()
{
  int result = TestRingContract();
  if (result != 0)
    return result;
  return TestBackendWiring();
}
