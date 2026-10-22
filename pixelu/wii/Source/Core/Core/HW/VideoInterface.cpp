// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/VideoInterface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PerformanceMetrics.h"

#include "Core/AchievementManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"

#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace VideoInterface
{
namespace
{
// A bit field inside a register value.
template <unsigned Shift, unsigned Width>
struct Bits
{
  static constexpr u32 MASK = ((1u << Width) - 1) << Shift;

  template <typename Reg>
  static constexpr u32 Get(Reg reg)
  {
    return (static_cast<u32>(reg) & MASK) >> Shift;
  }
  template <typename Reg>
  static constexpr void Set(Reg& reg, u32 value)
  {
    reg = static_cast<Reg>((static_cast<u32>(reg) & ~MASK) | ((value << Shift) & MASK));
  }
};

// Vertical timing.
using EQU = Bits<0, 4>;   // equalisation pulse, in half lines
using ACV = Bits<4, 10>;  // active video, in lines per field
// Display control.
using ENB = Bits<0, 1>;
using FMT = Bits<8, 2>;  // 0 NTSC, 1 PAL, 2 MPAL, 3 debug
constexpr u16 CONTROL_RESET = 0x0002;
constexpr u16 CONTROL_WRITABLE = 0x03FD;  // ENB, NIN, DLR, LE0, LE1, FMT
// Horizontal timing 0 and 1.
using HLW = Bits<0, 10>;  // half line width
using HCE = Bits<16, 7>;
using HCS = Bits<24, 7>;
using HSY = Bits<0, 7>;
using HBE640 = Bits<7, 10>;
using HBS640 = Bits<17, 10>;
// Vertical blanking (per field).
using PRB = Bits<0, 10>;   // pre-blanking, in half lines
using PSB = Bits<16, 10>;  // post-blanking, in half lines
// Burst blanking (per field).
using BS0 = Bits<0, 5>;
using BE0 = Bits<5, 11>;
using BS2 = Bits<16, 5>;
using BE2 = Bits<21, 11>;
// Frame buffer address.
using FBB = Bits<0, 24>;
using POFF = Bits<28, 1>;     // address is in units of 32 bytes
using CLRPOFF = Bits<29, 3>;  // writing any of these clears POFF
// Display interrupt.
using HCT = Bits<0, 11>;
using VCT = Bits<16, 11>;
using IR_MASK = Bits<28, 1>;
using IR_INT = Bits<31, 1>;
// Picture configuration.
using STD = Bits<0, 8>;  // stride, in 16-pixel units
using WPL = Bits<8, 7>;  // width, in 16-pixel units
// DTV status.
using COMPONENT_PLUGGED = Bits<0, 1>;
using NTSC_J = Bits<1, 1>;

constexpr std::array<u32, 2> CLOCK_FREQUENCIES{{27000000, 54000000}};

// Length of an SI poll, in half lines.
constexpr u32 NUM_HALF_LINES_FOR_SI_POLL = 7 * 2 + 1;

u16* High(u32& reg)
{
  return MMIO::Utils::HighPart(&reg);
}
u16* Low(u32& reg)
{
  return MMIO::Utils::LowPart(&reg);
}
// Saves or loads each field in turn.
template <typename... Fields>
void DoFields(PointerWrap& p, Fields&... fields)
{
  (p.Do(fields), ...);
}
}  // namespace

VideoInterfaceManager::VideoInterfaceManager(Core::System& system) : m_system(system)
{
}

VideoInterfaceManager::~VideoInterfaceManager()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

void VideoInterfaceManager::DoState(PointerWrap& p)
{
  p.Do(m_vertical_timing);
  p.Do(m_display_control);
  for (u32* reg : {&m_h_timing_0, &m_h_timing_1, &m_vblank_timing_odd, &m_vblank_timing_even,
                   &m_burst_blanking_odd, &m_burst_blanking_even, &m_xfb_info_top,
                   &m_xfb_info_bottom, &m_xfb_3d_info_top, &m_xfb_3d_info_bottom})
  {
    p.Do(*reg);
  }
  p.DoArray(m_interrupt_register);
  p.DoArray(m_latch_register);
  p.Do(m_picture_configuration);
  p.Do(m_horizontal_scaling);
  p.DoArray(m_filter_taps_10bit);
  p.DoArray(m_filter_taps_8bit);
  DoFields(p, m_unknown_aa_register, m_clock, m_dtv_status, m_fb_width, m_border_hblank);
  // Derived timing state.
  DoFields(p, m_target_refresh_rate, m_target_refresh_rate_numerator,
           m_target_refresh_rate_denominator, m_ticks_last_line_start, m_half_line_count,
           m_half_line_of_next_si_poll, m_even_field_first_hl, m_odd_field_first_hl,
           m_even_field_last_hl, m_odd_field_last_hl);
}

// The boot ROM's video setup (480i NTSC or 576i PAL timings). Fields not
// listed keep their bits, as on the console.
void VideoInterfaceManager::Preset(bool _bNTSC)
{
  EQU::Set(m_vertical_timing, 6);
  ACV::Set(m_vertical_timing, 0);

  // Video on, interlaced, no 3D, latches off.
  m_display_control = static_cast<u16>((m_display_control & ~0x03FFu) | 0x0001u);
  FMT::Set(m_display_control, _bNTSC ? 0 : 1);

  HLW::Set(m_h_timing_0, 429);
  HCE::Set(m_h_timing_0, 105);
  HCS::Set(m_h_timing_0, 71);
  HSY::Set(m_h_timing_1, 64);
  HBE640::Set(m_h_timing_1, 162);
  HBS640::Set(m_h_timing_1, 373);

  PRB::Set(m_vblank_timing_odd, 502);
  PSB::Set(m_vblank_timing_odd, 5);
  PRB::Set(m_vblank_timing_even, 503);
  PSB::Set(m_vblank_timing_even, 4);

  for (auto [reg, bs, be] : {std::tuple{&m_burst_blanking_odd, 12u, 520u},
                             std::tuple{&m_burst_blanking_even, 13u, 519u}})
  {
    BS0::Set(*reg, bs);
    BE0::Set(*reg, be);
    BS2::Set(*reg, bs);
    BE2::Set(*reg, be);
  }

  m_xfb_info_top = m_xfb_info_bottom = 0;
  m_xfb_3d_info_top = m_xfb_3d_info_bottom = 0;

  // Pre-retrace at line 263, post-retrace at line 1; both enabled.
  const u32 interrupt_positions[2][2] = {{430, 263}, {1, 1}};
  for (size_t i = 0; i < 2; i++)
  {
    u32& reg = m_interrupt_register[i];
    HCT::Set(reg, interrupt_positions[i][0]);
    VCT::Set(reg, interrupt_positions[i][1]);
    IR_MASK::Set(reg, 1);
    IR_INT::Set(reg, 0);
  }
  m_interrupt_register[2] = m_interrupt_register[3] = 0;
  m_latch_register = {};

  STD::Set(m_picture_configuration, 40);
  WPL::Set(m_picture_configuration, 40);

  m_horizontal_scaling = 0;
  m_filter_taps_10bit = {};
  m_filter_taps_8bit = {};
  m_unknown_aa_register = 0;

  const DiscIO::Region region = SConfig::GetInstance().m_region;
  // NTSC consoles run the 54 MHz clock (progressive scan capable).
  m_clock = DiscIO::IsNTSC(region);
  COMPONENT_PLUGGED::Set(m_dtv_status, Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN));
  // The Triforce IPL needs the NTSC-J flag.
  NTSC_J::Set(m_dtv_status, m_system.IsTriforce() || region == DiscIO::Region::NTSC_J);

  m_fb_width = 0;
  m_border_hblank = 0;

  m_half_line_count = 0;
  m_half_line_of_next_si_poll = NUM_HALF_LINES_FOR_SI_POLL;
  m_ticks_last_line_start = 0;

  UpdateParameters();
}

void VideoInterfaceManager::Init()
{
  Preset(true);
  // Registered first: RefreshConfig() may itself change the configuration.
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
}

void VideoInterfaceManager::RefreshConfig()
{
  float factor = 1.0f;
  if (Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE))
    factor = Config::Get(Config::MAIN_VI_OVERCLOCK);
  m_config_vi_oc_factor = factor;
  // Hardcore achievements do not allow slowing the VBI down.
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  if (hardcore && m_config_vi_oc_factor < 1.0f)
  {
    Config::SetCurrent(Config::MAIN_VI_OVERCLOCK, 1.0f);
    m_config_vi_oc_factor = 1.0f;
    OSD::AddMessage("Minimum VBI frequency is 100% in Hardcore Mode");
  }
  UpdateRefreshRate();
}

// --- Registers ----------------------------------------------------------------------

void VideoInterfaceManager::WriteControl(u16 value)
{
  m_display_control =
      static_cast<u16>((m_display_control & ~CONTROL_WRITABLE) | (value & CONTROL_WRITABLE));

  // Reset: idle the VI and drop all display interrupts.
  if (value & CONTROL_RESET)
  {
    m_display_control &= ~CONTROL_RESET;
    m_interrupt_register = {};
    UpdateInterrupts();
  }

  UpdateParameters();
}

void VideoInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  const auto plain = [&](u32 offset, u16* field) {
    mmio->Register(base | offset, MMIO::DirectRead<u16>(field), MMIO::DirectWrite<u16>(field));
  };
  // Registers whose writes run `after_write` once stored.
  using Action = void (*)(VideoInterfaceManager&);
  const auto with_action = [&](u32 offset, u16* field, Action after_write) {
    mmio->Register(base | offset, MMIO::DirectRead<u16>(field),
                   MMIO::ComplexWrite<u16>([field, after_write](Core::System& system, u32, u16 val) {
                     *field = val;
                     after_write(system.GetVideoInterface());
                   }));
  };
  const Action update_parameters = [](VideoInterfaceManager& vi) { vi.UpdateParameters(); };
  const Action update_interrupts = [](VideoInterfaceManager& vi) { vi.UpdateInterrupts(); };

  // Timing registers that change the field layout.
  with_action(VI_VERTICAL_TIMING, &m_vertical_timing, update_parameters);
  with_action(VI_HORIZONTAL_TIMING_0_HI, High(m_h_timing_0), update_parameters);
  with_action(VI_HORIZONTAL_TIMING_0_LO, Low(m_h_timing_0), update_parameters);
  with_action(VI_VBLANK_TIMING_ODD_HI, High(m_vblank_timing_odd), update_parameters);
  with_action(VI_VBLANK_TIMING_ODD_LO, Low(m_vblank_timing_odd), update_parameters);
  with_action(VI_VBLANK_TIMING_EVEN_HI, High(m_vblank_timing_even), update_parameters);
  with_action(VI_VBLANK_TIMING_EVEN_LO, Low(m_vblank_timing_even), update_parameters);
  with_action(VI_CLOCK, &m_clock, update_parameters);

  // Plain storage.
  plain(VI_HORIZONTAL_TIMING_1_HI, High(m_h_timing_1));
  plain(VI_HORIZONTAL_TIMING_1_LO, Low(m_h_timing_1));
  plain(VI_BURST_BLANKING_ODD_HI, High(m_burst_blanking_odd));
  plain(VI_BURST_BLANKING_ODD_LO, Low(m_burst_blanking_odd));
  plain(VI_BURST_BLANKING_EVEN_HI, High(m_burst_blanking_even));
  plain(VI_BURST_BLANKING_EVEN_LO, Low(m_burst_blanking_even));
  plain(VI_FB_LEFT_TOP_LO, Low(m_xfb_info_top));
  plain(VI_FB_RIGHT_TOP_LO, Low(m_xfb_3d_info_top));
  plain(VI_FB_LEFT_BOTTOM_LO, Low(m_xfb_info_bottom));
  plain(VI_FB_RIGHT_BOTTOM_LO, Low(m_xfb_3d_info_bottom));
  plain(VI_PRERETRACE_LO, Low(m_interrupt_register[0]));
  plain(VI_POSTRETRACE_LO, Low(m_interrupt_register[1]));
  plain(VI_DISPLAY_INTERRUPT_2_LO, Low(m_interrupt_register[2]));
  plain(VI_DISPLAY_INTERRUPT_3_LO, Low(m_interrupt_register[3]));
  plain(VI_DISPLAY_LATCH_0_HI, High(m_latch_register[0]));
  plain(VI_DISPLAY_LATCH_0_LO, Low(m_latch_register[0]));
  plain(VI_DISPLAY_LATCH_1_HI, High(m_latch_register[1]));
  plain(VI_DISPLAY_LATCH_1_LO, Low(m_latch_register[1]));
  plain(VI_HSCALEW, &m_picture_configuration);
  plain(VI_HSCALER, &m_horizontal_scaling);
  for (u32 i = 0; i < 3; i++)
  {
    plain(VI_FILTER_COEF_0_HI + 4 * i, High(m_filter_taps_10bit[i]));
    plain(VI_FILTER_COEF_0_LO + 4 * i, Low(m_filter_taps_10bit[i]));
  }
  for (u32 i = 0; i < 4; i++)
  {
    plain(VI_FILTER_COEF_3_HI + 4 * i, High(m_filter_taps_8bit[i]));
    plain(VI_FILTER_COEF_3_LO + 4 * i, Low(m_filter_taps_8bit[i]));
  }
  plain(VI_DTV_STATUS, &m_dtv_status);
  plain(VI_FBWIDTH, &m_fb_width);
  // The end register holds the low half, the start register the high half.
  plain(VI_BORDER_BLANK_END, Low(m_border_hblank));
  plain(VI_BORDER_BLANK_START, High(m_border_hblank));

  // Frame buffer address high halves: the CLRPOFF bits clear POFF.
  using FrameBufferRegister = u32 VideoInterfaceManager::*;
  const std::pair<u32, FrameBufferRegister> frame_buffer_highs[] = {
      {VI_FB_LEFT_TOP_HI, &VideoInterfaceManager::m_xfb_info_top},
      {VI_FB_RIGHT_TOP_HI, &VideoInterfaceManager::m_xfb_3d_info_top},
      {VI_FB_LEFT_BOTTOM_HI, &VideoInterfaceManager::m_xfb_info_bottom},
      {VI_FB_RIGHT_BOTTOM_HI, &VideoInterfaceManager::m_xfb_3d_info_bottom},
  };
  for (const auto& [offset, member] : frame_buffer_highs)
  {
    mmio->Register(base | offset, MMIO::DirectRead<u16>(High(this->*member)),
                   MMIO::ComplexWrite<u16>([member](Core::System& system, u32, u16 val) {
                     u32& fb = system.GetVideoInterface().*member;
                     *High(fb) = val;
                     if (CLRPOFF::Get(fb) != 0)
                       POFF::Set(fb, 0);
                   }));
  }

  // Display interrupt high halves hold the status and mask bits.
  with_action(VI_PRERETRACE_HI, High(m_interrupt_register[0]), update_interrupts);
  with_action(VI_POSTRETRACE_HI, High(m_interrupt_register[1]), update_interrupts);
  with_action(VI_DISPLAY_INTERRUPT_2_HI, High(m_interrupt_register[2]), update_interrupts);
  with_action(VI_DISPLAY_INTERRUPT_3_HI, High(m_interrupt_register[3]), update_interrupts);

  // Beam position (writes are not supported).
  mmio->Register(base | VI_VERTICAL_BEAM_POSITION,
                 MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return static_cast<u16>(1 + system.GetVideoInterface().m_half_line_count / 2);
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System&, u32, u16 val) {
                   WARN_LOG_FMT(VIDEOINTERFACE, "Unsupported write of {:#06x} to the vertical beam "
                                                "position",
                                val);
                 }));
  mmio->Register(base | VI_HORIZONTAL_BEAM_POSITION,
                 MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   auto& vi = system.GetVideoInterface();
                   const u64 since_line_start =
                       system.GetCoreTiming().GetTicks() - vi.m_ticks_last_line_start;
                   const u32 half_line_width = HLW::Get(vi.m_h_timing_0);
                   const u16 position = static_cast<u16>(
                       1 + half_line_width * since_line_start / vi.GetTicksPerHalfLine());
                   return std::clamp<u16>(position, 1, static_cast<u16>(half_line_width * 2));
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System&, u32, u16 val) {
                   WARN_LOG_FMT(VIDEOINTERFACE, "Unsupported write of {:#06x} to the horizontal "
                                                "beam position",
                                val);
                 }));

  // Anti-aliasing related register of unknown purpose.
  mmio->Register(base | VI_UNK_AA_REG_HI, MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return static_cast<u16>(system.GetVideoInterface().m_unknown_aa_register >> 16);
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   *High(system.GetVideoInterface().m_unknown_aa_register) = val;
                   WARN_LOG_FMT(VIDEOINTERFACE, "Write to unknown AA register (high half)");
                 }));
  mmio->Register(base | VI_UNK_AA_REG_LO, MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return static_cast<u16>(system.GetVideoInterface().m_unknown_aa_register);
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   *Low(system.GetVideoInterface().m_unknown_aa_register) = val;
                   WARN_LOG_FMT(VIDEOINTERFACE, "Write to unknown AA register (low half)");
                 }));

  mmio->Register(base | VI_CONTROL_REGISTER, MMIO::DirectRead<u16>(&m_display_control),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   system.GetVideoInterface().WriteControl(val);
                 }));

  // Byte reads come from the halfword registers; byte writes are not
  // supported. Word accesses are split into halfwords.
  for (u32 offset = 0; offset < 0x1000; offset += 2)
  {
    mmio->Register(base | offset, MMIO::ReadToLarger<u8>(mmio, base | offset, 8),
                   MMIO::InvalidWrite<u8>());
    mmio->Register(base | (offset + 1), MMIO::ReadToLarger<u8>(mmio, base | offset, 0),
                   MMIO::InvalidWrite<u8>());
  }
  for (u32 offset = 0; offset < 0x1000; offset += 4)
  {
    mmio->Register(base | offset,
                   MMIO::ReadToSmaller<u32>(mmio, base | offset, base | (offset + 2)),
                   MMIO::WriteToSmaller<u32>(mmio, base | offset, base | (offset + 2)));
  }
}

void VideoInterfaceManager::UpdateInterrupts()
{
  const bool active = std::ranges::any_of(m_interrupt_register, [](u32 reg) {
    return IR_INT::Get(reg) != 0 && IR_MASK::Get(reg) != 0;
  });
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_VI, active);
}

// --- Frame buffer and timing ---------------------------------------------------------

// The top field's POFF bit applies to both fields.
u32 VideoInterfaceManager::GetXFBAddressTop() const
{
  const u32 base = FBB::Get(m_xfb_info_top);
  return POFF::Get(m_xfb_info_top) ? base << 5 : base;
}

u32 VideoInterfaceManager::GetXFBAddressBottom() const
{
  const u32 base = FBB::Get(m_xfb_info_bottom);
  return POFF::Get(m_xfb_info_top) ? base << 5 : base;
}

// A field: equalisation (3 x EQU), pre-blanking, active lines (2 half lines
// each), post-blanking.
u32 VideoInterfaceManager::GetHalfLinesPerEvenField() const
{
  return 3 * EQU::Get(m_vertical_timing) + PRB::Get(m_vblank_timing_even) +
         2 * ACV::Get(m_vertical_timing) + PSB::Get(m_vblank_timing_even);
}

u32 VideoInterfaceManager::GetHalfLinesPerOddField() const
{
  return 3 * EQU::Get(m_vertical_timing) + PRB::Get(m_vblank_timing_odd) +
         2 * ACV::Get(m_vertical_timing) + PSB::Get(m_vblank_timing_odd);
}

u32 VideoInterfaceManager::GetTicksPerEvenField() const
{
  return GetTicksPerHalfLine() * GetHalfLinesPerEvenField();
}

u32 VideoInterfaceManager::GetTicksPerOddField() const
{
  return GetTicksPerHalfLine() * GetHalfLinesPerOddField();
}

// Compares the configured active area with the active area of a standard
// 625-line (PAL) or 525-line (NTSC/PAL-M) picture, which is 4:3 by
// definition. The FMT field picks the standard; the SDK always sets it to
// match the line count.
float VideoInterfaceManager::GetAspectRatio() const
{
  const int active_lines = static_cast<int>(ACV::Get(m_vertical_timing));
  const int active_samples = static_cast<int>(HLW::Get(m_h_timing_0) + HBS640::Get(m_h_timing_1) -
                                              HBE640::Get(m_h_timing_1));

  // Everything in seconds.
  const u32 ticks_per_half_line = GetNominalTicksPerHalfLine();
  const float tick = 1.0f / static_cast<float>(m_system.GetSystemTimers().GetTicksPerSecond());
  const float field_time = tick * static_cast<float>(ticks_per_half_line) *
                           static_cast<float>(GetHalfLinesPerEvenField());
  const float line_time = tick * static_cast<float>(ticks_per_half_line) * 2.0f;
  const float active_height_time = static_cast<float>(active_lines) * line_time;
  const float active_width_time =
      tick * static_cast<float>(GetTicksPerSample()) * static_cast<float>(active_samples);

  float vertical_ratio = active_height_time / field_time;
  float horizontal_ratio = active_width_time / line_time;

  if (FMT::Get(m_display_control) == 1)
  {
    // PAL: 52 us of a 64 us line, 576 of 625 lines.
    horizontal_ratio *= 64.0f / 52.0f;
    vertical_ratio *= 625.0f / 576.0f;
  }
  else
  {
    // NTSC: about 52.66 us of a 63.56 us line (the standards leave some
    // slack), 486 of 525 lines.
    horizontal_ratio *= 63.555555f / 52.655555f;
    vertical_ratio *= 525.0f / 486.0f;
  }

  const float ratio = horizontal_ratio / vertical_ratio;
  // Fall back to 4:3 for nonsense timings and for FIFO logs (unknown mode).
  const bool fifo_log = m_system.GetFifoPlayer().IsRunningWithFakeVideoInterfaceUpdates();
  if (!std::isnormal(ratio) || fifo_log)
    return 4.0f / 3.0f;
  return ratio * (4.0f / 3.0f);
}

// Works out the active half lines of both fields. The frame is:
//   odd field:  EQU x3, odd PRB, ACV x2 (active), odd PSB
//   even field: EQU x3, even PRB, ACV x2 (active), even PSB
// A field is output before its post-blanking. Games often split PRB/PSB
// differently between the fields (same sum), which would make the fields'
// output times uneven, so the even field is shifted by the PSB difference.
void VideoInterfaceManager::UpdateParameters()
{
  const u32 equ_half_lines = 3 * EQU::Get(m_vertical_timing);
  const u32 active_half_lines = 2 * ACV::Get(m_vertical_timing);
  const u32 odd_psb = PSB::Get(m_vblank_timing_odd);
  const u32 even_psb = PSB::Get(m_vblank_timing_even);

  m_odd_field_first_hl = equ_half_lines + PRB::Get(m_vblank_timing_odd);
  const u32 odd_field_active_end = m_odd_field_first_hl + active_half_lines;
  m_odd_field_last_hl = odd_field_active_end - 1;

  const u32 psb_difference = odd_psb - even_psb;
  m_even_field_first_hl = odd_field_active_end + odd_psb + equ_half_lines +
                          PRB::Get(m_vblank_timing_even) - psb_difference;
  m_even_field_last_hl = m_even_field_first_hl + active_half_lines - 1;

  UpdateRefreshRate();
}

void VideoInterfaceManager::UpdateRefreshRate()
{
  // Two fields per frame.
  m_target_refresh_rate_numerator = m_system.GetSystemTimers().GetTicksPerSecond() * 2;
  m_target_refresh_rate_denominator = GetTicksPerEvenField() + GetTicksPerOddField();
  m_target_refresh_rate = static_cast<double>(m_target_refresh_rate_numerator) /
                          m_target_refresh_rate_denominator;
}

double VideoInterfaceManager::GetTargetRefreshRate() const { return m_target_refresh_rate; }

u32 VideoInterfaceManager::GetTargetRefreshRateNumerator() const
{
  return m_target_refresh_rate_numerator;
}

u32 VideoInterfaceManager::GetTargetRefreshRateDenominator() const
{
  return m_target_refresh_rate_denominator;
}

u32 VideoInterfaceManager::GetTicksPerSample() const
{
  return 2 * m_system.GetSystemTimers().GetTicksPerSecond() / CLOCK_FREQUENCIES[m_clock & 1];
}

u32 VideoInterfaceManager::GetNominalTicksPerHalfLine() const
{
  return GetTicksPerSample() * HLW::Get(m_h_timing_0);
}

u32 VideoInterfaceManager::GetTicksPerHalfLine() const
{
  // Float division, truncated.
  return static_cast<u32>(static_cast<float>(GetNominalTicksPerHalfLine()) / m_config_vi_oc_factor);
}

u32 VideoInterfaceManager::GetTicksPerField() const { return GetTicksPerEvenField(); }

// --- Scan-out -------------------------------------------------------------------------

void VideoInterfaceManager::LogField(FieldType field, u32 xfb_address) const
{
  const bool even = field == FieldType::Even;
  const u32 vblank = even ? m_vblank_timing_even : m_vblank_timing_odd;
  DEBUG_LOG_FMT(VIDEOINTERFACE,
                "{} field: XFB {:08X}, WPL {}, STD {}, EQU {}, PRB {}, ACV {}, PSB {}",
                even ? "Even" : "Odd", xfb_address, WPL::Get(m_picture_configuration),
                STD::Get(m_picture_configuration), EQU::Get(m_vertical_timing), PRB::Get(vblank),
                ACV::Get(m_vertical_timing), PSB::Get(vblank));
  DEBUG_LOG_FMT(VIDEOINTERFACE, "Scaling {:04x}, FB width {}, field ticks {} / {}",
                m_horizontal_scaling, m_fb_width, GetTicksPerEvenField(), GetTicksPerOddField());
}

void VideoInterfaceManager::OutputField(FieldType field, u64 ticks)
{
  const u32 stride_units = STD::Get(m_picture_configuration);
  const u32 width_units = WPL::Get(m_picture_configuration);

  // Line pitch in bytes (16 pixels of 2 bytes per unit).
  u32 stride = stride_units * 16 * 2;
  const u32 width = width_units * 16;
  u32 height = ACV::Get(m_vertical_timing);
  u32 xfb_address = field == FieldType::Even ? GetXFBAddressBottom() : GetXFBAddressTop();

  // Forced progressive output: when the XFB holds both fields interleaved
  // (stride twice the width) and the mode is interlaced (odd half line count
  // per field), show the whole frame instead of one field. Whichever field
  // has the larger PRB holds the second line, so its address is moved back by
  // one line to reach the frame start.
  const bool interleaved_xfb = width_units != 0 && stride_units / width_units == 2;
  const bool interlaced_mode = (GetHalfLinesPerEvenField() & 1) != 0;
  if (interleaved_xfb && interlaced_mode && Config::Get(Config::GFX_HACK_FORCE_PROGRESSIVE))
  {
    stride /= 2;
    height *= 2;

    const u32 odd_prb = PRB::Get(m_vblank_timing_odd);
    const u32 even_prb = PRB::Get(m_vblank_timing_even);
    const bool second_line = field == FieldType::Odd ? odd_prb == even_prb + 1 :
                                                       odd_prb == even_prb - 1;
    if (second_line && xfb_address != 0)
      xfb_address -= stride;
  }

  LogField(field, xfb_address);

  // The whole field is drawn with the register values of this moment.
  if (xfb_address != 0)
    g_video_backend->Video_OutputXFB(xfb_address, width, stride, height, ticks);
}

void VideoInterfaceManager::BeginField(FieldType field, u64 ticks)
{
  // Early output: lower latency, as long as the game leaves the registers
  // alone during scan-out.
  if (Config::Get(Config::GFX_HACK_EARLY_XFB_OUTPUT))
    OutputField(field, ticks);
}

void VideoInterfaceManager::EndField(FieldType field, u64 ticks)
{
  // Late output picks up register changes made during scan-out.
  if (!Config::Get(Config::GFX_HACK_EARLY_XFB_OUTPUT))
    OutputField(field, ticks);

  // Pace on vblank unless immediate XFB is on (it trades pacing for latency).
  if (!g_ActiveConfig.bImmediateXFB)
    m_system.GetCoreTiming().Throttle(static_cast<s64>(ticks));

  m_system.GetPerfMetrics().CountVBlank();
  m_system.GetVideoEvents().vi_end_field_event.Trigger();
  Core::OnFrameEnd(m_system);
}

void VideoInterfaceManager::Update(u64 ticks)
{
  const u32 even_field_start = GetHalfLinesPerOddField();
  const bool field_start = m_half_line_count == 0 || m_half_line_count == even_field_start;

  // Before any output, so the movie frame counter on screen is current.
  if (field_start)
    m_system.GetMovie().FrameUpdate();

  if (m_half_line_count == m_odd_field_first_hl)
    BeginField(FieldType::Odd, ticks);
  else if (m_half_line_count == m_even_field_first_hl)
    BeginField(FieldType::Even, ticks);
  else if (m_half_line_count == m_odd_field_last_hl)
    EndField(FieldType::Odd, ticks);
  else if (m_half_line_count == m_even_field_last_hl)
    EndField(FieldType::Even, ticks);

  // Frame stepping, after output and before the SI poll.
  if (field_start)
    Core::Callback_NewField(m_system);

  auto& timing = m_system.GetCoreTiming();

  if (m_half_line_count == m_half_line_of_next_si_poll)
  {
    // Throttle first, so input is read as late as possible.
    timing.Throttle(static_cast<s64>(ticks));
    Core::UpdateInputGate(!Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT),
                          Config::Get(Config::MAIN_LOCK_CURSOR));
    auto& si = m_system.GetSerialInterface();
    si.UpdateDevices();
    m_half_line_of_next_si_poll += 2 * si.GetPollXLines();
  }

  // Each field's polling starts again at its vsync.
  if (field_start)
    m_half_line_of_next_si_poll = m_half_line_count + NUM_HALF_LINES_FOR_SI_POLL;

  m_half_line_count++;
  if (m_half_line_count == GetHalfLinesPerEvenField() + GetHalfLinesPerOddField())
    m_half_line_count = 0;
  if ((m_half_line_count & 1) == 0)
    m_ticks_last_line_start = ticks;

  if (timing.GetVISkip())
    return;

  // Display interrupts, at half line resolution: the first half of the line
  // unless HCT is past the half line width.
  const u32 line = 1 + m_half_line_count / 2;
  const u32 second_half = m_half_line_count & 1;
  for (u32& reg : m_interrupt_register)
  {
    const u32 wanted_half = HCT::Get(reg) > HLW::Get(m_h_timing_0) ? 1 : 0;
    if (line == VCT::Get(reg) && second_half == wanted_half)
      IR_INT::Set(reg, 1);
  }

  UpdateInterrupts();
}

void VideoInterfaceManager::FakeVIUpdate(u32 xfb_address, u32 fb_width, u32 fb_stride,
                                         u32 fb_height)
{
  // Taller than 240 lines: treat as an interleaved interlaced frame.
  const bool interlaced = fb_height > 480 / 2;
  if (interlaced)
  {
    fb_height /= 2;
    fb_stride *= 2;
  }

  POFF::Set(m_xfb_info_top, 1);
  POFF::Set(m_xfb_info_bottom, 1);
  ACV::Set(m_vertical_timing, fb_height);
  EQU::Set(m_vertical_timing, 6);
  PRB::Set(m_vblank_timing_odd, 502 - fb_height * 2);
  PSB::Set(m_vblank_timing_odd, 5);
  PRB::Set(m_vblank_timing_even, 503 - fb_height * 2);
  PSB::Set(m_vblank_timing_even, 4);
  WPL::Set(m_picture_configuration, fb_width / 16);
  STD::Set(m_picture_configuration, (fb_stride / 2) / 16);

  UpdateParameters();

  // Point whichever field comes next at the frame buffer.
  const u32 frame_half_lines = GetHalfLinesPerEvenField() + GetHalfLinesPerOddField();
  const u32 to_even = (m_half_line_count - m_even_field_first_hl) % frame_half_lines;
  const u32 to_odd = (m_half_line_count - m_odd_field_first_hl) % frame_half_lines;
  if (to_even < to_odd)
    FBB::Set(m_xfb_info_bottom, interlaced ? (xfb_address + fb_width * 2) >> 5 : xfb_address >> 5);
  else
    FBB::Set(m_xfb_info_top, xfb_address >> 5);
}

}  // namespace VideoInterface
