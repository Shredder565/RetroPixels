// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"

performanceMonitor_t performanceMonitor{};

namespace
{
	constexpr sint32 kSlots = PERFORMANCE_MONITOR_TRACK_CYCLES;

	auto& Slot(sint32 index)
	{
		return performanceMonitor.cycle[((index % kSlots) + kSlots) % kSlots];
	}

	void FinishFrameTimers()
	{
		auto& pm = performanceMonitor;
		LattePerfStatTimer* timers[] = {
			&pm.gpuTime_shaderCreate, &pm.gpuTime_frameTime, &pm.gpuTime_idleTime, &pm.gpuTime_fenceTime,
			&pm.gpuTime_dcStageTextures, &pm.gpuTime_dcStageVertexMgr, &pm.gpuTime_dcStageShaderAndUniformMgr,
			&pm.gpuTime_dcStageIndexMgr, &pm.gpuTime_dcStageMRT, &pm.gpuTime_dcStageDrawcallAPI, &pm.gpuTime_waitForAsync,
		};
		for (LattePerfStatTimer* timer : timers)
			timer->frameFinished();
	}

	// starts the next one-second slot
	void AdvanceSlot()
	{
		const sint32 next = (performanceMonitor.cycleIndex + 1) % kSlots;
		auto& slot = performanceMonitor.cycle[next];
		slot = {};
		slot.lastUpdate = GetTickCount();
		performanceMonitor.cycleIndex = next;
	}
}

void LattePerformanceMonitor_frameEnd()
{
	FinishFrameTimers();

	const sint32 current = performanceMonitor.cycleIndex;
	if (GetTickCount() - Slot(current).lastUpdate < 1000)
		return;
	const bool firstUpdate = Slot(current).lastUpdate == 0;

	// totals over all slots
	uint32 frames = 0, drawCalls = 0, fastDrawCalls = 0;
	uint64 indexBytes = 0;
	for (const auto& slot : performanceMonitor.cycle)
	{
		frames += slot.frameCounter;
		drawCalls += slot.drawCallCounter;
		fastDrawCalls += slot.fastDrawCallCounter;
		indexBytes += slot.indexDataUploaded;
	}
	frames = std::max<uint32>(frames, 1);
	performanceMonitor.stats.indexDataUploadPerFrame = (uint32)(indexBytes / frames);

	// frame rate over the current and the previous slot
	const uint32 recentFrames = std::max<uint32>(Slot(current).frameCounter + Slot(current - 1).frameCounter, 1);
	const uint32 recentMs = GetTickCount() - Slot(current - 1).lastUpdate;
	const double fps = (double)recentFrames * 1000.0 / (double)recentMs;

	AdvanceSlot();

	if (firstUpdate)
	{
		LatteOverlay_updateStats(0.0, 0, 0);
		WindowSystem::UpdateWindowTitles(false, false, 0.0);
		return;
	}
	LatteOverlay_updateStats(fps, drawCalls / frames, fastDrawCalls / frames);
	WindowSystem::UpdateWindowTitles(false, false, fps);
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
}
