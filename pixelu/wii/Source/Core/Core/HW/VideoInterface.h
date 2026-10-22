// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

enum class FieldType;
class PointerWrap;
namespace Core
{
class System;
}
namespace MMIO
{
class Mapping;
}

// The video interface (VI, 0x0C002000): video timing, the external frame
// buffer addresses, beam position and the four display interrupts. Update()
// is called once per half line and drives scan-out and SI polling.
namespace VideoInterface
{
// Register offsets.
enum
{
  VI_VERTICAL_TIMING = 0x00,
  VI_CONTROL_REGISTER = 0x02,
  VI_HORIZONTAL_TIMING_0_HI = 0x04,
  VI_HORIZONTAL_TIMING_0_LO = 0x06,
  VI_HORIZONTAL_TIMING_1_HI = 0x08,
  VI_HORIZONTAL_TIMING_1_LO = 0x0a,
  VI_VBLANK_TIMING_ODD_HI = 0x0c,
  VI_VBLANK_TIMING_ODD_LO = 0x0e,
  VI_VBLANK_TIMING_EVEN_HI = 0x10,
  VI_VBLANK_TIMING_EVEN_LO = 0x12,
  VI_BURST_BLANKING_ODD_HI = 0x14,
  VI_BURST_BLANKING_ODD_LO = 0x16,
  VI_BURST_BLANKING_EVEN_HI = 0x18,
  VI_BURST_BLANKING_EVEN_LO = 0x1a,
  VI_FB_LEFT_TOP_HI = 0x1c,
  VI_FB_LEFT_TOP_LO = 0x1e,
  VI_FB_RIGHT_TOP_HI = 0x20,
  VI_FB_RIGHT_TOP_LO = 0x22,
  VI_FB_LEFT_BOTTOM_HI = 0x24,
  VI_FB_LEFT_BOTTOM_LO = 0x26,
  VI_FB_RIGHT_BOTTOM_HI = 0x28,
  VI_FB_RIGHT_BOTTOM_LO = 0x2a,
  VI_VERTICAL_BEAM_POSITION = 0x2c,
  VI_HORIZONTAL_BEAM_POSITION = 0x2e,
  VI_PRERETRACE_HI = 0x30,
  VI_PRERETRACE_LO = 0x32,
  VI_POSTRETRACE_HI = 0x34,
  VI_POSTRETRACE_LO = 0x36,
  VI_DISPLAY_INTERRUPT_2_HI = 0x38,
  VI_DISPLAY_INTERRUPT_2_LO = 0x3a,
  VI_DISPLAY_INTERRUPT_3_HI = 0x3c,
  VI_DISPLAY_INTERRUPT_3_LO = 0x3e,
  VI_DISPLAY_LATCH_0_HI = 0x40,
  VI_DISPLAY_LATCH_0_LO = 0x42,
  VI_DISPLAY_LATCH_1_HI = 0x44,
  VI_DISPLAY_LATCH_1_LO = 0x46,
  VI_HSCALEW = 0x48,
  VI_HSCALER = 0x4a,
  VI_FILTER_COEF_0_HI = 0x4c,
  VI_FILTER_COEF_0_LO = 0x4e,
  VI_FILTER_COEF_1_HI = 0x50,
  VI_FILTER_COEF_1_LO = 0x52,
  VI_FILTER_COEF_2_HI = 0x54,
  VI_FILTER_COEF_2_LO = 0x56,
  VI_FILTER_COEF_3_HI = 0x58,
  VI_FILTER_COEF_3_LO = 0x5a,
  VI_FILTER_COEF_4_HI = 0x5c,
  VI_FILTER_COEF_4_LO = 0x5e,
  VI_FILTER_COEF_5_HI = 0x60,
  VI_FILTER_COEF_5_LO = 0x62,
  VI_FILTER_COEF_6_HI = 0x64,
  VI_FILTER_COEF_6_LO = 0x66,
  VI_UNK_AA_REG_HI = 0x68,
  VI_UNK_AA_REG_LO = 0x6a,
  VI_CLOCK = 0x6c,
  VI_DTV_STATUS = 0x6e,
  VI_FBWIDTH = 0x70,
  VI_BORDER_BLANK_END = 0x72,    // debug video mode only
  VI_BORDER_BLANK_START = 0x74,  // debug video mode only
};

class VideoInterfaceManager
{
public:
  explicit VideoInterfaceManager(Core::System& system);
  VideoInterfaceManager(const VideoInterfaceManager&) = delete;
  VideoInterfaceManager(VideoInterfaceManager&&) = delete;
  VideoInterfaceManager& operator=(const VideoInterfaceManager&) = delete;
  VideoInterfaceManager& operator=(VideoInterfaceManager&&) = delete;
  ~VideoInterfaceManager();

  // Puts the registers in the state the boot ROM leaves them in (used when
  // the IPL is skipped).
  void Preset(bool _bNTSC);

  void Init();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  // Physical addresses of the frame buffers of the two fields.
  u32 GetXFBAddressTop() const;
  u32 GetXFBAddressBottom() const;

  // Called once per half line.
  void Update(u64 ticks);

  // Recomputes the VI interrupt line.
  void UpdateInterrupts();

  // Recomputes the field layout after a timing register change.
  void UpdateParameters();

  double GetTargetRefreshRate() const;
  u32 GetTargetRefreshRateNumerator() const;
  u32 GetTargetRefreshRateDenominator() const;

  u32 GetTicksPerSample() const;
  u32 GetTicksPerHalfLine() const;
  u32 GetTicksPerField() const;

  // Without the VBI clock override.
  u32 GetNominalTicksPerHalfLine() const;

  // Aspect ratio of the active picture area relative to a standard TV
  // picture (4:3 when the whole area is used). Multiply by 4/3 for 16:9.
  float GetAspectRatio() const;

  // Sets up a video mode matching a frame buffer (FIFO log playback).
  void FakeVIUpdate(u32 xfb_address, u32 fb_width, u32 fb_stride, u32 fb_height);

private:
  u32 GetHalfLinesPerEvenField() const;
  u32 GetHalfLinesPerOddField() const;
  u32 GetTicksPerEvenField() const;
  u32 GetTicksPerOddField() const;

  void LogField(FieldType field, u32 xfb_address) const;
  void OutputField(FieldType field, u64 ticks);
  void BeginField(FieldType field, u64 ticks);
  void EndField(FieldType field, u64 ticks);

  void RefreshConfig();
  void UpdateRefreshRate();
  void WriteControl(u16 value);

  // Registers, as raw values (field layouts in the .cpp). This order is the
  // savestate order.
  u16 m_vertical_timing = 0;
  u16 m_display_control = 0;
  u32 m_h_timing_0 = 0;
  u32 m_h_timing_1 = 0;
  u32 m_vblank_timing_odd = 0;
  u32 m_vblank_timing_even = 0;
  u32 m_burst_blanking_odd = 0;
  u32 m_burst_blanking_even = 0;
  u32 m_xfb_info_top = 0;
  u32 m_xfb_info_bottom = 0;
  u32 m_xfb_3d_info_top = 0;  // stereoscopic mode
  u32 m_xfb_3d_info_bottom = 0;
  std::array<u32, 4> m_interrupt_register{};
  std::array<u32, 2> m_latch_register{};
  u16 m_picture_configuration = 0;
  u16 m_horizontal_scaling = 0;
  std::array<u32, 3> m_filter_taps_10bit{};  // filter tables 0-2
  std::array<u32, 4> m_filter_taps_8bit{};   // filter tables 3-6
  u32 m_unknown_aa_register = 0;
  u16 m_clock = 0;  // 0: 27 MHz, 1: 54 MHz
  u16 m_dtv_status = 0;
  u16 m_fb_width = 0;
  u32 m_border_hblank = 0;

  double m_target_refresh_rate = 0;
  u32 m_target_refresh_rate_numerator = 0;
  u32 m_target_refresh_rate_denominator = 1;

  // Scan state.
  u64 m_ticks_last_line_start = 0;  // when the current full line started
  u32 m_half_line_count = 0;        // half lines since the start of the frame
  u32 m_half_line_of_next_si_poll = 0;

  // First and last active half line of each field (0-based).
  u32 m_even_field_first_hl = 0;
  u32 m_odd_field_first_hl = 0;
  u32 m_even_field_last_hl = 0;
  u32 m_odd_field_last_hl = 0;

  float m_config_vi_oc_factor = 1.0f;

  Config::ConfigChangedCallbackID m_config_changed_callback_id;
  Core::System& m_system;
};
}  // namespace VideoInterface
