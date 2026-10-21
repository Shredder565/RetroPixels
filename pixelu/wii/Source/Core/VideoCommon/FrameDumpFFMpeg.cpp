// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/FrameDumpFFMpeg.h"
#include "Common/TimeUtil.h"

#include <array>
#include <cstdarg>
#include <functional>
#include <memory>
#include <string>

#include <fmt/chrono.h>
#include <fmt/format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

#include "VideoCommon/OnScreenDisplay.h"

// Everything libav needs for the file being written.
struct FrameDumpContext
{
  AVFormatContext* muxer = nullptr;
  AVStream* stream = nullptr;
  AVCodecContext* encoder = nullptr;
  // Wraps the caller's RGBA pixels (no buffer of its own).
  AVFrame* rgba = nullptr;
  // The frame in the encoder's pixel format.
  AVFrame* converted = nullptr;
  SwsContext* scaler = nullptr;

  s64 previous_pts = AV_NOPTS_VALUE;

  int width = 0;
  int height = 0;

  // Emulated time of the file's first frame, and the savestate generation it belongs to.
  u64 first_tick = 0;
  u32 savestate_index = 0;

  bool warned_variable_rate = false;
};

namespace
{
// The video's time base: one tick per emulated refresh (denominator/numerator of the rate),
// reduced to fit `max_denominator`.
// TODO: reading the target refresh rate is not thread safe from the dump path.
AVRational RefreshTimeBase(s64 max_denominator)
{
  auto& vi = Core::System::GetInstance().GetVideoInterface();
  int num;
  int den;
  av_reduce(&num, &den, static_cast<int>(vi.GetTargetRefreshRateDenominator()),
            static_cast<int>(vi.GetTargetRefreshRateNumerator()), max_denominator);
  return AVRational{num, den};
}

Common::Log::LogLevel LibavToLogLevel(int level)
{
  if (level >= AV_LOG_DEBUG)
    return Common::Log::LogLevel::LINFO;  // libav's debug output stays visible in release builds
  if (level >= AV_LOG_INFO)
    return Common::Log::LogLevel::LINFO;
  if (level >= AV_LOG_WARNING)
    return Common::Log::LogLevel::LWARNING;
  if (level >= AV_LOG_ERROR)
    return Common::Log::LogLevel::LERROR;
  return Common::Log::LogLevel::LNOTICE;  // panic and fatal
}

void ForwardLibavLog(void*, int level, const char* format, va_list args)
{
  // Negative levels (quiet) count as debug; the high bits carry flags.
  level = level < 0 ? AV_LOG_DEBUG : (level & 0xff);
  if (level > av_log_get_level())
    return;

  const Common::Log::LogLevel ours = LibavToLogLevel(level);
  auto* const log = Common::Log::LogManager::GetInstance();
  if (log == nullptr || !log->IsEnabled(Common::Log::LogType::FRAMEDUMP, ours))
    return;

  std::array<char, 1024> text;
  CharArrayFromFormatV(text.data(), static_cast<int>(text.size()), format, args);
  GENERIC_LOG_FMT(Common::Log::LogType::FRAMEDUMP, ours, "{}", text.data());
}

void SetUpLibavOnce()
{
  static bool done = false;
  if (done)
    return;

  av_log_set_level(AV_LOG_DEBUG);
  av_log_set_callback(ForwardLibavLog);
  // TODO: avformat_network_deinit is never called.
  avformat_network_init();
  done = true;
}

// "<dump folder><game id>_<local start time>_<index>.<extension>", or the configured path.
// Returns "" when dumping should not go ahead.
std::string SegmentPath(const std::string& extension, std::time_t start, u32 index)
{
  if (std::string fixed = Config::Get(Config::GFX_DUMP_PATH); !fixed.empty())
    return fixed;

  const auto local = Common::LocalTime(start);
  if (!local)
    return "";

  const std::string stem = fmt::format(
      "{}_{:%Y-%m-%d_%H-%M-%S}_{}",
      File::GetUserPath(D_DUMPFRAMES_IDX) + SConfig::GetInstance().GetGameID(), *local, index);
  std::string path = fmt::format("{}.{}", stem, extension);

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

std::string DescribeAVError(int error)
{
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text;
  av_make_error_string(text.data(), text.size(), error);
  return fmt::format("{:8x} {}", static_cast<u32>(error), text.data());
}

// The encoder's pixel format: the configured one if valid, else a format suited to the codec.
AVPixelFormat ChoosePixelFormat(AVCodecID codec)
{
  AVPixelFormat chosen = AV_PIX_FMT_NONE;
  if (const std::string name = Config::Get(Config::GFX_DUMP_PIXEL_FORMAT); !name.empty())
  {
    chosen = av_get_pix_fmt(name.c_str());
    if (chosen == AV_PIX_FMT_NONE)
      WARN_LOG_FMT(FRAMEDUMP, "Invalid pixel format {}", name);
  }
  if (chosen != AV_PIX_FMT_NONE)
    return chosen;

  switch (codec)
  {
  case AV_CODEC_ID_FFV1:
    return AV_PIX_FMT_BGR0;
  case AV_CODEC_ID_UTVIDEO:
    return AV_PIX_FMT_GBRP;
  default:
    return AV_PIX_FMT_YUV420P;
  }
}

// The encoder: the configured one if it exists, else the default encoder of the configured
// codec (lossless means Ut Video), else the container's default codec.
const AVCodec* ChooseEncoder(const AVOutputFormat& container)
{
  AVCodecID codec_id = container.video_codec;
  const std::string codec_name =
      Config::Get(Config::GFX_USE_LOSSLESS) ? "utvideo" : Config::Get(Config::GFX_DUMP_CODEC);
  if (!codec_name.empty())
  {
    if (const AVCodecDescriptor* descriptor = avcodec_descriptor_get_by_name(codec_name.c_str()))
      codec_id = descriptor->id;
    else
      WARN_LOG_FMT(FRAMEDUMP, "Invalid codec {}", codec_name);
  }

  const AVCodec* encoder = nullptr;
  if (const std::string encoder_name = Config::Get(Config::GFX_DUMP_ENCODER); !encoder_name.empty())
  {
    encoder = avcodec_find_encoder_by_name(encoder_name.c_str());
    if (encoder == nullptr)
      WARN_LOG_FMT(FRAMEDUMP, "Invalid encoder {}", encoder_name);
  }
  return encoder != nullptr ? encoder : avcodec_find_encoder(codec_id);
}

using PacketPtr = std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>;
}  // namespace

bool FFMpegFrameDump::Start(int w, int h, u64 start_ticks)
{
  if (IsStarted())
    return true;

  m_savestate_index = 0;
  m_start_time = std::time(nullptr);
  m_file_index = 0;
  return OpenSegment(w, h, start_ticks, m_savestate_index);
}

bool FFMpegFrameDump::OpenSegment(int w, int h, u64 start_ticks, u32 savestate_index)
{
  m_context = std::make_unique<FrameDumpContext>();
  m_context->width = w;
  m_context->height = h;
  m_context->first_tick = start_ticks;
  m_context->savestate_index = savestate_index;

  SetUpLibavOnce();
  if (CreateSegmentFile())
    return true;

  CloseSegment();
  OSD::AddMessage("FrameDump Start failed");
  return false;
}

bool FFMpegFrameDump::CreateSegmentFile()
{
  FrameDumpContext& ctx = *m_context;
  const std::string container_name = Config::Get(Config::GFX_DUMP_FORMAT);

  const std::string path = SegmentPath(container_name, m_start_time, m_file_index);
  if (path.empty())
    return false;
  File::CreateFullPath(path);

  // Container.
  const AVOutputFormat* const container =
      av_guess_format(container_name.c_str(), path.c_str(), nullptr);
  if (container == nullptr)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Invalid format {}", container_name);
    return false;
  }
  if (avformat_alloc_output_context2(&ctx.muxer, container, nullptr, path.c_str()) < 0)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not allocate output context");
    return false;
  }

  // Encoder.
  const AVCodec* const encoder = ChooseEncoder(*container);
  ctx.encoder = avcodec_alloc_context3(encoder);
  if (encoder == nullptr || ctx.encoder == nullptr)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not find encoder or allocate codec context");
    return false;
  }

  m_max_denominator = std::numeric_limits<s64>::max();
  if (encoder->id == AV_CODEC_ID_MPEG4)
  {
    // Tag MPEG-4 part 2 as XviD, which players recognise more widely.
    ctx.encoder->codec_tag = MKTAG('X', 'V', 'I', 'D');
    m_max_denominator = std::numeric_limits<unsigned short>::max();
  }

  const AVRational time_base = RefreshTimeBase(m_max_denominator);
  INFO_LOG_FMT(FRAMEDUMP, "Creating video file: {} x {} @ {}/{} fps", ctx.width, ctx.height,
               time_base.den, time_base.num);

  AVCodecContext& enc = *ctx.encoder;
  enc.codec_type = AVMEDIA_TYPE_VIDEO;
  enc.bit_rate = static_cast<int64_t>(Config::Get(Config::GFX_BITRATE_KBPS)) * 1000;
  enc.width = ctx.width;
  enc.height = ctx.height;
  enc.time_base = time_base;
  // Every frame is a key frame.
  enc.gop_size = 1;
  enc.level = 1;
  enc.pix_fmt = ChoosePixelFormat(enc.codec_id);

  if (enc.codec_id == AV_CODEC_ID_UTVIDEO)
    av_opt_set_int(enc.priv_data, "pred", 3, 0);  // median prediction

  if ((container->flags & AVFMT_GLOBALHEADER) != 0)
    enc.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (avcodec_open2(&enc, encoder, nullptr) < 0)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not open codec");
    return false;
  }

  // Frames.
  ctx.rgba = av_frame_alloc();
  ctx.converted = av_frame_alloc();
  ctx.converted->format = enc.pix_fmt;
  ctx.converted->width = ctx.width;
  ctx.converted->height = ctx.height;
  if (av_frame_get_buffer(ctx.converted, 1) != 0)
    return false;

  // Stream and file.
  ctx.stream = avformat_new_stream(ctx.muxer, encoder);
  if (ctx.stream == nullptr || avcodec_parameters_from_context(ctx.stream->codecpar, &enc) < 0)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not create stream");
    return false;
  }
  ctx.stream->time_base = enc.time_base;

  NOTICE_LOG_FMT(FRAMEDUMP, "Opening file {} for dumping", path);
  if (avio_open(&ctx.muxer->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ||
      avformat_write_header(ctx.muxer, nullptr) != 0)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not open {}", path);
    return false;
  }

  // The muxer may pick its own stream time base; packets are rescaled to it.
  if (av_cmp_q(ctx.stream->time_base, time_base) != 0)
  {
    WARN_LOG_FMT(FRAMEDUMP, "Stream time base differs at {}/{}", ctx.stream->time_base.den,
                 ctx.stream->time_base.num);
  }

  OSD::AddMessage(fmt::format("Dumping Frames to \"{}\" ({}x{})", path, ctx.width, ctx.height));
  return true;
}

void FFMpegFrameDump::AddFrame(const FrameData& frame)
{
  if (!IsStarted())
    return;

  RestartIfNeeded(frame);
  // The restart may have failed.
  if (!IsStarted())
    return;

  FrameDumpContext& ctx = *m_context;

  // Presentation time from the emulated time since the file's first frame.
  // TODO: GetTicksPerSecond is not thread safe from the dump path.
  const int ticks_per_second =
      static_cast<int>(Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond());
  const s64 pts = av_rescale_q(static_cast<int64_t>(frame.state.ticks - ctx.first_tick),
                               AVRational{1, ticks_per_second}, ctx.encoder->time_base);

  if (ctx.previous_pts != AV_NOPTS_VALUE)
  {
    if (pts <= ctx.previous_pts)
    {
      WARN_LOG_FMT(FRAMEDUMP, "PTS delta < 1. Current frame will not be dumped.");
      return;
    }
    if (pts > ctx.previous_pts + 1 && !ctx.warned_variable_rate)
    {
      WARN_LOG_FMT(FRAMEDUMP, "PTS delta > 1. Resulting file will have variable frame rate. "
                              "Subsequent occurrences will not be reported.");
      ctx.warned_variable_rate = true;
    }
  }

  constexpr AVPixelFormat source_format = AV_PIX_FMT_RGBA;
  ctx.rgba->data[0] = const_cast<u8*>(frame.data);
  ctx.rgba->linesize[0] = frame.stride;
  ctx.rgba->format = source_format;
  ctx.rgba->width = ctx.width;
  ctx.rgba->height = ctx.height;

  // Convert (and scale, if a zero-sized frame kept the old size) to the encoder's format.
  ctx.scaler = sws_getCachedContext(ctx.scaler, frame.width, frame.height, source_format, ctx.width,
                                    ctx.height, ctx.encoder->pix_fmt, SWS_BICUBIC, nullptr, nullptr,
                                    nullptr);
  if (ctx.scaler != nullptr)
  {
    sws_scale(ctx.scaler, ctx.rgba->data, ctx.rgba->linesize, 0, frame.height, ctx.converted->data,
              ctx.converted->linesize);
  }

  ctx.previous_pts = pts;
  ctx.converted->pts = pts;

  if (const int error = avcodec_send_frame(ctx.encoder, ctx.converted); error != 0)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Error while encoding video: {}", DescribeAVError(error));
    return;
  }

  DrainEncoder();
}

void FFMpegFrameDump::DrainEncoder()
{
  FrameDumpContext& ctx = *m_context;
  const PacketPtr packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
  if (!packet)
  {
    ERROR_LOG_FMT(FRAMEDUMP, "Could not allocate packet");
    return;
  }

  for (;;)
  {
    const int status = avcodec_receive_packet(ctx.encoder, packet.get());
    // Nothing more for now (EAGAIN), or the encoder is flushed (EOF).
    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
      return;
    if (status != 0)
    {
      ERROR_LOG_FMT(FRAMEDUMP, "Error receiving packet: {}", DescribeAVError(status));
      return;
    }

    av_packet_rescale_ts(packet.get(), ctx.encoder->time_base, ctx.stream->time_base);
    packet->stream_index = ctx.stream->index;
    if (const int error = av_interleaved_write_frame(ctx.muxer, packet.get()); error != 0)
    {
      ERROR_LOG_FMT(FRAMEDUMP, "Error writing packet: {}", DescribeAVError(error));
      return;
    }
  }
}

void FFMpegFrameDump::Stop()
{
  if (!IsStarted())
    return;

  // A null frame asks the encoder to flush what it still holds.
  if (const int error = avcodec_send_frame(m_context->encoder, nullptr); error != 0)
    WARN_LOG_FMT(FRAMEDUMP, "Error sending flush packet: {}", DescribeAVError(error));

  DrainEncoder();
  av_write_trailer(m_context->muxer);
  CloseSegment();

  NOTICE_LOG_FMT(FRAMEDUMP, "Stopping frame dump");
  OSD::AddMessage("Stopped dumping frames");
}

bool FFMpegFrameDump::IsStarted() const
{
  return m_context != nullptr;
}

void FFMpegFrameDump::CloseSegment()
{
  FrameDumpContext& ctx = *m_context;
  av_frame_free(&ctx.rgba);
  av_frame_free(&ctx.converted);
  avcodec_free_context(&ctx.encoder);
  if (ctx.muxer != nullptr)
    avio_closep(&ctx.muxer->pb);
  avformat_free_context(ctx.muxer);
  if (ctx.scaler != nullptr)
    sws_freeContext(ctx.scaler);
  m_context.reset();
}

void FFMpegFrameDump::DoState(PointerWrap& p)
{
  // Nothing is saved; a load just starts a new generation.
  if (p.IsReadMode())
    ++m_savestate_index;
}

void FFMpegFrameDump::RestartIfNeeded(const FrameData& frame)
{
  const FrameDumpContext& ctx = *m_context;
  const AVRational& time_base = ctx.encoder->time_base;

  // A zero width or height (the VI can switch output off) keeps the current size.
  const bool size_changed = (frame.width != ctx.width || frame.height != ctx.height) &&
                            frame.width > 0 && frame.height > 0;

  if (size_changed)
  {
    INFO_LOG_FMT(FRAMEDUMP, "Starting new dump on resolution change.");
  }
  else if (ctx.previous_pts != AV_NOPTS_VALUE && frame.state.savestate_index != ctx.savestate_index)
  {
    INFO_LOG_FMT(FRAMEDUMP, "Starting new dump on savestate load.");
  }
  else if (frame.state.refresh_rate_den != time_base.num ||
           frame.state.refresh_rate_num != time_base.den)
  {
    INFO_LOG_FMT(FRAMEDUMP, "Starting new dump on refresh rate change {}/{} vs {}/{}.",
                 time_base.den, time_base.num, frame.state.refresh_rate_num,
                 frame.state.refresh_rate_den);
  }
  else
  {
    return;
  }

  Stop();
  ++m_file_index;
  OpenSegment(frame.width, frame.height, frame.state.ticks, frame.state.savestate_index);
}

FrameState FFMpegFrameDump::FetchState(u64 ticks, int frame_number) const
{
  const AVRational time_base = RefreshTimeBase(m_max_denominator);

  FrameState state;
  state.ticks = ticks;
  state.frame_number = frame_number;
  state.savestate_index = m_savestate_index;
  state.refresh_rate_num = time_base.den;
  state.refresh_rate_den = time_base.num;
  return state;
}

FFMpegFrameDump::FFMpegFrameDump() = default;

FFMpegFrameDump::~FFMpegFrameDump()
{
  Stop();
}
