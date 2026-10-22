// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"

class AbstractShader;
class NativeVertexFormat;

// Which resource layout a pipeline is built against.
//   GX:      one uniform buffer per stage (the pixel stage can read the vertex constants too),
//            8 combined image samplers for the pixel stage, and a pixel-stage storage buffer
//            when bounding box emulation is on.
//   GXUber:  as GX, plus a vertex-stage storage buffer for dynamic vertex loading.
//   Utility: a single uniform buffer for all stages (set 0, binding 1), 8 pixel-stage samplers
//            (set 1, bindings 0-7) and one texel buffer (set 2, binding 0).
// Compute shaders use the utility layout plus one storage image (set 3, binding 0).
enum class AbstractPipelineUsage
{
  GX,
  GXUber,
  Utility
};

struct AbstractPipelineConfig
{
  const NativeVertexFormat* vertex_format = nullptr;
  const AbstractShader* vertex_shader = nullptr;
  const AbstractShader* geometry_shader = nullptr;
  const AbstractShader* pixel_shader = nullptr;
  RasterizationState rasterization_state;
  DepthState depth_state;
  BlendingState blending_state;
  FramebufferState framebuffer_state;

  AbstractPipelineUsage usage = AbstractPipelineUsage::GX;

  // Field-by-field comparison key; also gives the ordering used by pipeline maps.
  auto AsTuple() const
  {
    return std::make_tuple(vertex_format, vertex_shader, geometry_shader, pixel_shader,
                           rasterization_state.hex, depth_state.hex, blending_state.hex,
                           framebuffer_state.hex, usage);
  }
  bool operator==(const AbstractPipelineConfig& rhs) const { return AsTuple() == rhs.AsTuple(); }
  bool operator<(const AbstractPipelineConfig& rhs) const { return AsTuple() < rhs.AsTuple(); }
};

// A complete graphics pipeline object owned by a backend.
class AbstractPipeline
{
public:
  AbstractPipeline() = default;
  explicit AbstractPipeline(const AbstractPipelineConfig& config) : m_config(config) {}
  virtual ~AbstractPipeline() = default;

  AbstractPipelineConfig m_config;

  // Driver-specific data (for example compiled shader ISA) that can be handed back to the
  // backend when the same pipeline is created again, to make creation faster. The shaders and
  // the config are still needed.
  using CacheData = std::vector<u8>;
  virtual CacheData GetCacheData() const { return {}; }
};
