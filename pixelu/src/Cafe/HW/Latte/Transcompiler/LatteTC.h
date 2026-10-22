// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "util/Zir/Core/IR.h"

#include <array>
#include <bitset>

namespace ZpIR
{
	class BasicBlockBuilder;
}

// Experimental translator from Latte (R7xx) shader binaries to the ZpIR SSA form.
// Only straight-line programs and a small set of ALU operations are understood so far;
// anything else is reported through cemu_assert_debug and skipped.
class LatteTCGenIR
{
public:
	typedef unsigned short GPRElementIndex; // gpr * 4 + channel

	enum SHADER_TYPE
	{
		VERTEX,
		GEOMETRY,
		PIXEL,
	};

	// vertex shaders: fetch shader layout and the SQ_VTX_SEMANTIC_* table that maps
	// attribute semantics to input GPRs
	void setVertexShaderContext(const LatteFetchShader* parsedFetchShader, const uint32* vtxSemanticTable);

	ZpIR::ZpIRFunction* transcompileLatteToIR(const void* programData, uint32 programSize, SHADER_TYPE shaderType);

private:
	static constexpr uint32 kNumGPRs = 128;
	static constexpr uint32 kNumALUSlots = 5; // x, y, z, w, t

	static GPRElementIndex MakeElement(uint32 gpr, uint32 channel) { return (GPRElementIndex)(gpr * 4 + channel); }

	// IR value held by every GPR channel (and by PV/PS) at the current point of the block.
	// Values produced by an ALU group are queued and only become visible once the whole
	// group has read its operands.
	class ValueMap
	{
	public:
		bool lookup(GPRElementIndex element, ZpIR::IRReg& value) const;
		void bind(GPRElementIndex element, ZpIR::IRReg value);
		bool lookupPrevious(uint32 slot, ZpIR::IRReg& value) const;

		void queueWrite(uint32 slot, GPRElementIndex element, ZpIR::IRReg value);
		void queuePrevious(uint32 slot, ZpIR::IRReg value);
		void commitGroup();

		void clear();

	private:
		struct QueuedWrite
		{
			GPRElementIndex element{};
			ZpIR::IRReg value{};
			bool toGpr{};
			bool toPrevious{};
			ZpIR::IRReg previousValue{};
		};

		std::array<ZpIR::IRReg, kNumGPRs * 4> m_gprValue{};
		std::bitset<kNumGPRs * 4> m_gprKnown;
		std::array<ZpIR::IRReg, kNumALUSlots> m_prevValue{};
		std::bitset<kNumALUSlots> m_prevKnown;
		std::array<QueuedWrite, kNumALUSlots> m_queue{};
	};

	struct OperandDesc
	{
		uint32 sel;
		uint32 chan;
		bool neg;
		bool rel;
	};

	// translation of one block
	void translateBlock(ZpIR::ZpIRBasicBlock* block, uint32 firstCF, uint32 endCF, bool isEntry);
	void translateFetchShaderCall();
	void translateFetchedAttribute(const LatteParsedFetchShaderAttribute& attribute, uint32 dstGPR);
	void translateALUClause(const LatteCFInstruction_ALU& clause);
	void translateALUGroup(const LatteClauseInstruction_ALU_OP2* const slots[kNumALUSlots], const uint32* literals);
	void translateExport(const LatteCFInstruction_EXPORT_IMPORT& exp);

	// operand access
	ZpIR::IRReg readGPR(uint32 gpr, uint32 channel, ZpIR::DataType typeHint);
	ZpIR::IRReg readGPRAs(uint32 gpr, uint32 channel, ZpIR::DataType type);
	ZpIR::IRReg readOperand(const OperandDesc& op, const uint32* literals, ZpIR::DataType type);
	ZpIR::IRReg castTo(ZpIR::IRReg value, ZpIR::DataType type);
	ZpIR::DataType guessOperandType(const OperandDesc& op, const uint32* literals) const;

	void unsupported(); // marks the translation incomplete

	// state of the current translation
	const uint32* m_program{};
	uint32 m_programSize{};
	SHADER_TYPE m_shaderType{};
	ZpIR::BasicBlockBuilder* m_builder{};
	bool m_inEntryBlock{};
	bool m_incomplete{};
	ValueMap m_values;

	// vertex shader context
	const LatteFetchShader* m_fetchShader{};
	const uint32* m_semanticTable{};
};
