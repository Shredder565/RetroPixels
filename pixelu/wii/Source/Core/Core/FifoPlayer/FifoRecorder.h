// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Common/Assert.h"
#include "Common/HookableEvent.h"
#include "Core/FifoPlayer/FifoDataFile.h"

namespace Core
{
class System;
}

class FifoRecorder
{
public:
  using CallbackFunc = std::function<void()>;

  explicit FifoRecorder(Core::System& system);
  FifoRecorder(const FifoRecorder&) = delete;
  FifoRecorder(FifoRecorder&&) = delete;
  FifoRecorder& operator=(const FifoRecorder&) = delete;
  FifoRecorder& operator=(FifoRecorder&&) = delete;
  ~FifoRecorder();

  void StartRecording(s32 numFrames, CallbackFunc finishedCb);
  void StopRecording();

  bool IsRecordingDone() const;

  FifoDataFile* GetRecordedFile() const;

  // The functions below run on the video thread.

  // `data` holds exactly one complete GP command.
  void WriteGPCommand(const u8* data, u32 size);

  // Records the memory a command reads if it changed since it was last recorded. Memory the
  // video backend writes itself (dynamicUpdate) is only shadowed, so it never ends up in the log.
  void UseMemory(u32 address, u32 size, MemoryUpdate::Type type, bool dynamicUpdate = false);

  void EndFrame(u32 fifoStart, u32 fifoEnd);

  // Snapshot of the GPU state the recording starts from; must come before the first
  // WriteGPCommand. The CP state also seeds the command analyzer.
  void SetVideoMemory(const u32* bpMem, const u32* cpMem, const u32* xfMem, const u32* xfRegs,
                      u32 xfRegsSize, const u8* texMem);

  // Polled once per frame, before EndFrame().
  bool IsRecording() const;

private:
  class FifoRecordAnalyzer;

  void RecordInitialVideoMemory();

  // Shared between the UI and video threads (guarded by m_mutex).

  std::recursive_mutex m_mutex;
  // The video thread should keep feeding data.
  bool m_IsRecording = false;
  // At least one frame end has been seen while recording.
  bool m_WasRecording = false;
  bool m_RequestedRecordingEnd = false;
  s32 m_RecordFramesRemaining = 0;
  CallbackFunc m_FinishedCb;
  std::unique_ptr<FifoDataFile> m_File;

  // Video thread only.

  bool m_SkipNextData = true;
  bool m_SkipFutureData = true;
  bool m_FrameEnded = false;
  FifoFrameInfo m_CurrentFrame;
  std::unique_ptr<FifoRecordAnalyzer> m_record_analyzer;
  std::vector<u8> m_FifoData;
  std::vector<u8> m_Ram;
  std::vector<u8> m_ExRam;

  Common::EventHook m_end_of_frame_event;

  Core::System& m_system;
};
