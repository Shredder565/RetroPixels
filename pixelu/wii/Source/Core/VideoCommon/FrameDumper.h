// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/MathUtil.h"
#include "Common/Thread.h"

#include "VideoCommon/FrameDumpFFMpeg.h"
#include "VideoCommon/VideoEvents.h"

class AbstractStagingTexture;
class AbstractTexture;
class AbstractFramebuffer;

// Captures presented frames for screenshots and frame dumping.
//
// The video thread copies each frame into a readback staging texture. At the end of the frame
// the readback texture is swapped with the output texture, mapped, and handed to a worker
// thread, which writes a screenshot and/or appends the frame to the dump (video or PNG files).
class FrameDumper
{
public:
  FrameDumper();
  ~FrameDumper();

  // Hands the most recently captured frame (if any) to the worker thread.
  void FlushFrameDump();

  // Captures src_rect of src_texture, scaled to the size of target_rect.
  void DumpCurrentFrame(const AbstractTexture* src_texture,
                        const MathUtil::Rectangle<int>& src_rect,
                        const MathUtil::Rectangle<int>& target_rect, u64 ticks, int frame_number);

  void SaveScreenshot(std::string filename);

  bool IsFrameDumping() const;
  int GetRequiredResolutionLeastCommonMultiple() const;

  void DoState(PointerWrap& p);

private:
  // Video thread side.
  bool EnsureScaleTarget(u32 width, u32 height);
  bool EnsureReadback(u32 width, u32 height);
  void SubmitFrame(const u8* pixels, int width, int height, int stride);
  void WaitForWorker();
  void Shutdown();

  // Worker thread side.
  void WorkerLoop();
  bool BeginDump(bool use_ffmpeg, const FrameData& frame);
  void WriteFrame(bool use_ffmpeg, const FrameData& frame);
  std::string ImagePath() const;

  std::thread m_worker;
  Common::Flag m_worker_alive;
  Common::Event m_work_ready;
  Common::Event m_work_done;

  // The frame being handed to the worker, and the state captured with the pending readback.
  FrameData m_handoff;
  FrameState m_pending_state;

  // Intermediate target used when the frame has to be resized before readback.
  std::unique_ptr<AbstractTexture> m_scale_texture;
  std::unique_ptr<AbstractFramebuffer> m_scale_framebuffer;

  // m_readback receives new captures; m_mapped is the one currently read by the worker.
  std::unique_ptr<AbstractStagingTexture> m_readback;
  std::unique_ptr<AbstractStagingTexture> m_mapped;
  bool m_have_capture = false;
  bool m_worker_busy = false;

  // Number of the next PNG written when dumping frames as images.
  u32 m_image_number = 0;

  FFMpegFrameDump m_ffmpeg_dump;

  Common::Flag m_want_screenshot;
  std::mutex m_screenshot_mutex;
  std::string m_screenshot_path;

  Common::EventHook m_after_frame_hook;
};

extern std::unique_ptr<FrameDumper> g_frame_dumper;
