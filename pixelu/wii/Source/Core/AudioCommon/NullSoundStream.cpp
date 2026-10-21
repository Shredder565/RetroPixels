// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/NullSoundStream.h"

bool NullSound::Init()
{
  // An output rate of 0 tells the mixer to drop incoming samples.
  GetMixer()->SetSampleRate(0);
  return true;
}

bool NullSound::SetRunning(bool)
{
  return true;
}

void NullSound::SetVolume(int)
{
}
