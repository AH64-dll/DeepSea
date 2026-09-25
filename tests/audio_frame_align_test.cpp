// AU-1 Option A regression: MODERNGEKKO_AI_FRAME_ALIGN=1 snaps AI interrupt
// due times to the nearest guest video-frame boundary (10,790,400 ticks at
// 323.712 MHz). Default OFF keeps free-running elapsed-cycle timing.
#include "moderngekko/audio_system.hpp"

#include <cstdlib>
#include <cstdint>
#include <vector>

namespace
{
constexpr std::uint64_t GuestFrameCycles = 10790400u;
constexpr std::uint64_t AiCyclesPerSample = 10116u;
constexpr std::uint64_t RawDelay = 1000u * AiCyclesPerSample;

int RunCase(bool aligned)
{
  moderngekko::AddressSpace memory;
  CPUState cpu{};
  moderngekko::EventScheduler scheduler;
  moderngekko::ProcessorInterface processor_interface(cpu);
  moderngekko::MmioBus bus;
  processor_interface.RegisterMmio(bus);
  moderngekko::AudioSystem audio(memory, scheduler, processor_interface);
  audio.RegisterMmio(bus, false);

  bus.Write(moderngekko::ProcessorInterface::MmioBase + 4u,
            moderngekko::ProcessorInterface::AudioInterface, 4u);
  bus.Write(moderngekko::AudioSystem::AiMmioBase + 0x0Cu, 1000u, 4u);
  bus.Write(moderngekko::AudioSystem::AiMmioBase, 0x5u, 4u);

  // Advance to the raw (unsnapped) due time. With alignment OFF the
  // interrupt has fired; with it ON it is deferred to the frame boundary.
  scheduler.Advance(RawDelay);
  std::uint64_t control = 0;
  bus.Read(moderngekko::AudioSystem::AiMmioBase, 4u, &control);
  const bool fired_at_raw_due = (control & 8u) != 0u;
  if (aligned == fired_at_raw_due)
    return 1;

  scheduler.Advance(GuestFrameCycles - RawDelay);
  control = 0;
  bus.Read(moderngekko::AudioSystem::AiMmioBase, 4u, &control);
  if ((control & 8u) == 0u)
    return 2;

  // The interrupt must have landed exactly on the boundary tick.
  if (aligned && scheduler.GetTicks() != GuestFrameCycles)
    return 3;
  return 0;
}
}  // namespace

int main()
{
  // Default OFF first (free-running timing), then ON via the env knob.
  if (const int rc = RunCase(false); rc != 0)
    return rc;
#ifdef _WIN32
  // UCRT has no setenv; _putenv_s copies the value string.
  if (_putenv_s("MODERNGEKKO_AI_FRAME_ALIGN", "1") != 0)
    return 4;
#else
  if (::setenv("MODERNGEKKO_AI_FRAME_ALIGN", "1", 1) != 0)
    return 4;
#endif
  if (const int rc = RunCase(true); rc != 0)
    return 10 + rc;

  return 0;
}
