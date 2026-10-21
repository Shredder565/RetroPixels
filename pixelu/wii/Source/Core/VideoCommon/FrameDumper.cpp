// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/FrameDumper.h"

#include <utility>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

std::unique_ptr<FrameDumper> g_frame_dumper;

namespace
{
// Video encoders work on blocks, so dumped frame sizes are rounded to a multiple of this.
constexpr int ENCODER_SIZE_MULTIPLE = 4;

TextureConfig CaptureConfig(u32 width, u32 height, u32 flags)
{
  return TextureConfig(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, flags,
                       AbstractTextureType::Texture_2DArray);
}

bool WritePNG(const std::string& path, const FrameData& frame)
{
  const int level = Config::Get(Config::GFX_PNG_COMPRESSION_LEVEL);
  return Common::ConvertRGBAToRGBAndSavePNG(path, frame.data, static_cast<u32>(frame.width),
                                            static_cast<u32>(frame.height),
                                            static_cast<u32>(frame.stride), level);
}
}  // namespace

FrameDumper::FrameDumper()
{
  m_after_frame_hook =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { FlushFrameDump(); });
}

FrameDumper::~FrameDumper()
{
  Shutdown();
}

// ---------------------------------------------------------------------------------------------
// Video thread
// ---------------------------------------------------------------------------------------------

void FrameDumper::DumpCurrentFrame(const AbstractTexture* src_texture,
                                   const MathUtil::Rectangle<int>& src_rect,
                                   const MathUtil::Rectangle<int>& target_rect, u64 ticks,
                                   int frame_number)
{
  const int out_w = target_rect.GetWidth();
  const int out_h = target_rect.GetHeight();

  const AbstractTexture* source = src_texture;
  MathUtil::Rectangle<int> region = src_rect;

  // A staging copy cannot resize, so draw into an intermediate target first when needed.
  const bool resize = src_rect.GetWidth() != out_w || src_rect.GetHeight() != out_h;
  if (resize)
  {
    if (!EnsureScaleTarget(static_cast<u32>(out_w), static_cast<u32>(out_h)))
      return;
    g_gfx->ScaleTexture(m_scale_framebuffer.get(), m_scale_framebuffer->GetRect(), src_texture,
                        src_rect);
    source = m_scale_texture.get();
    region = source->GetRect();
  }

  if (!EnsureReadback(static_cast<u32>(out_w), static_cast<u32>(out_h)))
    return;

  m_readback->CopyFromTexture(source, region, 0, 0, m_readback->GetRect());
  m_pending_state = m_ffmpeg_dump.FetchState(ticks, frame_number);
  m_have_capture = true;
}

bool FrameDumper::EnsureScaleTarget(u32 width, u32 height)
{
  if (m_scale_texture && m_scale_texture->GetWidth() == width &&
      m_scale_texture->GetHeight() == height)
  {
    return true;
  }

  // Free the old target first so two of them never exist at once.
  m_scale_framebuffer.reset();
  m_scale_texture.reset();

  m_scale_texture = g_gfx->CreateTexture(
      CaptureConfig(width, height, AbstractTextureFlag_RenderTarget), "Frame dump render texture");
  if (m_scale_texture == nullptr)
  {
    PanicAlertFmt("Failed to allocate frame dump render texture");
    return false;
  }

  m_scale_framebuffer = g_gfx->CreateFramebuffer(m_scale_texture.get(), nullptr);
  ASSERT(m_scale_framebuffer);
  return true;
}

bool FrameDumper::EnsureReadback(u32 width, u32 height)
{
  if (m_readback && m_readback->GetWidth() == width && m_readback->GetHeight() == height)
    return true;

  m_readback.reset();
  m_readback =
      g_gfx->CreateStagingTexture(StagingTextureType::Readback, CaptureConfig(width, height, 0));
  return m_readback != nullptr;
}

void FrameDumper::FlushFrameDump()
{
  if (!m_have_capture)
    return;

  // The worker may still be reading the output texture; it must finish before the swap.
  WaitForWorker();
  std::swap(m_readback, m_mapped);

  m_mapped->Flush();
  if (m_mapped->Map())
  {
    const TextureConfig& cfg = m_mapped->GetConfig();
    SubmitFrame(reinterpret_cast<u8*>(m_mapped->GetMappedPointer()), static_cast<int>(cfg.width),
                static_cast<int>(cfg.height), static_cast<int>(m_mapped->GetMappedStride()));
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Failed to map texture for dumping.");
  }
  m_have_capture = false;

  if (!IsFrameDumping())
    Shutdown();
}

void FrameDumper::SubmitFrame(const u8* pixels, int width, int height, int stride)
{
  m_handoff = FrameData{pixels, width, height, stride, m_pending_state};

  if (!m_worker_alive.IsSet())
  {
    // A worker that exited earlier still has to be joined before a new one starts.
    if (m_worker.joinable())
      m_worker.join();
    m_worker_alive.Set();
    m_worker = std::thread(&FrameDumper::WorkerLoop, this);
  }

  m_work_ready.Set();
  m_worker_busy = true;
}

void FrameDumper::WaitForWorker()
{
  if (!m_worker_busy)
    return;

  m_work_done.Wait();
  m_worker_busy = false;
  m_mapped->Unmap();
}

void FrameDumper::Shutdown()
{
  // Anything already captured still goes to the worker first.
  FlushFrameDump();

  if (!m_worker_alive.IsSet())
    return;

  WaitForWorker();

  // Clearing the flag before the wake-up makes the worker leave its loop.
  m_worker_alive.Clear();
  m_work_ready.Set();
  if (m_worker.joinable())
    m_worker.join();

  m_scale_framebuffer.reset();
  m_scale_texture.reset();
  m_readback.reset();
  m_mapped.reset();
}

// ---------------------------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------------------------

void FrameDumper::WorkerLoop()
{
  Common::SetCurrentThreadName("FrameDumping");

  bool use_ffmpeg = !Config::Get(Config::GFX_DUMP_FRAMES_AS_IMAGES);
#if !defined(HAVE_FFMPEG)
  if (use_ffmpeg)
  {
    WARN_LOG_FMT(VIDEO, "FrameDump: Dolphin was not compiled with FFmpeg, using fallback option. "
                        "Frames will be saved as PNG images instead.");
    use_ffmpeg = false;
  }
#endif

  bool dumping = false;
  for (;;)
  {
    m_work_ready.Wait();
    if (!m_worker_alive.IsSet())
      break;

    const FrameData frame = m_handoff;

    if (m_want_screenshot.TestAndClear())
    {
      std::lock_guard guard(m_screenshot_mutex);
      if (WritePNG(m_screenshot_path, frame))
        OSD::AddMessage("Screenshot saved to " + m_screenshot_path);
      m_screenshot_path.clear();
    }

    if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
    {
      if (!dumping)
      {
        dumping = BeginDump(use_ffmpeg, frame);
        // Turn the option back off when the dump could not be started.
        if (!dumping)
          Config::SetCurrent(Config::MAIN_MOVIE_DUMP_FRAMES, false);
      }
      if (dumping)
        WriteFrame(use_ffmpeg, frame);
    }

    m_work_done.Set();
  }

  // Image dumps need no finishing step; a video file has to be closed.
  if (dumping && use_ffmpeg)
  {
#if defined(HAVE_FFMPEG)
    m_ffmpeg_dump.Stop();
#endif
  }
}

bool FrameDumper::BeginDump(bool use_ffmpeg, const FrameData& frame)
{
  if (use_ffmpeg)
  {
#if defined(HAVE_FFMPEG)
    // A dump that begins with the first frame is timed from boot, keeping audio in sync.
    const u64 first_ticks = frame.state.frame_number != 0 ? frame.state.ticks : 0;
    return m_ffmpeg_dump.Start(frame.width, frame.height, first_ticks);
#else
    (void)frame;
    return false;
#endif
  }

  m_image_number = 1;
  if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES_SILENT))
    return true;

  // Asking about the first file is enough: a previous dump always wrote at least that one.
  const std::string first = ImagePath();
  if (!File::Exists(first))
    return true;
  return AskYesNoFmtT("Frame dump image(s) '{0}' already exists. Overwrite?", first);
}

void FrameDumper::WriteFrame(bool use_ffmpeg, const FrameData& frame)
{
  if (use_ffmpeg)
  {
#if defined(HAVE_FFMPEG)
    m_ffmpeg_dump.AddFrame(frame);
#endif
    return;
  }

  WritePNG(ImagePath(), frame);
  ++m_image_number;
}

std::string FrameDumper::ImagePath() const
{
  return fmt::format("{}framedump_{}.png", File::GetUserPath(D_DUMPFRAMES_IDX), m_image_number);
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------

void FrameDumper::SaveScreenshot(std::string filename)
{
  std::lock_guard guard(m_screenshot_mutex);
  m_screenshot_path = std::move(filename);
  m_want_screenshot.Set();
}

bool FrameDumper::IsFrameDumping() const
{
  return m_want_screenshot.IsSet() || Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES);
}

int FrameDumper::GetRequiredResolutionLeastCommonMultiple() const
{
  return Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES) ? ENCODER_SIZE_MULTIPLE : 1;
}

void FrameDumper::DoState(PointerWrap& p)
{
#if defined(HAVE_FFMPEG)
  m_ffmpeg_dump.DoState(p);
#else
  (void)p;
#endif
}
