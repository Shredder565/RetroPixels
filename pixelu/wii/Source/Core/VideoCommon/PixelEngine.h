// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <mutex>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}
namespace MMIO
{
class Mapping;
}

// The pixel engine's CPU-visible registers (0x0C001000 on the console): EFB access
// configuration, the draw-sync token, the draw-done flag, bounding box and performance counters.
namespace PixelEngine
{
// Register offsets within the PE block.
enum
{
  PE_ZCONF = 0x00,
  PE_ALPHACONF = 0x02,
  PE_DSTALPHACONF = 0x04,
  PE_ALPHAMODE = 0x06,
  PE_ALPHAREAD = 0x08,
  PE_CTRL_REGISTER = 0x0a,
  PE_TOKEN_REG = 0x0e,
  PE_BBOX_LEFT = 0x10,
  PE_BBOX_RIGHT = 0x12,
  PE_BBOX_TOP = 0x14,
  PE_BBOX_BOTTOM = 0x16,

  // Performance counters, each a 32-bit value split into low/high halves. The assignment of
  // counters to addresses is a best guess.
  PE_PERF_ZCOMP_INPUT_ZCOMPLOC_L = 0x18,
  PE_PERF_ZCOMP_INPUT_ZCOMPLOC_H = 0x1a,
  PE_PERF_ZCOMP_OUTPUT_ZCOMPLOC_L = 0x1c,
  PE_PERF_ZCOMP_OUTPUT_ZCOMPLOC_H = 0x1e,
  PE_PERF_ZCOMP_INPUT_L = 0x20,
  PE_PERF_ZCOMP_INPUT_H = 0x22,
  PE_PERF_ZCOMP_OUTPUT_L = 0x24,
  PE_PERF_ZCOMP_OUTPUT_H = 0x26,
  PE_PERF_BLEND_INPUT_L = 0x28,
  PE_PERF_BLEND_INPUT_H = 0x2a,
  PE_PERF_EFB_COPY_CLOCKS_L = 0x2c,
  PE_PERF_EFB_COPY_CLOCKS_H = 0x2e,
};

// Alpha value returned by CPU reads of the EFB.
enum class AlphaReadMode : u16
{
  Read00 = 0,    // constant 0x00
  ReadFF = 1,    // constant 0xFF
  ReadNone = 2,  // the stored alpha
};

// Same values as the BP-side compare enum, but 16-bit so it fits the PE register BitFields.
enum class CompareMode : u16
{
  Never = 0,
  Less = 1,
  Equal = 2,
  LEqual = 3,
  Greater = 4,
  NEqual = 5,
  GEqual = 6,
  Always = 7
};

union UPEZConfReg
{
  u16 hex = 0;
  BitField<0, 1, bool, u16> z_comparator_enable;
  BitField<1, 3, CompareMode, u16> function;
  BitField<4, 1, bool, u16> z_update_enable;
};

enum class SrcBlendFactor : u16
{
  Zero = 0,
  One = 1,
  DstClr = 2,
  InvDstClr = 3,
  SrcAlpha = 4,
  InvSrcAlpha = 5,
  DstAlpha = 6,
  InvDstAlpha = 7
};

enum class DstBlendFactor : u16
{
  Zero = 0,
  One = 1,
  SrcClr = 2,
  InvSrcClr = 3,
  SrcAlpha = 4,
  InvSrcAlpha = 5,
  DstAlpha = 6,
  InvDstAlpha = 7
};

enum class LogicOp : u16
{
  Clear = 0,
  And = 1,
  AndReverse = 2,
  Copy = 3,
  AndInverted = 4,
  NoOp = 5,
  Xor = 6,
  Or = 7,
  Nor = 8,
  Equiv = 9,
  Invert = 10,
  OrReverse = 11,
  CopyInverted = 12,
  OrInverted = 13,
  Nand = 14,
  Set = 15
};

union UPEAlphaConfReg
{
  u16 hex = 0;
  BitField<0, 1, bool, u16> blend;  // blend or subtract mode
  BitField<1, 1, bool, u16> logic;  // logic-op mode
  BitField<2, 1, bool, u16> dither;
  BitField<3, 1, bool, u16> color_update_enable;
  BitField<4, 1, bool, u16> alpha_update_enable;
  BitField<5, 3, DstBlendFactor, u16> dst_factor;
  BitField<8, 3, SrcBlendFactor, u16> src_factor;
  BitField<11, 1, bool, u16> subtract;  // subtract mode
  BitField<12, 4, LogicOp, u16> logic_op;
};

union UPEDstAlphaConfReg
{
  u16 hex = 0;
  BitField<0, 8, u8, u16> alpha;
  BitField<8, 1, bool, u16> enable;
};

union UPEAlphaModeConfReg
{
  u16 hex = 0;
  BitField<0, 8, u8, u16> threshold;
  // Documented as an 8-bit field; only the low three bits carry the compare mode.
  BitField<8, 3, CompareMode, u16> compare_mode;
};

union UPEAlphaReadReg
{
  u16 hex = 0;
  BitField<0, 2, AlphaReadMode, u16> read_mode;
};

// Interrupt control. Writing 1 to pe_token / pe_finish acknowledges that interrupt; those two
// bits always read back as 0.
union UPECtrlReg
{
  u16 hex = 0;
  BitField<0, 1, bool, u16> pe_token_enable;
  BitField<1, 1, bool, u16> pe_finish_enable;
  BitField<2, 1, bool, u16> pe_token;
  BitField<3, 1, bool, u16> pe_finish;
};

class PixelEngineManager
{
public:
  explicit PixelEngineManager(Core::System& system);

  void Init();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  // Called from the GPU side when the command stream sets the token or signals draw done.
  void SetToken(const u16 token, const bool interrupt, int cycle_delay);
  void SetFinish(int cycle_delay);
  AlphaReadMode GetAlphaReadMode() const { return m_alpha_read.read_mode; }

private:
  void ScheduleDelivery(int cycles_into_future);
  void UpdateInterrupts();
  void DeliverPending();

  static void OnTokenFinishEvent(Core::System& system, u64 userdata, s64 cycles_late);

  // Savestate fields (saved in declaration order).
  UPEZConfReg m_z_conf;
  UPEAlphaConfReg m_alpha_conf;
  UPEDstAlphaConfReg m_dst_alpha_conf;
  UPEAlphaModeConfReg m_alpha_mode_conf;
  UPEAlphaReadReg m_alpha_read;
  UPECtrlReg m_control;

  // Guards the hand-over of tokens and draw-done from the GPU thread to the CPU thread.
  std::mutex m_handover_lock;
  u16 m_token = 0;          // value visible to the CPU
  u16 m_queued_token = 0;   // value waiting to be delivered
  bool m_queued_token_irq = false;
  bool m_queued_finish_irq = false;
  bool m_delivery_scheduled = false;

  bool m_token_irq_asserted = false;
  bool m_finish_irq_asserted = false;

  CoreTiming::EventType* m_delivery_event = nullptr;

  Core::System& m_system;
};

}  // namespace PixelEngine
