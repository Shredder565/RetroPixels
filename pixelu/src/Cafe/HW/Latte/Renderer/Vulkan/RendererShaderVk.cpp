// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "util/helpers/ConcurrentQueue.h"
#include "Cemu/FileCache/FileCache.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include "util/helpers/helpers.h"

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

namespace
{
	bool s_cacheLoadActive = false; // a title's shader cache is being loaded
	FileCache* s_spirvFileCache = nullptr; // precompiled SPIR-V of the running title

	// Resource limits handed to glslang. The values feed into the generated SPIR-V, and with that
	// into the precompiled cache, so they must not change.
	TBuiltInResource MakeGlslangResourceLimits()
	{
		TBuiltInResource r = {};

		// legacy / general
		r.maxLights = 32;
		r.maxClipPlanes = 6;
		r.maxTextureUnits = 32;
		r.maxTextureCoords = 32;
		r.maxVertexAttribs = 64;
		r.maxVaryingFloats = 64;
		r.maxVaryingVectors = 8;
		r.maxVaryingComponents = 60;
		r.maxDrawBuffers = 32;
		r.maxCombinedTextureImageUnits = 80;
		r.maxTextureImageUnits = 32;
		r.minProgramTexelOffset = -8;
		r.maxProgramTexelOffset = 7;
		r.maxClipDistances = 8;
		r.maxCullDistances = 8;
		r.maxCombinedClipAndCullDistances = 8;
		r.maxSamples = 4;
		r.maxViewports = 16;
		r.maxTransformFeedbackBuffers = 4;
		r.maxTransformFeedbackInterleavedComponents = 64;

		// vertex
		r.maxVertexUniformComponents = 4096;
		r.maxVertexTextureImageUnits = 32;
		r.maxVertexUniformVectors = 128;
		r.maxVertexOutputVectors = 16;
		r.maxVertexOutputComponents = 64;
		r.maxVertexImageUniforms = 0;
		r.maxVertexAtomicCounters = 0;
		r.maxVertexAtomicCounterBuffers = 0;

		// fragment
		r.maxFragmentUniformComponents = 4096;
		r.maxFragmentUniformVectors = 16;
		r.maxFragmentInputVectors = 15;
		r.maxFragmentInputComponents = 128;
		r.maxFragmentImageUniforms = 8;
		r.maxFragmentAtomicCounters = 8;
		r.maxFragmentAtomicCounterBuffers = 1;

		// geometry
		r.maxGeometryInputComponents = 64;
		r.maxGeometryOutputComponents = 128;
		r.maxGeometryImageUniforms = 0;
		r.maxGeometryTextureImageUnits = 16;
		r.maxGeometryOutputVertices = 256;
		r.maxGeometryTotalOutputComponents = 1024;
		r.maxGeometryUniformComponents = 1024;
		r.maxGeometryVaryingComponents = 64;
		r.maxGeometryAtomicCounters = 0;
		r.maxGeometryAtomicCounterBuffers = 0;

		// tessellation
		r.maxTessControlImageUniforms = 0;
		r.maxTessEvaluationImageUniforms = 0;
		r.maxTessControlInputComponents = 128;
		r.maxTessControlOutputComponents = 128;
		r.maxTessControlTextureImageUnits = 16;
		r.maxTessControlUniformComponents = 1024;
		r.maxTessControlTotalOutputComponents = 4096;
		r.maxTessEvaluationInputComponents = 128;
		r.maxTessEvaluationOutputComponents = 128;
		r.maxTessEvaluationTextureImageUnits = 16;
		r.maxTessEvaluationUniformComponents = 1024;
		r.maxTessPatchComponents = 120;
		r.maxPatchVertices = 32;
		r.maxTessGenLevel = 64;
		r.maxTessControlAtomicCounters = 0;
		r.maxTessEvaluationAtomicCounters = 0;
		r.maxTessControlAtomicCounterBuffers = 0;
		r.maxTessEvaluationAtomicCounterBuffers = 0;

		// compute
		r.maxComputeWorkGroupCountX = 65535;
		r.maxComputeWorkGroupCountY = 65535;
		r.maxComputeWorkGroupCountZ = 65535;
		r.maxComputeWorkGroupSizeX = 1024;
		r.maxComputeWorkGroupSizeY = 1024;
		r.maxComputeWorkGroupSizeZ = 64;
		r.maxComputeUniformComponents = 1024;
		r.maxComputeTextureImageUnits = 16;
		r.maxComputeImageUniforms = 8;
		r.maxComputeAtomicCounters = 8;
		r.maxComputeAtomicCounterBuffers = 1;

		// images and atomics across stages
		r.maxImageUnits = 8;
		r.maxCombinedImageUnitsAndFragmentOutputs = 8;
		r.maxCombinedShaderOutputResources = 8;
		r.maxImageSamples = 0;
		r.maxCombinedImageUniforms = 8;
		r.maxCombinedAtomicCounters = 8;
		r.maxAtomicCounterBindings = 1;
		r.maxCombinedAtomicCounterBuffers = 1;
		r.maxAtomicCounterBufferSize = 16384;

		// mesh / task (NV)
		r.maxMeshOutputVerticesNV = 256;
		r.maxMeshOutputPrimitivesNV = 512;
		r.maxMeshWorkGroupSizeX_NV = 32;
		r.maxMeshWorkGroupSizeY_NV = 1;
		r.maxMeshWorkGroupSizeZ_NV = 1;
		r.maxTaskWorkGroupSizeX_NV = 32;
		r.maxTaskWorkGroupSizeY_NV = 1;
		r.maxTaskWorkGroupSizeZ_NV = 1;
		r.maxMeshViewCountNV = 4;

		// no restrictions on loops and indexing
		r.limits = {};
		r.limits.nonInductiveForLoops = true;
		r.limits.whileLoops = true;
		r.limits.doWhileLoops = true;
		r.limits.generalUniformIndexing = true;
		r.limits.generalAttributeMatrixVectorIndexing = true;
		r.limits.generalVaryingIndexing = true;
		r.limits.generalSamplerIndexing = true;
		r.limits.generalVariableIndexing = true;
		r.limits.generalConstantMatrixVectorIndexing = true;
		return r;
	}

	EShLanguage GlslangStageOf(RendererShader::ShaderType type)
	{
		switch (type)
		{
		case RendererShader::ShaderType::kVertex:
			return EShLangVertex;
		case RendererShader::ShaderType::kFragment:
			return EShLangFragment;
		case RendererShader::ShaderType::kGeometry:
			return EShLangGeometry;
		}
		cemu_assert_debug(false);
		return EShLangVertex;
	}

	constexpr EShMessages kGlslangMessages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
}

// Two threads take shaders from a FIFO and compile them. A counting semaphore holds one count per
// queued shader; a shader taken out by PreponeCompilation() leaves its count behind, which a worker
// then consumes without finding work.
class VkShaderCompileWorkers
{
public:
	~VkShaderCompileWorkers()
	{
		Stop();
	}

	void Start()
	{
		if (m_running.exchange(true))
			return;
		constexpr uint32 kWorkerCount = 2;
		for (uint32 n = 0; n < kWorkerCount; n++)
			m_workers.emplace_back([this]() { WorkerMain(); });
	}

	void Stop()
	{
		if (!m_running.exchange(false))
			return;
		// one wake up per worker so each sees the stop flag
		for (size_t n = 0; n < m_workers.size(); n++)
			m_pending.increment();
		for (std::thread& worker : m_workers)
			worker.join();
		m_workers.clear();
	}

	bool IsRunning() const { return m_running; }

	void Enqueue(RendererShaderVk* shader)
	{
		std::lock_guard lock(m_queueLock);
		shader->m_state.setValue(RendererShaderVk::COMPILATION_STATE::QUEUED);
		m_queue.push_back(shader);
		m_pending.increment();
	}

	// takes the shader out of the queue if no worker picked it up yet; it is then marked COMPILING
	bool TryClaim(RendererShaderVk* shader)
	{
		std::lock_guard lock(m_queueLock);
		if (!shader->m_state.hasState(RendererShaderVk::COMPILATION_STATE::QUEUED))
			return false;
		m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), shader), m_queue.end());
		shader->m_state.setValue(RendererShaderVk::COMPILATION_STATE::COMPILING);
		return true;
	}

private:
	RendererShaderVk* TakeNext()
	{
		std::lock_guard lock(m_queueLock);
		if (m_queue.empty())
			return nullptr;
		RendererShaderVk* shader = m_queue.front();
		m_queue.pop_front();
		cemu_assert_debug(shader->m_state.getValue() == RendererShaderVk::COMPILATION_STATE::QUEUED);
		shader->m_state.setValue(RendererShaderVk::COMPILATION_STATE::COMPILING);
		return shader;
	}

	void WorkerMain()
	{
		SetThreadName("vkShaderComp");
		while (m_running.load(std::memory_order_relaxed))
		{
			m_pending.decrementWithWait();
			RendererShaderVk* shader = TakeNext();
			if (!shader)
				continue;
			shader->CompileInternal(false);
			++g_compiled_shaders_async;
			cemu_assert_debug(shader->m_state.getValue() == RendererShaderVk::COMPILATION_STATE::COMPILING);
			shader->m_state.setValue(RendererShaderVk::COMPILATION_STATE::DONE);
		}
	}

	std::vector<std::thread> m_workers;
	std::deque<RendererShaderVk*> m_queue;
	std::mutex m_queueLock;
	CounterSemaphore m_pending;
	std::atomic<bool> m_running{ false };
};

static VkShaderCompileWorkers s_compileWorkers;

RendererShaderVk::RendererShaderVk(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslCode)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_source(glslCode)
{
	s_compileWorkers.Enqueue(this);
	cemu_assert_debug(s_compileWorkers.IsRunning()); // Init() has to come first
}

RendererShaderVk::~RendererShaderVk()
{
	// a pipeline unregisters itself from its shaders when it is deleted
	while (!m_usedByPipelines.empty())
		delete m_usedByPipelines.front();
	vkDestroyShaderModule(VulkanRenderer::GetInstance()->GetLogicalDevice(), m_shader_module, nullptr);
}

void RendererShaderVk::Init()
{
	s_compileWorkers.Start();
}

void RendererShaderVk::Shutdown()
{
	s_compileWorkers.Stop();
}

void RendererShaderVk::CreateVkShaderModule(std::span<uint32> spirvBuffer)
{
	VulkanRenderer* renderer = (VulkanRenderer*)g_renderer.get();
	VkDevice device = renderer->GetLogicalDevice();

	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = spirvBuffer.size_bytes();
	info.pCode = spirvBuffer.data();
	VkResult res = vkCreateShaderModule(device, &info, nullptr, &m_shader_module);
	if (res != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan: Shader error");
		throw std::runtime_error(fmt::format("Failed to create shader module: {}", res));
	}

	if (!renderer->IsDebugMarkersEnabled())
		return;
	const std::string label = fmt::format("shader_{:016x}_{:016x}", m_baseHash, m_auxHash);
	VkDebugUtilsObjectNameInfoEXT nameInfo{};
	nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	nameInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
	nameInfo.objectHandle = (uint64_t)m_shader_module;
	nameInfo.pObjectName = label.c_str();
	vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
}

void RendererShaderVk::FinishCompilation()
{
	// the source is not needed anymore, free it
	std::string().swap(m_source);
}

bool RendererShaderVk::UsesSpirvCache(bool withDebugInfo) const
{
	// shaders replaced by graphic packs and debug builds of shaders never go through the cache
	return s_spirvFileCache && m_isGameShader && !m_isGfxPackShader && !withDebugInfo;
}

void RendererShaderVk::CompileInternal(bool isRenderThread)
{
	const bool withDebugInfo = ((VulkanRenderer*)g_renderer.get())->IsTracingToolEnabled();
	const bool useCache = UsesSpirvCache(withDebugInfo);

	// during cache loading the SPIR-V is usually already there
	if (useCache && s_cacheLoadActive)
	{
		cemu_assert_debug(m_baseHash != 0);
		uint64 key1, key2;
		GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, key1, key2);
		std::vector<uint8> cached;
		if (s_spirvFileCache->GetFile({ key1, key2 }, cached))
		{
			CreateVkShaderModule(std::span<uint32>((uint32*)cached.data(), cached.size() / sizeof(uint32)));
			FinishCompilation();
			return;
		}
	}

	const EShLanguage stage = GlslangStageOf(GetType());
	const TBuiltInResource limits = MakeGlslangResourceLimits();

	glslang::TShader shader(stage);
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

	// preprocess first, then parse the preprocessed text
	const char* sourceText = m_source.c_str();
	shader.setStrings(&sourceText, 1);
	std::string preprocessed;
	glslang::TShader::ForbidIncluder noIncludes;
	if (!shader.preprocess(&limits, 450, ENoProfile, false, false, kGlslangMessages, &preprocessed, noIncludes))
	{
		cemuLog_log(LogType::Force, fmt::format("GLSL Preprocessing Failed For {:016x}_{:016x}: \"{}\"", m_baseHash, m_auxHash, shader.getInfoLog()));
		FinishCompilation();
		return;
	}
	const char* preprocessedText = preprocessed.c_str();
	shader.setStrings(&preprocessedText, 1);
	if (!shader.parse(&limits, 100, false, kGlslangMessages))
	{
		cemuLog_log(LogType::Force, fmt::format("GLSL parsing failed for {:016x}_{:016x}: \"{}\"", m_baseHash, m_auxHash, shader.getInfoLog()));
		cemuLog_logDebug(LogType::Force, "GLSL source:\n{}", m_source);
		cemu_assert_debug(false);
		FinishCompilation();
		return;
	}

	glslang::TProgram program;
	program.addShader(&shader);
	const bool linked = program.link(kGlslangMessages);
	if (!linked || !program.mapIO())
	{
		cemuLog_log(LogType::Force, fmt::format("GLSL linking failed for {:016x}_{:016x}: \"{}\"", m_baseHash, m_auxHash, program.getInfoLog()));
		if (!linked)
			cemu_assert_debug(false);
		FinishCompilation();
		return;
	}

	glslang::SpvOptions options;
	options.disableOptimizer = false;
	options.validate = false;
	options.optimizeSize = true;
	if (withDebugInfo)
	{
		// embed the GLSL so tracing tools can show it
		options.generateDebugInfo = true;
		options.emitNonSemanticShaderDebugInfo = true;
		options.emitNonSemanticShaderDebugSource = true;
		shader.addSourceText(m_source.c_str(), (uint32)m_source.size());
		shader.setSourceFile(fmt::format("shader_{:016x}_{:016x}.glsl", m_baseHash, m_auxHash).c_str());
	}

	std::vector<uint32> spirv;
	spv::SpvBuildLogger spvLog;
	glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spvLog, &options);

	if (useCache)
	{
		uint64 key1, key2;
		GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, key1, key2);
		s_spirvFileCache->AddFile({ key1, key2 }, (const uint8*)spirv.data(), spirv.size() * sizeof(uint32));
	}

	CreateVkShaderModule(spirv);

	// shaders compiled while the cache loads are not counted
	if (!s_cacheLoadActive && m_isGameShader)
		++g_compiled_shaders_total;

	FinishCompilation();
}

void RendererShaderVk::PreponeCompilation(bool isRenderThread)
{
	if (s_compileWorkers.TryClaim(this))
	{
		CompileInternal(isRenderThread);
		m_state.setValue(COMPILATION_STATE::DONE);
		return;
	}
	// a worker has it (or is done), wait for it; the wait means it no longer counts as async
	m_state.waitUntilValue(COMPILATION_STATE::DONE);
	--g_compiled_shaders_async;
}

bool RendererShaderVk::IsCompiled()
{
	return m_state.hasState(COMPILATION_STATE::DONE);
}

bool RendererShaderVk::WaitForCompiled()
{
	m_state.waitUntilValue(COMPILATION_STATE::DONE);
	return true;
}

void RendererShaderVk::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
	delete s_spirvFileCache;
	s_spirvFileCache = nullptr;

	const std::string fileName = fmt::format("{:016x}_spirv.bin", cacheTitleId);
	const fs::path path = ActiveSettings::GetCachePath("shaderCache/precompiled/{}", fileName);
	s_spirvFileCache = FileCache::Open(path, true, GeneratePrecompiledCacheId());
	if (!s_spirvFileCache)
		cemuLog_log(LogType::Force, "Unable to open SPIR-V cache {}", fileName);
	s_cacheLoadActive = true;
}

void RendererShaderVk::ShaderCacheLoading_end()
{
	// the cache stays open, shaders compiled during gameplay are still added to it
	s_cacheLoadActive = false;
}

void RendererShaderVk::ShaderCacheLoading_Close()
{
	delete s_spirvFileCache;
	s_spirvFileCache = nullptr;
}
