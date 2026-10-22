// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoCommon/PerfQueryBase.h"

namespace MMIO
{
class Mapping;
}
class PointerWrap;

class AbstractGfx;
class BoundingBox;
class TextureCacheBase;
class VertexManagerBase;
class EFBInterfaceBase;

enum class FieldType
{
  Odd,
  Even,
};

// One selectable video backend (D3D11, D3D12, OpenGL, Vulkan, software, null). The base class
// holds what all of them share: the entry points the emulated hardware calls, backend
// selection, and creation/teardown of the common VideoCommon objects.
class VideoBackendBase
{
public:
  virtual ~VideoBackendBase() {}
  virtual bool Initialize(const WindowSystemInfo& wsi) = 0;
  virtual void Shutdown() = 0;

  virtual std::string GetConfigName() const = 0;
  virtual std::string GetDisplayName() const { return GetConfigName(); }
  virtual void InitBackendInfo(const WindowSystemInfo& wsi) = 0;
  virtual std::optional<std::string> GetWarningMessage() const { return {}; }

  // Gets a native window ready for rendering; runs on the thread that owns the window.
  virtual void PrepareWindow(WindowSystemInfo& /*wsi*/) {}

  // Path for dumping a shader that failed to compile.
  static std::string BadShaderFilename(const char* shader_stage, int counter);

  // CPU-thread entry points (video interface, PE counters, bounding box).
  void Video_OutputXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks);
  u32 Video_GetQueryResult(PerfQueryType type);
  u16 Video_GetBoundingBox(int index);

  static std::string GetDefaultBackendConfigName();
  static std::string GetDefaultBackendDisplayName();
  static const std::vector<std::unique_ptr<VideoBackendBase>>& GetAvailableBackends();
  static void ActivateBackend(const std::string& name);

  // Selects the configured backend and lets it fill g_backend_info for the current device.
  static void PopulateBackendInfo(const WindowSystemInfo& wsi);

  // Saves/loads the video state, on the GPU thread in dual-core mode.
  void DoState(PointerWrap& p);

protected:
  // Hardware backends: the standard EFB interface and texture cache are used.
  bool InitializeShared(std::unique_ptr<AbstractGfx> gfx,
                        std::unique_ptr<VertexManagerBase> vertex_manager,
                        std::unique_ptr<PerfQueryBase> perf_query,
                        std::unique_ptr<BoundingBox> bounding_box);

  // Software and null backends bring their own EFB interface and texture cache.
  bool InitializeShared(std::unique_ptr<AbstractGfx> gfx,
                        std::unique_ptr<VertexManagerBase> vertex_manager,
                        std::unique_ptr<PerfQueryBase> perf_query,
                        std::unique_ptr<BoundingBox> bounding_box,
                        std::unique_ptr<EFBInterfaceBase> efb_interface,
                        std::unique_ptr<TextureCacheBase> texture_cache);
  void ShutdownShared();

  bool m_initialized = false;
};

extern VideoBackendBase* g_video_backend;
