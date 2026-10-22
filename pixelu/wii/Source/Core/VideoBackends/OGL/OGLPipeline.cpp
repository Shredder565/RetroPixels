// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLPipeline.h"

#include <array>
#include <cstring>
#include <utility>

#include "VideoBackends/OGL/OGLShader.h"
#include "VideoBackends/OGL/OGLVertexManager.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
// GL draw modes, indexed by PrimitiveType.
constexpr std::array<GLenum, 4> GL_PRIMITIVES{GL_POINTS, GL_LINES, GL_TRIANGLES, GL_TRIANGLE_STRIP};

GLenum PrimitiveMode(PrimitiveType primitive)
{
  const auto index = static_cast<size_t>(primitive);
  return index < GL_PRIMITIVES.size() ? GL_PRIMITIVES[index] : 0;
}
}  // namespace

OGLPipeline::OGLPipeline(const AbstractPipelineConfig& config, const GLVertexFormat* vertex_format,
                         RasterizationState rasterization_state, DepthState depth_state,
                         BlendingState blending_state, PipelineProgram* program,
                         GLenum gl_primitive)
    : AbstractPipeline(config), m_vertex_format(vertex_format), m_raster(rasterization_state),
      m_depth(depth_state), m_blend(blending_state), m_program(program), m_primitive(gl_primitive)
{
}

OGLPipeline::~OGLPipeline()
{
  // Drops this pipeline's reference; the shaders themselves belong to their OGLShader objects.
  ProgramShaderCache::ReleasePipelineProgram(m_program);
}

AbstractPipeline::CacheData OGLPipeline::GetCacheData() const
{
  // Several pipelines can share one program. Only the first pipeline asked hands out the
  // binary; the program is then marked so that the cache holds it once. On the next boot the
  // cache is loaded in order and the later pipelines find the program already created.
  if (!g_backend_info.bSupportsPipelineCacheData || m_program->binary_retrieved)
    return {};

  const GLuint program = m_program->shader.glprogid;
  GLint length = 0;
  glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
  if (length == 0)
    return {};

  // Layout: the u32 binary format, then the binary.
  constexpr size_t HEADER = sizeof(u32);
  CacheData blob(static_cast<size_t>(length) + HEADER);
  GLsizei written = 0;
  GLenum format = 0;
  glGetError();  // discard any earlier error
  glGetProgramBinary(program, length, &written, &format, &blob[HEADER]);
  if (glGetError() != GL_NO_ERROR || written == 0)
    return {};

  const u32 format_u32 = static_cast<u32>(format);
  std::memcpy(blob.data(), &format_u32, HEADER);
  blob.resize(static_cast<size_t>(written) + HEADER);
  m_program->binary_retrieved = true;
  return blob;
}

std::unique_ptr<OGLPipeline> OGLPipeline::Create(const AbstractPipelineConfig& config,
                                                 const void* cache_data, size_t cache_data_size)
{
  const auto* vertex_format = static_cast<const GLVertexFormat*>(config.vertex_format);
  PipelineProgram* const program = ProgramShaderCache::GetPipelineProgram(
      vertex_format, static_cast<const OGLShader*>(config.vertex_shader),
      static_cast<const OGLShader*>(config.geometry_shader),
      static_cast<const OGLShader*>(config.pixel_shader), cache_data, cache_data_size);
  if (program == nullptr)
    return nullptr;

  return std::make_unique<OGLPipeline>(config, vertex_format, config.rasterization_state,
                                       config.depth_state, config.blending_state, program,
                                       PrimitiveMode(config.rasterization_state.primitive));
}
}  // namespace OGL
