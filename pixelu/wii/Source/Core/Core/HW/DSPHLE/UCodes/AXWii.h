// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/AX.h"

namespace Memory
{
class MemoryManager;
}

namespace DSP::HLE
{
struct AXPBWii;
class DSPHLE;

// HLE of the Wii AX ucode: 3 ms frames, a third aux bus (C), output volume ramps, and the four
// Wii Remote speakers. Several builds are handled, which differ in their PB layout and command
// numbering.
class AXWiiUCode final : public AXUCode
{
public:
  AXWiiUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void DoState(PointerWrap& p) override;

private:
  static constexpr size_t FRAME_SAMPLES = 3 * SAMPLES_PER_MS;
  // The Wii Remote speakers get 6 samples per millisecond.
  static constexpr size_t REMOTE_SAMPLES_PER_MS = 6;
  static constexpr size_t REMOTE_FRAME_SAMPLES = 3 * REMOTE_SAMPLES_PER_MS;

  enum AuxCBus : size_t
  {
    AUXC_L,
    AUXC_R,
    AUXC_S,
  };

  // What a command list entry does, independent of the build's opcode numbering.
  enum class Op
  {
    Setup,
    AddToLR,
    SubToLR,
    AddSubToLR,
    SetPBAddr,
    ProcessStoredPBs,
    ProcessPBs,
    MixAuxA,
    MixAuxB,
    MixAuxC,
    UploadAuxAMixLRSC,
    UploadAuxBMixLRSC,
    Compressor,
    Output,
    OutputDPL2,
    OutputRemotes,
    End,
    Ignored,
  };
  Op DecodeOp(u16 opcode) const;

  // Volume ramp from one frame's volume to the next. Uses float steps; the ucode's integer ramp
  // is close but not identical.
  static void GenerateVolumeRamp(u16* output, u16 from, u16 to, size_t count);
  static u32 ConvertMixerControl(u32 mixer_control);

  void HandleCommandList() override;

  int* AuxBus(int aux_id, size_t channel);
  int* RemoteMain(size_t speaker) { return m_remote[speaker].data(); }
  int* RemoteAux(size_t speaker) { return m_remote[4 + speaker].data(); }

  void SetupProcessing(u32 init_addr);
  void AddToLR(u32 val_addr, bool neg);
  void AddSubToLR(u32 val_addr);
  void ProcessPBList(u32 pb_addr);
  void MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr, u16 volume);
  void UploadAUXMixLRSC(int aux_id, const u32* addresses, u16 volume);
  void OutputSamples(u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc);
  void OutputWMSamples(const u32* addresses);

  void ReadPB(Memory::MemoryManager& memory, u32 addr, AXPBWii& pb) const;
  void WritePB(Memory::MemoryManager& memory, u32 addr, const AXPBWii& pb) const;

  std::array<std::array<int, FRAME_SAMPLES>, 3> m_auxc{};
  // Main 0-3, then aux 0-3 (the savestate order).
  std::array<std::array<int, REMOTE_FRAME_SAMPLES>, 8> m_remote{};

  // Early builds: the old opcode numbering, and PB updates each millisecond.
  bool m_old_axwii = false;
  // Late builds: the biquad and Wii Remote filters.
  bool m_new_filter = false;

  // The previous frame's output and aux return volumes, where the next ramps start.
  u16 m_last_main_volume = 0x8000;
  std::array<u16, 3> m_last_aux_volumes{0x8000, 0x8000, 0x8000};
};
}  // namespace DSP::HLE
