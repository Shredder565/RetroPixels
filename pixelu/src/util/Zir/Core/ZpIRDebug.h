// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/Zir/Core/IR.h"

namespace ZpIR
{
	// prints a function as text to stdout, for debugging
	class DebugPrinter
	{
	public:
		void debugPrint(ZpIRFunction* irFunction);

		// print the physical register assignment instead of the virtual register numbers
		void setShowPhysicalRegisters(bool showPhys)
		{
			m_showPhysicalRegisters = showPhys;
		}

		void setVirtualRegisterNameSource(std::string(*getRegisterNameCustom)(ZpIRBasicBlock* block, IRReg r))
		{
			m_virtualRegName = getRegisterNameCustom;
		}

		void setPhysicalRegisterNameSource(std::string(*getRegisterNameCustom)(ZpIRBasicBlock* block, ZpIRPhysicalReg r))
		{
			m_physicalRegName = getRegisterNameCustom;
		}

	private:
		std::string getRegisterName(ZpIRBasicBlock* block, IRReg r);
		std::string getInstructionHRF(ZpIRBasicBlock* block, IR::__InsBase* cmd);
		void debugPrintBlock(ZpIRBasicBlock* block);

		std::string(*m_virtualRegName)(ZpIRBasicBlock* block, IRReg r) { nullptr };
		std::string(*m_physicalRegName)(ZpIRBasicBlock* block, ZpIRPhysicalReg r) { nullptr };
		bool m_showPhysicalRegisters{};
	};
}
