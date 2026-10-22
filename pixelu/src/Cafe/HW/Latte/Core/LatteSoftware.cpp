// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/Core/LatteSoftware.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"

// Minimal CPU interpreter for vertex programs: runs ALU clauses with a handful of
// opcodes and captures the position export. Meant for debugging and for computing
// positions on the CPU; anything beyond that subset is ignored.

bool LatteDecompiler_IsALUTransInstruction(bool isOP3, uint32 opcode);

namespace
{
	union Vec4
	{
		float f[4];
		uint32 u[4];
		sint32 i[4];
	};

	constexpr uint32 kAddressRegister = 128; // AR lives after the 128 GPRs
	constexpr uint32 kPreviousScalar = 4; // PS slot of the previous-vector/scalar results

	struct Machine
	{
		Vec4 gpr[129];
		sint32 prev[5]; // PV.xyzw and PS of the last instruction group
		sint32 prevNext[5]; // results of the group being executed
		const uint32* uniforms; // uniform register file (4 words per register)
		const Vec4* literals;
		const Vec4* kcache0;
		const Vec4* kcache1;
		Vec4 position;
		const uint32* program;
		sint32 programSize;
		LatteFetchShader* fetchShader;
	} s_vm;

	struct PendingWrite
	{
		uint32 gpr;
		uint32 chan;
		sint32 bits;
	};

	sint32 FloatBits(float f)
	{
		sint32 v;
		memcpy(&v, &f, 4);
		return v;
	}

	float BitsFloat(sint32 v)
	{
		float f;
		memcpy(&f, &v, 4);
		return f;
	}

	// raw 32 bit value of an ALU operand
	sint32 OperandBits(uint32 sel, uint32 chan, bool relative, uint32 indexMode)
	{
		auto relOffset = [&](sint32& offset) -> bool
		{
			offset = 0;
			if (!relative)
				return true;
			if (indexMode > GPU7_INDEX_AR_W)
				return false; // loop index / global addressing not supported
			offset = s_vm.gpr[kAddressRegister].i[indexMode];
			return true;
		};
		sint32 offset;
		if (GPU7_ALU_SRC_IS_GPR(sel))
			return s_vm.gpr[sel].i[chan];
		if (GPU7_ALU_SRC_IS_CFILE(sel))
			return relOffset(offset) ? (sint32)s_vm.uniforms[(offset + GPU7_ALU_SRC_GET_CFILE_INDEX(sel)) * 4 + chan] : 0;
		if (GPU7_ALU_SRC_IS_CBANK0(sel))
			return relOffset(offset) ? s_vm.kcache0[offset + GPU7_ALU_SRC_GET_CBANK0_INDEX(sel)].i[chan] : 0;
		if (GPU7_ALU_SRC_IS_CBANK1(sel))
			return relOffset(offset) ? s_vm.kcache1[offset + GPU7_ALU_SRC_GET_CBANK1_INDEX(sel)].i[chan] : 0;
		if (GPU7_ALU_SRC_IS_PV(sel))
			return s_vm.prev[chan];
		if (GPU7_ALU_SRC_IS_PS(sel))
			return s_vm.prev[kPreviousScalar];
		if (GPU7_ALU_SRC_IS_CONST_1F(sel))
			return FloatBits(1.0f);
		if (GPU7_ALU_SRC_IS_CONST_0_5F(sel))
			return FloatBits(0.5f);
		if (GPU7_ALU_SRC_IS_LITERAL(sel))
			return s_vm.literals->i[chan];
		// 0.0 and the operand kinds that are not interpreted
		return 0;
	}

	struct Operand
	{
		uint32 sel, chan;
		bool rel, neg, abs;

		sint32 bits(uint32 indexMode) const { return OperandBits(sel, chan, rel, indexMode); }

		float value(uint32 indexMode) const
		{
			float f = BitsFloat(bits(indexMode));
			if (abs)
				f = std::fabs(f);
			return neg ? -f : f;
		}
	};

	float ApplyOutputModifier(uint32 omod, float f)
	{
		switch (omod)
		{
		case ALU_OMOD_MUL2: return f * 2.0f;
		case ALU_OMOD_MUL4: return f * 4.0f;
		case ALU_OMOD_DIV2: return f / 2.0f;
		default: return f;
		}
	}

	void ExecuteALUClause(uint32 addr, uint32 count, uint32 bank0, uint32 bank1, uint32 bank0Offset, uint32 bank1Offset)
	{
		auto kcacheBase = [](uint32 bank, uint32 offset)
		{
			const MPTR block = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + bank * 7];
			return (const Vec4*)memory_getPointerFromPhysicalOffset(block + offset);
		};
		s_vm.kcache0 = kcacheBase(bank0, bank0Offset);
		s_vm.kcache1 = kcacheBase(bank1, bank1Offset);

		const uint32* slot = s_vm.program + addr * 2;
		const uint32* const clauseEnd = slot + count * 2;
		PendingWrite writes[16];
		while (slot < clauseEnd)
		{
			// an instruction group ends with the slot that has the LAST bit set
			sint32 groupSize = 6;
			for (sint32 n = 0; n < 6; n++)
			{
				if (slot[n * 2] & 0x80000000)
				{
					groupSize = n + 1;
					break;
				}
			}
			s_vm.literals = (const Vec4*)(slot + groupSize * 2);
			uint32 writeCount = 0;
			uint32 unitsUsed = 0;
			uint32 literalChannels = 0;
			bool isDot = false;
			float dotSum = 0.0f;
			auto queueWrite = [&](uint32 gpr, uint32 chan, sint32 bits) { writes[writeCount++] = { gpr, chan, bits }; };
			for (sint32 n = 0; n < groupSize; n++, slot += 2)
			{
				const uint32 w0 = slot[0];
				const uint32 w1 = slot[1];
				const uint32 indexMode = (w0 >> 26) & 7;
				Operand a{ w0 & 0x1FF, (w0 >> 10) & 3, ((w0 >> 9) & 1) != 0, ((w0 >> 12) & 1) != 0, false };
				Operand b{ (w0 >> 13) & 0x1FF, (w0 >> 23) & 3, ((w0 >> 22) & 1) != 0, ((w0 >> 25) & 1) != 0, false };
				const uint32 destGpr = (w1 >> 21) & 0x7F;
				const uint32 destChan = (w1 >> 29) & 3;
				const uint32 op3 = (w1 >> 13) & 0x1F;
				const bool isOP3 = op3 >= 8;
				const uint32 op2 = (w1 >> 7) & 0x7FF;
				// result slot in PV/PS: the destination channel, PS for the second write to a channel
				uint32 unit = (!isOP3 && LatteDecompiler_IsALUTransInstruction(false, op2)) ? kPreviousScalar : destChan;
				if (unitsUsed & (1u << destChan))
					unit = kPreviousScalar;
				else
					unitsUsed |= 1u << destChan;
				for (const Operand* o : { &a, &b })
				{
					if (GPU7_ALU_SRC_IS_LITERAL(o->sel))
						literalChannels |= 1u << o->chan;
				}

				if (isOP3)
				{
					Operand c{ w1 & 0x1FF, (w1 >> 10) & 3, ((w1 >> 9) & 1) != 0, ((w1 >> 12) & 1) != 0, false };
					if (GPU7_ALU_SRC_IS_LITERAL(c.sel))
						literalChannels |= 1u << c.chan;
					bool handled = true;
					sint32 result = 0;
					switch (op3)
					{
					case ALU_OP3_INST_CMOVE:
						result = a.value(indexMode) == 0.0f ? b.bits(indexMode) : c.bits(indexMode);
						break;
					case ALU_OP3_INST_CMOVGT:
						result = a.value(indexMode) > 0.0f ? b.bits(indexMode) : c.bits(indexMode);
						break;
					case ALU_OP3_INST_MULADD:
						result = FloatBits(a.value(indexMode) * b.value(indexMode) + c.value(indexMode));
						break;
					default:
						handled = false;
						break;
					}
					if (handled)
					{
						queueWrite(destGpr, destChan, result);
						s_vm.prevNext[unit] = result;
					}
					continue;
				}

				a.abs = (w1 & 1) != 0;
				b.abs = ((w1 >> 1) & 1) != 0;
				const bool writeGpr = ((w1 >> 4) & 1) != 0;
				const uint32 omod = (w1 >> 5) & 3;
				auto emitFloat = [&](float f, bool modify)
				{
					if (modify)
						f = ApplyOutputModifier(omod, f);
					if (writeGpr)
						queueWrite(destGpr, destChan, FloatBits(f));
					s_vm.prevNext[unit] = FloatBits(f);
				};
				auto emitBits = [&](sint32 v)
				{
					if (writeGpr)
						queueWrite(destGpr, destChan, v);
					s_vm.prevNext[unit] = v;
				};
				switch (op2)
				{
				case ALU_OP2_INST_MOV:
					if (a.neg || a.abs || omod != 0)
						emitFloat(a.value(indexMode), true);
					else
						emitBits(a.bits(indexMode));
					break;
				case ALU_OP2_INST_ADD:
					emitFloat(a.value(indexMode) + b.value(indexMode), true);
					break;
				case ALU_OP2_INST_MUL:
				case ALU_OP2_INST_MUL_IEEE:
				{
					// 0 * anything is 0 on this GPU
					const float x = a.value(indexMode), y = b.value(indexMode);
					emitFloat((x == 0.0f || y == 0.0f) ? 0.0f : x * y, true);
					break;
				}
				case ALU_OP2_INST_TRUNC:
					emitFloat(std::trunc(a.value(indexMode)), true);
					break;
				case ALU_OP2_INST_RECIP_IEEE:
					emitFloat(1.0f / a.value(indexMode), true);
					break;
				case ALU_OP2_INST_SETGT:
					emitFloat(a.value(indexMode) > b.value(indexMode) ? 1.0f : 0.0f, true);
					break;
				case ALU_OP2_INST_SETGE:
					emitFloat(a.value(indexMode) >= b.value(indexMode) ? 1.0f : 0.0f, true);
					break;
				case ALU_OP2_INST_INT_TO_FLOAT:
					emitFloat((float)a.bits(indexMode), true);
					break;
				case ALU_OP2_INST_FLT_TO_INT:
					emitBits((sint32)a.value(indexMode));
					break;
				case ALU_OP2_INST_MOVA_FLOOR:
				{
					const float f = std::clamp((float)std::floor(a.value(indexMode)), -256.0f, 255.0f);
					queueWrite(kAddressRegister, destChan, (sint32)f);
					emitFloat(f, false);
					break;
				}
				case ALU_OP2_INST_DOT4:
				{
					const float product = a.value(indexMode) * b.value(indexMode);
					dotSum += product;
					isDot = true;
					emitFloat(product, true);
					break;
				}
				default:
					break; // NOP and everything not interpreted
				}
			}
			// commit the group
			if (isDot)
			{
				// every write of the group receives the dot product
				for (uint32 n = 0; n < writeCount; n++)
					s_vm.gpr[writes[n].gpr].f[writes[n].chan] = dotSum;
				for (uint32 c = 0; c < 4; c++)
					s_vm.prev[c] = FloatBits(dotSum);
			}
			else
			{
				for (uint32 n = 0; n < writeCount; n++)
					s_vm.gpr[writes[n].gpr].i[writes[n].chan] = writes[n].bits;
				memcpy(s_vm.prev, s_vm.prevNext, sizeof(s_vm.prev));
			}
			// literal constants follow the group in pairs
			if (literalChannels & 0x3)
				slot += 2;
			if (literalChannels & 0xC)
				slot += 2;
		}
	}

	void RunProgram()
	{
		const uint32 wordCount = (uint32)std::max(s_vm.programSize, 0) / 4;
		for (uint32 pc = 0; pc + 1 < wordCount; pc += 2)
		{
			const uint32 w0 = s_vm.program[pc];
			const uint32 w1 = s_vm.program[pc + 1];
			const uint32 op = (w1 >> 23) & 0x7F;
			if (op >= 0x40)
			{
				// ALU clause (shorter opcode field)
				const uint32 aluOp = ((w1 >> 26) & 0xF) | GPU7_CF_INST_ALU_MASK;
				switch (aluOp)
				{
				case GPU7_CF_INST_ALU:
				case GPU7_CF_INST_ALU_PUSH_BEFORE:
				case GPU7_CF_INST_ALU_POP_AFTER:
				case GPU7_CF_INST_ALU_POP2_AFTER:
				case GPU7_CF_INST_ALU_BREAK:
				case GPU7_CF_INST_ALU_ELSE_AFTER:
					ExecuteALUClause(w0 & 0x3FFFFF, ((w1 >> 18) & 0x7F) + 1, (w0 >> 22) & 0xF, (w0 >> 26) & 0xF, ((w1 >> 2) & 0xFF) * 16, ((w1 >> 10) & 0xFF) * 16);
					break;
				default:
					break;
				}
				continue;
			}
			if (op == GPU7_CF_INST_EXPORT || op == GPU7_CF_INST_EXPORT_DONE)
			{
				const uint32 type = (w0 >> 13) & 3;
				const uint32 arrayBase = w0 & 0x1FFF;
				const uint32 sourceGpr = (w0 >> 15) & 0x7F;
				if (type == 1 && arrayBase == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION)
				{
					for (uint32 c = 0; c < 4; c++)
					{
						const uint32 sel = (w1 >> (c * 3)) & 7;
						s_vm.position.i[c] = sel < 4 ? s_vm.gpr[sourceGpr].i[sel] : 0;
					}
				}
			}
			// flow control, texture fetches and other exports are not interpreted
			if ((w1 >> 21) & 1)
				return; // end of program
		}
	}

	void FetchAttributes(sint32 vertexIndex)
	{
		s_vm.gpr[0].i[0] = vertexIndex;
		for (const LatteParsedFetchShaderBufferGroup& group : s_vm.fetchShader->bufferGroups)
		{
			for (sint32 n = 0; n < group.attribCount; n++)
			{
				const LatteParsedFetchShaderAttribute& attr = group.attrib[n];
				if (attr.attributeBufferIndex >= 16)
					continue;
				const uint32* bufferRegs = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START + attr.attributeBufferIndex * 7;
				const MPTR bufferAddr = bufferRegs[0];
				const uint32 stride = (bufferRegs[2] >> 11) & 0xFFFF;
				if (bufferAddr == MPTR_NULL)
					continue;
				// the input register is one past the semantic table slot (R0 holds the vertex index)
				sint32 slotIndex = -1;
				for (sint32 s = 0; s < 32; s++)
				{
					if (LatteGPUState.contextRegister[mmSQ_VTX_SEMANTIC_0 + s] == attr.semanticId)
					{
						slotIndex = s;
						break;
					}
				}
				if (slotIndex < 0)
					continue;
				// big endian 32 bit float vectors are the only supported formats
				uint32 componentCount = 0;
				const bool plainFloat = (uint32)attr.endianSwap == 2 && attr.nfa == LATTE_NFA_2 && attr.isSigned == LATTE_VTX_UNSIGNED;
				if (plainFloat)
				{
					if (attr.format == Latte::E_HWFMT::HWFMT_32_32_FLOAT)
						componentCount = 2;
					else if (attr.format == Latte::E_HWFMT::HWFMT_32_32_32_FLOAT)
						componentCount = 3;
					else if (attr.format == Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT)
						componentCount = 4;
				}
				Vec4 data{};
				const uint32be* src = (const uint32be*)memory_getPointerFromPhysicalOffset(bufferAddr + vertexIndex * stride);
				for (uint32 c = 0; c < componentCount; c++)
					data.u[c] = src[c];
				Vec4& dst = s_vm.gpr[slotIndex + 1];
				for (uint32 c = 0; c < 4; c++)
				{
					// the swizzle only selects between data, 0 and 1 here
					if (attr.ds[c] < 4)
						dst.i[c] = data.i[c];
					else if (attr.ds[c] == 4)
						dst.i[c] = 0;
					else if (attr.ds[c] == 5)
						dst.f[c] = 1.0f;
				}
			}
		}
	}
}

void LatteSoftware_setupVertexShader(LatteFetchShader* fetchShader, void* shaderPtr, sint32 size)
{
	s_vm.fetchShader = fetchShader;
	s_vm.program = (const uint32*)shaderPtr;
	s_vm.programSize = size;
	s_vm.uniforms = LatteGPUState.contextRegister + LATTE_REG_BASE_ALU_CONST + 0x400;
}

void LatteSoftware_executeVertex(sint32 index)
{
	FetchAttributes(index);
	RunProgram();
}

float* LatteSoftware_getPositionExport()
{
	return s_vm.position.f;
}
