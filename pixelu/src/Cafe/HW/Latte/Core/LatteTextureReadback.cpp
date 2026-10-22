// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

// Texture readback mirrors GPU-written textures back into guest memory for the CPU.
// A requested readback first waits a few draws (the texture is often written more
// than once in a row), then the transfer runs asynchronously and is written to guest
// memory once the backend reports it finished.

namespace
{
	struct PendingReadback
	{
		HRTick requestTime;
		uint32 lastWriteDrawIndex; // draw counter at the latest request
		LatteTextureView* view;
	};

	constexpr uint32 kDrawsBeforeTransfer = 5;

	std::vector<PendingReadback> s_pendingReadbacks; // waiting for the transfer to start
	std::queue<LatteTextureReadbackInfo*> s_transfersInFlight; // in start order

	HRTick NowTick()
	{
		return HighResolutionTimer::now().getTick();
	}

	double MillisecondsSince(HRTick tick)
	{
		return HighResolutionTimer::getTimeDiff(tick, NowTick()) * 1000.0;
	}
}

void LatteTextureReadback_StartTransfer(LatteTextureView* textureView)
{
	LatteTexture* tex = textureView->baseTexture;
	cemuLog_log(LogType::TextureReadback, "[TextureReadback] start {:08x} {}x{} format {} slice {} mip {}", tex->physAddress, tex->width, tex->height, tex->format, textureView->firstSlice, textureView->firstMip);
	const HRTick startTick = NowTick();
	LatteTextureReadbackInfo* transfer = g_renderer->texture_createReadback(textureView);
	if (!transfer)
		return;
	s_transfersInFlight.push(transfer);
	transfer->StartTransfer();
	transfer->transferStartTime = startTick;
}

// starts the transfers whose texture was not written for a few draws (or all of them
// when forced); returns true if any transfer was started
bool LatteTextureReadback_Update(bool forceStart)
{
	bool started = false;
	for (size_t i = 0; i < s_pendingReadbacks.size();)
	{
		const PendingReadback entry = s_pendingReadbacks[i];
		const uint32 drawsSinceWrite = LatteGPUState.drawCallCounter - entry.lastWriteDrawIndex;
		if (!forceStart && drawsSinceWrite < kDrawsBeforeTransfer)
		{
			i++;
			continue;
		}
		cemuLog_log(LogType::TextureReadback, "[TextureReadback] transfer {:08x} after {} draws, {:.4}ms after the request{}", entry.view->baseTexture->physAddress, drawsSinceWrite, MillisecondsSince(entry.requestTime), forceStart ? " (forced)" : "");
		s_pendingReadbacks.erase(s_pendingReadbacks.begin() + i);
		LatteTextureReadback_StartTransfer(entry.view);
		started = true;
	}
	return started;
}

void LatteTextureReadback_NotifyTextureDeletion(LatteTexture* texture)
{
	// requests for any view of the texture become invalid
	s_pendingReadbacks.erase(std::remove_if(s_pendingReadbacks.begin(), s_pendingReadbacks.end(),
		[texture](const PendingReadback& p) { return p.view->baseTexture == texture; }), s_pendingReadbacks.end());
}

void LatteTextureReadback_Initate(LatteTextureView* textureView)
{
	LatteTexture* tex = textureView->baseTexture;
	if (tex->overwriteInfo.hasResolutionOverwrite)
	{
		cemuLog_log(LogType::Force, "Texture readback is not supported for textures resized by texture rules ({:08x} {}x{})", tex->physAddress, tex->width, tex->height);
		return;
	}
	// a repeated request only delays the transfer
	for (PendingReadback& p : s_pendingReadbacks)
	{
		if (p.view == textureView)
		{
			p.lastWriteDrawIndex = LatteGPUState.drawCallCounter;
			return;
		}
	}
	s_pendingReadbacks.push_back({ NowTick(), LatteGPUState.drawCallCounter, textureView });
}

// Writes finished transfers to guest memory, in the order they were started. With
// forceFinish every pending and in-flight transfer is completed first.
void LatteTextureReadback_UpdateFinishedTransfers(bool forceFinish)
{
	if (forceFinish)
		LatteTextureReadback_Update(true);
	performanceMonitor.gpuTime_waitForAsync.beginMeasuring();
	while (!s_transfersInFlight.empty())
	{
		LatteTextureReadbackInfo* transfer = s_transfersInFlight.front();
		if (!transfer->IsFinished())
		{
			if (!forceFinish)
				break;
			transfer->waitStartTime = NowTick();
			if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
				cemuLog_log(LogType::TextureReadback, "[TextureReadback] waiting for {:08x} {}x{} tm {} format {:04x}, in flight for {:.4}ms", transfer->hostTextureCopy.physAddress, transfer->hostTextureCopy.width, transfer->hostTextureCopy.height, transfer->hostTextureCopy.tileMode, (uint32)transfer->hostTextureCopy.format, MillisecondsSince(transfer->transferStartTime));
			transfer->forceFinish = true;
			// the backend may complete other transfers (and modify the queue) from here, so look at the queue again
			transfer->ForceFinish();
			continue;
		}
		if (!forceFinish)
			transfer->waitStartTime = NowTick();
		if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
			cemuLog_log(LogType::TextureReadback, "[TextureReadback] done {:08x} {}x{} tm {} format {:04x}, latency {:6.3}ms, waited {:6.3}ms{}", transfer->hostTextureCopy.physAddress, transfer->hostTextureCopy.width, transfer->hostTextureCopy.height, transfer->hostTextureCopy.tileMode, (uint32)transfer->hostTextureCopy.format, MillisecondsSince(transfer->transferStartTime), MillisecondsSince(transfer->waitStartTime), transfer->forceFinish ? " (forced)" : "");
		LatteTextureLoader_writeReadbackTextureToMemory(&transfer->hostTextureCopy, transfer->m_firstSlice, transfer->m_firstMip, transfer->GetData(), transfer->m_rowPitch);
		transfer->ReleaseData();
		// guest memory now holds the texture data; keep the texture (if still cached) from reloading it
		const LatteTextureDefinition& def = transfer->hostTextureCopy;
		if (LatteTextureView* view = LatteTextureViewLookupCache::lookupSlice(def.physAddress, def.width, def.height, def.pitch, 0, 0, def.format))
			LatteTC_ResetTextureChangeTracker(view->baseTexture, true);
		delete transfer;
		cemu_assert_debug(!s_transfersInFlight.empty() && s_transfersInFlight.front() == transfer);
		s_transfersInFlight.pop();
	}
	performanceMonitor.gpuTime_waitForAsync.endMeasuring();
}

// synchronous readback of a view's first mip into a linear buffer (dstPitch in texels)
bool LatteTextureReadback_ReadbackToLinearBlocking(LatteTextureView* sourceView, uint8* dstPtr, uint32 dstWidth, uint32 dstHeight, uint32 dstPitch)
{
	LatteTextureReadbackInfo* transfer = g_renderer->texture_createReadback(sourceView);
	if (!transfer)
		return false;
	transfer->StartTransfer();
	transfer->ForceFinish();
	cemu_assert(transfer->IsFinished());
	// the backend delivers texels in the Latte format
	const uint8* src = transfer->GetData();
	const uint32 bytesPerTexel = Latte::GetFormatBits(sourceView->baseTexture->format) / 8;
	const uint32 rowBytes = dstWidth * bytesPerTexel;
	for (uint32 y = 0; y < dstHeight; y++)
		memcpy(dstPtr + (size_t)y * dstPitch * bytesPerTexel, src + (size_t)y * transfer->m_rowPitch, rowBytes);
	transfer->ReleaseData();
	delete transfer;
	return true;
}

uint32 LatteTextureReadbackInfo::GetReadbackRowPitch(LatteTextureView* textureView, uint32 rowAlignment)
{
	uint32 rowElements = textureView->baseTexture->GetMipWidth(textureView->firstMip);
	if (Latte::IsCompressedFormat(textureView->format))
		rowElements = (rowElements + 3) / 4;
	const uint32 rowBytes = rowElements * (Latte::GetFormatBits(textureView->format) / 8);
	return (rowBytes + rowAlignment - 1) & ~(rowAlignment - 1);
}

uint32 LatteTextureReadbackInfo::GetReadbackImageSize(LatteTextureView* textureView, uint32 rowPitch)
{
	uint32 rows = textureView->baseTexture->GetMipHeight(textureView->firstMip);
	if (Latte::IsCompressedFormat(textureView->format))
		rows = (rows + 3) / 4;
	return rows * rowPitch;
}
