// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLUtil.h"

#include "Common/GL/GLContext.h"
#include "Common/Logging/Log.h"

namespace GLUtil
{
namespace
{
#if defined(_DEBUG) || defined(DEBUGFAST)
#define GLUTIL_CHECK_BUILD_LOGS 1
constexpr GLsizei BUILD_LOG_CAPACITY = 1024;
#endif

// Feeds one source string to a shader object and compiles it. In debug builds the compile log
// is reported; the return value is false only when the debug check saw a failed compile.
bool CompileStage(GLuint shader, const std::string& text, const char* stage_name)
{
  const GLchar* text_ptr = text.c_str();
  glShaderSource(shader, 1, &text_ptr, nullptr);
  glCompileShader(shader);

#ifndef GLUTIL_CHECK_BUILD_LOGS
  (void)stage_name;
  return true;
#else
  GLint compiled = GL_FALSE;
  GLsizei log_length = 0;
  char log[BUILD_LOG_CAPACITY];
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  glGetShaderInfoLog(shader, BUILD_LOG_CAPACITY, &log_length, log);

  if (!compiled)
    ERROR_LOG_FMT(VIDEO, "GLSL {} shader error:\n{}{}", stage_name, log, text);
  else if (log_length != 0)
    ERROR_LOG_FMT(VIDEO, "GLSL {} shader warnings:\n{}{}", stage_name, log, text);
  else
    INFO_LOG_FMT(VIDEO, "GLSL {} shader compiled:\n{}", stage_name, text);

  return compiled != GL_FALSE;
#endif
}

// Debug-build report of the link log. A link error is only reported when both stages compiled,
// since a stage error already explains it.
void ReportLink(GLuint program, bool stages_ok, const std::string& vertex_text,
                const std::string& fragment_text)
{
#ifndef GLUTIL_CHECK_BUILD_LOGS
  (void)program;
  (void)stages_ok;
  (void)vertex_text;
  (void)fragment_text;
#else
  GLint linked = GL_FALSE;
  GLsizei log_length = 0;
  char log[BUILD_LOG_CAPACITY];
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  glGetProgramInfoLog(program, BUILD_LOG_CAPACITY, &log_length, log);

  if (linked && log_length != 0)
    ERROR_LOG_FMT(VIDEO, "GLSL linker warnings:\n{}{}{}", log, vertex_text, fragment_text);
  else if (!linked && stages_ok)
    ERROR_LOG_FMT(VIDEO, "GLSL linker error:\n{}{}{}", log, vertex_text, fragment_text);
#endif
}
}  // namespace

GLuint CompileProgram(const std::string& vertexShader, const std::string& fragmentShader)
{
  // All three objects are created before anything is compiled.
  const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  const GLuint program = glCreateProgram();

  const bool vs_ok = CompileStage(vs, vertexShader, "vertex");
  const bool fs_ok = CompileStage(fs, fragmentShader, "fragment");

  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  ReportLink(program, vs_ok && fs_ok, vertexShader, fragmentShader);

  // The linked program keeps what it needs; the shader objects can go.
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

void EnablePrimitiveRestart(const GLContext* context)
{
  constexpr GLuint RESTART_INDEX = 0xFFFF;

  if (context->IsGLES())
  {
    // ES 3.0 only has the fixed-index form, which for 16-bit indices is 0xFFFF.
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    return;
  }

  if (GLExtensions::Version() < 310)
  {
    // Before GL 3.1 the feature comes from NV_primitive_restart, as client state.
    glEnableClientState(GL_PRIMITIVE_RESTART_NV);
    glPrimitiveRestartIndexNV(RESTART_INDEX);
    return;
  }

  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(RESTART_INDEX);
}
}  // namespace GLUtil
