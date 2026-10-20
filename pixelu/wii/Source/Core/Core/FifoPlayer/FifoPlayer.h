// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Common/Assert.h"
#include "Common/Config/Config.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/OpcodeDecoding.h"

class FifoDataFile;
struct MemoryUpdate;

namespace Core
{
class System;
}
namespace CPU
{
enum class State;
}

// Old recordings captured the RAM behind EFB copies like any other texture data, so whatever was
// there at recording time (an EFB-to-RAM result or garbage) is part of the log and is written
// back during playback. Since EFB-to-texture copies now hash memory they zeroed themselves, that
// write-back would make the hash mismatch and the texture cache would load the stale bytes.
// While such a log plays, this flag tells the texture cache to trust the hash instead. Until the
// recorder handles EFB copies properly it is set for every log of an affected version.
extern bool IsPlayingBackFifologWithBrokenEFBCopies;

enum class FramePartType
{
  Commands,
  PrimitiveData,
  EFBCopy,
};

struct FramePart
{
  constexpr FramePart(FramePartType type, u32 start, u32 end, const CPState& cpmem)
      : m_type(type), m_start(start), m_end(end), m_cpmem(cpmem)
  {
  }

  const FramePartType m_type;
  const u32 m_start;
  const u32 m_end;
  const CPState m_cpmem;
};

struct AnalyzedFrameInfo
{
  std::vector<FramePart> parts;
  Common::EnumMap<u32, FramePartType::EFBCopy> part_type_counts;

  void AddPart(FramePartType type, u32 start, u32 end, const CPState& cpmem)
  {
    parts.emplace_back(type, start, end, cpmem);
    part_type_counts[type]++;
  }
};

class FifoPlayer
{
public:
  using CallbackFunc = std::function<void()>;

  explicit FifoPlayer(Core::System& system);
  FifoPlayer(const FifoPlayer&) = delete;
  FifoPlayer(FifoPlayer&&) = delete;
  FifoPlayer& operator=(const FifoPlayer&) = delete;
  FifoPlayer& operator=(FifoPlayer&&) = delete;
  ~FifoPlayer();

  bool Open(const std::string& filename);
  void Close();

  // A stand-in CPU core that plays the log; the PowerPC code runs it like any other core, so
  // the usual run/pause/stop controls apply. Valid only while a file is open; null when no
  // file (or an empty one) is loaded.
  std::unique_ptr<CPUCoreBase> GetCPUCore();

  bool IsPlaying() const;

  FifoDataFile* GetFile() const { return m_File.get(); }
  u32 GetMaxObjectCount() const;
  u32 GetFrameObjectCount(u32 frame) const;
  u32 GetCurrentFrameObjectCount() const;
  u32 GetCurrentFrameNum() const { return m_CurrentFrame; }
  const AnalyzedFrameInfo& GetAnalyzedFrameInfo(u32 frame) const { return m_FrameInfo[frame]; }
  // Frames to play (inclusive).
  u32 GetFrameRangeStart() const { return m_FrameRangeStart; }
  void SetFrameRangeStart(u32 start);

  u32 GetFrameRangeEnd() const { return m_FrameRangeEnd; }
  void SetFrameRangeEnd(u32 end);

  // Objects to draw within each frame (inclusive).
  u32 GetObjectRangeStart() const { return m_ObjectRangeStart; }
  void SetObjectRangeStart(u32 start) { m_ObjectRangeStart = start; }
  u32 GetObjectRangeEnd() const { return m_ObjectRangeEnd; }
  void SetObjectRangeEnd(u32 end) { m_ObjectRangeEnd = end; }

  void SetFileLoadedCallback(CallbackFunc callback);
  void SetFrameWrittenCallback(CallbackFunc callback) { m_FrameWrittenCb = std::move(callback); }

  bool IsRunningWithFakeVideoInterfaceUpdates() const;

private:
  class CPUCore;
  friend class CPUCore;

  CPU::State AdvanceFrame();

  void WriteFrame(const FifoFrameInfo& frame, const AnalyzedFrameInfo& info);
  void WriteFramePart(const FramePart& part, u32* next_mem_update, const FifoFrameInfo& frame);

  void WriteAllMemoryUpdates();
  void WriteMemory(const MemoryUpdate& memUpdate);

  // Feeds [start, end) of the current frame's FIFO data to the GPU (offsets into the frame,
  // which is what the cycle accounting is based on).
  void WriteFifo(const u8* data, u32 start, u32 end);

  void SetupFifo();

  void LoadMemory();
  void LoadRegisters();
  void LoadTextureMemory();
  void ClearEfb();

  void WriteCP(u32 address, u16 value);
  void WritePI(u32 address, u32 value);

  void FlushWGP();
  void WaitForGPUInactive();

  void LoadBPReg(u8 reg, u32 value);
  void LoadCPReg(u8 reg, u32 value);
  void LoadXFReg(u16 reg, u32 value);
  void LoadXFMem16(u16 address, const u32* data);

  static bool ShouldLoadBP(u8 address);
  static bool ShouldLoadXF(u8 address);

  bool IsIdleSet() const;
  bool IsHighWatermarkSet() const;

  void RefreshConfig();

  Core::System& m_system;

  bool m_Loop = true;
  // Apply every frame's memory updates up front instead of at their FIFO positions.
  bool m_EarlyMemoryUpdates = false;

  u32 m_CurrentFrame = 0;
  u32 m_FrameRangeStart = 0;
  u32 m_FrameRangeEnd = 0;

  u32 m_ObjectRangeStart = 0;
  u32 m_ObjectRangeEnd = 10000;

  u64 m_CyclesPerFrame = 0;
  u32 m_ElapsedCycles = 0;
  u32 m_FrameFifoSize = 0;

  CallbackFunc m_FileLoadedCb = nullptr;
  CallbackFunc m_FrameWrittenCb = nullptr;
  Config::ConfigChangedCallbackID m_config_changed_callback_id;

  std::unique_ptr<FifoDataFile> m_File;

  std::vector<AnalyzedFrameInfo> m_FrameInfo;
};
