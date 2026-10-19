// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <IOSU/iosu_ipc_common.h>
#include "fsa_types.h"

namespace iosu
{
	namespace fsa
	{
		// request part of the shim buffer, one layout per FSA command
		struct FSARequest
		{
			uint32be ukn0;
			union
			{
				uint8 ukn04[0x51C];
				struct
				{
					MEMPTR<void> dest;
					uint32be size; // element size
					uint32be count;
					uint32be filePos;
					uint32be fileHandle;
					uint32be flag;
				} cmdReadFile;
				struct
				{
					MEMPTR<void> dest;
					uint32be size; // element size
					uint32be count;
					uint32be filePos;
					uint32be fileHandle;
					uint32be flag;
				} cmdWriteFile;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
					uint8 mode[12]; // fopen style string
					uint32be createMode;
					uint32be openFlags;
					uint32be preallocSize;
				} cmdOpenFile;
				struct
				{
					uint32be fileHandle;
				} cmdCloseFile;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
				} cmdRemove;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
					uint8 ukn0284[12];
				} cmdOpenDir;
				struct
				{
					betype<uint32> dirHandle;
				} cmdReadDir;
				struct
				{
					betype<uint32> dirHandle;
				} cmdCloseDir;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
					uint32be uknParam;
				} cmdMakeDir;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
				} cmdChangeDir;
				struct
				{
					uint8 query[FSA_CMD_PATH_MAX_LENGTH];
					uint32be queryType;
				} cmdQueryInfo;
				struct
				{
					uint8 srcPath[FSA_CMD_PATH_MAX_LENGTH];
					uint8 dstPath[FSA_CMD_PATH_MAX_LENGTH];
				} cmdRename;
				struct
				{
					uint32be size;
					uint32be count;
					uint32be fileHandle;
					uint32be uknParam;
				} cmdAppendFile;
				struct
				{
					uint32be fileHandle;
				} cmdTruncateFile;
				struct
				{
					uint32be fileHandle;
				} cmdGetStatFile;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
				} cmdFlushQuota;
				struct
				{
					uint32be fileHandle;
					uint32be filePos;
				} cmdSetPosFile;
				struct
				{
					uint32be fileHandle;
				} cmdGetPosFile;
				struct
				{
					uint32be fileHandle;
				} cmdIsEof;
				struct
				{
					uint32be dirHandle;
				} cmdRewindDir;
				struct
				{
					uint32be fileHandle;
				} cmdFlushFile;
				struct
				{
					uint8 path[FSA_CMD_PATH_MAX_LENGTH];
					uint32be mode1;
					uint32be mode2;
				} cmdChangeMode;
			};
		};
		static_assert(sizeof(FSARequest) == 0x520);

#pragma pack(1)
		// response part of the shim buffer
		struct FSAResponse
		{
			uint32be ukn0;
			union
			{
				uint8 ukn04[0x28F];
				struct
				{
					uint32be fileHandleOutput;
				} cmdOpenFile;
				struct
				{
					uint32be dirHandleOutput;
				} cmdOpenDir;
				struct
				{
					uint32be filePos;
				} cmdGetPosFile;
				struct
				{
					FSStat_t statOut;
				} cmdStatFile;
				struct
				{
					FSDirEntry_t dirEntry;
				} cmdReadDir;
				struct
				{
					char path[FSA_CMD_PATH_MAX_LENGTH];
				} cmdGetCWD;
				struct
				{
					union
					{
						uint8 ukn04[0x64];
						struct
						{
							uint64be freespace;
						} queryFreeSpace;
						struct
						{
							FSStat_t stat;
						} queryStat;
						struct
						{
							FSADeviceInfo_t info;
						} queryDeviceInfo;
					};
				} cmdQueryInfo;
			};
		};
		static_assert(sizeof(FSAResponse) == 0x293);
#pragma pack()

		// The block coreinit hands to /dev/fsa for every FS command. Only the
		// fields used by the emulation are named; the total size is a best guess.
		struct FSAShimBuffer
		{
			FSARequest request;
			uint8 ukn0520[0x60];
			FSAResponse response; // +0x580
			uint8 ukn0813[0x6D];
			IPCIoctlVector ioctlvVec[3]; // +0x880
			uint8 ukn08A4[0x5C];
			uint32be operationType; // +0x900
			betype<IOSDevHandle> fsaDevHandle;
			uint16be ipcReqType; // +0x908: 0 = ioctl, 1 = ioctlv
			uint8 ioctlvVecIn;
			uint8 ioctlvVecOut;
			uint32 ukn090C;
			uint32 ukn0910;
			uint32 ukn0914;
			uint32 ukn0918;
			uint32 ukn091C;
			uint32 ukn0920;
			uint32 ukn0924;
			uint32 ukn0928;
			uint32 ukn092C;
			uint32 ukn0930;
			uint32 ukn0934;
		};
		static_assert(sizeof(FSAShimBuffer) == 0x938);
		static_assert(offsetof(FSAShimBuffer, response) == 0x580);
		static_assert(offsetof(FSAShimBuffer, operationType) == 0x900);

		void Initialize();
		void Shutdown();
	}
}
