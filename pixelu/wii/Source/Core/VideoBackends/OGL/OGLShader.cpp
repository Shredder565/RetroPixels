// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLShader.h"

#include <array>
#include <cctype>
#include <sstream>
#include <string>

#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/ShaderCompileUtils.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
constexpr std::string_view INCLUDE_DIRECTIVE = "#include";

// What follows "#include" on a line: the file name and whether it was written <like_this>.
// A line whose name is cut off ("<name" or "\"name" with no closing mark) is not an include
// we handle. Anything else after the directive is treated as an empty quoted name.
struct IncludeTarget
{
  bool usable = false;
  bool system = false;
  std::string name;
};

IncludeTarget ParseIncludeTarget(std::string_view rest)
{
  IncludeTarget target;
  size_t open = 0;
  while (open < rest.size() && std::isspace(static_cast<unsigned char>(rest[open])))
    ++open;

  const char opener = open < rest.size() ? rest[open] : '\0';
  if (opener != '<' && opener != '"')
  {
    target.usable = true;
    return target;
  }

  const char closer = opener == '<' ? '>' : '"';
  const size_t name_start = open + 1;
  if (name_start >= rest.size())
    return target;
  const size_t close = rest.find(closer, name_start);
  if (close == std::string_view::npos)
    return target;

  target.usable = true;
  target.system = opener == '<';
  target.name = std::string(rest.substr(name_start, close - name_start));
  return target;
}

// GL compiles plain source strings, so #include lines are expanded here with the includer the
// caller gave (if any). Lines the includer cannot resolve are passed through unchanged; every
// line comes out newline-terminated. Each expanded include in a file gets the next depth.
std::string ExpandIncludes(glslang::TShader::Includer* includer, std::string_view source,
                           const char* includer_name = "", size_t depth = 1)
{
  if (includer == nullptr)
    return std::string(source);

  std::string expanded;
  std::istringstream lines{std::string(source)};
  std::string line;
  while (std::getline(lines, line))
  {
    const bool is_include = std::string_view(line).starts_with(INCLUDE_DIRECTIVE);
    const IncludeTarget target =
        is_include ? ParseIncludeTarget(std::string_view(line).substr(INCLUDE_DIRECTIVE.size())) :
                     IncludeTarget{};
    glslang::TShader::Includer::IncludeResult* result = nullptr;
    if (target.usable)
    {
      result = target.system ? includer->includeSystem(target.name.c_str(), includer_name, depth) :
                               includer->includeLocal(target.name.c_str(), includer_name, depth);
    }
    if (result == nullptr)
    {
      expanded += line;
      expanded += '\n';
      continue;
    }

    ++depth;
    expanded += ExpandIncludes(includer, result->headerData, target.name.c_str(), depth);
  }
  return expanded;
}

// GL shader object types, indexed by ShaderStage.
constexpr std::array<GLenum, 4> GL_SHADER_TYPES{GL_VERTEX_SHADER, GL_GEOMETRY_SHADER,
                                                GL_FRAGMENT_SHADER, GL_COMPUTE_SHADER};

GLenum StageToGLType(ShaderStage stage)
{
  const auto index = static_cast<size_t>(stage);
  return index < GL_SHADER_TYPES.size() ? GL_SHADER_TYPES[index] : 0;
}
}  // namespace

OGLShader::OGLShader(ShaderStage stage, GLenum gl_type, GLuint gl_id, std::string source,
                     std::string name)
    : AbstractShader(stage), m_uid(ProgramShaderCache::GenerateShaderID()), m_gl_type(gl_type),
      m_shader(gl_id), m_source(std::move(source)), m_name(std::move(name))
{
  Label(GL_SHADER, m_shader);
}

OGLShader::OGLShader(GLuint gl_compute_program_id, std::string source, std::string name)
    : AbstractShader(ShaderStage::Compute), m_uid(ProgramShaderCache::GenerateShaderID()),
      m_gl_type(GL_COMPUTE_SHADER), m_compute_program(gl_compute_program_id),
      m_source(std::move(source)), m_name(std::move(name))
{
  Label(GL_PROGRAM, m_compute_program);
}

void OGLShader::Label(GLenum identifier, GLuint object) const
{
  if (m_name.empty() || !g_backend_info.bSupportsSettingObjectNames)
    return;
  glObjectLabel(identifier, object, static_cast<GLsizei>(m_name.size()), m_name.c_str());
}

OGLShader::~OGLShader()
{
  if (m_stage == ShaderStage::Compute)
    glDeleteProgram(m_compute_program);
  else
    glDeleteShader(m_shader);
}

std::unique_ptr<OGLShader> OGLShader::CreateFromSource(ShaderStage stage, std::string_view source,
                                                       VideoCommon::ShaderIncluder* shader_includer,
                                                       std::string_view name)
{
  // The expanded text is what GL sees, so compile errors refer to its line numbers rather
  // than to the included files.
  std::string text = ExpandIncludes(shader_includer, source);

  if (stage == ShaderStage::Compute)
  {
    SHADER program;
    if (!ProgramShaderCache::CompileComputeShader(program, text))
      return nullptr;
    return std::make_unique<OGLShader>(program.glprogid, std::move(text), std::string(name));
  }

  const GLenum type = StageToGLType(stage);
  const GLuint shader = ProgramShaderCache::CompileSingleShader(type, text);
  if (shader == 0)
    return nullptr;
  return std::make_unique<OGLShader>(stage, type, shader, std::move(text), std::string(name));
}

}  // namespace OGL
