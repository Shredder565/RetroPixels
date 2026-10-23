// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Shader.h
//
// Building a GLSL program from a vertex and a fragment shader.

#ifndef INCLUDED_SHADER_H
#define INCLUDED_SHADER_H

#include <GL/glew.h>
#include <string>
#include "Types.h"

// Compiles and links a program. Each shader's source is read from vsFile /
// fsFile when that name is not empty, otherwise taken from vsString /
// fsString. The new handles are written out even if a step fails (so they
// can be destroyed); compile and link errors are logged. On success the
// program is left in use.
extern Result LoadShaderProgram(GLuint *shaderProgramPtr, GLuint *vertexShaderPtr,
                                GLuint *fragmentShaderPtr, const std::string &vsFile,
                                const std::string &fsFile, const char *vsString,
                                const char *fsString);

// Stops using any program and deletes the given program and shaders.
extern void DestroyShaderProgram(GLuint shaderProgram, GLuint vertexShader,
                                 GLuint fragmentShader);

#endif  // INCLUDED_SHADER_H
