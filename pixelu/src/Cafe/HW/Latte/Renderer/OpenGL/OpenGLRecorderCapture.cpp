// Original code for RetroPixels (PixelU). Not derived from Cemu.
// PixelU recorder, OpenGL side: the TV picture of a frame is copied into a pixel pack buffer and fenced.
// recorder_pollFrames() later picks up every copy the GPU has finished and passes the pixels to LatteRecorder,
// oldest frame slot first, so the render thread never stalls on the readback.

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

namespace
{
	// matches the output shader: convert only when the TV buffer and the source texture disagree about sRGB
	LatteRecorder::ColorConversion ChooseColorConversion(const LatteTextureView* view)
	{
		const bool textureIsSRGB = HAS_FLAG(view->format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
		const bool outputIsSRGB = LatteGPUState.tvBufferUsesSRGB;
		if (textureIsSRGB == outputIsSRGB)
			return LatteRecorder::ColorConversion::None;
		return textureIsSRGB ? LatteRecorder::ColorConversion::SRGBToLinear : LatteRecorder::ColorConversion::LinearToSRGB;
	}
}

void OpenGLRenderer::recorder_captureTVFrame(LatteTextureView* texView, sint64 frameSlot)
{
	auto* texture = static_cast<LatteTextureGL*>(texView->baseTexture);
	if (texView->firstMip != 0 || texture->dim == Latte::E_DIM::DIM_3D)
	{
		LatteRecorder::NotifyFrameDropped("unsupported TV buffer layout");
		return;
	}
	auto slot = std::find_if(m_recorderReadbacks.begin(), m_recorderReadbacks.end(), [](const RecorderReadback& r) { return !r.pending; });
	if (slot == m_recorderReadbacks.end())
	{
		LatteRecorder::NotifyFrameDropped("GPU readback still busy");
		return;
	}
	RecorderReadback& rb = *slot;

	sint32 width, height;
	texture->GetEffectiveSize(width, height, 0);
	if (width <= 0 || height <= 0)
		return;
	const size_t byteCount = (size_t)width * (size_t)height * 4;
	const LatteRecorder::ColorConversion conversion = ChooseColorConversion(texView);

	// buffers are reused and only grow
	if (rb.pixelBuffer == 0)
		glGenBuffers(1, &rb.pixelBuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, rb.pixelBuffer);
	if (rb.bufferSize < byteCount)
	{
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)byteCount, nullptr, GL_STREAM_READ);
		rb.bufferSize = byteCount;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	// the driver converts whatever the TV format is to RGBA8 while writing into the pack buffer
	glGetTextureSubImage(texture->glId_texture, 0, 0, 0, texView->firstSlice, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)byteCount, nullptr);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	rb.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();

	rb.pending = true;
	rb.frameSlot = frameSlot;
	rb.width = (uint32)width;
	rb.height = (uint32)height;
	rb.colorConversion = conversion;
}

void OpenGLRenderer::recorder_pollFrames()
{
	constexpr size_t kSlotCount = std::tuple_size_v<decltype(m_recorderReadbacks)>;
	RecorderReadback* ready[kSlotCount];
	size_t readyCount = 0;
	for (RecorderReadback& rb : m_recorderReadbacks)
	{
		if (!rb.pending)
			continue;
		switch (glClientWaitSync(rb.fence, 0, 0))
		{
		case GL_ALREADY_SIGNALED:
		case GL_CONDITION_SATISFIED:
			ready[readyCount++] = &rb;
			break;
		case GL_WAIT_FAILED:
			// give the slot up, the frame never reaches the recorder
			glDeleteSync(rb.fence);
			rb.fence = nullptr;
			rb.pending = false;
			break;
		default:
			break; // still in flight
		}
	}
	std::sort(ready, ready + readyCount, [](const RecorderReadback* x, const RecorderReadback* y) { return x->frameSlot < y->frameSlot; });

	for (size_t n = 0; n < readyCount; n++)
	{
		RecorderReadback& rb = *ready[n];
		glDeleteSync(rb.fence);
		rb.fence = nullptr;
		rb.pending = false;

		const size_t byteCount = (size_t)rb.width * rb.height * 4;
		glBindBuffer(GL_PIXEL_PACK_BUFFER, rb.pixelBuffer);
		if (const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)byteCount, GL_MAP_READ_BIT))
		{
			std::vector<uint8> pixels = LatteRecorder::AcquireFrameBuffer(byteCount);
			memcpy(pixels.data(), mapped, byteCount);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			LatteRecorder::SubmitVideoFrame(std::move(pixels), rb.width, rb.height, rb.frameSlot, rb.colorConversion);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
}
