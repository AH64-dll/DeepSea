#pragma once

#include "moderngekko/audio_system.hpp"

#include <cstdint>
#include <memory>

namespace moderngekko
{
// Host-side output backend for the emulated DSP DMA stream.
//
// Start() installs a DmaSink on the AudioSystem that feeds an SPSC ring and
// opens the platform playback device (SDL3 device stream: WASAPI shared mode
// on Windows, ALSA/Pulse elsewhere). The device runs at its native rate
// (48 kHz typical) with SDL converting from the ~21.3 kHz DSP block stream
// (see kSourceRateHz in src/audio/audio_output.cpp). The sink
// runs on the emulation thread; the SDL callback runs on an SDL-owned thread
// and must stay lock-free and allocation-free — both hold by construction.
class AudioOutput
{
public:
  virtual ~AudioOutput() = default;

  // Wires the sink and opens the playback device. Returns false when no
  // usable output exists; callers must keep running (samples drop, exactly
  // the pre-backend behavior). Repeated calls after success are no-ops.
  virtual bool Start(AudioSystem& audio_system) = 0;

  // Detaches the sink and closes the device. Safe when never started.
  virtual void Stop() = 0;

  // Sample-count clocking: total DSP frames handed to the ring (emulation
  // thread view) and frames delivered into the host stream (callback view).
  virtual std::uint64_t ProducedFrames() const = 0;
  virtual std::uint64_t ConsumedFrames() const = 0;
};

// Backend for this build: SDL3 device stream when linked against SDL3, a
// graceful no-output stub otherwise.
std::unique_ptr<AudioOutput> CreateDefaultAudioOutput();
}  // namespace moderngekko
