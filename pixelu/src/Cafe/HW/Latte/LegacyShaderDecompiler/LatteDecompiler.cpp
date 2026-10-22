// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

// Front end of the legacy decompiler: decodes the control flow program and
// its ALU/TEX clauses into LatteDecompilerCFInstruction lists, then drives the
// analyzer and the code generator.
//
// Encoding reference (R600/R700 ISA):
//  CF word1 bits 23..29 hold the opcode for non-ALU entries (values < 0x40).
//  Larger values mean an ALU clause, whose opcode lives in bits 26..29.

namespace
{
	// bounds-checked little endian dword reader over the program blob
	struct ProgramView
	{
		const uint8* data;
		uint32 size; // bytes

		uint32 dword(uint64 dwordIndex) const
		{
			uint64 byteOffset = dwordIndex * 4;
			if (byteOffset + 4 <= size)
			{
				uint32 v;
				memcpy(&v, data + byteOffset, 4);
				return v;
			}
			// past the end: the hardware would read whatever follows, we read zeros
			uint32 v = 0;
			for (uint32 i = 0; i < 4; i++)
			{
				if (byteOffset + i < size)
					v |= (uint32)data[byteOffset + i] << (i * 8);
			}
			return v;
		}
	};

	inline uint32 bits(uint32 v, uint32 shift, uint32 width)
	{
		return (v >> shift) & ((1u << width) - 1u);
	}

	inline sint8 signExtend(uint32 v, uint32 width)
	{
		uint32 signBit = 1u << (width - 1);
		return (sint8)(sint32)((v ^ signBit) - signBit);
	}

	bool isALUClauseType(uint32 type)
	{
		switch (type)
		{
		case GPU7_CF_INST_ALU:
		case GPU7_CF_INST_ALU_PUSH_BEFORE:
		case GPU7_CF_INST_ALU_POP_AFTER:
		case GPU7_CF_INST_ALU_POP2_AFTER:
		case GPU7_CF_INST_ALU_BREAK:
		case GPU7_CF_INST_ALU_ELSE_AFTER:
			return true;
		default:
			return false;
		}
	}

	void rememberSubroutine(LatteDecompilerShaderContext* ctx, uint32 target)
	{
		for (const auto& s : ctx->list_subroutines)
		{
			if (s.cfAddr == target)
				return;
		}
		LatteDecompilerSubroutineInfo info{};
		info.cfAddr = target;
		ctx->list_subroutines.push_back(std::move(info));
	}

	enum class CFDecodeResult
	{
		Appended, // a new entry was added to the list
		Ignored, // valid instruction without effect on code generation
		Invalid, // unsupported encoding, shader marked as broken
	};

	// decodes one 64-bit control flow word pair
	CFDecodeResult decodeCFInstruction(LatteDecompilerShaderContext* ctx, uint32 cfIndex, uint32 w0, uint32 w1, bool& endOfProgram, std::vector<LatteDecompilerCFInstruction>& list)
	{
		uint32 op = bits(w1, 23, 7);
		if (op >= 0x40)
		{
			// ALU clause
			uint32 aluType = bits(w1, 26, 4) | GPU7_CF_INST_ALU_MASK;
			if (!isALUClauseType(aluType))
			{
				ctx->shader->hasError = true;
				return CFDecodeResult::Invalid;
			}
			LatteDecompilerCFInstruction& cf = list.emplace_back();
			cf.type = aluType;
			cf.cfAddr = cfIndex;
			cf.cfCond = 0;
			cf.addr = bits(w0, 0, 22);
			cf.count = (sint32)bits(w1, 18, 7) + 1;
			cf.cBank0Index = bits(w0, 22, 4);
			cf.cBank1Index = bits(w0, 26, 4);
			cf.cBank0AddrBase = bits(w1, 2, 8) * 16;
			cf.cBank1AddrBase = bits(w1, 10, 8) * 16;
			return CFDecodeResult::Appended;
		}

		endOfProgram = bits(w1, 21, 1) != 0;
		const uint32 cond = bits(w1, 8, 2);
		const uint32 pop = bits(w1, 0, 3);
		const uint32 count = (bits(w1, 10, 3) | (bits(w1, 19, 1) << 3)) + 1;

		auto append = [&]() -> LatteDecompilerCFInstruction& {
			LatteDecompilerCFInstruction& cf = list.emplace_back();
			cf.type = op;
			cf.cfAddr = cfIndex;
			return cf;
		};

		switch (op)
		{
		case GPU7_CF_INST_NOP:
		case GPU7_CF_INST_JUMP: // flow is reconstructed from the execution mask instructions
		case GPU7_CF_INST_CALL_FS: // fetch shader is inlined separately
			return CFDecodeResult::Ignored;
		case GPU7_CF_INST_EXPORT:
		case GPU7_CF_INST_EXPORT_DONE:
		{
			LatteDecompilerCFInstruction& cf = append();
			cf.cfCond = cond;
			for (uint32 i = 0; i < 4; i++)
				cf.exportComponentSel[i] = (uint8)bits(w1, i * 3, 3);
			cf.exportArrayBase = bits(w0, 0, 13);
			cf.exportType = bits(w0, 13, 2);
			cf.exportSourceGPR = bits(w0, 15, 7);
			cf.exportBurstCount = bits(w1, 17, 4);
			return CFDecodeResult::Appended;
		}
		case GPU7_CF_INST_TEX:
		{
			LatteDecompilerCFInstruction& cf = append();
			cf.cfCond = cond;
			cf.addr = w0;
			cf.count = (sint32)count;
			return CFDecodeResult::Appended;
		}
		case GPU7_CF_INST_ELSE:
		case GPU7_CF_INST_POP:
		case GPU7_CF_INST_LOOP_START_DX10:
		case GPU7_CF_INST_LOOP_START_NO_AL:
		case GPU7_CF_INST_LOOP_END:
		case GPU7_CF_INST_LOOP_BREAK:
		{
			LatteDecompilerCFInstruction& cf = append();
			cf.cfCond = cond;
			cf.popCount = pop;
			cf.addr = w0;
			cf.count = (sint32)count;
			return CFDecodeResult::Appended;
		}
		case GPU7_CF_INST_MEM_STREAM0_WRITE:
		case GPU7_CF_INST_MEM_STREAM1_WRITE:
		case GPU7_CF_INST_MEM_RING_WRITE:
		{
			LatteDecompilerCFInstruction& cf = append();
			cf.exportArrayBase = bits(w0, 0, 13);
			cf.exportSourceGPR = bits(w0, 15, 7);
			cf.memWriteArraySize = bits(w1, 0, 12);
			cf.memWriteCompMask = (uint8)bits(w1, 12, 4);
			if (op == GPU7_CF_INST_MEM_RING_WRITE)
			{
				cf.memWriteElemSize = (uint8)bits(w0, 30, 2);
				cf.exportBurstCount = bits(w1, 17, 4);
			}
			return CFDecodeResult::Appended;
		}
		case GPU7_CF_INST_EMIT_VERTEX:
			append();
			return CFDecodeResult::Appended;
		case GPU7_CF_INST_CALL:
		{
			LatteDecompilerCFInstruction& cf = append();
			cf.addr = w0; // CF index of the subroutine
			cf.count = (sint32)bits(w1, 13, 6); // call count
			rememberSubroutine(ctx, w0);
			return CFDecodeResult::Appended;
		}
		case GPU7_CF_INST_RETURN:
		{
			LatteDecompilerCFInstruction& cf = append();
			cf.cfCond = cond;
			cf.popCount = pop;
			return CFDecodeResult::Appended;
		}
		default:
			ctx->shader->hasError = true;
			return CFDecodeResult::Invalid;
		}
	}

	void decodeControlFlow(LatteDecompilerShaderContext* ctx, const ProgramView& prog)
	{
		const uint32 numSlots = prog.size / 8;
		// main program runs until the end-of-program bit
		for (uint32 i = 0; i < numSlots; i++)
		{
			bool eop = false;
			decodeCFInstruction(ctx, i, prog.dword(i * 2), prog.dword(i * 2 + 1), eop, ctx->cfInstructions);
			if (eop)
				break;
		}
		// subroutines run from their entry to the first RETURN. Only the
		// subroutines called from the main program are decoded; calls nested
		// inside subroutines are not supported by the code generator.
		const size_t numSubroutines = ctx->list_subroutines.size();
		for (size_t s = 0; s < numSubroutines; s++)
		{
			LatteDecompilerSubroutineInfo& sub = ctx->list_subroutines[s];
			for (uint32 i = sub.cfAddr; i < numSlots; i++)
			{
				bool eop = false;
				CFDecodeResult r = decodeCFInstruction(ctx, i, prog.dword(i * 2), prog.dword(i * 2 + 1), eop, sub.instructions);
				if (r == CFDecodeResult::Invalid)
					continue;
				cemu_assert_debug(!eop);
				if (ctx->shader->hasError)
					break;
				if (!sub.instructions.empty() && sub.instructions.back().type == GPU7_CF_INST_RETURN)
					break;
			}
		}
	}

	// ops that can only execute on the scalar (trans) unit
	bool isTransOnlyOp2(uint32 opcode)
	{
		switch (opcode)
		{
		case ALU_OP2_INST_SIN:
		case ALU_OP2_INST_COS:
		case ALU_OP2_INST_EXP_IEEE:
		case ALU_OP2_INST_LOG_CLAMPED:
		case ALU_OP2_INST_LOG_IEEE:
		case ALU_OP2_INST_SQRT_IEEE:
		case ALU_OP2_INST_RECIP_CLAMPED:
		case ALU_OP2_INST_RECIP_FF:
		case ALU_OP2_INST_RECIP_IEEE:
		case ALU_OP2_INST_RECIPSQRT_CLAMPED:
		case ALU_OP2_INST_RECIPSQRT_FF:
		case ALU_OP2_INST_RECIPSQRT_IEEE:
		case ALU_OP2_INST_MULLO_INT:
		case ALU_OP2_INST_MULLO_UINT:
		case ALU_OP2_INST_FLT_TO_INT:
		case ALU_OP2_INST_FLT_TO_UINT:
		case ALU_OP2_INST_INT_TO_FLOAT:
		case ALU_OP2_INST_UINT_TO_FLOAT:
			return true;
		default:
			return false;
		}
	}

	void decodeSourceOperand(LatteDecompilerALUInstruction& ins, int slot, uint32 word, uint32 shift)
	{
		auto& op = ins.sourceOperand[slot];
		op.sel = bits(word, shift, 9);
		op.rel = (uint8)bits(word, shift + 9, 1);
		op.chan = (uint8)bits(word, shift + 10, 2);
		op.neg = (uint8)bits(word, shift + 12, 1);
		op.abs = 0;
	}

	void decodeALUClause(LatteDecompilerCFInstruction* cf, const ProgramView& prog)
	{
		uint32 groupIndex = 0;
		uint8 slotInGroup = 0;
		uint32 unitsTaken = 0; // x,y,z,w,t occupancy of the current group
		uint32 literalChannels = 0; // literal components referenced by the current group
		size_t groupStart = cf->instructionsALU.size();
		sint32 pos = 0;
		while (pos < cf->count)
		{
			const uint64 dw = ((uint64)cf->addr + (uint32)pos) * 2;
			const uint32 w0 = prog.dword(dw);
			const uint32 w1 = prog.dword(dw + 1);
			pos++;

			LatteDecompilerALUInstruction ins{};
			ins.cfInstruction = cf;
			ins.instructionGroupIndex = groupIndex;
			ins.indexMode = (uint8)bits(w0, 26, 3);
			decodeSourceOperand(ins, 0, w0, 0);
			decodeSourceOperand(ins, 1, w0, 13);
			ins.destGpr = bits(w1, 21, 7);
			ins.destRel = (uint8)bits(w1, 28, 1);
			ins.destElem = (uint8)bits(w1, 29, 2);
			ins.destClamp = (uint8)bits(w1, 31, 1);

			uint32 unit = ins.destElem;
			const uint32 op3 = bits(w1, 13, 5);
			if (op3 >= 8)
			{
				ins.isOP3 = true;
				ins.opcode = op3;
				decodeSourceOperand(ins, 2, w1, 0);
				ins.writeMask = 1;
				ins.omod = 0;
			}
			else
			{
				ins.isOP3 = false;
				ins.opcode = bits(w1, 7, 11);
				ins.sourceOperand[0].abs = (uint8)bits(w1, 0, 1);
				ins.sourceOperand[1].abs = (uint8)bits(w1, 1, 1);
				ins.updateExecuteMask = (uint8)bits(w1, 2, 1);
				ins.updatePredicate = (uint8)bits(w1, 3, 1);
				ins.writeMask = (uint8)bits(w1, 4, 1);
				ins.omod = (uint8)bits(w1, 5, 2);
				ins.sourceOperand[2].sel = 0xFFFFFFFF; // no third operand
				if (isTransOnlyOp2(ins.opcode))
					unit = 4;
			}
			const int numSrc = ins.isOP3 ? 3 : 2;
			for (int s = 0; s < numSrc; s++)
			{
				if (GPU7_ALU_SRC_IS_LITERAL(ins.sourceOperand[s].sel))
					literalChannels |= 1u << ins.sourceOperand[s].chan;
			}
			// a vector slot that is already occupied means this one runs on trans
			if (unit < 4 && (unitsTaken & (1u << unit)) != 0)
				unit = 4;
			unitsTaken |= 1u << unit;
			ins.aluUnit = (uint8)unit;
			ins.indexInGroup = slotInGroup++;
			ins.isLastInstructionOfGroup = bits(w0, 31, 1) != 0;
			cf->instructionsALU.push_back(ins);

			if (!ins.isLastInstructionOfGroup)
				continue;
			// literal constants follow the group, two per 64-bit slot
			if (literalChannels != 0)
			{
				uint32 lit[4] = {};
				const uint64 litDw = ((uint64)cf->addr + (uint32)pos) * 2;
				const bool needZW = (literalChannels & 0xC) != 0;
				lit[0] = prog.dword(litDw);
				lit[1] = prog.dword(litDw + 1);
				if (needZW)
				{
					lit[2] = prog.dword(litDw + 2);
					lit[3] = prog.dword(litDw + 3);
				}
				pos += needZW ? 2 : 1;
				for (size_t i = groupStart; i < cf->instructionsALU.size(); i++)
				{
					for (int c = 0; c < 4; c++)
						cf->instructionsALU[i].literalData.w[c] = lit[c];
				}
			}
			groupIndex++;
			slotInGroup = 0;
			unitsTaken = 0;
			literalChannels = 0;
			groupStart = cf->instructionsALU.size();
		}
	}

	// fields shared by the TEX_WORD* sample-style encodings
	void decodeSampleCommon(LatteDecompilerTEXInstruction& t, uint32 w0, uint32 w1, uint32 w2, bool withDest, bool withResource)
	{
		t.srcGpr = (sint32)bits(w0, 16, 7);
		if (withResource)
		{
			t.textureFetch.textureIndex = (sint32)bits(w0, 8, 8);
			t.textureFetch.samplerIndex = (sint32)bits(w2, 15, 5);
		}
		if (withDest)
		{
			t.dstGpr = (sint32)bits(w1, 0, 7);
			for (int i = 0; i < 4; i++)
				t.dstSel[i] = (sint8)bits(w1, 9 + i * 3, 3);
		}
		for (int i = 0; i < 4; i++)
			t.textureFetch.srcSel[i] = (sint8)bits(w2, 20 + i * 3, 3);
	}

	void decodeTEXClause(LatteDecompilerShader* shader, LatteDecompilerCFInstruction* cf, const ProgramView& prog)
	{
		for (sint32 i = 0; i < cf->count; i++)
		{
			const uint64 dw = (uint64)cf->addr * 2 + (uint64)i * 4;
			const uint32 w0 = prog.dword(dw);
			const uint32 w1 = prog.dword(dw + 1);
			const uint32 w2 = prog.dword(dw + 2);
			const uint32 op = bits(w0, 0, 5);

			LatteDecompilerTEXInstruction t{};
			t.cfInstruction = cf;
			t.opcode = op;
			switch (op)
			{
			case GPU7_TEX_INST_SAMPLE:
			case GPU7_TEX_INST_SAMPLE_L:
			case GPU7_TEX_INST_SAMPLE_LB:
			case GPU7_TEX_INST_SAMPLE_LZ:
			case GPU7_TEX_INST_SAMPLE_G:
			case GPU7_TEX_INST_SAMPLE_C:
			case GPU7_TEX_INST_SAMPLE_C_L:
			case GPU7_TEX_INST_SAMPLE_C_LZ:
			case GPU7_TEX_INST_FETCH4:
			case GPU7_TEX_INST_LD:
			case GPU7_TEX_INST_GET_TEXTURE_RESINFO:
			case GPU7_TEX_INST_GET_COMP_TEX_LOD:
				decodeSampleCommon(t, w0, w1, w2, true, true);
				for (int c = 0; c < 4; c++)
					t.textureFetch.unnormalized[c] = bits(w1, 28 + c, 1) == GPU7_TEX_UNNORMALIZED;
				t.textureFetch.offsetX = signExtend(bits(w2, 0, 5), 5);
				t.textureFetch.offsetY = signExtend(bits(w2, 5, 5), 5);
				t.textureFetch.offsetZ = signExtend(bits(w2, 10, 5), 5);
				t.textureFetch.lodBias = signExtend(bits(w2, 21, 7), 7);
				break;
			case GPU7_TEX_INST_SET_CUBEMAP_INDEX:
				decodeSampleCommon(t, w0, w1, w2, true, true);
				break;
			case GPU7_TEX_INST_GET_GRADIENTS_H:
			case GPU7_TEX_INST_GET_GRADIENTS_V:
				decodeSampleCommon(t, w0, w1, w2, true, false);
				break;
			case GPU7_TEX_INST_SET_GRADIENTS_H:
			case GPU7_TEX_INST_SET_GRADIENTS_V:
				decodeSampleCommon(t, w0, w1, w2, false, true);
				t.dstGpr = -1; // writes the gradient registers, no GPR
				break;
			case GPU7_TEX_INST_VFETCH:
				// VTX_WORD* encoding
				t.textureFetch.textureIndex = (sint32)bits(w0, 8, 8); // buffer id
				t.textureFetch.samplerIndex = 0;
				t.textureFetch.offset = bits(w2, 0, 16);
				t.srcGpr = (sint32)bits(w0, 16, 7);
				t.dstGpr = (sint32)bits(w1, 0, 7);
				for (int c = 0; c < 4; c++)
					t.dstSel[c] = (sint8)bits(w1, 9 + c * 3, 3);
				t.textureFetch.srcSel[0] = (sint8)bits(w0, 24, 2);
				break;
			case GPU7_TEX_INST_MEM:
			{
				// MEM_RD_WORD* encoding; only scatter buffer reads are supported
				if (bits(w0, 8, 3) != 2)
				{
					cemu_assert_unimplemented();
					continue;
				}
				cemu_assert_debug(bits(w0, 23, 1) == 0 || bits(w1, 7, 1) == 0);
				t.srcGpr = (sint32)bits(w0, 16, 7);
				t.memRead.srcSelX = (sint8)bits(w0, 24, 2);
				t.dstGpr = (sint32)bits(w1, 0, 7);
				for (int c = 0; c < 4; c++)
					t.dstSel[c] = (sint8)bits(w1, 9 + c * 3, 3);
				t.memRead.format = (Latte::E_HWFMT)bits(w1, 22, 6);
				t.memRead.nfa = (uint8)bits(w1, 28, 2);
				t.memRead.isSigned = (uint8)bits(w1, 30, 1);
				t.memRead.arrayBase = bits(w2, 0, 13);
				break;
			}
			default:
				cemuLog_logDebug(LogType::Force, "Unsupported tex instruction {}", op);
				shader->hasError = true;
				return;
			}
			cf->instructionsTEX.push_back(t);
		}
	}

	void decodeClauses(LatteDecompilerShaderContext* ctx, const ProgramView& prog, std::vector<LatteDecompilerCFInstruction>& list)
	{
		for (auto& cf : list)
		{
			if (isALUClauseType(cf.type))
				decodeALUClause(&cf, prog);
			else if (cf.type == GPU7_CF_INST_TEX)
				decodeTEXClause(ctx->shader, &cf, prog);
		}
	}

	// byte offsets of the remapped constants, grouped by constant buffer
	void buildFastAccessTables(LatteDecompilerShader* shader)
	{
		if (shader->hasError)
			return;
		for (const auto& e : shader->list_remappedUniformEntries)
		{
			if (e.isRegister)
			{
				LatteFastAccessRemappedUniformEntry_register_t r;
				r.indexOffset = e.index * 16;
				r.mappedIndexOffset = e.mappedIndex * 16;
				shader->list_remappedUniformEntries_register.push_back(r);
				continue;
			}
			LatteFastAccessRemappedUniformEntry_buffer_t b;
			b.indexOffset = (uint16)(e.index * 16);
			b.mappedIndexOffset = (uint16)(e.mappedIndex * 16);
			const uint16 bankOffset = (uint16)(e.kcacheBankId * 28); // 7 dwords of state per buffer slot
			auto& groups = shader->list_remappedUniformEntries_bufferGroups;
			auto it = std::find_if(groups.begin(), groups.end(), [bankOffset](const LatteDecompilerShader::_RemappedUniformBufferGroup& g) { return g.kcacheBankIdOffset == bankOffset; });
			if (it == groups.end())
			{
				groups.emplace_back(e.kcacheBankId, bankOffset);
				it = groups.end() - 1;
			}
			it->entries.push_back(b);
		}
	}

	void runPipeline(LatteDecompilerShaderContext* ctx, uint8* programData, uint32 programSize)
	{
		ProgramView prog{ programData, programSize };
		LatteDecompilerShader* shader = ctx->shader;
		if (!shader->hasError)
			decodeControlFlow(ctx, prog);
		if (!shader->hasError)
		{
			decodeClauses(ctx, prog, ctx->cfInstructions);
			for (auto& sub : ctx->list_subroutines)
				decodeClauses(ctx, prog, sub.instructions);
		}
		if (!shader->hasError)
			LatteDecompiler_analyze(ctx, shader);
		if (!shader->hasError)
			LatteDecompiler_analyzeDataTypes(ctx);
		// GX2 configures a shader for either uniform registers or uniform blocks
		if (ctx->analyzer.uniformRegisterAccessTracker.HasAccess())
		{
			bool anyBuffer = false;
			for (const auto& t : ctx->analyzer.uniformBufferAccessTracker)
				anyBuffer = anyBuffer || t.HasAccess();
			if (anyBuffer)
				cemuLog_log(LogType::APIErrors, "Shader {:08x} accesses both uniform registers and uniform blocks. Latte does not support using both at the same time (uniform mode is configured via GX2SetShaderModeEx)", ctx->shaderBaseHash);
		}
		if (!shader->hasError)
		{
			const RendererAPI api = g_renderer->GetType();
			if (api == RendererAPI::OpenGL || api == RendererAPI::Vulkan)
			{
#if defined(ENABLE_OPENGL) || defined(ENABLE_VULKAN)
				LatteDecompiler_emitGLSLShader(ctx, shader);
#endif
			}
		}
		LatteDecompiler_cleanup(ctx);
		buildFastAccessTables(shader);
	}

	LatteDecompilerShader* beginShader(LatteDecompilerShaderContext& ctx, LatteConst::ShaderType type, const LatteDecompilerOptions& options, LatteDecompilerOutput_t* output, uint64 baseHash, uint32* contextRegisters)
	{
		ctx.output = output;
		ctx.shaderType = type;
		ctx.options = &options;
		ctx.shaderBaseHash = baseHash;
		ctx.contextRegisters = contextRegisters;
		ctx.contextRegistersNew = (LatteContextRegister*)contextRegisters;
		LatteDecompilerShader* shader = new LatteDecompilerShader(type);
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			shader->textureUnitSamplerAssignment[i] = LATTE_DECOMPILER_SAMPLER_NONE;
			shader->textureUsesDepthCompare[i] = false;
		}
		ctx.shader = shader;
		output->shaderType = type;
		output->shader = shader;
		return shader;
	}
}

// also used by the software rasterizer (LatteSoftware.cpp) to place ALU slots
bool LatteDecompiler_IsALUTransInstruction(bool isOP3, uint32 opcode)
{
	return !isOP3 && isTransOnlyOp2(opcode);
}

void LatteDecompiler_DecompileVertexShader(uint64 shaderBaseHash, uint32* contextRegisters, uint8* programData, uint32 programSize, struct LatteFetchShader* fetchShader, LatteDecompilerOptions& options, LatteDecompilerOutput_t* output)
{
	cemu_assert_debug(fetchShader);
	cemu_assert_debug((programSize & 3) == 0);
	performanceMonitor.gpuTime_shaderCreate.beginMeasuring();
	LatteDecompilerShaderContext ctx = { 0 };
	LatteDecompilerShader* shader = beginShader(ctx, LatteConst::ShaderType::Vertex, options, output, shaderBaseHash, contextRegisters);
	ctx.fetchShader = fetchShader;
	shader->compatibleFetchShader = fetchShader;
	runPipeline(&ctx, programData, programSize);
	performanceMonitor.gpuTime_shaderCreate.endMeasuring();
}

void LatteDecompiler_DecompileGeometryShader(uint64 shaderBaseHash, uint32* contextRegisters, uint8* programData, uint32 programSize, uint8* gsCopyProgramData, uint32 gsCopyProgramSize, uint32 vsRingParameterCount, LatteDecompilerOptions& options, LatteDecompilerOutput_t* output)
{
	cemu_assert_debug((programSize & 3) == 0);
	performanceMonitor.gpuTime_shaderCreate.beginMeasuring();
	LatteDecompilerShaderContext ctx = { 0 };
	LatteDecompilerShader* shader = beginShader(ctx, LatteConst::ShaderType::Geometry, options, output, shaderBaseHash, contextRegisters);
	shader->ringParameterCountFromPrevStage = vsRingParameterCount;
	// the copy shader tells us how ring buffer offsets map to outputs
	if (gsCopyProgramData != nullptr)
		ctx.parsedGSCopyShader = LatteGSCopyShaderParser_parse(gsCopyProgramData, gsCopyProgramSize);
	if (ctx.parsedGSCopyShader == nullptr)
		shader->hasError = true;
	runPipeline(&ctx, programData, programSize);
	delete ctx.parsedGSCopyShader;
	performanceMonitor.gpuTime_shaderCreate.endMeasuring();
}

void LatteDecompiler_DecompilePixelShader(uint64 shaderBaseHash, uint32* contextRegisters, uint8* programData, uint32 programSize, LatteDecompilerOptions& options, LatteDecompilerOutput_t* output)
{
	cemu_assert_debug((programSize & 3) == 0);
	performanceMonitor.gpuTime_shaderCreate.beginMeasuring();
	LatteDecompilerShaderContext ctx = { 0 };
	beginShader(ctx, LatteConst::ShaderType::Pixel, options, output, shaderBaseHash, contextRegisters);
	runPipeline(&ctx, programData, programSize);
	performanceMonitor.gpuTime_shaderCreate.endMeasuring();
}

void LatteDecompiler_cleanup(LatteDecompilerShaderContext* shaderContext)
{
	shaderContext->cfInstructions.clear();
}
