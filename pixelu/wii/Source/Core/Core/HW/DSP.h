// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

class PointerWrap;
class DSPEmulator;
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

// The DSP interface (0x0C005000): mailboxes and control register of the DSP,
// the ARAM controller and its DMA engine, and the audio DMA that feeds the
// mixer from RAM.
namespace DSP
{
// Interrupt status bits in the control register; each one's enable bit is
// the next bit up.
enum DSPInterruptType
{
  INT_DSP = 0x80,
  INT_ARAM = 0x20,
  INT_AID = 0x08,
};

// GameCube ARAM.
enum
{
  ARAM_SIZE = 0x01000000,  // 16 MiB
  ARAM_MASK = 0x00FFFFFF,
};

// Control register bits owned by the DSP emulator (reset, assert interrupt,
// halt, init code, init).
constexpr u16 DSP_CONTROL_MASK = 0x0C07;

// DSP control register.
union UDSPControl
{
  u16 Hex;
  struct
  {
    u16 DSPReset : 1;      // write 1 to reset; reads 1 until done
    u16 DSPAssertInt : 1;  // CPU -> DSP interrupt
    u16 DSPHalt : 1;
    u16 AID : 1;  // audio DMA interrupt
    u16 AID_mask : 1;
    u16 ARAM : 1;  // ARAM DMA interrupt
    u16 ARAM_mask : 1;
    u16 DSP : 1;  // DSP -> CPU interrupt
    u16 DSP_mask : 1;
    u16 DMAState : 1;  // ARAM DMA in progress
    u16 DSPInitCode : 1;
    u16 DSPInit : 1;
    u16 pad : 4;
  };
  UDSPControl(u16 hex = 0) : Hex(hex) {}
};

class DSPManager
{
public:
  explicit DSPManager(Core::System& system);
  DSPManager(const DSPManager&) = delete;
  DSPManager(DSPManager&&) = delete;
  DSPManager& operator=(const DSPManager&) = delete;
  DSPManager& operator=(DSPManager&&) = delete;
  ~DSPManager();

  void Init(bool hle);
  void Reinit(bool hle);
  void Shutdown();

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  DSPEmulator* GetDSPEmulator();

  void DoState(PointerWrap& p);

  // For the DSP emulator (any thread): raise a DSP interrupt later.
  void GenerateDSPInterruptFromDSPEmu(DSPInterruptType type, int cycles_into_future = 0);

  // ARAM access for the DSP emulator.
  u8 ReadARAM(u32 address) const;
  void WriteARAM(u8 value, u32 address);

  // For the debugger.
  u8* GetARAMPtr() const;
  u32 GetARAMSize() const;

  // Periodic work, driven by SystemTimers.
  void UpdateAudioDMA();
  void UpdateDSPSlice(int cycles);

private:
  void GenerateDSPInterrupt(u64 DSPIntType, s64 cyclesLate);
  static void GlobalGenerateDSPInterrupt(Core::System& system, u64 DSPIntType, s64 cyclesLate);
  void CompleteARAM(u64 userdata, s64 cyclesLate);
  static void GlobalCompleteARAM(Core::System& system, u64 userdata, s64 cyclesLate);
  void UpdateInterrupts();
  void Do_ARAM_DMA();
  void WriteControl(u16 value);
  void WriteAudioDMAControl(u16 value);
  u16 ReadMailHigh(bool cpu_mailbox);

  // The structs below are stored in savestates as raw bytes; their layout
  // must not change.

  // Audio DMA. The control word holds the block count (32-byte blocks) in
  // bits 0-14 and the enable flag in bit 15.
  struct AudioDMA
  {
    u32 current_source_address = 0;
    u16 remaining_blocks_count = 0;
    u32 source_address = 0;
    u16 control = 0;
  };

  // ARAM DMA. The count word holds the byte count in bits 0-30 and the
  // direction in bit 31 (1 = ARAM to main memory).
  struct AramDMA
  {
    u32 mm_address = 0;
    u32 ar_address = 0;
    u32 count = 0;
  };

  // Where the auxiliary memory lives: ARAM on the GameCube, MEM2 on the Wii
  // (owned by the memory manager and saved there).
  struct AuxMemory
  {
    bool wii_mode = false;
    u32 size = ARAM_SIZE;
    u32 mask = ARAM_MASK;
    u8* ptr = nullptr;
  };

  AuxMemory m_aram;
  AudioDMA m_audio_dma;
  AramDMA m_aram_dma;
  UDSPControl m_dsp_control;
  u16 m_aram_info = 0;  // size configuration in bits 0-5, bit 6 unknown
  // ARAM controller mode. Only bit 0 (controller ready) matters.
  u16 m_aram_mode = 0;
  u16 m_aram_refresh = 0;
  // LLE: DSP cycles not yet run in the current slice.
  int m_dsp_slice = 0;

  std::unique_ptr<DSPEmulator> m_dsp_emulator;

  bool m_is_lle = false;

  CoreTiming::EventType* m_event_type_generate_dsp_interrupt = nullptr;
  CoreTiming::EventType* m_event_type_complete_aram = nullptr;

  Core::System& m_system;
};
}  // namespace DSP
