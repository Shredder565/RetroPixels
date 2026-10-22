// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/crypto/aes128.h"
#include "WindowSystem.h"
#include "Common/FileStream.h"

// Experimental low-level emulation entry: boots the console's own boot ROM
// and kernel image (bootrom.bin and kernel.img in the working folder)
// instead of high-level emulating the OS.

void CemuCommonInit();

namespace
{
	constexpr uint32 kBootRomPhysAddr = 0x00000000;
	constexpr uint32 kKernelPhysAddr = 0x08000000;
	constexpr uint32 kAncastMagic = 0xEFA282D9; // first word of an ancast image
	constexpr uint32 kResetVector = 0x00000100;

	// loads a file into physical memory; exits the process if it is missing
	std::vector<uint8> loadIntoPhysicalMemory(const char* fileName, uint32 physAddr)
	{
		std::optional<std::vector<uint8>> data = FileStream::LoadIntoMemory(fileName);
		if (!data)
			exit(-1);
		memcpy(memory_getPointerFromPhysicalOffset(physAddr), data->data(), data->size());
		return std::move(*data);
	}
}

void mainEmulatorLLE()
{
	CemuCommonInit();
	memory_initPhysicalLayout();
	WindowSystem::Create(); // starts the GUI thread

	loadIntoPhysicalMemory("bootrom.bin", kBootRomPhysAddr);
	const std::vector<uint8> kernel = loadIntoPhysicalMemory("kernel.img", kKernelPhysAddr);
	uint32be magic = 0;
	if (kernel.size() >= sizeof(magic))
		memcpy(&magic, kernel.data(), sizeof(magic));
	if (magic != kAncastMagic)
		assert_dbg(); // not an ancast image

	PPCTimer_waitForInit();
	PPCCoreLLE_startSingleCoreScheduler(kResetVector);
}
