// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// HLE of the GameCube AX audio ucode. The CPU sends a command list each frame (5 ms); the ucode
// renders every voice parameter block of the frame into 32 kHz mixing buses, runs the aux
// effects round trips through RAM, and outputs the final stereo samples.
//
// Not emulated: depop, interaural time delay, and Dolby Pro Logic II mixing in the AX builds
// that have it.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP
{
class Accelerator;
}

namespace Memory
{
class MemoryManager;
}

namespace DSP::HLE
{
struct AXPB;
class DSPHLE;

// The voice mixer settings in one form for every AX version (each version encodes the PB's
// mixer_control differently): an enable bit and a ramp bit per send.
enum AXMixControl
{
  // clang-format off
  MIX_MAIN_L      = 0x000001,
  MIX_MAIN_L_RAMP = 0x000002,
  MIX_MAIN_R      = 0x000004,
  MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S      = 0x000010,
  MIX_MAIN_S_RAMP = 0x000020,
  MIX_AUXA_L      = 0x000040,
  MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R      = 0x000100,
  MIX_AUXA_R_RAMP = 0x000200,
  MIX_AUXA_S      = 0x000400,
  MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L      = 0x001000,
  MIX_AUXB_L_RAMP = 0x002000,
  MIX_AUXB_R      = 0x004000,
  MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S      = 0x010000,
  MIX_AUXB_S_RAMP = 0x020000,
  MIX_AUXC_L      = 0x040000,
  MIX_AUXC_L_RAMP = 0x080000,
  MIX_AUXC_R      = 0x100000,
  MIX_AUXC_R_RAMP = 0x200000,
  MIX_AUXC_S      = 0x400000,
  MIX_AUXC_S_RAMP = 0x800000,
  MIX_ALL_RAMPS   = 0xAAAAAA,
  // clang-format on
};

class AXUCode /* AXWiiUCode derives from it */ : public UCodeInterface
{
public:
  AXUCode(DSPHLE* dsphle, u32 crc);
  ~AXUCode() override;

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

protected:
  // 0xBABE0000 | size announces a command list; its address follows.
  static constexpr u32 MAIL_CMDLIST = 0xBABE0000;
  static constexpr u32 MAIL_CMDLIST_MASK = 0xFFFF0000;

  // The mixing buses. A GameCube frame is 5 ms at 32 kHz, 160 samples; the Wii uses the first 96
  // (3 ms) of each.
  enum Bus : size_t
  {
    BUS_MAIN_L,
    BUS_MAIN_R,
    BUS_MAIN_S,
    BUS_AUXA_L,
    BUS_AUXA_R,
    BUS_AUXA_S,
    BUS_AUXB_L,
    BUS_AUXB_R,
    BUS_AUXB_S,
    NUM_BUSES,
  };
  static constexpr size_t SAMPLES_PER_MS = 32;
  static constexpr size_t BUS_SAMPLES = 5 * SAMPLES_PER_MS;
  using BusBuffer = std::array<int, BUS_SAMPLES>;

  struct WiiVariant
  {
  };
  // For AXWiiUCode, which sets up its own accelerator.
  AXUCode(DSPHLE* dsphle, u32 crc, WiiVariant);

  int* BusData(size_t bus) { return m_buses[bus].data(); }

  // Startup shared with the Wii version: DSP_INIT, then the resampling coefficients.
  void InitializeShared();

  // Looks for the polyphase coefficient tables (dsp_coef.bin, from the DSP ROM) in the user's
  // GC folder, then in Sys. With require_same_checksum, only a file with that Adler-32 is used.
  bool LoadResamplingCoefficients(bool require_same_checksum, u32 desired_checksum);
  const s16* CoefficientTables() const { return m_coeffs_checksum ? m_coeffs.data() : nullptr; }

  // Walks the command list. Reading past its end gives 0; AtEnd() then stops the walk.
  class CommandReader
  {
  public:
    explicit CommandReader(const std::array<u16, 512>& list) : m_list(list) {}

    bool AtEnd() const { return m_pos >= m_list.size(); }
    u16 Word() { return m_pos < m_list.size() ? m_list[m_pos++] : u16{0}; }
    u32 Address()
    {
      const u32 hi = Word();
      const u32 lo = Word();
      return (hi << 16) | lo;
    }
    void Skip(size_t words) { m_pos += words; }
    void Restart() { m_pos = 0; }

  private:
    const std::array<u16, 512>& m_list;
    size_t m_pos = 0;
  };

  void CopyCmdList(u32 addr, u16 size);
  virtual void HandleCommandList();
  void SignalWorkEnd();

  // Fills buffers from a RAM list of (32-bit start value, 16-bit per-sample delta) triplets,
  // one per buffer; a zero start value clears the buffer.
  struct RampTarget
  {
    int* samples;
    size_t count;
  };
  void InitBuffersFromRamps(u32 list_addr, const RampTarget* targets, size_t num_targets);

  // The frame compressor: gain ramps from a table in RAM, triggered when main L/R exceed the
  // threshold, then released over `release_frames` frames.
  void RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis);

  // Saves the state shared with AXWii.
  void DoAXState(PointerWrap& p);

  std::array<BusBuffer, NUM_BUSES> m_buses{};

  std::array<u16, 512> m_cmdlist{};
  u32 m_cmdlist_size = 0;

  // Polyphase coefficient tables (4 tables of 128 phases x 4 taps), present only when the file
  // was found; the checksum identifies them in savestates.
  std::optional<u32> m_coeffs_checksum = std::nullopt;
  std::array<s16, 0x800> m_coeffs{};

  u16 m_compressor_pos = 0;

  std::unique_ptr<Accelerator> m_accelerator;

private:
  enum class MailState
  {
    WaitingForCmdListSize,
    WaitingForCmdListAddress,
    WaitingForNextTask,
  };

  u32 ConvertMixerControl(u16 mixer_control) const;
  bool PBHasLowPassFilter() const;
  void ReadPB(Memory::MemoryManager& memory, u32 addr, AXPB& pb) const;
  void WritePB(Memory::MemoryManager& memory, u32 addr, const AXPB& pb) const;

  // Command list operations.
  void SetupProcessing(u32 init_addr);
  void DownloadAndMixWithVolume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb);
  void ProcessPBList(u32 pb_addr);
  void MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr);
  void UploadLRS(u32 dst_addr);
  void SetMainLR(u32 src_addr);
  void OutputSamples(u32 lr_addr, u32 surround_addr);
  void MixAUXBLR(u32 ul_addr, u32 dl_addr);
  void SetOppositeLR(u32 src_addr);
  void SendAUXAndMix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                     u32 auxb_r_dl);

  MailState m_mail_state = MailState::WaitingForCmdListSize;
};
}  // namespace DSP::HLE
