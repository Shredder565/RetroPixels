// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "AudioCommon/SoundStream.h"

// The "No Audio Output" backend: always available, plays nothing.
class NullSound final : public SoundStream
{
public:
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }
};
