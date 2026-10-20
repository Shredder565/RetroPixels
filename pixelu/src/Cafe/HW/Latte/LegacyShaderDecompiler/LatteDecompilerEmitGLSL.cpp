// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"
#include "util/helpers/StringBuf.h"

#include <bit>
#include <bitset>
#include <span>

// GLSL back end of the legacy decompiler (OpenGL and Vulkan).
//
// The generated code models the Latte register file directly:
//  - GPRs are vec4/ivec4 variables R<n>f / R<n>i (or one array Rf[]/Ri[] when
//    the program writes registers through an index). Which representation is
//    used is decided by LatteDecompiler_analyzeDataTypes; values are moved
//    between int and float views with the bit-cast built-ins.
//  - The results of the previous ALU group (PV vector, PS scalar) live in
//    PV<n><t><c> / PS<n><t> where n alternates between groups.
//  - Per-pixel execution masks of the control flow stack are bool arrays
//    activeMaskStack[] (pushed state) and activeMaskStackC[] (combined state).
// The exact text produced here is part of the shader cache key space and is
// what graphic pack replacement shaders are written against, so formatting is
// kept stable.

void LatteDecompiler_emitAttributeDecodeGLSL(LatteDecompilerShader* shaderContext, StringBuf* src, LatteParsedFetchShaderAttribute* attrib);

// ---- helpers (external linkage) ----

// semantic id a vertex shader output parameter is routed to, 0xFF if the pixel shader does not consume it
sint32 _getVertexShaderOutParamSemanticId(uint32* contextRegisters, sint32 index)
{
	const uint32 word = contextRegisters[mmSPI_VS_OUT_ID_0 + (index / 4)];
	const uint32 semanticId = (word >> ((index % 4) * 8)) & 0xFF;
	const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
	for (sint32 i = 0; i < inputs->count; i++)
	{
		if (inputs->import[i].semanticId == semanticId)
			return (sint32)semanticId;
	}
	return 0xFF;
}

// all registers currently share one representation
sint32 _getInputRegisterDataType(LatteDecompilerShaderContext* shaderContext, LatteDecompilerALUInstruction* aluInstruction, sint32 operandIndex)
{
	return shaderContext->typeTracker.defaultDataType;
}

sint32 _getALUInstructionOutputDataType(LatteDecompilerShaderContext* shaderContext, LatteDecompilerALUInstruction* aluInstruction)
{
	return shaderContext->typeTracker.defaultDataType;
}

// operations that consume the operands of all four vector slots of a group
bool _isReductionInstruction(LatteDecompilerALUInstruction* aluInstruction)
{
	if (aluInstruction->isOP3)
		return false;
	return aluInstruction->opcode == ALU_OP2_INST_DOT4 || aluInstruction->opcode == ALU_OP2_INST_DOT4_IEEE || aluInstruction->opcode == ALU_OP2_INST_CUBE;
}

namespace
{
	using ALUIns = LatteDecompilerALUInstruction;
	using TEXIns = LatteDecompilerTEXInstruction;
	using CFIns = LatteDecompilerCFInstruction;

	constexpr sint32 kInt = LATTE_DECOMPILER_DTYPE_SIGNED_INT;
	constexpr sint32 kUInt = LATTE_DECOMPILER_DTYPE_UNSIGNED_INT;
	constexpr sint32 kFloat = LATTE_DECOMPILER_DTYPE_FLOAT;

	std::string_view laneName(sint32 c)
	{
		static constexpr std::string_view kNames[4] = { "x", "y", "z", "w" };
		if (c < 0 || c > 3)
			return "UNDEFINED";
		return kNames[c];
	}

	std::string_view typeTag(sint32 type)
	{
		switch (type)
		{
		case kInt: return "i";
		case kUInt: return "ui";
		case kFloat: return "f";
		default:
			cemu_assert_unimplemented();
			return "";
		}
	}

	// bit-preserving (or value, for int<->uint) conversion wrappers
	std::string_view castOpen(sint32 from, sint32 to)
	{
		if (from == to)
			return "";
		if (from == kFloat && to == kInt)
			return "floatBitsToInt(";
		if (from == kFloat && to == kUInt)
			return "floatBitsToUint(";
		if (from == kInt && to == kFloat)
			return "intBitsToFloat(";
		if (from == kUInt && to == kInt)
			return "int(";
		if (from == kInt && to == kUInt)
			return "uint(";
		cemu_assert_debug(false);
		return "";
	}

	std::string_view castClose(sint32 from, sint32 to)
	{
		return from == to ? "" : ")";
	}

	std::string_view stageTag(LatteConst::ShaderType t)
	{
		switch (t)
		{
		case LatteConst::ShaderType::Vertex: return "VS";
		case LatteConst::ShaderType::Pixel: return "PS";
		case LatteConst::ShaderType::Geometry: return "GS";
		default:
			cemu_assert_unimplemented();
			return "";
		}
	}

	std::string_view addressRegisterLane(sint32 indexMode)
	{
		switch (indexMode)
		{
		case GPU7_INDEX_AR_X: return "ARi.x";
		case GPU7_INDEX_AR_Y: return "ARi.y";
		case GPU7_INDEX_AR_Z: return "ARi.z";
		case GPU7_INDEX_AR_W: return "ARi.w";
		default:
			cemu_assert_unimplemented(); // loop index relative addressing is not supported
			return "";
		}
	}

	// shortest round-trip decimal with a guaranteed fractional part
	std::string floatLiteral(float f)
	{
		std::string s = fmt::format("{:#}", f);
		if (!s.empty() && s.back() == '.')
			s.push_back('0');
		cemu_assert(s.size() < 50);
		return s;
	}

	bool isNop(const ALUIns& ins)
	{
		return !ins.isOP3 && ins.opcode == ALU_OP2_INST_NOP;
	}

	bool isALUClauseType(uint32 type)
	{
		return type == GPU7_CF_INST_ALU || type == GPU7_CF_INST_ALU_PUSH_BEFORE || type == GPU7_CF_INST_ALU_POP_AFTER ||
			type == GPU7_CF_INST_ALU_POP2_AFTER || type == GPU7_CF_INST_ALU_BREAK || type == GPU7_CF_INST_ALU_ELSE_AFTER;
	}

	bool isTextureSampleOp(uint32 op)
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

	// append-only text target; all writes go through the growing string_view path of StringBuf
	class GlslText
	{
	public:
		explicit GlslText(StringBuf* buf) : m_buf(buf) {}

		GlslText& operator<<(std::string_view s)
		{
			m_buf->add(s);
			return *this;
		}

		GlslText& operator<<(const std::string& s)
		{
			m_buf->add(std::string_view(s));
			return *this;
		}

		GlslText& operator<<(const char* s)
		{
			m_buf->add(std::string_view(s));
			return *this;
		}

		template<typename... TArgs>
		void format(fmt::format_string<TArgs...> f, TArgs&&... args)
		{
			fmt::memory_buffer tmp;
			fmt::format_to(std::back_inserter(tmp), f, std::forward<TArgs>(args)...);
			m_buf->add(std::string_view(tmp.data(), tmp.size()));
		}

		StringBuf* buffer() const { return m_buf; }

	private:
		StringBuf* m_buf;
	};

	// Where the PV/PS values of the previous group can be read from, and which
	// GPR lanes of the current group must be read from a copy because an
	// earlier slot of the same group overwrites them (all slots of a group
	// conceptually read their inputs before any of them writes).
	class GroupResultTracker
	{
	public:
		enum class Source : uint8
		{
			Nothing,
			Register, // the slot wrote a GPR lane, PV/PS alias it
			Scratch, // the slot only wrote PV/PS
		};

		struct Slot
		{
			Source source{ Source::Nothing };
			uint8 reg{}; // GPR index (Register)
			uint8 lane{}; // GPR lane (Register) or ALU unit (Scratch)
		};

		struct SavedLane
		{
			uint8 reg;
			uint8 lane;
			uint8 variable;
		};

		void latchGroup(std::span<const ALUIns> group)
		{
			for (Slot& s : m_slots)
				s.source = Source::Nothing;
			for (const ALUIns& ins : group)
			{
				if (isNop(ins))
					continue;
				Slot& s = m_slots[ins.aluUnit];
				if (ins.writeMask == 0)
				{
					s.source = Source::Scratch;
					s.reg = (uint8)(group.front().instructionGroupIndex & 1);
					s.lane = ins.aluUnit;
				}
				else if (ins.destRel == 0)
				{
					// an indexed write leaves PV/PS undefined for us
					s.source = Source::Register;
					s.reg = (uint8)ins.destGpr;
					s.lane = ins.destElem;
				}
			}
		}

		const Slot& slot(uint32 unit) const
		{
			return m_slots[unit];
		}

		void clearSaved() { m_saved.clear(); }

		void save(uint8 reg, uint8 lane)
		{
			m_saved.push_back({ reg, lane, (uint8)m_saved.size() });
		}

		sint32 savedVariable(uint32 reg, uint32 lane) const
		{
			for (const SavedLane& s : m_saved)
			{
				if (s.reg == reg && s.lane == lane)
					return s.variable;
			}
			return -1;
		}

		const std::vector<SavedLane>& saved() const { return m_saved; }

	private:
		Slot m_slots[5]{};
		std::vector<SavedLane> m_saved;
	};

	class GlslShaderWriter
	{
	public:
		GlslShaderWriter(LatteDecompilerShaderContext* ctx, StringBuf* buf)
			: m_ctx(ctx), m_shader(ctx->shader), out(buf) {}

		void writeShader();

	private:
		LatteDecompilerShaderContext* m_ctx;
		LatteDecompilerShader* m_shader;
		GlslText out;
		GroupResultTracker m_group;

		sint32 defType() const { return m_ctx->typeTracker.defaultDataType; }
		LatteConst::ShaderType stage() const { return m_ctx->shaderType; }
		const auto& facts() const { return m_ctx->analyzer; }

		// ---------------------------------------------------------------
		// names
		// ---------------------------------------------------------------

		std::string gpr(sint32 index, sint32 relMode = -1) const
		{
			const std::string_view t = defType() == kInt ? "i" : "f";
			if (!m_ctx->typeTracker.useArrayGPRs)
				return fmt::format("R{}{}", index, t);
			if (relMode >= 0)
				return fmt::format("R{}[{}+{}]", t, index, addressRegisterLane(relMode));
			return fmt::format("R{}[{}]", t, index);
		}

		void emitLaneAccess(sint32 lane)
		{
			cemu_assert_debug(lane >= 0 && lane <= 3);
			if (lane >= 0 && lane <= 3)
				out << "." << laneName(lane);
		}

		// <register>.<lane>, optionally viewed as another type
		void emitRegisterLane(sint32 gprIndex, sint32 lane, sint32 asType = -1)
		{
			cemu_assert_debug(gprIndex >= 0 && gprIndex <= 127);
			if (asType >= 0)
				out << castOpen(defType(), asType);
			out << gpr(gprIndex) << ".";
			if (lane >= 0 && lane <= 3)
				out << laneName(lane);
			else if (lane != -1)
				cemu_assert_unimplemented();
			if (asType >= 0)
				out << castClose(defType(), asType);
		}

		std::string previousResultName(uint32 groupIndex, uint32 unit) const
		{
			cemu_assert_debug(unit < 5);
			if (unit == 4)
				return fmt::format("PS{}{}", groupIndex & 1, typeTag(defType()));
			return fmt::format("PV{}{}{}", groupIndex & 1, typeTag(defType()), laneName((sint32)unit));
		}

		std::string maskName(sint32 level) const
		{
			if (m_ctx->isSubroutine)
				return fmt::format("activeMaskStackSub{:04x}[{}]", m_ctx->subroutineInfo->cfAddr, level);
			return fmt::format("activeMaskStack[{}]", level);
		}

		std::string combinedMaskName(sint32 level) const
		{
			if (m_ctx->isSubroutine)
				return fmt::format("activeMaskStackCSub{:04x}[{}]", m_ctx->subroutineInfo->cfAddr, level);
			return fmt::format("activeMaskStackC[{}]", level);
		}

		std::string textureName(sint32 unit) const
		{
			return fmt::format("textureUnit{}{}", stageTag(m_shader->shaderType), unit);
		}

		std::string uniformBlockVar(sint32 index) const
		{
			return fmt::format("uf_block{}{}", stageTag(m_shader->shaderType), index);
		}

		bool isGPRMarked(sint32 index) const
		{
			return (facts().gprUseMask[index / 8] & (1 << (index % 8))) != 0;
		}

		// ---------------------------------------------------------------
		// ALU operands
		// ---------------------------------------------------------------

		// where an ALU slot writes its result
		void emitTarget(const ALUIns& ins)
		{
			if (ins.writeMask == 0)
			{
				if (!_isReductionInstruction(const_cast<ALUIns*>(&ins)))
				{
					out << previousResultName(ins.instructionGroupIndex, ins.aluUnit);
					return;
				}
				// reduction lanes without GPR output still need somewhere to land
				out << "temp" << typeTag(defType());
				emitLaneAccess(ins.aluUnit);
				return;
			}
			out << gpr(ins.destGpr, ins.destRel == 0 ? -1 : ins.indexMode);
			emitLaneAccess(ins.destElem);
		}

		void emitPreviousResultRead(uint32 unit, uint32 currentGroup)
		{
			const auto& s = m_group.slot(unit);
			switch (s.source)
			{
			case GroupResultTracker::Source::Register:
			{
				const sint32 saved = m_group.savedVariable(s.reg, s.lane);
				if (saved < 0)
				{
					out << gpr(s.reg);
					emitLaneAccess(s.lane);
				}
				else
					out.format("backupReg{}{}", saved, typeTag(defType()));
				break;
			}
			case GroupResultTracker::Source::Scratch:
				out << previousResultName(currentGroup - 1, s.lane);
				break;
			default:
				cemuLog_log(LogType::Force, "Shader {:016x} accesses PV/PS without writing to it", m_ctx->shaderBaseHash);
				cemu_assert_suspicious();
				break;
			}
		}

		// plain GPR read in the register's own type, honouring saved copies
		void emitGPRSourceRaw(const ALUIns& ins, sint32 n)
		{
			const auto& op = ins.sourceOperand[n];
			const sint32 reg = (sint32)GPU7_ALU_SRC_GET_GPR_INDEX(op.sel);
			const sint32 saved = m_group.savedVariable(reg, op.chan);
			if (saved >= 0)
				out.format("backupReg{}{}", saved, typeTag(defType()));
			else
				emitRegisterLane(reg, op.chan);
		}

		// constant file / constant buffer element index (static or AR relative)
		void emitConstantIndex(const ALUIns& ins, sint32 n)
		{
			const auto& op = ins.sourceOperand[n];
			sint32 offset;
			if (GPU7_ALU_SRC_IS_CFILE(op.sel))
				offset = (sint32)GPU7_ALU_SRC_GET_CFILE_INDEX(op.sel);
			else if (GPU7_ALU_SRC_IS_CBANK0(op.sel))
				offset = (sint32)(GPU7_ALU_SRC_GET_CBANK0_INDEX(op.sel) + ins.cfInstruction->cBank0AddrBase);
			else
				offset = (sint32)(GPU7_ALU_SRC_GET_CBANK1_INDEX(op.sel) + ins.cfInstruction->cBank1AddrBase);
			if (op.rel == 0)
			{
				out.format("{}", offset);
				return;
			}
			switch (ins.indexMode)
			{
			case GPU7_INDEX_AR_X:
			case GPU7_INDEX_AR_Y:
			case GPU7_INDEX_AR_Z:
			case GPU7_INDEX_AR_W:
				out.format("{}+{}", addressRegisterLane(ins.indexMode), offset);
				break;
			default:
				cemu_assert_unimplemented();
				break;
			}
		}

		void emitConstantRead(const ALUIns& ins, sint32 n, sint32 wantType)
		{
			const auto& op = ins.sourceOperand[n];
			const uint8 mode = m_shader->uniformMode;
			const std::string_view stg = stageTag(m_shader->shaderType);
			if (mode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
			{
				cemu_assert_debug(op.rel == 0); // relative reads force a full mode
				const bool fromRegisterFile = GPU7_ALU_SRC_IS_CFILE(op.sel);
				uint32 index;
				uint32 bank = 0;
				if (fromRegisterFile)
					index = GPU7_ALU_SRC_GET_CFILE_INDEX(op.sel);
				else if (GPU7_ALU_SRC_IS_CBANK0(op.sel))
				{
					index = GPU7_ALU_SRC_GET_CBANK0_INDEX(op.sel) + ins.cfInstruction->cBank0AddrBase;
					bank = ins.cfInstruction->cBank0Index;
				}
				else
				{
					index = GPU7_ALU_SRC_GET_CBANK1_INDEX(op.sel) + ins.cfInstruction->cBank1AddrBase;
					bank = ins.cfInstruction->cBank1Index;
				}
				// entry lookup: register entries match on index for register reads; every
				// other combination matches on (bank, index) like the analyzer stored it
				const LatteDecompilerRemappedUniformEntry_t* found = nullptr;
				for (const auto& e : m_shader->list_remappedUniformEntries)
				{
					const bool match = (e.isRegister && fromRegisterFile) ? (e.index == index) : (e.kcacheBankId == bank && e.index == index);
					if (match)
					{
						found = &e;
						break;
					}
				}
				cemu_assert_debug(found);
				out << castOpen(kInt, wantType);
				out.format("uf_remapped{}[{}]", stg, found ? found->mappedIndex : 0);
				emitLaneAccess(op.chan);
				out << castClose(kInt, wantType);
			}
			else if (mode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE)
			{
				out << castOpen(kInt, wantType);
				out << "uf_uniformRegister" << stg << "[";
				emitConstantIndex(ins, n);
				out << "]";
				emitLaneAccess(op.chan);
				out << castClose(kInt, wantType);
			}
			else if (mode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
			{
				cemu_assert_debug(!GPU7_ALU_SRC_IS_CFILE(op.sel));
				const uint32 bank = GPU7_ALU_SRC_IS_CBANK0(op.sel) ? ins.cfInstruction->cBank0Index : ins.cfInstruction->cBank1Index;
				out << castOpen(kFloat, wantType);
				out << uniformBlockVar((sint32)bank) << "[";
				emitConstantIndex(ins, n);
				out << "]";
				emitLaneAccess(op.chan);
				out << castClose(kFloat, wantType);
			}
			else
				cemu_assert_debug(false);
		}

		// GPR read with a runtime index. Without a register array this becomes a
		// chain of selects over every register the program touches.
		void emitIndexedGPRRead(const ALUIns& ins, sint32 n, sint32 wantType)
		{
			const auto& op = ins.sourceOperand[n];
			const sint32 base = (sint32)GPU7_ALU_SRC_GET_GPR_INDEX(op.sel);
			cemu_assert_debug(op.rel != 0);
			if (m_ctx->typeTracker.useArrayGPRs)
			{
				out << castOpen(defType(), wantType);
				out << gpr(base, ins.indexMode);
				emitLaneAccess(op.chan);
				out << castClose(defType(), wantType);
				return;
			}
			const std::string_view idx = addressRegisterLane(ins.indexMode);
			out << castOpen(kInt, wantType);
			out << "(";
			for (sint32 r = base; r < LATTE_NUM_GPR; r++)
			{
				if (!isGPRMarked(r))
					continue;
				out.format("({}=={})?", idx, r - base);
				out << gpr(r);
				emitLaneAccess(op.chan);
				out << ":";
			}
			out << "0)";
			out << castClose(kInt, wantType);
		}

		void emitLiteral(const ALUIns& ins, sint32 n, sint32 wantType)
		{
			const uint32 bits = ins.literalData.w[ins.sourceOperand[n].chan];
			if (wantType == kInt)
				out.format("0x{:x}", bits);
			else if (wantType == kUInt)
				out.format("uint(0x{:x})", bits);
			else if (wantType == kFloat)
			{
				// short, exactly representable values are written as decimals for readability
				const sint32 exponent = (sint32)((bits >> 23) & 0xFF) - 127;
				if ((bits & 0xFF) == 0 && exponent >= -10 && exponent <= 10)
					out << floatLiteral(std::bit_cast<float>(bits));
				else
					out.format("intBitsToFloat(0x{:08x})", bits);
			}
		}

		// one ALU source operand, converted to wantType, with abs/neg applied
		void emitOperand(const ALUIns& ins, sint32 n, sint32 wantType)
		{
			cemu_assert_debug(n >= 0 && n < 3);
			const auto& op = ins.sourceOperand[n];
			const uint32 sel = op.sel;
			// abs/neg are float operations, so such operands are produced as float and converted afterwards
			const sint32 finalType = wantType;
			if (wantType != kFloat && (op.abs != 0 || op.neg != 0))
				wantType = kFloat;
			out << castOpen(wantType, finalType);
			if (op.neg != 0)
				out << "-(";
			if (op.abs != 0)
				out << "abs(";

			if (GPU7_ALU_SRC_IS_GPR(sel))
			{
				if (op.rel != 0)
					emitIndexedGPRRead(ins, n, wantType);
				else if (wantType == kInt)
				{
					out << castOpen(defType(), kInt);
					emitGPRSourceRaw(ins, n);
					out << castClose(defType(), kInt);
				}
				else if (wantType == kUInt)
				{
					// only an int register view can be converted here
					const bool wrap = defType() == kInt;
					if (wrap)
						out << "uint(";
					emitGPRSourceRaw(ins, n);
					if (wrap)
						out << ")";
				}
				else if (wantType == kFloat)
				{
					const bool wrap = defType() == kInt;
					if (wrap)
						out << "intBitsToFloat(";
					emitGPRSourceRaw(ins, n);
					if (wrap)
						out << ")";
				}
			}
			else if (GPU7_ALU_SRC_IS_CONST_0F(sel))
			{
				if (wantType == kInt || wantType == kUInt)
					out << "0";
				else if (wantType == kFloat)
					out << "0.0";
			}
			else if (GPU7_ALU_SRC_IS_CONST_1F(sel))
				out << castOpen(kFloat, wantType) << "1.0" << castClose(kFloat, wantType);
			else if (GPU7_ALU_SRC_IS_CONST_0_5F(sel))
				out << castOpen(kFloat, wantType) << "0.5" << castClose(kFloat, wantType);
			else if (GPU7_ALU_SRC_IS_CONST_1I(sel))
			{
				if (wantType == kInt)
					out << "int(1)";
				else if (wantType == kUInt)
					out << "uint(1)";
				else
					cemu_assert_suspicious();
			}
			else if (GPU7_ALU_SRC_IS_CONST_M1I(sel))
			{
				if (wantType == kInt)
					out << "int(-1)";
				else
					cemu_assert_suspicious();
			}
			else if (GPU7_ALU_SRC_IS_LITERAL(sel))
				emitLiteral(ins, n, wantType);
			else if (GPU7_ALU_SRC_IS_CFILE(sel) || GPU7_ALU_SRC_IS_CBANK0(sel) || GPU7_ALU_SRC_IS_CBANK1(sel))
				emitConstantRead(ins, n, wantType);
			else if (GPU7_ALU_SRC_IS_PV(sel) || GPU7_ALU_SRC_IS_PS(sel))
			{
				cemu_assert_debug(ins.instructionGroupIndex > 0);
				out << castOpen(defType(), wantType);
				emitPreviousResultRead(GPU7_ALU_SRC_IS_PV(sel) ? op.chan : 4, ins.instructionGroupIndex);
				out << castClose(defType(), wantType);
			}
			else
				cemuLog_log(LogType::Force, "Unsupported shader ALU operand sel {:#x}\n", sel);

			if (op.abs != 0)
				out << ")";
			if (op.neg != 0)
				out << ")";
			out << castClose(wantType, finalType);
		}

		// ---------------------------------------------------------------
		// ALU instructions
		// ---------------------------------------------------------------

		// <target> = <expression of exprType converted to the register type>;
		template<typename TBody>
		void emitAssign(const ALUIns& ins, sint32 exprType, TBody&& body)
		{
			emitTarget(ins);
			out << " = " << castOpen(exprType, defType());
			body();
			out << castClose(exprType, defType()) << ";\r\n";
		}

		void emitInfix(const ALUIns& ins, sint32 type, std::string_view op)
		{
			emitAssign(ins, type, [&] {
				emitOperand(ins, 0, type);
				out << op;
				emitOperand(ins, 1, type);
			});
		}

		void emitCall1(const ALUIns& ins, sint32 resultType, std::string_view fn, sint32 argType)
		{
			emitAssign(ins, resultType, [&] {
				out << fn << "(";
				emitOperand(ins, 0, argType);
				out << ")";
			});
		}

		void emitAssignScratchFloat(const ALUIns& ins)
		{
			emitAssign(ins, kFloat, [&] { out << "tempResultf"; });
		}

		static bool usesPlainMultiply(const ALUIns& ins)
		{
			// with a constant factor the 0*x=0 rule cannot make a difference
			for (sint32 i = 0; i < 2; i++)
			{
				const uint32 sel = ins.sourceOperand[i].sel;
				if (GPU7_ALU_SRC_IS_LITERAL(sel) || GPU7_ALU_SRC_IS_ANY_CONST(sel))
					return true;
			}
			return false;
		}

		static std::string_view compareOp(uint32 opcode)
		{
			switch (opcode)
			{
			case ALU_OP2_INST_SETE: case ALU_OP2_INST_SETE_DX10: case ALU_OP2_INST_SETE_INT:
			case ALU_OP2_INST_PRED_SETE: case ALU_OP2_INST_PRED_SETE_INT:
			case ALU_OP2_INST_KILLE: case ALU_OP2_INST_KILLE_INT:
				return " == ";
			case ALU_OP2_INST_SETNE: case ALU_OP2_INST_SETNE_DX10: case ALU_OP2_INST_SETNE_INT:
			case ALU_OP2_INST_PRED_SETNE: case ALU_OP2_INST_PRED_SETNE_INT:
			case ALU_OP2_INST_KILLNE_INT:
				return " != ";
			case ALU_OP2_INST_SETGT: case ALU_OP2_INST_SETGT_DX10: case ALU_OP2_INST_SETGT_INT: case ALU_OP2_INST_SETGT_UINT:
			case ALU_OP2_INST_PRED_SETGT: case ALU_OP2_INST_PRED_SETGT_INT:
			case ALU_OP2_INST_KILLGT: case ALU_OP2_INST_KILLGT_INT:
				return " > ";
			case ALU_OP2_INST_SETGE: case ALU_OP2_INST_SETGE_DX10: case ALU_OP2_INST_SETGE_INT: case ALU_OP2_INST_SETGE_UINT:
			case ALU_OP2_INST_PRED_SETGE: case ALU_OP2_INST_PRED_SETGE_INT:
			case ALU_OP2_INST_KILLGE:
				return " >= ";
			default:
				cemu_assert_debug(false);
				return "";
			}
		}

		void emitAddressLoadLane(const ALUIns& ins, std::string_view value)
		{
			static constexpr std::string_view kAR[4] = { "ARi.x", "ARi.y", "ARi.z", "ARi.w" };
			out << kAR[std::min<uint32>(ins.destElem, 3)] << " = " << value << ";\r\n";
		}

		void emitOP2(const CFIns& cf, const ALUIns& ins)
		{
			const uint32 op = ins.opcode;
			switch (op)
			{
			case ALU_OP2_INST_MOV:
				if (ins.sourceOperand[0].abs != 0 || ins.sourceOperand[0].neg != 0)
					emitAssign(ins, kFloat, [&] { emitOperand(ins, 0, kFloat); });
				else
					emitAssign(ins, defType(), [&] { emitOperand(ins, 0, defType()); });
				break;
			case ALU_OP2_INST_MOVA_FLOOR:
				cemu_assert_debug(ins.writeMask == 0 && ins.omod == 0);
				out << "tempResultf = ";
				emitOperand(ins, 0, kFloat);
				out << ";\r\n";
				out << "tempResultf = floor(tempResultf);\r\n";
				// out of range values select index -256
				out << "tempResultf = (tempResultf >= -256.0 && tempResultf <= 255.0) ? tempResultf : -256.0;\r\n";
				emitAddressLoadLane(ins, "int(tempResultf)");
				emitTarget(ins);
				cemu_assert_debug(defType() == kInt);
				out << " = floatBitsToInt(tempResultf);\r\n";
				break;
			case ALU_OP2_INST_MOVA_INT:
				cemu_assert_debug(ins.writeMask == 0 && ins.omod == 0);
				out << "tempResulti = ";
				emitOperand(ins, 0, kInt);
				out << ";\r\n";
				out << "tempResulti = (tempResulti >= -256 && tempResulti <= 255) ? tempResulti : -256;\r\n";
				emitAddressLoadLane(ins, "tempResulti");
				emitTarget(ins);
				cemu_assert_debug(defType() == kInt);
				out << " = tempResulti;\r\n";
				break;
			case ALU_OP2_INST_ADD:
				emitInfix(ins, kFloat, " + ");
				break;
			case ALU_OP2_INST_MUL:
				// legacy multiply: 0 * anything (including inf/nan) is 0
				emitAssign(ins, kFloat, [&] {
					if (GPU7_ALU_SRC_IS_CONST_0F(ins.sourceOperand[0].sel) || GPU7_ALU_SRC_IS_CONST_0F(ins.sourceOperand[1].sel))
					{
						out << "0.0";
						return;
					}
					if (m_ctx->options->strictMul && !usesPlainMultiply(ins))
					{
						out << "mul_nonIEEE(";
						emitOperand(ins, 0, kFloat);
						out << ", ";
						emitOperand(ins, 1, kFloat);
						out << ")";
					}
					else
					{
						emitOperand(ins, 0, kFloat);
						out << " * ";
						emitOperand(ins, 1, kFloat);
					}
				});
				break;
			case ALU_OP2_INST_MUL_IEEE:
				emitInfix(ins, kFloat, " * ");
				break;
			case ALU_OP2_INST_RECIP_IEEE:
				emitAssign(ins, kFloat, [&] {
					out << "1.0 / ";
					emitOperand(ins, 0, kFloat);
				});
				break;
			case ALU_OP2_INST_RECIP_CLAMPED:
				out << "tempResultf = 1.0 / (";
				emitOperand(ins, 0, kFloat);
				out << ");\r\n";
				// infinity is clamped to +-FLT_MAX
				out << "if (isinf(tempResultf)) tempResultf = uintBitsToFloat((floatBitsToUint(tempResultf) & 0x80000000u) | 0x7f7fffffu);\r\n";
				emitAssignScratchFloat(ins);
				break;
			case ALU_OP2_INST_RECIP_FF:
				out << "tempResultf = 1.0 / (";
				emitOperand(ins, 0, kFloat);
				out << ");\r\n";
				// infinity flushes to signed zero
				out << "if( isinf(tempResultf) == true && (floatBitsToInt(tempResultf)&0x80000000) == 0 ) tempResultf = 0.0;\r\n";
				out << "else if( isinf(tempResultf) == true && (floatBitsToInt(tempResultf)&0x80000000) != 0 ) tempResultf = -0.0;\r\n";
				emitAssignScratchFloat(ins);
				break;
			case ALU_OP2_INST_RECIPSQRT_IEEE:
			case ALU_OP2_INST_RECIPSQRT_CLAMPED:
			case ALU_OP2_INST_RECIPSQRT_FF:
				out << "tempResultf = 1.0 / sqrt(";
				emitOperand(ins, 0, kFloat);
				out << ");\r\n";
				if (op == ALU_OP2_INST_RECIPSQRT_CLAMPED)
				{
					out << "if( isinf(tempResultf) == true && (floatBitsToInt(tempResultf)&0x80000000) != 0 ) tempResultf = -3.40282347E+38F;\r\n";
					out << "else if( isinf(tempResultf) == true && (floatBitsToInt(tempResultf)&0x80000000) == 0 ) tempResultf = 3.40282347E+38F;\r\n";
				}
				else if (op == ALU_OP2_INST_RECIPSQRT_FF)
				{
					out << "if( isinf(tempResultf) == true && (floatBitsToInt(tempResultf)&0x80000000) != 0 ) tempResultf = -0.0;\r\n";
					out << "else if( isinf(tempResultf) == true && (floatBitsToInt(tempResultf)&0x80000000) == 0 ) tempResultf = 0.0;\r\n";
				}
				emitAssignScratchFloat(ins);
				break;
			case ALU_OP2_INST_MAX:
			case ALU_OP2_INST_MAX_DX10:
			case ALU_OP2_INST_MIN:
			case ALU_OP2_INST_MIN_DX10:
				emitAssign(ins, kFloat, [&] {
					out << ((op == ALU_OP2_INST_MAX || op == ALU_OP2_INST_MAX_DX10) ? "max(" : "min(");
					emitOperand(ins, 0, kFloat);
					out << ", ";
					emitOperand(ins, 1, kFloat);
					out << ")";
				});
				break;
			case ALU_OP2_INST_FLOOR: emitCall1(ins, kFloat, "floor", kFloat); break;
			case ALU_OP2_INST_FRACT: emitCall1(ins, kFloat, "fract", kFloat); break;
			case ALU_OP2_INST_TRUNC: emitCall1(ins, kFloat, "trunc", kFloat); break;
			case ALU_OP2_INST_RNDNE: emitCall1(ins, kFloat, "roundEven", kFloat); break;
			case ALU_OP2_INST_EXP_IEEE: emitCall1(ins, kFloat, "exp2", kFloat); break;
			case ALU_OP2_INST_SQRT_IEEE: emitCall1(ins, kFloat, "sqrt", kFloat); break;
			case ALU_OP2_INST_FLT_TO_INT: emitCall1(ins, kInt, "int", kFloat); break;
			case ALU_OP2_INST_FLT_TO_UINT: emitCall1(ins, kUInt, "uint", kFloat); break;
			case ALU_OP2_INST_INT_TO_FLOAT: emitCall1(ins, kFloat, "float", kInt); break;
			case ALU_OP2_INST_UINT_TO_FLOAT: emitCall1(ins, kFloat, "float", kUInt); break;
			case ALU_OP2_INST_NOT_INT: emitCall1(ins, kInt, "~", kInt); break;
			case ALU_OP2_INST_LOG_CLAMPED:
			case ALU_OP2_INST_LOG_IEEE:
				out << "tempResultf = max(0.0, ";
				emitOperand(ins, 0, kFloat);
				out << ");\r\n";
				out << "tempResultf = log2(tempResultf);\r\n";
				if (op == ALU_OP2_INST_LOG_CLAMPED)
					out << "if( isinf(tempResultf) == true ) tempResultf = -3.40282347E+38F;\r\n";
				emitAssignScratchFloat(ins);
				break;
			case ALU_OP2_INST_SIN:
			case ALU_OP2_INST_COS:
				// the hardware takes the angle in turns (input is pre-scaled by 1/(2*pi))
				emitAssign(ins, kFloat, [&] {
					out << (op == ALU_OP2_INST_SIN ? "sin((" : "cos((");
					emitOperand(ins, 0, kFloat);
					out << ")/0.1591549367)";
				});
				break;
			case ALU_OP2_INST_AND_INT: emitInfix(ins, kInt, " & "); break;
			case ALU_OP2_INST_OR_INT: emitInfix(ins, kInt, " | "); break;
			case ALU_OP2_INST_XOR_INT: emitInfix(ins, kInt, " ^ "); break;
			case ALU_OP2_INST_ADD_INT: emitInfix(ins, kInt, " + "); break;
			case ALU_OP2_INST_SUB_INT: emitInfix(ins, kInt, " - "); break; // src0 - src1
			case ALU_OP2_INST_MULLO_INT: emitInfix(ins, kInt, " * "); break;
			case ALU_OP2_INST_MULLO_UINT: emitInfix(ins, kUInt, " * "); break;
			case ALU_OP2_INST_LSHL_INT: emitInfix(ins, kInt, " << "); break;
			case ALU_OP2_INST_LSHR_INT: emitInfix(ins, kUInt, " >> "); break;
			case ALU_OP2_INST_ASHR_INT: emitInfix(ins, kInt, " >> "); break;
			case ALU_OP2_INST_MAX_INT:
			case ALU_OP2_INST_MIN_INT:
			case ALU_OP2_INST_MAX_UINT:
			case ALU_OP2_INST_MIN_UINT:
			{
				const sint32 t = (op == ALU_OP2_INST_MAX_UINT || op == ALU_OP2_INST_MIN_UINT) ? kUInt : kInt;
				emitAssign(ins, t, [&] {
					out << ((op == ALU_OP2_INST_MAX_INT || op == ALU_OP2_INST_MAX_UINT) ? "max(" : "min(");
					emitOperand(ins, 0, t);
					out << ", ";
					emitOperand(ins, 1, t);
					out << ")";
				});
				break;
			}
			case ALU_OP2_INST_SETE:
			case ALU_OP2_INST_SETGT:
			case ALU_OP2_INST_SETGE:
			case ALU_OP2_INST_SETNE:
				emitAssign(ins, kFloat, [&] {
					out << "(";
					emitOperand(ins, 0, kFloat);
					out << compareOp(op);
					emitOperand(ins, 1, kFloat);
					out << ")?1.0:0.0";
				});
				break;
			case ALU_OP2_INST_SETE_DX10:
			case ALU_OP2_INST_SETGT_DX10:
			case ALU_OP2_INST_SETGE_DX10:
			case ALU_OP2_INST_SETNE_DX10:
				cemu_assert_debug(ins.omod == 0);
				emitAssign(ins, kInt, [&] {
					out << "((";
					emitOperand(ins, 0, kFloat);
					out << compareOp(op);
					emitOperand(ins, 1, kFloat);
					out << ")?-1:0)";
				});
				break;
			case ALU_OP2_INST_SETE_INT:
			case ALU_OP2_INST_SETGT_INT:
			case ALU_OP2_INST_SETGE_INT:
			case ALU_OP2_INST_SETNE_INT:
				emitAssign(ins, kInt, [&] {
					out << "(";
					emitOperand(ins, 0, kInt);
					out << compareOp(op);
					emitOperand(ins, 1, kInt);
					out << ")?-1:0";
				});
				break;
			case ALU_OP2_INST_SETGT_UINT:
			case ALU_OP2_INST_SETGE_UINT:
				emitAssign(ins, kInt, [&] {
					out << "(";
					emitOperand(ins, 0, kUInt);
					out << compareOp(op);
					emitOperand(ins, 1, kUInt);
					out << ")?int(0xFFFFFFFF):int(0x0)";
				});
				break;
			case ALU_OP2_INST_PRED_SETE:
			case ALU_OP2_INST_PRED_SETGT:
			case ALU_OP2_INST_PRED_SETGE:
			case ALU_OP2_INST_PRED_SETNE:
			case ALU_OP2_INST_PRED_SETE_INT:
			case ALU_OP2_INST_PRED_SETGT_INT:
			case ALU_OP2_INST_PRED_SETGE_INT:
			case ALU_OP2_INST_PRED_SETNE_INT:
			{
				cemu_assert_debug(ins.writeMask == 0);
				const bool intCompare = op == ALU_OP2_INST_PRED_SETE_INT || op == ALU_OP2_INST_PRED_SETGT_INT || op == ALU_OP2_INST_PRED_SETGE_INT || op == ALU_OP2_INST_PRED_SETNE_INT;
				const sint32 t = intCompare ? kInt : kFloat;
				out << "predResult = (";
				emitOperand(ins, 0, t);
				out << compareOp(op);
				emitOperand(ins, 1, t);
				out << ");\r\n";
				emitPredicateUse(cf, ins);
				break;
			}
			case ALU_OP2_INST_KILLE:
			case ALU_OP2_INST_KILLGT:
			case ALU_OP2_INST_KILLGE:
			case ALU_OP2_INST_KILLE_INT:
			case ALU_OP2_INST_KILLGT_INT:
			case ALU_OP2_INST_KILLNE_INT:
			{
				const bool intCompare = op == ALU_OP2_INST_KILLE_INT || op == ALU_OP2_INST_KILLGT_INT || op == ALU_OP2_INST_KILLNE_INT;
				const sint32 t = intCompare ? kInt : kFloat;
				out << "if(  (";
				emitOperand(ins, 0, t);
				out << compareOp(op);
				emitOperand(ins, 1, t);
				out << ")) discard;\r\n";
				break;
			}
			default:
				out << "Unsupported instruction;\r\n";
				debug_printf("GLSL emitter: OP2 opcode 0x%x not handled\n", op);
				m_shader->hasError = true;
				break;
			}
		}

		// the predicate either opens a new execution mask level or requests a loop break
		void emitPredicateUse(const CFIns& cf, const ALUIns& ins)
		{
			if (cf.type == GPU7_CF_INST_ALU_PUSH_BEFORE)
			{
				const sint32 d = cf.activeStackDepth;
				out << maskName(d) << " = predResult;\r\n";
				out << combinedMaskName(d + 1) << " = predResult == true && " << combinedMaskName(d) << " == true;\r\n";
			}
			else if (cf.type == GPU7_CF_INST_ALU_BREAK)
			{
				// applied once the whole clause ran
				if (ins.updateExecuteMask)
					out << "aluBreak = aluBreak || (predResult == false);\r\n";
			}
			else
				cemu_assert_debug(false);
		}

		static bool sameGPRSource(const ALUIns& ins, sint32 a, sint32 b)
		{
			const auto& x = ins.sourceOperand[a];
			const auto& y = ins.sourceOperand[b];
			return x.sel == y.sel && GPU7_ALU_SRC_IS_GPR(x.sel) && x.chan == y.chan && x.abs == y.abs && x.neg == y.neg && x.rel == y.rel;
		}

		void emitOP3(const ALUIns& ins)
		{
			const uint32 op = ins.opcode;
			cemu_assert_debug(ins.destRel == 0);
			const bool isSelect = op == ALU_OP3_INST_CMOVE || op == ALU_OP3_INST_CMOVGT || op == ALU_OP3_INST_CMOVGE ||
				op == ALU_OP3_INST_CNDE_INT || op == ALU_OP3_INST_CNDGT_INT || op == ALU_OP3_INST_CMOVGE_INT;
			// both select inputs identical: the condition does not matter
			if (isSelect && sameGPRSource(ins, 1, 2) && ins.sourceOperand[1].abs == 0 && ins.sourceOperand[1].neg == 0)
			{
				emitAssign(ins, defType(), [&] { emitOperand(ins, 1, defType()); });
				return;
			}
			switch (op)
			{
			case ALU_OP3_INST_MULADD:
			case ALU_OP3_INST_MULADD_M2:
			case ALU_OP3_INST_MULADD_M4:
			case ALU_OP3_INST_MULADD_D2:
			case ALU_OP3_INST_MULADD_IEEE:
				emitAssign(ins, kFloat, [&] {
					const bool scaled = op != ALU_OP3_INST_MULADD;
					if (scaled)
						out << "(";
					const bool plain = op == ALU_OP3_INST_MULADD_IEEE || usesPlainMultiply(ins);
					if (m_ctx->options->strictMul && !plain)
					{
						out << "mul_nonIEEE(";
						emitOperand(ins, 0, kFloat);
						out << ",";
						emitOperand(ins, 1, kFloat);
						out << ")";
					}
					else
					{
						emitOperand(ins, 0, kFloat);
						out << " * ";
						emitOperand(ins, 1, kFloat);
					}
					out << " + ";
					emitOperand(ins, 2, kFloat);
					if (scaled)
						out << ")";
					if (op == ALU_OP3_INST_MULADD_D2)
						out << "/2.0";
					else if (op == ALU_OP3_INST_MULADD_M2)
						out << "*2.0";
					else if (op == ALU_OP3_INST_MULADD_M4)
						out << "*4.0";
				});
				break;
			case ALU_OP3_INST_CNDE_INT:
			case ALU_OP3_INST_CNDGT_INT:
			case ALU_OP3_INST_CMOVGE_INT:
			{
				// negated inputs only make sense as floats
				const sint32 t = (ins.sourceOperand[1].neg != 0 || ins.sourceOperand[2].neg != 0) ? kFloat : kInt;
				emitAssign(ins, t, [&] {
					out << "((";
					emitOperand(ins, 0, kInt);
					out << (op == ALU_OP3_INST_CNDE_INT ? " == " : op == ALU_OP3_INST_CNDGT_INT ? " > " : " >= ");
					out << "0)?(";
					emitOperand(ins, 1, t);
					out << "):(";
					emitOperand(ins, 2, t);
					out << "))";
				});
				break;
			}
			case ALU_OP3_INST_CMOVE:
			case ALU_OP3_INST_CMOVGT:
			case ALU_OP3_INST_CMOVGE:
				emitAssign(ins, kInt, [&] {
					out << "((";
					emitOperand(ins, 0, kFloat);
					out << (op == ALU_OP3_INST_CMOVE ? " == " : op == ALU_OP3_INST_CMOVGE ? " >= " : " > ");
					out << "0.0)?(";
					emitOperand(ins, 1, kInt);
					out << "):(";
					emitOperand(ins, 2, kInt);
					out << "))";
				});
				break;
			default:
				out << "Unsupported instruction;\r\n";
				debug_printf("GLSL emitter: OP3 opcode 0x%x not handled\n", op);
				m_shader->hasError = true;
				break;
			}
		}

		// the four operand pairs of a reduction as two vec4 constructors
		void emitReductionInputs(const ALUIns* const lanes[4], std::string_view between)
		{
			for (sint32 operand = 0; operand < 2; operand++)
			{
				if (operand == 1)
					out << between;
				out << "vec4(";
				for (sint32 l = 0; l < 4; l++)
				{
					if (l != 0)
						out << ",";
					emitOperand(*lanes[l], operand, kFloat);
				}
			}
			out << ")";
		}

		void emitReduction(const ALUIns* const lanes[4])
		{
			const ALUIns& first = *lanes[0];
			if (first.opcode == ALU_OP2_INST_DOT4 || first.opcode == ALU_OP2_INST_DOT4_IEEE)
			{
				emitAssign(first, kFloat, [&] {
					out << "dot(";
					emitReductionInputs(lanes, "),");
					out << ")";
				});
			}
			else if (first.opcode == ALU_OP2_INST_CUBE)
			{
				// CUBE reads src0.zzxy / src1.yxzz; results are T, S, 2*major axis, face
				out << "redcCUBE(";
				emitReductionInputs(lanes, "),");
				out << ",cubeMapSTM,cubeMapFaceId);\r\n";
				static constexpr std::string_view kParts[4] = { "cubeMapSTM.x", "cubeMapSTM.y", "cubeMapSTM.z", "float(cubeMapFaceId)" };
				for (sint32 l = 0; l < 4; l++)
					emitAssign(*lanes[l], kFloat, [&] { out << kParts[l]; });
			}
			else
				cemu_assert_unimplemented();
		}

		// a reduction's result is visible in every lane's PV and destination
		void emitReductionBroadcast(const ALUIns* const lanes[4])
		{
			if (lanes[0]->opcode == ALU_OP2_INST_CUBE)
			{
				for (sint32 l = 0; l < 4; l++)
				{
					if (lanes[l]->writeMask != 0)
						continue;
					out << previousResultName(lanes[l]->instructionGroupIndex, lanes[l]->aluUnit) << " = ";
					emitTarget(*lanes[l]);
					out << ";\r\n";
				}
				return;
			}
			for (sint32 l = 0; l < 4; l++)
			{
				if (lanes[l]->writeMask == 0)
					out << previousResultName(lanes[l]->instructionGroupIndex, lanes[l]->aluUnit);
				else if (l == 0)
					continue;
				else
					emitTarget(*lanes[l]);
				out << " = ";
				emitTarget(*lanes[0]);
				out << ";\r\n";
			}
		}

		void emitOutputModifiers(const ALUIns& ins)
		{
			const sint32 t = defType();
			if (ins.omod != ALU_OMOD_NONE)
			{
				if (t == kFloat)
				{
					emitTarget(ins);
					if (ins.omod == ALU_OMOD_MUL2)
						out << " *= 2.0;\r\n";
					else if (ins.omod == ALU_OMOD_MUL4)
						out << " *= 4.0;\r\n";
					else if (ins.omod == ALU_OMOD_DIV2)
						out << " /= 2.0;\r\n";
				}
				else if (t == kInt)
				{
					emitTarget(ins);
					out << " = floatBitsToInt(intBitsToFloat(";
					emitTarget(ins);
					out << ")";
					if (ins.omod == ALU_OMOD_MUL2)
						out << " * 2.0";
					else if (ins.omod == ALU_OMOD_MUL4)
						out << " * 4.0";
					else if (ins.omod == ALU_OMOD_DIV2)
						out << " / 2.0";
					out << ");\r\n";
				}
				else
					cemu_assert_unimplemented();
			}
			if (ins.destClamp != 0)
			{
				if (t == kFloat)
				{
					emitTarget(ins);
					out << " = clamp(";
					emitTarget(ins);
					out << ", 0.0, 1.0);\r\n";
				}
				else if (t == kInt)
				{
					emitTarget(ins);
					out << " = clampFI32(";
					emitTarget(ins);
					out << ");\r\n";
				}
				else
					cemu_assert_unimplemented();
			}
		}

		// copies of GPR lanes that a group reads after an earlier slot of the same group wrote them
		void emitGroupBackups(std::span<const ALUIns> group)
		{
			std::bitset<LATTE_NUM_GPR * 4> written;
			m_group.clearSaved();
			for (const ALUIns& ins : group)
			{
				if (isNop(ins))
					continue;
				cemu_assert_debug(ins.destElem <= 3);
				for (const auto& src : ins.sourceOperand)
				{
					uint32 reg;
					uint32 lane;
					if (GPU7_ALU_SRC_IS_GPR(src.sel))
					{
						reg = GPU7_ALU_SRC_GET_GPR_INDEX(src.sel);
						lane = src.chan;
					}
					else if (GPU7_ALU_SRC_IS_PV(src.sel) || GPU7_ALU_SRC_IS_PS(src.sel))
					{
						// PV/PS aliasing a GPR count as a read of that GPR
						const auto& s = m_group.slot(GPU7_ALU_SRC_IS_PV(src.sel) ? src.chan : 4);
						if (s.source != GroupResultTracker::Source::Register)
							continue;
						reg = s.reg;
						lane = s.lane;
					}
					else
						continue;
					// note: the conflict test uses the operand's lane field, also for PV/PS reads
					if (written.test(reg * 4 + src.chan) && m_group.savedVariable(reg, lane) < 0)
						m_group.save((uint8)reg, (uint8)lane);
				}
				if (ins.writeMask != 0)
					written.set(ins.destGpr * 4 + ins.destElem);
			}
			for (const auto& s : m_group.saved())
			{
				out.format("backupReg{}{} = ", s.variable, typeTag(defType()));
				out << gpr(s.reg);
				emitLaneAccess(s.lane);
				out << ";\r\n";
			}
		}

		void emitALUClause(const CFIns& cf)
		{
			m_group = GroupResultTracker();
			const auto& list = cf.instructionsALU;
			size_t groupStart = 0;
			for (size_t i = 0; i < list.size(); i++)
			{
				const ALUIns& ins = list[i];
				if (ins.indexInGroup == 0)
				{
					out.format("// {}\r\n", ins.instructionGroupIndex);
					if (i > 0)
						m_group.latchGroup(std::span<const ALUIns>(list.data() + groupStart, i - groupStart));
					groupStart = i;
					size_t groupEnd = i + 1;
					while (groupEnd < list.size() && list[groupEnd].instructionGroupIndex == ins.instructionGroupIndex)
						groupEnd++;
					emitGroupBackups(std::span<const ALUIns>(list.data() + i, groupEnd - i));
				}
				const bool reduction = _isReductionInstruction(const_cast<ALUIns*>(&ins));
				const ALUIns* lanes[4]{};
				if (reduction)
				{
					cemu_assert_debug(i + 4 <= list.size());
					for (sint32 l = 0; l < 4; l++)
						lanes[l] = &list[i + l];
					emitReduction(lanes);
					i += 3;
				}
				else if (ins.isOP3)
					emitOP3(ins);
				else if (ins.opcode == ALU_OP2_INST_NOP)
					continue;
				else
					emitOP2(cf, ins);
				emitOutputModifiers(ins);
				if (reduction)
					emitReductionBroadcast(lanes);
			}
		}

		// ---------------------------------------------------------------
		// TEX / VTX clauses
		// ---------------------------------------------------------------

		// one lane of the coordinate vector (register lane or the constants 0/1)
		void emitCoord(const TEXIns& t, sint32 comp, sint32 asType)
		{
			cemu_assert(comp >= 0 && comp < 4);
			cemu_assert_debug(asType == kInt || asType == kFloat);
			const sint32 sel = t.textureFetch.srcSel[comp];
			if (sel < 4)
			{
				emitRegisterLane(t.srcGpr, sel, asType);
				return;
			}
			if (sel == 4)
				out << (asType == kInt ? "floatBitsToInt(0.0)" : "0.0");
			else if (sel == 5)
				out << (asType == kInt ? "floatBitsToInt(1.0)" : "1.0");
		}

		// float view of up to two coordinate lanes, '_' for constant selects
		std::string coordSwizzle(sint32 gprIndex, sint32 selA, sint32 selB = -1) const
		{
			static constexpr std::string_view kSel[8] = { "x", "y", "z", "w", "_", "_", "_", "_" };
			const bool reinterpret = defType() == kInt;
			std::string s;
			if (reinterpret)
				s += "intBitsToFloat(";
			s += gpr(gprIndex);
			s += ".";
			if (selA >= 0)
				s += kSel[selA & 7];
			if (selB >= 0)
				s += kSel[selB & 7];
			if (reinterpret)
				s += ")";
			return s;
		}

		// "<dst>.<written lanes>" and the number of lanes
		sint32 emitDestinationMask(const TEXIns& t)
		{
			sint32 n = 0;
			for (sint32 f = 0; f < 4; f++)
			{
				if (t.dstSel[f] < 4)
				{
					out << laneName(f);
					n++;
				}
				else if (t.dstSel[f] != 7)
					cemu_assert_unimplemented();
			}
			return n;
		}

		void emitSelectedLanes(const TEXIns& t)
		{
			for (sint32 f = 0; f < 4; f++)
			{
				if (t.dstSel[f] < 4)
					out << laneName(t.dstSel[f]);
				else if (t.dstSel[f] != 7)
					cemu_assert_unimplemented();
			}
		}

		Latte::E_DIM textureDim(sint32 unit) const
		{
			if (unit < 0 || unit >= LATTE_NUM_MAX_TEX_UNITS)
				return (Latte::E_DIM)0xFF;
			return m_shader->textureUnitDim[unit];
		}

		static std::string_view sampleFunction(uint32 op, bool texelCoords, bool hasOffset)
		{
			if (texelCoords)
				return "texelFetch(";
			switch (op)
			{
			case GPU7_TEX_INST_FETCH4:
				return "textureGather(";
			case GPU7_TEX_INST_SAMPLE_L:
			case GPU7_TEX_INST_SAMPLE_LZ:
			case GPU7_TEX_INST_SAMPLE_C_L:
			case GPU7_TEX_INST_SAMPLE_C_LZ:
				return hasOffset ? "textureLodOffset(" : "textureLod(";
			case GPU7_TEX_INST_SAMPLE:
			case GPU7_TEX_INST_SAMPLE_LB: // bias comes from the coordinate register
			case GPU7_TEX_INST_SAMPLE_C:
				return hasOffset ? "textureOffset(" : "texture(";
			case GPU7_TEX_INST_SAMPLE_G:
				return hasOffset ? "textureGradOffset(" : "textureGrad(";
			default:
				cemu_assert_unimplemented();
				return "texture(";
			}
		}

		void emitCubeCoords(const TEXIns& t)
		{
			// the shader passes face-space s/t (+1.5 bias) and the face id; rebuild a direction
			out << "vec4(";
			out << "redcCUBEReverse(" << coordSwizzle(t.srcGpr, t.textureFetch.srcSel[0], t.textureFetch.srcSel[1]) << ",";
			out << "int(";
			emitCoord(t, 2, kFloat);
			out << "))";
			out.format(",cubeMapArrayIndex{})", t.textureFetch.textureIndex);
		}

		void emitTextureSample(const TEXIns& t)
		{
			const sint32 unit = t.textureFetch.textureIndex;
			if (unit < 0 || unit >= LATTE_NUM_MAX_TEX_UNITS)
				return; // invalid unit, nothing sensible to generate
			const Latte::E_DIM dim = m_shader->textureUnitDim[unit];
			out << gpr(t.dstGpr) << ".";
			const sint32 lanes = emitDestinationMask(t);
			uint32 op = t.opcode;
			// vertex shaders have no derivatives; the depth compare LOD-zero variant is replaced by the implicit one
			if (stage() == LatteConst::ShaderType::Vertex && op == GPU7_TEX_INST_SAMPLE_C_LZ)
				op = GPU7_TEX_INST_SAMPLE_C;
			const auto& tf = t.textureFetch;
			const bool hasOffset = tf.offsetX != 0 || tf.offsetY != 0 || tf.offsetZ != 0;

			// result conversion into the register type
			if (m_shader->textureIsIntegerFormat[unit])
			{
				if (defType() == kInt)
				{
					if (lanes == 1)
						out << " = int(";
					else
						out.format(" = ivec{}(", lanes);
				}
				else if (defType() == kFloat)
					out << " = uintBitsToFloat(";
			}
			else
			{
				if (defType() == kInt)
					out << " = floatBitsToInt(";
				else if (defType() == kFloat)
					out << " = (";
			}

			if (op == GPU7_TEX_INST_FETCH4 && (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY))
			{
				// gather has no 1D form; return zero
				if (lanes == 1)
					out << "0.0";
				else
					out.format("vec{}(0.0)", lanes);
				out << ");\r\n";
				return;
			}

			const bool allUnnormalized = tf.unnormalized[0] && tf.unnormalized[1] && tf.unnormalized[2] && tf.unnormalized[3];
			const bool texelCoords = op == GPU7_TEX_INST_LD || (op == GPU7_TEX_INST_SAMPLE && allUnnormalized);
			if (hasOffset && (texelCoords || op == GPU7_TEX_INST_FETCH4))
				cemu_assert_unimplemented();
			out << sampleFunction(op, texelCoords, hasOffset);
			out << textureName(unit) << ", ";
			if (op == GPU7_TEX_INST_FETCH4 && dim == Latte::E_DIM::DIM_2D)
			{
				// tiny bias so truncating texel selection does not drop a row/column on some drivers
				out << "vec2(0.0001) + ";
			}

			const bool isCompare = op == GPU7_TEX_INST_SAMPLE_C || op == GPU7_TEX_INST_SAMPLE_C_L || op == GPU7_TEX_INST_SAMPLE_C_LZ;
			if (texelCoords)
			{
				const sint32 coordType = op == GPU7_TEX_INST_LD ? kInt : kFloat;
				// the coordinates are scaled for resolution-scaled render targets
				if (dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_MSAA)
				{
					out << "ivec2(vec2(";
					emitCoord(t, 0, coordType);
					out << ", ";
					emitCoord(t, 1, coordType);
					out.format(")*uf_tex{}Scale), ", unit);
				}
				else if (dim == Latte::E_DIM::DIM_1D)
				{
					out << "int(float(";
					emitCoord(t, 0, coordType);
					out.format(")*uf_tex{}Scale.x), ", unit);
				}
				else
					cemu_assert_debug(false);
				if (dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_MSAA || dim == Latte::E_DIM::DIM_1D)
				{
					if (op == GPU7_TEX_INST_LD)
						emitCoord(t, 3, kInt); // mip level
					else
						out << "0";
				}
			}
			else
			{
				if (isCompare)
				{
					// the reference value is the last coordinate of the shadow lookup
					if (dim == Latte::E_DIM::DIM_2D_ARRAY)
					{
						out << "vec4(";
						emitCoord(t, 0, kFloat);
						out << ",";
						emitCoord(t, 1, kFloat);
						out << ",";
						emitCoord(t, 2, kFloat);
						out << "," << coordSwizzle(t.srcGpr, tf.srcSel[3]) << ")";
					}
					else if (dim == Latte::E_DIM::DIM_CUBEMAP)
						emitCubeCoords(t);
					else if (dim == Latte::E_DIM::DIM_1D)
						out << "vec3(" << coordSwizzle(t.srcGpr, tf.srcSel[0]) << ",0.0," << coordSwizzle(t.srcGpr, tf.srcSel[3]) << ")";
					else
					{
						// SAMPLE_C_L carries the LOD in w, so the reference moves to z
						const sint32 refSel = (uint8)(op == GPU7_TEX_INST_SAMPLE_C_L ? tf.srcSel[2] : tf.srcSel[3]);
						out << "vec3(" << coordSwizzle(t.srcGpr, tf.srcSel[0], tf.srcSel[1]) << ", " << coordSwizzle(t.srcGpr, refSel) << ")";
					}
				}
				else if (dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_2D_ARRAY)
				{
					out << "vec3(";
					emitCoord(t, 0, kFloat);
					out << ",";
					emitCoord(t, 1, kFloat);
					out << ",";
					emitCoord(t, 2, kFloat);
					out << ")";
				}
				else if (dim == Latte::E_DIM::DIM_CUBEMAP)
				{
					cemu_assert_debug(tf.srcSel[0] < 4 && tf.srcSel[1] < 4);
					emitCubeCoords(t);
				}
				else if (dim == Latte::E_DIM::DIM_1D)
					out << coordSwizzle(t.srcGpr, tf.srcSel[0]);
				else
				{
					out << "vec2(";
					emitCoord(t, 0, kFloat);
					out << ",";
					emitCoord(t, 1, kFloat);
					out << ")";
					// nudge coordinates off exact texel edges
					if (ActiveSettings::ForceSamplerRoundToPrecision())
						out << "+ vec2(1.0)/vec2(textureSize(" << textureName(unit) << ", 0))/512.0";
				}
				if (op == GPU7_TEX_INST_SAMPLE_L || op == GPU7_TEX_INST_SAMPLE_C_L)
				{
					out << ",";
					emitCoord(t, 3, kFloat);
				}
				else if (op == GPU7_TEX_INST_SAMPLE_LZ || op == GPU7_TEX_INST_SAMPLE_C_LZ)
					out << ",0.0";
			}

			if (op == GPU7_TEX_INST_SAMPLE_G)
			{
				if (dim == Latte::E_DIM::DIM_1D)
					out << ",gradH.x,gradV.x";
				else if (dim == Latte::E_DIM::DIM_2D)
					out << ",gradH.xy,gradV.xy";
				else
					cemu_assert_unimplemented();
			}

			const bool offsetCapable = op == GPU7_TEX_INST_SAMPLE || op == GPU7_TEX_INST_SAMPLE_L || op == GPU7_TEX_INST_SAMPLE_LB ||
				op == GPU7_TEX_INST_SAMPLE_LZ || op == GPU7_TEX_INST_SAMPLE_G || op == GPU7_TEX_INST_SAMPLE_C || op == GPU7_TEX_INST_SAMPLE_C_LZ;
			if (offsetCapable && hasOffset)
			{
				// offsets are encoded in half texels
				if ((tf.offsetX & 1) || (tf.offsetY & 1) || (tf.offsetZ & 1))
					cemu_assert_unimplemented();
				const sint32 ox = tf.offsetX / 2, oy = tf.offsetY / 2, oz = tf.offsetZ / 2;
				if (dim == Latte::E_DIM::DIM_1D)
					out.format(",{}", ox);
				else if (dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_ARRAY)
					out.format(",ivec2({},{})", ox, oy);
				else if (dim == Latte::E_DIM::DIM_3D)
					out.format(",ivec3({},{},{})", ox, oy, oz);
				else
					cemu_assert_unimplemented();
			}

			if (op == GPU7_TEX_INST_SAMPLE_LB)
			{
				out << ",";
				emitCoord(t, 3, kFloat);
			}

			if (op == GPU7_TEX_INST_SAMPLE_C || op == GPU7_TEX_INST_SAMPLE_C_LZ)
			{
				// shadow lookups return a scalar; replicate it
				out << ")";
				if (lanes > 1)
				{
					out << ".";
					for (sint32 f = 0; f < lanes; f++)
					{
						cemu_assert_debug(t.dstSel[f] == 0);
						out << "x";
					}
				}
			}
			else
			{
				out << ").";
				// FETCH4 returns texels in a different corner order than textureGather
				static constexpr uint8 kGatherOrder[4] = { 2, 0, 1, 3 };
				for (sint32 f = 0; f < 4; f++)
				{
					const sint32 sel = t.dstSel[f];
					if (sel < 4)
						out << laneName(op == GPU7_TEX_INST_FETCH4 ? kGatherOrder[sel] : sel);
					else if (sel != 7)
						cemu_assert_unimplemented();
				}
			}
			out << ");";
#ifdef CEMU_DEBUG_ASSERT
			switch (t.opcode)
			{
			case GPU7_TEX_INST_LD: out << " // TEX_INST_LD"; break;
			case GPU7_TEX_INST_SAMPLE: out << " // TEX_INST_SAMPLE"; break;
			case GPU7_TEX_INST_SAMPLE_L: out << " // TEX_INST_SAMPLE_L"; break;
			case GPU7_TEX_INST_SAMPLE_LZ: out << " // TEX_INST_SAMPLE_LZ"; break;
			case GPU7_TEX_INST_SAMPLE_C: out << " // TEX_INST_SAMPLE_C"; break;
			case GPU7_TEX_INST_SAMPLE_G: out << " // TEX_INST_SAMPLE_G"; break;
			default: out.format(" // 0x{:02x}", t.opcode); break;
			}
			if (t.opcode != op)
				out.format(" (applied as 0x{:02x})", op);
			out.format(" OffsetXYZ {:02x} {:02x} {:02x}", (uint8)tf.offsetX & 0xFF, (uint8)tf.offsetY & 0xFF, (uint8)tf.offsetZ & 0xFF);
#endif
			out << "\r\n";
		}

		// writes each unmasked destination lane from a local vec4/ivec4 named 'source'
		template<typename TSelect>
		void emitLaneResults(const TEXIns& t, sint32 valueType, TSelect&& select)
		{
			for (sint32 i = 0; i < 4; i++)
			{
				const uint8 sel = (uint8)t.dstSel[i];
				if (sel == 7)
					continue;
				out << gpr(t.dstGpr) << "." << laneName(i) << " = ";
				out << castOpen(valueType, defType());
				if (sel < 4)
					select(sel);
				else
				{
					cemu_assert_debug(sel == 4 || sel == 5);
					select(sel == 5 ? 5 : 4);
				}
				out << castClose(valueType, defType()) << ";\r\n";
			}
		}

		void emitTextureInfo(const TEXIns& t)
		{
			const std::string tex = textureName(t.textureFetch.textureIndex);
			const Latte::E_DIM dim = textureDim(t.textureFetch.textureIndex);
			out << "{\r\n";
			out.format("int levels = textureQueryLevels({});\r\n", tex);
			out << "int mip = clamp(";
			emitCoord(t, 3, kInt);
			out << ", 0, levels - 1);\r\n";
			// pack as (width, height, depth/layers, levels)
			if (dim == Latte::E_DIM::DIM_1D)
				out.format("ivec4 info = ivec4(textureSize({}, mip), 0, 0, levels);\r\n", tex);
			else if (dim == Latte::E_DIM::DIM_1D_ARRAY || dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_MSAA)
				out.format("ivec4 info = ivec4(textureSize({}, mip), 0, levels);\r\n", tex);
			else if (dim == Latte::E_DIM::DIM_CUBEMAP)
				out.format("ivec4 info = ivec4(textureSize({}, mip).xy, 0, levels);\r\n", tex);
			else
				out.format("ivec4 info = ivec4(textureSize({}, mip), levels);\r\n", tex);
			emitLaneResults(t, kInt, [&](uint8 sel) {
				if (sel < 4)
					out << "info." << laneName(sel);
				else
					out << (sel == 5 ? "1" : "0");
			});
			out << "}\r\n";
		}

		void emitTextureLod(const TEXIns& t)
		{
			const std::string tex = textureName(t.textureFetch.textureIndex);
			const Latte::E_DIM dim = textureDim(t.textureFetch.textureIndex);
			sint32 n = 2;
			if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY)
				n = 1;
			else if (dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_CUBEMAP)
				n = 3;
			out << "{\r\n";
			out.format("float lod = textureQueryLod({}, ", tex);
			if (n > 1)
				out.format("vec{}(", n);
			for (sint32 i = 0; i < n; i++)
			{
				if (i != 0)
					out << ", ";
				emitCoord(t, i, kFloat);
			}
			if (n > 1)
				out << ")";
			out << ").y;\r\n";
			// even lanes: computed LOD, odd lanes: the mip level actually sampled
			out.format("float level = clamp(floor(lod + 0.5), 0.0, float(textureQueryLevels({}) - 1));\r\n", tex);
			emitLaneResults(t, kFloat, [&](uint8 sel) {
				if (sel < 4)
					out << ((sel & 1) ? "level" : "lod");
				else
					out << (sel == 5 ? "1.0" : "0.0");
			});
			out << "}\r\n";
		}

		void emitGradientQuery(const TEXIns& t)
		{
			out << "{\r\n";
			out << "vec4 gradient = " << (t.opcode == GPU7_TEX_INST_GET_GRADIENTS_H ? "dFdx" : "dFdy") << "(vec4(";
			for (sint32 i = 0; i < 4; i++)
			{
				if (i != 0)
					out << ", ";
				emitCoord(t, i, kFloat);
			}
			out << "));\r\n";
			emitLaneResults(t, kFloat, [&](uint8 sel) {
				if (sel < 4)
					out << "gradient." << laneName(sel);
				else
					out << (sel == 5 ? "1.0" : "0.0");
			});
			out << "}\r\n";
		}

		void emitGradientSet(const TEXIns& t)
		{
			out << (t.opcode == GPU7_TEX_INST_SET_GRADIENTS_H ? "gradH = " : "gradV = ") << "vec4(";
			for (sint32 i = 0; i < 4; i++)
			{
				if (i != 0)
					out << ", ";
				emitCoord(t, i, kFloat);
			}
			out << ");\r\n";
		}

		void emitCubemapIndex(const TEXIns& t)
		{
			static constexpr std::string_view kSel[8] = { "x", "y", "z", "w", "_", "_", "_", "_" };
			const std::string_view lane = kSel[t.textureFetch.srcSel[0] & 7];
			// note: addresses the register variable by name even in array mode
			out.format("cubeMapArrayIndex{}", t.textureFetch.textureIndex);
			if (defType() == kInt)
				out.format(" = intBitsToFloat(R{}i.{});\r\n", t.srcGpr, lane);
			else if (defType() == kFloat)
				out.format(" = R{}f.{};\r\n", t.srcGpr, lane);
			else
				cemu_assert_unimplemented();
		}

		// geometry shaders read their vertex inputs through VFETCH from the ring
		void emitRingInputFetch(const TEXIns& t)
		{
			out << gpr(t.dstGpr) << ".";
			emitDestinationMask(t);
			out << " = " << castOpen(kInt, defType()) << "(v2g[";
			if (t.textureFetch.srcSel[0] >= 4 || t.textureFetch.srcSel[1] >= 4)
				cemu_assert_unimplemented();
			// the vertex index is assumed to be 0
			out.format("0].passV2GParameter{}.", t.textureFetch.offset / 16);
			emitSelectedLanes(t);
			out << ")" << castClose(kInt, defType()) << ";\r\n";
		}

		// VFETCH from a constant buffer (dynamic uniform indexing)
		void emitBufferFetch(const TEXIns& t)
		{
			if (t.textureFetch.textureIndex == 0x9F && stage() == LatteConst::ShaderType::Geometry)
			{
				emitRingInputFetch(t);
				return;
			}
			static constexpr std::string_view kSel[8] = { "x", "y", "z", "w", "_", "_", "_", "_" };
			out << gpr(t.dstGpr) << ".";
			emitDestinationMask(t);
			out << " = " << (defType() == kInt ? "floatBitsToInt(" : "(");
			out << uniformBlockVar(t.textureFetch.textureIndex - 0x80) << "[";
			const std::string_view lane = kSel[t.textureFetch.srcSel[0] & 7];
			if (defType() == kInt)
				out << gpr(t.srcGpr) << "." << lane;
			else
				out << "floatBitsToInt(" << gpr(t.srcGpr) << "." << lane << ")";
			out << "].";
			emitSelectedLanes(t);
			out << ");\r\n";
		}

		// scatter buffer reads are not implemented, the result is zero
		void emitMemoryRead(const TEXIns& t)
		{
			out << gpr(t.dstGpr) << ".";
			const sint32 lanes = emitDestinationMask(t);
			out << " = " << (defType() == kInt ? "floatBitsToInt(" : "(");
			sint32 available = 0;
			switch (t.memRead.format)
			{
			case Latte::E_HWFMT::HWFMT_32_FLOAT: available = 1; out << "0.0"; break;
			case Latte::E_HWFMT::HWFMT_32_32_FLOAT: available = 2; out << "vec2(0.0,0.0)"; break;
			case Latte::E_HWFMT::HWFMT_32_32_32_FLOAT: available = 3; out << "vec3(0.0,0.0,0.0)"; break;
			default: cemu_assert_unimplemented(); break;
			}
			if (lanes < available && lanes >= 1 && lanes <= 3)
				out << "." << std::string_view("xyz").substr(0, lanes);
			out << ");\r\n";
		}

		void emitTEXClause(const CFIns& cf)
		{
			cemu_assert_debug(cf.instructionsALU.empty());
			for (const TEXIns& t : cf.instructionsTEX)
			{
				const uint32 op = t.opcode;
				if (isTextureSampleOp(op))
					emitTextureSample(t);
				else if (op == GPU7_TEX_INST_GET_TEXTURE_RESINFO)
					emitTextureInfo(t);
				else if (op == GPU7_TEX_INST_GET_COMP_TEX_LOD)
					emitTextureLod(t);
				else if (op == GPU7_TEX_INST_SET_CUBEMAP_INDEX)
					emitCubemapIndex(t);
				else if (op == GPU7_TEX_INST_GET_GRADIENTS_H || op == GPU7_TEX_INST_GET_GRADIENTS_V)
					emitGradientQuery(t);
				else if (op == GPU7_TEX_INST_SET_GRADIENTS_H || op == GPU7_TEX_INST_SET_GRADIENTS_V)
					emitGradientSet(t);
				else if (op == GPU7_TEX_INST_VFETCH)
					emitBufferFetch(t);
				else if (op == GPU7_TEX_INST_MEM)
					emitMemoryRead(t);
				else
					cemu_assert_unimplemented();
			}
		}

		// ---------------------------------------------------------------
		// exports and memory writes
		// ---------------------------------------------------------------

		// constructor of the exported vector: vecN(...) / ivecN(...)
		void emitExportValue(const CFIns& cf, sint32 type, uint32 burstIndex)
		{
			const bool isRing = cf.type == GPU7_CF_INST_MEM_RING_WRITE;
			const uint32 count = isRing ? (uint32)std::popcount((uint32)(cf.memWriteCompMask & 0xF)) : 4;
			if (type == kFloat)
			{
				if (count == 1)
					out << "float(";
				else
					out.format("vec{}(", count);
			}
			else if (type == kInt)
			{
				if (count == 1)
					out << "int(";
				else
					out.format("ivec{}(", count);
			}
			else
				cemu_assert_unimplemented();
			bool first = true;
			for (uint32 i = 0; i < 4; i++)
			{
				uint32 sel;
				if (isRing)
				{
					if ((cf.memWriteCompMask & (1u << i)) == 0)
						continue;
					sel = i;
				}
				else
					sel = cf.exportComponentSel[i];
				if (!first)
					out << ", ";
				first = false;
				if (sel < 4)
					emitRegisterLane((sint32)(cf.exportSourceGPR + burstIndex), (sint32)sel, type);
				else if (sel == 5)
					out << "1.0";
				else
				{
					// 4 = constant 0, 7 = masked (exports 0)
					cemu_assert_debug(sel == 4 || sel == 7);
					out << "0";
				}
			}
			out << ")";
		}

		bool viewportTransformDisabled() const
		{
			const auto& vte = m_ctx->contextRegistersNew->PA_CL_VTE_CNTL;
			return !vte.get_VPORT_X_SCALE_ENA() || !vte.get_VPORT_Y_SCALE_ENA() || !vte.get_VPORT_Z_SCALE_ENA();
		}

		void emitVertexExport(const CFIns& cf)
		{
			if (cf.exportBurstCount != 0 && cf.exportType != 2)
				cemu_assert_unimplemented();
			if (cf.exportType == 1 && cf.exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION)
			{
				if (viewportTransformDisabled())
				{
					// position is already in window space; convert back to clip space
					out << "vec4 finalPos = ";
					emitExportValue(cf, kFloat, 0);
					out << ";\r\n";
					out << "finalPos.xy = finalPos.xy * uf_windowSpaceToClipSpaceTransform - vec2(1.0,1.0);";
					out << "SET_POSITION(finalPos);";
				}
				else
				{
					out << "SET_POSITION(";
					emitExportValue(cf, kFloat, 0);
					out << ");\r\n";
				}
			}
			else if (cf.exportType == 1 && cf.exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_POINT_SIZE)
			{
				if (facts().outputPointSize)
				{
					cemu_assert_debug(facts().writesPointSize);
					out << "gl_PointSize = (";
					emitExportValue(cf, kFloat, 0);
					out << ").x;\r\n";
				}
			}
			else if (cf.exportType == 2 && cf.exportArrayBase < 32)
			{
				for (uint32 b = 0; b <= cf.exportBurstCount; b++)
				{
					const uint32 param = cf.exportArrayBase + b;
					if (param >= 32)
					{
						cemu_assert_unimplemented();
						break;
					}
					const sint32 semanticId = _getVertexShaderOutParamSemanticId(m_ctx->contextRegisters, (sint32)param);
					if (semanticId == 0xFF)
					{
						out << "// skipped export to semanticId 255\r\n";
						continue;
					}
					// masked lanes keep what an earlier export wrote
					std::string mask;
					for (uint32 c = 0; c < 4; c++)
					{
						if (cf.exportComponentSel[c] != 7)
							mask += laneName((sint32)c);
					}
					if (mask.empty())
						continue;
					out.format("passParameterSem{}", semanticId);
					if (mask.size() < 4)
						out << "." << mask;
					out << " = ";
					emitExportValue(cf, kFloat, b);
					if (mask.size() < 4)
						out << "." << mask;
					out << ";\r\n";
				}
			}
			else
				cemu_assert_unimplemented();
		}

		void emitPixelExport(const CFIns& cf)
		{
			if (cf.exportType == 0 && cf.exportArrayBase < 8)
			{
				const auto& alphaCtl = m_ctx->contextRegistersNew->SX_ALPHA_TEST_CONTROL;
				const bool alphaTest = alphaCtl.get_ALPHA_TEST_ENABLE();
				const auto alphaFunc = alphaCtl.get_ALPHA_FUNC();
				for (uint32 i = 0; i <= cf.exportBurstCount; i++)
				{
					const sint32 target = LatteDecompiler_getColorOutputIndexFromExportIndex(m_ctx, (sint32)(cf.exportArrayBase + i));
					// alpha test is evaluated on the value written to the first color target
					if (target == 0 && alphaTest && alphaFunc == Latte::E_COMPAREFUNC::NEVER)
						out << "discard;\r\n";
					else if (target == 0 && alphaTest && alphaFunc != Latte::E_COMPAREFUNC::ALWAYS)
					{
						out << "if( ((";
						emitExportValue(cf, kFloat, i);
						out << ").a ";
						switch (alphaFunc)
						{
						case Latte::E_COMPAREFUNC::LESS: out << "<"; break;
						case Latte::E_COMPAREFUNC::EQUAL: out << "=="; break;
						case Latte::E_COMPAREFUNC::LEQUAL: out << "<="; break;
						case Latte::E_COMPAREFUNC::GREATER: out << ">"; break;
						case Latte::E_COMPAREFUNC::NOTEQUAL: out << "!="; break;
						case Latte::E_COMPAREFUNC::GEQUAL: out << ">="; break;
						default: break;
						}
						out << " uf_alphaTestRef) == false) discard;\r\n";
					}
					out.format("passPixelColor{} = ", target);
					emitExportValue(cf, kFloat, i);
					out << ";\r\n";
					if (cf.exportArrayBase + i >= 8)
						cemu_assert_unimplemented();
				}
			}
			else if (cf.exportType == 0 && cf.exportArrayBase == 61)
			{
				// depth (x); stencil reference and the other lanes are not supported
				if (cf.exportBurstCount > 0 || cf.exportComponentSel[0] == 7 || cf.exportComponentSel[1] != 7 ||
					cf.exportComponentSel[2] != 7 || cf.exportComponentSel[3] != 7)
					cemu_assert_unimplemented();
				out << "gl_FragDepth = ";
				emitExportValue(cf, kFloat, 0);
				out << ".x;\r\n";
			}
			else
				cemu_assert_unimplemented();
		}

		void emitExport(const CFIns& cf)
		{
			out << "// export\r\n";
			if (stage() == LatteConst::ShaderType::Vertex)
				emitVertexExport(cf);
			else if (stage() == LatteConst::ShaderType::Pixel)
				emitPixelExport(cf);
		}

		void emitLaneMask(uint32 mask)
		{
			for (sint32 c = 0; c < 4; c++)
			{
				if (mask & (1u << c))
					out << laneName(c);
			}
		}

		// streamout destination of one dword
		void emitStreamoutSlot(uint32 bufferIndex, uint32 dwordIndex)
		{
			if (m_ctx->options->useTFViaSSBO)
				out.format("sb_buffer[sbBase{} + {}]", bufferIndex, dwordIndex);
			else
				out.format("sb{}[{}]", bufferIndex, dwordIndex);
		}

		void emitRingWrite(const CFIns& cf)
		{
			const uint32 vertexBytes = std::max<uint32>(m_ctx->contextRegisters[mmSQ_GS_VERT_ITEMSIZE] * 4, 1);
			const uint32 offsetInVertex = (cf.exportArrayBase * 4) % vertexBytes;
			if (stage() == LatteConst::ShaderType::Geometry && facts().hasStreamoutEnable)
			{
				// with streamout the GS output goes straight to the buffers the copy shader would write
				for (uint32 b = 0; b <= cf.exportBurstCount; b++)
				{
					const uint32 offset = (cf.exportArrayBase * 4 + b * 0x10) % vertexBytes;
					const LatteGSCopyShaderStreamWrite_t* sw = nullptr;
					for (const auto& w : m_ctx->parsedGSCopyShader->list_streamWrites)
					{
						if (w.offset == offset)
						{
							sw = &w;
							break;
						}
					}
					if (!sw)
					{
						cemu_assert_suspicious();
						return;
					}
					for (sint32 c = 0; c < 4; c++)
					{
						if ((cf.memWriteCompMask & (1u << c)) == 0)
							continue;
						emitStreamoutSlot(sw->bufferIndex, sw->exportArrayBase + c);
						out << " = " << castOpen(defType(), kInt);
						out << gpr((sint32)(cf.exportSourceGPR + b)) << "." << laneName(c);
						out << castClose(defType(), kInt) << ";\r\n";
					}
				}
				return;
			}
			if (stage() == LatteConst::ShaderType::Vertex)
			{
				// vertex shader feeding a geometry shader
				if (cf.memWriteElemSize != 3 || (cf.exportArrayBase & 3) != 0)
					cemu_assert_unimplemented();
				for (uint32 b = 0; b <= cf.exportBurstCount; b++)
				{
					out.format("v2g.passV2GParameter{}.", cf.exportArrayBase / 4 + b);
					emitLaneMask(cf.memWriteCompMask);
					out << " = ";
					emitExportValue(cf, kInt, b);
					out << ";\r\n";
				}
			}
			else if (stage() == LatteConst::ShaderType::Geometry)
			{
				// the copy shader tells which output a ring offset belongs to
				cemu_assert_debug(cf.memWriteElemSize == 3 && (cf.exportArrayBase & 3) == 0);
				for (uint32 b = 0; b <= cf.exportBurstCount; b++)
				{
					uint32 exportType = 0;
					uint32 exportBase = 0;
					const uint32 ringOffset = offsetInVertex + b * (cf.memWriteElemSize + 1) * 4;
					if (!LatteGSCopyShaderParser_getExportTypeByOffset(m_ctx->parsedGSCopyShader, ringOffset, &exportType, &exportBase))
					{
						cemu_assert_debug(false);
						m_ctx->hasError = true;
						return;
					}
					if (exportType == 1 && exportBase == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION)
					{
						out << "{\r\n";
						out << "vec4 pos = vec4(0.0,0.0,0.0,1.0);\r\n";
						out << "pos.";
						emitLaneMask(cf.memWriteCompMask);
						out << " = ";
						emitExportValue(cf, kFloat, b);
						out << ";\r\n";
						out << "SET_POSITION(pos);\r\n";
						out << "}\r\n";
					}
					else if (exportType == 2 && exportBase < 16)
					{
						out.format("passG2PParameter{}.", exportBase);
						emitLaneMask(cf.memWriteCompMask);
						out << " = ";
						emitExportValue(cf, kFloat, b);
						out << ";\r\n";
					}
					else
						cemu_assert_debug(false);
				}
			}
		}

		void emitStreamWrite(const CFIns& cf)
		{
			if (!facts().hasStreamoutEnable)
			{
#ifdef CEMU_DEBUG_ASSERT
				out << "// omitted streamout write\r\n";
#endif
				return;
			}
			const uint32 buffer = cf.type == GPU7_CF_INST_MEM_STREAM0_WRITE ? 0 : 1;
			if (stage() != LatteConst::ShaderType::Vertex)
			{
				cemu_assert_debug(false);
				return;
			}
			const uint32 elements = cf.memWriteArraySize + 1;
			// the component mask has four bits, so at most four dwords are written
			for (uint32 i = 0; i < elements && i < 4; i++)
			{
				if ((cf.memWriteCompMask & (1u << i)) == 0)
					continue;
				emitStreamoutSlot(buffer, cf.exportArrayBase + i);
				out << " = " << castOpen(defType(), kInt);
				out << gpr((sint32)cf.exportSourceGPR) << "." << laneName((sint32)i);
				out << castClose(defType(), kInt) << ";\r\n";
			}
		}

		// subroutines are inlined at the call site, with their own mask stack
		void emitCall(const CFIns& cf)
		{
			LatteDecompilerSubroutineInfo* sub = nullptr;
			for (auto& s : m_ctx->list_subroutines)
			{
				if (s.cfAddr == cf.addr)
				{
					sub = &s;
					break;
				}
			}
			if (!sub || m_ctx->isSubroutine)
			{
				cemu_assert_debug(false); // unknown target or nested call
				return;
			}
			out.format("activeMaskStackSub{:04x}[0] = {};\r\n", sub->cfAddr, combinedMaskName(cf.activeStackDepth + 1));
			out.format("activeMaskStackCSub{0:04x}[0] = activeMaskStackSub{0:04x}[0];\r\n", sub->cfAddr);
			out.format("activeMaskStackCSub{0:04x}[1] = activeMaskStackSub{0:04x}[0];\r\n", sub->cfAddr);
			m_ctx->isSubroutine = true;
			m_ctx->subroutineInfo = sub;
			for (auto& inner : sub->instructions)
				emitControlFlow(inner);
			m_ctx->isSubroutine = false;
			m_ctx->subroutineInfo = nullptr;
		}

		// ---------------------------------------------------------------
		// control flow
		// ---------------------------------------------------------------

		// combined[target] = pushed[src] && combined[src]
		void emitMaskCombine(sint32 target, sint32 src)
		{
			out << combinedMaskName(target) << " = " << maskName(src) << " == true && " << combinedMaskName(src) << " == true;\r\n";
		}

		void emitMaskInvert(sint32 d)
		{
			out << maskName(d) << " = " << maskName(d) << " == false;\r\n";
			emitMaskCombine(d + 1, d);
		}

		void openMaskedBlock(sint32 level)
		{
			out << "if( " << combinedMaskName(level) << " == true ) {\r\n";
		}

		void emitControlFlow(const CFIns& cf)
		{
			const sint32 d = cf.activeStackDepth;
			const bool masked = facts().modifiesPixelActiveState;
			const uint32 type = cf.type;
			if (isALUClauseType(type))
			{
				const bool push = type == GPU7_CF_INST_ALU_PUSH_BEFORE;
				if (masked)
					openMaskedBlock(push ? d : d + 1);
				if (push)
				{
					out << maskName(d) << " = " << maskName(d - 1) << ";\r\n";
					out << combinedMaskName(d + 1) << " = " << combinedMaskName(d) << ";\r\n";
				}
				if (type == GPU7_CF_INST_ALU_BREAK)
					out << "bool aluBreak = false;\r\n";
				emitALUClause(cf);
				if (type == GPU7_CF_INST_ALU_BREAK)
					out << "if (aluBreak) break;\r\n";
				if (masked)
					out << "}\r\n";
				cemu_assert_debug(masked || type == GPU7_CF_INST_ALU);
				if (push)
				{
					// lanes that did not enter the clause are inactive at the new level
					out << "else {\r\n";
					out << maskName(d) << " = false;\r\n";
					out << combinedMaskName(d + 1) << " = false;\r\n";
					out << "}\r\n";
				}
				if (type == GPU7_CF_INST_ALU_POP_AFTER)
					emitMaskCombine(d, d - 1);
				else if (type == GPU7_CF_INST_ALU_POP2_AFTER)
					emitMaskCombine(d - 1, d - 2);
				else if (type == GPU7_CF_INST_ALU_ELSE_AFTER)
				{
					cemu_assert_debug(cf.popCount == 0);
					emitMaskInvert(d);
				}
				return;
			}
			switch (type)
			{
			case GPU7_CF_INST_TEX:
				if (masked)
					openMaskedBlock(d + 1);
				emitTEXClause(cf);
				if (masked)
					out << "}\r\n";
				break;
			case GPU7_CF_INST_EXPORT:
			case GPU7_CF_INST_EXPORT_DONE:
				emitExport(cf);
				break;
			case GPU7_CF_INST_ELSE:
				emitMaskInvert(d);
				break;
			case GPU7_CF_INST_POP:
			{
				const sint32 pc = (sint32)cf.popCount;
				emitMaskCombine(d + 1 - pc, d - pc);
				break;
			}
			case GPU7_CF_INST_LOOP_START_DX10:
			case GPU7_CF_INST_LOOP_START_NO_AL:
				// loops are skipped entirely when no lane is active
				out << "{\r\n";
				out << "bool loopActive = " << combinedMaskName(d + 1) << ";\r\n";
				if (ActiveSettings::ShaderPreventInfiniteLoopsEnabled())
				{
					const sint32 id = (sint32)cf.cfAddr;
					out.format("int loopCounter{} = 0;\r\n", id);
					out.format("while( {} == true && loopCounter{} < 500 )\r\n", combinedMaskName(d + 1), id);
					out << "{\r\n";
					out.format("loopCounter{}++;\r\n", id);
				}
				else
				{
					out << "while( " << combinedMaskName(d + 1) << " == true )\r\n";
					out << "{\r\n";
				}
				break;
			case GPU7_CF_INST_LOOP_END:
				cemu_assert_debug(cf.popCount == 0);
				out << "}\r\n";
				out << combinedMaskName(d + 1) << " = loopActive;\r\n";
				out << "}\r\n";
				break;
			case GPU7_CF_INST_LOOP_BREAK:
				cemu_assert_debug(cf.popCount == 0);
				if (masked)
					openMaskedBlock(d + 1);
				out << "break;\r\n";
				if (masked)
					out << "}\r\n";
				break;
			case GPU7_CF_INST_MEM_STREAM0_WRITE:
			case GPU7_CF_INST_MEM_STREAM1_WRITE:
				emitStreamWrite(cf);
				break;
			case GPU7_CF_INST_MEM_RING_WRITE:
				emitRingWrite(cf);
				break;
			case GPU7_CF_INST_EMIT_VERTEX:
				if (masked)
					openMaskedBlock(d + 1);
				if (facts().outputPointSize && !facts().writesPointSize)
					out << "gl_PointSize = uf_pointSize;\r\n";
				out << "EmitVertex();\r\n";
				if (facts().useSSBOForStreamout)
				{
					for (sint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++)
					{
						if (!m_ctx->output->streamoutBufferWriteMask[i])
							continue;
						cemu_assert_debug((m_ctx->output->streamoutBufferStride[i] & 3) == 0);
						out.format("sbBase{} += {};\r\n", i, m_ctx->output->streamoutBufferStride[i] / 4);
					}
				}
				if (masked)
					out << "}\r\n";
				break;
			case GPU7_CF_INST_CALL:
				emitCall(cf);
				break;
			case GPU7_CF_INST_RETURN:
				break; // end of an inlined subroutine
			default:
				cemu_assert_debug(false);
				break;
			}
		}

		// ---------------------------------------------------------------
		// prologue
		// ---------------------------------------------------------------

		void emitHelperFunctions()
		{
			if (facts().hasRedcCUBE)
			{
				out << "void redcCUBE(vec4 src0, vec4 src1, out vec3 stm, out int faceId)\r\n"
					"{\r\n"
					"float x = src1.y, y = src1.x, z = src0.x;\r\n"
					"float ax = abs(x);\r\n"
					"float ay = abs(y);\r\n"
					"float az = abs(z);\r\n"
					"if (az >= ax && az >= ay)\r\n"
					"{\r\n"
					"stm = vec3(-y, z < 0.0 ? -x : x, z * 2.0);\r\n"
					"faceId = z < 0.0 ? 5 : 4;\r\n"
					"}\r\n"
					"else if (ay >= ax)\r\n"
					"{\r\n"
					"stm = vec3(y < 0.0 ? -z : z, x, y * 2.0);\r\n"
					"faceId = y < 0.0 ? 3 : 2;\r\n"
					"}\r\n"
					"else\r\n"
					"{\r\n"
					"stm = vec3(-y, x < 0.0 ? z : -z, x * 2.0);\r\n"
					"faceId = x < 0.0 ? 1 : 0;\r\n"
					"}\r\n"
					"}\r\n";
			}
			if (facts().hasCubeMapTexture)
			{
				out << "vec3 redcCUBEReverse(vec2 st, int faceId)\r\n"
					"{\r\n"
					"vec2 tc = (st - vec2(1.5)) * 2.0;\r\n"
					"if (faceId == 0) return vec3(1.0, -tc.y, -tc.x);\r\n"
					"if (faceId == 1) return vec3(-1.0, -tc.y, tc.x);\r\n"
					"if (faceId == 2) return vec3(tc.x, 1.0, tc.y);\r\n"
					"if (faceId == 3) return vec3(tc.x, -1.0, -tc.y);\r\n"
					"if (faceId == 4) return vec3(tc.x, -tc.y, 1.0);\r\n"
					"return vec3(-tc.x, -tc.y, -1.0);\r\n"
					"}\r\n";
			}
			// clamp of a float stored in an int register; the integer all-ones and
			// INT_MAX patterns behave as boolean results
			out << "int clampFI32(int v)\r\n"
				"{\r\n"
				"if( v == 0x7FFFFFFF )\r\n"
				"\treturn floatBitsToInt(1.0);\r\n"
				"else if( v == 0xFFFFFFFF )\r\n"
				"\treturn floatBitsToInt(0.0);\r\n"
				"return floatBitsToInt(clamp(intBitsToFloat(v), 0.0, 1.0));\r\n"
				"}\r\n";
			if (m_ctx->options->strictMul)
			{
				// 0 * x = 0 even for x = inf/nan; the mix() form is cheaper for NVIDIA's compiler
				if (LatteGPUState.glVendor == GLVENDOR_NVIDIA && !ActiveSettings::DumpShadersEnabled())
					out << "float mul_nonIEEE(float a, float b){return mix(0.0, a*b, (a != 0.0) && (b != 0.0));}\r\n";
				else
					out << "float mul_nonIEEE(float a, float b){ if( a == 0.0 || b == 0.0 ) return 0.0; return a*b; }\r\n";
			}
		}

		// loads one fetch shader attribute into its GPR
		void emitAttributeLoad(LatteParsedFetchShaderAttribute& attrib)
		{
			// the vertex semantic table maps semantics to GPR 1..32 (R0 holds the vertex/instance id)
			sint32 slot = -1;
			for (sint32 f = 0; f < 32; f++)
			{
				if (m_ctx->contextRegisters[mmSQ_VTX_SEMANTIC_0 + f] == attrib.semanticId)
				{
					slot = f;
					break;
				}
			}
			if (slot < 0)
				return;
			const sint32 reg = slot + 1;
			if (!isGPRMarked(reg))
			{
				out.format("// skipped unused attribute for r{}\r\n", reg);
				return;
			}
			LatteDecompiler_emitAttributeDecodeGLSL(m_shader, out.buffer(), &attrib);
			out << gpr(reg) << (defType() == kInt ? " = ivec4(" : " = vec4(");
			// destination swizzle: lanes 0-3 of the decoded value, 4 = 0, 5 = 1 (1.0f for non-integer formats)
			static constexpr std::string_view kSwizzleFloat[6] = { "int(attrDecoder.x)", "int(attrDecoder.y)", "int(attrDecoder.z)", "int(attrDecoder.w)", "0", "0x3f800000" };
			static constexpr std::string_view kSwizzleInt[6] = { "int(attrDecoder.x)", "int(attrDecoder.y)", "int(attrDecoder.z)", "int(attrDecoder.w)", "0", "1" };
			for (sint32 f = 0; f < 4; f++)
			{
				if (f > 0)
					out << ", ";
				uint8 ds = attrib.ds[f];
				if (ds >= 6)
				{
					cemu_assert_unimplemented();
					ds = 4;
				}
				out << castOpen(kInt, defType());
				out << (attrib.nfa != 1 ? kSwizzleFloat[ds] : kSwizzleInt[ds]);
				out << castClose(kInt, defType());
			}
			out << ");\r\n";
		}

		void emitVariables()
		{
			const auto& tt = m_ctx->typeTracker;
			const auto& a = facts();
			if (!tt.useArrayGPRs)
			{
				for (sint32 i = 0; i < 128; i++)
				{
					if (!a.usesRelativeGPRRead && !isGPRMarked(i))
						continue;
					if (tt.genIntReg)
						out.format("ivec4 R{}i = ivec4(0);\r\n", i);
					else if (tt.genFloatReg)
						out.format("vec4 R{}f = vec4(0.0);\r\n", i);
				}
			}
			else
			{
				if (tt.genIntReg)
					out << "ivec4 Ri[128];\r\n";
				else if (tt.genFloatReg)
					out << "vec4 Rf[128];\r\n";
				for (sint32 i = 0; i < 128; i++)
				{
					if (tt.genIntReg)
						out.format("Ri[{}] = ivec4(0);\r\n", i);
					else if (tt.genFloatReg)
						out.format("Rf[{}] = vec4(0.0);\r\n", i);
				}
			}
			if (m_shader->shaderType == LatteConst::ShaderType::Vertex)
				out << "uvec4 attrDecoder;\r\n";
			if (tt.genIntReg)
				out << "int backupReg0i, backupReg1i, backupReg2i, backupReg3i, backupReg4i;\r\n";
			if (tt.genFloatReg)
				out << "float backupReg0f, backupReg1f, backupReg2f, backupReg3f, backupReg4f;\r\n";
			if (tt.genIntReg)
			{
				out << "int PV0ix = 0, PV0iy = 0, PV0iz = 0, PV0iw = 0, PV1ix = 0, PV1iy = 0, PV1iz = 0, PV1iw = 0;\r\n";
				out << "int PS0i = 0, PS1i = 0;\r\n";
				out << "ivec4 tempi = ivec4(0);\r\n";
			}
			if (tt.genFloatReg)
			{
				out << "float PV0fx = 0.0, PV0fy = 0.0, PV0fz = 0.0, PV0fw = 0.0, PV1fx = 0.0, PV1fy = 0.0, PV1fz = 0.0, PV1fw = 0.0;\r\n";
				out << "float PS0f = 0.0, PS1f = 0.0;\r\n";
				out << "vec4 tempf = vec4(0.0);\r\n";
			}
			if (a.hasGradientLookup)
			{
				out << "vec4 gradH;\r\n";
				out << "vec4 gradV;\r\n";
			}
			out << "float tempResultf;\r\n";
			out << "int tempResulti;\r\n";
			out << "ivec4 ARi = ivec4(0);\r\n";
			out << "bool predResult = true;\r\n";
			if (a.modifiesPixelActiveState)
			{
				const sint32 depth = a.activeStackMaxDepth;
				out.format("bool activeMaskStack[{}];\r\n", depth + 1);
				out.format("bool activeMaskStackC[{}];\r\n", depth + 2);
				for (sint32 i = 0; i < depth; i++)
					out.format("activeMaskStack[{}] = false;\r\n", i);
				for (sint32 i = 0; i < depth + 1; i++)
					out.format("activeMaskStackC[{}] = false;\r\n", i);
				out << "activeMaskStack[0] = true;\r\n";
				out << "activeMaskStackC[0] = true;\r\n";
				out << "activeMaskStackC[1] = true;\r\n";
				// subroutines use a single level of their own
				for (const auto& sub : m_ctx->list_subroutines)
				{
					out.format("bool activeMaskStackSub{:04x}[1];\r\n", sub.cfAddr);
					out.format("bool activeMaskStackCSub{:04x}[2];\r\n", sub.cfAddr);
				}
			}
			if (a.hasRedcCUBE)
			{
				out << "vec3 cubeMapSTM;\r\n";
				out << "int cubeMapFaceId;\r\n";
			}
			for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
			{
				if (m_ctx->output->textureUnitMask[i] && m_shader->textureUnitDim[i] == Latte::E_DIM::DIM_CUBEMAP)
					out.format("float cubeMapArrayIndex{} = 0.0;\r\n", i);
			}
			// per-invocation dword offset into each streamout buffer (SSBO path)
			const auto type = m_shader->shaderType;
			if (a.useSSBOForStreamout && (type == LatteConst::ShaderType::Vertex || type == LatteConst::ShaderType::Geometry))
			{
				for (sint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++)
				{
					if (!m_ctx->output->streamoutBufferWriteMask[i])
						continue;
					const uint32 strideDw = m_ctx->output->streamoutBufferStride[i] / 4;
					cemu_assert_debug((m_ctx->output->streamoutBufferStride[i] & 3) == 0);
					if (type == LatteConst::ShaderType::Vertex)
						out.format("int sbBase{0} = uf_streamoutBufferBase{0}/4 + (gl_VertexID + uf_verticesPerInstance * gl_InstanceID)*{1};\r\n", i, strideDw);
					else
					{
						cemu_assert_debug(m_ctx->contextRegisters[mmVGT_GS_OUT_PRIM_TYPE] == 0); // only point output is handled
						out.format("int sbBase{0} = uf_streamoutBufferBase{0}/4 + (gl_PrimitiveIDIn * {1})*{2};\r\n", i, maxGSOutputVertices(false), strideDw);
					}
				}
			}
		}

		// vertices one GS invocation can emit, from the ring item sizes
		uint32 maxGSOutputVertices(bool useEmitCount) const
		{
			const uint32 vertexBytes = m_ctx->contextRegisters[mmSQ_GS_VERT_ITEMSIZE] * 4;
			uint32 n = vertexBytes ? ((m_ctx->contextRegisters[mmSQ_GSVS_RING_ITEMSIZE] & 0x7FFF) * 4) / vertexBytes : 0;
			if (useEmitCount && !facts().hasLoops && facts().numEmitVertex < n)
				n = facts().numEmitVertex;
			return n;
		}

		void emitInputLoads()
		{
			const auto type = m_shader->shaderType;
			if (type == LatteConst::ShaderType::Vertex)
			{
				if (isGPRMarked(0))
				{
					if (defType() == kInt)
						out << gpr(0) << " = ivec4(gl_VertexID, 0, 0, gl_InstanceID);\r\n";
					else if (defType() == kFloat)
						out << gpr(0) << " = floatBitsToInt(ivec4(gl_VertexID, 0, 0, gl_InstanceID));\r\n";
					else
						cemu_assert_unimplemented();
				}
				LatteFetchShader* fs = m_ctx->fetchShader;
				for (auto& group : fs->bufferGroups)
				{
					for (sint32 i = 0; i < group.attribCount; i++)
						emitAttributeLoad(group.attrib[i]);
				}
				// attributes of buffers that do not exist are decoded as zero
				for (auto& group : fs->bufferGroupsInvalid)
				{
					for (sint32 i = 0; i < group.attribCount; i++)
						emitAttributeLoad(group.attrib[i]);
				}
			}
			else if (type == LatteConst::ShaderType::Pixel)
			{
				const LatteShaderPSInputTable* inputs = LatteSHRC_GetPSInputTable();
				const uint32 psControl1 = m_ctx->contextRegisters[mmSPI_PS_IN_CONTROL_1];
				cemu_assert_debug(((m_ctx->contextRegisters[mmSPI_INTERP_CONTROL_0] >> 1) & 1) == 0); // point sprites
				const bool frontFaceEnabled = ((psControl1 >> 8) & 1) != 0;
				const uint32 frontFaceLane = (psControl1 >> 9) & 3;
				const bool frontFaceAllBits = ((psControl1 >> 11) & 1) != 0;
				const uint32 frontFaceReg = (psControl1 >> 12) & 0x1F;
				if (inputs->paramGen != 0)
				{
					// point coordinate generation
					cemu_assert_debug(inputs->paramGen == 1);
					if (defType() == kFloat)
						out << gpr(inputs->paramGenGPR) << " = gl_PointCoord.xyxy;\r\n";
					else
						out << gpr(inputs->paramGenGPR) << " = floatBitsToInt(gl_PointCoord.xyxy);\r\n";
				}
				for (sint32 i = 0; i < inputs->count; i++)
				{
					if (!isGPRMarked(i) && !facts().usesRelativeGPRRead)
						continue;
					const uint32 semanticId = inputs->import[i].semanticId;
					if (semanticId == LATTE_ANALYZER_IMPORT_INDEX_SPIPOSITION)
					{
						out << gpr(i) << (defType() == kFloat ? " = GET_FRAGCOORD();\r\n" : " = floatBitsToInt(GET_FRAGCOORD());\r\n");
						continue;
					}
					std::string source = m_ctx->options->usesGeometryShader ? fmt::format("passG2PParameter{}", semanticId & 0x7F) : fmt::format("passParameterSem{}", semanticId);
					if (defType() == kInt)
						out << gpr(i) << " = floatBitsToInt(" << source << ");\r\n";
					else if (defType() == kFloat)
						out << gpr(i) << " = " << source << ";\r\n";
					else
						cemu_assert_unimplemented();
				}
				// note: gated on R0 being used, matching the established behaviour
				if (frontFaceEnabled && isGPRMarked(0))
				{
					cemu_assert_debug(!frontFaceAllBits);
					out << gpr(frontFaceReg) << "." << laneName(frontFaceLane);
					if (defType() == kInt)
						out << " = floatBitsToInt(gl_FrontFacing?1.0:0.0);\r\n";
					else if (defType() == kFloat)
						out << " = gl_FrontFacing?1.0:0.0;\r\n";
					else
						cemu_assert_debug(false);
				}
			}
		}

		// ---- declarations (LatteDecompilerEmitGLSLHeader.hpp) ----
		void emitDeclarations();
		void emitPreprocessorSetup();
		void emitUniformVariables(RendererAPI api, LatteDecompilerOutputUniformOffsets& offsets);
		void emitUniformBlocks();
		void emitSamplers();
		void emitVertexAttributes();
		void emitStageInterface();
		void emitVertexOutputs();
		void emitPixelInputs();
	};

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSLHeader.hpp"

	void GlslShaderWriter::writeShader()
	{
		const auto& a = facts();
		out << "#version 430\r\n"; // shader storage buffers are needed for the Vulkan streamout path
		out << "#extension GL_ARB_texture_gather : enable\r\n";
		out << "#extension GL_ARB_separate_shader_objects : enable\r\n";
		if (a.hasStreamoutWrite || m_ctx->options->usesGeometryShader)
			out << "#extension GL_ARB_enhanced_layouts : enable\r\n";
		out.format("// shader {:016x}\r\n", m_ctx->shaderBaseHash);
#ifdef CEMU_DEBUG_ASSERT
		out.format("// usesIntegerValues: {}\r\n", a.usesIntegerValues ? "true" : "false");
		out << "\r\n";
#endif
		emitDeclarations();
		emitHelperFunctions();
		out << "void main()\r\n";
		out << "{\r\n";
		emitVariables();
		emitInputLoads();
		for (auto& cf : m_ctx->cfInstructions)
			emitControlFlow(cf);
		if (m_shader->shaderType == LatteConst::ShaderType::Geometry)
			out << "EndPrimitive();\r\n";
		// a point size the shader does not write comes from render state
		if (a.outputPointSize && !a.writesPointSize && m_shader->shaderType == LatteConst::ShaderType::Vertex && !m_ctx->options->usesGeometryShader)
			out << "gl_PointSize = uf_pointSize;\r\n";
		out << "}\r\n";
	}
}

void LatteDecompiler_emitGLSLShader(LatteDecompilerShaderContext* shaderContext, LatteDecompilerShader* shader)
{
	// grows on demand and is trimmed once the source is complete
	StringBuf* src = new StringBuf(64 * 1024);
	shaderContext->shaderSource = src;
	GlslShaderWriter writer(shaderContext, src);
	writer.writeShader();
	src->shrink_to_fit();
	shader->strBuf_shaderSource = src;
}
