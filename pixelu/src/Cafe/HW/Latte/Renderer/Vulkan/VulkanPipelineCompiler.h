// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "VKRBase.h"

// Builds one graphics pipeline. InitFromCurrentGPUState() translates the Latte register state into
// Vulkan create infos (this has to happen on the render thread), Compile() then creates the pipeline,
// either right away or later on one of the pipeline compile threads.
class PipelineCompiler : public VKRMoveableRefCounter
{
private:
	// geometry shader that turns RECT primitives into quads when VK_NV_fill_rectangle is missing
	RendererShaderVk* m_rectEmulationGS{};

	bool SetupShaderStages(VulkanRenderer* renderer);
	void SetupVertexInput(const LatteContextRegister& regs, LatteDecompilerShader* vertexShader, LatteFetchShader* fetchShader);
	void SetupInputAssembly(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode);
	void SetupViewport();
	void SetupRasterization(const LatteContextRegister& regs, VulkanRenderer* renderer, bool isRectPrimitive, bool& usesDepthBias);
	void SetupColorBlend(const LatteContextRegister& regs, bool& usesBlendConstants, VKRObjectRenderPass* renderPass);
	void SetupDescriptorSetLayouts(VulkanRenderer* renderer, PipelineInfo* pipelineInfo);
	void SetupDepthStencil();
	void SetupDynamicState(PipelineInfo* pipelineInfo, bool usesBlendConstants, bool usesDepthBias);

public:
	PipelineCompiler();
	~PipelineCompiler();

	VKRObjectPipeline* m_vkrObjPipeline{};
	LatteFetchShader* m_fetchShader{};
	RendererShaderVk* m_vkVertexShader{};
	RendererShaderVk* m_vkPixelShader{};
	RendererShaderVk* m_vkGeometryShader{};

	bool InitFromCurrentGPUState(PipelineInfo* pipelineInfo, const LatteContextRegister& latteRegister, VKRObjectRenderPass* renderPassObj, bool requireRobustBufferAccess);
	void TrackAsCached(uint64 baseHash, uint64 pipelineStateHash); // adds the pipeline to the transferable cache unless it is known already; render thread only, it snapshots the GPU state

	// robust buffer access is needed when a shader may access buffers out of bounds
	static bool CalcRobustBufferAccessRequirement(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader);

	// background compilation; a queued compiler is deleted by the thread that compiles it
	static void CompileThreadPool_Start();
	static void CompileThreadPool_Stop();
	static void CompileThreadPool_QueueCompilation(PipelineCompiler* v);

	VkPipelineLayout m_pipelineLayout;
	VKRObjectRenderPass* m_renderPassObj{};
	bool m_requestRobustBufferAccess{false};

	// create info storage, referenced by the VkGraphicsPipelineCreateInfo in Compile()
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

	std::vector<VkVertexInputAttributeDescription> vertexInputAttributeDescription;
	std::vector<VkVertexInputBindingDescription> vertexInputBindingDescription;
	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};

	VkPipelineViewportStateCreateInfo viewportState{};

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	VkPipelineRasterizationDepthClipStateCreateInfoEXT rasterizerExt{};
	VkPipelineMultisampleStateCreateInfo multisampling{};

	std::array<VkPipelineColorBlendAttachmentState, 8> colorBlendAttachments{};
	VkPipelineColorBlendStateCreateInfo colorBlending{};

	// set 0 vertex, set 1 pixel (or an empty placeholder), set 2 geometry
	VkDescriptorSetLayout descriptorSetLayout[3];
	sint32 descriptorSetLayoutCount = 0;

	VkPipelineDepthStencilStateCreateInfo depthStencilState{};

	std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};

	// returns true when the pipeline is finished with, successfully or not; false when forceCompile
	// is off and the pipeline could not be created without blocking
	bool Compile(bool forceCompile, bool isRenderThread, bool showInOverlay);
};
