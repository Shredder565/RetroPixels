// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Common/MemPtr.h"
#include "HW/Latte/ISA/LatteReg.h"
#ifdef ENABLE_METAL
#include "HW/Latte/Renderer/Metal/MetalCommon.h"
#include "HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif

// Analysis pass of the legacy decompiler. It walks the decoded program once
// and collects everything the code generator has to know up front: which
// registers, constants, textures and outputs are touched, how deep the
// execution mask stack gets, and which binding slots each backend uses.

// implemented with the texture cache
Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N& texUnitWord1, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N& texUnitWord4);

// The pixel shader writes its color exports densely (export 0, 1, ...) while
// CB_SHADER_MASK enables attachments sparsely; export n goes to the n-th
// enabled attachment.
sint32 LatteDecompiler_getColorOutputIndexFromExportIndex(LatteDecompilerShaderContext* shaderContext, sint32 exportIndex)
{
	const uint32 shaderMask = shaderContext->contextRegisters[mmCB_SHADER_MASK];
	sint32 enabledSeen = 0;
	for (sint32 target = 0; target < LATTE_NUM_COLOR_TARGET; target++)
	{
		const uint32 channels = (shaderMask >> (target * 4)) & 0xF;
		if (channels == 0)
			continue;
		cemu_assert_debug(channels == 0xF); // partial channel masks are not handled
		if (enabledSeen == exportIndex)
			return target;
		enabledSeen++;
	}
	cemu_assert_debug(false); // shader exports more colors than the mask enables
	return -1;
}

namespace
{
	bool isALUClause(uint32 type)
	{
		return type == GPU7_CF_INST_ALU || type == GPU7_CF_INST_ALU_PUSH_BEFORE || type == GPU7_CF_INST_ALU_POP_AFTER ||
			type == GPU7_CF_INST_ALU_POP2_AFTER || type == GPU7_CF_INST_ALU_BREAK || type == GPU7_CF_INST_ALU_ELSE_AFTER;
	}

	void markGPRUsed(LatteDecompilerShaderContext* ctx, sint64 index)
	{
		if (index < 0 || index >= LATTE_NUM_GPR)
			return;
		ctx->analyzer.gprUseMask[index >> 3] |= (uint8)(1u << (index & 7));
	}

	// statically addressed constants get a slot in the compact uniform array
	void addRemappedConstant(LatteDecompilerShaderContext* ctx, bool isRegister, uint32 bufferSlot, uint32 index)
	{
		auto& entries = ctx->shader->list_remappedUniformEntries;
		for (const auto& e : entries)
		{
			if (e.isRegister != isRegister || e.index != index)
				continue;
			if (isRegister || e.kcacheBankId == bufferSlot)
				return;
		}
		LatteDecompilerRemappedUniformEntry_t e{};
		e.isRegister = isRegister;
		e.kcacheBankId = isRegister ? 0 : (uint8)bufferSlot;
		e.index = index;
		e.mappedIndex = (uint32)entries.size();
		entries.push_back(e);
	}

	// true for operations whose inputs or result are integers. Any of these
	// makes the code generator store registers as ints.
	bool usesIntegerData(const LatteDecompilerALUInstruction& ins)
	{
		if (ins.isOP3)
		{
			switch (ins.opcode)
			{
			case ALU_OP3_INST_CNDE_INT:
			case ALU_OP3_INST_CNDGT_INT:
			case ALU_OP3_INST_CMOVGE_INT:
				return true;
			default:
				return false;
			}
		}
		switch (ins.opcode)
		{
		// integer arithmetic, logic and shifts
		case ALU_OP2_INST_AND_INT:
		case ALU_OP2_INST_OR_INT:
		case ALU_OP2_INST_XOR_INT:
		case ALU_OP2_INST_NOT_INT:
		case ALU_OP2_INST_ADD_INT:
		case ALU_OP2_INST_SUB_INT:
		case ALU_OP2_INST_MAX_INT:
		case ALU_OP2_INST_MIN_INT:
		case ALU_OP2_INST_MAX_UINT:
		case ALU_OP2_INST_MIN_UINT:
		case ALU_OP2_INST_ASHR_INT:
		case ALU_OP2_INST_LSHR_INT:
		case ALU_OP2_INST_LSHL_INT:
		case ALU_OP2_INST_MULLO_INT:
		case ALU_OP2_INST_MULLO_UINT:
		// integer compares, predicates and kills
		case ALU_OP2_INST_SETE_INT:
		case ALU_OP2_INST_SETGT_INT:
		case ALU_OP2_INST_SETGE_INT:
		case ALU_OP2_INST_SETNE_INT:
		case ALU_OP2_INST_SETGT_UINT:
		case ALU_OP2_INST_SETGE_UINT:
		case ALU_OP2_INST_PRED_SETE_INT:
		case ALU_OP2_INST_PRED_SETGT_INT:
		case ALU_OP2_INST_PRED_SETGE_INT:
		case ALU_OP2_INST_PRED_SETNE_INT:
		case ALU_OP2_INST_KILLE_INT:
		case ALU_OP2_INST_KILLGT_INT:
		case ALU_OP2_INST_KILLNE_INT:
		// conversions
		case ALU_OP2_INST_FLT_TO_INT:
		case ALU_OP2_INST_FLT_TO_UINT:
		case ALU_OP2_INST_INT_TO_FLOAT:
		case ALU_OP2_INST_UINT_TO_FLOAT:
		// address register loads
		case ALU_OP2_INST_MOVA_FLOOR:
		case ALU_OP2_INST_MOVA_INT:
		// float compares with an integer mask result
		case ALU_OP2_INST_SETE_DX10:
		case ALU_OP2_INST_SETGT_DX10:
		case ALU_OP2_INST_SETGE_DX10:
		case ALU_OP2_INST_SETNE_DX10:
			return true;
		default:
			return false;
		}
	}

	void trackConstantBufferRead(LatteDecompilerShaderContext* ctx, uint32 slot, uint32 offset, bool relative)
	{
		cemu_assert(slot < LATTE_NUM_MAX_UNIFORM_BUFFERS);
		addRemappedConstant(ctx, false, slot, offset);
		ctx->analyzer.uniformBufferAccessTracker[slot].TrackAccess((sint32)offset, relative);
	}

	void scanALUClause(LatteDecompilerShaderContext* ctx, LatteDecompilerCFInstruction* cf)
	{
		auto& a = ctx->analyzer;
		if (cf->type != GPU7_CF_INST_ALU)
			a.modifiesPixelActiveState = true; // every other ALU clause type pushes/pops/breaks
		for (auto& ins : cf->instructionsALU)
		{
			if (!ins.isOP3 && ins.opcode == ALU_OP2_INST_NOP)
				continue;
			if (!ins.isOP3 && ins.opcode == ALU_OP2_INST_CUBE)
				a.hasRedcCUBE = true;
			if (usesIntegerData(ins))
				a.usesIntegerValues = true;
			for (const auto& src : ins.sourceOperand)
			{
				const uint32 sel = src.sel;
				if (sel == 0xFFFFFFFF)
					continue;
				const bool rel = src.rel != 0;
				if (GPU7_ALU_SRC_IS_CFILE(sel))
				{
					const uint32 reg = GPU7_ALU_SRC_GET_CFILE_INDEX(sel);
					// relative reads can reach any register, so they are not remapped
					if (!rel)
						addRemappedConstant(ctx, true, 0, reg);
					a.uniformRegisterAccessTracker.TrackAccess((sint32)reg, rel);
				}
				else if (GPU7_ALU_SRC_IS_CBANK0(sel))
					trackConstantBufferRead(ctx, cf->cBank0Index, GPU7_ALU_SRC_GET_CBANK0_INDEX(sel) + cf->cBank0AddrBase, rel);
				else if (GPU7_ALU_SRC_IS_CBANK1(sel))
					trackConstantBufferRead(ctx, cf->cBank1Index, GPU7_ALU_SRC_GET_CBANK1_INDEX(sel) + cf->cBank1AddrBase, rel);
				else if (GPU7_ALU_SRC_IS_GPR(sel))
				{
					markGPRUsed(ctx, GPU7_ALU_SRC_GET_GPR_INDEX(sel));
					if (rel)
						a.usesRelativeGPRRead = true;
				}
			}
			if (ins.destRel != 0)
				a.usesRelativeGPRWrite = true;
			markGPRUsed(ctx, ins.destGpr);
		}
	}

	bool isSampleOpcode(uint32 op)
	{
		switch (op)
		{
		case GPU7_TEX_INST_SAMPLE:
		case GPU7_TEX_INST_SAMPLE_L:
		case GPU7_TEX_INST_SAMPLE_LB:
		case GPU7_TEX_INST_SAMPLE_LZ:
		case GPU7_TEX_INST_SAMPLE_C:
		case GPU7_TEX_INST_SAMPLE_C_L:
		case GPU7_TEX_INST_SAMPLE_C_LZ:
		case GPU7_TEX_INST_FETCH4:
		case GPU7_TEX_INST_SAMPLE_G:
		case GPU7_TEX_INST_LD:
			return true;
		default:
			return false;
		}
	}

	void scanTEXClause(LatteDecompilerShaderContext* ctx, LatteDecompilerCFInstruction* cf)
	{
		LatteDecompilerShader* shader = ctx->shader;
		auto& a = ctx->analyzer;
		for (auto& t : cf->instructionsTEX)
		{
			const sint32 unit = t.textureFetch.textureIndex;
			if (isSampleOpcode(t.opcode))
			{
				if (unit < 0 || unit >= LATTE_NUM_MAX_TEX_UNITS)
				{
					cemuLog_logDebug(LogType::Force, "Shader {:16x} has out of bounds texture access (texture {})", shader->baseHash, unit);
					continue;
				}
				cemu_assert_debug(t.textureFetch.samplerIndex >= 0 && t.textureFetch.samplerIndex < 0x12);
				cemu_assert_debug(!ctx->output->textureUnitMask[unit] || shader->textureUnitSamplerAssignment[unit] == LATTE_DECOMPILER_SAMPLER_NONE || shader->textureUnitSamplerAssignment[unit] == t.textureFetch.samplerIndex);
				ctx->output->textureUnitMask[unit] = true;
				shader->textureUnitSamplerAssignment[unit] = (uint16)t.textureFetch.samplerIndex;
				if (t.opcode == GPU7_TEX_INST_SAMPLE_C || t.opcode == GPU7_TEX_INST_SAMPLE_C_L || t.opcode == GPU7_TEX_INST_SAMPLE_C_LZ)
					shader->textureUsesDepthCompare[unit] = true;
				// texelFetch() paths need the texture size scale uniform
				const auto& un = t.textureFetch.unnormalized;
				const bool allTexelCoords = un[0] && un[1] && un[2] && un[3];
				if (t.opcode == GPU7_TEX_INST_LD || (t.opcode == GPU7_TEX_INST_SAMPLE && allTexelCoords))
					a.texUnitUsesTexelCoordinates.set(unit);
			}
			else if (t.opcode == GPU7_TEX_INST_GET_COMP_TEX_LOD || t.opcode == GPU7_TEX_INST_GET_TEXTURE_RESINFO)
			{
				if (unit >= 0 && unit < LATTE_NUM_MAX_TEX_UNITS)
					ctx->output->textureUnitMask[unit] = true;
			}
			else if (t.opcode == GPU7_TEX_INST_SET_GRADIENTS_H || t.opcode == GPU7_TEX_INST_SET_GRADIENTS_V)
				a.hasGradientLookup = true;
			else if (t.opcode == GPU7_TEX_INST_VFETCH)
			{
				// a vertex fetch from buffer ids 0x80+ reads a constant buffer with a computed index
				if (unit >= 0x80 && unit < 0x80 + LATTE_NUM_MAX_UNIFORM_BUFFERS)
					a.uniformBufferAccessTracker[unit - 0x80].TrackAccess(0, true);
			}
			else if (t.opcode == GPU7_TEX_INST_MEM)
				a.hasSSBORead = true;
			markGPRUsed(ctx, t.dstGpr);
			markGPRUsed(ctx, t.srcGpr);
		}
	}

	void scanExport(LatteDecompilerShaderContext* ctx, LatteDecompilerCFInstruction* cf)
	{
		LatteDecompilerShader* shader = ctx->shader;
		const uint32 slots = cf->exportBurstCount + 1;
		if (shader->shaderType == LatteConst::ShaderType::Pixel && cf->exportType == 0)
		{
			if (cf->exportArrayBase < 8)
			{
				for (uint32 i = 0; i < slots; i++)
				{
					sint32 target = LatteDecompiler_getColorOutputIndexFromExportIndex(ctx, (sint32)(cf->exportArrayBase + i));
					if (target >= 0)
						shader->pixelColorOutputMask |= (1u << target);
				}
			}
			else if (cf->exportArrayBase == 61)
			{
				// depth export; Metal additionally requires a bound depth buffer (part of its pipeline key)
				if (g_renderer->GetType() != RendererAPI::Metal || LatteMRT::GetActiveDepthBufferMask(*ctx->contextRegistersNew))
					shader->depthMask = true;
			}
		}
		else if (shader->shaderType == LatteConst::ShaderType::Vertex)
		{
			if (cf->exportType == 2 && cf->exportArrayBase < 32)
			{
				for (uint32 i = 0; i < slots; i++)
				{
					const uint32 param = cf->exportArrayBase + i;
					if (param >= 32)
						break;
					shader->outputParameterMask |= (1u << param);
				}
			}
			else if (cf->exportType == 1 && cf->exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_POINT_SIZE)
				ctx->analyzer.writesPointSize = true;
		}
		for (uint32 i = 0; i < slots; i++)
			markGPRUsed(ctx, (sint64)cf->exportSourceGPR + i);
	}

	// highest component written by a memory export, as byte count
	uint32 vectorBytesFromMask(uint32 compMask)
	{
		uint32 bytes = 0;
		for (uint32 c = 0; c < 4; c++)
		{
			if (compMask & (1u << c))
				bytes = (c + 1) * 4;
		}
		return bytes;
	}

	void scanControlFlow(LatteDecompilerShaderContext* ctx)
	{
		auto& a = ctx->analyzer;
		LatteDecompilerShader* shader = ctx->shader;
		for (auto& cf : ctx->cfInstructions)
		{
			const uint32 type = cf.type;
			if (isALUClause(type))
				scanALUClause(ctx, &cf);
			else if (type == GPU7_CF_INST_TEX)
				scanTEXClause(ctx, &cf);
			else if (type == GPU7_CF_INST_EXPORT || type == GPU7_CF_INST_EXPORT_DONE)
				scanExport(ctx, &cf);
			else if (type == GPU7_CF_INST_ELSE || type == GPU7_CF_INST_POP)
				a.modifiesPixelActiveState = true;
			else if (type == GPU7_CF_INST_LOOP_START_DX10 || type == GPU7_CF_INST_LOOP_START_NO_AL || type == GPU7_CF_INST_LOOP_END || type == GPU7_CF_INST_LOOP_BREAK)
			{
				a.modifiesPixelActiveState = true;
				a.hasLoops = true;
			}
			else if (type == GPU7_CF_INST_MEM_STREAM0_WRITE || type == GPU7_CF_INST_MEM_STREAM1_WRITE)
			{
				const uint32 buffer = type == GPU7_CF_INST_MEM_STREAM0_WRITE ? 0 : 1;
				a.hasStreamoutWrite = true;
				ctx->output->streamoutBufferWriteMask[buffer] = true;
				// strides of all streamout buffers come from the registers
				for (uint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
					ctx->output->streamoutBufferStride[b] = ctx->contextRegisters[mmVGT_STRMOUT_VTX_STRIDE_0 + b * 4] << 2;
				cemu_assert_debug(cf.exportArrayBase * 4 + vectorBytesFromMask(cf.memWriteCompMask) <= ctx->output->streamoutBufferStride[buffer]);
			}
			else if (type == GPU7_CF_INST_MEM_RING_WRITE)
			{
				cemu_assert_debug(cf.memWriteElemSize == 3);
				// ring parameters are vec4; remember the furthest one written
				const uint32 dwordsWritten = (cf.exportBurstCount + 1) * 4;
				shader->ringParameterCount = std::max(shader->ringParameterCount, (cf.exportArrayBase + dwordsWritten) / 4);
				for (uint32 i = 0; i <= cf.exportBurstCount; i++)
					markGPRUsed(ctx, (sint64)cf.exportSourceGPR + i);
			}
			else if (type == GPU7_CF_INST_EMIT_VERTEX)
				a.numEmitVertex++;
		}
	}

	// execution mask stack depth before/at each CF instruction of the main program
	void computeStackDepths(LatteDecompilerShaderContext* ctx)
	{
		sint32 depth = 0;
		sint32 maxDepth = 0;
		for (auto& cf : ctx->cfInstructions)
		{
			switch (cf.type)
			{
			case GPU7_CF_INST_ALU_PUSH_BEFORE:
				depth++;
				maxDepth = std::max(maxDepth, depth);
				cf.activeStackDepth = depth;
				break;
			case GPU7_CF_INST_ALU_POP_AFTER:
				cf.activeStackDepth = depth;
				depth -= 1;
				break;
			case GPU7_CF_INST_ALU_POP2_AFTER:
				cf.activeStackDepth = depth;
				depth -= 2;
				break;
			case GPU7_CF_INST_POP:
				cf.activeStackDepth = depth;
				depth -= (sint32)cf.popCount;
				cemu_assert_debug(depth >= 0);
				break;
			case GPU7_CF_INST_ALU:
			case GPU7_CF_INST_ALU_BREAK:
			case GPU7_CF_INST_ALU_ELSE_AFTER:
			case GPU7_CF_INST_ELSE:
			case GPU7_CF_INST_LOOP_START_DX10:
			case GPU7_CF_INST_LOOP_START_NO_AL:
			case GPU7_CF_INST_LOOP_END:
			case GPU7_CF_INST_LOOP_BREAK: // treated as not taken, so no stack change
			case GPU7_CF_INST_TEX:
			case GPU7_CF_INST_EXPORT:
			case GPU7_CF_INST_EXPORT_DONE:
			case GPU7_CF_INST_MEM_STREAM0_WRITE:
			case GPU7_CF_INST_MEM_STREAM1_WRITE:
			case GPU7_CF_INST_MEM_RING_WRITE:
			case GPU7_CF_INST_EMIT_VERTEX:
			case GPU7_CF_INST_CALL:
				cf.activeStackDepth = depth;
				break;
			default:
				break;
			}
		}
		ctx->analyzer.activeStackMaxDepth = maxDepth;
		cemu_assert_debug(depth == 0);
	}

	void chooseUniformMode(LatteDecompilerShaderContext* ctx)
	{
		const auto& a = ctx->analyzer;
		bool anyBufferRead = false;
		bool anyBufferIndexed = false;
		for (const auto& t : a.uniformBufferAccessTracker)
		{
			anyBufferRead = anyBufferRead || t.HasAccess();
			anyBufferIndexed = anyBufferIndexed || t.HasRelativeAccess();
		}
		uint8 mode;
		if (anyBufferIndexed)
			mode = LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
		else if (a.uniformRegisterAccessTracker.HasRelativeAccess())
			mode = LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE;
		else if (anyBufferRead || a.uniformRegisterAccessTracker.HasAccess())
			mode = LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED;
		else
			mode = LATTE_DECOMPILER_UNIFORM_MODE_NONE;
		ctx->shader->uniformMode = mode;
		// buffers the renderer has to bind, with the byte range actually read
		cemu_assert_debug(ctx->shader->list_quickBufferList.empty());
		for (uint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
		{
			if (!a.uniformBufferAccessTracker[i].HasAccess())
				continue;
			LatteDecompilerShader::QuickBufferEntry q;
			q.index = i;
			q.size = a.uniformBufferAccessTracker[i].DetermineSize(ctx->shaderBaseHash, LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE) * 16;
			ctx->shader->list_quickBufferList.push_back(q);
		}
	}

	_LatteRegisterSetTextureUnit* stageTextureRegisters(LatteDecompilerShaderContext* ctx)
	{
		switch (ctx->shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex: return ctx->contextRegistersNew->SQ_TEX_START_VS;
		case LatteConst::ShaderType::Pixel: return ctx->contextRegistersNew->SQ_TEX_START_PS;
		case LatteConst::ShaderType::Geometry: return ctx->contextRegistersNew->SQ_TEX_START_GS;
		default: return nullptr;
		}
	}

	void collectTextureInfo(LatteDecompilerShaderContext* ctx)
	{
		LatteDecompilerShader* shader = ctx->shader;
		_LatteRegisterSetTextureUnit* units = stageTextureRegisters(ctx);
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			if (!ctx->output->textureUnitMask[i])
			{
				shader->textureUnitDim[i] = (Latte::E_DIM)0xFF;
				continue;
			}
			const Latte::E_DIM dim = units[i].word0.get_DIM();
			shader->textureUnitDim[i] = dim;
			if (dim == Latte::E_DIM::DIM_CUBEMAP)
				ctx->analyzer.hasCubeMapTexture = true;
			shader->textureIsIntegerFormat[i] = units[i].word4.get_NUM_FORM_ALL() == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_INT;
		}
		shader->textureUnitListCount = 0;
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			if (ctx->output->textureUnitMask[i])
				shader->textureUnitList[shader->textureUnitListCount++] = (uint8)i;
			shader->textureRenderTargetIndex[i] = 255;
		}
		if (shader->shaderType != LatteConst::ShaderType::Pixel)
			return;
		// detect textures that alias a bound color buffer (feedback loops)
		struct BoundTarget
		{
			sint32 index;
			MPTR address;
			Latte::E_GX2SURFFMT format;
			Latte::E_HWTILEMODE tileMode;
		};
		BoundTarget targets[LATTE_NUM_COLOR_TARGET]{};
		sint32 numTargets = 0;
		const uint8 enabledTargets = LatteMRT::GetActiveColorBufferMask(shader, *ctx->contextRegistersNew);
		for (sint32 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
		{
			if ((enabledTargets & (1 << i)) == 0)
				continue;
			const uint32 base = ctx->contextRegisters[mmCB_COLOR0_BASE + i] & 0xFFFFFF00;
			const uint32 info = ctx->contextRegisters[mmCB_COLOR0_INFO + i];
			targets[numTargets++] = { i, base, LatteMRT::GetColorBufferFormat(i, *ctx->contextRegistersNew), (Latte::E_HWTILEMODE)((info >> 8) & 0xF) };
		}
		for (sint32 n = 0; n < shader->textureUnitListCount; n++)
		{
			const sint32 unit = shader->textureUnitList[n];
			const auto& regs = units[unit];
			const MPTR address = regs.word2.get_BASE_ADDRESS() << 8;
			if (address == MPTR_NULL)
				continue;
			// only single-level 2D textures are matched
			if (shader->textureUnitDim[unit] != Latte::E_DIM::DIM_2D || regs.word5.get_LAST_LEVEL() != 0)
				continue;
			const auto tileMode = regs.word0.get_TILE_MODE();
			const Latte::E_GX2SURFFMT format = LatteTexture_ReconstructGX2Format(regs.word1, regs.word4);
			for (sint32 t = 0; t < numTargets; t++)
			{
				if (targets[t].address == address && targets[t].format == format && targets[t].tileMode == tileMode)
				{
					shader->textureRenderTargetIndex[unit] = (uint8)targets[t].index;
					break;
				}
			}
		}
	}

	void collectCopyShaderStreamout(LatteDecompilerShaderContext* ctx)
	{
		if (ctx->shader->shaderType != LatteConst::ShaderType::Geometry)
			return;
		const auto& writes = ctx->parsedGSCopyShader->list_streamWrites;
		if (writes.empty())
			return;
		ctx->analyzer.hasStreamoutWrite = true;
		if (ctx->contextRegisters[mmVGT_STRMOUT_EN] != 0)
			ctx->analyzer.hasStreamoutEnable = true;
		for (const auto& w : writes)
		{
			ctx->output->streamoutBufferWriteMask[w.bufferIndex] = true;
			uint32& stride = ctx->output->streamoutBufferStride[w.bufferIndex];
			stride = std::max(stride, w.exportArrayBase * 4 + vectorBytesFromMask(w.memWriteCompMask));
		}
	}

	// GPR number an attribute is loaded into (R0 holds vertex/instance id), or -1
	sint32 attributeRegister(LatteDecompilerShaderContext* ctx, uint32 semanticId)
	{
		for (sint32 slot = 0; slot < 32; slot++)
		{
			if (ctx->contextRegisters[mmSQ_VTX_SEMANTIC_0 + slot] == semanticId)
				return slot + 1;
		}
		return -1;
	}

	// shader inputs are loaded into GPRs at startup; with indexed reads every one may be used
	void markInputsForRelativeReads(LatteDecompilerShaderContext* ctx)
	{
		if (!ctx->analyzer.usesRelativeGPRRead)
			return;
		const auto type = ctx->shader->shaderType;
		if (type == LatteConst::ShaderType::Vertex || type == LatteConst::ShaderType::Geometry)
		{
			if (!ctx->fetchShader)
				return;
			for (const auto& group : ctx->fetchShader->bufferGroups)
			{
				for (sint32 i = 0; i < group.attribCount; i++)
				{
					sint32 reg = attributeRegister(ctx, group.attrib[i].semanticId);
					if (reg >= 0)
						markGPRUsed(ctx, reg);
				}
			}
		}
		else if (type == LatteConst::ShaderType::Pixel)
		{
			const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
			for (sint32 i = 0; i < inputs->count; i++)
				markGPRUsed(ctx, i);
		}
	}

	bool isGPRUsed(LatteDecompilerShaderContext* ctx, sint32 index)
	{
		return (ctx->analyzer.gprUseMask[index >> 3] & (1u << (index & 7))) != 0;
	}

	void detectFragCoordUse(LatteDecompilerShaderContext* ctx)
	{
		if (ctx->shader->shaderType != LatteConst::ShaderType::Pixel)
			return;
		const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
		for (sint32 i = 0; i < inputs->count; i++)
		{
			if (!isGPRUsed(ctx, i) && !ctx->analyzer.usesRelativeGPRRead)
				continue;
			if (inputs->import[i].semanticId == LATTE_ANALYZER_IMPORT_INDEX_SPIPOSITION)
			{
				ctx->analyzer.hasFragCoordAccess = true;
				break;
			}
		}
		// graphic pack replacements for these shaders expect uf_fragCoordScale to exist
		static const uint64 kNeedsFragCoordScale[] = {
			0x21e6bc9b0cdbe8d7, 0x37040a485a29d54e, 0x37a4ec1a7dbc7391, 0x50e29e8929cea348, 0x572a6cfa3943923d,
			0x59df1c7e1806366c, 0x6ea8b1aa69c0b6f7, 0x88133ee405eaae28, 0x95a5a89d62998e0d, 0x998a9f67e353657b,
			0x9f6adb9a651f84b9, 0xa5e9d150276a805c, 0xa7f4801a8d29e333, 0xbe99d80628d31127, 0xc14019840473ff86,
			0xc612390d4c70f430, 0xcb0e6e8cbec4502a, 0xe334517825fdd599, 0xe39a2a718bc419fe, 0xfdf33c607cd1d737,
			0xff71dcd2ad4defdc,
		};
		for (uint64 h : kNeedsFragCoordScale)
		{
			if (h == ctx->shaderBaseHash)
				ctx->analyzer.hasFragCoordAccess = true;
		}
	}

	// ---- binding point assignment ----

	void assignTextureBindings(LatteDecompilerShaderContext* ctx)
	{
		LatteDecompilerOutput_t* out = ctx->output;
		// OpenGL: fixed per-stage ranges, unit numbers kept as-is
		sint8 glBase = 0;
		switch (ctx->shaderType)
		{
		case LatteConst::ShaderType::Vertex: glBase = LATTE_CEMU_VS_TEX_UNIT_BASE; break;
		case LatteConst::ShaderType::Geometry: glBase = LATTE_CEMU_GS_TEX_UNIT_BASE; break;
		case LatteConst::ShaderType::Pixel: glBase = LATTE_CEMU_PS_TEX_UNIT_BASE; break;
		default: break;
		}
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			if (out->textureUnitMask[i])
				out->resourceMappingGL.textureUnitToBindingPoint[i] = (sint8)(glBase + i);
		}
		// Vulkan: packed descriptor bindings
		auto& vk = out->resourceMappingVK;
		vk.textureUnitBaseBindingPoint = (sint8)ctx->currentBindingPointVK;
		sint32 packed = 0;
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			if (!out->textureUnitMask[i])
				continue;
			vk.textureUnitToBindingPoint[i] = (sint8)ctx->currentBindingPointVK++;
			vk.relBindingPointToRelTextureUnit[packed++] = (sint8)i;
			vk.textureUnitCount++;
		}
		if (packed == 0)
			vk.textureUnitBaseBindingPoint = -1;
#ifdef ENABLE_METAL
		// Metal: packed as well, textures read through framebuffer fetch get no binding
		auto& mtl = out->resourceMappingMTL;
		mtl.textureUnitBaseBindingPoint = (sint8)ctx->currentTextureBindingPointMTL;
		sint32 packedMtl = 0;
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			if (!out->textureUnitMask[i] || ctx->shader->textureRenderTargetIndex[i] != 255)
				continue;
			mtl.textureUnitToBindingPoint[i] = (sint8)ctx->currentTextureBindingPointMTL++;
			mtl.relBindingPointToRelTextureUnit[packedMtl++] = (sint8)i;
			mtl.textureUnitCount++;
		}
		if (packedMtl == 0)
			mtl.textureUnitBaseBindingPoint = -1;
#endif
	}

	bool anyViewportScaleDisabled(LatteDecompilerShaderContext* ctx)
	{
		const auto& vte = ctx->contextRegistersNew->PA_CL_VTE_CNTL;
		return !vte.get_VPORT_X_SCALE_ENA() || !vte.get_VPORT_Y_SCALE_ENA() || !vte.get_VPORT_Z_SCALE_ENA();
	}

	// whether the shader declares the block of helper uniforms
	bool needsUniformVarBlock(LatteDecompilerShaderContext* ctx)
	{
		const auto& a = ctx->analyzer;
		const auto type = ctx->shaderType;
		const bool isVS = type == LatteConst::ShaderType::Vertex;
		const bool isPS = type == LatteConst::ShaderType::Pixel;
		const bool isGS = type == LatteConst::ShaderType::Geometry;
		const uint8 mode = ctx->shader->uniformMode;
		bool needed = mode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED || mode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE;
		if (isVS && anyViewportScaleDisabled(ctx))
			needed = true; // uf_windowSpaceToClipSpaceTransform
		if (isPS && ctx->contextRegistersNew->SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE())
			needed = true; // uf_alphaTestRef
		if (isPS && a.hasFragCoordAccess)
			needed = true; // uf_fragCoordScale
		if ((isVS || isGS) && a.outputPointSize && !a.writesPointSize)
			needed = true; // uf_pointSize
		// note: geometry shaders always get the block here, matching the
		// historical binding layout that existing pipelines/caches rely on
		if ((a.useSSBOForStreamout && isVS && !ctx->options->usesGeometryShader) || isGS)
			needed = true; // uf_verticesPerInstance, uf_streamoutBufferBase*
#ifdef ENABLE_METAL
		if (g_renderer->GetType() == RendererAPI::Metal)
		{
			if (static_cast<MetalRenderer*>(g_renderer.get())->SupportsFramebufferFetch())
			{
				for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
				{
					if (ctx->shader->textureRenderTargetIndex[t] != 255)
						needed = true; // framebufferFetchSize
				}
			}
			if (isVS && UseGeometryShader(*ctx->contextRegistersNew, ctx->options->usesGeometryShader))
				needed = true; // uf_verticesPerInstance
		}
#endif
		if (isPS && a.texUnitUsesTexelCoordinates.any())
			needed = true; // uf_tex*Scale
		return needed;
	}

	void assignUniformBindings(LatteDecompilerShaderContext* ctx)
	{
		LatteDecompilerOutput_t* out = ctx->output;
		ctx->hasUniformVarBlock = needsUniformVarBlock(ctx);
		if (ctx->hasUniformVarBlock)
		{
			out->resourceMappingVK.uniformVarsBufferBindingPoint = (sint8)ctx->currentBindingPointVK++;
			out->resourceMappingMTL.uniformVarsBufferBindingPoint = (sint8)ctx->currentBufferBindingPointMTL++;
		}
		else if (ctx->shaderType == LatteConst::ShaderType::Pixel)
		{
			// pixel shaders always reserve the slot so bindings stay stable for
			// graphic pack replacement shaders written before the block became optional
			ctx->currentBindingPointVK++;
			ctx->currentBufferBindingPointMTL++;
		}
		if (ctx->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
		{
			sint32 glStageBase = 0;
			if (ctx->shaderType == LatteConst::ShaderType::Pixel)
				glStageBase = 32;
			else if (ctx->shaderType == LatteConst::ShaderType::Geometry)
				glStageBase = 64;
			for (uint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
			{
				if (!ctx->analyzer.uniformBufferAccessTracker[i].HasAccess())
					continue;
				out->resourceMappingVK.uniformBuffersBindingPoint[i] = (sint8)ctx->currentBindingPointVK++;
				out->resourceMappingMTL.uniformBuffersBindingPoint[i] = (sint8)ctx->currentBufferBindingPointMTL++;
				out->resourceMappingGL.uniformBuffersBindingPoint[i] = (sint8)(glStageBase + (sint32)i);
			}
		}
		if (ctx->analyzer.useSSBOForStreamout)
		{
			out->resourceMappingVK.tfStorageBindingPoint = (sint8)ctx->currentBindingPointVK++;
			out->resourceMappingMTL.tfStorageBindingPoint = (sint8)ctx->currentBufferBindingPointMTL++;
		}
	}

	void assignAttributeBindings(LatteDecompilerShaderContext* ctx)
	{
		if (ctx->shaderType != LatteConst::ShaderType::Vertex)
			return;
		sint8 location = 0;
		for (sint32 sem = 0; sem < LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS; sem++)
		{
			if (!ctx->analyzer.inputAttributSemanticMask[sem])
				continue;
			ctx->output->resourceMappingGL.attributeMapping[sem] = location;
			ctx->output->resourceMappingVK.attributeMapping[sem] = location;
			ctx->output->resourceMappingMTL.attributeMapping[sem] = location;
			location++;
		}
	}
}

void LatteDecompiler_analyze(LatteDecompilerShaderContext* shaderContext, LatteDecompilerShader* shader)
{
	auto* ctx = shaderContext;
	auto& a = ctx->analyzer;
	const auto type = shader->shaderType;
	// render state that influences the generated code
	a.isPointsPrimitive = ctx->contextRegistersNew->VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE() == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::POINTS;
	a.hasStreamoutEnable = ctx->contextRegisters[mmVGT_STRMOUT_EN] != 0;
	if (ctx->shaderType == LatteConst::ShaderType::Vertex && !ctx->options->usesGeometryShader)
		a.outputPointSize = a.isPointsPrimitive;
	else if (ctx->shaderType == LatteConst::ShaderType::Geometry && ctx->contextRegisters[mmVGT_GS_OUT_PRIM_TYPE] == 0)
		a.outputPointSize = true; // point output
	// attributes provided by the fetch shader
	if ((type == LatteConst::ShaderType::Vertex || type == LatteConst::ShaderType::Geometry) && ctx->fetchShader)
	{
		for (const auto& group : ctx->fetchShader->bufferGroups)
		{
			for (sint32 i = 0; i < group.attribCount; i++)
			{
				const uint8 sem = group.attrib[i].semanticId;
				if (sem == 0xFF)
					continue; // unused slot (seen in some titles)
				cemu_assert_debug(sem < 0x80);
				a.inputAttributSemanticMask[sem] = true;
			}
		}
	}
	// note: subroutine bodies are not scanned, only the main program
	scanControlFlow(ctx);
	chooseUniformMode(ctx);
	collectTextureInfo(ctx);
	collectCopyShaderStreamout(ctx);
	markInputsForRelativeReads(ctx);
	detectFragCoordUse(ctx);
	computeStackDepths(ctx);
	if (ctx->options->useTFViaSSBO && ctx->output->streamoutBufferWriteMask.any())
		a.useSSBOForStreamout = true;
	// binding points
	if (ctx->shaderType == LatteConst::ShaderType::Vertex)
		ctx->output->resourceMappingVK.setIndex = 0;
	else if (ctx->shaderType == LatteConst::ShaderType::Pixel)
		ctx->output->resourceMappingVK.setIndex = 1;
	else if (ctx->shaderType == LatteConst::ShaderType::Geometry)
		ctx->output->resourceMappingVK.setIndex = 2;
	assignTextureBindings(ctx);
	assignUniformBindings(ctx);
	assignAttributeBindings(ctx);
	auto& mtl = ctx->output->resourceMappingMTL;
	mtl.verticesPerInstanceBinding = (sint8)ctx->currentBufferBindingPointMTL++;
	mtl.indexBufferBinding = (sint8)ctx->currentBufferBindingPointMTL++;
	mtl.indexTypeBinding = (sint8)ctx->currentBufferBindingPointMTL++;
}
