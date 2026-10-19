// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// GPU statistics shown by the overlay. Counters are collected in one-second slots and
// the last PERFORMANCE_MONITOR_TRACK_CYCLES slots are averaged.

#define PERFORMANCE_MONITOR_TRACK_CYCLES	(5)

uint64 PPCTimer_getRawTsc();
uint64 PPCTimer_tscToMicroseconds(uint64 us);

// accumulates time between begin/end pairs over a frame
class LattePerfStatTimer
{
public:
	void beginMeasuring()
	{
		m_start = PPCTimer_getRawTsc();
	}

	void endMeasuring()
	{
		m_frameTotal += PPCTimer_getRawTsc() - m_start;
	}

	// closes the frame: the total becomes the previous frame value
	void frameFinished()
	{
		m_lastFrame = std::exchange(m_frameTotal, 0);
	}

	uint64 getPreviousFrameValue()
	{
		return m_lastFrame;
	}

private:
	uint64 m_start{};
	uint64 m_frameTotal{};
	uint64 m_lastFrame{};
};

// thread-safe object counter
class LattePerfStatCounter
{
public:
	void increment()
	{
		m_count++;
	}

	void decrement()
	{
		cemu_assert_debug(m_count > 0);
		m_count--;
	}

	void decrement(uint32 count)
	{
		cemu_assert_debug(count <= m_count);
		m_count -= count;
	}

	uint32 get()
	{
		return m_count;
	}

	void reset()
	{
		m_count = 0;
	}

private:
	std::atomic_uint32_t m_count{};
};

struct performanceMonitor_t
{
	// one slot per second
	struct
	{
		uint64 lastCycleCount;
		uint64 skippedCycles;
		uint32 recompilerLeaveCount;
		uint32 threadLeaveCount;
		uint32 lastUpdate; // tick count when the slot started
		uint32 frameCounter;
		uint32 drawCallCounter;
		uint32 fastDrawCallCounter;
		uint32 shaderBindCount;
		uint64 vertexDataUploaded; // bytes
		uint64 vertexDataCached; // bytes served from the buffer cache
		uint64 uniformBankUploadedData; // uniform buffer bytes (not counting remapped uniforms)
		uint64 uniformBankUploadedCount;
		uint64 indexDataUploaded;
		uint64 indexDataCached;
	}cycle[PERFORMANCE_MONITOR_TRACK_CYCLES];
	sint32 cycleIndex;

	// per-frame timers
	LattePerfStatTimer gpuTime_frameTime;
	LattePerfStatTimer gpuTime_shaderCreate;
	LattePerfStatTimer gpuTime_idleTime; // waiting for commands from the CPU
	LattePerfStatTimer gpuTime_fenceTime; // waiting on fences
	// draw call stages
	LattePerfStatTimer gpuTime_dcStageTextures;
	LattePerfStatTimer gpuTime_dcStageVertexMgr;
	LattePerfStatTimer gpuTime_dcStageShaderAndUniformMgr;
	LattePerfStatTimer gpuTime_dcStageIndexMgr;
	LattePerfStatTimer gpuTime_dcStageMRT;
	LattePerfStatTimer gpuTime_dcStageDrawcallAPI;
	LattePerfStatTimer gpuTime_waitForAsync; // GX2DrawDone, forced readbacks, query polling

	uint32 numCompiledVS;
	uint32 numCompiledGS;
	uint32 numCompiledPS;

	// Vulkan object counts
	struct
	{
		LattePerfStatCounter numDescriptorSets;
		LattePerfStatCounter numDescriptorDynUniformBuffers;
		LattePerfStatCounter numDescriptorStorageBuffers;
		LattePerfStatCounter numDescriptorSamplerTextures;
		LattePerfStatCounter numGraphicPipelines;
		LattePerfStatCounter numImages;
		LattePerfStatCounter numImageViews;
		LattePerfStatCounter numSamplers;
		LattePerfStatCounter numRenderPass;
		LattePerfStatCounter numFramebuffer;
		// reset every frame
		LattePerfStatCounter numDrawBarriersPerFrame;
		LattePerfStatCounter numBeginRenderpassPerFrame;
	}vk;

	// derived values, updated once per slot
	struct
	{
		uint32 indexDataUploadPerFrame;
	}stats;
};

extern performanceMonitor_t performanceMonitor;

void LattePerformanceMonitor_frameEnd();
void LattePerformanceMonitor_frameBegin();

// for code templated on a THasProfiling flag
#define beginPerfMonProfiling(__obj) if( THasProfiling ) __obj.beginMeasuring()
#define endPerfMonProfiling(__obj) if( THasProfiling ) __obj.endMeasuring()
