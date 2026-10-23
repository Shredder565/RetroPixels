// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Sound output to the host. The sound board hands over one frame of the
// Model 3's four speaker channels at a time; the host side (SDL/Audio.cpp)
// mixes them for the speakers the player has and keeps a short queue that
// the sound card drains.

#ifndef INCLUDED_AUDIO_H
#define INCLUDED_AUDIO_H

#include "Types.h"
#include "Util/NewConfig.h"
#include "Game.h"

typedef void (*AudioCallbackFPtr)(void *data);

// Called from the sound card's thread whenever the queue has room for more
// frames (the emulator uses it to run its sound thread ahead).
extern void SetAudioCallback(AudioCallbackFPtr callback, void *data);

// While disabled the sound card plays silence (the queue keeps moving).
extern void SetAudioEnabled(bool enabled);

// How the game's cabinet wires its channels (Games.xml); call before OpenAudio.
extern void SetAudioType(Game::AudioTypes type);

// Starts playback with the settings in config (NbSoundChannels, SoundFreq,
// BalanceLeftRight, BalanceFrontRear).
extern Result OpenAudio(const Util::Config::Node &config);

// Queues one frame of sound: numSamples samples for front left/right and rear
// left/right. Returns true when the queue is close to full.
extern bool OutputAudio(unsigned numSamples, const float *leftFrontBuffer, const float *rightFrontBuffer, const float *leftRearBuffer, const float *rightRearBuffer, bool flipStereo);

// Stops playback.
extern void CloseAudio();

#endif // INCLUDED_AUDIO_H
