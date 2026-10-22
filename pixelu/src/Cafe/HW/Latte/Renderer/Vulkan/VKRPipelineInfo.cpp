// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "config/CemuConfig.h"

namespace
{
	RendererShaderVk* HostShaderOf(LatteDecompilerShader* shader)
	{
		return shader ? (RendererShaderVk*)shader->shader : nullptr;
	}

	// Pixel shaders that render wrong without full barriers between draws, so they keep them
	// even when the user turned "Accurate barriers" off (base hash, aux hash).
	struct ShaderId
	{
		uint64 baseHash;
		uint64 auxHash;
	};
	constexpr ShaderId s_alwaysAccurateBarrierShaders[] = {
		{ 0x6f6f6e7b9aae57af, 0x00078787f9249249 }, // Breath of the Wild: lava
		{ 0x4c0bd596e3aef4a6, 0x003c3c3fc9269249 }, // Breath of the Wild: foam at the foot of waterfalls
		{ 0x3a7e6b48dae31305, 0x003c3c3fc9269249 }, // Breath of the Wild: Gerudo Town water
	};
}

PipelineInfo::PipelineInfo(uint64 minimalStateHash, uint64 pipelineHash, LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader)
{
	this->minimalStateHash = minimalStateHash;
	this->stateHash = pipelineHash;
	this->fetchShader = fetchShader;
	this->vertexShader = vertexShader;
	this->geometryShader = geometryShader;
	this->pixelShader = pixelShader;
	vertexShaderVk = HostShaderOf(vertexShader);
	geometryShaderVk = HostShaderOf(geometryShader);
	pixelShaderVk = HostShaderOf(pixelShader);

	m_vkrObjPipeline = new VKRObjectPipeline();

	// the shaders know which pipelines use them, so a shader reload can drop those
	for (RendererShaderVk* shader : { vertexShaderVk, geometryShaderVk, pixelShaderVk })
	{
		if (shader)
			shader->TrackDependency(this);
	}

	if (pixelShader)
	{
		for (const ShaderId& id : s_alwaysAccurateBarrierShaders)
		{
			if (pixelShader->baseHash == id.baseHash && pixelShader->auxHash == id.auxHash)
				neverSkipAccurateBarrier = true;
		}
	}
}

PipelineInfo::~PipelineInfo()
{
	delete rectEmulationGS;
	rectEmulationGS = nullptr;

	// a descriptor set info removes itself from its cache when deleted
	for (auto& cache : ds_cache)
	{
		while (!cache.empty())
			delete cache.begin()->second;
	}

	for (RendererShaderVk* shader : { vertexShaderVk, geometryShaderVk, pixelShaderVk })
	{
		if (shader)
			shader->RemoveDependency(this);
	}

	if (m_vkrObjPipeline)
	{
		VulkanRenderer::GetInstance()->ReleaseDestructibleObject(m_vkrObjPipeline);
		m_vkrObjPipeline = nullptr;
	}
	VulkanRenderer::GetInstance()->unregisterGraphicsPipeline(this);
}
