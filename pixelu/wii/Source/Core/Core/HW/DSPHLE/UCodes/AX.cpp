// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/AX.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/AXStructs.h"
#include "Core/HW/DSPHLE/UCodes/AXVoice.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace DSP::HLE
{
using AXVoice::AddWrapped;
using AXVoice::Clamp16;
using AXVoice::Join16;

namespace
{
// The oldest GameCube AX build: no lpf in its PBs, its own mixer_control encoding, and no
// compressor command.
constexpr u32 CRC_OLDEST_AX = 0x4e8a8b21;

// GameCube command list opcodes.
enum : u16
{
  CMD_SETUP = 0x00,
  CMD_DL_AND_VOL_MIX = 0x01,
  CMD_PB_ADDR = 0x02,
  CMD_PROCESS = 0x03,
  CMD_MIX_AUXA = 0x04,
  CMD_MIX_AUXB = 0x05,
  CMD_UPLOAD_LRS = 0x06,
  CMD_SET_LR = 0x07,
  CMD_UNK_08 = 0x08,
  CMD_MIX_AUXB_NOWRITE = 0x09,
  CMD_UNK_0A = 0x0A,
  CMD_UNK_0B = 0x0B,
  CMD_UNK_0C = 0x0C,
  CMD_MORE = 0x0D,
  CMD_OUTPUT = 0x0E,
  CMD_END = 0x0F,
  CMD_MIX_AUXB_LR = 0x10,
  CMD_SET_OPPOSITE_LR = 0x11,
  CMD_COMPRESSOR = 0x12,
  CMD_SEND_AUX_AND_MIX = 0x13,
};

// Reads 32-bit big-endian sample `index` of a RAM buffer.
s32 ReadSampleBE(const u8* buffer, size_t index)
{
  u32 raw;
  std::memcpy(&raw, buffer + index * sizeof(u32), sizeof(raw));
  return static_cast<s32>(Common::swap32(raw));
}

// A buffer of `count` 32-bit samples in RAM.
const u8* SampleBuffer(const Memory::MemoryManager& memory, u32 address, size_t count)
{
  return memory.GetPointerForRange(address, count * sizeof(u32));
}
}  // namespace

AXUCode::AXUCode(DSPHLE* dsphle, u32 crc, WiiVariant) : UCodeInterface(dsphle, crc)
{
}

AXUCode::AXUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc)
{
  INFO_LOG_FMT(DSPHLE, "AX (GameCube) ucode {:08x}", crc);
  m_accelerator = std::make_unique<AXVoice::VoiceAccelerator<AXPB>>(dsphle->GetSystem().GetDSP());
}

AXUCode::~AXUCode() = default;

void AXUCode::Initialize()
{
  InitializeShared();
}

void AXUCode::InitializeShared()
{
  m_mail_handler.PushMail(DSP_INIT, true);
  LoadResamplingCoefficients(false, 0);
}

bool AXUCode::LoadResamplingCoefficients(bool require_same_checksum, u32 desired_checksum)
{
  constexpr size_t FILE_SIZE = 0x800 * sizeof(s16);
  m_coeffs_checksum.reset();

  const std::string candidates[] = {
      File::GetUserPath(D_GCUSER_IDX) + "dsp_coef.bin",
      File::GetSysDirectory() + "/GC/dsp_coef.bin",
  };
  for (const std::string& path : candidates)
  {
    INFO_LOG_FMT(DSPHLE, "Looking for polyphase resampling coefficients in {}", path);
    if (File::GetSize(path) != FILE_SIZE)
      continue;

    std::array<u8, FILE_SIZE> raw{};
    File::IOFile file(path, "rb");
    file.ReadBytes(raw.data(), raw.size());

    const u32 checksum = Common::HashAdler32(raw.data(), raw.size());
    if (require_same_checksum && checksum != desired_checksum)
      continue;

    for (size_t i = 0; i < m_coeffs.size(); ++i)
      m_coeffs[i] = static_cast<s16>((raw[2 * i] << 8) | raw[2 * i + 1]);

    INFO_LOG_FMT(DSPHLE, "Using polyphase resampling coefficients from {}", path);
    m_coeffs_checksum = checksum;
    return true;
  }
  return false;
}

void AXUCode::SignalWorkEnd()
{
  // The interrupt must not come too soon after the command list (The Clone Wars hangs at boot
  // otherwise). Real hardware takes from about 50,000 to over 700,000 cycles; games budget about
  // 2,500 cycles for an empty list, which is a safe lower bound.
  constexpr int EMPTY_COMMAND_LIST_CYCLES = 2500;
  m_mail_handler.PushMail(DSP_YIELD, true, EMPTY_COMMAND_LIST_CYCLES);
}

void AXUCode::HandleCommandList()
{
  u32 pb_addr = 0;
  CommandReader in(m_cmdlist);

  while (!in.AtEnd())
  {
    const u16 cmd = in.Word();
    switch (cmd)
    {
    case CMD_SETUP:
      SetupProcessing(in.Address());
      break;

    case CMD_DL_AND_VOL_MIX:
    {
      const u32 addr = in.Address();
      const u16 vol_main = in.Word();
      const u16 vol_auxa = in.Word();
      const u16 vol_auxb = in.Word();
      DownloadAndMixWithVolume(addr, vol_main, vol_auxa, vol_auxb);
      break;
    }

    case CMD_PB_ADDR:
      pb_addr = in.Address();
      break;

    case CMD_PROCESS:
      ProcessPBList(pb_addr);
      break;

    case CMD_MIX_AUXA:
    case CMD_MIX_AUXB:
    {
      const u32 write_addr = in.Address();
      const u32 read_addr = in.Address();
      MixAUXSamples(cmd == CMD_MIX_AUXA ? 0 : 1, write_addr, read_addr);
      break;
    }

    case CMD_UPLOAD_LRS:
      UploadLRS(in.Address());
      break;

    case CMD_SET_LR:
      SetMainLR(in.Address());
      break;

    case CMD_UNK_08:
      // Unknown, with ten words of arguments.
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesUnimplementedAXCommand);
      in.Skip(10);
      break;

    case CMD_MIX_AUXB_NOWRITE:
      MixAUXSamples(1, 0, in.Address());
      break;

    case CMD_UNK_0A:
    case CMD_UNK_0B:
    case CMD_UNK_0C:
      // No-ops in every known build.
      break;

    case CMD_MORE:
    {
      // Continue with another list.
      const u32 addr = in.Address();
      const u16 size = in.Word();
      CopyCmdList(addr, size);
      in.Restart();
      break;
    }

    case CMD_OUTPUT:
    {
      const u32 surround_addr = in.Address();
      const u32 lr_addr = in.Address();
      OutputSamples(lr_addr, surround_addr);
      break;
    }

    case CMD_END:
      return;

    case CMD_MIX_AUXB_LR:
    {
      const u32 upload_addr = in.Address();
      const u32 download_addr = in.Address();
      MixAUXBLR(upload_addr, download_addr);
      break;
    }

    case CMD_SET_OPPOSITE_LR:
      SetOppositeLR(in.Address());
      break;

    case CMD_COMPRESSOR:
    {
      // The oldest build has no such command (it jumps into a mixer routine instead).
      ASSERT(m_crc != CRC_OLDEST_AX);
      const u16 threshold = in.Word();
      const u16 release_frames = in.Word();
      const u32 table_addr = in.Address();
      RunCompressor(threshold, release_frames, table_addr, 5);
      break;
    }

    case CMD_SEND_AUX_AND_MIX:
    {
      const u32 auxa_lrs_up = in.Address();
      const u32 auxb_s_up = in.Address();
      const u32 main_l_dl = in.Address();
      const u32 main_r_dl = in.Address();
      const u32 auxb_l_dl = in.Address();
      const u32 auxb_r_dl = in.Address();
      SendAUXAndMix(auxa_lrs_up, auxb_s_up, main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl);
      break;
    }

    default:
      ERROR_LOG_FMT(DSPHLE, "AX: unknown command {:04x} in the command list", cmd);
      return;
    }
  }
  ERROR_LOG_FMT(DSPHLE, "AX: command list without an end command");
}

u32 AXUCode::ConvertMixerControl(u16 mixer_control) const
{
  u32 mix = 0;

  if (m_crc == CRC_OLDEST_AX)
  {
    // Main L/R are always on. Bit 4 selects Dolby Pro Logic II mixing, with its own meaning for
    // the low bits; bit 3 turns on every ramp.
    mix = MIX_MAIN_L | MIX_MAIN_R;
    if ((mixer_control & 0x0010) != 0)
    {
      if ((mixer_control & 0x0006) == 0)
        mix |= MIX_AUXB_L | MIX_AUXB_R;
      if ((mixer_control & 0x0007) == 1)
        mix |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
    }
    else
    {
      if ((mixer_control & 0x0001) != 0)
        mix |= MIX_AUXA_L | MIX_AUXA_R;
      if ((mixer_control & 0x0002) != 0)
        mix |= MIX_AUXB_L | MIX_AUXB_R;
      if ((mixer_control & 0x0004) != 0)
      {
        // Surround for main, and for whichever aux buses are on.
        mix |= MIX_MAIN_S;
        if ((mix & MIX_AUXA_L) != 0)
          mix |= MIX_AUXA_S;
        if ((mix & MIX_AUXB_L) != 0)
          mix |= MIX_AUXB_S;
      }
    }
    if ((mixer_control & 0x0008) != 0)
      mix |= MIX_ALL_RAMPS;
    return mix;
  }

  // Later builds: one bit per send or ramp group. (0x4000, which picks the surround input of the
  // aux B sends for Dolby Pro Logic II, is ignored.)
  struct BitMeaning
  {
    u16 bit;
    u32 flags;
  };
  static constexpr BitMeaning MEANINGS[] = {
      {0x0001, MIX_MAIN_L},
      {0x0002, MIX_MAIN_R},
      {0x0004, MIX_MAIN_S},
      {0x0008, MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP},
      {0x0010, MIX_AUXA_L},
      {0x0020, MIX_AUXA_R},
      {0x0040, MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP},
      {0x0080, MIX_AUXA_S},
      {0x0100, MIX_AUXA_S_RAMP},
      {0x0200, MIX_AUXB_L},
      {0x0400, MIX_AUXB_R},
      {0x0800, MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP},
      {0x1000, MIX_AUXB_S},
      {0x2000, MIX_AUXB_S_RAMP},
  };
  for (const BitMeaning& meaning : MEANINGS)
  {
    if ((mixer_control & meaning.bit) != 0)
      mix |= meaning.flags;
  }
  return mix;
}

void AXUCode::InitBuffersFromRamps(u32 list_addr, const RampTarget* targets, size_t num_targets)
{
  std::array<u16, 3 * 32> words{};
  const auto& memory = m_dsphle->GetSystem().GetMemory();
  memory.CopyFromEmuSwapped(words.data(), list_addr, 3 * num_targets * sizeof(u16));

  for (size_t i = 0; i < num_targets; ++i)
  {
    const RampTarget& target = targets[i];
    const s32 start = static_cast<s32>(Join16(words[3 * i], words[3 * i + 1]));
    const s16 delta = static_cast<s16>(words[3 * i + 2]);
    if (start == 0)
    {
      std::fill_n(target.samples, target.count, 0);
      continue;
    }
    for (size_t j = 0; j < target.count; ++j)
      target.samples[j] = AddWrapped(start, static_cast<s64>(j) * delta);
  }
}

void AXUCode::SetupProcessing(u32 init_addr)
{
  std::array<RampTarget, NUM_BUSES> targets;
  for (size_t bus = 0; bus < NUM_BUSES; ++bus)
    targets[bus] = {BusData(bus), BUS_SAMPLES};
  InitBuffersFromRamps(init_addr, targets.data(), targets.size());
}

void AXUCode::DownloadAndMixWithVolume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb)
{
  // The same three buffers (L, R, S) are mixed into main, aux A and aux B, each with its volume.
  const u16 volumes[3] = {vol_main, vol_auxa, vol_auxb};
  const auto& memory = m_dsphle->GetSystem().GetMemory();
  for (size_t group = 0; group < 3; ++group)
  {
    const u8* source = SampleBuffer(memory, addr, 3 * BUS_SAMPLES);
    for (size_t channel = 0; channel < 3; ++channel)
    {
      int* bus = BusData(3 * group + channel);
      for (size_t i = 0; i < BUS_SAMPLES; ++i)
      {
        const s64 scaled = s64{ReadSampleBE(source, channel * BUS_SAMPLES + i)} * volumes[group];
        bus[i] = AddWrapped(bus[i], static_cast<s32>(scaled >> 15));
      }
    }
  }
}

bool AXUCode::PBHasLowPassFilter() const
{
  return m_crc != CRC_OLDEST_AX;
}

void AXUCode::ReadPB(Memory::MemoryManager& memory, u32 addr, AXPB& pb) const
{
  u16* const words = reinterpret_cast<u16*>(&pb);
  if (PBHasLowPassFilter())
  {
    memory.CopyFromEmuSwapped(words, addr, sizeof(pb));
    return;
  }

  // In RAM the PB goes straight from the loop info to loop_counter.
  constexpr size_t lpf_begin = offsetof(AXPB, lpf);
  constexpr size_t lpf_end = offsetof(AXPB, loop_counter);
  memory.CopyFromEmuSwapped(words, addr, lpf_begin);
  pb.lpf = {};
  memory.CopyFromEmuSwapped(words + lpf_end / 2, addr + static_cast<u32>(lpf_begin),
                            sizeof(pb) - lpf_end);
}

void AXUCode::WritePB(Memory::MemoryManager& memory, u32 addr, const AXPB& pb) const
{
  const u16* const words = reinterpret_cast<const u16*>(&pb);
  if (PBHasLowPassFilter())
  {
    memory.CopyToEmuSwapped(addr, words, sizeof(pb));
    return;
  }

  constexpr size_t lpf_begin = offsetof(AXPB, lpf);
  constexpr size_t lpf_end = offsetof(AXPB, loop_counter);
  memory.CopyToEmuSwapped(addr, words, lpf_begin);
  memory.CopyToEmuSwapped(addr + static_cast<u32>(lpf_begin), words + lpf_end / 2,
                          sizeof(pb) - lpf_end);
}

void AXUCode::ProcessPBList(u32 pb_addr)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  auto& accelerator = static_cast<AXVoice::VoiceAccelerator<AXPB>&>(*m_accelerator);

  AXPB pb{};
  while (pb_addr != 0)
  {
    ReadPB(memory, pb_addr, pb);
    const PBUpdateData updates = AXVoice::FetchUpdateList(memory, pb);

    // The frame is rendered a millisecond at a time, applying that millisecond's updates first.
    for (u32 ms = 0; ms < 5; ++ms)
    {
      AXVoice::ApplyUpdatesForMillisecond(pb, ms, updates);

      AXVoice::VoiceTargets<AXPB> targets;
      for (size_t bus = 0; bus < NUM_BUSES; ++bus)
        targets.buses[bus] = BusData(bus) + ms * SAMPLES_PER_MS;

      AXVoice::RenderVoice(accelerator, pb, targets, SAMPLES_PER_MS,
                           ConvertMixerControl(pb.mixer_control), CoefficientTables(), false);
    }

    WritePB(memory, pb_addr, pb);
    pb_addr = Join16(pb.next_pb_hi, pb.next_pb_lo);
  }
}

void AXUCode::MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = BUS_SAMPLES * sizeof(int);

  // Send the aux bus (L, R, S) to the CPU for its effect, unless there is nowhere to send it...
  const size_t aux_first = aux_id == 0 ? BUS_AUXA_L : BUS_AUXB_L;
  if (write_addr != 0)
  {
    for (size_t i = 0; i < 3; ++i)
      memory.CopyToEmuSwapped(write_addr + static_cast<u32>(i) * BUS_BYTES, BusData(aux_first + i),
                              BUS_BYTES);
  }

  // ...and add the effect's output (L, R, S) to main.
  const u8* returned = SampleBuffer(memory, read_addr, 3 * BUS_SAMPLES);
  for (size_t channel = 0; channel < 3; ++channel)
  {
    int* bus = BusData(BUS_MAIN_L + channel);
    for (size_t i = 0; i < BUS_SAMPLES; ++i)
      bus[i] = AddWrapped(bus[i], ReadSampleBE(returned, channel * BUS_SAMPLES + i));
  }
}

void AXUCode::UploadLRS(u32 dst_addr)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = BUS_SAMPLES * sizeof(int);
  for (size_t i = 0; i < 3; ++i)
  {
    memory.CopyToEmuSwapped(dst_addr + static_cast<u32>(i) * BUS_BYTES, BusData(BUS_MAIN_L + i),
                            BUS_BYTES);
  }
}

void AXUCode::SetMainLR(u32 src_addr)
{
  const u8* source = SampleBuffer(m_dsphle->GetSystem().GetMemory(), src_addr, BUS_SAMPLES);
  for (size_t i = 0; i < BUS_SAMPLES; ++i)
  {
    const s32 sample = ReadSampleBE(source, i);
    m_buses[BUS_MAIN_L][i] = sample;
    m_buses[BUS_MAIN_R][i] = sample;
    m_buses[BUS_MAIN_S][i] = 0;
  }
}

void AXUCode::SetOppositeLR(u32 src_addr)
{
  const u8* source = SampleBuffer(m_dsphle->GetSystem().GetMemory(), src_addr, BUS_SAMPLES);
  for (size_t i = 0; i < BUS_SAMPLES; ++i)
  {
    const s32 sample = ReadSampleBE(source, i);
    m_buses[BUS_MAIN_L][i] = static_cast<int>(0u - static_cast<u32>(sample));
    m_buses[BUS_MAIN_R][i] = sample;
    m_buses[BUS_MAIN_S][i] = 0;
  }
}

void AXUCode::RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis)
{
  const size_t count = SAMPLES_PER_MS * millis;
  BusBuffer& left = m_buses[BUS_MAIN_L];
  BusBuffer& right = m_buses[BUS_MAIN_R];

  // |sample| > threshold, where (as with the ucode's 32-bit absolute value) INT_MIN stays
  // negative and never counts as loud.
  const s32 limit = threshold;
  const auto too_loud = [limit](s32 sample) {
    return sample != std::numeric_limits<s32>::min() && (sample > limit || sample < -limit);
  };
  bool triggered = false;
  for (size_t i = 0; i < count && !triggered; ++i)
    triggered = too_loud(left[i]) || too_loud(right[i]);

  // The table holds one gain ramp per frame: 11 attack ramps, then the release ramps.
  constexpr u32 ATTACK_RAMPS = 11;
  const u32 ramp_bytes = static_cast<u32>(count * sizeof(u16));
  u32 ramp_index;
  if (triggered)
  {
    ramp_index = m_compressor_pos;
    m_compressor_pos = release_frames;
  }
  else if (m_compressor_pos != 0)
  {
    --m_compressor_pos;
    ramp_index = ATTACK_RAMPS + m_compressor_pos;
  }
  else
  {
    return;
  }

  const u8* ramp = m_dsphle->GetSystem().GetMemory().GetPointerForRange(
      table_addr + ramp_index * ramp_bytes, ramp_bytes);
  for (size_t i = 0; i < count; ++i)
  {
    u16 gain;
    std::memcpy(&gain, ramp + i * sizeof(u16), sizeof(gain));
    gain = Common::swap16(gain);
    left[i] = static_cast<int>((s64{left[i]} * gain) >> 15);
    right[i] = static_cast<int>((s64{right[i]} * gain) >> 15);
  }
}

void AXUCode::OutputSamples(u32 lr_addr, u32 surround_addr)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  memory.CopyToEmuSwapped(surround_addr, BusData(BUS_MAIN_S), BUS_SAMPLES * sizeof(int));

  // Final output: 16-bit, interleaved right then left, big-endian.
  std::array<u16, BUS_SAMPLES * 2> frame;
  for (size_t i = 0; i < BUS_SAMPLES; ++i)
  {
    frame[2 * i] = Common::swap16(static_cast<u16>(Clamp16(m_buses[BUS_MAIN_R][i])));
    frame[2 * i + 1] = Common::swap16(static_cast<u16>(Clamp16(m_buses[BUS_MAIN_L][i])));
  }
  memory.CopyToEmu(lr_addr, frame.data(), sizeof(frame));
}

void AXUCode::MixAUXBLR(u32 ul_addr, u32 dl_addr)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = BUS_SAMPLES * sizeof(int);
  memory.CopyToEmuSwapped(ul_addr, BusData(BUS_AUXB_L), BUS_BYTES);
  memory.CopyToEmuSwapped(ul_addr + BUS_BYTES, BusData(BUS_AUXB_R), BUS_BYTES);

  // The returned L/R replace aux B L/R and are added to main L/R.
  const u8* returned = SampleBuffer(memory, dl_addr, 2 * BUS_SAMPLES);
  for (size_t channel = 0; channel < 2; ++channel)
  {
    int* aux = BusData(BUS_AUXB_L + channel);
    int* main = BusData(BUS_MAIN_L + channel);
    for (size_t i = 0; i < BUS_SAMPLES; ++i)
    {
      const s32 sample = ReadSampleBE(returned, channel * BUS_SAMPLES + i);
      aux[i] = sample;
      main[i] = AddWrapped(main[i], sample);
    }
  }
}

void AXUCode::SendAUXAndMix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl,
                            u32 auxb_l_dl, u32 auxb_r_dl)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = BUS_SAMPLES * sizeof(int);

  for (size_t i = 0; i < 3; ++i)
  {
    memory.CopyToEmuSwapped(auxa_lrs_up + static_cast<u32>(i) * BUS_BYTES, BusData(BUS_AUXA_L + i),
                            BUS_BYTES);
  }
  memory.CopyToEmuSwapped(auxb_s_up, BusData(BUS_AUXB_S), BUS_BYTES);

  const std::pair<u32, size_t> downloads[] = {
      {main_l_dl, BUS_MAIN_L},
      {main_r_dl, BUS_MAIN_R},
      {auxb_l_dl, BUS_AUXB_L},
      {auxb_r_dl, BUS_AUXB_R},
  };
  for (const auto& [address, bus_index] : downloads)
  {
    const u8* source = SampleBuffer(memory, address, BUS_SAMPLES);
    int* bus = BusData(bus_index);
    for (size_t i = 0; i < BUS_SAMPLES; ++i)
      bus[i] = AddWrapped(bus[i], ReadSampleBE(source, i));
  }
}

void AXUCode::HandleMail(u32 mail)
{
  if (m_upload_setup_in_progress)
  {
    PrepareBootUCode(mail);
    return;
  }

  switch (m_mail_state)
  {
  case MailState::WaitingForCmdListSize:
    if ((mail & MAIL_CMDLIST_MASK) != MAIL_CMDLIST)
    {
      ERROR_LOG_FMT(DSPHLE, "AX: expected a command list mail, got {:08x}", mail);
      return;
    }
    m_cmdlist_size = mail & 0xFFFF;
    m_mail_state = MailState::WaitingForCmdListAddress;
    return;

  case MailState::WaitingForCmdListAddress:
    CopyCmdList(mail, static_cast<u16>(m_cmdlist_size));
    HandleCommandList();
    m_cmdlist_size = 0;
    SignalWorkEnd();
    m_mail_state = MailState::WaitingForNextTask;
    return;

  case MailState::WaitingForNextTask:
    break;
  }

  // The ucode does not check the 0xCDD1 prefix.
  if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP)
  {
    WARN_LOG_FMT(DSPHLE, "AX: task mail without the CDD1 prefix: {:08x}", mail);
    mail = TASK_MAIL_TO_DSP | (mail & 0xFFFF);
  }

  switch (mail)
  {
  case MAIL_RESUME:
    m_mail_handler.PushMail(DSP_RESUME, true);
    m_mail_state = MailState::WaitingForCmdListSize;
    break;
  case MAIL_NEW_UCODE:
    // When we are switched back to later, Update() sends the resume mail.
    m_upload_setup_in_progress = true;
    m_mail_state = MailState::WaitingForCmdListSize;
    break;
  case MAIL_RESET:
    m_dsphle->SetUCode(UCODE_ROM);
    break;
  case MAIL_CONTINUE:
    // The CPU sends the next command list without waiting for anything.
    m_mail_state = MailState::WaitingForCmdListSize;
    break;
  default:
    WARN_LOG_FMT(DSPHLE, "AX: unknown task mail {:08x}", mail);
    break;
  }
}

void AXUCode::CopyCmdList(u32 addr, u16 size)
{
  if (size >= m_cmdlist.size())
  {
    // Keeps the previous list.
    ERROR_LOG_FMT(DSPHLE, "AX: command list at {:08x} too large ({} words)", addr, size);
    return;
  }
  m_dsphle->GetSystem().GetMemory().CopyFromEmuSwapped(m_cmdlist.data(), addr,
                                                       size * sizeof(u16));
}

void AXUCode::Update()
{
  if (NeedsResumeMail())
    m_mail_handler.PushMail(DSP_RESUME, true);
}

void AXUCode::DoAXState(PointerWrap& p)
{
  p.Do(m_cmdlist);
  p.Do(m_cmdlist_size);
  p.Do(m_mail_state);
  p.Do(m_buses);

  const std::optional<u32> coefficients_before = m_coeffs_checksum;
  p.Do(m_coeffs_checksum);
  if (p.IsReadMode() && m_coeffs_checksum && m_coeffs_checksum != coefficients_before)
  {
    // The state was made with other coefficient tables: find the same ones.
    if (!LoadResamplingCoefficients(true, *m_coeffs_checksum))
    {
      Core::DisplayMessage("Could not find the DSP polyphase resampling coefficients used by the "
                           "savestate. Aborting load state.",
                           3000);
      p.SetVerifyMode();
      return;
    }
  }

  p.Do(m_compressor_pos);
  m_accelerator->DoState(p);
}

void AXUCode::DoState(PointerWrap& p)
{
  DoStateShared(p);
  DoAXState(p);
}
}  // namespace DSP::HLE
