// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/ProgramShaderCache.h"

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/GL/GLContext.h"
#include "Common/GL/GLUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Version.h"

#include "Core/ConfigManager.h"
#include "Core/System.h"

#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoBackends/OGL/OGLShader.h"
#include "VideoBackends/OGL/OGLStreamBuffer.h"
#include "VideoBackends/OGL/OGLVertexManager.h"

#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
ProgramShaderCache::PipelineProgramMap ProgramShaderCache::s_programs;
std::mutex ProgramShaderCache::s_programs_lock;
u32 ProgramShaderCache::s_constants_size = 0;
s32 ProgramShaderCache::s_ubo_align = 1;
GLuint ProgramShaderCache::s_empty_vbo = 0;
GLuint ProgramShaderCache::s_empty_vao = 0;
GLuint ProgramShaderCache::s_bound_vao = 0;

namespace
{
// Uniform block binding points, matching the UBO_BINDING numbers the shader generators use.
constexpr GLuint PIXEL_BLOCK_BINDING = 1;
constexpr GLuint VERTEX_BLOCK_BINDING = 2;
constexpr GLuint CUSTOM_BLOCK_BINDING = 3;
constexpr GLuint GEOMETRY_BLOCK_BINDING = 4;
constexpr GLuint UBER_BLOCK_BINDING = 5;

constexpr int SAMPLER_UNITS = 8;
constexpr int TEXCOORD_ATTRIBUTES = 8;

std::unique_ptr<StreamBuffer> s_uniform_stream;
GLuint s_current_program = 0;
std::string s_glsl_header;
std::atomic<u64> s_next_shader_id{0};
int s_bad_shader_count = 0;
// True on the asynchronous compiler's worker threads, which have their own context.
thread_local bool s_is_shared_context = false;

u32 UboAlign()
{
  return static_cast<u32>(ProgramShaderCache::GetUniformBufferAlignment());
}

// The #version line for each GlslVersion, in enum order.
constexpr std::array<std::string_view, 10> VERSION_LINES{
    "#version 130", "#version 140", "#version 150",    "#version 330",    "#version 400",
    "#version 430", "#version 450", "#version 300 es", "#version 310 es", "#version 320 es",
};

std::string VersionDirective(GlslVersion version)
{
  const auto index = static_cast<size_t>(version);
  return std::string(index < VERSION_LINES.size() ? VERSION_LINES[index] : "#version ERROR");
}

const char* StagePrefix(GLenum type)
{
  switch (type)
  {
  case GL_VERTEX_SHADER:
    return "vs";
  case GL_GEOMETRY_SHADER:
    return "gs";
  case GL_FRAGMENT_SHADER:
    return "ps";
  case GL_COMPUTE_SHADER:
    return "cs";
  default:
    return "";
  }
}

// The tail of every bad-shader dump: the build and the backend that produced it.
void WriteDumpFooter(std::ofstream& out)
{
  out << "\n";
  out << "Dolphin Version: " + Common::GetScmRevStr() + "\n";
  out << "Video Backend: " + g_video_backend->GetDisplayName();
}

// Reads an info log of the length GL reported. The string keeps that length even when GL
// writes fewer characters (the terminator included).
template <typename GetLog> std::string ReadInfoLog(GLuint id, GLsizei length, GetLog get_log)
{
  std::string log(static_cast<size_t>(length), '\0');
  get_log(id, length, &length, log.data());
  return log;
}

std::string VersionedExtension(bool wanted, const char* line)
{
  return wanted ? std::string(line) : std::string();
}
// Programs are linked against the current state, so a worker context gets the same fixed state
// the main context runs with (see OGLGfx). The driver is then less likely to recompile a
// program the first time it is used for drawing.
void MirrorMainContextState(GLContext* context)
{
  const auto& info = g_backend_info;
  if (!context->IsGLES())
    glEnable(GL_PROGRAM_POINT_SIZE);
  if (info.bSupportsClipControl)
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
  if (info.bSupportsDepthClamp)
  {
    constexpr std::array<GLenum, 3> CLAMP_CAPS{GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1,
                                               GL_DEPTH_CLAMP};
    for (const GLenum cap : CLAMP_CAPS)
      glEnable(cap);
  }
  if (info.bSupportsPrimitiveRestart)
    GLUtil::EnablePrimitiveRestart(context);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// SHADER

void SHADER::SetProgramVariables()
{
  if (g_backend_info.bSupportsBindingLayout)
    return;

  // Uniforms can only be set on the bound program; the previous binding is put back after.
  glUseProgram(glprogid);

  struct BlockBinding
  {
    const char* name;
    GLuint binding;
  };
  constexpr std::array<BlockBinding, 4> BLOCKS{{
      {"PSBlock", PIXEL_BLOCK_BINDING},
      {"VSBlock", VERTEX_BLOCK_BINDING},
      {"GSBlock", GEOMETRY_BLOCK_BINDING},
      {"UBERBlock", UBER_BLOCK_BINDING},
  }};
  // All four indices are looked up before any binding is made.
  std::array<GLuint, BLOCKS.size()> indices{};
  for (size_t i = 0; i < BLOCKS.size(); ++i)
    indices[i] = glGetUniformBlockIndex(glprogid, BLOCKS[i].name);
  for (size_t i = 0; i < BLOCKS.size(); ++i)
  {
    if (indices[i] != GL_INVALID_INDEX)
      glUniformBlockBinding(glprogid, indices[i], BLOCKS[i].binding);
  }

  // Samplers are declared either as an array or as separate uniforms, depending on the shader.
  for (int unit = 0; unit < SAMPLER_UNITS; ++unit)
  {
    GLint location = glGetUniformLocation(glprogid, fmt::format("samp[{}]", unit).c_str());
    if (location < 0)
      location = glGetUniformLocation(glprogid, fmt::format("samp{}", unit).c_str());
    if (location >= 0)
      glUniform1i(location, unit);
  }

  glUseProgram(s_current_program);
}

void SHADER::SetProgramBindings(bool is_compute)
{
  if (g_ogl_config.bSupportsExplicitLayoutInShader)
    return;

  const auto bind_attrib = [this](ShaderAttrib attrib, const char* name) {
    glBindAttribLocation(glprogid, static_cast<GLuint>(attrib), name);
  };

  if (!is_compute)
  {
    if (g_backend_info.bSupportsDualSourceBlend)
    {
      glBindFragDataLocationIndexed(glprogid, 0, 0, "ocol0");
      glBindFragDataLocationIndexed(glprogid, 0, 1, "ocol1");
    }
    bind_attrib(ShaderAttrib::Position, "rawpos");
    bind_attrib(ShaderAttrib::PositionMatrix, "posmtx");
    bind_attrib(ShaderAttrib::Color0, "rawcolor0");
    bind_attrib(ShaderAttrib::Color1, "rawcolor1");
    bind_attrib(ShaderAttrib::Normal, "rawnormal");
    bind_attrib(ShaderAttrib::Tangent, "rawtangent");
    bind_attrib(ShaderAttrib::Binormal, "rawbinormal");
  }

  // GL copies the name, so the temporary string may go away straight after the call.
  for (int i = 0; i < TEXCOORD_ATTRIBUTES; ++i)
    bind_attrib(ShaderAttrib::TexCoord0 + i, fmt::format("rawtex{}", i).c_str());
}

void SHADER::Bind() const
{
  if (s_current_program == glprogid)
    return;
  INCSTAT(g_stats.this_frame.num_shader_changes);
  glUseProgram(glprogid);
  s_current_program = glprogid;
}

void SHADER::DestroyShaders()
{
  for (GLuint* id : {&vsid, &gsid, &psid})
  {
    if (*id == 0)
      continue;
    glDeleteShader(*id);
    *id = 0;
  }
}

// ---------------------------------------------------------------------------------------------
// Program keys

bool PipelineProgramKey::operator==(const PipelineProgramKey& rhs) const
{
  return vertex_shader_id == rhs.vertex_shader_id && geometry_shader_id == rhs.geometry_shader_id &&
         pixel_shader_id == rhs.pixel_shader_id;
}

bool PipelineProgramKey::operator<(const PipelineProgramKey& rhs) const
{
  const auto as_tuple = [](const PipelineProgramKey& k) {
    return std::make_tuple(k.vertex_shader_id, k.geometry_shader_id, k.pixel_shader_id);
  };
  return as_tuple(*this) < as_tuple(rhs);
}

std::size_t PipelineProgramKeyHash::operator()(const PipelineProgramKey& key) const
{
  // The three hashes are summed.
  const std::hash<u64> h;
  return h(key.vertex_shader_id) + h(key.geometry_shader_id) + h(key.pixel_shader_id);
}

// ---------------------------------------------------------------------------------------------
// Uniforms

StreamBuffer* ProgramShaderCache::GetUniformBuffer()
{
  return s_uniform_stream.get();
}

u32 ProgramShaderCache::GetUniformBufferAlignment()
{
  return static_cast<u32>(s_ubo_align);
}

void ProgramShaderCache::UploadConstants()
{
  auto& system = Core::System::GetInstance();
  auto& pixel = system.GetPixelShaderManager();
  auto& vertex = system.GetVertexShaderManager();
  auto& geometry = system.GetGeometryShaderManager();
  if (!pixel.dirty && !vertex.dirty && !geometry.dirty && !pixel.custom_constants_dirty)
    return;

  // Layout in the stream: pixel block, vertex block, the custom pixel block (if any), then the
  // geometry block, each starting on the UBO alignment.
  const u32 align = UboAlign();
  const size_t custom_bytes = pixel.custom_constants.size();
  const bool has_custom = custom_bytes != 0;
  const u32 custom_span = static_cast<u32>(Common::AlignUp(custom_bytes, align));
  const u32 total = s_constants_size + custom_span;

  const u32 pixel_at = 0;
  const u32 vertex_at =
      pixel_at + static_cast<u32>(Common::AlignUp(sizeof(PixelShaderConstants), align));
  const u32 custom_at =
      vertex_at + static_cast<u32>(Common::AlignUp(sizeof(VertexShaderConstants), align));
  const u32 geometry_at = custom_at + (has_custom ? custom_span : 0u);

  const auto [dst, base] = s_uniform_stream->Map(total, align);
  std::memcpy(dst + pixel_at, &pixel.constants, sizeof(PixelShaderConstants));
  std::memcpy(dst + vertex_at, &vertex.constants, sizeof(VertexShaderConstants));
  if (has_custom)
    std::memcpy(dst + custom_at, pixel.custom_constants.data(), custom_bytes);
  std::memcpy(dst + geometry_at, &geometry.constants, sizeof(GeometryShaderConstants));
  s_uniform_stream->Unmap(total);

  const GLuint buffer = s_uniform_stream->GetGLBufferId();
  const auto bind_range = [buffer, base](GLuint index, u32 offset, size_t size) {
    glBindBufferRange(GL_UNIFORM_BUFFER, index, buffer, static_cast<GLintptr>(base + offset),
                      static_cast<GLsizeiptr>(size));
  };
  bind_range(PIXEL_BLOCK_BINDING, pixel_at, sizeof(PixelShaderConstants));
  bind_range(VERTEX_BLOCK_BINDING, vertex_at, sizeof(VertexShaderConstants));
  if (has_custom)
    bind_range(CUSTOM_BLOCK_BINDING, custom_at, custom_bytes);
  bind_range(GEOMETRY_BLOCK_BINDING, geometry_at, sizeof(GeometryShaderConstants));

  pixel.dirty = false;
  vertex.dirty = false;
  geometry.dirty = false;
  pixel.custom_constants_dirty = false;

  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, total);
}

void ProgramShaderCache::UploadConstants(const void* data, u32 data_size)
{
  const u32 align = UboAlign();
  const u32 reserved = Common::AlignUp(data_size, align);
  const auto [dst, base] = s_uniform_stream->Map(reserved, align);
  std::memcpy(dst, data, data_size);
  s_uniform_stream->Unmap(reserved);

  // Utility shaders read the one block through whichever binding their stage declares.
  const GLuint buffer = s_uniform_stream->GetGLBufferId();
  for (GLuint index = PIXEL_BLOCK_BINDING; index <= GEOMETRY_BLOCK_BINDING; ++index)
  {
    glBindBufferRange(GL_UNIFORM_BUFFER, index, buffer, static_cast<GLintptr>(base),
                      static_cast<GLsizeiptr>(data_size));
  }

  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
}

// ---------------------------------------------------------------------------------------------
// Compiling and linking

bool ProgramShaderCache::CompileComputeShader(SHADER& shader, std::string_view code)
{
  // Drivers that offer compute through the extension on an older GLSL (Mesa, for one) need
  // it switched on in the shader.
  const bool needs_extension =
      g_backend_info.bSupportsComputeShaders && g_ogl_config.eSupportedGLSLVersion < Glsl430;
  std::string source = needs_extension ? "#extension GL_ARB_compute_shader : enable\n" : "";
  source.append(code);

  const GLuint stage = CompileSingleShader(GL_COMPUTE_SHADER, source);
  if (stage == 0)
    return false;

  shader.glprogid = glCreateProgram();
  glAttachShader(shader.glprogid, stage);
  shader.SetProgramBindings(true);
  glLinkProgram(shader.glprogid);
  // The program keeps what it needs; the stage object can go now.
  glDeleteShader(stage);

  if (!CheckProgramLinkResult(shader.glprogid, source, {}, {}))
  {
    shader.Destroy();
    return false;
  }
  shader.SetProgramVariables();
  return true;
}

GLuint ProgramShaderCache::CompileSingleShader(GLenum type, std::string_view code)
{
  const GLuint id = glCreateShader(type);

  // The common header goes in as a separate string ahead of the shader's own text.
  const std::array<const GLchar*, 2> strings{s_glsl_header.data(), code.data()};
  const std::array<GLint, 2> lengths{static_cast<GLint>(s_glsl_header.size()),
                                     static_cast<GLint>(code.size())};
  glShaderSource(id, static_cast<GLsizei>(strings.size()), strings.data(), lengths.data());
  glCompileShader(id);

  if (CheckShaderCompileResult(id, type, code))
    return id;
  glDeleteShader(id);
  return 0;
}

bool ProgramShaderCache::CheckShaderCompileResult(GLuint id, GLenum type, std::string_view code)
{
  GLint status = GL_FALSE;
  glGetShaderiv(id, GL_COMPILE_STATUS, &status);
  GLsizei log_length = 0;
  glGetShaderiv(id, GL_INFO_LOG_LENGTH, &log_length);

  const bool failed = status != GL_TRUE;
  // A one-character log is just the terminator: nothing to report.
  if (!failed && log_length <= 1)
    return true;

  const std::string log = ReadInfoLog(id, log_length, glGetShaderInfoLog);
  const char* const stage = StagePrefix(type);
  if (!failed)
  {
    WARN_LOG_FMT(VIDEO, "{} compiled with warnings:\n{}", stage, log);
    return true;
  }

  ERROR_LOG_FMT(VIDEO, "{} failed compilation:\n{}", stage, log);
  const std::string dump_path = VideoBackendBase::BadShaderFilename(stage, s_bad_shader_count++);
  std::ofstream dump;
  File::OpenFStream(dump, dump_path, std::ios_base::out);
  dump << s_glsl_header << code << log;
  WriteDumpFooter(dump);
  dump.close();

  PanicAlertFmt("Failed to compile {} shader: {}\n"
                "Debug info ({}, {}, {}):\n{}",
                stage, dump_path, g_ogl_config.gl_vendor, g_ogl_config.gl_renderer,
                g_ogl_config.gl_version, log);
  return false;
}

bool ProgramShaderCache::CheckProgramLinkResult(GLuint id, std::string_view vcode,
                                                std::string_view pcode, std::string_view gcode)
{
  GLint status = GL_FALSE;
  glGetProgramiv(id, GL_LINK_STATUS, &status);
  GLsizei log_length = 0;
  glGetProgramiv(id, GL_INFO_LOG_LENGTH, &log_length);

  const bool failed = status != GL_TRUE;
  if (!failed && log_length <= 1)
    return true;

  const std::string log = ReadInfoLog(id, log_length, glGetProgramInfoLog);
  if (!failed)
  {
    WARN_LOG_FMT(VIDEO, "Program linked with warnings:\n{}", log);
    return true;
  }

  ERROR_LOG_FMT(VIDEO, "Program failed linking:\n{}", log);
  const std::string dump_path = VideoBackendBase::BadShaderFilename("p", s_bad_shader_count++);
  std::ofstream dump;
  File::OpenFStream(dump, dump_path, std::ios_base::out);
  // Stages go into the dump in the order vcode, gcode, pcode.
  for (const std::string_view stage_code : {vcode, gcode, pcode})
  {
    if (!stage_code.empty())
      dump << s_glsl_header << stage_code << '\n';
  }
  dump << log;
  WriteDumpFooter(dump);
  dump.close();

  PanicAlertFmt("Failed to link shaders: {}\n"
                "Debug info ({}, {}, {}):\n{}",
                dump_path, g_ogl_config.gl_vendor, g_ogl_config.gl_renderer,
                g_ogl_config.gl_version, log);
  return false;
}

// ---------------------------------------------------------------------------------------------
// Lifetime

void ProgramShaderCache::Init()
{
  // Every block offset in the uniform stream must be a multiple of this.
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s_ubo_align);
  const u32 align = UboAlign();
  s_constants_size = static_cast<u32>(Common::AlignUp(sizeof(PixelShaderConstants), align) +
                                      Common::AlignUp(sizeof(VertexShaderConstants), align) +
                                      Common::AlignUp(sizeof(GeometryShaderConstants), align));

  s_uniform_stream =
      StreamBuffer::Create(GL_UNIFORM_BUFFER, VertexManagerBase::UNIFORM_STREAM_BUFFER_SIZE);

  CreateHeader();
  CreateAttributelessVAO();
  s_current_program = 0;
}

void ProgramShaderCache::Shutdown()
{
  s_uniform_stream.reset();

  glBindVertexArray(0);
  glDeleteBuffers(1, &s_empty_vbo);
  glDeleteVertexArrays(1, &s_empty_vao);
  s_empty_vbo = 0;
  s_empty_vao = 0;
  s_bound_vao = 0;

  // Every pipeline should have released its program by now.
  DEBUG_ASSERT(s_programs.empty());
  s_programs.clear();
}

void ProgramShaderCache::CreateAttributelessVAO()
{
  // Draws without vertex input still use a VAO, and compatibility contexts also insist on a
  // bound array buffer and an attribute 0. The buffer is a small, never-read placeholder.
  glGenVertexArrays(1, &s_empty_vao);
  glGenBuffers(1, &s_empty_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, s_empty_vbo);
  glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(GLfloat), nullptr, GL_STATIC_DRAW);
  glBindVertexArray(s_empty_vao);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(0);
}

// ---------------------------------------------------------------------------------------------
// Vertex array tracking

void ProgramShaderCache::BindVertexFormat(const GLVertexFormat* vertex_format)
{
  const GLuint wanted = vertex_format != nullptr ? vertex_format->GetVAO() : s_empty_vao;
  if (wanted == s_bound_vao)
    return;
  glBindVertexArray(wanted);
  s_bound_vao = wanted;
}

void ProgramShaderCache::ReBindVertexFormat()
{
  if (s_bound_vao != 0)
    glBindVertexArray(s_bound_vao);
}

bool ProgramShaderCache::IsValidVertexFormatBound()
{
  return s_bound_vao != 0 && s_bound_vao != s_empty_vao;
}

void ProgramShaderCache::InvalidateVertexFormat()
{
  s_bound_vao = 0;
}

void ProgramShaderCache::InvalidateVertexFormatIfBound(GLuint vao)
{
  if (vao == s_bound_vao)
    s_bound_vao = 0;
}

void ProgramShaderCache::InvalidateLastProgram()
{
  s_current_program = 0;
}

// ---------------------------------------------------------------------------------------------
// Pipeline programs

PipelineProgram* ProgramShaderCache::FindAndReference(const PipelineProgramKey& key)
{
  // Caller holds s_programs_lock.
  const auto found = s_programs.find(key);
  if (found == s_programs.end())
    return nullptr;
  found->second->reference_count++;
  return found->second.get();
}

bool ProgramShaderCache::LoadFromBinary(PipelineProgram& prog, const void* cache_data,
                                        size_t cache_data_size)
{
  // Cache entry: u32 binary format, then the binary itself.
  u32 format = 0;
  std::memcpy(&format, cache_data, sizeof(format));
  glProgramBinary(prog.shader.glprogid, static_cast<GLenum>(format),
                  static_cast<const u8*>(cache_data) + sizeof(format),
                  static_cast<GLsizei>(cache_data_size - sizeof(format)));

  GLint status = GL_FALSE;
  glGetProgramiv(prog.shader.glprogid, GL_LINK_STATUS, &status);
  if (status != GL_TRUE)
  {
    // Returning failure stops the shader cache handing out the same stale binary again.
    WARN_LOG_FMT(VIDEO, "Failed to create GL program from program binary.");
    return false;
  }

  // It came from the cache, so it must not be written back to it (see OGLPipeline).
  prog.binary_retrieved = true;
  return true;
}

bool ProgramShaderCache::LinkFromSource(PipelineProgram& prog, const GLVertexFormat* vertex_format,
                                        const OGLShader* vertex_shader,
                                        const OGLShader* geometry_shader,
                                        const OGLShader* pixel_shader)
{
  const GLuint program = prog.shader.glprogid;

  // Linking with the pipeline's own VAO bound saves NVIDIA's driver a recompile on first draw.
  // A worker context has no tracked binding, so it always binds.
  const GLuint vao = vertex_format != nullptr ? vertex_format->GetVAO() : s_empty_vao;
  const bool swap_vao = vao != s_bound_vao;
  if (s_is_shared_context || swap_vao)
    glBindVertexArray(vao);

  ASSERT(vertex_shader && vertex_shader->GetStage() == ShaderStage::Vertex);
  ASSERT(pixel_shader && pixel_shader->GetStage() == ShaderStage::Pixel);
  glAttachShader(program, vertex_shader->GetGLShaderID());
  glAttachShader(program, pixel_shader->GetGLShaderID());
  if (geometry_shader != nullptr)
  {
    ASSERT(geometry_shader->GetStage() == ShaderStage::Geometry);
    glAttachShader(program, geometry_shader->GetGLShaderID());
  }

  if (g_backend_info.bSupportsPipelineCacheData)
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

  prog.shader.SetProgramBindings(false);
  glLinkProgram(program);

  if (!s_is_shared_context && swap_vao)
    glBindVertexArray(s_bound_vao);

  const auto source_of = [](const OGLShader* shader) {
    return shader != nullptr ? std::string_view(shader->GetSource()) : std::string_view{};
  };
  // Argument order is vertex, geometry, pixel into (vcode, pcode, gcode).
  return CheckProgramLinkResult(program, source_of(vertex_shader), source_of(geometry_shader),
                                source_of(pixel_shader));
}

PipelineProgram* ProgramShaderCache::GetPipelineProgram(const GLVertexFormat* vertex_format,
                                                        const OGLShader* vertex_shader,
                                                        const OGLShader* geometry_shader,
                                                        const OGLShader* pixel_shader,
                                                        const void* cache_data,
                                                        size_t cache_data_size)
{
  const auto id_of = [](const OGLShader* shader) -> u64 {
    return shader != nullptr ? shader->GetID() : 0;
  };
  const PipelineProgramKey key{id_of(vertex_shader), id_of(geometry_shader), id_of(pixel_shader)};

  {
    std::lock_guard lock{s_programs_lock};
    if (PipelineProgram* existing = FindAndReference(key))
      return existing;
  }

  // Built without the lock held, so other threads can look up or build other programs.
  auto prog = std::make_unique<PipelineProgram>();
  prog->key = key;
  prog->shader.glprogid = glCreateProgram();

  const bool built =
      cache_data_size >= sizeof(u32) ?
          LoadFromBinary(*prog, cache_data, cache_data_size) :
          LinkFromSource(*prog, vertex_format, vertex_shader, geometry_shader, pixel_shader);
  if (!built)
  {
    prog->shader.Destroy();
    return nullptr;
  }

  std::lock_guard lock{s_programs_lock};
  // Another thread may have finished the same program first; theirs wins.
  if (PipelineProgram* existing = FindAndReference(key))
  {
    prog->shader.Destroy();
    return existing;
  }

  prog->shader.SetProgramVariables();

  // Some drivers (AMD among them) can hang if a program from a worker context is used on the
  // main one before the worker's commands have completed.
  if (s_is_shared_context)
    glFinish();

  PipelineProgram* const result = prog.get();
  s_programs.emplace(key, std::move(prog));
  return result;
}

void ProgramShaderCache::ReleasePipelineProgram(PipelineProgram* prog)
{
  if (--prog->reference_count > 0)
    return;

  prog->shader.Destroy();

  std::lock_guard lock{s_programs_lock};
  const auto found = s_programs.find(prog->key);
  ASSERT(found != s_programs.end() && prog == found->second.get());
  s_programs.erase(found);
}

u64 ProgramShaderCache::GenerateShaderID()
{
  return s_next_shader_id++;
}

// ---------------------------------------------------------------------------------------------
// Common shader header

void ProgramShaderCache::CreateHeader()
{
  const GlslVersion version = g_ogl_config.eSupportedGLSLVersion;
  const bool es = version >= GlslEs300;
  const auto& info = g_backend_info;
  const auto& gl = g_ogl_config;

  std::string point_size;
  if (gl.SupportedESPointSize == EsPointSizeType::PointSizeOes)
    point_size = "#extension GL_OES_geometry_point_size : enable";
  else if (gl.SupportedESPointSize == EsPointSizeType::PointSizeExt)
    point_size = "#extension GL_EXT_geometry_point_size : enable";

  std::string es_texture_buffer;
  if (gl.SupportedESTextureBuffer == EsTexbufType::TexbufExt)
    es_texture_buffer = "#extension GL_EXT_texture_buffer : enable";
  else if (gl.SupportedESTextureBuffer == EsTexbufType::TexbufOes)
    es_texture_buffer = "#extension GL_OES_texture_buffer : enable";

  // FORCE_EARLY_Z: the real thing with image load/store, otherwise the conservative-depth
  // stand-in that PixelShaderGen knows how to use.
  std::string early_z;
  if (info.bSupportsEarlyZ && gl.bSupportsImageLoadStore)
  {
    early_z = "#define FORCE_EARLY_Z layout(early_fragment_tests) in\n";
  }
  else if (info.bSupportsEarlyZ && gl.bSupportsConservativeDepth)
  {
    early_z = "#define FORCE_EARLY_Z layout(depth_unchanged) out float gl_FragDepth\n"
              "#extension GL_ARB_conservative_depth : enable\n";
  }

  std::string fb_fetch;
  if (gl.SupportedFramebufferFetch == EsFbFetchType::FbFetchExt)
  {
    fb_fetch = "#extension GL_EXT_shader_framebuffer_fetch: enable\n"
               "#define FRAGMENT_INOUT inout";
  }
  else if (gl.SupportedFramebufferFetch == EsFbFetchType::FbFetchArm)
  {
    fb_fetch = "#extension GL_ARM_shader_framebuffer_fetch: enable\n"
               "#define FB_FETCH_VALUE gl_LastFragColorARM\n"
               "#define FRAGMENT_INOUT out";
  }

  // Binding macros. Locations are only spelled out when the shader may declare them; bindings
  // need binding-layout support.
  const bool explicit_locations = info.bSupportsBindingLayout && gl.bSupportsExplicitLayoutInShader;
  std::string bindings;
  if (explicit_locations)
  {
    bindings = "#extension GL_ARB_explicit_attrib_location : enable\n"
               "#define ATTRIBUTE_LOCATION(x) layout(location = x)\n"
               "#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)\n"
               "#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)\n";
  }
  else
  {
    bindings = "#define ATTRIBUTE_LOCATION(x)\n"
               "#define FRAGMENT_OUTPUT_LOCATION(x)\n"
               "#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y)\n";
  }
  if (info.bSupportsBindingLayout)
  {
    bindings += "#define UBO_BINDING(packing, x) layout(packing, binding = x)\n"
                "#define SAMPLER_BINDING(x) layout(binding = x)\n"
                "#define TEXEL_BUFFER_BINDING(x) layout(binding = x)\n"
                "#define SSBO_BINDING(x) layout(std430, binding = x)\n"
                "#define IMAGE_BINDING(format, x) layout(format, binding = x)\n";
  }
  else
  {
    bindings += "#define UBO_BINDING(packing, x) layout(packing)\n"
                "#define SAMPLER_BINDING(x)\n"
                "#define TEXEL_BUFFER_BINDING(x)\n"
                "#define SSBO_BINDING(x) layout(std430)\n"
                "#define IMAGE_BINDING(format, x) layout(format)\n";
  }

  std::string subgroups;
  if (gl.bSupportsKHRShaderSubgroup)
  {
    subgroups = "\n"
                "#extension GL_KHR_shader_subgroup_basic : enable\n"
                "#extension GL_KHR_shader_subgroup_arithmetic : enable\n"
                "#extension GL_KHR_shader_subgroup_ballot : enable\n"
                "\n"
                "#define SUPPORTS_SUBGROUP_REDUCTION 1\n"
                "#define IS_HELPER_INVOCATION gl_HelperInvocation\n"
                "#define IS_FIRST_ACTIVE_INVOCATION (subgroupElect())\n"
                "#define SUBGROUP_MIN(value) value = subgroupMin(value)\n"
                "#define SUBGROUP_MAX(value) value = subgroupMax(value)\n";
  }

  // sampler2DMSArray is reserved in GLSL ES 3.0/3.1 unless the OES extension is enabled; 3.2
  // has it built in. Desktop ARB_texture_storage_multisample needs no #extension.
  const bool ms_array_precision =
      version >= GlslEs320 ||
      gl.SupportedMultisampleTexStorage != MultisampleTexStorageType::TexStorageNone;
  const bool image_ext_needed =
      gl.bSupportsImageLoadStore && (es ? version < GlslEs310 : version < Glsl430);

  // One entry per line of the header, in order; empty entries still produce their line.
  const std::array<std::string, 27> lines{
      VersionDirective(version),
      VersionedExtension(version < Glsl140, "#extension GL_ARB_uniform_buffer_object : enable"),
      early_z,
      VersionedExtension(info.bSupportsBindingLayout && version < GlslEs310,
                         "#extension GL_ARB_shading_language_420pack : enable"),
      VersionedExtension(gl.bSupportsMSAA && version < Glsl150,
                         "#extension GL_ARB_texture_multisample : enable"),
      bindings,
      // Varying locations are not used yet, even with explicit layout support.
      "#define VARYING_LOCATION(x)\n",
      VersionedExtension(!es && info.bSupportsFragmentStoresAndAtomics,
                         "#extension GL_ARB_shader_storage_buffer_object : enable"),
      VersionedExtension(version < Glsl400 && info.bSupportsGSInstancing,
                         "#extension GL_ARB_gpu_shader5 : enable"),
      VersionedExtension(version < Glsl400 && info.bSupportsSSAA,
                         "#extension GL_ARB_sample_shading : enable"),
      point_size,
      VersionedExtension(gl.bSupportsAEP, "#extension GL_ANDROID_extension_pack_es31a : enable"),
      VersionedExtension(version < Glsl140 && info.bSupportsPaletteConversion,
                         "#extension GL_ARB_texture_buffer_object : enable"),
      es_texture_buffer,
      VersionedExtension(es && info.bSupportsDualSourceBlend,
                         "#extension GL_EXT_blend_func_extended : enable"),
      VersionedExtension(image_ext_needed, "#extension GL_ARB_shader_image_load_store : enable"),
      fb_fetch,
      subgroups,
      VersionedExtension(info.bSupportsCoarseDerivatives,
                         "#extension GL_ARB_derivative_control : enable"),
      VersionedExtension(info.bSupportsTextureQueryLevels,
                         "#extension GL_ARB_texture_query_levels : enable"),
      VersionedExtension(gl.SupportedMultisampleTexStorage ==
                             MultisampleTexStorageType::TexStorageOes,
                         "#extension GL_OES_texture_storage_multisample_2d_array : enable"),
      VersionedExtension(es, "precision highp float;"),
      VersionedExtension(es, "precision highp int;"),
      VersionedExtension(es, "precision highp sampler2DArray;"),
      VersionedExtension(es && info.bSupportsPaletteConversion, "precision highp usamplerBuffer;"),
      VersionedExtension(ms_array_precision, "precision highp sampler2DMSArray;"),
      VersionedExtension(version >= GlslEs310, "precision highp image2DArray;"),
  };

  std::string header;
  for (const std::string& line : lines)
  {
    header += line;
    header += '\n';
  }
  // The shared generators write HLSL-style type and function names; map them to GLSL.
  header += "#define API_OPENGL 1\n";
  constexpr std::array<std::pair<const char*, const char*>, 11> ALIASES{{
      {"float2", "vec2"},
      {"float3", "vec3"},
      {"float4", "vec4"},
      {"uint2", "uvec2"},
      {"uint3", "uvec3"},
      {"uint4", "uvec4"},
      {"int2", "ivec2"},
      {"int3", "ivec3"},
      {"int4", "ivec4"},
      {"frac", "fract"},
      {"lerp", "mix"},
  }};
  for (const auto& [hlsl, glsl] : ALIASES)
    header += fmt::format("#define {} {}\n", hlsl, glsl);
  s_glsl_header = std::move(header);
}

// ---------------------------------------------------------------------------------------------
// Asynchronous compiling

bool SharedContextAsyncShaderCompiler::WorkerThreadInitMainThread(void** param)
{
  // Shared contexts must be created on the thread that owns the main context. The worker
  // takes ownership of the raw pointer and deletes it in WorkerThreadExit.
  GLContext* const worker_context =
      GetOGLGfx()->GetMainGLContext()->CreateSharedContext().release();
  *param = worker_context;
  if (worker_context != nullptr)
    return true;
  PanicAlertFmt("Failed to create shared context for shader compiling.");
  return false;
}

bool SharedContextAsyncShaderCompiler::WorkerThreadInitWorkerThread(void* param)
{
  auto* const worker_context = static_cast<GLContext*>(param);
  if (!worker_context->MakeCurrent())
    return false;
  s_is_shared_context = true;
  MirrorMainContextState(worker_context);
  return true;
}

void SharedContextAsyncShaderCompiler::WorkerThreadExit(void* param)
{
  auto* const worker_context = static_cast<GLContext*>(param);
  worker_context->ClearCurrent();
  delete worker_context;
}
}  // namespace OGL
