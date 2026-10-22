// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/RenderState.h"

namespace OGL
{
// GL has no pipeline objects: a pipeline is a linked program (shared between pipelines with
// the same shaders) plus the fixed-function state OGLGfx applies when it is bound.
class OGLPipeline final : public AbstractPipeline
{
public:
  explicit OGLPipeline(const AbstractPipelineConfig& config, const GLVertexFormat* vertex_format,
                       RasterizationState rasterization_state, DepthState depth_state,
                       BlendingState blending_state, PipelineProgram* program, GLenum gl_primitive);
  ~OGLPipeline() override;

  const GLVertexFormat* GetVertexFormat() const { return m_vertex_format; }
  const RasterizationState& GetRasterizationState() const { return m_raster; }
  const DepthState& GetDepthState() const { return m_depth; }
  const BlendingState& GetBlendingState() const { return m_blend; }
  const PipelineProgram* GetProgram() const { return m_program; }
  bool HasVertexInput() const { return m_vertex_format != nullptr; }
  GLenum GetGLPrimitive() const { return m_primitive; }
  CacheData GetCacheData() const override;
  static std::unique_ptr<OGLPipeline> Create(const AbstractPipelineConfig& config,
                                             const void* cache_data, size_t cache_data_size);

private:
  const GLVertexFormat* m_vertex_format;
  RasterizationState m_raster;
  DepthState m_depth;
  BlendingState m_blend;
  PipelineProgram* m_program;
  GLenum m_primitive;
};

}  // namespace OGL
