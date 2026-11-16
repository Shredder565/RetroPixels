// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// AVI layout (Microsoft's AVI RIFF format): RIFF 'AVI ' { LIST 'hdrl' { avih, LIST 'strl'
// { strh, strf } }, LIST 'movi' { '00dc' chunks }, idx1 }. Sizes and frame counts in the headers
// are filled in when the file is closed. Frames are timed at the emulated refresh rate; a
// refresh with no new frame is written as an empty chunk, which players treat as a repeat of
// the previous frame.

#include "VideoCommon/FrameDumpAVI.h"

#include <cstring>
#include <limits>
#include <numeric>
#include <string>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/TimeUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

#include "VideoCommon/OnScreenDisplay.h"

namespace
{
constexpr int kJpegQuality = 90;
// Start a new file before this size: AVI 1.0 offsets are 32-bit and many readers treat them
// as signed.
constexpr u64 kMaxFileSize = 0x7800'0000;

constexpr u32 kAvihSize = 56;
constexpr u32 kStrhSize = 56;
constexpr u32 kStrfSize = 40;
constexpr u32 kKeyFrameFlag = 0x10;   // AVIIF_KEYFRAME
constexpr u32 kHasIndexFlag = 0x10;   // AVIF_HASINDEX

// Offsets of the fields patched on close.
constexpr u64 kRiffSizeAt = 4;
constexpr u64 kAvihAt = 12 + 12 + 8;  // after RIFF header, LIST hdrl header, avih header
constexpr u64 kTotalFramesAt = kAvihAt + 16;
constexpr u64 kAvihBufferAt = kAvihAt + 28;
constexpr u64 kStrhAt = kAvihAt + kAvihSize + 12 + 8;
constexpr u64 kStrhLengthAt = kStrhAt + 32;
constexpr u64 kStrhBufferAt = kStrhAt + 36;
constexpr u64 kMoviListAt = kStrhAt + kStrhSize + 8 + kStrfSize;

void Put32(std::vector<u8>* out, u32 v)
{
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<u8>(v >> (8 * i)));
}
void Put16(std::vector<u8>* out, u32 v)
{
  out->push_back(static_cast<u8>(v));
  out->push_back(static_cast<u8>(v >> 8));
}
void PutTag(std::vector<u8>* out, const char tag[4])
{
  out->insert(out->end(), tag, tag + 4);
}

// The emulated refresh rate as frames per second num/den, both kept within 32 bits.
// TODO: reading the target refresh rate is not thread safe from the dump path.
void RefreshRate(int* num, int* den)
{
  auto& vi = Core::System::GetInstance().GetVideoInterface();
  u64 n = vi.GetTargetRefreshRateNumerator();
  u64 d = vi.GetTargetRefreshRateDenominator();
  if (n == 0 || d == 0)
    n = 60, d = 1;
  const u64 g = std::gcd(n, d);
  n /= g;
  d /= g;
  while (n > 0x7fffffff || d > 0x7fffffff)
    n >>= 1, d >>= 1;
  *num = static_cast<int>(std::max<u64>(n, 1));
  *den = static_cast<int>(std::max<u64>(d, 1));
}

// "<dump folder><game id>_<local start time>_<index>.avi", or the configured path.
// Returns "" when dumping should not go ahead.
std::string SegmentPath(std::time_t start, u32 index)
{
  if (std::string fixed = Config::Get(Config::GFX_DUMP_PATH); !fixed.empty())
    return fixed;

  const auto local = Common::LocalTime(start);
  if (!local)
    return "";

  std::string path =
      fmt::format("{}{}_{:%Y-%m-%d_%H-%M-%S}_{}.avi", File::GetUserPath(D_DUMPFRAMES_IDX),
                  SConfig::GetInstance().GetGameID(), *local, index);

  if (File::Exists(path))
  {
    const bool replace = Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES_SILENT) ||
                         AskYesNoFmtT("Delete the existing file '{0}'?", path);
    if (!replace)
      return "";  // the user cancelled the dump
    File::Delete(path);
  }
  return path;
}
}  // namespace

struct AVIFrameDump::Segment
{
  File::IOFile file;
  int width = 0;
  int height = 0;
  int rate_num = 0;
  int rate_den = 0;

  // Emulated time of the file's first frame, and the savestate generation it belongs to.
  u64 first_tick = 0;
  u32 savestate_index = 0;

  // Frame slots written so far (including repeats), and the largest chunk.
  s64 frames = 0;
  u32 largest_chunk = 0;
  // idx1 entries: flags, offset from the 'movi' tag, size.
  std::vector<u8> index;
  bool warned_late = false;
};

AVIFrameDump::AVIFrameDump() : m_encoder(kJpegQuality)
{
}

AVIFrameDump::~AVIFrameDump()
{
  Stop();
}

bool AVIFrameDump::IsStarted() const
{
  return m_segment != nullptr;
}

bool AVIFrameDump::Start(int w, int h, u64 start_ticks)
{
  if (IsStarted())
    return true;

  m_savestate_index = 0;
  m_start_time = std::time(nullptr);
  m_file_index = 0;
  int num, den;
  RefreshRate(&num, &den);
  return OpenSegment(w, h, start_ticks, m_savestate_index, num, den);
}

bool AVIFrameDump::OpenSegment(int w, int h, u64 start_ticks, u32 savestate_index,
                               int rate_num, int rate_den)
{
  const std::string path = SegmentPath(m_start_time, m_file_index);
  if (path.empty() || w <= 0 || h <= 0)
  {
    OSD::AddMessage("FrameDump Start failed");
    return false;
  }
  File::CreateFullPath(path);

  auto segment = std::make_unique<Segment>();
  segment->width = w;
  segment->height = h;
  segment->rate_num = rate_num;
  segment->rate_den = rate_den;
  segment->first_tick = start_ticks;
  segment->savestate_index = savestate_index;
  if (!segment->file.Open(path, "wb"))
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not open {}", path);
    OSD::AddMessage("FrameDump Start failed");
    return false;
  }

  std::vector<u8> head;
  PutTag(&head, "RIFF");
  Put32(&head, 0);  // patched
  PutTag(&head, "AVI ");
  PutTag(&head, "LIST");
  Put32(&head, 4 + 8 + kAvihSize + 12 + 8 + kStrhSize + 8 + kStrfSize);
  PutTag(&head, "hdrl");

  PutTag(&head, "avih");
  Put32(&head, kAvihSize);
  Put32(&head, static_cast<u32>(1'000'000ull * rate_den / rate_num));  // microseconds per frame
  Put32(&head, 0);                                                     // max bytes per second
  Put32(&head, 0);                                                     // padding granularity
  Put32(&head, kHasIndexFlag);
  Put32(&head, 0);  // total frames, patched
  Put32(&head, 0);  // initial frames
  Put32(&head, 1);  // streams
  Put32(&head, 0);  // suggested buffer size, patched
  Put32(&head, static_cast<u32>(w));
  Put32(&head, static_cast<u32>(h));
  for (int i = 0; i < 4; ++i)
    Put32(&head, 0);

  PutTag(&head, "LIST");
  Put32(&head, 4 + 8 + kStrhSize + 8 + kStrfSize);
  PutTag(&head, "strl");

  PutTag(&head, "strh");
  Put32(&head, kStrhSize);
  PutTag(&head, "vids");
  PutTag(&head, "MJPG");
  Put32(&head, 0);  // flags
  Put16(&head, 0);  // priority
  Put16(&head, 0);  // language
  Put32(&head, 0);  // initial frames
  Put32(&head, static_cast<u32>(rate_den));  // scale
  Put32(&head, static_cast<u32>(rate_num));  // rate: rate / scale frames per second
  Put32(&head, 0);                           // start
  Put32(&head, 0);                           // length, patched
  Put32(&head, 0);                           // suggested buffer size, patched
  Put32(&head, 0xffffffff);                  // quality: default
  Put32(&head, 0);                           // sample size: varies
  Put16(&head, 0);
  Put16(&head, 0);
  Put16(&head, static_cast<u32>(w));
  Put16(&head, static_cast<u32>(h));

  PutTag(&head, "strf");
  Put32(&head, kStrfSize);
  Put32(&head, kStrfSize);
  Put32(&head, static_cast<u32>(w));
  Put32(&head, static_cast<u32>(h));
  Put16(&head, 1);   // planes
  Put16(&head, 24);  // bits per pixel
  PutTag(&head, "MJPG");
  Put32(&head, static_cast<u32>(w * h * 3));
  for (int i = 0; i < 4; ++i)
    Put32(&head, 0);

  PutTag(&head, "LIST");
  Put32(&head, 0);  // patched
  PutTag(&head, "movi");

  if (head.size() != kMoviListAt + 12 || !segment->file.WriteBytes(head.data(), head.size()))
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not write to {}", path);
    OSD::AddMessage("FrameDump Start failed");
    return false;
  }

  m_segment = std::move(segment);
  NOTICE_LOG_FMT(FRAMEDUMP, "Dumping frames to {} ({}x{} at {}/{} fps)", path, w, h, rate_num,
                 rate_den);
  OSD::AddMessage(fmt::format("Dumping Frames to \"{}\" ({}x{})", path, w, h));
  return true;
}

void AVIFrameDump::WriteChunk(const u8* data, u32 size, bool key_frame)
{
  Segment& seg = *m_segment;
  const u64 at = seg.file.Tell();
  std::vector<u8> header;
  PutTag(&header, "00dc");
  Put32(&header, size);
  seg.file.WriteBytes(header.data(), header.size());
  if (size != 0)
    seg.file.WriteBytes(data, size);
  if (size % 2 != 0)
    seg.file.WriteBytes("", 1);

  PutTag(&seg.index, "00dc");
  Put32(&seg.index, key_frame ? kKeyFrameFlag : 0);
  Put32(&seg.index, static_cast<u32>(at - (kMoviListAt + 8)));
  Put32(&seg.index, size);
  seg.largest_chunk = std::max(seg.largest_chunk, size);
  ++seg.frames;
}

void AVIFrameDump::AddFrame(const FrameData& frame)
{
  if (!IsStarted())
    return;

  RestartIfNeeded(frame);
  // The restart may have failed.
  if (!IsStarted())
    return;

  Segment& seg = *m_segment;

  // The frame's slot from the emulated time since the file's first frame.
  // TODO: GetTicksPerSecond is not thread safe from the dump path.
  const u64 ticks_per_second = Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond();
  const u64 elapsed = frame.state.ticks - seg.first_tick;
  const s64 slot = static_cast<s64>(
      (static_cast<double>(elapsed) * seg.rate_num) / (static_cast<double>(ticks_per_second) *
                                                       seg.rate_den) + 0.5);

  if (slot < seg.frames)
  {
    if (!seg.warned_late)
    {
      WARN_LOG_FMT(FRAMEDUMP, "Frame arrived for a time already written; skipping it.");
      seg.warned_late = true;
    }
    return;
  }

  // The VI can switch its output off (a zero-sized frame): the picture holds still.
  if (frame.width <= 0 || frame.height <= 0 || frame.data == nullptr)
  {
    while (seg.frames <= slot)
      WriteChunk(nullptr, 0, false);
    return;
  }

  m_encoder.Encode(frame.data, seg.width, seg.height, frame.stride, &m_jpeg);

  // Start a new file rather than pass the size limit.
  const u64 needed = (slot - seg.frames) * 24 + m_jpeg.size() + 32 + seg.index.size() + 16;
  if (seg.file.Tell() + needed > kMaxFileSize)
  {
    const FrameState state = frame.state;
    const int w = seg.width, h = seg.height, num = seg.rate_num, den = seg.rate_den;
    CloseSegment();
    ++m_file_index;
    if (!OpenSegment(w, h, state.ticks, state.savestate_index, num, den))
      return;
    WriteChunk(m_jpeg.data(), static_cast<u32>(m_jpeg.size()), true);
    return;
  }

  // Refreshes with no new frame repeat the previous one.
  while (seg.frames < slot)
    WriteChunk(nullptr, 0, false);
  WriteChunk(m_jpeg.data(), static_cast<u32>(m_jpeg.size()), true);
}

void AVIFrameDump::CloseSegment()
{
  if (!m_segment)
    return;
  Segment& seg = *m_segment;

  const u64 movi_end = seg.file.Tell();
  std::vector<u8> idx;
  PutTag(&idx, "idx1");
  Put32(&idx, static_cast<u32>(seg.index.size()));
  seg.file.WriteBytes(idx.data(), idx.size());
  seg.file.WriteBytes(seg.index.data(), seg.index.size());
  const u64 end = seg.file.Tell();

  auto patch = [&](u64 at, u32 value) {
    std::vector<u8> bytes;
    Put32(&bytes, value);
    seg.file.Seek(static_cast<s64>(at), File::SeekOrigin::Begin);
    seg.file.WriteBytes(bytes.data(), bytes.size());
  };
  patch(kRiffSizeAt, static_cast<u32>(end - 8));
  patch(kTotalFramesAt, static_cast<u32>(seg.frames));
  patch(kAvihBufferAt, seg.largest_chunk);
  patch(kStrhLengthAt, static_cast<u32>(seg.frames));
  patch(kStrhBufferAt, seg.largest_chunk);
  patch(kMoviListAt + 4, static_cast<u32>(movi_end - (kMoviListAt + 8)));

  if (!seg.file.IsGood())
    ERROR_LOG_FMT(FRAMEDUMP, "Error while writing the frame dump");
  seg.file.Close();
  m_segment.reset();
}

void AVIFrameDump::Stop()
{
  if (!IsStarted())
    return;
  CloseSegment();
  NOTICE_LOG_FMT(FRAMEDUMP, "Stopping frame dump");
  OSD::AddMessage("Stopped dumping frames");
}

void AVIFrameDump::DoState(PointerWrap& p)
{
  // Nothing is saved; a load just starts a new generation.
  if (p.IsReadMode())
    ++m_savestate_index;
}

void AVIFrameDump::RestartIfNeeded(const FrameData& frame)
{
  const Segment& seg = *m_segment;

  // A zero width or height (the VI can switch output off) keeps the current size.
  const bool size_changed = (frame.width != seg.width || frame.height != seg.height) &&
                            frame.width > 0 && frame.height > 0;

  if (size_changed)
  {
    INFO_LOG_FMT(FRAMEDUMP, "Starting new dump on resolution change.");
  }
  else if (seg.frames != 0 && frame.state.savestate_index != seg.savestate_index)
  {
    INFO_LOG_FMT(FRAMEDUMP, "Starting new dump on savestate load.");
  }
  else if (frame.state.refresh_rate_num != seg.rate_num ||
           frame.state.refresh_rate_den != seg.rate_den)
  {
    INFO_LOG_FMT(FRAMEDUMP, "Starting new dump on refresh rate change {}/{} vs {}/{}.",
                 seg.rate_num, seg.rate_den, frame.state.refresh_rate_num,
                 frame.state.refresh_rate_den);
  }
  else
  {
    return;
  }

  const int w = frame.width > 0 ? frame.width : seg.width;
  const int h = frame.height > 0 ? frame.height : seg.height;
  CloseSegment();
  NOTICE_LOG_FMT(FRAMEDUMP, "Stopping frame dump");
  ++m_file_index;
  OpenSegment(w, h, frame.state.ticks, frame.state.savestate_index, frame.state.refresh_rate_num,
              frame.state.refresh_rate_den);
}

FrameState AVIFrameDump::FetchState(u64 ticks, int frame_number) const
{
  FrameState state;
  state.ticks = ticks;
  state.frame_number = frame_number;
  state.savestate_index = m_savestate_index;
  RefreshRate(&state.refresh_rate_num, &state.refresh_rate_den);
  return state;
}
