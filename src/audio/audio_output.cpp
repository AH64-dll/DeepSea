#include "moderngekko/audio_output.hpp"

#include "moderngekko/spsc_audio_ring.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef MODERNGEKKO_HAVE_SDL_AUDIO
#include <SDL3/SDL.h>
#endif

namespace moderngekko
{
namespace
{
// The emulated AI DMA chain emits one 32-byte DSP block (16 s16 samples =
// 8 interleaved stereo frames) every AudioDmaPeriod ticks. Scheduler ticks
// run at 323.712 MHz (AiCyclesPerSample=10116 <-> 32 kHz AI counter in
// audio_system.cpp), so the real production cadence is
// 323.712e6 * 8 / 121500 ~= 21314 stereo frames/s -- NOT the 32 kHz quoted
// in phase3-win-audio-audit.md (that doc miscounted samples as frames).
// The SDL source format declares this measured rate so the host hears the
// emulated mix at the right pitch.
constexpr int kSourceRateHz = 21314;
constexpr std::size_t kFramesPerBlock = 8;
constexpr std::size_t kFillTargetBlocks = 3;
constexpr std::size_t kFillTargetFrames = kFramesPerBlock * kFillTargetBlocks;

// 16 blocks = 128 frames ~= 6 ms @ the production rate. Capacity >= 8 blocks
// is the design floor; steady-state depth is set by the fill target plus the
// +/-500 ppm drift trim, not by the capacity.
constexpr std::size_t kRingBlocks = 16;
constexpr std::size_t kMaxCallbackFrames = 2048;

// Drift correction: SDL converts source rate -> device rate at a nominal
// ratio of 1.0. Trim that ratio toward keeping the ring at the 3-block fill
// target so clock drift between emulation time and the device clock is
// absorbed by resampling instead of depth growth. Clamped to +/-500 ppm.
constexpr double kDriftGainPerFrame = 2e-6;
constexpr double kMaxDriftRatioOffset = 5e-4;

// Pacing-stats gate (default OFF): MODERNGEKKO_PACING_STATS=1 enables the
// throttled stderr stats lines in SdlAudioOutput and GxStateBackend. Unset,
// empty, or "0" keeps both paths at a single predictable branch.
[[maybe_unused]] bool PacingStatsEnabled()
{
  static const bool enabled = [] {
    const char* raw = std::getenv("MODERNGEKKO_PACING_STATS");
    return raw != nullptr && raw[0] != '\0' && !(raw[0] == '0' && raw[1] == '\0');
  }();
  return enabled;
}


#ifdef MODERNGEKKO_HAVE_SDL_AUDIO
class SdlAudioOutput final : public AudioOutput
{
public:
  ~SdlAudioOutput() override { Stop(); }

  bool Start(AudioSystem& audio_system) override
  {
    if (m_stream != nullptr)
      return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
      std::fprintf(stderr, "audio: SDL audio init failed (%s); running silent\n",
                   SDL_GetError());
      return false;
    }
    m_subsystem_initialized = true;

    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSourceRateHz};
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                         &SdlAudioOutput::CallbackTrampoline, this);
    if (m_stream == nullptr)
    {
      std::fprintf(stderr, "audio: no playback device (%s); running silent\n",
                   SDL_GetError());
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      m_subsystem_initialized = false;
      return false;
    }

    // Opened paused by SDL; resume once the 3-block fill target is reached.
    m_device_started = false;
    m_audio_system = &audio_system;
    m_pacing_stats = PacingStatsEnabled();
    m_dropped.store(0, std::memory_order_relaxed);
    m_underruns.store(0, std::memory_order_relaxed);
    m_callbacks.store(0, std::memory_order_relaxed);
    audio_system.SetDmaSink([this](std::span<const std::int16_t> block) {
      const std::size_t pushed = m_ring.Push(block);
      m_produced.fetch_add(pushed, std::memory_order_relaxed);
      // Drop-on-full by design (never block the emulation thread); count
      // the excess only so the stats line can report push-side pressure.
      m_dropped.fetch_add(block.size() / 2 - pushed, std::memory_order_relaxed);
      if (!m_device_started && m_ring.StoredFrames() >= kFillTargetFrames)
        m_device_started = SDL_ResumeAudioStreamDevice(m_stream);
    });
    return true;
  }

  void Stop() override
  {
    if (m_audio_system != nullptr)
    {
      m_audio_system->SetDmaSink({});
      m_audio_system = nullptr;
    }
    if (m_stream != nullptr)
    {
      SDL_DestroyAudioStream(m_stream);
      m_stream = nullptr;
    }
    m_device_started = false;
    if (m_subsystem_initialized)
    {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      m_subsystem_initialized = false;
    }
  }

  std::uint64_t ProducedFrames() const override
  {
    return m_produced.load(std::memory_order_acquire);
  }

  std::uint64_t ConsumedFrames() const override
  {
    return m_consumed.load(std::memory_order_acquire);
  }

private:
  static void SDLCALL CallbackTrampoline(void* userdata, SDL_AudioStream* stream,
                                         int additional_amount, int /*total_amount*/)
  {
    static_cast<SdlAudioOutput*>(userdata)->Consume(stream, additional_amount);
  }

  // SDL callback thread. Pulls from the SPSC ring into preallocated storage,
  // zero-fills underruns, and trims the conversion ratio so the ring hovers
  // at the fill target. No allocation, no locks.
  void Consume(SDL_AudioStream* stream, int additional_amount)
  {
    const std::size_t requested =
        std::min<std::size_t>(static_cast<std::size_t>(additional_amount) /
                                  (2 * sizeof(std::int16_t)),
                              kMaxCallbackFrames);
    const std::size_t got = m_ring.Pop(m_scratch.data(), requested);
    if (got > 0)
    {
      SDL_PutAudioStreamData(stream, m_scratch.data(),
                             static_cast<int>(got * 2 * sizeof(std::int16_t)));
    }
    if (got < requested)
    {
      std::memset(m_scratch.data(), 0, (requested - got) * 2 * sizeof(std::int16_t));
      SDL_PutAudioStreamData(stream, m_scratch.data(),
                             static_cast<int>((requested - got) * 2 * sizeof(std::int16_t)));
    }
    m_consumed.fetch_add(got, std::memory_order_relaxed);
    if (got < requested)
      m_underruns.fetch_add(requested - got, std::memory_order_relaxed);
    // Throttled stats line (default OFF): produced/consumed totals plus
    // drop/underrun pressure counters and current ring depth. One line per
    // 600 callbacks keeps the SDL callback thread free of log spam.
    const std::uint64_t callbacks = m_callbacks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m_pacing_stats && callbacks % 600 == 0)
    {
      const std::uint64_t produced = ProducedFrames();
      const std::uint64_t consumed = ConsumedFrames();
      std::fprintf(stderr,
                   "pacing: audio produced=%llu consumed=%llu dropped=%llu underrun=%llu depth=%lld\n",
                   static_cast<unsigned long long>(produced),
                   static_cast<unsigned long long>(consumed),
                   static_cast<unsigned long long>(m_dropped.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(m_underruns.load(std::memory_order_relaxed)),
                   static_cast<long long>(produced - consumed));
    }

    const double depth = static_cast<double>(ProducedFrames() - ConsumedFrames());
    const double offset = std::clamp((depth - static_cast<double>(kFillTargetFrames)) *
                                         kDriftGainPerFrame,
                                     -kMaxDriftRatioOffset, kMaxDriftRatioOffset);
    SDL_SetAudioStreamFrequencyRatio(stream, static_cast<float>(1.0 + offset));
  }
  SpscStereoRing<kRingBlocks * kFramesPerBlock> m_ring;
  std::array<std::int16_t, kMaxCallbackFrames * 2> m_scratch{};
  std::atomic<std::uint64_t> m_produced{0};
  std::atomic<std::uint64_t> m_consumed{0};
  // Pacing diagnostics (MODERNGEKKO_PACING_STATS, default OFF): frames the
  // SPSC ring had to drop because the host clock fell a whole buffer behind
  // (push-side pressure, never blocks the emulation thread), frames the
  // callback zero-filled because the ring ran dry (pull-side starvation),
  // and a callback count used to throttle the stats line. Windows runs
  // sample these alongside VI Hz and window FPS; sustained drops mean the
  // guest produces faster than the host drains, sustained underruns mean
  // the guest is too slow to keep the buffer fed.
  std::atomic<std::uint64_t> m_dropped{0};
  std::atomic<std::uint64_t> m_underruns{0};
  std::atomic<std::uint64_t> m_callbacks{0};
  bool m_pacing_stats = false;
  AudioSystem* m_audio_system = nullptr;
  SDL_AudioStream* m_stream = nullptr;
  bool m_subsystem_initialized = false;
  bool m_device_started = false;
};

#endif  // MODERNGEKKO_HAVE_SDL_AUDIO

class NullAudioOutput final : public AudioOutput
{
public:
  bool Start(AudioSystem&) override { return false; }
  void Stop() override {}
  std::uint64_t ProducedFrames() const override { return 0; }
  std::uint64_t ConsumedFrames() const override { return 0; }
};
}  // namespace

std::unique_ptr<AudioOutput> CreateDefaultAudioOutput()
{
#ifdef MODERNGEKKO_HAVE_SDL_AUDIO
  return std::make_unique<SdlAudioOutput>();
#else
  return std::make_unique<NullAudioOutput>();
#endif
}
}  // namespace moderngekko
