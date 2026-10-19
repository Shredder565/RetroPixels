// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "util/helpers/Serializer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/Core/LatteShaderCache.h"
#include "util/helpers/helpers.h"
#include <openssl/sha.h>

// One cache entry in memory. On disk (big endian):
//   u8 version (1), u8 stage mask (1 = vertex, 2 = geometry, 4 = pixel),
//   per present stage in that order: u64 base hash, u64 aux hash,
//   then the serialized compacted register state.
struct CachedPipeline
{
	struct ShaderHash
	{
		uint64 baseHash;
		uint64 auxHash;
		bool isPresent{};

		void set(uint64 base, uint64 aux)
		{
			baseHash = base;
			auxHash = aux;
			isPresent = true;
		}
	};

	ShaderHash vsHash; // the fetch shader is implied by the vertex shader
	ShaderHash gsHash;
	ShaderHash psHash;

	Latte::GPUCompactedRegisterState gpuState;
};

namespace
{
	constexpr uint8 kEntryVersion = 1;
	constexpr uint8 kStageVertex = 1;
	constexpr uint8 kStageGeometry = 2;
	constexpr uint8 kStagePixel = 4;
	constexpr size_t kMaxQueuedEntries = 50;

	// progress of the loader, driven by UpdateLoading() on the loading screen
	struct
	{
		uint32 nextFileIndex;
		uint32 lastFileIndex;
		std::atomic_uint32_t queued;
		std::atomic_uint32_t done;
	} s_load;

	ConcurrentQueue<CachedPipeline*> s_writeQueue; // entries waiting to be written to the file
	FSpinlock s_loaderCleanupLock;

	// the render pass of a cached pipeline is rebuilt from the register state; the size does not matter
	VkFormat ColorAttachmentFormat(uint32 index, const LatteContextRegister& regs)
	{
		VulkanRenderer::FormatInfoVK info;
		VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(LatteMRT::GetColorBufferFormat(index, regs), false, Latte::E_DIM::DIM_2D, 1280, 720, &info);
		return info.vkImageFormat;
	}

	VKRObjectRenderPass* MakeStandInRenderPass(const LatteDecompilerShader* pixelShader, const LatteContextRegister& regs)
	{
		VKRObjectRenderPass::AttachmentInfo_t attachments;
		const uint8 colorMask = LatteMRT::GetActiveColorBufferMask(pixelShader, regs);
		for (uint32 i = 0; i < 8; i++)
		{
			auto& color = attachments.colorAttachment[i];
			color.viewObj = nullptr;
			if (!(colorMask & (1 << i)))
				continue;
			color.isPresent = true;
			color.format = ColorAttachmentFormat(i, regs);
		}

		auto& depth = attachments.depthAttachment;
		depth.viewObj = nullptr;
		depth.isPresent = LatteMRT::GetActiveDepthBufferMask(regs);
		if (depth.isPresent)
		{
			VulkanRenderer::FormatInfoVK info;
			VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(LatteMRT::GetDepthBufferFormat(regs), true, Latte::E_DIM::DIM_2D, 1280, 720, &info);
			depth.format = info.vkImageFormat;
			depth.hasStencil = (info.vkImageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
		}
		return new VKRObjectRenderPass(attachments);
	}

	void WriteShaderHash(MemStreamWriter& w, const CachedPipeline::ShaderHash& h)
	{
		if (!h.isPresent)
			return;
		w.writeBE<uint64>(h.baseHash);
		w.writeBE<uint64>(h.auxHash);
	}

	void ReadShaderHash(MemStreamReader& r, uint8 stageMask, uint8 stageBit, CachedPipeline::ShaderHash& h)
	{
		if (!(stageMask & stageBit))
			return;
		uint64 base = r.readBE<uint64>();
		uint64 aux = r.readBE<uint64>();
		h.set(base, aux);
	}

	// looks up a stage's shader; false if the entry names a shader that is not loaded
	bool ResolveShader(const CachedPipeline::ShaderHash& h, LatteDecompilerShader* (*find)(uint64, uint64), const char* stageName, LatteDecompilerShader*& out)
	{
		out = nullptr;
		if (!h.isPresent)
			return true;
		out = find(h.baseHash, h.auxHash);
		if (out)
			return true;
		cemuLog_logDebug(LogType::Force, "{} shader not found in cache", stageName);
		return false;
	}
}

VulkanPipelineStableCache g_vkPipelineStableCacheInstance;

VulkanPipelineStableCache& VulkanPipelineStableCache::GetInstance()
{
	return g_vkPipelineStableCacheInstance;
}

uint32 VulkanPipelineStableCache::BeginLoading(uint64 cacheTitleId)
{
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetCachePath("shaderCache/transferable"), ec);
	const fs::path filePath = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_vkpipeline.bin", cacheTitleId);

	s_load.nextFileIndex = 0;
	s_load.lastFileIndex = 0;
	s_load.done = 0;
	s_load.queued = 0;
	m_loadQueue.clear();

	// one loader per physical core, at most 8
	uint32 threads = std::clamp(GetPhysicalCoreCount(), 1u, 8u);
	if (VulkanRenderer::GetInstance()->GetDisableMultithreadedCompilation())
		threads = 1;
	m_loaderThreadCount = threads;
	for (uint32 i = 0; i < threads; i++)
		std::thread(&VulkanPipelineStableCache::CompilerThread, this).detach();

	cemu_assert_debug(m_file == nullptr);
	m_file = FileCache::Open(filePath, true, LatteShaderCache_getPipelineCacheExtraVersion(cacheTitleId));
	if (!m_file)
	{
		cemuLog_log(LogType::Force, "Failed to open or create Vulkan pipeline cache file: {}", _pathToUtf8(filePath));
		return 0;
	}
	m_file->UseCompression(false);
	s_load.lastFileIndex = m_file->GetMaximumFileIndex();
	return m_file->GetFileCount();
}

bool VulkanPipelineStableCache::UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders)
{
	pipelinesLoadedTotal = s_load.done;
	pipelinesMissingShaders = 0;
	// hand out at most one entry per call and keep the queue short
	for (; s_load.nextFileIndex <= s_load.lastFileIndex; s_load.nextFileIndex++)
	{
		if (m_loadQueue.size() >= kMaxQueuedEntries)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			return true;
		}
		uint64 nameA, nameB;
		std::vector<uint8> entry;
		if (!m_file->GetFileByIndex(s_load.nextFileIndex, &nameA, &nameB, entry))
			continue; // unused index
		s_load.queued++;
		m_loadQueue.push(std::move(entry));
		s_load.nextFileIndex++;
		return true;
	}
	if (s_load.done != s_load.queued)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return true; // loaders are still busy
	}
	return false;
}

void VulkanPipelineStableCache::EndLoading()
{
	// each loader exits after taking an empty entry once the count is zero
	const uint32 threads = m_loaderThreadCount.exchange(0);
	for (uint32 i = 0; i < threads; i++)
		m_loadQueue.push({});
	// the file stays open, new pipelines are still added during gameplay
}

void VulkanPipelineStableCache::Close()
{
	delete m_file;
	m_file = nullptr;
}

void VulkanPipelineStableCache::LoadPipelineFromCache(std::span<uint8> fileData)
{
	auto entry = std::make_unique<CachedPipeline>();
	MemStreamReader reader(fileData.data(), fileData.size());
	if (!DeserializePipeline(reader, *entry))
		return;
	auto regs = std::make_unique<LatteContextRegister>();
	Latte::LoadGPURegisterState(*regs, entry->gpuState);

	LatteDecompilerShader* vs;
	LatteDecompilerShader* gs;
	LatteDecompilerShader* ps;
	if (!ResolveShader(entry->vsHash, LatteSHRC_FindVertexShader, "Vertex", vs) ||
		!ResolveShader(entry->gsHash, LatteSHRC_FindGeometryShader, "Geometry", gs) ||
		!ResolveShader(entry->psHash, LatteSHRC_FindPixelShader, "Pixel", ps))
		return;
	if (!ps)
	{
		cemu_assert_debug(false);
		return;
	}

	VKRObjectRenderPass* renderPass = MakeStandInRenderPass(ps, *regs);
	m_knownPipelinesLock.lock();
	PipelineInfo* pipeline = new PipelineInfo(0, 0, vs->compatibleFetchShader, vs, ps, gs);
	m_knownPipelinesLock.unlock();

	bool compiled;
	{
		PipelineCompiler compiler;
		const bool robustAccess = PipelineCompiler::CalcRobustBufferAccessRequirement(vs, ps, gs);
		compiled = compiler.InitFromCurrentGPUState(pipeline, *regs, renderPass, robustAccess);
		if (compiled)
			compiler.Compile(true, true, false);
	}
	if (!compiled)
		return;

	const uint64 stateHash = VulkanRenderer::draw_calculateGraphicsPipelineHash(vs->compatibleFetchShader, vs, gs, ps, renderPass, *regs);
	m_knownPipelinesLock.lock();
	m_knownPipelines.emplace(vs->baseHash, stateHash);
	m_knownPipelinesLock.unlock();

	// loaders tear down one at a time
	s_loaderCleanupLock.lock();
	delete pipeline;
	regs.reset();
	entry.reset();
	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(renderPass);
	s_loaderCleanupLock.unlock();
}

bool VulkanPipelineStableCache::HasPipelineCached(uint64 baseHash, uint64 pipelineStateHash)
{
	return m_knownPipelines.find(PipelineHash(baseHash, pipelineStateHash)) != m_knownPipelines.end();
}

void VulkanPipelineStableCache::AddCurrentStateToCache(uint64 baseHash, uint64 pipelineStateHash)
{
	m_knownPipelines.emplace(baseHash, pipelineStateHash);
	if (!m_writerThread)
	{
		m_writerThread = new std::thread(&VulkanPipelineStableCache::WorkerThread, this);
		m_writerThread->detach();
	}
	// snapshot of the active shaders and the register state (minus irrelevant ALU constants)
	CachedPipeline* entry = new CachedPipeline();
	if (auto* vs = LatteSHRC_GetActiveVertexShader())
		entry->vsHash.set(vs->baseHash, vs->auxHash);
	if (auto* gs = LatteSHRC_GetActiveGeometryShader())
		entry->gsHash.set(gs->baseHash, gs->auxHash);
	if (auto* ps = LatteSHRC_GetActivePixelShader())
		entry->psHash.set(ps->baseHash, ps->auxHash);
	Latte::StoreGPURegisterState(LatteGPUState.contextNew, entry->gpuState);
	s_writeQueue.push(entry);
}

bool VulkanPipelineStableCache::SerializePipeline(MemStreamWriter& memWriter, CachedPipeline& cachedPipeline)
{
	uint8 stageMask = 0;
	if (cachedPipeline.vsHash.isPresent)
		stageMask |= kStageVertex;
	if (cachedPipeline.gsHash.isPresent)
		stageMask |= kStageGeometry;
	if (cachedPipeline.psHash.isPresent)
		stageMask |= kStagePixel;
	memWriter.writeBE<uint8>(kEntryVersion);
	memWriter.writeBE<uint8>(stageMask);
	WriteShaderHash(memWriter, cachedPipeline.vsHash);
	WriteShaderHash(memWriter, cachedPipeline.gsHash);
	WriteShaderHash(memWriter, cachedPipeline.psHash);
	Latte::SerializeRegisterState(cachedPipeline.gpuState, memWriter);
	return true;
}

bool VulkanPipelineStableCache::DeserializePipeline(MemStreamReader& memReader, CachedPipeline& cachedPipeline)
{
	if (memReader.readBE<uint8>() != kEntryVersion)
	{
		cemuLog_log(LogType::Force, "Cached Vulkan pipeline corrupted or has unknown version");
		return false;
	}
	const uint8 stageMask = memReader.readBE<uint8>();
	ReadShaderHash(memReader, stageMask, kStageVertex, cachedPipeline.vsHash);
	ReadShaderHash(memReader, stageMask, kStageGeometry, cachedPipeline.gsHash);
	ReadShaderHash(memReader, stageMask, kStagePixel, cachedPipeline.psHash);
	if (!Latte::DeserializeRegisterState(cachedPipeline.gpuState, memReader))
		return false;
	cemu_assert_debug(!memReader.hasError());
	return true;
}

int VulkanPipelineStableCache::CompilerThread()
{
	SetThreadName("plCacheCompiler");
	while (m_loaderThreadCount != 0)
	{
		std::vector<uint8> entry = m_loadQueue.pop();
		if (entry.empty())
			continue; // wake up from EndLoading()
		LoadPipelineFromCache(entry);
		++s_load.done;
	}
	return 0;
}

void VulkanPipelineStableCache::WorkerThread()
{
	SetThreadName("plCacheWriter");
	for (;;)
	{
		CachedPipeline* entry;
		s_writeQueue.pop(entry);
		std::unique_ptr<CachedPipeline> owner(entry);
		if (!m_file)
			continue;
		MemStreamWriter writer(4 * 1024);
		SerializePipeline(writer, *entry);
		auto data = writer.getResult();
		// entries are named after the SHA-256 of their content (first 16 bytes, big endian)
		uint8 digest[SHA256_DIGEST_LENGTH];
		SHA256(data.data(), data.size(), digest);
		const uint64 nameA = *(uint64be*)(digest + 0);
		const uint64 nameB = *(uint64be*)(digest + 8);
		m_file->AddFileAsync({ nameA, nameB }, data.data(), data.size());
	}
}
