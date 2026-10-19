// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "util/helpers/Serializer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "HW/Latte/Renderer/RendererCore.h"

extern std::atomic_int g_compiling_pipelines;
extern std::atomic_int g_compiling_pipelines_async;
extern std::atomic_uint64_t g_compiling_pipelines_syncTimeSum;

namespace
{
	// Geometry shader for RECT primitives: each incoming triangle holds three corners of a rectangle,
	// the fourth corner is derived from them and the quad is emitted as a strip. The vertex order
	// depends on which edge of the triangle is the diagonal (the longest one).
	RendererShaderVk* BuildRectEmulationGS(LatteDecompilerShader* vertexShader, const LatteContextRegister& regs)
	{
		LatteShaderPSInputTable* psInputs = LatteSHRC_GetPSInputTable();
		std::string src;
		src += "#version 450\r\n";
		src += "layout(triangles) in;\r\n";
		src += "layout(triangle_strip) out;\r\n";
		src += "layout(max_vertices = 4) out;\r\n";

		// pass-through of every VS output the pixel shader reads; all inputs first, then all outputs
		const auto outputMask = vertexShader->outputParameterMask;
		for (const bool asOutput : { false, true })
		{
			for (uint32 param = 0; param < 32; param++)
			{
				if (!(outputMask & (1u << param)))
					continue;
				const sint32 semantic = psInputs->getVertexShaderOutParamSemanticId(regs.GetRawView(), param);
				if (semantic < 0)
					continue;
				const auto* import = psInputs->getPSImportBySemanticId(semantic);
				if (!import)
					continue;
				src += fmt::format("layout(location = {}) ", psInputs->getPSImportLocationBySemanticId(semantic));
				if (import->isFlat)
					src += "flat ";
				if (import->isNoPerspective)
					src += "noperspective ";
				if (asOutput)
					src += fmt::format("out vec4 passParameterSem{}Out;\r\n", semantic);
				else
					src += fmt::format("in vec4 passParameterSem{}In[];\r\n", semantic);
			}
		}

		// fourth corner for each choice of diagonal
		const std::pair<const char*, const char*> fourthCorner[] = {
			{ "A", "b - (c - a)" },
			{ "B", "c - (b - a)" },
			{ "C", "c + (b - a)" },
		};
		for (const auto& [suffix, expression] : fourthCorner)
		{
			src += fmt::format("vec4 gen4thVertex{}(vec4 a, vec4 b, vec4 c)\r\n", suffix);
			src += "{\r\n";
			src += fmt::format("return {};\r\n", expression);
			src += "}\r\n";
		}

		src += "void main()\r\n";
		src += "{\r\n";
		src += "float dist0_1 = length(gl_in[1].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n";
		src += "float dist0_2 = length(gl_in[2].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n";
		src += "float dist1_2 = length(gl_in[2].gl_Position.xy - gl_in[1].gl_Position.xy);\r\n";
		src += "if(dist0_1 > dist0_2 && dist0_1 > dist1_2)\r\n";
		src += "{\r\n";
		rectsEmulationGS_outputVerticesCode(src, vertexShader, psInputs, 2, 1, 0, 3, "A", regs); // 0-1 is the diagonal
		src += "} else if ( dist0_2 > dist0_1 && dist0_2 > dist1_2 ) {\r\n";
		rectsEmulationGS_outputVerticesCode(src, vertexShader, psInputs, 1, 2, 0, 3, "B", regs); // 0-2 is the diagonal
		src += "} else {\r\n";
		rectsEmulationGS_outputVerticesCode(src, vertexShader, psInputs, 0, 1, 2, 3, "C", regs); // 1-2 is the diagonal
		src += "}\r\n";
		src += "}\r\n";

		auto* shader = new RendererShaderVk(RendererShader::ShaderType::kGeometry, 0, 0, false, false, src);
		shader->PreponeCompilation(true);
		return shader;
	}

	// Vertex data is fetched raw: the shader reinterprets the bits, so only the element size and
	// count matter here and every format maps to an unsigned integer format.
	VkFormat RawVertexFormat(Latte::E_HWFMT format)
	{
		using F = Latte::E_HWFMT;
		switch (format)
		{
		case F::HWFMT_32_32_32_32_FLOAT:
		case F::HWFMT_32_32_32_32:
			return VK_FORMAT_R32G32B32A32_UINT;
		case F::HWFMT_32_32_32_FLOAT:
		case F::HWFMT_32_32_32:
			return VK_FORMAT_R32G32B32_UINT;
		case F::HWFMT_32_32_FLOAT:
		case F::HWFMT_32_32:
			return VK_FORMAT_R32G32_UINT;
		case F::HWFMT_32_FLOAT:
		case F::HWFMT_32:
		case F::HWFMT_2_10_10_10: // packed into one dword, unpacked by the shader
			return VK_FORMAT_R32_UINT;
		case F::HWFMT_16_16_16_16_FLOAT:
		case F::HWFMT_16_16_16_16:
			return VK_FORMAT_R16G16B16A16_UINT;
		case F::HWFMT_16_16_16_FLOAT:
		case F::HWFMT_16_16_16:
			return VK_FORMAT_R16G16B16_UINT;
		case F::HWFMT_16_16_FLOAT:
		case F::HWFMT_16_16:
			return VK_FORMAT_R16G16_UINT;
		case F::HWFMT_16_FLOAT:
		case F::HWFMT_16:
			return VK_FORMAT_R16_UINT;
		case F::HWFMT_8_8_8_8:
			return VK_FORMAT_R8G8B8A8_UINT;
		case F::HWFMT_8_8_8:
			return VK_FORMAT_R8G8B8_UINT;
		case F::HWFMT_8_8:
			return VK_FORMAT_R8G8_UINT;
		case F::HWFMT_8:
			return VK_FORMAT_R8_UINT;
		default:
			break;
		}
		cemuLog_log(LogType::Force, "Unsupported vertex format: {:02x}", (uint32)format);
		assert_dbg();
		return VK_FORMAT_UNDEFINED;
	}

	VkBlendOp ToVkBlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC func)
	{
		using C = Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC;
		switch (func)
		{
		case C::DST_PLUS_SRC: return VK_BLEND_OP_ADD;
		case C::SRC_MINUS_DST: return VK_BLEND_OP_SUBTRACT;
		case C::DST_MINUS_SRC: return VK_BLEND_OP_REVERSE_SUBTRACT;
		case C::MIN_DST_SRC: return VK_BLEND_OP_MIN;
		case C::MAX_DST_SRC: return VK_BLEND_OP_MAX;
		default: break;
		}
		cemu_assert_suspicious();
		return VK_BLEND_OP_ADD;
	}

	// indexed by the Latte blend factor; 0x0B and 0x0C have no Vulkan equivalent
	constexpr VkBlendFactor s_blendFactors[] = {
		VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
		VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
		VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
		VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
		VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
		VK_BLEND_FACTOR_MAX_ENUM, VK_BLEND_FACTOR_MAX_ENUM,
		VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
		VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
		VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
		VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
	};

	VkBlendFactor ToVkBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
	{
		cemu_assert_debug((uint32)factor < std::size(s_blendFactors));
		return s_blendFactors[(uint32)factor];
	}

	// blend factors that read the blend constant, which is set as dynamic state
	bool ReadsBlendConstant(VkBlendFactor f)
	{
		switch (f)
		{
		case VK_BLEND_FACTOR_CONSTANT_COLOR:
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
		case VK_BLEND_FACTOR_CONSTANT_ALPHA:
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
			return true;
		default:
			return false;
		}
	}

	// render targets with integer formats cannot blend
	bool IsIntegerColorFormat(VkFormat f)
	{
		switch (f)
		{
		case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
		case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
		case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT:
		case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_B8G8R8A8_SINT:
		case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
		case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
		case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT:
		case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
		case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
		case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
		case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
			return true;
		default:
			return false;
		}
	}

	// Latte compare functions and stencil ops are numbered like the Vulkan enums
	constexpr VkCompareOp s_compareOps[8] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
		VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS
	};
	constexpr VkStencilOp s_stencilOps[8] = {
		VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
		VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP
	};

	struct StageBinding
	{
		VkShaderStageFlags flags;
		uint32 index;
	};

	StageBinding StageOf(LatteConst::ShaderType type)
	{
		switch (type)
		{
		case LatteConst::ShaderType::Vertex: return { VK_SHADER_STAGE_VERTEX_BIT, VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX };
		case LatteConst::ShaderType::Pixel: return { VK_SHADER_STAGE_FRAGMENT_BIT, VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT };
		case LatteConst::ShaderType::Geometry: return { VK_SHADER_STAGE_GEOMETRY_BIT, VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY };
		default: return { 0, 0 };
		}
	}

	// one descriptor set per shader stage: textures, the uniform register buffer, uniform blocks and
	// the transform feedback buffer, in that binding order. Records which dynamic offsets the stage needs.
	VkDescriptorSetLayout CreateStageSetLayout(VulkanRenderer* renderer, LatteDecompilerShader* shader, PipelineInfo* pipelineInfo)
	{
		const StageBinding stage = StageOf(shader->shaderType);
		auto& mapping = shader->resourceMapping;
		auto& offsets = pipelineInfo->dynamicOffsetInfo;

		std::vector<VkDescriptorSetLayoutBinding> bindings;
		auto add = [&](uint32 binding, VkDescriptorType type) {
			VkDescriptorSetLayoutBinding b{};
			b.binding = binding;
			b.descriptorType = type;
			b.descriptorCount = 1;
			b.stageFlags = stage.flags;
			bindings.push_back(b);
		};

		const sint32 firstTexture = mapping.getTextureBaseBindingPoint();
		if (firstTexture >= 0)
		{
			const sint32 count = mapping.getTextureCount();
			for (sint32 t = 0; t < count; t++)
				add((uint32)(firstTexture + t), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		}
		if (mapping.uniformVarsBufferBindingPoint >= 0)
			add(mapping.uniformVarsBufferBindingPoint, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
		for (sint32 ub = 0; ub < LATTE_NUM_MAX_UNIFORM_BUFFERS; ub++)
		{
			if (mapping.uniformBuffersBindingPoint[ub] < 0)
				continue;
			add(mapping.uniformBuffersBindingPoint[ub], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
			offsets.list_uniformBuffers[stage.index].emplace_back((uint8)ub);
		}
		if (mapping.tfStorageBindingPoint >= 0)
			add(mapping.tfStorageBindingPoint, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

		if (mapping.uniformVarsBufferBindingPoint >= 0)
			offsets.hasUniformVar[stage.index] = true;
		if (mapping.hasUniformBuffers())
			offsets.hasUniformBuffers[stage.index] = true;

		VkDescriptorSetLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = (uint32)bindings.size();
		info.pBindings = bindings.data();
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		if (vkCreateDescriptorSetLayout(renderer->GetLogicalDevice(), &info, nullptr, &layout) != VK_SUCCESS)
			renderer->UnrecoverableError(fmt::format("Failed to create descriptor set layout for shader {0:#x}", shader->baseHash).c_str());
		return layout;
	}
}

PipelineCompiler::PipelineCompiler() {}

PipelineCompiler::~PipelineCompiler()
{
	// references taken at the end of InitFromCurrentGPUState()
	if (m_vkrObjPipeline)
		m_vkrObjPipeline->decRef();
	if (m_renderPassObj)
		m_renderPassObj->decRef();
}

bool PipelineCompiler::SetupShaderStages(VulkanRenderer* renderer)
{
	RendererShaderVk* const stages[] = { m_vkVertexShader, m_vkPixelShader, m_vkGeometryShader };
	for (RendererShaderVk* s : stages)
		cemu_assert_debug(s == nullptr || s->IsCompiled());
	for (RendererShaderVk* s : stages)
	{
		if (s && s->GetShaderModule() == VK_NULL_HANDLE)
		{
			cemuLog_log(LogType::Force, "Vulkan-Info: Pipeline creation failed due to invalid shader(s)");
			return false;
		}
	}

	if (m_vkVertexShader)
		shaderStages.push_back(renderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, m_vkVertexShader->GetShaderModule(), "main"));
	RendererShaderVk* gs = m_vkGeometryShader ? m_vkGeometryShader : m_rectEmulationGS;
	if (gs)
		shaderStages.push_back(renderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_GEOMETRY_BIT, gs->GetShaderModule(), "main"));
	if (m_vkPixelShader)
		shaderStages.push_back(renderer->CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, m_vkPixelShader->GetShaderModule(), "main"));
	return true;
}

void PipelineCompiler::SetupVertexInput(const LatteContextRegister& regs, LatteDecompilerShader* vertexShader, LatteFetchShader* fetchShader)
{
	vertexInputAttributeDescription.reserve(16);
	vertexInputBindingDescription.reserve(fetchShader->bufferGroups.size());

	for (const auto& group : fetchShader->bufferGroups)
	{
		// all attributes of a buffer share one fetch type (per vertex or per instance)
		std::optional<LatteConst::VertexFetchType2> rate;
		for (sint32 a = 0; a < group.attribCount; a++)
		{
			const auto& attrib = group.attrib[a];
			const uint32 location = vertexShader->resourceMapping.attributeMapping[attrib.semanticId];
			if (location == (uint32)-1)
				continue; // the shader does not read it

			VkVertexInputAttributeDescription desc{};
			desc.location = location;
			desc.binding = attrib.attributeBufferIndex;
			desc.format = RawVertexFormat(attrib.format);
			desc.offset = attrib.offset;
			vertexInputAttributeDescription.push_back(desc);

			if (!rate)
				rate = attrib.fetchType;
			else
				cemu_assert_debug(rate == attrib.fetchType);
			if (attrib.fetchType == LatteConst::INSTANCE_DATA)
				cemu_assert_debug(attrib.aluDivisor == 1); // only a divisor of one is handled
		}

		// stride from SQ_VTX_CONSTANT word 2 of the buffer's fetch constant
		const uint32 bufferIndex = group.attributeBufferIndex;
		uint32 stride = (regs.GetRawView()[mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7 + 2] >> 11) & 0xFFFF;
#if BOOST_OS_MACOS
		stride = (stride + 3) & ~3u; // MoltenVK wants 4 byte aligned strides
#endif
		VkVertexInputBindingDescription binding{};
		binding.binding = bufferIndex;
		binding.stride = stride;
		if (!rate || *rate == LatteConst::VertexFetchType2::VERTEX_DATA)
			binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		else if (*rate == LatteConst::VertexFetchType2::INSTANCE_DATA)
			binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
		else
			cemu_assert(false);
		vertexInputBindingDescription.push_back(binding);
	}

	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = (uint32)vertexInputBindingDescription.size();
	vertexInputInfo.pVertexBindingDescriptions = vertexInputBindingDescription.data();
	vertexInputInfo.vertexAttributeDescriptionCount = (uint32)vertexInputAttributeDescription.size();
	vertexInputInfo.pVertexAttributeDescriptions = vertexInputAttributeDescription.data();
}

void PipelineCompiler::SetupInputAssembly(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode)
{
	using P = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	// strips and fans keep primitive restart, list topologies cannot use it
	bool restart = true;
	switch (primitiveMode)
	{
	case P::POINTS:
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		restart = false;
		break;
	case P::LINES:
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		restart = false;
		break;
	case P::LINE_STRIP:
	case P::LINE_LOOP: // drawn as a strip, the closing segment is added to the index data
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		break;
	case P::LINE_STRIP_ADJACENT:
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
		break;
	case P::TRIANGLE_FAN:
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		break;
	case P::TRIANGLE_STRIP:
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		break;
	case P::TRIANGLES:
	case P::QUADS: // quads, quad strips and rects become triangle lists
	case P::QUAD_STRIP:
	case P::RECTS:
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		restart = false;
		break;
	default:
		cemuLog_logDebug(LogType::Force, "Vulkan-Unsupported: Graphics pipeline with primitive mode {} created", primitiveMode);
		cemu_assert_debug(false);
	}
	inputAssembly.primitiveRestartEnable = restart ? VK_TRUE : VK_FALSE;
}

void PipelineCompiler::SetupViewport()
{
	// viewport and scissor rectangles are dynamic state
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
}

void PipelineCompiler::SetupRasterization(const LatteContextRegister& regs, VulkanRenderer* renderer, bool isRectPrimitive, bool& usesDepthBias)
{
	const auto& mode = regs.PA_SU_SC_MODE_CNTL;
	const auto frontFace = mode.get_FRONT_FACE();
	bool cullFront = mode.get_CULL_FRONT() != 0;
	bool cullBack = mode.get_CULL_BACK() != 0;
	const bool depthOffset = mode.get_OFFSET_FRONT_ENABLED() != 0;

	// clip and rasterizer kill state is read from the live GPU registers
	const auto& clip = LatteGPUState.contextNew.PA_CL_CLIP_CNTL;
	cemu_assert_debug(clip.get_ZCLIP_NEAR_DISABLE() == clip.get_ZCLIP_FAR_DISABLE()); // separate near/far clip control is not handled

	rasterizerExt.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	rasterizerExt.flags = 0;
	rasterizerExt.depthClipEnable = clip.get_ZCLIP_FAR_DISABLE() ? VK_FALSE : VK_TRUE;

	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.pNext = VulkanRenderer::GetInstance()->m_featureControl.deviceExtensions.depth_clip_enable ? &rasterizerExt : nullptr;
	// a disabled viewport X offset means GX2SetSpecialState(0, true), which also ignores rasterizer kill
	rasterizer.rasterizerDiscardEnable = (clip.get_DX_RASTERIZATION_KILL() && LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA()) ? VK_TRUE : VK_FALSE;
	rasterizer.polygonMode = (isRectPrimitive && renderer->m_featureControl.deviceExtensions.nv_fill_rectangle) ? VK_POLYGON_MODE_FILL_RECTANGLE_NV : VK_POLYGON_MODE_FILL;
	rasterizer.depthClampEnable = VK_TRUE;
	rasterizer.lineWidth = 1.0f;

	// the bias values themselves are dynamic state
	usesDepthBias = depthOffset;
	rasterizer.depthBiasEnable = depthOffset ? VK_TRUE : VK_FALSE;
	rasterizer.depthBiasConstantFactor = 0.0f;
	rasterizer.depthBiasSlopeFactor = 0.0f;
	rasterizer.depthBiasClamp = 0.0f;

	// rects are treated as clockwise, so only the cull flag of the clockwise side applies
	if (isRectPrimitive)
	{
		if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			cullFront = cullBack;
		else
			cullBack = cullFront;
	}
	rasterizer.cullMode = (cullFront ? VK_CULL_MODE_FRONT_BIT : 0) | (cullBack ? VK_CULL_MODE_BACK_BIT : 0);
	rasterizer.frontFace = frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;

	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisampling.sampleShadingEnable = VK_FALSE;
}

void PipelineCompiler::SetupColorBlend(const LatteContextRegister& regs, bool& usesBlendConstants, VKRObjectRenderPass* renderPass)
{
	const auto& colorControl = regs.CB_COLOR_CONTROL;
	const uint32 blendMask = colorControl.get_BLEND_MASK();
	const uint32 writeMask = regs.CB_TARGET_MASK.get_MASK();

	usesBlendConstants = false;
	for (uint32 rt = 0; rt < (uint32)colorBlendAttachments.size(); rt++)
	{
		auto& att = colorBlendAttachments[rt];
		const auto& blend = regs.CB_BLENDN_CONTROL[rt];
		const bool blendOn = (blendMask >> rt) & 1;
		att.blendEnable = (blendOn && !IsIntegerColorFormat(renderPass->GetColorFormat(rt))) ? VK_TRUE : VK_FALSE;
		att.colorWriteMask = (writeMask >> (rt * 4)) & 0xF;

		att.colorBlendOp = ToVkBlendOp(blend.get_COLOR_COMB_FCN());
		att.srcColorBlendFactor = ToVkBlendFactor(blend.get_COLOR_SRCBLEND());
		att.dstColorBlendFactor = ToVkBlendFactor(blend.get_COLOR_DSTBLEND());
		if (blend.get_SEPARATE_ALPHA_BLEND())
		{
			att.alphaBlendOp = ToVkBlendOp(blend.get_ALPHA_COMB_FCN());
			att.srcAlphaBlendFactor = ToVkBlendFactor(blend.get_ALPHA_SRCBLEND());
			att.dstAlphaBlendFactor = ToVkBlendFactor(blend.get_ALPHA_DSTBLEND());
		}
		else
		{
			att.alphaBlendOp = att.colorBlendOp;
			att.srcAlphaBlendFactor = att.srcColorBlendFactor;
			att.dstAlphaBlendFactor = att.dstColorBlendFactor;
		}
		for (VkBlendFactor f : { att.srcColorBlendFactor, att.dstColorBlendFactor, att.srcAlphaBlendFactor, att.dstAlphaBlendFactor })
			usesBlendConstants |= ReadsBlendConstant(f);
	}

	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = (uint32)colorBlendAttachments.size();
	colorBlending.pAttachments = colorBlendAttachments.data();
	// the blend constants are dynamic state
	for (float& c : colorBlending.blendConstants)
		c = 0.0f;

	using Rop = Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP;
	const Rop rop = colorControl.get_ROP();
	colorBlending.logicOpEnable = rop == Rop::COPY ? VK_FALSE : VK_TRUE;
	switch (rop)
	{
	case Rop::COPY:
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
		break;
	case Rop::SET:
		colorBlending.logicOp = VK_LOGIC_OP_SET;
		break;
	case Rop::CLEAR:
		colorBlending.logicOp = VK_LOGIC_OP_CLEAR;
		break;
	case Rop::OR:
		colorBlending.logicOp = VK_LOGIC_OP_OR;
		break;
	default:
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
		cemu_assert_unimplemented();
	}
}

void PipelineCompiler::SetupDescriptorSetLayouts(VulkanRenderer* renderer, PipelineInfo* pipelineInfo)
{
	VKRObjectPipeline* pipelineObj = pipelineInfo->m_vkrObjPipeline;
	LatteDecompilerShader* vs = pipelineInfo->vertexShader;
	LatteDecompilerShader* ps = pipelineInfo->pixelShader;
	LatteDecompilerShader* gs = pipelineInfo->geometryShader;

	if (vs)
	{
		cemu_assert_debug(descriptorSetLayoutCount == 0);
		pipelineObj->m_vertexDSL = descriptorSetLayout[descriptorSetLayoutCount++] = CreateStageSetLayout(renderer, vs, pipelineInfo);
	}
	if (ps)
	{
		cemu_assert_debug(descriptorSetLayoutCount == 1);
		pipelineObj->m_pixelDSL = descriptorSetLayout[descriptorSetLayoutCount++] = CreateStageSetLayout(renderer, ps, pipelineInfo);
	}
	else if (gs)
	{
		// the geometry set has to stay at index 2, so an empty set fills the pixel slot
		VkDescriptorSetLayoutCreateInfo emptyInfo{};
		emptyInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		if (vkCreateDescriptorSetLayout(renderer->GetLogicalDevice(), &emptyInfo, nullptr, &descriptorSetLayout[descriptorSetLayoutCount]) != VK_SUCCESS)
			renderer->UnrecoverableError(fmt::format("Failed to create placeholder descriptor set layout for shader {0:#x}", gs->baseHash).c_str());
		descriptorSetLayoutCount++;
	}
	if (gs)
	{
		cemu_assert_debug(descriptorSetLayoutCount == 2);
		pipelineObj->m_geometryDSL = descriptorSetLayout[descriptorSetLayoutCount++] = CreateStageSetLayout(renderer, gs, pipelineInfo);
	}
}

void PipelineCompiler::SetupDepthStencil()
{
	// depth and stencil state comes from the live GPU registers
	const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
	const auto& refMaskFront = LatteGPUState.contextNew.DB_STENCILREFMASK;
	const auto& refMaskBack = LatteGPUState.contextNew.DB_STENCILREFMASK_BF;

	depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilState.depthTestEnable = depthControl.get_Z_ENABLE() ? VK_TRUE : VK_FALSE;
	depthStencilState.depthWriteEnable = depthControl.get_Z_WRITE_ENABLE() ? VK_TRUE : VK_FALSE;
	depthStencilState.depthCompareOp = s_compareOps[(size_t)depthControl.get_Z_FUNC()];
	depthStencilState.depthBoundsTestEnable = VK_FALSE;
	depthStencilState.minDepthBounds = 0.0f;
	depthStencilState.maxDepthBounds = 1.0f;
	depthStencilState.stencilTestEnable = depthControl.get_STENCIL_ENABLE() ? VK_TRUE : VK_FALSE;

	VkStencilOpState front{};
	front.failOp = s_stencilOps[(size_t)depthControl.get_STENCIL_FAIL_F()];
	front.passOp = s_stencilOps[(size_t)depthControl.get_STENCIL_ZPASS_F()];
	front.depthFailOp = s_stencilOps[(size_t)depthControl.get_STENCIL_ZFAIL_F()];
	front.compareOp = s_compareOps[(size_t)depthControl.get_STENCIL_FUNC_F()];
	front.compareMask = refMaskFront.get_STENCILMASK_F();
	front.writeMask = refMaskFront.get_STENCILWRITEMASK_F();
	front.reference = refMaskFront.get_STENCILREF_F();
	depthStencilState.front = front;

	// without separate back face stencil both faces use the front settings
	if (!depthControl.get_BACK_STENCIL_ENABLE())
	{
		depthStencilState.back = front;
		return;
	}
	VkStencilOpState back{};
	back.failOp = s_stencilOps[(size_t)depthControl.get_STENCIL_FAIL_B()];
	back.passOp = s_stencilOps[(size_t)depthControl.get_STENCIL_ZPASS_B()];
	back.depthFailOp = s_stencilOps[(size_t)depthControl.get_STENCIL_ZFAIL_B()];
	back.compareOp = s_compareOps[(size_t)depthControl.get_STENCIL_FUNC_B()];
	back.compareMask = refMaskBack.get_STENCILMASK_B();
	back.writeMask = refMaskBack.get_STENCILWRITEMASK_B();
	back.reference = refMaskBack.get_STENCILREF_B();
	depthStencilState.back = back;
}

void PipelineCompiler::SetupDynamicState(PipelineInfo* pipelineInfo, bool usesBlendConstants, bool usesDepthBias)
{
	// viewport and scissor are already in the list
	if (usesBlendConstants)
	{
		dynamicStates.push_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
		pipelineInfo->usesBlendConstants = true;
	}
	if (usesDepthBias)
	{
		dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
		pipelineInfo->usesDepthBias = true;
	}
	if (VulkanRenderer::GetInstance()->UseAttachmentFeedbackLoop())
		dynamicStates.push_back(VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT);

	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = (uint32)dynamicStates.size();
	dynamicState.pDynamicStates = dynamicStates.data();
}

bool PipelineCompiler::InitFromCurrentGPUState(PipelineInfo* pipelineInfo, const LatteContextRegister& latteRegister, VKRObjectRenderPass* renderPassObj, bool requireRobustBufferAccess)
{
	VulkanRenderer* renderer = VulkanRenderer::GetInstance();
	const auto primitiveMode = latteRegister.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	const bool isRect = primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS;

	m_fetchShader = pipelineInfo->fetchShader;
	m_vkVertexShader = pipelineInfo->vertexShaderVk;
	m_vkPixelShader = pipelineInfo->pixelShaderVk;
	m_vkGeometryShader = pipelineInfo->geometryShaderVk;
	m_vkrObjPipeline = pipelineInfo->m_vkrObjPipeline;
	m_renderPassObj = renderPassObj;
	m_requestRobustBufferAccess = requireRobustBufferAccess;

	if (isRect && !renderer->m_featureControl.deviceExtensions.nv_fill_rectangle)
	{
		cemu_assert(m_vkGeometryShader == nullptr); // a game supplied GS together with rects is not handled
		m_rectEmulationGS = BuildRectEmulationGS(pipelineInfo->vertexShader, latteRegister);
		pipelineInfo->rectEmulationGS = m_rectEmulationGS;
	}

	pipelineInfo->primitiveMode = primitiveMode;
	SetupVertexInput(latteRegister, pipelineInfo->vertexShader, pipelineInfo->fetchShader);
	SetupInputAssembly(primitiveMode);
	SetupViewport();
	bool usesDepthBias = false;
	SetupRasterization(latteRegister, renderer, isRect, usesDepthBias);
	bool usesBlendConstants = false;
	SetupColorBlend(latteRegister, usesBlendConstants, renderPassObj);
	SetupDescriptorSetLayouts(renderer, pipelineInfo);

	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = (uint32)descriptorSetLayoutCount;
	layoutInfo.pSetLayouts = descriptorSetLayout;
	VkResult res = vkCreatePipelineLayout(renderer->GetLogicalDevice(), &layoutInfo, nullptr, &m_pipelineLayout);
	if (res != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create pipeline layout: {}", res);
		return false;
	}

	SetupDepthStencil();
	SetupDynamicState(pipelineInfo, usesBlendConstants, usesDepthBias);
	pipelineInfo->m_vkrObjPipeline->m_pipelineLayout = m_pipelineLayout;

	// compilation may happen later on another thread, keep both objects alive until then
	m_vkrObjPipeline->incRef();
	m_renderPassObj->incRef();
	return true;
}

bool PipelineCompiler::Compile(bool forceCompile, bool isRenderThread, bool showInOverlay)
{
	VulkanRenderer* renderer = VulkanRenderer::GetInstance();
	const auto& ext = renderer->m_featureControl.deviceExtensions;

	// a non blocking attempt needs VK_EXT_pipeline_creation_cache_control
	if (!ext.pipeline_creation_cache_control)
		forceCompile = true;

	for (RendererShaderVk* s : { m_vkVertexShader, m_vkPixelShader, m_vkGeometryShader })
	{
		if (!s || s->IsCompiled())
			continue;
		if (!forceCompile)
			return false; // try again once the shaders are ready
		s->PreponeCompilation(isRenderThread);
	}

	if (shaderStages.empty() && !SetupShaderStages(renderer))
		return true; // broken shaders, give up on this pipeline

	VkGraphicsPipelineCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.flags = forceCompile ? 0 : VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
	info.stageCount = (uint32)shaderStages.size();
	info.pStages = shaderStages.data();
	info.pVertexInputState = &vertexInputInfo;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &rasterizer;
	info.pMultisampleState = &multisampling;
	info.pDepthStencilState = &depthStencilState;
	info.pColorBlendState = &colorBlending;
	info.pDynamicState = &dynamicState;
	info.layout = m_pipelineLayout;
	info.renderPass = m_renderPassObj->m_renderPass;
	info.subpass = 0;
	info.basePipelineHandle = VK_NULL_HANDLE;

	// optional structs, each one is put in front of the chain
	const void* chain = nullptr;

	VkPipelineCreationFeedbackEXT feedback{};
	boost::container::static_vector<VkPipelineCreationFeedbackEXT, 3> stageFeedback;
	VkPipelineCreationFeedbackCreateInfoEXT feedbackInfo{};
	if (ext.pipeline_feedback)
	{
		feedback.flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT;
		stageFeedback.assign(info.stageCount, VkPipelineCreationFeedbackEXT{ VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT, 0 });
		feedbackInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT;
		feedbackInfo.pNext = chain;
		feedbackInfo.pPipelineCreationFeedback = &feedback;
		feedbackInfo.pipelineStageCreationFeedbackCount = info.stageCount;
		feedbackInfo.pPipelineStageCreationFeedbacks = stageFeedback.data();
		chain = &feedbackInfo;
	}

	// robust buffer access only for the pipelines that need it; without the extension the device
	// wide robustBufferAccess feature covers it
	VkPipelineRobustnessCreateInfoEXT robustness{};
	if (ext.pipeline_robustness && m_requestRobustBufferAccess)
	{
		robustness.sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT;
		robustness.pNext = chain;
		robustness.storageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
		robustness.uniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
		robustness.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
		robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT;
		chain = &robustness;
	}
	info.pNext = chain;

	// out of device memory is retried up to three times; the driver cache may not be saved meanwhile
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	for (uint32 attempt = 0; attempt < 3 && res == VK_ERROR_OUT_OF_DEVICE_MEMORY; attempt++)
	{
		std::shared_lock cacheLock(renderer->m_pipeline_cache_save_mutex);
		res = vkCreateGraphicsPipelines(renderer->GetLogicalDevice(), renderer->m_pipeline_cache, 1, &info, nullptr, &pipeline);
	}

	if (res == VK_ERROR_PIPELINE_COMPILE_REQUIRED_EXT)
		return false;
	if (res != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create graphics pipeline. Error {}", (sint32)res);
		cemu_assert_debug(false);
		return true; // do not try again
	}
	m_vkrObjPipeline->SetPipeline(pipeline);
	renderer->m_pipeline_cache_semaphore.notify(); // the driver cache has new content to save

	// only pipelines that missed the driver cache count as compiled in the overlay
	if (showInOverlay && ext.pipeline_feedback && HAS_FLAG(feedback.flags, VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) &&
		!HAS_FLAG(feedback.flags, VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT))
	{
		if (isRenderThread)
			g_compiling_pipelines_syncTimeSum += feedback.duration;
		else
			g_compiling_pipelines_async++;
		g_compiling_pipelines++;
	}
	return true;
}

void PipelineCompiler::TrackAsCached(uint64 baseHash, uint64 pipelineStateHash)
{
	auto& stableCache = VulkanPipelineStableCache::GetInstance();
	if (!stableCache.HasPipelineCached(baseHash, pipelineStateHash))
		stableCache.AddCurrentStateToCache(baseHash, pipelineStateHash);
}

bool PipelineCompiler::CalcRobustBufferAccessRequirement(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader)
{
	// out of bounds access can come from streamout into a too small buffer, or from dynamically
	// indexed uniforms (the shader then uses the full constant bank mode)
	bool required = false;
	if (vertexShader)
	{
		cemu_assert_debug(vertexShader->shaderType == LatteConst::ShaderType::Vertex);
		required |= vertexShader->hasStreamoutBufferWrite;
		required |= vertexShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
	}
	if (geometryShader)
	{
		cemu_assert_debug(geometryShader->shaderType == LatteConst::ShaderType::Geometry);
		required |= geometryShader->hasStreamoutBufferWrite;
		required |= geometryShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
	}
	if (pixelShader)
	{
		cemu_assert_debug(pixelShader->shaderType == LatteConst::ShaderType::Pixel);
		required |= pixelShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
	}
	return required;
}

namespace
{
	std::vector<std::thread> s_compilerThreads;
	std::atomic_bool s_compilerStop{};
	ConcurrentQueue<PipelineCompiler*> s_compilerQueue; // nullptr entries only wake threads up

	void CompilerThreadMain(uint32 index)
	{
		SetThreadName("compilePl");
#ifdef _WIN32
		// all but the first thread run below normal so they do not starve the emulation threads;
		// the first one stays normal so compilation always makes progress
		if (index != 0)
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
		while (!s_compilerStop)
		{
			PipelineCompiler* job = s_compilerQueue.pop();
			if (!job)
				continue;
			job->Compile(true, false, true);
			delete job;
		}
	}
}

void PipelineCompiler::CompileThreadPool_Start()
{
	cemu_assert_debug(s_compilerThreads.empty());
	s_compilerStop = false;
	// one thread on up to two cores, otherwise one less than the core count, at most 8
	const uint32 cores = GetPhysicalCoreCount();
	const uint32 threads = std::min(cores <= 2 ? 1u : cores - 1, 8u);
	for (uint32 i = 0; i < threads; i++)
		s_compilerThreads.emplace_back(CompilerThreadMain, i);
}

void PipelineCompiler::CompileThreadPool_Stop()
{
	s_compilerStop = true;
	for (size_t i = 0; i < s_compilerThreads.size(); i++)
		s_compilerQueue.push(nullptr); // wake every thread so it sees the stop flag
	for (std::thread& t : s_compilerThreads)
		t.join();
	// drop requests nobody got to
	while (!s_compilerQueue.empty())
		delete s_compilerQueue.pop();
	s_compilerThreads.clear();
}

void PipelineCompiler::CompileThreadPool_QueueCompilation(PipelineCompiler* v)
{
	s_compilerQueue.push(v);
}
