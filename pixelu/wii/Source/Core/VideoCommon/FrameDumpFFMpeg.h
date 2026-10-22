// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <ctime>
#include <limits>
#include <memory>

#include "Common/CommonTypes.h"

struct FrameDumpContext;
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

// Encodes frames into a video file with FFmpeg (libavformat / libavcodec / libswscale).
// A new file is started whenever the resolution or the refresh rate changes, or after a
// savestate is loaded.
class FFMpegFrameDump
{
public:
  FFMpegFrameDump();
  ~FFMpegFrameDump();

  bool Start(int w, int h, u64 start_ticks);
  void AddFrame(const FrameData&);
  void Stop();
  void DoState(PointerWrap&);
  bool IsStarted() const;
  FrameState FetchState(u64 ticks, int frame_number) const;

private:
  bool OpenSegment(int w, int h, u64 start_ticks, u32 savestate_index);
  bool CreateSegmentFile();
  void CloseSegment();
  void RestartIfNeeded(const FrameData&);
  void DrainEncoder();

#if defined(HAVE_FFMPEG)
  std::unique_ptr<FrameDumpContext> m_context;
#endif

  // Bumped by every savestate load; frames carry it so a load starts a new file.
  u32 m_savestate_index = 0;

  // File naming: the time dumping started and the number of the current file.
  std::time_t m_start_time = {};
  u32 m_file_index = 0;

  // Largest time base denominator the current codec accepts (MPEG-4 is limited to 16 bits).
  int64_t m_max_denominator = std::numeric_limits<s64>::max();
};

#if !defined(HAVE_FFMPEG)
inline FFMpegFrameDump::FFMpegFrameDump() = default;
inline FFMpegFrameDump::~FFMpegFrameDump() = default;

inline FrameState FFMpegFrameDump::FetchState(u64, int) const
{
  return {};
}
#endif
