// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The DSP's ARAM accelerator: it walks a sample buffer in ARAM between a start
// and an end address, decoding 4-bit ADPCM or scaling 8/16-bit PCM on the way,
// and signals the end of the buffer. The LLE DSP drives it through its hardware
// registers; the HLE ucodes drive it directly through this class.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;

namespace DSP
{
class Accelerator
{
public:
  virtual ~Accelerator() = default;

  // Decoded sample read (ACDSAMP). coefs points at the 16 ADPCM coefficients.
  u16 ReadSample(const s16* coefs);

  // Raw reads and writes (ACDRAW), used for example by the Zelda ucode.
  u16 ReadRaw();
  void WriteRaw(u16 value);

  u32 GetStartAddress() const { return m_start_address; }
  u32 GetEndAddress() const { return m_end_address; }
  u32 GetCurrentAddress() const { return m_current_address; }
  u16 GetSampleFormat() const { return m_format; }
  s16 GetGain() const { return m_gain; }
  s16 GetYn1() const { return m_yn1; }
  s16 GetYn2() const { return m_yn2; }
  u16 GetPredScale() const { return m_pred_scale; }
  u16 GetInput() const { return m_input; }

  void SetStartAddress(u32 address);
  void SetEndAddress(u32 address);
  void SetCurrentAddress(u32 address);
  void SetSampleFormat(u16 format);
  void SetGain(s16 gain);
  void SetYn1(s16 yn1);
  // Writing YN2 also lets reads resume after the end of the buffer was reached.
  void SetYn2(s16 yn2);
  void SetPredScale(u16 pred_scale);
  void SetInput(u16 input);

  void DoState(PointerWrap& p);

protected:
  virtual void OnRawReadEndException() = 0;
  virtual void OnRawWriteEndException() = 0;
  virtual void OnSampleReadEndException() = 0;
  virtual u8 ReadMemory(u32 address) = 0;
  virtual void WriteMemory(u32 address, u8 value) = 0;

  // Fetches the sample at the current address in the current access size.
  u16 GetCurrentSample();

private:
  u32 m_start_address = 0;
  u32 m_end_address = 0;
  u32 m_current_address = 0;

  // FORMAT register: bits 0-1 access size, 2-3 decoder, 4-5 PCM gain scale.
  u16 m_format = 0;

  s16 m_gain = 0;
  s16 m_yn1 = 0;
  s16 m_yn2 = 0;
  u16 m_pred_scale = 0;
  u16 m_input = 0;

  // Set when a sample read hits the end of the buffer. Until YN2 is written the
  // accelerator returns zero and leaves the current address alone. Not visible
  // through any register.
  bool m_reads_stopped = false;
};
}  // namespace DSP
