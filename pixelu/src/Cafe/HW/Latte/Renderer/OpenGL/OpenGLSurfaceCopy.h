// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/GLInclude/GLInclude.h"

// GL programs used by OpenGLRenderer::surfaceCopy_copySurfaceWithFormatConversion
struct LatteGLDefaultShader_t
{
	GLuint glProgamId;
	struct
	{
		GLint uniformLoc_textureSrc;
		GLint uniformLoc_vertexOffsets; // vec4[4]: clip space xy and source uv per quad corner
	} copyShaderUniforms;
};

// samples the red channel of a texture into colour output 0
LatteGLDefaultShader_t* LatteGLDefaultShader_getPixelCopyShader_depthToColor();
// samples the red channel of a texture into gl_FragDepth
LatteGLDefaultShader_t* LatteGLDefaultShader_getPixelCopyShader_colorToDepth();

// compiles and links a vertex + fragment program, returns 0 if linking failed (LatteShaderGL.cpp)
GLuint OpenGL_LinkRawProgram(const std::string& vertexSource, const std::string& fragmentSource);
