// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/Zir/Core/IR.h"
#include "util/Zir/Core/ZpIRPasses.h"
#include "util/helpers/StringBuf.h"

namespace ZirEmitter
{
	// Writes a register-allocated (RegisterAllocatorForGLSL) single-block
	// function as a GLSL main(). A register that is read at most once is not
	// stored in a variable; its expression is substituted at the use instead.
	class GLSL
	{
	public:
		GLSL() {}

		// appends the code to output
		void Emit(ZpIR::ZpIRFunction* irFunction, StringBuf* output);

	private:
		void emitBlock(ZpIR::ZpIRBasicBlock& block);
		void emit(ZpIR::IR::InsRR* ins);
		void emit(ZpIR::IR::InsRRR* ins);
		void emit(ZpIR::IR::InsIMPORT* ins);
		void emit(ZpIR::IR::InsEXPORT* ins);

		// text of an operand: constant literal, pending expression or variable name
		std::string operand(ZpIR::IRReg r) const;
		std::string variableName(ZpIR::IRReg r) const;
		// makes expr the value of r, either as pending expression or as a declared variable
		void define(ZpIR::IRReg r, std::string expr, bool allowInline = true);

		StringBuf* m_out{};
		ZpIR::ZpIRBasicBlock* m_block{};
		std::vector<uint8> m_readCount;              // per register, saturating
		std::vector<std::optional<std::string>> m_pending; // per register, inlined expression
	};
}
