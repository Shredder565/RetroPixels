// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"

// Streamout is emulated with transform feedback into one ring buffer (glStreamoutCacheRingBuffer)

void OpenGLRenderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
	catchOpenGLError();
	glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, bufferIndex, glStreamoutCacheRingBuffer, ringBufferOffset, rangeSize);
	catchOpenGLError();
}

void OpenGLRenderer::streamout_begin()
{
	// the decompiled vertex shaders write streamout data per vertex, so every supported mode captures points
	const auto mode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	using PrimType = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	GLenum captureMode = GL_POINTS;
	if (mode != PrimType::POINTS && mode != PrimType::TRIANGLES && mode != PrimType::QUADS)
	{
		cemuLog_logDebug(LogType::Force, "Streamout with unsupported primitive mode 0x{:02x}", (uint32)mode);
		cemu_assert_debug(false);
	}
	cemu_assert_debug(!m_isXfbActive);
	glEnable(GL_RASTERIZER_DISCARD);
	glBeginTransformFeedback(captureMode);
	catchOpenGLError();
	m_isXfbActive = true;
}

void OpenGLRenderer::streamout_rendererFinishDrawcall()
{
	if (!m_isXfbActive)
		return;
	glEndTransformFeedback();
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
	glDisable(GL_RASTERIZER_DISCARD);
	m_isXfbActive = false;
}

void OpenGLRenderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	if (!glCopyNamedBufferSubData)
	{
		cemuLog_log(LogType::Force, "OpenGL: glCopyNamedBufferSubData is not available, streamout data is lost");
		return;
	}
	glCopyNamedBufferSubData(glStreamoutCacheRingBuffer, glAttributeCacheAB, srcOffset, dstOffset, size);
}
