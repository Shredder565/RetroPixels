// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// GLSLShader.h
//
// A vertex + fragment shader program. Uniform and attribute locations can be
// looked up once and kept in the name maps (keys are compared as strings).

#ifndef INCLUDED_NEW3D_GLSLSHADER_H
#define INCLUDED_NEW3D_GLSLSHADER_H

#include <GL/glew.h>
#include <cstring>
#include <map>

class GLSLShader
{
public:
  GLSLShader();
  ~GLSLShader();
  GLSLShader(GLSLShader &&other) noexcept;
  GLSLShader &operator=(GLSLShader &&other) noexcept;
  GLSLShader(const GLSLShader &) = delete;
  GLSLShader &operator=(const GLSLShader &) = delete;

  // Compiles and links; compiler and linker messages are printed.
  bool LoadShaders(const char *vertexShader, const char *fragmentShader);
  void UnloadShaders();
  void EnableShader();
  void DisableShader();

  int GetUniformLocation(const char *name);
  void GetUniformLocationMap(const char *name);    // stores into uniformLocMap[name]
  int GetAttributeLocation(const char *name);
  void GetAttributeLocationMap(const char *name);  // stores into attribLocMap[name]

  struct NameLess
  {
    bool operator()(const char *a, const char *b) const { return std::strcmp(a, b) < 0; }
  };

  int uniformLoc[64];
  int attribLoc[16];
  std::map<const char *, int, NameLess> uniformLocMap;
  std::map<const char *, int, NameLess> attribLocMap;

private:
  void Clear();
  void TakeFrom(GLSLShader &other);

  GLuint m_program = 0;
  GLuint m_vertex = 0;
  GLuint m_fragment = 0;
};

#endif  // INCLUDED_NEW3D_GLSLSHADER_H
