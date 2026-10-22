// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// SCSP low-frequency oscillators. Included by SCSP.cpp; not compiled on its
// own.
//
// Each voice has a pitch LFO and an amplitude LFO that share one frequency
// (LFOF, 0.17-172.3 Hz) but have their own waveform (saw, square, triangle
// or noise) and depth. The phase is 16 bits; its top 8 bits index a
// 256-entry waveform, whose value then indexes a depth curve giving a
// multiplier in 1/256 units (pitch: +/- up to PSCALE cents; amplitude: down
// to -ASCALE dB).

#include <cmath>
#include <cstdlib>

namespace ScspLfo
{
  struct Osc
  {
    UINT16 phase;
    UINT32 rate;          // phase increment per sample
    const int *wave;
    const int *depth;
  };

  // Register waveform numbers
  enum { kSaw, kSquare, kTriangle, kNoise };

  // LFO frequencies in Hz for LFOF = 0-31 (from the SCSP manual).
  constexpr float kFrequency[32] =
  {
    0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f,
    0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f, 1.87f, 2.27f,
    2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f,
    14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f
  };
  // Maximum pitch swing in cents and amplitude swing in dB for depth 0-7.
  constexpr float kPitchCents[8] = { 0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f };
  constexpr float kAmpDecibels[8] = { 0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f };

  // All tables, 256 entries each, in one block in this order:
  //   rows 0-7    amplitude depth curves (indexed by wave value 0..255)
  //   rows 8-15   pitch depth curves (indexed by wave value + 128)
  //   rows 16-19  amplitude waves: noise, saw, square, triangle (0..255)
  //   rows 20-23  pitch waves: noise, saw, square, triangle (-128..127)
  // The order matters, as it always has in RetroPixels' Model 3 build:
  // - the pitch depth field as decoded here (see SCSP.cpp) runs 0-14, and
  //   depths 8-14 take rows 16-22 (waves) as their curve;
  // - the pitch noise wave reaches +128, so its curve lookup can read entry
  //   256: the first entry of the next row.
  enum { kAmpDepthRow = 0, kPitchDepthRow = 8, kAmpWaveRow = 16, kPitchWaveRow = 20, kRows = 24 };
  static int s_table[kRows * 256 + 1];

  inline int *Row(int r) { return &s_table[r * 256]; }

  // Register waveform number -> wave row offset (noise is stored first).
  inline int WaveRow(unsigned waveform) { return waveform == kNoise ? 0 : (int)waveform + 1; }

  void Init()
  {
    int *ampSaw = Row(kAmpWaveRow + WaveRow(kSaw)), *pitchSaw = Row(kPitchWaveRow + WaveRow(kSaw));
    int *ampSquare = Row(kAmpWaveRow + WaveRow(kSquare)), *pitchSquare = Row(kPitchWaveRow + WaveRow(kSquare));
    int *ampTri = Row(kAmpWaveRow + WaveRow(kTriangle)), *pitchTri = Row(kPitchWaveRow + WaveRow(kTriangle));
    int *ampNoise = Row(kAmpWaveRow + WaveRow(kNoise)), *pitchNoise = Row(kPitchWaveRow + WaveRow(kNoise));

    for (int i = 0; i < 256; ++i)
    {
      ampSaw[i] = 255 - i;
      pitchSaw[i] = (i < 128) ? i : i - 256;

      ampSquare[i] = (i < 128) ? 255 : 0;
      pitchSquare[i] = (i < 128) ? 127 : -128;

      ampTri[i] = (i < 128) ? 255 - 2 * i : 2 * i - 256;
      if (i < 64)
        pitchTri[i] = 2 * i;
      else if (i < 128)
        pitchTri[i] = 255 - 2 * i;
      else if (i < 192)
        pitchTri[i] = 256 - 2 * i;
      else
        pitchTri[i] = 2 * i - 511;

      const int noise = std::rand() & 0xFF;
      ampNoise[i] = noise;
      pitchNoise[i] = 128 - noise;
    }

    for (int d = 0; d < 8; ++d)
    {
      int *pitch = Row(kPitchDepthRow + d);
      const float cents = kPitchCents[d];
      for (int i = -128; i < 128; ++i)
      {
        const double c = ((double)cents * (double)i) / 128.0;
        pitch[i + 128] = (int)(unsigned)(256.0 * std::exp2(c * (1.0 / 1200.0)));
      }
      int *amp = Row(kAmpDepthRow + d);
      const float db = -kAmpDecibels[d];
      for (int i = 0; i < 256; ++i)
      {
        const double v = ((double)db * (double)i) / 256.0;
        amp[i] = (int)(unsigned)(256.0 * std::pow(10.0, v * (1.0 / 20.0)));
      }
    }
  }

  // Points an oscillator at its tables before any register sets it up.
  void Clear(Osc &lfo)
  {
    lfo.phase = 0;
    lfo.rate = 0;
    lfo.wave = Row(kPitchWaveRow + WaveRow(kSaw));
    lfo.depth = Row(kPitchDepthRow);
  }

  void Setup(Osc &lfo, unsigned freq, unsigned waveform, unsigned depth, bool amplitude)
  {
    const float perSample = kFrequency[freq] * 256.0f / 44100.0f;
    lfo.rate = (UINT32)(256.0f * perSample);
    if (amplitude)
    {
      lfo.wave = Row(kAmpWaveRow + WaveRow(waveform));
      lfo.depth = Row(kAmpDepthRow + (int)depth);
    }
    else
    {
      lfo.wave = Row(kPitchWaveRow + WaveRow(waveform));
      lfo.depth = Row(kPitchDepthRow + (int)depth);
    }
  }

  // Both return the multiplier in 1/4096 units.
  inline int PitchStep(Osc &lfo)
  {
    lfo.phase = (UINT16)(lfo.phase + lfo.rate);
    return lfo.depth[lfo.wave[lfo.phase >> 8] + 128] * 16;
  }

  inline int AmpStep(Osc &lfo)
  {
    lfo.phase = (UINT16)(lfo.phase + lfo.rate);
    return lfo.depth[lfo.wave[lfo.phase >> 8]] * 16;
  }
}
