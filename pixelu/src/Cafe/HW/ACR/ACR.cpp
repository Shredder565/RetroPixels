// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Common/HwReg.h"

// ACR video interface window. Wii U mode software selects a legacy VI register
// through the select register and then accesses it through the data register,
// holding the control register's ownership bit while doing so. The legacy VI
// itself is not emulated, so data accesses are only logged.

namespace HW_ACR
{
	namespace
	{
		HWREG::ACR_VI_ADDR s_selectedViReg;
		HWREG::ACR_VI_CTRL s_viControl;

		HWREG::ACR_VI_DATA ReadViData(PAddr)
		{
			cemuLog_logDebug(LogType::Force, "ACR: read of legacy VI register {:08x} (not emulated)", s_selectedViReg.Index());
			return {};
		}

		void WriteViData(PAddr, HWREG::ACR_VI_DATA value)
		{
			cemuLog_logDebug(LogType::Force, "ACR: write {:08x} to legacy VI register {:08x} (not emulated)", value.Data(), s_selectedViReg.Index());
		}

		HWREG::ACR_VI_ADDR ReadViSelect(PAddr) { return s_selectedViReg; }
		void WriteViSelect(PAddr, HWREG::ACR_VI_ADDR value) { s_selectedViReg = value; }

		HWREG::ACR_VI_CTRL ReadViControl(PAddr) { return s_viControl; }
		void WriteViControl(PAddr, HWREG::ACR_VI_CTRL value) { s_viControl = value; }
	}

	RunAtCemuBoot _initACR([]()
	{
		using MMU::MMIOInterface;
		MMU::RegisterMMIO_32<HWREG::ACR_VI_DATA, ReadViData, WriteViData>(MMIOInterface::INTERFACE_0D000000, 0x21C);
		MMU::RegisterMMIO_32<HWREG::ACR_VI_ADDR, ReadViSelect, WriteViSelect>(MMIOInterface::INTERFACE_0D000000, 0x224);
		MMU::RegisterMMIO_32<HWREG::ACR_VI_CTRL, ReadViControl, WriteViControl>(MMIOInterface::INTERFACE_0D000000, 0x228);
	});
}
