// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// PixelU recorder: asynchronous readback of the TV picture for LatteRecorder
// Like HandleScreenshotRequest, but the texture is read into a pixel buffer object guarded by a fence
// and collected on a later frame, so the GPU is never waited on.

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

void OpenGLRenderer::recorder_pollFrames()
{
	std::array<RecorderReadback*, std::tuple_size_v<decltype(m_recorderReadbacks)>> finished;
	size_t finishedCount = 0;
	for (auto& readback : m_recorderReadbacks)
	{
		if (!readback.pending)
			continue;
		const GLenum status = glClientWaitSync(readback.fence, 0, 0);
		if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
			finished[finishedCount++] = &readback;
		else if (status == GL_WAIT_FAILED)
		{
			glDeleteSync(readback.fence);
			readback.fence = nullptr;
			readback.pending = false;
		}
	}
	// hand over in picture order
	std::sort(finished.begin(), finished.begin() + finishedCount, [](const RecorderReadback* a, const RecorderReadback* b) { return a->frameSlot < b->frameSlot; });
	for (size_t i = 0; i < finishedCount; i++)
	{
		RecorderReadback& readback = *finished[i];
		glDeleteSync(readback.fence);
		readback.fence = nullptr;
		readback.pending = false;
		const size_t size = (size_t)readback.width * readback.height * 4;
		glBindBuffer(GL_PIXEL_PACK_BUFFER, readback.pixelBuffer);
		const uint8* ptr = (const uint8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)size, GL_MAP_READ_BIT);
		if (ptr)
		{
			std::vector<uint8> data = LatteRecorder::AcquireFrameBuffer(size);
			memcpy(data.data(), ptr, size);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			LatteRecorder::SubmitVideoFrame(std::move(data), readback.width, readback.height, readback.frameSlot, readback.colorConversion);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
}

void OpenGLRenderer::recorder_captureTVFrame(LatteTextureView* texView, sint64 frameSlot)
{
	LatteTextureGL* baseTexture = (LatteTextureGL*)texView->baseTexture;
	if (texView->firstMip != 0 || baseTexture->dim == Latte::E_DIM::DIM_3D)
	{
		LatteRecorder::NotifyFrameDropped("unsupported TV buffer layout");
		return;
	}
	RecorderReadback* readback = nullptr;
	for (auto& it : m_recorderReadbacks)
	{
		if (!it.pending)
		{
			readback = &it;
			break;
		}
	}
	if (!readback)
	{
		LatteRecorder::NotifyFrameDropped("GPU readback still busy");
		return;
	}

	sint32 width, height;
	baseTexture->GetEffectiveSize(width, height, 0);
	if (width <= 0 || height <= 0)
		return;
	const size_t size = (size_t)width * height * 4;

	// same rule as the screenshot code: match what the output shader shows
	const bool srcUsesSRGB = HAS_FLAG(texView->format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
	const bool dstUsesSRGB = LatteGPUState.tvBufferUsesSRGB;
	LatteRecorder::ColorConversion colorConversion = LatteRecorder::ColorConversion::None;
	if (srcUsesSRGB && !dstUsesSRGB)
		colorConversion = LatteRecorder::ColorConversion::SRGBToLinear;
	else if (!srcUsesSRGB && dstUsesSRGB)
		colorConversion = LatteRecorder::ColorConversion::LinearToSRGB;

	if (readback->pixelBuffer == 0)
		glGenBuffers(1, &readback->pixelBuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->pixelBuffer);
	if (readback->bufferSize < size)
	{
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)size, nullptr, GL_STREAM_READ);
		readback->bufferSize = size;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	// converts any color format to RGBA8 on the GPU side
	glGetTextureSubImage(baseTexture->glId_texture, 0, 0, 0, texView->firstSlice, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)size, nullptr);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	readback->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();

	readback->pending = true;
	readback->frameSlot = frameSlot;
	readback->width = (uint32)width;
	readback->height = (uint32)height;
	readback->colorConversion = colorConversion;
}
