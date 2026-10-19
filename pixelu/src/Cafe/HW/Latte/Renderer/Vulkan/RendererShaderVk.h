// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "util/helpers/ConcurrentQueue.h"

#include <vulkan/vulkan_core.h>
#include "util/helpers/Semaphore.h"
#include "util/helpers/fspinlock.h"

// A shader stage compiled from GLSL to SPIR-V by glslang. Compilation runs on a small worker pool;
// PreponeCompilation() pulls a still queued shader forward and compiles it on the calling thread.
class RendererShaderVk : public RendererShader
{
	friend class VulkanRenderer;
	friend class VkShaderCompileWorkers;

	enum class COMPILATION_STATE : uint32
	{
		NONE,
		QUEUED,
		COMPILING,
		DONE
	};

public:
	// while a title's shader cache loads, SPIR-V comes from (and goes to) the precompiled cache file
	static void ShaderCacheLoading_begin(uint64 cacheTitleId);
	static void ShaderCacheLoading_end();
	static void ShaderCacheLoading_Close();

	RendererShaderVk(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslCode);
	virtual ~RendererShaderVk();

	static void Init();
	static void Shutdown();

	VkShaderModule& GetShaderModule() { return m_shader_module; }

	static inline FSpinlock s_dependencyLock;

	// pipelines built from this shader register here, deleting the shader deletes them too
	void TrackDependency(class PipelineInfo* p)
	{
		s_dependencyLock.lock();
		m_usedByPipelines.emplace_back(p);
		s_dependencyLock.unlock();
	}

	void RemoveDependency(class PipelineInfo* p)
	{
		s_dependencyLock.lock();
		vectorRemoveByValue(m_usedByPipelines, p);
		s_dependencyLock.unlock();
	}

	void PreponeCompilation(bool isRenderThread) override;
	bool IsCompiled() override;
	bool WaitForCompiled() override;

private:
	void CompileInternal(bool isRenderThread);
	void FinishCompilation();
	void CreateVkShaderModule(std::span<uint32> spirvBuffer);

	bool UsesSpirvCache(bool withDebugInfo) const;

	VkShaderModule m_shader_module = nullptr;
	StateSemaphore<COMPILATION_STATE> m_state{ COMPILATION_STATE::NONE };
	std::string m_source;
	std::vector<class PipelineInfo*> m_usedByPipelines;
};
