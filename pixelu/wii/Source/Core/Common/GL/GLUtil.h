// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/GL/GLExtensions/GLExtensions.h"

class GLContext;

// Texture unit used for creating and uploading textures, so that those binds never disturb the
// units the draw state uses.
constexpr GLenum GL_MUTABLE_TEXTURE_INDEX = GL_TEXTURE10;

namespace GLUtil
{
// Compiles and links a vertex + fragment program. Returns the program name; on failure the
// program is still returned (unlinked), and debug builds log the info logs.
GLuint CompileProgram(const std::string& vertexShader, const std::string& fragmentShader);

// Turns on primitive restart with index 0xFFFF, in whichever form the context supports.
void EnablePrimitiveRestart(const GLContext* context);
}  // namespace GLUtil
