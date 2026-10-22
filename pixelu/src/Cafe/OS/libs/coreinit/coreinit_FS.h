// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/IOSU/fsa/fsa_types.h"
#include "Cafe/IOSU/fsa/iosu_fsa.h"
#include "coreinit_MessageQueue.h"

typedef MEMPTR<betype<FSFileHandle2>> FSFileHandlePtr;
typedef MEMPTR<betype<FSDirHandle2>> FSDirHandlePtr;

typedef uint32 FSAClientHandle;

// How an asynchronous FS call reports completion: either through a callback or by posting a
// message to a queue (exactly one of the two must be set)
struct FSAsyncParams
{
	MEMPTR<void> userCallback;
	MEMPTR<void> userContext;
	MEMPTR<coreinit::OSMessageQueue> ioMsgQueue;
};
static_assert(sizeof(FSAsyncParams) == 0xC);

namespace coreinit
{
	struct FSCmdBlockBody;

	// Per client queue of commands waiting to be sent to /dev/fsa, ordered by priority
	struct FSCmdQueue
	{
		enum class QUEUE_FLAG : uint32
		{
			IS_FULL = (1 << 0),	   // the maximum number of commands is in flight
			CANCEL_ALL = (1 << 4),
		};

		MEMPTR<FSCmdBlockBody> head;
		MEMPTR<FSCmdBlockBody> tail;
		OSFastMutex mutex;
		uint32 dequeueHandler;		 // guest function, stored byte swapped (always 0 here)
		uint32be inFlight;
		uint32 maxInFlight;			 // host byte order
		betype<QUEUE_FLAG> flags;
	};
	DEFINE_ENUM_FLAG_OPERATORS(FSCmdQueue::QUEUE_FLAG);
	static_assert(offsetof(FSCmdQueue, mutex) == 0x08);
	static_assert(offsetof(FSCmdQueue, dequeueHandler) == 0x34);
	static_assert(offsetof(FSCmdQueue, inFlight) == 0x38);
	static_assert(offsetof(FSCmdQueue, flags) == 0x40);
	static_assert(sizeof(FSCmdQueue) == 0x44);

#define FS_CLIENT_BUFFER_SIZE (5888)
#define FS_CMD_BLOCK_SIZE (2688)

	// Opaque guest buffers. The actual state lives at the first 64 byte aligned address inside
	struct FSClient_t
	{
		uint8 buffer[FS_CLIENT_BUFFER_SIZE];
	};

	struct FSCmdBlock_t
	{
		uint8 buffer[FS_CMD_BLOCK_SIZE];
	};
	static_assert(sizeof(FSCmdBlock_t) == 0xA80);

	struct FSClientBody_t
	{
		uint8 reserved0000[0x1444];
		/* +0x1444 */ betype<IOSDevHandle> fsaHandle;
		uint8 reserved1448[0x38];
		/* +0x1480 */ FSCmdQueue cmdQueue;
		/* +0x14C4 */ MEMPTR<FSCmdBlockBody> activeCmd;
		uint8 reserved14C8[0x14C];
		/* +0x1614 */ MEMPTR<FSClientBody_t> nextClient; // singly linked list of registered clients
		uint32 reserved1618;
		/* +0x161C */ MEMPTR<FSClient_t> owner;			  // the FSClient_t that contains this body
		uint32 reserved1620;
	};
	static_assert(offsetof(FSClientBody_t, fsaHandle) == 0x1444);
	static_assert(offsetof(FSClientBody_t, cmdQueue) == 0x1480);
	static_assert(offsetof(FSClientBody_t, activeCmd) == 0x14C4);
	static_assert(offsetof(FSClientBody_t, nextClient) == 0x1614);
	static_assert(offsetof(FSClientBody_t, owner) == 0x161C);
	static_assert(sizeof(FSClientBody_t) == 0x1624);
	static_assert(sizeof(FSClientBody_t) + 0x3F <= sizeof(FSClient_t));

	// Completion record. When a message queue is used, the OSMessage posted to it is the embedded
	// message below and its first word points back at this record
	struct FSAsyncResult
	{
		/* +0x00 */ FSAsyncParams params;

		struct FSMessage
		{
			MEMPTR<FSAsyncResult> result;
			uint32 data0;
			uint32 data1;
			uint32be kind; // 8 for FS command completions
		};
		union
		{
			OSMessage osMsg;
			FSMessage fsMsg;
		} msg;

		/* +0x1C */ MEMPTR<FSClient_t> client;
		/* +0x20 */ MEMPTR<FSCmdBlock_t> block;
		/* +0x24 */ uint32be status;
	};
	static_assert(offsetof(FSAsyncResult, msg) == 0x0C);
	static_assert(offsetof(FSAsyncResult, client) == 0x1C);
	static_assert(offsetof(FSAsyncResult, status) == 0x24);
	static_assert(sizeof(FSAsyncResult) == 0x28);

	// Where the results of a command are copied once it completes
	struct FSCmdBlockReturnValues_t
	{
		union
		{
			uint8 raw[0x14];
			MEMPTR<betype<FSResHandle>> fileHandleOut;
			MEMPTR<uint32be> dirHandleOut;
			MEMPTR<uint32be> filePosOut;
			MEMPTR<FSDirEntry_t> dirEntryOut;
			MEMPTR<void> queryOut;
			MEMPTR<void> statOut;
			struct
			{
				MEMPTR<char> buffer;
				uint32be length;
			} cwdOut;
		};
	};
	static_assert(sizeof(FSCmdBlockReturnValues_t) == 0x14);

	struct FSCmdBlockBody
	{
		/* +0x000 */ iosu::fsa::FSAShimBuffer shim;
		/* +0x938 */ MEMPTR<FSClientBody_t> client;
		/* +0x93C */ uint32be state;		  // one of the kCmdState values in coreinit_FS.cpp
		/* +0x940 */ uint32be cancelFlags;	  // bit 0: cancel requested
		/* +0x944 */ FSCmdBlockReturnValues_t out;
		/* +0x958 */ MEMPTR<FSCmdBlockBody> queueNext;
		/* +0x95C */ MEMPTR<FSCmdBlockBody> queuePrev;
		/* +0x960 */ betype<FSA_RESULT> fsaResult;
		uint32 reserved964;
		/* +0x968 */ uint8 errorMask;
		/* +0x96C */ FSAsyncResult completion;
		/* +0x994 */ MEMPTR<void> userData;
		/* +0x998 */ OSMessageQueue syncQueue; // used when a blocking call waits for its async version
		/* +0x9D4 */ OSMessage syncQueueStorage[1];
		/* +0x9E4 */ uint32 finishFunc;		  // guest function run before completion is reported, host byte order
		/* +0x9E8 */ uint8 priority;
		uint8 reserved9E9;
		uint8 reserved9EA;
		uint8 reserved9EB;
		uint32 reserved9EC;
		uint32 reserved9F0;
		uint32be reserved9F4;
		/* +0x9F8 */ MEMPTR<FSCmdBlock_t> owner;
		uint32 reserved9FC;
	};
	static_assert(offsetof(FSCmdBlockBody, client) == 0x938);
	static_assert(offsetof(FSCmdBlockBody, out) == 0x944);
	static_assert(offsetof(FSCmdBlockBody, queueNext) == 0x958);
	static_assert(offsetof(FSCmdBlockBody, fsaResult) == 0x960);
	static_assert(offsetof(FSCmdBlockBody, errorMask) == 0x968);
	static_assert(offsetof(FSCmdBlockBody, completion) == 0x96C);
	static_assert(offsetof(FSCmdBlockBody, userData) == 0x994);
	static_assert(offsetof(FSCmdBlockBody, syncQueue) == 0x998);
	static_assert(offsetof(FSCmdBlockBody, syncQueueStorage) == 0x9D4);
	static_assert(offsetof(FSCmdBlockBody, finishFunc) == 0x9E4);
	static_assert(offsetof(FSCmdBlockBody, priority) == 0x9E8);
	static_assert(offsetof(FSCmdBlockBody, owner) == 0x9F8);
	static_assert(sizeof(FSCmdBlockBody) == 0xA00);
	static_assert(sizeof(FSCmdBlockBody) + 0x3F <= sizeof(FSCmdBlock_t));

#define FSA_CMD_FLAG_SET_POS (1 << 0)

	enum FS_VOLSTATE : sint32
	{
		FS_VOLSTATE_READY = 1,
	};

	// used by nn_save
	sint32 __FSQueryInfoAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint8* queryString, uint32 queryType, void* queryResult, uint32 errHandling, FSAsyncParams* fsAsyncParams);

	FS_RESULT FSAddClientEx(FSClient_t* fsClient, uint32 uknR4, uint32 errHandling);
	FS_RESULT FSAddClient(FSClient_t* fsClient, uint32 errHandling);
	FS_RESULT FSDelClient(FSClient_t* fsClient, uint32 errHandling);

	void FSInitCmdBlock(FSCmdBlock_t* fsCmdBlock);

	sint32 FSOpenFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, char* mode, FSFileHandlePtr outFileHandle, uint32 errHandling, FSAsyncParams* asyncParams);
	sint32 FSOpenFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, char* mode, FSFileHandlePtr outFileHandle, uint32 errHandling);

	sint32 FSReadFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSReadFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask);
	sint32 FSReadFileWithPosAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSReadFileWithPos(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask);

	sint32 FSWriteFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSWriteFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask);
	sint32 FSWriteFileWithPosAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSWriteFileWithPos(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask);

	sint32 FSSetPosFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 filePos, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSSetPosFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 filePos, uint32 errorMask);
	sint32 FSGetPosFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32be* returnedFilePos, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSGetPosFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32be* returnedFilePos, uint32 errorMask);

	// note: the definitions take (size, count, fileHandle) in that order
	sint32 FSAppendFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 size, uint32 count, uint32 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSAppendFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 size, uint32 count, uint32 fileHandle, uint32 errorMask);

	sint32 FSIsEofAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSIsEof(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errorMask);

	sint32 FSRenameAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* srcPath, char* dstPath, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSRename(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* srcPath, char* dstPath, uint32 errorMask);
	sint32 FSRemoveAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint8* filePath, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSRemove(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint8* filePath, uint32 errorMask);
	sint32 FSMakeDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* dirPath, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSMakeDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, uint32 errorMask);
	sint32 FSChangeDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSChangeDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask);
	sint32 FSGetCwdAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* dirPathOut, sint32 dirPathMaxLen, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSGetCwd(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* dirPathOut, sint32 dirPathMaxLen, uint32 errorMask);

	sint32 FSGetFreeSpaceSizeAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, FSLargeSize* returnedFreeSize, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSGetFreeSpaceSize(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, FSLargeSize* returnedFreeSize, uint32 errorMask);

	sint32 FSOpenDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, FSDirHandlePtr dirHandleOut, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSOpenDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, FSDirHandlePtr dirHandleOut, uint32 errorMask);
	sint32 FSReadDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, FSDirEntry_t* dirEntryOut, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSReadDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, FSDirEntry_t* dirEntryOut, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSCloseDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSCloseDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, uint32 errorMask);

	sint32 FSFlushQuotaAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask, FSAsyncParams* fsAsyncParams);
	sint32 FSFlushQuota(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask);

	FS_VOLSTATE FSGetVolumeState(FSClient_t* fsClient);

	void InitializeFS();
} // namespace coreinit
