// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <ctime>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "VideoCommon/JpegEncoder.h"

class PointerWrap;

// Emulation state captured when a frame is rendered, travelling with the frame to the dump
// thread that writes it later.
struct FrameState
{
  u64 ticks = 0;
  int frame_number = 0;
  u32 savestate_index = 0;
  int refresh_rate_num = 0;
  int refresh_rate_den = 0;
};

// One RGBA8 frame to encode (rows `stride` bytes apart).
struct FrameData
{
  const u8* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  FrameState state;
};

// Writes frames as Motion JPEG in AVI files (our own JPEG encoder and AVI writer). A new file
// is started whenever the resolution or the refresh rate changes, after a savestate is loaded,
// and before a file would pass the AVI 1.0 size limit.
class AVIFrameDump
{
public:
  AVIFrameDump();
  ~AVIFrameDump();

  bool Start(int w, int h, u64 start_ticks);
  void AddFrame(const FrameData&);
  void Stop();
  void DoState(PointerWrap&);
  bool IsStarted() const;
  FrameState FetchState(u64 ticks, int frame_number) const;

private:
  struct Segment;

  bool OpenSegment(int w, int h, u64 start_ticks, u32 savestate_index, int rate_num,
                   int rate_den);
  void CloseSegment();
  void RestartIfNeeded(const FrameData&);
  void WriteChunk(const u8* data, u32 size, bool key_frame);

  std::unique_ptr<Segment> m_segment;
  VideoCommon::JpegEncoder m_encoder;
  std::vector<u8> m_jpeg;

  // Bumped by every savestate load; frames carry it so a load starts a new file.
  u32 m_savestate_index = 0;

  // File naming: the time dumping started and the number of the current file.
  std::time_t m_start_time = {};
  u32 m_file_index = 0;
};
