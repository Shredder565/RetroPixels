// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Shader.cpp

#include "Shader.h"
#include "Supermodel.h"

#include <fstream>
#include <iterator>

namespace
{
  bool ReadSource(const std::string &path, std::string &out)
  {
    std::ifstream in(path);
    if (!in)
    {
      ErrorLog("Unable to open shader source file: %s", path.c_str());
      return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
  }

  // Compiles one shader; logs and returns false on failure.
  bool Compile(GLuint shader, const char *source, const char *kind)
  {
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok)
      return true;
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    ErrorLog("%s shader failed to compile. Your OpenGL driver said:\n%s", kind, log);
    return false;
  }
}

Result LoadShaderProgram(GLuint *shaderProgramPtr, GLuint *vertexShaderPtr, GLuint *fragmentShaderPtr, const std::string &vsFile, const std::string &fsFile, const char *vsString, const char *fsString)
{
  std::string vsText, fsText;
  if (!vsFile.empty())
  {
    if (!ReadSource(vsFile, vsText))
      return Result::FAIL;
    vsString = vsText.c_str();
  }
  if (!fsFile.empty())
  {
    if (!ReadSource(fsFile, fsText))
      return Result::FAIL;
    fsString = fsText.c_str();
  }
  if (vsString == nullptr || fsString == nullptr)
    return Result::FAIL;

  if (!glCreateProgram || !glCreateShader || !glShaderSource || !glCompileShader)
  {
    ErrorLog("OpenGL 2.x does not appear to be present. Unable to proceed.");
    return Result::FAIL;
  }

  GLuint program = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  *shaderProgramPtr = program;
  *vertexShaderPtr = vs;
  *fragmentShaderPtr = fs;

  bool ok = Compile(vs, vsString, "Vertex");
  ok = Compile(fs, fsString, "Fragment") && ok;

  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked)
  {
    char log[2048] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    ErrorLog("Failed to link shader objects. Your OpenGL driver said:\n%s\n", log);
    ok = false;
  }

  if (!ok)
    return Result::FAIL;
  glUseProgram(program);
  return Result::OKAY;
}

void DestroyShaderProgram(GLuint shaderProgram, GLuint vertexShader, GLuint fragmentShader)
{
  if (!glUseProgram || !glDeleteShader || !glDeleteProgram)
    return;   // no shader support was loaded
  glUseProgram(0);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);
  glDeleteProgram(shaderProgram);
}
