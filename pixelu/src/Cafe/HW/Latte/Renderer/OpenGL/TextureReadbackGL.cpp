// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLTextureReadback.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"

namespace
{
	struct PackFormat
	{
		GLenum format;
		GLenum type;
	};

	// formats games read back to CPU memory; depth textures are read through the depth component
	bool GetPackFormat(Latte::E_GX2SURFFMT format, bool isDepth, PackFormat& out)
	{
		using F = Latte::E_GX2SURFFMT;
		switch (format)
		{
		case F::R8_G8_B8_A8_UNORM:
		case F::R8_G8_B8_A8_SRGB:
			out = { GL_RGBA, GL_UNSIGNED_BYTE };
			return true;
		case F::R8_G8_UNORM:
			out = { GL_RG, GL_UNSIGNED_BYTE };
			return true;
		case F::R16_UNORM:
			if (isDepth)
			{
				out = { GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT };
				cemu_assert_unimplemented();
			}
			else
				out = { GL_RED, GL_UNSIGNED_SHORT };
			return true;
		case F::R16_G16_B16_A16_UNORM:
			out = { GL_RGBA, GL_UNSIGNED_SHORT };
			return true;
		case F::R16_G16_B16_A16_FLOAT:
			out = { GL_RGBA, GL_HALF_FLOAT };
			return true;
		case F::R32_FLOAT:
			out = { isDepth ? (GLenum)GL_DEPTH_COMPONENT : (GLenum)GL_RED, GL_FLOAT };
			return true;
		case F::R32_G32_B32_A32_FLOAT:
			out = { GL_RGBA, GL_FLOAT };
			return true;
		case F::R32_G32_B32_A32_UINT:
			out = { GL_RGBA_INTEGER, GL_UNSIGNED_INT };
			return true;
		case F::R32_G32_B32_A32_SINT:
			out = { GL_RGBA_INTEGER, GL_INT };
			return true;
		default:
			return false;
		}
	}
}

LatteTextureReadbackInfoGL::LatteTextureReadbackInfoGL(LatteTextureView* textureView)
	: LatteTextureReadbackInfo(textureView, textureView->firstMip)
{
	PackFormat pack;
	if (!GetPackFormat(textureView->format, textureView->baseTexture->isDepth, pack))
	{
		cemuLog_logDebug(LogType::Force, "OpenGL: texture readback of format {:04x} is not supported", (uint32)textureView->format);
		return;
	}
	m_packFormat = pack.format;
	m_packType = pack.type;
	// rows keep GL's default pack alignment of 4
	m_rowPitch = GetReadbackRowPitch(textureView, 4);
	m_image_size = GetReadbackImageSize(textureView, m_rowPitch);
}

LatteTextureReadbackInfoGL::~LatteTextureReadbackInfoGL()
{
	if (m_fence)
		glDeleteSync(m_fence);
	if (m_packBuffer)
		glDeleteBuffers(1, &m_packBuffer);
}

void LatteTextureReadbackInfoGL::StartTransfer()
{
	cemu_assert(m_textureView);
	auto* tex = static_cast<LatteTextureGL*>(m_textureView->baseTexture);
	cemu_assert_debug(tex->dim != Latte::E_DIM::DIM_3D);
	glGenBuffers(1, &m_packBuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_packBuffer);
	glBufferData(GL_PIXEL_PACK_BUFFER, m_image_size, nullptr, GL_DYNAMIC_READ);
	// with a pack buffer bound the pixels pointer is an offset into it
	glGetTextureSubImage(tex->glId_texture, m_firstMip, 0, 0, m_firstSlice, tex->GetMipWidth(m_firstMip), tex->GetMipHeight(m_firstMip), 1, m_packFormat, m_packType, m_image_size, nullptr);
	glFlush();
	m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	m_textureView = nullptr;
}

bool LatteTextureReadbackInfoGL::WaitForFence(GLuint64 timeoutNs)
{
	const GLenum result = glClientWaitSync(m_fence, timeoutNs ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, timeoutNs);
	switch (result)
	{
	case GL_ALREADY_SIGNALED:
	case GL_CONDITION_SATISFIED:
		return true;
	case GL_TIMEOUT_EXPIRED:
		return false;
	default:
		throw std::runtime_error("OpenGL: Error waiting for texture readback");
	}
}

bool LatteTextureReadbackInfoGL::IsFinished()
{
	return WaitForFence(0);
}

void LatteTextureReadbackInfoGL::ForceFinish()
{
	while (!WaitForFence(1000000000ull)) // poll in one second steps
		;
}

uint8* LatteTextureReadbackInfoGL::GetData()
{
	glBindBuffer(GL_PIXEL_PACK_BUFFER, m_packBuffer);
	return static_cast<uint8*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
}

void LatteTextureReadbackInfoGL::ReleaseData()
{
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
}
