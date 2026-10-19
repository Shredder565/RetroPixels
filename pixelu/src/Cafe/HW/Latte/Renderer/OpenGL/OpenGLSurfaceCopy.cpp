// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/CachedFBOGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLSurfaceCopy.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"

void LatteDraw_resetAttributePointerCache();

namespace
{
	// the quad corners come from a uniform array indexed by gl_VertexID, so no vertex buffer is needed
	const char* const kCopyVertexShader =
		"#version 420\n"
		"out vec2 passUV;\n"
		"uniform vec4 uf_vertexOffsets[4];\n"
		"void main()\n"
		"{\n"
		"	vec4 corner = uf_vertexOffsets[gl_VertexID];\n"
		"	passUV = corner.zw;\n"
		"	gl_Position = vec4(corner.xy, 0.0, 1.0);\n"
		"}\n";

	const char* const kCopyToColorFragmentShader =
		"#version 420\n"
		"in vec2 passUV;\n"
		"uniform sampler2D textureSrc;\n"
		"layout(location = 0) out vec4 colorOut0;\n"
		"void main()\n"
		"{\n"
		"	colorOut0 = vec4(texture(textureSrc, passUV).r, 0.0, 0.0, 1.0);\n"
		"}\n";

	const char* const kCopyToDepthFragmentShader =
		"#version 420\n"
		"in vec2 passUV;\n"
		"uniform sampler2D textureSrc;\n"
		"layout(location = 0) out vec4 colorOut0;\n"
		"void main()\n"
		"{\n"
		"	gl_FragDepth = texture(textureSrc, passUV).r;\n"
		"}\n";

	LatteGLDefaultShader_t* s_copyToColor = nullptr;
	LatteGLDefaultShader_t* s_copyToDepth = nullptr;

	LatteGLDefaultShader_t* BuildCopyProgram(const char* fragmentSource)
	{
		catchOpenGLError();
		auto* prog = new LatteGLDefaultShader_t{};
		prog->glProgamId = OpenGL_LinkRawProgram(kCopyVertexShader, fragmentSource);
		catchOpenGLError();
		prog->copyShaderUniforms.uniformLoc_textureSrc = glGetUniformLocation(prog->glProgamId, "textureSrc");
		prog->copyShaderUniforms.uniformLoc_vertexOffsets = glGetUniformLocation(prog->glProgamId, "uf_vertexOffsets");
		catchOpenGLError();
		return prog;
	}

	void DisableDepthCompare(LatteTextureViewGL* view)
	{
		// sampling a depth texture as a value needs comparison mode off; the view must be bound to the active unit
		if (view->samplerState.depthCompareMode == 0)
			return;
		glTexParameteri(view->glTexTarget, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		view->samplerState.depthCompareMode = 0;
	}
}

LatteGLDefaultShader_t* LatteGLDefaultShader_getPixelCopyShader_depthToColor()
{
	if (!s_copyToColor)
		s_copyToColor = BuildCopyProgram(kCopyToColorFragmentShader);
	return s_copyToColor;
}

LatteGLDefaultShader_t* LatteGLDefaultShader_getPixelCopyShader_colorToDepth()
{
	if (!s_copyToDepth)
		s_copyToDepth = BuildCopyProgram(kCopyToDepthFragmentShader);
	return s_copyToDepth;
}

void OpenGLRenderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	sint32 targetWidth = width;
	sint32 targetHeight = height;
	LatteTexture_scaleToEffectiveSize(sourceTexture, &targetWidth, &targetHeight, 0);

	// neutral blend/depth state and no vertex attributes except the two the copy shader would use
	renderstate_resetColorControl();
	renderstate_resetDepthControl();
	attributeStream_unbindVertexBuffer();
	SetArrayElementBuffer(0);
	LatteDraw_resetAttributePointerCache();
	for (uint32 attrib = 0; attrib < GPU_GL_MAX_NUM_ATTRIBUTE; attrib++)
		SetAttributeArrayState(attrib, attrib < 2, -1);
	catchOpenGLError();

	g_renderer->renderTarget_setViewport(0, 0, (float)targetWidth, (float)targetHeight, 0.0f, 1.0f);
	catchOpenGLError();

	LatteTextureView* srcView = sourceTexture->GetOrCreateView(srcMip, 1, srcSlice, 1);
	LatteTextureView* dstView = destinationTexture->GetOrCreateView(dstMip, 1, dstSlice, 1);
	texture_bindAndActivate(srcView, 0);
	catchOpenGLError();
	DisableDepthCompare(static_cast<LatteTextureViewGL*>(srcView));
	catchOpenGLError();

	const bool toDepth = destinationTexture->isDepth;
	if (toDepth)
		LatteMRT::BindDepthBufferOnly(dstView);
	else
		LatteMRT::BindColorBufferOnly(dstView);
	catchOpenGLError();
	if (toDepth)
		renderstate_setAlwaysWriteDepth();

	const LatteGLDefaultShader_t* prog = toDepth ? LatteGLDefaultShader_getPixelCopyShader_colorToDepth() : LatteGLDefaultShader_getPixelCopyShader_depthToColor();
	glUseProgram(prog->glProgamId);
	catchOpenGLError();
	glUniform1i(prog->copyShaderUniforms.uniformLoc_textureSrc, 0);
	catchOpenGLError();

	// corners: top-left, top-right, bottom-left, bottom-right; uv spans the copied part of the (unscaled) source
	const float u = (float)width / (float)sourceTexture->width;
	const float v = (float)height / (float)sourceTexture->height;
	const float corners[4][4] = {
		{ -1.0f, 1.0f, 0.0f, 0.0f },
		{ 1.0f, 1.0f, u, 0.0f },
		{ -1.0f, -1.0f, 0.0f, v },
		{ 1.0f, -1.0f, u, v },
	};
	glUniform4fv(prog->copyShaderUniforms.uniformLoc_vertexOffsets, 4, &corners[0][0]);
	catchOpenGLError();

	const uint16 quadIndices[6] = { 0, 1, 3, 0, 2, 3 };
	glDrawRangeElements(GL_TRIANGLES, 0, 5, 6, GL_UNSIGNED_SHORT, quadIndices);
	catchOpenGLError();

	LatteGPUState.repeatTextureInitialization = true;
	glUseProgram(0);
}
