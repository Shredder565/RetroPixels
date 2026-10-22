// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/Zir/Core/IR.h"

namespace ZirPass
{
	// a transformation applied to a whole function
	class ZpIRPass
	{
	public:
		ZpIRPass(ZpIR::ZpIRFunction* irFunction) : m_irFunction(irFunction) {}
		virtual ~ZpIRPass() = default;

		virtual void applyPass() = 0;

	protected:
		ZpIR::ZpIRFunction* m_irFunction;
	};

	struct RABlock_t;

	// lifetime of one virtual register inside one block, in instruction indices
	// (-1 = live on entry, instruction count = live on exit), both ends inclusive
	struct RALivenessRange_t
	{
		RALivenessRange_t(RABlock_t* block, ZpIR::IRReg irReg, sint32 start, sint32 end, ZpIR::DataType irDataType)
			: m_block(block), m_irReg(irReg), m_irDataType(irDataType), m_startIndex(start), m_endIndex(end) {}

		void setStart(sint32 startIndex) { m_startIndex = startIndex; }
		void setEnd(sint32 endIndex) { m_endIndex = std::max(m_endIndex, endIndex); }

		bool isOverlapping(sint32 start, sint32 end) const
		{
			return m_startIndex <= end && start <= m_endIndex;
		}

		bool isOverlapping(RALivenessRange_t* range) const
		{
			return isOverlapping(range->m_startIndex, range->m_endIndex);
		}

		void assignPhysicalRegister(ZpIR::ZpIRPhysicalReg physReg)
		{
			m_physicalRegister = (sint32)physReg;
			m_hasPhysicalRegister = true;
		}

		RABlock_t* m_block;
		ZpIR::IRReg m_irReg;
		ZpIR::DataType m_irDataType;
		sint32 m_startIndex;
		sint32 m_endIndex;
		bool m_hasPhysicalRegister{ false };
		sint32 m_physicalRegister{ -1 };
	};

	struct RABlock_t
	{
		// one range per virtual register, by register index
		std::vector<std::unique_ptr<RALivenessRange_t>> livenessRanges;
	};

	// General purpose linear scan register allocator for single-block
	// functions. Physical registers 0-15 hold 64-bit values (U64, POINTER),
	// 16-31 are the 32-bit views of the same registers (U32). Spilling is not
	// supported. The result is stored in IRRegDef::physicalRegister.
	class RARegular : public ZpIRPass
	{
	public:
		RARegular(ZpIR::ZpIRFunction* irFunction) : ZpIRPass(irFunction) {}

		void applyPass() override;

	private:
		void buildRanges(ZpIR::ZpIRBasicBlock& block, RABlock_t& raBlock);
		void assignRegisters(ZpIR::ZpIRBasicBlock& block, RABlock_t& raBlock);

		static std::span<const sint32> candidatesFor(ZpIR::DataType dataType);
		// the 64-bit register a physical register belongs to
		static sint32 baseRegister(sint32 physReg) { return physReg & 15; }

		std::vector<RABlock_t> m_raBlockArray;
	};

	// Allocator for the GLSL emitter: every virtual register gets its own
	// variable. A physical register is (type << 30) | index with a separate
	// index counter per type.
	class RegisterAllocatorForGLSL : public ZpIRPass
	{
		enum class PHYS_REG_TYPE : uint8
		{
			U32 = 0,
			S32 = 1,
			F32 = 2
		};

	public:
		RegisterAllocatorForGLSL(ZpIR::ZpIRFunction* irFunction) : ZpIRPass(irFunction) {}

		void applyPass() override
		{
			assignPhysicalRegisters();
			m_irFunction->state.registersAllocated = true;
		}

		static bool IsPhysRegTypeU32(ZpIR::ZpIRPhysicalReg physReg) { return typeOf(physReg) == PHYS_REG_TYPE::U32; }
		static bool IsPhysRegTypeS32(ZpIR::ZpIRPhysicalReg physReg) { return typeOf(physReg) == PHYS_REG_TYPE::S32; }
		static bool IsPhysRegTypeF32(ZpIR::ZpIRPhysicalReg physReg) { return typeOf(physReg) == PHYS_REG_TYPE::F32; }

		static uint32 GetPhysRegIndex(ZpIR::ZpIRPhysicalReg physReg)
		{
			return (uint32)physReg & kIndexMask;
		}

		static std::string DebugPrintHelper_getPhysRegisterName(ZpIR::ZpIRBasicBlock* block, ZpIR::ZpIRPhysicalReg r);

	private:
		static constexpr uint32 kTypeShift = 30;
		static constexpr uint32 kIndexMask = (1u << kTypeShift) - 1;

		static PHYS_REG_TYPE typeOf(ZpIR::ZpIRPhysicalReg physReg)
		{
			return (PHYS_REG_TYPE)((uint32)physReg >> kTypeShift);
		}

		static ZpIR::ZpIRPhysicalReg MakePhysReg(PHYS_REG_TYPE regType, uint32 index)
		{
			return (ZpIR::ZpIRPhysicalReg)(((uint32)regType << kTypeShift) | index);
		}

		void assignPhysicalRegisters();
		void assignPhysicalRegistersForBlock(ZpIR::ZpIRBasicBlock* basicBlock);

		uint32 m_nextIndex[3]{}; // per PHYS_REG_TYPE
	};
};
