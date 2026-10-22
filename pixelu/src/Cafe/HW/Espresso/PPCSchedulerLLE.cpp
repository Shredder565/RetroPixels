// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Experimental low level mode: runs the Cafe OS kernel on three emulated
// cores with the address translating interpreter, switching between the
// cores round robin on a single host thread.

namespace
{
	// SCR bits that release cores 1 and 2 from reset
	constexpr uint32 kScrEnableCore1 = 0x00400000;
	constexpr uint32 kScrEnableCore2 = 0x00200000;

	// instructions per core before moving on to the next one
	constexpr sint32 kCoreSliceInstructions = 10000;

	// secondary cores start at the system reset vector
	constexpr uint32 kResetVector = 0xFFF00100;

	// ---- boot information block ----
	// The boot loader leaves this block at physical address 0x01FFF000 for
	// the kernel (layout as expected by kernel 5.5.2).

	constexpr uint32 kBootInfoAddress = 0x01FFF000;

	struct BootMemoryRegion
	{
		uint32be id;
		uint32be baseAddress;
		uint32be size;
	};

	struct BootInfoBlock
	{
		uint32be _unk000;
		uint32be blockSize;           // +0x004
		uint32be _unk008[4];
		uint32be busFrequency;        // +0x018
		uint32be _unk01C[42];
		BootMemoryRegion memory[3];   // +0x0C4
		uint8 _unk0E8[0x3B4 - 0xE8];
		uint32be smdpArea;            // +0x3B4, physical address of the IOSU mailbox
		uint8 _unk3B8[0x40C - 0x3B8];
	};

	static_assert(offsetof(BootInfoBlock, blockSize) == 0x004);
	static_assert(offsetof(BootInfoBlock, busFrequency) == 0x018);
	static_assert(offsetof(BootInfoBlock, memory) == 0x0C4);
	static_assert(offsetof(BootInfoBlock, smdpArea) == 0x3B4);
	static_assert(sizeof(BootInfoBlock) == 0x40C);

	BootInfoBlock* GetBootInfo()
	{
		return (BootInfoBlock*)memory_getPointerFromPhysicalOffset(kBootInfoAddress);
	}

	void WriteBootInfo()
	{
		BootInfoBlock* info = GetBootInfo();
		memset(info, 0, sizeof(BootInfoBlock));
		info->blockSize = (uint32)sizeof(BootInfoBlock);
		info->busFrequency = ESPRESSO_BUS_CLOCK;
		// memory regions as seen by the PowerPC: MEM0, MEM1 and MEM2
		const uint32 regions[3][2] = {
			{ 0x08000000, 0x00120000 },
			{ 0x00000000, 0x02000000 },
			{ 0x28000000, 0xA8000000 },
		};
		for (uint32 i = 0; i < 3; i++)
		{
			info->memory[i].id = i;
			info->memory[i].baseAddress = regions[i][0];
			info->memory[i].size = regions[i][1];
		}
	}

	// ---- SMDP mailbox ----
	// A ring of command pointers sent to IOSU and a ring of results sent back.
	// Indices live in separate 64 byte blocks.

	constexpr uint32 kSmdpMagic = 0x736D6470; // "smdp"
	constexpr uint32 kSmdpRingSize = 0xC00;

	struct SmdpMailbox
	{
		uint32be magic;
		uint32be ringLength;
		uint8 _pad008[0x38];
		uint32be commandReadIndex;    // +0x040, advanced by IOSU (us)
		uint8 _pad044[0x3C];
		uint32be commandWriteIndex;   // +0x080
		uint8 _pad084[0x3C];
		uint32be resultReadIndex;     // +0x0C0
		uint8 _pad0C4[0x3C];
		uint32be resultWriteIndex;    // +0x100, advanced by IOSU (us)
		uint8 _pad104[0x3C];
		uint32be commands[kSmdpRingSize]; // +0x140
		uint32be results[kSmdpRingSize];  // +0x3140
	};

	static_assert(offsetof(SmdpMailbox, commandReadIndex) == 0x040);
	static_assert(offsetof(SmdpMailbox, commandWriteIndex) == 0x080);
	static_assert(offsetof(SmdpMailbox, resultReadIndex) == 0x0C0);
	static_assert(offsetof(SmdpMailbox, resultWriteIndex) == 0x100);
	static_assert(offsetof(SmdpMailbox, commands) == 0x140);
	static_assert(offsetof(SmdpMailbox, results) == 0x3140);

	struct SmdpCommand
	{
		uint32be type;
		uint32be _unk04;
		uint32be status;
		uint32be _unk0C[9];
	};

	void SmdpPostResult(SmdpMailbox* box, MPTR result)
	{
		uint32 index = box->resultWriteIndex;
		box->results[index] = result;
		box->resultWriteIndex = (index + 1) % (uint32)box->ringLength;
	}

	void SmdpHandleCommand(SmdpMailbox* box, SmdpCommand* cmd)
	{
		if (cmd->type != 1)
		{
			assert_dbg();
			return;
		}
		// acknowledge without doing anything else
		cmd->status = 1;
		cemuLog_logDebug(LogType::Force, "SMDP: acknowledged command type 1 (not implemented)");
		SmdpPostResult(box, memory_getVirtualOffsetFromPointer(cmd));
	}

	// stands in for the IOSU side of the mailbox, polling it periodically
	void SmdpServiceThread()
	{
		for (;;)
		{
			uint32 boxAddress = GetBootInfo()->smdpArea;
			if (boxAddress != MPTR_NULL)
			{
				SmdpMailbox* box = (SmdpMailbox*)memory_getPointerFromPhysicalOffset(boxAddress);
				if (box->magic == kSmdpMagic)
				{
					uint32 readIndex = box->commandReadIndex;
					if (readIndex != (uint32)box->commandWriteIndex)
					{
						SmdpCommand* cmd = (SmdpCommand*)memory_getPointerFromPhysicalOffset(box->commands[readIndex]);
						SmdpHandleCommand(box, cmd);
						box->commandReadIndex = (readIndex + 1) % (uint32)box->ringLength;
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	// ---- cores ----

	PPCInterpreterGlobal_t s_sharedCoreState{};

	struct LLECoreBlock
	{
		uint8 recompilerScratch[128 * 1024]; // stack space below the contexts used by the recompiler
		PPCInterpreter_t cores[3];
	};

	bool CoreReleased(uint32 coreIndex)
	{
		uint32 scr = s_sharedCoreState.sprGlobal.scr;
		if (coreIndex == 1)
			return (scr & kScrEnableCore1) != 0;
		if (coreIndex == 2)
			return (scr & kScrEnableCore2) != 0;
		return true; // core 0 always runs
	}
}

void PPCCoreLLE_startSingleCoreScheduler(uint32 entrypoint)
{
	LLECoreBlock* block = (LLECoreBlock*)malloc(sizeof(LLECoreBlock));
	memset((void*)block, 0, sizeof(LLECoreBlock));

	WriteBootInfo();

	for (uint32 i = 0; i < 3; i++)
	{
		block->cores[i].spr.UPIR = i;
		block->cores[i].global = &s_sharedCoreState;
		block->cores[i].instructionPointer = (i == 0) ? entrypoint : kResetVector;
	}

	std::thread(SmdpServiceThread).detach();

	for (;;)
	{
		for (uint32 coreIndex = 0; coreIndex < 3; coreIndex++)
		{
			PPCInterpreter_t* cpu = block->cores + coreIndex;
			PPCInterpreter_setCurrentInstance(cpu);
			if (!CoreReleased(coreIndex))
				continue;
			cpu->remainingCycles = kCoreSliceInstructions;
			while (--cpu->remainingCycles >= 0)
				PPCInterpreterFull_executeInstruction(cpu);
		}
	}
}
