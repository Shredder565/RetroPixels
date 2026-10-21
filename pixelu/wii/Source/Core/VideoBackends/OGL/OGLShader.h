// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/AbstractShader.h"

namespace OGL
{
// A compiled shader stage. Vertex, geometry and pixel stages are GL shader objects that the
// program cache links into programs; a compute shader is a complete program by itself.
class OGLShader final : public AbstractShader
{
public:
  explicit OGLShader(ShaderStage stage, GLenum gl_type, GLuint gl_id, std::string source,
                     std::string name);
  explicit OGLShader(GLuint gl_compute_program_id, std::string source, std::string name);
  ~OGLShader() override;

  // Unique for the session; programs are keyed by the IDs of their stages.
  u64 GetID() const { return m_uid; }
  GLenum GetGLShaderType() const { return m_gl_type; }
  GLuint GetGLShaderID() const { return m_shader; }
  GLuint GetGLComputeProgramID() const { return m_compute_program; }
  const std::string& GetSource() const { return m_source; }

  static std::unique_ptr<OGLShader> CreateFromSource(ShaderStage stage, std::string_view source,
                                                     VideoCommon::ShaderIncluder* shader_includer,
                                                     std::string_view name);

private:
  void Label(GLenum identifier, GLuint object) const;

  u64 m_uid;
  GLenum m_gl_type;
  GLuint m_shader = 0;
  GLuint m_compute_program = 0;
  std::string m_source;
  std::string m_name;
};

}  // namespace OGL
