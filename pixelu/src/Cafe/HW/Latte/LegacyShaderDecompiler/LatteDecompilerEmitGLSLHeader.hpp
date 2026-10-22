// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Declaration section of a generated GLSL shader: preprocessor setup, uniform
// variables/blocks, samplers, vertex attributes and the inter-stage interface.
// Included from LatteDecompilerEmitGLSL.cpp inside its anonymous namespace; all
// functions here are members of GlslShaderWriter.

// Packs the loose uniform variables one after another and records the byte
// offset of each so the renderer can upload them.
class UniformVarPacker
{
public:
	// round the cursor up to a multiple of alignment (power of two) and reserve size bytes
	uint32 place(uint32 alignment, uint32 size)
	{
		m_cursor = (m_cursor + (alignment - 1)) & ~(alignment - 1);
		const uint32 at = m_cursor;
		m_cursor += size;
		return at;
	}

	uint32 end() const { return m_cursor; }

private:
	uint32 m_cursor{ 0 };
};

void GlslShaderWriter::emitDeclarations()
{
	const bool annotate = ActiveSettings::DumpShadersEnabled();
	if (annotate)
		out << "// start of shader inputs/outputs, predetermined by Cemu. Do not touch\r\n";
	emitPreprocessorSetup();
	out << "#ifdef VULKAN\r\n";
	emitUniformVariables(RendererAPI::Vulkan, m_ctx->output->uniformOffsetsVK);
	out << "#else\r\n";
	emitUniformVariables(RendererAPI::OpenGL, m_ctx->output->uniformOffsetsGL);
	out << "#endif\r\n";
	emitUniformBlocks();
	emitSamplers();
	emitVertexAttributes();
	emitStageInterface();
	if (annotate)
		out << "// end of shader inputs/outputs\r\n";
}

void GlslShaderWriter::emitPreprocessorSetup()
{
	const LatteConst::ShaderType type = stage();
	const bool vertexLike = type == LatteConst::ShaderType::Vertex || type == LatteConst::ShaderType::Geometry;
	const bool pixelFragCoord = type == LatteConst::ShaderType::Pixel && facts().hasFragCoordAccess;
	const bool withGS = m_ctx->options->usesGeometryShader;
	const auto* regs = m_ctx->contextRegistersNew;

	// the same source is compiled for both backends, which only differ in how
	// resources get bound
	out << "#ifdef VULKAN\r\n";
	out << "#define ATTR_LAYOUT(__location) layout(location = __location)\r\n";
	out << "#define UNIFORM_BUFFER_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(set = __vkSet, binding = __vkLocation, std140)\r\n";
	out << "#define TEXTURE_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(set = __vkSet, binding = __vkLocation)\r\n";
	if (vertexLike)
	{
		out << "#define gl_VertexID gl_VertexIndex\r\n";
		out << "#define gl_InstanceID gl_InstanceIndex\r\n";
		if (facts().hasStreamoutWrite)
			out << "#define XFB_BLOCK_LAYOUT(__bufferIndex, __stride, __location) layout(location = __location, xfb_buffer = __bufferIndex, xfb_stride = __stride, xfb_offset = 0)\r\n";
		// Vulkan clip space depth is [0,1]; remap from [-1,1] unless the game already uses that convention
		if (regs->PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF())
			out << "#define SET_POSITION(_v) gl_Position = _v\r\n";
		else
			out << "#define SET_POSITION(_v) gl_Position = _v; gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0\r\n";
		if (withGS)
			out << "#define V2G_LAYOUT layout(location = 0)\r\n";
	}
	else if (pixelFragCoord)
	{
		out << "#define GET_FRAGCOORD() vec4(gl_FragCoord.xy*uf_fragCoordScale.xy,gl_FragCoord.z, 1.0/gl_FragCoord.w)\r\n";
	}
	if (m_ctx->options->spirvInstrinsics.hasRoundingModeRTEFloat32)
	{
		// RoundingModeRTE (execution mode 4462) for 16/32/64-bit floats, from SPV_KHR_float_controls
		out << "#extension GL_EXT_spirv_intrinsics: enable\r\n";
		for (uint32 bits : { 16u, 32u, 64u })
			out.format("spirv_execution_mode(capabilities = [4467], extensions = [\"SPV_KHR_float_controls\"], 4462, {});\r\n", bits);
	}
	out << "#else\r\n";
	out << "#define ATTR_LAYOUT(__location) layout(location = __location)\r\n";
	out << "#define UNIFORM_BUFFER_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(binding = __glLocation, std140) \r\n";
	out << "#define TEXTURE_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(binding = __glLocation)\r\n";
	if (vertexLike)
	{
		if (facts().hasStreamoutWrite)
			out << "#define XFB_BLOCK_LAYOUT(__bufferIndex, __stride, __location) layout(xfb_buffer = __bufferIndex, xfb_stride = __stride)\r\n";
		out << "#define SET_POSITION(_v) gl_Position = _v\r\n";
		if (withGS)
			out << "#define V2G_LAYOUT\r\n";
	}
	else if (pixelFragCoord)
	{
		out << "#define GET_FRAGCOORD() vec4(gl_FragCoord.xy*uf_fragCoordScale,gl_FragCoord.zw)\r\n";
	}
	out << "#endif\r\n";

	if (type != LatteConst::ShaderType::Geometry)
		return;

	// The GS input topology is not part of the GS state; it follows the
	// primitive mode of the draw that uses it.
	using PrimMode = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const PrimMode drawMode = regs->VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	std::string_view inputTopology;
	switch (drawMode)
	{
	case PrimMode::POINTS: inputTopology = "points"; break;
	case PrimMode::TRIANGLES: inputTopology = "triangles"; break;
	case PrimMode::LINE_STRIP: inputTopology = "lines_adjacency"; break;
	default:
		debug_printf("drawPrimitiveMode %d\n", drawMode);
		cemu_assert_debug(false);
		break;
	}
	out << "layout(" << inputTopology << ") in;\r\n";

	out << "layout (";
	switch (m_ctx->contextRegisters[mmVGT_GS_OUT_PRIM_TYPE])
	{
	case 0: out << "points"; break;
	case 1: out << "line_strip"; break;
	case 2: out << "triangle_strip"; break;
	default: cemu_assert_debug(false); break;
	}
	out.format(", max_vertices={}) out;\r\n", (sint32)maxGSOutputVertices(true));
}

void GlslShaderWriter::emitUniformVariables(RendererAPI api, LatteDecompilerOutputUniformOffsets& offsets)
{
	const bool vulkan = api == RendererAPI::Vulkan;
	const LatteDecompilerShaderResourceMapping& binding = vulkan ? m_ctx->output->resourceMappingVK : m_ctx->output->resourceMappingGL;
	const LatteConst::ShaderType type = m_shader->shaderType;
	const std::string_view tag = stageTag(type);
	const auto* regs = m_ctx->contextRegistersNew;
	const bool wrapInBlock = vulkan && m_ctx->hasUniformVarBlock;

	// Vulkan has no loose uniforms, they live in a uniform buffer instead
	if (wrapInBlock)
	{
		cemu_assert_debug(binding.uniformVarsBufferBindingPoint >= 0);
		out.format("layout(set = {}, binding = {}) uniform ufBlock\r\n{{\r\n", (sint32)binding.setIndex, (sint32)binding.uniformVarsBufferBindingPoint);
	}

	UniformVarPacker pack;
	switch (m_shader->uniformMode)
	{
	case LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED:
	{
		// only the statically addressed constants, compacted into one array
		const uint32 entries = (uint32)m_shader->list_remappedUniformEntries.size();
		out.format("uniform ivec4 uf_remapped{}[{}];\r\n", tag, (sint32)entries);
		offsets.offset_remapped = pack.place(1, 16 * entries);
		break;
	}
	case LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE:
	{
		// (a prefix of) the whole constant register file
		const uint32 regCount = facts().uniformRegisterAccessTracker.DetermineSize(m_ctx->shaderBaseHash, 256);
		out.format("uniform ivec4 uf_uniformRegister{}[{}];\r\n", tag, regCount);
		offsets.offset_uniformRegister = pack.place(1, 16 * regCount);
		offsets.count_uniformRegister = regCount;
		break;
	}
	default:
		break;
	}

	// emulation state that the GPU applies in fixed function hardware
	const bool viewportScalePartlyOff =
		!regs->PA_CL_VTE_CNTL.get_VPORT_X_SCALE_ENA() ||
		!regs->PA_CL_VTE_CNTL.get_VPORT_Y_SCALE_ENA() ||
		!regs->PA_CL_VTE_CNTL.get_VPORT_Z_SCALE_ENA();
	if (stage() == LatteConst::ShaderType::Vertex && viewportScalePartlyOff)
	{
		out << "uniform vec2 uf_windowSpaceToClipSpaceTransform;\r\n";
		offsets.offset_windowSpaceToClipSpaceTransform = pack.place(8, 8);
	}
	if (stage() == LatteConst::ShaderType::Pixel && regs->SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE())
	{
		out << "uniform float uf_alphaTestRef;\r\n";
		offsets.offset_alphaTestRef = pack.place(4, 4);
	}
	if (facts().outputPointSize && !facts().writesPointSize)
	{
		const bool lastGeometryStage = (stage() == LatteConst::ShaderType::Vertex && !m_ctx->options->usesGeometryShader) || stage() == LatteConst::ShaderType::Geometry;
		if (lastGeometryStage)
		{
			out << "uniform float uf_pointSize;\r\n";
			offsets.offset_pointSize = pack.place(4, 4);
		}
	}

	// Render target vs. effective resolution scale. It is left out when
	// unused, but its slot is kept (as padding) whenever something follows it
	// so that the layout matches what existing shader replacements expect.
	bool owesFragCoordSlot = false;
	if (type == LatteConst::ShaderType::Pixel)
	{
		if (!facts().hasFragCoordAccess)
			owesFragCoordSlot = true;
		else if (vulkan)
		{
			out << "uniform vec4 uf_fragCoordScale;\r\n"; // zw hold the origin
			offsets.offset_fragCoordScale = pack.place(16, 16);
		}
		else
		{
			out << "uniform vec2 uf_fragCoordScale;\r\n";
			offsets.offset_fragCoordScale = pack.place(8, 8);
		}
	}
	// scale factors for textures read with texel coordinates
	for (sint32 unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; unit++)
	{
		if (!facts().texUnitUsesTexelCoordinates.test(unit))
			continue;
		if (owesFragCoordSlot)
		{
			if (vulkan)
			{
				out << "uniform vec4 uf_fragCoordScaleCompatPadding;\r\n";
				pack.place(16, 16);
			}
			else
			{
				out << "uniform vec2 uf_fragCoordScaleCompatPadding;\r\n";
				pack.place(8, 8);
			}
			owesFragCoordSlot = false;
		}
		out.format("uniform vec2 uf_tex{}Scale;\r\n", unit);
		offsets.offset_texScale[unit] = pack.place(8, 8);
	}

	// Streamout through a storage buffer needs the vertex numbering and the
	// buffer bases. Note: the condition groups as (A && B) || C, so a
	// geometry shader always declares these.
	const bool ssboStreamoutVS = facts().useSSBOForStreamout && type == LatteConst::ShaderType::Vertex && !m_ctx->options->usesGeometryShader;
	if (ssboStreamoutVS || type == LatteConst::ShaderType::Geometry)
	{
		out << "uniform int uf_verticesPerInstance;\r\n";
		offsets.offset_verticesPerInstance = pack.place(1, 4);
		for (uint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
		{
			if (!m_ctx->output->streamoutBufferWriteMask[b])
				continue;
			out.format("uniform int uf_streamoutBufferBase{};\r\n", b);
			offsets.offset_streamoutBufferBase[b] = pack.place(1, 4);
		}
	}

	offsets.offset_endOfBlock = pack.end();
	if (wrapInBlock)
		out << "};\r\n";
}

void GlslShaderWriter::emitUniformBlocks()
{
	// the other uniform modes are declared with the uniform variables
	if (m_shader->uniformMode != LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
		return;
	const std::string_view tag = stageTag(m_shader->shaderType);
	const auto& gl = m_ctx->output->resourceMappingGL;
	const auto& vk = m_ctx->output->resourceMappingVK;
	for (uint32 bank = 0; bank < LATTE_NUM_MAX_UNIFORM_BUFFERS; bank++)
	{
		auto& tracker = facts().uniformBufferAccessTracker[bank];
		if (!tracker.HasAccess())
			continue;
		cemu_assert_debug(gl.uniformBuffersBindingPoint[bank] >= 0);
		cemu_assert_debug(vk.uniformBuffersBindingPoint[bank] >= 0);
		out.format("UNIFORM_BUFFER_LAYOUT({}, {}, {}) ", (sint32)gl.uniformBuffersBindingPoint[bank], (sint32)vk.setIndex, (sint32)vk.uniformBuffersBindingPoint[bank]);
		out.format("uniform uniformBlock{}{}\r\n{{\r\n", tag, bank);
		out.format("vec4 uf_block{}{}[{}];\r\n", tag, bank, tracker.DetermineSize(m_ctx->shaderBaseHash, LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE));
		out << "};\r\n\r\n\r\n";
	}
}

void GlslShaderWriter::emitSamplers()
{
	const auto& gl = m_ctx->output->resourceMappingGL;
	const auto& vk = m_ctx->output->resourceMappingVK;
	for (sint32 unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; unit++)
	{
		if (!m_ctx->output->textureUnitMask[unit])
			continue;
		out.format("TEXTURE_LAYOUT({}, {}, {}) ", (sint32)gl.textureUnitToBindingPoint[unit], (sint32)vk.setIndex, (sint32)vk.textureUnitToBindingPoint[unit]);
		out << "uniform ";
		const Latte::E_DIM dim = m_shader->textureUnitDim[unit];
		std::string_view samplerType;
		if (m_shader->textureIsIntegerFormat[unit])
		{
			if (dim == Latte::E_DIM::DIM_1D)
				samplerType = "usampler1D";
			else if (dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_MSAA)
				samplerType = "usampler2D";
			else
				cemu_assert_unimplemented();
		}
		else
		{
			switch (dim)
			{
			case Latte::E_DIM::DIM_1D: samplerType = "sampler1D"; break;
			case Latte::E_DIM::DIM_2D:
			case Latte::E_DIM::DIM_2D_MSAA: samplerType = "sampler2D"; break;
			case Latte::E_DIM::DIM_2D_ARRAY: samplerType = "sampler2DArray"; break;
			case Latte::E_DIM::DIM_CUBEMAP: samplerType = "samplerCubeArray"; break; // cube faces are addressed as layers
			case Latte::E_DIM::DIM_3D: samplerType = "sampler3D"; break;
			default: cemu_assert_unimplemented(); break;
			}
		}
		out << samplerType;
		if (m_shader->textureUsesDepthCompare[unit])
			out << "Shadow";
		out << " " << textureName(unit) << ";\r\n";
	}
}

void GlslShaderWriter::emitVertexAttributes()
{
	if (m_shader->shaderType != LatteConst::ShaderType::Vertex)
		return;
	const auto& gl = m_ctx->output->resourceMappingGL;
	const auto& vk = m_ctx->output->resourceMappingVK;
	for (uint32 sem = 0; sem < LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS; sem++)
	{
		if (!facts().inputAttributSemanticMask[sem])
			continue;
		cemu_assert_debug(vk.attributeMapping[sem] >= 0);
		cemu_assert_debug(gl.attributeMapping[sem] == vk.attributeMapping[sem]);
		// raw 32-bit words, decoded in main()
		out.format("ATTR_LAYOUT({}) in uvec4 attrDataSem{};\r\n", (sint32)vk.attributeMapping[sem], sem);
	}
}

// a varying shared with the pixel stage, qualified like the matching PS input
static void writeVarying(GlslText& out, const LatteShaderPSInputTable::psImport_t& input, uint32 location, std::string_view direction, std::string_view name, sint32 nameIndex)
{
	out.format("layout(location = {}) ", location);
	if (input.isFlat)
		out << "flat ";
	if (input.isNoPerspective)
		out << "noperspective ";
	out << direction;
	out.format(" vec4 {}{};\r\n", name, nameIndex);
}

void GlslShaderWriter::emitVertexOutputs()
{
	// a VS export is only declared if the pixel shader consumes its semantic;
	// the location is the index of that PS input
	const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
	const uint32 exportMask = m_shader->outputParameterMask;
	for (sint32 param = 0; param < 32; param++)
	{
		if (((exportMask >> param) & 1) == 0)
			continue;
		const uint32 sem = (uint32)_getVertexShaderOutParamSemanticId(m_ctx->contextRegisters, param);
		if (sem > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			continue;
		for (sint32 slot = 0; slot < inputs->count; slot++)
		{
			if (inputs->import[slot].semanticId != sem)
				continue;
			writeVarying(out, inputs->import[slot], (uint32)slot, "out", "passParameterSem", (sint32)inputs->import[slot].semanticId);
			break;
		}
	}
}

void GlslShaderWriter::emitPixelInputs()
{
	const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
	for (sint32 slot = 0; slot < inputs->count; slot++)
	{
		const auto& input = inputs->import[slot];
		if (input.semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			continue;
		writeVarying(out, input, (uint32)slot, "in", "passParameterSem", (sint32)input.semanticId);
	}
}

void GlslShaderWriter::emitStageInterface()
{
	const LatteConst::ShaderType type = stage();
	const bool isVS = type == LatteConst::ShaderType::Vertex;
	const bool isGS = type == LatteConst::ShaderType::Geometry;
	const bool isPS = type == LatteConst::ShaderType::Pixel;
	const bool withGS = m_ctx->options->usesGeometryShader;

	// the stage that feeds the rasterizer redeclares the position block
	if ((isVS && !withGS) || isGS)
	{
		out << "out gl_PerVertex\r\n{\r\n\tvec4 gl_Position;\r\n";
		if (facts().outputPointSize)
			out << "\tfloat gl_PointSize;\r\n";
		out << "};\r\n";
	}

	if (!withGS)
	{
		if (isVS)
			emitVertexOutputs();
		else if (isPS)
			emitPixelInputs();
	}
	else
	{
		// VS -> GS: the ring buffer items, passed as raw integer vectors
		if (isVS || isGS)
		{
			out << "V2G_LAYOUT " << (isVS ? "out Vertex\r\n" : "in Vertex\r\n") << "{\r\n";
			const uint32 ringParams = isVS ? m_shader->ringParameterCount : m_shader->ringParameterCountFromPrevStage;
			for (uint32 p = 0; p < ringParams; p++)
				out.format(" ivec4 passV2GParameter{};\r\n", p);
			out << (isVS ? "}v2g;\r\n" : "}v2g[];\r\n");
		}
		// GS -> PS: named after the export parameter of the GS copy shader
		if (isGS)
		{
			cemu_assert_debug(((m_ctx->contextRegisters[mmSQ_GSVS_RING_ITEMSIZE] & 0x7FFF) & 0xF) == 0);
			const auto* copyShader = m_ctx->parsedGSCopyShader;
			for (sint32 p = 0; p < copyShader->numParam; p++)
			{
				const auto& mapping = copyShader->paramMapping[p];
				if (mapping.exportType != 2)
					continue;
				out.format("layout(location = {}) out vec4 passG2PParameter{};\r\n", mapping.exportParam & 0x7F, (sint32)mapping.exportParam);
			}
		}
		else if (isPS)
		{
			const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
			for (sint32 slot = 0; slot < inputs->count; slot++)
			{
				const auto& input = inputs->import[slot];
				if (input.semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
					continue;
				const uint32 location = input.semanticId & 0x7F;
				writeVarying(out, input, location, "in", "passG2PParameter", (sint32)location);
			}
		}
	}

	if (isPS)
	{
		for (uint32 target = 0; target < LATTE_NUM_COLOR_TARGET; target++)
		{
			if ((m_shader->pixelColorOutputMask >> target) & 1)
				out.format("layout(location = {0}) out vec4 passPixelColor{0};\r\n", target);
		}
	}

	// transform feedback
	if ((isVS || isGS) && facts().hasStreamoutEnable)
	{
		if (m_ctx->options->useTFViaSSBO)
		{
			if (facts().useSSBOForStreamout && facts().hasStreamoutWrite)
			{
				auto& vk = m_ctx->output->resourceMappingVK;
				out.format("layout(set = {}, binding = {}) buffer StreamoutBuffer\r\n", (sint32)vk.setIndex, vk.getTFStorageBufferBindingPoint());
				out << "{\r\nint sb_buffer[];\r\n};\r\n";
			}
		}
		else
		{
			// glslang requires explicit locations on xfb outputs, so they are packed consecutively
			uint32 nextLocation = 0;
			for (uint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
			{
				if (!m_ctx->output->streamoutBufferWriteMask[b])
					continue;
				const uint32 stride = m_ctx->output->streamoutBufferStride[b];
				out.format("XFB_BLOCK_LAYOUT({0}, {1}, {2}) out XfbBlock{0} \r\n{{\r\n", b, stride, nextLocation);
				out.format("layout(xfb_buffer = {0}, xfb_offset = 0) int sb{0}[{1}];\r\n", b, stride / 4);
				out << "};\r\n";
				nextLocation += stride / 4;
			}
		}
	}
}
