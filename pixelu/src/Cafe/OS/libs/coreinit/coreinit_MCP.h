// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// system production settings returned by MCP_GetSysProdSettings. Only the region fields are known
struct SysProdSettings
{
	/* +0x00 */ uint8 unk00[3];
	/* +0x03 */ uint8 platformRegion;
	/* +0x04 */ uint8 unk04[4];
	/* +0x08 */ uint8 prevBlock;
	/* +0x09 */ uint8 unk09[2];
	/* +0x0B */ uint8 gameRegion;
	/* +0x0C */ uint8 nextBlock;
	/* +0x0D */ uint8 unk0D[0x46 - 0x0D];
};
static_assert(offsetof(SysProdSettings, platformRegion) == 0x03);
static_assert(offsetof(SysProdSettings, gameRegion) == 0x0B);
static_assert(sizeof(SysProdSettings) == 0x46);

typedef uint32 MCPHANDLE;

MCPHANDLE MCP_Open();
void MCP_Close(MCPHANDLE mcpHandle);
sint32 MCP_GetSysProdSettings(MCPHANDLE mcpHandle, SysProdSettings* sysProdSettings);

void coreinitExport_UCReadSysConfig(PPCInterpreter_t* hCPU);

namespace coreinit
{
	void InitializeMCP();
}
