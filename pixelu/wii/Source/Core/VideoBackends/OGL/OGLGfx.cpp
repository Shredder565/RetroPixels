// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLGfx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "Common/GL/GLContext.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/GL/GLUtil.h"
#include "Common/Logging/LogManager.h"

#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLPipeline.h"
#include "VideoBackends/OGL/OGLShader.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/SamplerCache.h"

#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
VideoConfig g_ogl_config;

namespace
{
// ---------------------------------------------------------------------------------------------
// GL debug output

const char* DebugSourceName(GLenum source)
{
  switch (source)
  {
  case GL_DEBUG_SOURCE_API_ARB:
    return "API";
  case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB:
    return "Window System";
  case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB:
    return "Shader Compiler";
  case GL_DEBUG_SOURCE_THIRD_PARTY_ARB:
    return "Third Party";
  case GL_DEBUG_SOURCE_APPLICATION_ARB:
    return "Application";
  case GL_DEBUG_SOURCE_OTHER_ARB:
    return "Other";
  default:
    return "Unknown";
  }
}

const char* DebugTypeName(GLenum type)
{
  switch (type)
  {
  case GL_DEBUG_TYPE_ERROR_ARB:
    return "Error";
  case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB:
    return "Deprecated";
  case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB:
    return "Undefined";
  case GL_DEBUG_TYPE_PORTABILITY_ARB:
    return "Portability";
  case GL_DEBUG_TYPE_PERFORMANCE_ARB:
    return "Performance";
  case GL_DEBUG_TYPE_OTHER_ARB:
    return "Other";
  default:
    return "Unknown";
  }
}

// High severity and anything unrecognised are errors, medium is a warning, the rest is debug
// output.
Common::Log::LogLevel DebugSeverityLevel(GLenum severity)
{
  switch (severity)
  {
  case GL_DEBUG_SEVERITY_MEDIUM_ARB:
    return Common::Log::LogLevel::LWARNING;
  case GL_DEBUG_SEVERITY_LOW_ARB:
  case GL_DEBUG_SEVERITY_NOTIFICATION:
    return Common::Log::LogLevel::LDEBUG;
  default:
    return Common::Log::LogLevel::LERROR;
  }
}

void APIENTRY OnDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity,
                             GLsizei /*length*/, const char* message, const void* /*user*/)
{
  // NVIDIA's "application thread syncing with server thread" performance note is pure noise.
  constexpr GLuint NVIDIA_THREAD_SYNC_NOTE = 0x200b0;
  if (id == NVIDIA_THREAD_SYNC_NOTE)
    return;

  const char* source_name = DebugSourceName(source);
  const char* type_name = DebugTypeName(type);
  switch (DebugSeverityLevel(severity))
  {
  case Common::Log::LogLevel::LWARNING:
    WARN_LOG_FMT(HOST_GPU, "id: {:x}, source: {}, type: {} - {}", id, source_name, type_name,
                 message);
    break;
  case Common::Log::LogLevel::LDEBUG:
    DEBUG_LOG_FMT(HOST_GPU, "id: {:x}, source: {}, type: {} - {}", id, source_name, type_name,
                  message);
    break;
  default:
    ERROR_LOG_FMT(HOST_GPU, "id: {:x}, source: {}, type: {} - {}", id, source_name, type_name,
                  message);
    break;
  }
}

// GL 3.x without ARB_ES2_compatibility only has the double-precision depth functions.
void APIENTRY DepthRangeFloatShim(GLfloat near_depth, GLfloat far_depth)
{
  glDepthRange(near_depth, far_depth);
}

void APIENTRY ClearDepthFloatShim(GLfloat depth)
{
  glClearDepth(depth);
}

// ---------------------------------------------------------------------------------------------
// Fixed-function state translation (indexed by the emulated enum values)

// CompareMode: never, less, equal, lequal, greater, nequal, gequal, always.
constexpr GLenum COMPARE_FUNCS[8] = {GL_NEVER,   GL_LESS,     GL_EQUAL,  GL_LEQUAL,
                                     GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

// Source and destination factors share slots 4 and 5 (source alpha), which switch to the
// second colour output with dual-source blending.
constexpr GLenum SOURCE_FACTORS[8] = {GL_ZERO,      GL_ONE,
                                      GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                      GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
constexpr GLenum DEST_FACTORS[8] = {GL_ZERO,      GL_ONE,
                                    GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};

GLenum BlendFactor(const GLenum (&table)[8], u32 index, bool dual_source)
{
  if (dual_source && index == 4)
    return GL_SRC1_ALPHA;
  if (dual_source && index == 5)
    return GL_ONE_MINUS_SRC1_ALPHA;
  return table[index];
}

// LogicOp 0-15 in GX order, which matches the GL list.
constexpr GLenum LOGIC_OPS[16] = {GL_CLEAR,         GL_AND,         GL_AND_REVERSE, GL_COPY,
                                  GL_AND_INVERTED,  GL_NOOP,        GL_XOR,         GL_OR,
                                  GL_NOR,           GL_EQUIV,       GL_INVERT,      GL_OR_REVERSE,
                                  GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND,        GL_SET};

GLboolean AsGLBool(bool value)
{
  return value ? GL_TRUE : GL_FALSE;
}

bool HostGpuErrorsLogged()
{
  return Common::Log::LogManager::GetInstance()->IsEnabled(Common::Log::LogType::HOST_GPU,
                                                           Common::Log::LogLevel::LERROR);
}

void SetEnabled(GLenum cap, bool enable)
{
  if (enable)
    glEnable(cap);
  else
    glDisable(cap);
}

const OGLPipeline* AsGL(const AbstractPipeline* pipeline)
{
  return static_cast<const OGLPipeline*>(pipeline);
}
}  // namespace

OGLGfx::OGLGfx(std::unique_ptr<GLContext> main_gl_context, float backbuffer_scale)
    : m_context(std::move(main_gl_context)),
      m_raster_state(RenderState::GetInvalidRasterizationState()),
      m_depth_state(RenderState::GetInvalidDepthState()),
      m_blend_state(RenderState::GetInvalidBlendingState()), m_surface_scale(backbuffer_scale)
{
  // Framebuffer object 0 is the window.
  if (!m_context->IsHeadless())
  {
    m_window_framebuffer = std::make_unique<OGLFramebuffer>(
        nullptr, nullptr, std::vector<AbstractTexture*>{}, AbstractTextureFormat::RGBA8,
        AbstractTextureFormat::Undefined, std::max(m_context->GetBackBufferWidth(), 1u),
        std::max(m_context->GetBackBufferHeight(), 1u), 1, 1, 0);
    m_current_framebuffer = m_window_framebuffer.get();
  }

  // The float depth functions are core from 4.1; route them to the double versions otherwise.
  if (!m_context->IsGLES() && !GLExtensions::Supports("GL_ARB_ES2_compatibility"))
  {
    glDepthRangef = DepthRangeFloatShim;
    glClearDepthf = ClearDepthFloatShim;
  }

  InitDriverInfo();

  if (g_ogl_config.bSupportsDebug)
  {
    // Ask for every message; the callback sorts them by severity.
    if (GLExtensions::Supports("GL_KHR_debug"))
    {
      glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
      glDebugMessageCallback(OnDebugMessage, nullptr);
    }
    else
    {
      glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
      glDebugMessageCallbackARB(OnDebugMessage, nullptr);
    }
    SyncDebugOutputWithLogLevel();
  }

  if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VSYNC))
    m_context->SwapInterval(g_ActiveConfig.bVSyncActive);

  // Depth in [0, 1] with a lower-left origin, like the other backends' clip space.
  if (g_backend_info.bSupportsClipControl)
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

  // Depth clamping is emulated with two clip distances plus GL depth clamp.
  if (g_backend_info.bSupportsDepthClamp)
  {
    SetClipDistances(true);
    glEnable(GL_DEPTH_CLAMP);
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  glGenFramebuffers(1, &m_scratch_read_fbo);
  glGenFramebuffers(1, &m_scratch_draw_fbo);

  if (g_backend_info.bSupportsPrimitiveRestart)
    GLUtil::EnablePrimitiveRestart(m_context.get());

  UpdateActiveConfig();
}

OGLGfx::~OGLGfx()
{
  glDeleteFramebuffers(1, &m_scratch_draw_fbo);
  glDeleteFramebuffers(1, &m_scratch_read_fbo);
}

void OGLGfx::SyncDebugOutputWithLogLevel()
{
  // Debug output costs driver time; only have it while HOST_GPU errors are being logged.
  SetEnabled(GL_DEBUG_OUTPUT, HostGpuErrorsLogged());
}

void OGLGfx::SetClipDistances(bool enable)
{
  SetEnabled(GL_CLIP_DISTANCE0, enable);
  SetEnabled(GL_CLIP_DISTANCE1, enable);
}

bool OGLGfx::IsHeadless() const
{
  return m_context->IsHeadless();
}

bool OGLGfx::IsGLES() const
{
  return m_context->IsGLES();
}

// ---------------------------------------------------------------------------------------------
// Object creation

std::unique_ptr<AbstractTexture> OGLGfx::CreateTexture(const TextureConfig& config,
                                                       std::string_view name)
{
  return std::make_unique<OGLTexture>(config, name);
}

std::unique_ptr<AbstractStagingTexture> OGLGfx::CreateStagingTexture(StagingTextureType type,
                                                                     const TextureConfig& config)
{
  return OGLStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractFramebuffer>
OGLGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                          std::vector<AbstractTexture*> additional_color_attachments)
{
  return OGLFramebuffer::Create(static_cast<OGLTexture*>(color_attachment),
                                static_cast<OGLTexture*>(depth_attachment),
                                std::move(additional_color_attachments));
}

std::unique_ptr<AbstractShader>
OGLGfx::CreateShaderFromSource(ShaderStage stage, std::string_view source,
                               VideoCommon::ShaderIncluder* shader_includer, std::string_view name)
{
  return OGLShader::CreateFromSource(stage, source, shader_includer, name);
}

std::unique_ptr<AbstractShader> OGLGfx::CreateShaderFromBinary(ShaderStage, const void*, size_t,
                                                               std::string_view)
{
  // GL has program binaries but no per-stage binaries.
  return nullptr;
}

std::unique_ptr<AbstractPipeline> OGLGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                         const void* cache_data,
                                                         size_t cache_data_length)
{
  return OGLPipeline::Create(config, cache_data, cache_data_length);
}

std::unique_ptr<VideoCommon::AsyncShaderCompiler> OGLGfx::CreateAsyncShaderCompiler()
{
  return std::make_unique<SharedContextAsyncShaderCompiler>();
}

// ---------------------------------------------------------------------------------------------
// Pipeline state

void OGLGfx::SetRasterization(RasterizationState state)
{
  if (state == m_raster_state)
    return;

  // Culling everything (CullMode::All) is treated like culling back faces with CW front.
  const CullMode cull = state.cull_mode;
  if (cull == CullMode::None)
  {
    glDisable(GL_CULL_FACE);
  }
  else
  {
    glEnable(GL_CULL_FACE);
    glFrontFace(cull == CullMode::Front ? GL_CCW : GL_CW);
  }
  m_raster_state = state;
}

void OGLGfx::SetDepth(DepthState state)
{
  if (state == m_depth_state)
    return;

  if (!state.test_enable)
  {
    // No test means no writes either.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
  }
  else
  {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(AsGLBool(state.update_enable != 0));
    glDepthFunc(COMPARE_FUNCS[static_cast<u32>(state.func.Value())]);
  }
  m_depth_state = state;
}

void OGLGfx::SetBlending(BlendingState state)
{
  if (state == m_blend_state)
    return;

  SetEnabled(GL_BLEND, state.blend_enable != 0);

  // The equation and factors are set even with blending off: some drivers misbehave otherwise
  // (seen in Sonic Adventure 2 Battle's first Dark level).
  const bool dual = state.use_dual_src != 0;
  glBlendEquationSeparate(state.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD,
                          state.subtract_alpha ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
  glBlendFuncSeparate(
      BlendFactor(SOURCE_FACTORS, static_cast<u32>(state.src_factor.Value()), dual),
      BlendFactor(DEST_FACTORS, static_cast<u32>(state.dst_factor.Value()), dual),
      BlendFactor(SOURCE_FACTORS, static_cast<u32>(state.src_factor_alpha.Value()), dual),
      BlendFactor(DEST_FACTORS, static_cast<u32>(state.dst_factor_alpha.Value()), dual));

  // ES has no logic ops.
  if (!IsGLES())
  {
    SetEnabled(GL_COLOR_LOGIC_OP, state.logic_op_enable != 0);
    if (state.logic_op_enable)
      glLogicOp(LOGIC_OPS[static_cast<u32>(state.logic_mode.Value())]);
  }

  const GLboolean rgb = AsGLBool(state.color_update != 0);
  glColorMask(rgb, rgb, rgb, AsGLBool(state.alpha_update != 0));
  m_blend_state = state;
}

void OGLGfx::SetPipeline(const AbstractPipeline* pipeline)
{
  if (pipeline == m_current_pipeline)
    return;

  if (pipeline == nullptr)
  {
    ProgramShaderCache::InvalidateLastProgram();
    glUseProgram(0);
  }
  else
  {
    const OGLPipeline* gl_pipeline = AsGL(pipeline);
    SetRasterization(gl_pipeline->GetRasterizationState());
    SetDepth(gl_pipeline->GetDepthState());
    SetBlending(gl_pipeline->GetBlendingState());
    ProgramShaderCache::BindVertexFormat(gl_pipeline->GetVertexFormat());
    gl_pipeline->GetProgram()->shader.Bind();
  }
  m_current_pipeline = pipeline;
}

// ---------------------------------------------------------------------------------------------
// Resource binding

void OGLGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  const auto* gl_texture = static_cast<const OGLTexture*>(texture);
  if (m_unit_textures[index] == gl_texture)
    return;

  glActiveTexture(GL_TEXTURE0 + index);
  if (gl_texture == nullptr)
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  else
    glBindTexture(gl_texture->GetGLTarget(), gl_texture->GetGLTextureId());
  m_unit_textures[index] = gl_texture;
}

void OGLGfx::SetSamplerState(u32 index, const SamplerState& state)
{
  g_sampler_cache->SetSamplerState(index, state);
}

void OGLGfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  if (m_image_units[index] == texture)
    return;

  if (texture == nullptr)
  {
    glBindImageTexture(index, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
  }
  else
  {
    const auto* gl_texture = static_cast<OGLTexture*>(texture);
    GLenum access = GL_WRITE_ONLY;
    if (read)
      access = write ? GL_READ_WRITE : GL_READ_ONLY;
    glBindImageTexture(index, gl_texture->GetGLTextureId(), 0, GL_TRUE, 0, access,
                       gl_texture->GetGLFormatForImageTexture());
  }
  m_image_units[index] = texture;
}

void OGLGfx::UnbindTexture(const AbstractTexture* texture)
{
  // Clear every texture unit and then every image unit that still refers to the texture.
  for (u32 unit = 0; unit < m_unit_textures.size(); ++unit)
  {
    if (m_unit_textures[unit] == texture)
    {
      glActiveTexture(GL_TEXTURE0 + unit);
      glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
      m_unit_textures[unit] = nullptr;
    }
  }
  for (u32 unit = 0; unit < m_image_units.size(); ++unit)
  {
    if (m_image_units[unit] == texture)
    {
      glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
      m_image_units[unit] = nullptr;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Framebuffers and clears

void OGLGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (framebuffer == m_current_framebuffer)
    return;

  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<OGLFramebuffer*>(framebuffer)->GetFBO());
  m_current_framebuffer = framebuffer;
}

void OGLGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  // Nothing to gain from discarding on desktop GL.
  SetFramebuffer(framebuffer);
}

void OGLGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                    float depth_value)
{
  SetFramebuffer(framebuffer);

  const bool has_color = framebuffer->HasColorBuffer();
  const bool has_depth = framebuffer->HasDepthBuffer();

  // The clear covers the whole framebuffer and every channel; the write masks of the current
  // pipeline and the scissor test are restored afterwards.
  glDisable(GL_SCISSOR_TEST);
  GLbitfield buffers = 0;
  if (has_color)
  {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(color_value[0], color_value[1], color_value[2], color_value[3]);
    buffers |= GL_COLOR_BUFFER_BIT;
  }
  if (has_depth)
  {
    glDepthMask(GL_TRUE);
    glClearDepthf(depth_value);
    buffers |= GL_DEPTH_BUFFER_BIT;
  }
  glClear(buffers);
  glEnable(GL_SCISSOR_TEST);

  if (has_color)
  {
    const GLboolean rgb = AsGLBool(m_blend_state.color_update != 0);
    glColorMask(rgb, rgb, rgb, AsGLBool(m_blend_state.alpha_update != 0));
  }
  if (has_depth)
    glDepthMask(AsGLBool(m_depth_state.update_enable != 0));
}

void OGLGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                         bool alphaEnable, bool zEnable, u32 color, u32 z)
{
  const bool clear_color = colorEnable || alphaEnable;
  GLbitfield buffers = 0;
  if (clear_color)
  {
    // color is ARGB8888.
    glColorMask(AsGLBool(colorEnable), AsGLBool(colorEnable), AsGLBool(colorEnable),
                AsGLBool(alphaEnable));
    const auto channel = [color](int shift) {
      return static_cast<float>((color >> shift) & 0xFF) / 255.0f;
    };
    glClearColor(channel(16), channel(8), channel(0), channel(24));
    buffers = GL_COLOR_BUFFER_BIT;
  }
  if (zEnable)
  {
    // z is a 24-bit depth value.
    glDepthMask(GL_TRUE);
    glClearDepthf(static_cast<float>(z & 0xFFFFFF) / 16777216.0f);
    buffers |= GL_DEPTH_BUFFER_BIT;
  }

  // glClear honours the write masks and the scissor rectangle (not the viewport).
  SetScissorRect(target_rc);
  glClear(buffers);

  if (clear_color)
  {
    const GLboolean rgb = AsGLBool(m_blend_state.color_update != 0);
    glColorMask(rgb, rgb, rgb, AsGLBool(m_blend_state.alpha_update != 0));
  }
  if (zEnable)
    glDepthMask(AsGLBool(m_depth_state.update_enable != 0));
}

void OGLGfx::BindSharedReadFramebuffer()
{
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_scratch_read_fbo);
}

void OGLGfx::BindSharedDrawFramebuffer()
{
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scratch_draw_fbo);
}

void OGLGfx::RestoreFramebufferBinding()
{
  const GLuint fbo = m_current_framebuffer == nullptr ?
                         0 :
                         static_cast<OGLFramebuffer*>(m_current_framebuffer)->GetFBO();
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

// ---------------------------------------------------------------------------------------------
// Viewport, scissor and draws

void OGLGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  glScissor(rc.left, rc.top, rc.GetWidth(), rc.GetHeight());
}

void OGLGfx::SetViewport(float x, float y, float width, float height, float near_depth,
                         float far_depth)
{
  if (g_ogl_config.bSupportViewportFloat)
  {
    glViewportIndexedf(0, x, y, width, height);
  }
  else
  {
    // Integer viewports round each value up.
    const auto up = [](float value) { return static_cast<GLint>(std::ceil(value)); };
    glViewport(up(x), up(y), up(width), up(height));
  }
  glDepthRangef(near_depth, far_depth);
}

void OGLGfx::Draw(u32 base_vertex, u32 num_vertices)
{
  glDrawArrays(AsGL(m_current_pipeline)->GetGLPrimitive(), static_cast<GLint>(base_vertex),
               static_cast<GLsizei>(num_vertices));
}

void OGLGfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  // Indices are 16-bit, read from the bound element buffer at an offset of base_index.
  const GLenum primitive = AsGL(m_current_pipeline)->GetGLPrimitive();
  const auto* offset =
      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(base_index) * sizeof(u16));
  if (g_ogl_config.bSupportsGLBaseVertex)
  {
    glDrawElementsBaseVertex(primitive, static_cast<GLsizei>(num_indices), GL_UNSIGNED_SHORT,
                             offset, static_cast<GLint>(base_vertex));
  }
  else
  {
    glDrawElements(primitive, static_cast<GLsizei>(num_indices), GL_UNSIGNED_SHORT, offset);
  }
}

void OGLGfx::DispatchComputeShader(const AbstractShader* shader, u32, u32, u32, u32 groups_x,
                                   u32 groups_y, u32 groups_z)
{
  glUseProgram(static_cast<const OGLShader*>(shader)->GetGLComputeProgramID());
  glDispatchCompute(groups_x, groups_y, groups_z);

  // Put the graphics program back.
  ProgramShaderCache::InvalidateLastProgram();
  if (m_current_pipeline != nullptr)
    AsGL(m_current_pipeline)->GetProgram()->shader.Bind();

  // Make image writes visible to texture fetches that follow.
  const bool any_image = std::ranges::any_of(
      m_image_units, [](const AbstractTexture* image) { return image != nullptr; });
  if (any_image)
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
}

void OGLGfx::BeginUtilityDrawing()
{
  AbstractGfx::BeginUtilityDrawing();
  // Utility shaders do not write the clip distances.
  if (g_backend_info.bSupportsDepthClamp)
    SetClipDistances(false);
}

void OGLGfx::EndUtilityDrawing()
{
  AbstractGfx::EndUtilityDrawing();
  if (g_backend_info.bSupportsDepthClamp)
    SetClipDistances(true);
}

// ---------------------------------------------------------------------------------------------
// Window and presentation

void OGLGfx::SelectLeftBuffer()
{
  glDrawBuffer(GL_BACK_LEFT);
}

void OGLGfx::SelectRightBuffer()
{
  glDrawBuffer(GL_BACK_RIGHT);
}

void OGLGfx::SelectMainBuffer()
{
  glDrawBuffer(GL_BACK);
}

void OGLGfx::ResizeWindowFramebuffer()
{
  const u32 width = m_context->GetBackBufferWidth();
  const u32 height = m_context->GetBackBufferHeight();
  g_presenter->SetBackbuffer(static_cast<int>(width), static_cast<int>(height));
  m_window_framebuffer->UpdateDimensions(width, height);
}

void OGLGfx::PickUpSurfaceChange()
{
  if (!g_presenter->SurfaceChangedTestAndClear())
    return;

  // A new surface usually comes with a new size.
  m_context->UpdateSurface(g_presenter->GetNewSurfaceHandle());
  ResizeWindowFramebuffer();
}

void OGLGfx::PickUpSurfaceResize()
{
  if (!g_presenter->SurfaceResizedTestAndClear())
    return;

  m_context->Update();
  ResizeWindowFramebuffer();
}

bool OGLGfx::BindBackbuffer(const ClearColor& clear_color)
{
  PickUpSurfaceChange();
  PickUpSurfaceResize();
  SetAndClearFramebuffer(m_window_framebuffer.get(), clear_color);
  return true;
}

void OGLGfx::PresentBackbuffer()
{
  // The log level may have changed since the last frame.
  if (g_ogl_config.bSupportsDebug)
    SyncDebugOutputWithLogLevel();

  m_context->Swap();
}

SurfaceInfo OGLGfx::GetSurfaceInfo() const
{
  return {std::max(m_context->GetBackBufferWidth(), 1u),
          std::max(m_context->GetBackBufferHeight(), 1u), m_surface_scale,
          AbstractTextureFormat::RGBA8};
}

void OGLGfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  if ((bits & CONFIG_CHANGE_BIT_VSYNC) != 0 &&
      !DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VSYNC))
  {
    m_context->SwapInterval(g_ActiveConfig.bVSyncActive);
  }

  // Samplers carry the anisotropy level, so they are rebuilt on demand.
  if ((bits & CONFIG_CHANGE_BIT_ANISOTROPY) != 0)
    g_sampler_cache->Clear();
}

void OGLGfx::Flush()
{
  // Hand queued commands to the GPU now rather than letting the driver batch frames.
  glFlush();
}

void OGLGfx::WaitForGPUIdle()
{
  glFinish();
}

}  // namespace OGL
