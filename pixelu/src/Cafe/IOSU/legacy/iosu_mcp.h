// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#pragma pack(1)

// title entry returned by the title list queries (big endian, guest layout)
struct MCPTitleInfo
{
	uint32be titleIdHigh;
	uint32be titleIdLow;
	uint32be appGroup;
	char appPath[0x38]; // size not confirmed
	uint32be appType;
	uint16be titleVersion;
	// layout from here on is not confirmed
	uint64be osVersion;
	uint32be sdkVersion;
	uint8 deviceName[10];
	uint8 uknPadding;
};
static_assert(sizeof(MCPTitleInfo) == 0x61);

#pragma pack()

// emulator specific request on /dev/mcp
#define IOSU_MCP_REQUEST_CEMU (0xEE)

typedef struct
{
	uint32 requestCode;
	struct
	{
		MEMPTR<MCPTitleInfo> titleList;
		uint32be titleCount;
		uint32be titleListBufferSize;
		uint64 titleId;
		uint32be appType; // filter for IOSU_MCP_GET_TITLE_LIST_BY_APP_TYPE
	}titleListRequest;

	// output
	uint32 returnCode;
	union
	{
		struct
		{
			uint32 u32;
		}resultU32;
	};
}iosuMcpCemuRequest_t;

// sub codes of IOSU_MCP_REQUEST_CEMU
#define IOSU_MCP_GET_TITLE_LIST								0x01
#define IOSU_MCP_GET_TITLE_LIST_BY_APP_TYPE					0x02
#define IOSU_MCP_GET_TITLE_INFO								0x03 // list filtered by title id
#define IOSU_MCP_GET_TITLE_COUNT							0x04

namespace iosu
{
	sint32 mcpGetTitleCount();
	sint32 mcpGetTitleList(MCPTitleInfo* titleList, uint32 titleListBufferSize, uint32be* titleCount);

	void iosuMcp_init();

	namespace mcp
	{
		void Init();
		void Shutdown();
	};
}
