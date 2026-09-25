#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace moderngekko
{
// Single-producer/single-consumer lock-free ring of interleaved stereo s16
// frames. The producer is the emulation thread (AudioSystem DmaSink); the
// consumer is the host audio callback thread. Both sides are wait-free and
// perform no allocation. Indices are free-running frame counters; the power-
// of-two capacity makes wraparound a mask operation, and the unsigned
// subtraction write - read is always the number of stored frames.
template <std::size_t CapacityFrames>
class SpscStereoRing
{
public:
  static_assert(CapacityFrames != 0 && (CapacityFrames & (CapacityFrames - 1)) == 0,
                "capacity must be a power of two");

  // Stores as many whole frames as fit; excess frames are dropped (a full
  // ring means the host clock fell behind by the whole buffer — dropping
  // beats blocking the emulation thread). Returns frames actually stored.
  std::size_t Push(std::span<const std::int16_t> interleaved_stereo)
  {
    const std::size_t frames = interleaved_stereo.size() / kChannels;
    const std::size_t write = m_write.load(std::memory_order_relaxed);
    const std::size_t read = m_read.load(std::memory_order_acquire);
    const std::size_t free_frames = CapacityFrames - (write - read);
    const std::size_t count = frames < free_frames ? frames : free_frames;
    for (std::size_t i = 0; i < count * kChannels; ++i)
      m_samples[((write * kChannels) + i) & kSampleMask] = interleaved_stereo[i];
    m_write.store(write + count, std::memory_order_release);
    return count;
  }

  // Copies up to max_frames into dst (which needs room for
  // max_frames * kChannels samples). Returns frames copied.
  std::size_t Pop(std::int16_t* dst, std::size_t max_frames)
  {
    const std::size_t read = m_read.load(std::memory_order_relaxed);
    const std::size_t write = m_write.load(std::memory_order_acquire);
    const std::size_t count = max_frames < (write - read) ? max_frames : (write - read);
    for (std::size_t i = 0; i < count * kChannels; ++i)
      dst[i] = m_samples[((read * kChannels) + i) & kSampleMask];
    m_read.store(read + count, std::memory_order_release);
    return count;
  }

  std::size_t StoredFrames() const
  {
    return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
  }

private:
  static constexpr std::size_t kChannels = 2;
  static constexpr std::size_t kSampleMask = CapacityFrames * kChannels - 1;

  std::array<std::int16_t, CapacityFrames * kChannels> m_samples{};
  std::atomic<std::size_t> m_write{0};
  std::atomic<std::size_t> m_read{0};
};
}  // namespace moderngekko
