// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_HWInterface.h"

// Hardware register access for titles. OSRead/OSWrite* go to register blocks that are mapped into
// user space; __OSRead/__OSWriteRegister32Ex reach registers through the kernel. Only the blocks
// titles are known to use are mapped, and only reads (plus kernel writes) are forwarded to MMIO.
namespace coreinit
{
	// register blocks reachable from user space
	enum class RegisterInterfaceId : uint32
	{
		VI = 0, // 0x0C1E0000
		VI2 = 3,
	};

	// register blocks reached through the kernel
	enum class SysRegisterInterfaceId : uint32
	{
		UNKNOWN0 = 0,
		ACR_VI = 3, // 0x0D00021C
		SI = 6,		// 0x0D006400
		AI2 = 7,	// 0x0D046C00
	};

	namespace
	{
		// physical address of a register, 0 when the block is not mapped
		PAddr UserRegisterAddress(RegisterInterfaceId block, uint32 offset)
		{
			if (block == RegisterInterfaceId::VI)
				return 0x0C1E0000 + offset;
			cemu_assert_debug(false); // unmapped register block
			return 0;
		}

		PAddr KernelRegisterAddress(SysRegisterInterfaceId block, uint32 offset)
		{
			switch (block)
			{
			case SysRegisterInterfaceId::ACR_VI:
				return 0x0D00021C + offset;
			case SysRegisterInterfaceId::SI:
				return 0x0D006400 + offset;
			default:
				cemu_assert_debug(false); // unmapped register block
				return 0;
			}
		}

		// writes are not forwarded; note the first attempt in debug builds
		void IgnoredRegisterWrite(bool& noted)
		{
			if (!noted)
			{
				cemu_assert_debug(false);
				noted = true;
			}
		}
	}

	uint32 OSReadRegister32(RegisterInterfaceId interfaceId, uint32 offset)
	{
		PAddr addr = UserRegisterAddress(interfaceId, offset);
		cemu_assert_debug(addr != 0);
		return MMU::ReadMMIO_32(addr);
	}

	uint16 OSReadRegister16(RegisterInterfaceId interfaceId, uint32 offset)
	{
		PAddr addr = UserRegisterAddress(interfaceId, offset);
		cemu_assert_debug(addr != 0);
		return MMU::ReadMMIO_16(addr);
	}

	void OSWriteRegister16(uint16 newValue, RegisterInterfaceId interfaceId, uint32 offset)
	{
		static bool s_noted = false;
		IgnoredRegisterWrite(s_noted);
	}

	// the value parameter is 16 bit wide in this export's signature (kept as registered)
	void OSWriteRegister32(uint16 newValue, RegisterInterfaceId interfaceId, uint32 offset)
	{
		static bool s_noted = false;
		IgnoredRegisterWrite(s_noted);
	}

	void OSModifyRegister16(RegisterInterfaceId interfaceId, uint32 uknR4, uint32 uknR5, uint32 uknR6)
	{
		static bool s_noted = false;
		IgnoredRegisterWrite(s_noted);
	}

	uint32 __OSReadRegister32Ex(SysRegisterInterfaceId interfaceId, uint32 registerId)
	{
		uint32 offset = registerId * 4;
		cemu_assert_debug(offset < 0x40);
		PAddr addr = KernelRegisterAddress(interfaceId, offset);
		cemu_assert_debug(addr != 0);
		return MMU::ReadMMIO_32(addr);
	}

	void __OSWriteRegister32Ex(SysRegisterInterfaceId interfaceId, uint32 registerId, uint32 newValue)
	{
		uint32 offset = registerId * 4;
		cemu_assert_debug(offset < 0x40);
		PAddr addr = KernelRegisterAddress(interfaceId, offset);
		cemu_assert_debug(addr != 0);
		MMU::WriteMMIO_32(addr, newValue);
	}

	void InitializeHWInterface()
	{
		cafeExportRegister("coreinit", OSReadRegister32, LogType::Placeholder);
		cafeExportRegister("coreinit", OSReadRegister16, LogType::Placeholder);
		cafeExportRegister("coreinit", OSWriteRegister16, LogType::Placeholder);
		cafeExportRegister("coreinit", OSWriteRegister32, LogType::Placeholder);
		cafeExportRegister("coreinit", OSModifyRegister16, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSReadRegister32Ex, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSWriteRegister32Ex, LogType::Placeholder);
	}
}
