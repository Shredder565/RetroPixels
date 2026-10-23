// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Sound output through SDL (OSD/Audio.h).
//
// The Model 3 makes four channels of 44.1 kHz sound: the two SCSP chips
// usually feed the front (SCSP1) and rear (SCSP2) speaker pairs, with the
// MPEG music board mixed into one of them. How a cabinet is wired differs
// from game to game, so Games.xml names a layout (Game::AudioTypes) and the
// mix for the player's speakers (1, 2 or 4 channels) is made here.
//
// Mixed frames go into a ring of 16-bit samples, about 200 ms long, that
// SDL's audio thread drains. The ring starts half full. If the sound card
// catches up with the emulator (under-run) the read position jumps back one
// frame past the newest data, replaying recent sound rather than leaving a
// gap; if the emulator gets too far ahead (over-run) its frame is dropped.
// OutputAudio() and the drain callback both report "nearly full" so the
// emulator can pace its sound thread.

#include "Audio.h"

#include "Supermodel.h"
#include "OSD/RetroPixels.h"
#include "SDLIncludes.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace
{
  constexpr int kRate = 44100;          // Model 3 output rate
  constexpr int kModelChannels = 4;     // FL, FR, RL, RR
  constexpr float kMinFrameHz = 45.0f;  // SoundFreq is held to this range
  constexpr float kMaxFrameHz = 75.0f;
  constexpr unsigned kRingPercent = 20; // ring length: 20% of a second
  constexpr unsigned kCallbackSamples = 512;

  // Most samples one OutputAudio() call can bring (a frame at the lowest rate).
  constexpr int kMaxFrameSamples = int(kRate / kMinFrameHz);

  Game::AudioTypes s_layout = Game::STEREO_LR;
  int s_hostChannels = kModelChannels;

  // Per-speaker gains from the balance settings.
  float s_gainFL = 1.0f, s_gainFR = 1.0f, s_gainRL = 1.0f, s_gainRR = 1.0f;

  bool s_enabled = true;
  AudioCallbackFPtr s_callback = nullptr;
  void *s_callbackData = nullptr;

  // The ring. Positions are byte offsets. readPos is where the sound card
  // reads next, writePos where the next frame goes; writeAhead says writePos
  // has wrapped past the end while readPos hasn't yet, i.e. the data really
  // ends at writePos + size.
  struct Ring
  {
    INT8 *data = nullptr;
    UINT32 size = 0;
    UINT32 readPos = 0;
    UINT32 writePos = 0;
    bool writeAhead = false;
    UINT32 frameBytes = 0;      // bytes in one emulated frame
    UINT32 sampleBytes = 0;     // bytes in one sample of all host channels
    int frameSamples = 0;       // samples in one emulated frame
    unsigned underRuns = 0;
    unsigned overRuns = 0;

    UINT32 Wrap(UINT32 pos) const { return pos % size; }

    // Copies len bytes out of the ring starting at pos (wrapping at the end).
    void CopyOut(UINT32 pos, Uint8 *dst, UINT32 len) const
    {
      UINT32 first = std::min(len, size - pos);
      memcpy(dst, data + pos, first);
      if (len > first)
        memcpy(dst + first, data, len - first);
    }

    void CopyIn(UINT32 pos, const void *src, UINT32 len)
    {
      UINT32 first = std::min(len, size - pos);
      memcpy(data + pos, src, first);
      if (len > first)
        memcpy(data, static_cast<const UINT8 *>(src) + first, len - first);
    }
  };

  Ring s_ring;

  inline INT16 ToSample(float x)
  {
    INT32 v = INT32(x);
    if (v > 32767)
      v = 32767;
    else if (v < -32768)
      v = -32768;
    return INT16(v);
  }

  inline float Average(float a, float b)
  {
    return (a + b) * 0.5f;
  }

  inline INT16 AverageSample(float a, float b)
  {
    return ToSample(Average(a, b));
  }

  // Layouts that put the right channel first.
  bool LayoutIsReversed(Game::AudioTypes layout)
  {
    return layout == Game::STEREO_RL || layout == Game::QUAD_1_FRL_2_RRL || layout == Game::QUAD_1_RRL_2_FRL;
  }

  // Mixes numSamples samples of the four Model 3 channels for the host's
  // speakers into out (s_hostChannels values per sample).
  void Mix(unsigned numSamples, const float *fl, const float *fr, const float *rl, const float *rr, INT16 *out, bool swap)
  {
    if (s_hostChannels == 1)
    {
      for (unsigned i = 0; i < numSamples; i++)
        *out++ = AverageSample(Average(fl[i] * s_gainFL, fr[i] * s_gainFR), Average(rl[i] * s_gainRL, rr[i] * s_gainRR));
      return;
    }

    if (LayoutIsReversed(s_layout))
      swap = !swap;

    if (s_hostChannels == 2)
    {
      // Front and rear of each side together.
      for (unsigned i = 0; i < numSamples; i++)
      {
        INT16 left = AverageSample(fl[i] * s_gainFL, rl[i] * s_gainRL);
        INT16 right = AverageSample(fr[i] * s_gainFR, rr[i] * s_gainRR);
        *out++ = swap ? right : left;
        *out++ = swap ? left : right;
      }
      return;
    }

    if (s_hostChannels != 4)
    {
      memset(out, 0, numSamples * s_hostChannels * sizeof(INT16));
      return;
    }

    for (unsigned i = 0; i < numSamples; i++)
    {
      float a = fl[i] * s_gainFL, b = fr[i] * s_gainFR;    // first chip's pair
      float c = rl[i] * s_gainRL, d = rr[i] * s_gainRR;    // second chip's pair
      INT16 o[4];
      switch (s_layout)
      {
      case Game::MONO:
        o[0] = o[1] = o[2] = o[3] = AverageSample(Average(a, b), Average(c, d));
        break;
      case Game::STEREO_LR:
      case Game::STEREO_RL:
      {
        // Each chip's pair folded into one side, on front and rear alike.
        INT16 x = AverageSample(a, b), y = AverageSample(c, d);
        o[0] = o[2] = swap ? y : x;
        o[1] = o[3] = swap ? x : y;
        break;
      }
      case Game::QUAD_1_FLR_2_RLR:
      case Game::QUAD_1_FRL_2_RRL:
        o[0] = ToSample(swap ? b : a);
        o[1] = ToSample(swap ? a : b);
        o[2] = ToSample(swap ? d : c);
        o[3] = ToSample(swap ? c : d);
        break;
      case Game::QUAD_1_RLR_2_FLR:
      case Game::QUAD_1_RRL_2_FRL:
        o[0] = ToSample(swap ? d : c);
        o[1] = ToSample(swap ? c : d);
        o[2] = ToSample(swap ? b : a);
        o[3] = ToSample(swap ? a : b);
        break;
      case Game::QUAD_1_LR_2_FR_MIX:
      default:
      {
        // Sega Rally 2: the first chip's channels are front (left) and rear
        // (right) mono, each spread over the second chip's left/right.
        INT16 frontL = AverageSample(a, c), frontR = AverageSample(a, d);
        INT16 rearL = AverageSample(b, c), rearR = AverageSample(b, d);
        o[0] = swap ? frontR : frontL;
        o[1] = swap ? frontL : frontR;
        o[2] = swap ? rearR : rearL;
        o[3] = swap ? rearL : rearR;
        break;
      }
      }
      for (int k = 0; k < 4; k++)
        *out++ = o[k];
    }
  }

  // SDL's audio thread asks for len more bytes.
  void Drain(void *, Uint8 *stream, int lenInt)
  {
    Ring &r = s_ring;
    const UINT32 len = UINT32(lenInt);
    const UINT32 dataEnd = r.writePos + (r.writeAhead ? r.size : 0);

    if (r.readPos + len > dataEnd)
    {
      // Under-run: replay from one frame past the newest data.
      r.underRuns++;
      r.readPos = dataEnd + r.frameBytes;
      if (r.readPos >= r.size)
        r.readPos -= r.size;
      else
        r.writeAhead = true;
      r.readPos = r.Wrap(r.readPos);
    }

    if (s_enabled)
      r.CopyOut(r.readPos, stream, len);
    else
      memset(stream, 0, len);

    r.readPos += len;
    const bool nearlyFull = dataEnd + 2 * r.frameBytes > r.readPos + r.size;
    if (r.readPos >= r.size)
    {
      r.readPos -= r.size;
      r.writeAhead = false;
    }

    if (s_callback && !nearlyFull)
      s_callback(s_callbackData);
  }
}

void SetAudioCallback(AudioCallbackFPtr callback, void *data)
{
  SDL_LockAudio();
  s_callback = callback;
  s_callbackData = data;
  SDL_UnlockAudio();
}

void SetAudioEnabled(bool enabled)
{
  s_enabled = enabled;
}

void SetAudioType(Game::AudioTypes type)
{
  s_layout = type;
}

Result OpenAudio(const Util::Config::Node &config)
{
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    return ErrorLog("Unable to initialize SDL audio sub-system: %s\n", SDL_GetError());

  // Speakers: what the player asked for, but no more than the game has.
  s_hostChannels = config["NbSoundChannels"].ValueAs<int>();
  if (s_layout == Game::MONO)
    s_hostChannels = std::min(s_hostChannels, 1);
  else if (s_layout == Game::STEREO_LR || s_layout == Game::STEREO_RL)
    s_hostChannels = std::min(s_hostChannels, 2);

  // Balance, in percent: positive left/right favours the left speakers,
  // positive front/rear the front ones. The far side is turned down.
  float lr = std::max(-100.0f, std::min(100.0f, config["BalanceLeftRight"].ValueAs<float>())) * 0.01f;
  float fb = std::max(-100.0f, std::min(100.0f, config["BalanceFrontRear"].ValueAs<float>())) * 0.01f;
  float leftGain = lr < 0.0f ? 1.0f + lr : 1.0f;
  float rightGain = lr > 0.0f ? 1.0f - lr : 1.0f;
  float frontGain = fb < 0.0f ? 1.0f + fb : 1.0f;
  float rearGain = fb > 0.0f ? 1.0f - fb : 1.0f;
  s_gainFL = leftGain * frontGain;
  s_gainFR = rightGain * frontGain;
  s_gainRL = leftGain * rearGain;
  s_gainRR = rightGain * rearGain;

  SDL_AudioSpec want{};
  want.freq = kRate;
  want.format = AUDIO_S16SYS;
  want.channels = Uint8(s_hostChannels);
  want.samples = kCallbackSamples;
  want.callback = Drain;
  // A NULL "obtained" spec makes SDL convert to the device's own format.
  if (SDL_OpenAudio(&want, nullptr) < 0)
  {
    if (want.channels == 2 || want.channels == 4)
      return ErrorLog("Unable to open 44.1KHz %d-channel audio with SDL: %s\n", int(want.channels), SDL_GetError());
    return ErrorLog("Unable to open 44.1KHz channel audio with SDL: %s\n", SDL_GetError());
  }

  // Frame size from the emulated frame rate.
  float frameHz = std::max(kMinFrameHz, std::min(kMaxFrameHz, config["SoundFreq"].ValueAs<float>()));
  Ring &r = s_ring;
  r.frameSamples = INT32(kRate / frameHz);
  r.sampleBytes = UINT32(s_hostChannels) * sizeof(INT16);
  r.frameBytes = UINT32(r.frameSamples) * r.sampleBytes;

  const UINT32 ringBytes = (kRate * kRingPercent / 100) * r.sampleBytes;
  r.size = std::max(ringBytes, 3 * r.frameBytes);
  delete[] r.data;
  r.data = new (std::nothrow) INT8[r.size];
  if (r.data == nullptr)
    return ErrorLog("Insufficient memory for audio latency buffer (need %1.1f MB).", double(r.size) / double(0x100000));
  memset(r.data, 0, r.size);

  // Start half full: the write position is the first whole frame at or
  // after the middle of what follows the first frame (but leaving room for
  // one frame before the end).
  UINT32 middle = r.frameBytes + (ringBytes - r.frameBytes) / 2;
  middle += (r.frameBytes - middle % r.frameBytes) % r.frameBytes;
  r.readPos = 0;
  r.writePos = std::min(ringBytes - r.frameBytes, middle);
  r.writeAhead = false;
  r.underRuns = 0;
  r.overRuns = 0;

  SDL_PauseAudio(0);
  return Result::OKAY;
}

bool OutputAudio(unsigned numSamples, const float *leftFrontBuffer, const float *rightFrontBuffer, const float *leftRearBuffer, const float *rightRearBuffer, bool flipStereo)
{
  Ring &r = s_ring;
  numSamples = std::min(numSamples, unsigned(r.frameSamples));

  // RetroPixels: the sound for the recording, if one is being made
  RetroPixels::Recorder::Audio(numSamples, leftFrontBuffer, rightFrontBuffer, leftRearBuffer, rightRearBuffer);

  INT16 mixed[kModelChannels * kMaxFrameSamples];
  Mix(numSamples, leftFrontBuffer, rightFrontBuffer, leftRearBuffer, rightRearBuffer, mixed, flipStereo);

  SDL_LockAudio();

  const UINT32 bytes = numSamples * r.sampleBytes;
  const UINT32 readEnd = r.readPos + r.frameBytes;   // writing must stay beyond this

  // Compare positions on one straight line: readPos < readEnd < writePos.
  if (readEnd > r.writePos && r.writeAhead)
    r.writePos += r.size;

  if (readEnd > r.writePos)
  {
    // Under-run: the sound card has caught up. Move its read position to
    // one frame past the end of this new frame.
    r.underRuns++;
    r.readPos = r.writePos + bytes + r.frameBytes;
    if (r.readPos >= r.size)
      r.readPos -= r.size;
    else
    {
      r.writeAhead = true;
      r.writePos += r.size;
    }
    r.readPos = r.Wrap(r.readPos);
  }

  const bool overRun = r.writePos + bytes > r.readPos + r.size;
  bool nearlyFull = r.writePos + 2 * r.frameBytes > r.readPos + r.size;
  if (r.writePos >= r.size)
    r.writePos -= r.size;

  if (overRun)
  {
    // No room: this frame is dropped.
    r.overRuns++;
    nearlyFull = true;
  }
  else
  {
    r.CopyIn(r.writePos, mixed, bytes);
    r.writePos += bytes;
    if (r.writePos >= r.size)
    {
      r.writePos -= r.size;
      r.writeAhead = true;
    }
  }

  SDL_UnlockAudio();
  return nearlyFull;
}

void CloseAudio()
{
  SDL_CloseAudio();
  delete[] s_ring.data;
  s_ring.data = nullptr;
}
