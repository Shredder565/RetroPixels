// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// GLSLShader.cpp

#include "GLSLShader.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
  void PrintLog(GLuint object, bool isProgram)
  {
    GLint length = 0;
    if (isProgram)
      glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
    else
      glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    if (length <= 0)
      return;
    std::string text(size_t(length), '\0');
    if (isProgram)
      glGetProgramInfoLog(object, length, nullptr, &text[0]);
    else
      glGetShaderInfoLog(object, length, nullptr, &text[0]);
    printf("%s\n", text.c_str());
  }
}

GLSLShader::GLSLShader()
{
  Clear();
}

GLSLShader::~GLSLShader()
{
  UnloadShaders();
}

GLSLShader::GLSLShader(GLSLShader &&other) noexcept
{
  Clear();
  TakeFrom(other);
}

GLSLShader &GLSLShader::operator=(GLSLShader &&other) noexcept
{
  if (this != &other)
    TakeFrom(other);
  return *this;
}

void GLSLShader::TakeFrom(GLSLShader &other)
{
  m_program = other.m_program;
  m_vertex = other.m_vertex;
  m_fragment = other.m_fragment;
  uniformLocMap = other.uniformLocMap;
  attribLocMap = other.attribLocMap;
  std::copy(std::begin(other.uniformLoc), std::end(other.uniformLoc), uniformLoc);
  std::copy(std::begin(other.attribLoc), std::end(other.attribLoc), attribLoc);
  other.Clear();
}

void GLSLShader::Clear()
{
  m_program = m_vertex = m_fragment = 0;
  std::fill(std::begin(uniformLoc), std::end(uniformLoc), -1);
  std::fill(std::begin(attribLoc), std::end(attribLoc), -1);
  uniformLocMap.clear();
  attribLocMap.clear();
}

bool GLSLShader::LoadShaders(const char *vertexShader, const char *fragmentShader)
{
  m_program = glCreateProgram();
  m_vertex = glCreateShader(GL_VERTEX_SHADER);
  m_fragment = glCreateShader(GL_FRAGMENT_SHADER);

  glShaderSource(m_vertex, 1, &vertexShader, nullptr);
  glShaderSource(m_fragment, 1, &fragmentShader, nullptr);
  glCompileShader(m_vertex);
  glCompileShader(m_fragment);
  glAttachShader(m_program, m_vertex);
  glAttachShader(m_program, m_fragment);
  glLinkProgram(m_program);

  PrintLog(m_vertex, false);
  PrintLog(m_fragment, false);
  PrintLog(m_program, true);
  return true;
}

void GLSLShader::UnloadShaders()
{
  if (m_program)
  {
    glDeleteShader(m_vertex);
    glDeleteShader(m_fragment);
    glDeleteProgram(m_program);
  }
  m_program = m_vertex = m_fragment = 0;
}

void GLSLShader::EnableShader()
{
  glUseProgram(m_program);
}

void GLSLShader::DisableShader()
{
  glUseProgram(0);
}

int GLSLShader::GetUniformLocation(const char *name)
{
  return glGetUniformLocation(m_program, name);
}

void GLSLShader::GetUniformLocationMap(const char *name)
{
  uniformLocMap[name] = GetUniformLocation(name);
}

int GLSLShader::GetAttributeLocation(const char *name)
{
  return glGetAttribLocation(m_program, name);
}

void GLSLShader::GetAttributeLocationMap(const char *name)
{
  attribLocMap[name] = GetAttributeLocation(name);
}
