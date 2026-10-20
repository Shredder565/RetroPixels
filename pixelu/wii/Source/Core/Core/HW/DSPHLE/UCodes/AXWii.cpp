// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/AXWii.h"

#include <array>
#include <cstddef>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/AXStructs.h"
#include "Core/HW/DSPHLE/UCodes/AXVoice.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace DSP::HLE
{
using AXVoice::AddWrapped;
using AXVoice::Clamp16;
using AXVoice::Join16;

namespace
{
// Builds by their PB layout:
//  - Wii Sports (PAL) and the startup menu: old opcodes, PB updates, high-pass filter.
//  - System Menu 1.0 and Elebits: no updates field, high-pass filter.
//  - Raving Rabbids and Bleach: no updates field, biquad filter (and the new filters).
constexpr u32 CRC_WII_SPORTS_PAL = 0xfa450138;
constexpr u32 CRC_STARTUP_MENU = 0x7699af32;
constexpr u32 CRC_SYSTEM_MENU_1_0 = 0xd9c4bf34;
constexpr u32 CRC_ELEBITS = 0xadbc06bd;
constexpr u32 CRC_RAVING_RABBIDS = 0x347112ba;
constexpr u32 CRC_BLEACH = 0x4cc52064;

enum class PBLayout
{
  UpdatesAndHighPass,
  HighPassOnly,
  BiquadOnly,
  Unknown,
};

PBLayout LayoutFor(u32 crc)
{
  switch (crc)
  {
  case CRC_WII_SPORTS_PAL:
  case CRC_STARTUP_MENU:
    return PBLayout::UpdatesAndHighPass;
  case CRC_SYSTEM_MENU_1_0:
  case CRC_ELEBITS:
    return PBLayout::HighPassOnly;
  case CRC_RAVING_RABBIDS:
  case CRC_BLEACH:
    return PBLayout::BiquadOnly;
  default:
    return PBLayout::Unknown;
  }
}

// Byte ranges of the full AXPBWii struct that a layout leaves out in RAM (where the struct
// gets zeros).
constexpr size_t UPDATES_BEGIN = offsetof(AXPBWii, updates);
constexpr size_t UPDATES_END = UPDATES_BEGIN + sizeof(PBUpdatesWii);
constexpr size_t FILTER_GAP_BEGIN = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
constexpr size_t FILTER_GAP_END = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);

struct Segment
{
  size_t begin;
  size_t end;
};

// The parts of the struct that exist in RAM for a layout, in order.
size_t PresentSegments(PBLayout layout, std::array<Segment, 3>& out)
{
  switch (layout)
  {
  case PBLayout::UpdatesAndHighPass:
    out = {{{0, FILTER_GAP_BEGIN}, {FILTER_GAP_END, sizeof(AXPBWii)}}};
    return 2;
  case PBLayout::HighPassOnly:
    out = {{{0, UPDATES_BEGIN},
            {UPDATES_END, FILTER_GAP_BEGIN},
            {FILTER_GAP_END, sizeof(AXPBWii)}}};
    return 3;
  case PBLayout::BiquadOnly:
    out = {{{0, UPDATES_BEGIN}, {UPDATES_END, sizeof(AXPBWii)}}};
    return 2;
  case PBLayout::Unknown:
    break;
  }
  return 0;
}
}  // namespace

AXWiiUCode::AXWiiUCode(DSPHLE* dsphle, u32 crc) : AXUCode(dsphle, crc, WiiVariant{})
{
  INFO_LOG_FMT(DSPHLE, "AX (Wii) ucode {:08x}", crc);

  m_old_axwii = crc == CRC_WII_SPORTS_PAL || crc == CRC_STARTUP_MENU;
  m_new_filter = crc == CRC_RAVING_RABBIDS || crc == CRC_BLEACH;
  m_accelerator =
      std::make_unique<AXVoice::VoiceAccelerator<AXPBWii>>(dsphle->GetSystem().GetDSP());
}

void AXWiiUCode::Initialize()
{
  InitializeShared();
}

AXWiiUCode::Op AXWiiUCode::DecodeOp(u16 opcode) const
{
  // The early numbering has a separate "set PB address" command, which shifts everything after
  // it by one; the output commands there take no volume.
  static constexpr Op OLD_OPS[] = {
      Op::Setup,          Op::AddToLR,           Op::SubToLR,           Op::AddSubToLR,
      Op::SetPBAddr,      Op::ProcessStoredPBs,  Op::MixAuxA,           Op::MixAuxB,
      Op::MixAuxC,        Op::UploadAuxAMixLRSC, Op::UploadAuxBMixLRSC, Op::Compressor,
      Op::Output,         Op::OutputDPL2,        Op::OutputRemotes,     Op::End,
  };
  static constexpr Op NEW_OPS[] = {
      Op::Setup,          Op::AddToLR,           Op::SubToLR,       Op::AddSubToLR,
      Op::ProcessPBs,     Op::MixAuxA,           Op::MixAuxB,       Op::MixAuxC,
      Op::UploadAuxAMixLRSC, Op::UploadAuxBMixLRSC, Op::Compressor, Op::Output,
      Op::OutputDPL2,     Op::OutputRemotes,     Op::End,
  };

  if (m_old_axwii)
    return opcode < std::size(OLD_OPS) ? OLD_OPS[opcode] : Op::Ignored;
  return opcode < std::size(NEW_OPS) ? NEW_OPS[opcode] : Op::Ignored;
}

void AXWiiUCode::HandleCommandList()
{
  u32 stored_pb_addr = 0;
  CommandReader in(m_cmdlist);

  while (!in.AtEnd())
  {
    const Op op = DecodeOp(in.Word());
    switch (op)
    {
    case Op::Setup:
      SetupProcessing(in.Address());
      break;

    case Op::AddToLR:
    case Op::SubToLR:
      AddToLR(in.Address(), op == Op::SubToLR);
      break;

    case Op::AddSubToLR:
      AddSubToLR(in.Address());
      break;

    case Op::SetPBAddr:
      stored_pb_addr = in.Address();
      break;

    case Op::ProcessStoredPBs:
      ProcessPBList(stored_pb_addr);
      break;

    case Op::ProcessPBs:
      ProcessPBList(in.Address());
      break;

    case Op::MixAuxA:
    case Op::MixAuxB:
    case Op::MixAuxC:
    {
      const u16 volume = in.Word();
      const u32 write_addr = in.Address();
      const u32 read_addr = in.Address();
      const int aux_id = op == Op::MixAuxA ? 0 : op == Op::MixAuxB ? 1 : 2;
      MixAUXSamples(aux_id, write_addr, read_addr, volume);
      break;
    }

    case Op::UploadAuxAMixLRSC:
    case Op::UploadAuxBMixLRSC:
    {
      const u16 volume = in.Word();
      u32 addresses[6];
      for (u32& address : addresses)
        address = in.Address();
      UploadAUXMixLRSC(op == Op::UploadAuxBMixLRSC ? 1 : 0, addresses, volume);
      break;
    }

    case Op::Compressor:
    {
      const u16 threshold = in.Word();
      const u16 release_frames = in.Word();
      const u32 table_addr = in.Address();
      RunCompressor(threshold, release_frames, table_addr, 3);
      break;
    }

    case Op::Output:
    case Op::OutputDPL2:
    {
      // Early builds and System Menu 1.0 have no output volume.
      const bool has_volume = !m_old_axwii && m_crc != CRC_SYSTEM_MENU_1_0;
      const u16 volume = has_volume ? in.Word() : u16{0x8000};
      const u32 surround_addr = in.Address();
      const u32 lr_addr = in.Address();
      OutputSamples(lr_addr, surround_addr, volume, op == Op::OutputDPL2);
      break;
    }

    case Op::OutputRemotes:
    {
      u32 addresses[4];
      for (u32& address : addresses)
        address = in.Address();
      OutputWMSamples(addresses);
      break;
    }

    case Op::End:
      return;

    case Op::Ignored:
      // Unknown words are skipped one at a time.
      break;
    }
  }
  ERROR_LOG_FMT(DSPHLE, "AXWii: command list without an end command");
}

int* AXWiiUCode::AuxBus(int aux_id, size_t channel)
{
  switch (aux_id)
  {
  case 0:
    return BusData(BUS_AUXA_L + channel);
  case 1:
    return BusData(BUS_AUXB_L + channel);
  default:
    return m_auxc[AUXC_L + channel].data();
  }
}

void AXWiiUCode::SetupProcessing(u32 init_addr)
{
  std::array<RampTarget, 20> targets;
  size_t n = 0;
  for (size_t bus = 0; bus < NUM_BUSES; ++bus)
    targets[n++] = {BusData(bus), FRAME_SAMPLES};
  for (auto& bus : m_auxc)
    targets[n++] = {bus.data(), FRAME_SAMPLES};
  for (size_t speaker = 0; speaker < 4; ++speaker)
  {
    targets[n++] = {RemoteMain(speaker), REMOTE_FRAME_SAMPLES};
    targets[n++] = {RemoteAux(speaker), REMOTE_FRAME_SAMPLES};
  }
  InitBuffersFromRamps(init_addr, targets.data(), n);
}

namespace
{
s32 ReadSampleBE(const u8* buffer, size_t index)
{
  u32 raw;
  std::memcpy(&raw, buffer + index * sizeof(u32), sizeof(raw));
  return static_cast<s32>(Common::swap32(raw));
}

constexpr int Negate(s32 value)
{
  return static_cast<int>(0u - static_cast<u32>(value));
}
}  // namespace

void AXWiiUCode::AddToLR(u32 val_addr, bool neg)
{
  const u8* source = m_dsphle->GetSystem().GetMemory().GetPointerForRange(
      val_addr, FRAME_SAMPLES * sizeof(u32));
  int* left = BusData(BUS_MAIN_L);
  int* right = BusData(BUS_MAIN_R);
  for (size_t i = 0; i < FRAME_SAMPLES; ++i)
  {
    const s32 sample = ReadSampleBE(source, i);
    const s32 value = neg ? Negate(sample) : sample;
    left[i] = AddWrapped(left[i], value);
    right[i] = AddWrapped(right[i], value);
  }
}

void AXWiiUCode::AddSubToLR(u32 val_addr)
{
  const u8* source = m_dsphle->GetSystem().GetMemory().GetPointerForRange(
      val_addr, 2 * FRAME_SAMPLES * sizeof(u32));
  int* left = BusData(BUS_MAIN_L);
  int* right = BusData(BUS_MAIN_R);
  for (size_t i = 0; i < FRAME_SAMPLES; ++i)
    left[i] = AddWrapped(left[i], ReadSampleBE(source, i));
  for (size_t i = 0; i < FRAME_SAMPLES; ++i)
    right[i] = AddWrapped(right[i], Negate(ReadSampleBE(source, FRAME_SAMPLES + i)));
}

u32 AXWiiUCode::ConvertMixerControl(u32 mixer_control)
{
  // Each bus has "left", "right", "left+right with ramp", "surround" and "surround with ramp"
  // bits; main in the low half, aux A/B/C in the high half.
  struct BitMeaning
  {
    u32 bit;
    u32 flags;
  };
  static constexpr BitMeaning MEANINGS[] = {
      {0x00000001, MIX_MAIN_L},
      {0x00000002, MIX_MAIN_R},
      {0x00000004, MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP},
      {0x00000008, MIX_MAIN_S},
      {0x00000010, MIX_MAIN_S | MIX_MAIN_S_RAMP},
      {0x00010000, MIX_AUXA_L},
      {0x00020000, MIX_AUXA_R},
      {0x00040000, MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP},
      {0x00080000, MIX_AUXA_S},
      {0x00100000, MIX_AUXA_S | MIX_AUXA_S_RAMP},
      {0x00200000, MIX_AUXB_L},
      {0x00400000, MIX_AUXB_R},
      {0x00800000, MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP},
      {0x01000000, MIX_AUXB_S},
      {0x02000000, MIX_AUXB_S | MIX_AUXB_S_RAMP},
      {0x04000000, MIX_AUXC_L},
      {0x08000000, MIX_AUXC_R},
      {0x10000000, MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP},
      {0x20000000, MIX_AUXC_S},
      {0x40000000, MIX_AUXC_S | MIX_AUXC_S_RAMP},
  };
  u32 mix = 0;
  for (const BitMeaning& meaning : MEANINGS)
  {
    if ((mixer_control & meaning.bit) != 0)
      mix |= meaning.flags;
  }
  return mix;
}

void AXWiiUCode::GenerateVolumeRamp(u16* output, u16 from, u16 to, size_t count)
{
  const float step = static_cast<float>(to - from) / static_cast<float>(count);
  float level = from;
  for (size_t i = 0; i < count; ++i)
  {
    level += step;
    output[i] = static_cast<u16>(level);
  }
}

void AXWiiUCode::ReadPB(Memory::MemoryManager& memory, u32 addr, AXPBWii& pb) const
{
  std::array<Segment, 3> segments;
  const size_t count = PresentSegments(LayoutFor(m_crc), segments);
  if (count == 0)
    return;

  // Fields missing from this layout read as zero.
  u8* const bytes = reinterpret_cast<u8*>(&pb);
  size_t ram_offset = 0;
  size_t struct_offset = 0;
  for (size_t i = 0; i < count; ++i)
  {
    const Segment& segment = segments[i];
    std::memset(bytes + struct_offset, 0, segment.begin - struct_offset);
    const size_t length = segment.end - segment.begin;
    memory.CopyFromEmuSwapped(reinterpret_cast<u16*>(bytes + segment.begin),
                              addr + static_cast<u32>(ram_offset), length);
    ram_offset += length;
    struct_offset = segment.end;
  }
}

void AXWiiUCode::WritePB(Memory::MemoryManager& memory, u32 addr, const AXPBWii& pb) const
{
  std::array<Segment, 3> segments;
  const size_t count = PresentSegments(LayoutFor(m_crc), segments);

  const u8* const bytes = reinterpret_cast<const u8*>(&pb);
  size_t ram_offset = 0;
  for (size_t i = 0; i < count; ++i)
  {
    const size_t length = segments[i].end - segments[i].begin;
    memory.CopyToEmuSwapped(addr + static_cast<u32>(ram_offset),
                            reinterpret_cast<const u16*>(bytes + segments[i].begin), length);
    ram_offset += length;
  }
}

void AXWiiUCode::ProcessPBList(u32 pb_addr)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  auto& accelerator = static_cast<AXVoice::VoiceAccelerator<AXPBWii>&>(*m_accelerator);

  // Buses for millisecond `ms` (0 for a whole frame).
  const auto targets_from = [this](size_t ms) {
    AXVoice::VoiceTargets<AXPBWii> targets;
    for (size_t bus = 0; bus < NUM_BUSES; ++bus)
      targets.buses[bus] = BusData(bus) + ms * SAMPLES_PER_MS;
    for (size_t i = 0; i < 3; ++i)
      targets.buses[NUM_BUSES + i] = m_auxc[i].data() + ms * SAMPLES_PER_MS;
    for (size_t speaker = 0; speaker < 4; ++speaker)
    {
      targets.remote[2 * speaker] = RemoteMain(speaker) + ms * REMOTE_SAMPLES_PER_MS;
      targets.remote[2 * speaker + 1] = RemoteAux(speaker) + ms * REMOTE_SAMPLES_PER_MS;
    }
    return targets;
  };

  AXPBWii pb{};
  while (pb_addr != 0)
  {
    ReadPB(memory, pb_addr, pb);

    const bool has_updates = (pb.updates.num_updates[0] | pb.updates.num_updates[1] |
                              pb.updates.num_updates[2]) != 0;
    if (m_old_axwii && has_updates)
    {
      // Updates are per millisecond, so the frame is rendered a millisecond at a time.
      const PBUpdateData updates = AXVoice::FetchUpdateList(memory, pb);
      for (u32 ms = 0; ms < 3; ++ms)
      {
        AXVoice::ApplyUpdatesForMillisecond(pb, ms, updates);
        AXVoice::RenderVoice(accelerator, pb, targets_from(ms), SAMPLES_PER_MS,
                             ConvertMixerControl(Join16(pb.mixer_control_hi, pb.mixer_control_lo)),
                             CoefficientTables(), m_new_filter);
      }
    }
    else
    {
      AXVoice::RenderVoice(accelerator, pb, targets_from(0), FRAME_SAMPLES,
                           ConvertMixerControl(Join16(pb.mixer_control_hi, pb.mixer_control_lo)),
                           CoefficientTables(), m_new_filter);
    }

    WritePB(memory, pb_addr, pb);
    pb_addr = Join16(pb.next_pb_hi, pb.next_pb_lo);
  }
}

void AXWiiUCode::MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr, u16 volume)
{
  std::array<u16, FRAME_SAMPLES> ramp;
  GenerateVolumeRamp(ramp.data(), m_last_aux_volumes[static_cast<size_t>(aux_id)], volume, ramp.size());
  m_last_aux_volumes[static_cast<size_t>(aux_id)] = volume;

  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = FRAME_SAMPLES * sizeof(int);

  // Send the aux bus to the CPU...
  if (write_addr != 0)
  {
    for (size_t channel = 0; channel < 3; ++channel)
    {
      memory.CopyToEmuSwapped(write_addr + static_cast<u32>(channel) * BUS_BYTES,
                              AuxBus(aux_id, channel), BUS_BYTES);
    }
  }

  // ...and add what comes back to main, through the volume ramp.
  const u8* returned = memory.GetPointerForRange(read_addr, 3 * BUS_BYTES);
  for (size_t channel = 0; channel < 3; ++channel)
  {
    int* main = BusData(BUS_MAIN_L + channel);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i)
    {
      const s64 scaled = s64{ReadSampleBE(returned, channel * FRAME_SAMPLES + i)} * ramp[i];
      main[i] = AddWrapped(main[i], static_cast<s32>(scaled >> 15));
    }
  }
}

void AXWiiUCode::UploadAUXMixLRSC(int aux_id, const u32* addresses, u16 volume)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = FRAME_SAMPLES * sizeof(int);

  // Upload aux L/R/S, plus aux C R (for aux A) or aux C S (for aux B).
  int* aux_left = AuxBus(aux_id, 0);
  for (size_t channel = 0; channel < 3; ++channel)
  {
    memory.CopyToEmuSwapped(addresses[0] + static_cast<u32>(channel) * BUS_BYTES,
                            AuxBus(aux_id, channel), BUS_BYTES);
  }
  memory.CopyToEmuSwapped(addresses[1], m_auxc[aux_id != 0 ? AUXC_S : AUXC_R].data(), BUS_BYTES);

  std::array<u16, FRAME_SAMPLES> ramp;
  GenerateVolumeRamp(ramp.data(), m_last_aux_volumes[static_cast<size_t>(aux_id)], volume, ramp.size());
  m_last_aux_volumes[static_cast<size_t>(aux_id)] = volume;

  // Four returned buffers mix into main L, R, S and aux C L. Each is downloaded into the aux L
  // bus first, which therefore ends up holding the last one.
  int* const destinations[4] = {BusData(BUS_MAIN_L), BusData(BUS_MAIN_R), BusData(BUS_MAIN_S),
                                m_auxc[AUXC_L].data()};
  for (size_t d = 0; d < 4; ++d)
  {
    memory.CopyFromEmuSwapped(aux_left, addresses[2 + d], BUS_BYTES);
    for (size_t i = 0; i < FRAME_SAMPLES; ++i)
    {
      const s64 scaled = s64{aux_left[i]} * ramp[i];
      destinations[d][i] = AddWrapped(destinations[d][i], static_cast<s32>(scaled >> 15));
    }
  }
}

void AXWiiUCode::OutputSamples(u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc)
{
  std::array<u16, FRAME_SAMPLES> ramp;
  GenerateVolumeRamp(ramp.data(), m_last_main_volume, volume, ramp.size());
  m_last_main_volume = volume;

  auto& memory = m_dsphle->GetSystem().GetMemory();
  constexpr u32 BUS_BYTES = FRAME_SAMPLES * sizeof(int);
  memory.CopyToEmuSwapped(surround_addr, BusData(BUS_MAIN_S), BUS_BYTES);
  if (upload_auxc)
    memory.CopyToEmuSwapped(surround_addr + BUS_BYTES, m_auxc[AUXC_L].data(), BUS_BYTES);

  // The output volume is applied to main L/R in place, clamped to 16 bits.
  int* left = BusData(BUS_MAIN_L);
  int* right = BusData(BUS_MAIN_R);
  std::array<u16, FRAME_SAMPLES * 2> frame;
  for (size_t i = 0; i < FRAME_SAMPLES; ++i)
  {
    left[i] = Clamp16((s64{left[i]} * ramp[i]) >> 15);
    right[i] = Clamp16((s64{right[i]} * ramp[i]) >> 15);
    frame[2 * i] = Common::swap16(static_cast<u16>(right[i]));
    frame[2 * i + 1] = Common::swap16(static_cast<u16>(left[i]));
  }
  memory.CopyToEmu(lr_addr, frame.data(), sizeof(frame));

  m_mail_handler.PushMail(DSP_SYNC, true);
}

void AXWiiUCode::OutputWMSamples(const u32* addresses)
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  for (size_t speaker = 0; speaker < 4; ++speaker)
  {
    const int* samples = RemoteMain(speaker);
    u8* out = memory.GetPointerForRange(addresses[speaker], REMOTE_FRAME_SAMPLES * sizeof(u16));
    for (size_t i = 0; i < REMOTE_FRAME_SAMPLES; ++i)
    {
      const u16 value = Common::swap16(static_cast<u16>(Clamp16(samples[i])));
      std::memcpy(out + i * sizeof(u16), &value, sizeof(value));
    }
  }
}

void AXWiiUCode::DoState(PointerWrap& p)
{
  DoStateShared(p);
  DoAXState(p);
  p.Do(m_auxc);
  p.Do(m_remote);
  p.Do(m_last_main_volume);
  p.Do(m_last_aux_volumes);
}
}  // namespace DSP::HLE
