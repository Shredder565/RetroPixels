// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// MPEG audio playback for the Digital Sound Board: plays an MPEG stream
// from the board's ROM (or a replacement track from Music.xml) through
// minimp3 (Pkgs/minimp3.h). One stream plays at a time.

#ifndef _MPEG_AUDIO_H_
#define _MPEG_AUDIO_H_

#include "Game.h"
#include <cstdint>
#include <string>

namespace MpegDec
{
  // Reads the replacement tracks for 'game' from the Music.xml file (if any).
  void LoadCustomTracks(const std::string &music_filepath, const Game &game);

  // Starts playing 'length' bytes at 'offset' in 'data'.
  void SetMemory(const uint8_t *data, int offset, int length, bool loop);
  // Moves the stream's start and end (a loop) without restarting it.
  void UpdateMemory(const uint8_t *data, int offset, int length, bool loop);
  // Byte position in the stream.
  int GetPosition();
  void SetPosition(int pos);
  // Decodes the next samples (silence when stopped or at the end).
  void DecodeAudio(int16_t *left, int16_t *right, int numStereoSamples);
  void Stop();
  // Whether a stream was ever set (also after Stop()).
  bool IsLoaded();
}

#endif
