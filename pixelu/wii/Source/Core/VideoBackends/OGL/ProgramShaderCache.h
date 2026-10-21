// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/AsyncShaderCompiler.h"

namespace OGL
{
class OGLShader;
class GLVertexFormat;
class StreamBuffer;

// The GL objects of one program: the program itself plus any stage shaders it still owns.
struct SHADER
{
  // Deletes the stage shaders and then the program.
  void Destroy()
  {
    DestroyShaders();
    if (glprogid == 0)
      return;
    glDeleteProgram(glprogid);
    glprogid = 0;
  }

  GLuint vsid = 0;
  GLuint gsid = 0;
  GLuint psid = 0;
  GLuint glprogid = 0;

  // After linking: uniform block and sampler bindings for drivers without layout(binding).
  void SetProgramVariables();
  // Before linking: attribute and fragment output locations for drivers without
  // layout(location).
  void SetProgramBindings(bool is_compute);
  void Bind() const;
  void DestroyShaders();
};

// Programs are shared between pipelines that use the same three shaders.
struct PipelineProgramKey
{
  u64 vertex_shader_id;
  u64 geometry_shader_id;
  u64 pixel_shader_id;

  bool operator==(const PipelineProgramKey& rhs) const;
  bool operator<(const PipelineProgramKey& rhs) const;
};

struct PipelineProgramKeyHash
{
  std::size_t operator()(const PipelineProgramKey& key) const;
};

struct PipelineProgram
{
  PipelineProgramKey key{};
  SHADER shader;
  std::atomic_size_t reference_count{1};
  // Set once the binary has been handed to the pipeline cache (or came from it).
  bool binary_retrieved = false;
};

class ProgramShaderCache
{
public:
  // The backend tracks the bound vertex array object here so redundant binds are skipped.
  static void BindVertexFormat(const GLVertexFormat* vertex_format);
  static void ReBindVertexFormat();
  static bool IsValidVertexFormatBound();
  static void InvalidateVertexFormat();
  static void InvalidateVertexFormatIfBound(GLuint vao);
  static void InvalidateLastProgram();

  static bool CompileComputeShader(SHADER& shader, std::string_view code);
  static GLuint CompileSingleShader(GLenum type, std::string_view code);
  static bool CheckShaderCompileResult(GLuint id, GLenum type, std::string_view code);
  static bool CheckProgramLinkResult(GLuint id, std::string_view vcode, std::string_view pcode,
                                     std::string_view gcode);
  static StreamBuffer* GetUniformBuffer();
  static u32 GetUniformBufferAlignment();
  // Streams the vertex/pixel/geometry constants (when any changed) and binds them.
  static void UploadConstants();
  // Streams one block and binds it for every stage (utility shaders).
  static void UploadConstants(const void* data, u32 data_size);

  static void Init();
  static void Shutdown();
  // Builds the text placed in front of every shader for the current GL capabilities.
  static void CreateHeader();

  // Shader and GL object pointers can be reused once freed, so programs are keyed by an ID
  // that is never handed out twice in a session.
  static u64 GenerateShaderID();

  static PipelineProgram* GetPipelineProgram(const GLVertexFormat* vertex_format,
                                             const OGLShader* vertex_shader,
                                             const OGLShader* geometry_shader,
                                             const OGLShader* pixel_shader, const void* cache_data,
                                             size_t cache_data_size);
  static void ReleasePipelineProgram(PipelineProgram* prog);

private:
  using PipelineProgramMap =
      std::unordered_map<PipelineProgramKey, std::unique_ptr<PipelineProgram>,
                         PipelineProgramKeyHash>;

  static void CreateAttributelessVAO();
  static PipelineProgram* FindAndReference(const PipelineProgramKey& key);
  static bool LinkFromSource(PipelineProgram& prog, const GLVertexFormat* vertex_format,
                             const OGLShader* vertex_shader, const OGLShader* geometry_shader,
                             const OGLShader* pixel_shader);
  static bool LoadFromBinary(PipelineProgram& prog, const void* cache_data, size_t cache_data_size);

  static PipelineProgramMap s_programs;
  static std::mutex s_programs_lock;

  // Bytes of the three standard constant blocks, each padded to the UBO alignment.
  static u32 s_constants_size;
  static s32 s_ubo_align;

  static GLuint s_empty_vbo;
  static GLuint s_empty_vao;
  static GLuint s_bound_vao;
};

// Compiles and links on worker threads, each with its own GL context sharing objects with the
// main one.
class SharedContextAsyncShaderCompiler : public VideoCommon::AsyncShaderCompiler
{
protected:
  bool WorkerThreadInitMainThread(void** param) override;
  bool WorkerThreadInitWorkerThread(void* param) override;
  void WorkerThreadExit(void* param) override;
};

}  // namespace OGL
