// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "AudioCommon/Mixer.h"
#include "Common/CommonTypes.h"

// Base class of the host sound backends. Every backend owns a mixer, created for 48 kHz output.
class SoundStream
{
public:
  SoundStream() : m_mixer(std::make_unique<Mixer>(48000)) {}
  virtual ~SoundStream() = default;

  // Backends hide this with their own check; the base class is never usable on its own.
  static bool IsValid() { return false; }

  Mixer* GetMixer() const { return m_mixer.get(); }

  virtual bool Init() { return false; }
  virtual void SetVolume(int) {}
  // Starts or stops output; returns false on failure.
  virtual bool SetRunning(bool) { return false; }

protected:
  std::unique_ptr<Mixer> m_mixer;
};
