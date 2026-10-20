// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <OS/RPL/rpl.h>
#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "coreinit_IPC.h"
#include "Cafe/Filesystem/fsc.h"
#include "coreinit_IPCBuf.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleInfo.h"

// coreinit's FS client API. Every FS call fills the FSA shim buffer embedded in the caller's
// command block, queues the block on its client and sends it to /dev/fsa (one command per client
// in flight). When IOSU answers, the result is copied to the caller's output pointers and reported
// through the callback or message queue given in FSAsyncParams. The blocking variants run the async
// version with a private one-slot message queue and wait on it.
// The FSA* functions are the lower level interface; they talk to /dev/fsa synchronously using a
// shim buffer from a shared IPC pool.

namespace coreinit
{
	using iosu::fsa::FSAShimBuffer;

	namespace
	{
		// --- guest memory (keep this definition order, it fixes the addresses) ---------------------

		SysAllocator<OSMutex> s_fsMutex;
		SysAllocator<uint8, 128, 64> s_emptyTransferBuffer; // target of zero sized reads and writes
		SysAllocator<IPCBufPool_t*> s_shimPool;
		SysAllocator<uint8, 0x37500> s_shimPoolMemory;
		SysAllocator<uint32be> s_shimPoolEntryCount;

		SysAllocator s_strOK("FSA_STATUS_OK");
		SysAllocator s_strNotInit("FSA_STATUS_NOT_INIT");
		SysAllocator s_strEndOfDir("FSA_STATUS_END_OF_DIRECTORY");
		SysAllocator s_strEndOfFile("FSA_STATUS_END_OF_FILE");
		SysAllocator s_strMaxClients("FSA_STATUS_MAX_CLIENTS");
		SysAllocator s_strMaxFiles("FSA_STATUS_MAX_FILES");
		SysAllocator s_strMaxDirs("FSA_STATUS_MAX_DIRS");
		SysAllocator s_strAlreadyExists("FSA_STATUS_ALREADY_EXISTS");
		SysAllocator s_strNotFound("FSA_STATUS_NOT_FOUND");
		SysAllocator s_strPermission("FSA_STATUS_PERMISSION_ERROR");
		SysAllocator s_strInvalidParam("FSA_STATUS_INVALID_PARAM");
		SysAllocator s_strInvalidPath("FSA_STATUS_INVALID_PATH");
		SysAllocator s_strInvalidBuffer("FSA_STATUS_INVALID_BUFFER");
		SysAllocator s_strInvalidAlignment("FSA_STATUS_INVALID_ALIGNMENT");
		SysAllocator s_strInvalidClient("FSA_STATUS_INVALID_CLIENT_HANDLE");
		SysAllocator s_strInvalidFile("FSA_STATUS_INVALID_FILE_HANDLE");
		SysAllocator s_strInvalidDir("FSA_STATUS_INVALID_DIR_HANDLE");
		SysAllocator s_strNotFile("FSA_STATUS_NOT_FILE");
		SysAllocator s_strNotDir("FSA_STATUS_NOT_DIR");
		SysAllocator s_strOutOfResources("FSA_STATUS_OUT_OF_RESOURCES");
		SysAllocator s_strUnknown("FSA_STATUS_???");

		// --- constants -------------------------------------------------------------------------------

		// command block states as coreinit stores them
		constexpr uint32 kCmdStateIdle = 0x0D900A21;		// freshly initialized
		constexpr uint32 kCmdStateQueued = 0x0D900A22;
		constexpr uint32 kCmdStateDone = 0x0D900A24;		// finished, may be reused
		constexpr uint32 kCmdStateCompleting = 0x0D900A26;	// IOSU answered, results are being delivered

		constexpr uint32 kFSMessageKind = 8;
		constexpr uint8 kDefaultCmdPriority = 0x10;
		constexpr sint32 kStatusBadRequest = -0x400; // returned when a request is rejected up front
		constexpr uint32 kNoFinishFunc = 0xF122330E;
		constexpr uint32 kCancelRequested = 1;
		constexpr sint64 kMaxTransferSize = 2LL * 1024 * 1024 * 1024;

		// --- global state ----------------------------------------------------------------------------

		// Some titles touch files before calling FSInit (Twilight Princess HD), so FS counts as
		// initialized from the start
		bool s_fsInitialized = true;
		bool s_fsShutdown = false;
		FSClientBody_t* s_clientList = nullptr;

		bool s_sdCardMounted = false;
		bool s_mlcMounted = false;

		std::mutex s_fsaClientsLock;
		std::vector<FSAClientHandle> s_fsaClients;
		std::mutex s_shimPoolLock;
		bool s_shimPoolReady = false;

		void LockFS()
		{
			OSLockMutex(&s_fsMutex);
		}

		void UnlockFS()
		{
			OSUnlockMutex(&s_fsMutex);
		}

		[[noreturn]] void HaltOnFatalError(std::string_view reason)
		{
			cemuLog_log(LogType::Force, "FS: fatal error, halting the calling thread: {}", reason);
			for (;;)
				std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		// copies src into dst only when it fits including the terminator; otherwise dst becomes ""
		bool CopyStringIfFits(char* dst, size_t dstSize, const char* src)
		{
			size_t length = strlen(src);
			if (length >= dstSize)
			{
				dst[0] = '\0';
				return false;
			}
			memcpy(dst, src, length + 1);
			return true;
		}

		// The usable state of an FSClient or FSCmdBlock sits at the first 64 byte boundary inside it
		template<typename TBody>
		TBody* AlignToBody(void* outer)
		{
			return reinterpret_cast<TBody*>((reinterpret_cast<uintptr_t>(outer) + 0x3F) & ~(uintptr_t)0x3F);
		}

		FSClientBody_t* GetClientBody(FSClient_t* client)
		{
			if (!client)
				return nullptr;
			FSClientBody_t* body = AlignToBody<FSClientBody_t>(client);
			body->owner = client;
			return body;
		}

		FSCmdBlockBody* GetCmdBody(FSCmdBlock_t* block)
		{
			if (!block)
				return nullptr;
			FSCmdBlockBody* body = AlignToBody<FSCmdBlockBody>(block);
			body->owner = block;
			return body;
		}

		// logs the command block when its completion record no longer looks like one of ours
		void CheckCmdIntegrity(const char* where, FSCmdBlockBody* cmd)
		{
			if (cmd->completion.msg.fsMsg.kind == kFSMessageKind)
				return;
			cemuLog_log(LogType::Force, "FS: command block {:08x} is corrupted ({})", memory_getVirtualOffsetFromPointer(cmd), where);
			const uint8* bytes = reinterpret_cast<const uint8*>(cmd);
			for (size_t row = 0; row < sizeof(FSCmdBlockBody); row += 16)
			{
				std::string line = fmt::format("  +{:03x}:", row);
				for (size_t i = row; i < row + 16 && i < sizeof(FSCmdBlockBody); i++)
					line += fmt::format(" {:02x}", bytes[i]);
				cemuLog_log(LogType::Force, "{}", line);
			}
		}

		// --- client list -----------------------------------------------------------------------------

		bool IsClientRegistered(FSClientBody_t* body)
		{
			LockFS();
			bool found = false;
			for (FSClientBody_t* it = s_clientList; it; )
			{
				if (it == body)
				{
					found = true;
					break;
				}
				it = it->nextClient.GetPtr();
				if (it == s_clientList)
					break;
			}
			UnlockFS();
			return found;
		}

		void InitCmdQueue(FSCmdQueue* queue, MPTR dequeueHandler, uint32 maxInFlight)
		{
			cemu_assert(maxInFlight > 0);
			queue->dequeueHandler = _swapEndianU32(dequeueHandler);
			queue->inFlight = 0;
			queue->maxInFlight = maxInFlight;
			OSFastMutex_Init(&queue->mutex, nullptr);
			queue->head = nullptr;
			queue->tail = nullptr;
		}

		// --- command queue ---------------------------------------------------------------------------

		// Inserts cmd behind the last queued command whose priority value is not below its own
		void InsertByPriority(FSCmdQueue* queue, FSCmdBlockBody* cmd)
		{
			if (!queue->head)
			{
				cemu_assert(!queue->tail);
				queue->head = cmd;
				queue->tail = cmd;
				cmd->queueNext = nullptr;
				cmd->queuePrev = nullptr;
				return;
			}
			FSCmdBlockBody* before = queue->tail.GetPtr();
			FSCmdBlockBody* after = nullptr;
			while (before && before->priority < cmd->priority)
			{
				after = before;
				before = before->queuePrev.GetPtr();
			}
			cmd->queuePrev = before;
			cmd->queueNext = after;
			if (before)
				before->queueNext = cmd;
			else
				queue->head = cmd;
			if (after)
				after->queuePrev = cmd;
			else
				queue->tail = cmd;
		}

		FSCmdBlockBody* PopFront(FSCmdQueue* queue)
		{
			FSCmdBlockBody* cmd = queue->head.GetPtr();
			if (!cmd)
				return nullptr;
			if (queue->head == queue->tail)
				queue->tail = nullptr;
			queue->head = cmd->queueNext;
			cmd->queueNext = nullptr;
			// the new head keeps its queuePrev link to the removed command (long standing behaviour)
			return cmd;
		}

		// --- talking to /dev/fsa ---------------------------------------------------------------------

		void SendShimAsync(FSAShimBuffer* shim, MEMPTR<void> onDone, void* context)
		{
			if (shim->ipcReqType == 0)
			{
				IOS_ERROR r = IOS_IoctlAsync(shim->fsaDevHandle, shim->operationType, &shim->request, sizeof(shim->request), &shim->response, sizeof(shim->response), onDone, context);
				cemu_assert(!IOS_ResultIsError(r));
			}
			else if (shim->ipcReqType == 1)
			{
				IOS_ERROR r = IOS_IoctlvAsync(shim->fsaDevHandle, shim->operationType, shim->ioctlvVecIn, shim->ioctlvVecOut, shim->ioctlvVec, onDone, context);
				cemu_assert(!IOS_ResultIsError(r));
			}
			else
			{
				cemu_assert_error();
			}
		}

		FSA_RESULT SendShim(FSAShimBuffer* shim)
		{
			IOS_ERROR r;
			if (shim->ipcReqType == 0)
				r = IOS_Ioctl(shim->fsaDevHandle, shim->operationType, &shim->request, sizeof(shim->request), &shim->response, sizeof(shim->response));
			else if (shim->ipcReqType == 1)
				r = IOS_Ioctlv(shim->fsaDevHandle, shim->operationType, shim->ioctlvVecIn, shim->ioctlvVecOut, shim->ioctlvVec);
			else
				return FSA_RESULT::FATAL_ERROR;
			return (FSA_RESULT)r; // FSA results travel as IOS results unchanged
		}

		FS_RESULT ToFSResult(FSA_RESULT r)
		{
			if ((sint32)r > 0)
				return (FS_RESULT)r; // positive values are byte or element counts
			switch (r)
			{
			case FSA_RESULT::OK:
				return FS_RESULT::SUCCESS;
			case FSA_RESULT::END_OF_DIRECTORY:
			case FSA_RESULT::END_OF_FILE:
				return FS_RESULT::END_ITERATION;
			case FSA_RESULT::ALREADY_EXISTS:
				return FS_RESULT::ALREADY_EXISTS;
			case FSA_RESULT::NOT_FOUND:
				return FS_RESULT::NOT_FOUND;
			case FSA_RESULT::PERMISSION_ERROR:
				return FS_RESULT::PERMISSION_ERROR;
			case FSA_RESULT::NOT_FILE:
				return FS_RESULT::NOT_FILE;
			case FSA_RESULT::NOT_DIR:
				return FS_RESULT::NOT_DIR;
			case FSA_RESULT::MAX_FILES:
			case FSA_RESULT::MAX_DIRS:
				return FS_RESULT::MAX_HANDLES;
			case FSA_RESULT::INVALID_CLIENT_HANDLE:
			case FSA_RESULT::INVALID_FILE_HANDLE:
			case FSA_RESULT::INVALID_DIR_HANDLE:
			case FSA_RESULT::INVALID_PARAM:
			case FSA_RESULT::INVALID_PATH:
			case FSA_RESULT::INVALID_BUFFER:
			case FSA_RESULT::INVALID_ALIGNMENT:
			case FSA_RESULT::NOT_INIT:
			case FSA_RESULT::MAX_CLIENTS:
			case FSA_RESULT::OUT_OF_RESOURCES:
			case FSA_RESULT::FATAL_ERROR:
				return FS_RESULT::FATAL_ERROR;
			}
			cemu_assert_unimplemented();
			return FS_RESULT::FATAL_ERROR;
		}

		void OnIoctlDone(PPCInterpreter_t* hCPU);

		// sends the next queued command of a client if it has room for one
		void DispatchNext(FSCmdQueue* queue)
		{
			LockFS();
			if (queue->inFlight < queue->maxInFlight)
			{
				if (FSCmdBlockBody* cmd = PopFront(queue))
				{
					queue->inFlight += 1;
					if (queue->inFlight >= queue->maxInFlight)
						queue->flags = queue->flags | FSCmdQueue::QUEUE_FLAG::IS_FULL;
					cemu_assert_debug(queue->dequeueHandler == 0); // custom dequeue handlers are not supported
					SendShimAsync(&cmd->shim, MEMPTR<void>(PPCInterpreter_makeCallableExportDepr(OnIoctlDone)), &cmd->shim);
				}
			}
			UnlockFS();
		}

		// copies what IOSU returned to the output pointers the caller handed in
		void CopyOutResults(FSCmdBlockBody* cmd)
		{
			FSAShimBuffer& shim = cmd->shim;
			switch ((FSA_CMD_OPERATION_TYPE)shim.operationType.value())
			{
			case FSA_CMD_OPERATION_TYPE::OPENFILE:
				*cmd->out.fileHandleOut = shim.response.cmdOpenFile.fileHandleOutput;
				break;
			case FSA_CMD_OPERATION_TYPE::OPENDIR:
				*cmd->out.dirHandleOut = shim.response.cmdOpenDir.dirHandleOutput;
				break;
			case FSA_CMD_OPERATION_TYPE::READDIR:
				*cmd->out.dirEntryOut = shim.response.cmdReadDir.dirEntry;
				break;
			case FSA_CMD_OPERATION_TYPE::GETPOS:
				*cmd->out.filePosOut = shim.response.cmdGetPosFile.filePos;
				break;
			case FSA_CMD_OPERATION_TYPE::GETCWD:
				memcpy(cmd->out.cwdOut.buffer.GetPtr(), shim.response.cmdGetCWD.path, cmd->out.cwdOut.length);
				break;
			case FSA_CMD_OPERATION_TYPE::GETSTATFILE:
				*(FSStat_t*)cmd->out.statOut.GetPtr() = shim.response.cmdStatFile.statOut;
				break;
			case FSA_CMD_OPERATION_TYPE::QUERYINFO:
			{
				uint32 queryType = shim.request.cmdQueryInfo.queryType;
				if (queryType == FSA_QUERY_TYPE_FREESPACE)
					*(uint64be*)cmd->out.queryOut.GetPtr() = shim.response.cmdQueryInfo.queryFreeSpace.freespace;
				else if (queryType == FSA_QUERY_TYPE_STAT)
					*(FSStat_t*)cmd->out.queryOut.GetPtr() = shim.response.cmdQueryInfo.queryStat.stat;
				else
					cemu_assert_unimplemented();
				break;
			}
			default:
				break; // nothing to copy
			}
		}

		// guest callable, used as the command block's finish function
		void FinishFuncTrampoline(PPCInterpreter_t* hCPU)
		{
			ppcDefineParamPtr(cmd, FSCmdBlockBody, 0);
			CopyOutResults(cmd);
			osLib_returnFromFunction(hCPU, 0);
		}

		// marks the command finished and tells the caller through its callback or message queue
		void ReportCompletion(FSCmdBlockBody* cmd, FS_RESULT result)
		{
			CheckCmdIntegrity("report", cmd);
			FSClientBody_t* client = cmd->client.GetPtr();
			OSFastMutex_Lock(&client->cmdQueue.mutex);
			cmd->cancelFlags &= ~kCancelRequested;
			if (client->activeCmd.GetPtr() == cmd)
				client->activeCmd = nullptr;
			cmd->state = kCmdStateDone;
			OSFastMutex_Unlock(&client->cmdQueue.mutex);

			FSAsyncResult& completion = cmd->completion;
			cemu_assert_debug(!completion.params.ioMsgQueue != !completion.params.userCallback);
			cmd->reserved9EA = 0;
			cmd->reserved9F4 = 0;
			if (completion.params.ioMsgQueue)
			{
				OSMessageQueue* queue = completion.params.ioMsgQueue.GetPtr();
				completion.block = cmd->owner;
				completion.status = (uint32)result;
				while (OSSendMessage(queue, &completion.msg.osMsg, 0) == 0)
				{
					cemuLog_log(LogType::Force, "FS: completion queue is full, retrying");
					if (PPCInterpreter_getCurrentInstance())
						PPCCore_switchToScheduler();
					else
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}
			else
			{
				PPCCoreCallback(completion.params.userCallback, completion.client.GetPtr(), completion.block.GetPtr(), (sint32)result, completion.params.userContext);
			}
		}

		// guest callable, completion handler for IOS_Ioctl(v)Async (r3 = IOS result, r4 = command)
		void OnIoctlDone(PPCInterpreter_t* hCPU)
		{
			ppcDefineParamU32(iosResult, 0);
			ppcDefineParamPtr(cmd, FSCmdBlockBody, 1);
			FSA_RESULT fsaResult = (FSA_RESULT)iosResult;
			cmd->state = kCmdStateCompleting;
			cmd->fsaResult = fsaResult;
			FS_RESULT result = ToFSResult(fsaResult);

			FSClientBody_t* client = cmd->client.GetPtr();
			LockFS();
			client->cmdQueue.inFlight -= 1;
			client->cmdQueue.flags = client->cmdQueue.flags & ~FSCmdQueue::QUEUE_FLAG::IS_FULL;
			UnlockFS();

			// coreinit hands this off to its AppIO threads; we finish it right here
			uint32 finishFunc = cmd->finishFunc;
			if (finishFunc != 0 && finishFunc != kNoFinishFunc)
				PPCCoreCallback(MEMPTR<void>(finishFunc), cmd, result);
			ReportCompletion(cmd, result);
			// the caller may reuse the command block from here on
			DispatchNext(&client->cmdQueue);
			osLib_returnFromFunction(hCPU, 0);
		}

		void QueueCommand(FSClientBody_t* client, FSCmdBlockBody* cmd)
		{
			cmd->finishFunc = RPLLoader_MakePPCCallable(FinishFuncTrampoline);
			LockFS();
			cmd->state = kCmdStateQueued;
			InsertByPriority(&client->cmdQueue, cmd);
			UnlockFS();
			DispatchNext(&client->cmdQueue);
		}

		// validates a request and binds the command block to the client and the async parameters
		sint32 BeginCommand(FSClientBody_t* client, FSCmdBlockBody* cmd, uint32 errorMask, FSAsyncParams* params)
		{
			if (!s_fsInitialized || s_fsShutdown)
				return kStatusBadRequest;
			if (!client || !cmd)
			{
				cemu_assert(false);
				return kStatusBadRequest;
			}
			uint32 state = cmd->state;
			if (state != kCmdStateIdle && state != kCmdStateDone)
			{
				cemu_assert(false); // command block is still in use
				return kStatusBadRequest;
			}
			bool hasCallback = params->userCallback != nullptr;
			bool hasQueue = params->ioMsgQueue != nullptr;
			if (hasCallback == hasQueue)
			{
				cemu_assert(false); // exactly one way of reporting completion must be given
				return kStatusBadRequest;
			}
			cmd->client = client;
			cmd->errorMask = (uint8)(errorMask >> 24); // coreinit keeps only this byte
			cmd->reserved9E9 = 0;
			cmd->cancelFlags &= ~kCancelRequested;
			cmd->shim.fsaDevHandle = client->fsaHandle;

			FSAsyncResult& completion = cmd->completion;
			completion.params = *params;
			completion.client = client->owner;
			completion.block = cmd->owner;
			completion.msg.fsMsg.result = &completion;
			completion.msg.fsMsg.kind = kFSMessageKind;
			return 0;
		}

		// Shared shape of the FS*Async functions. setup() runs after the request was accepted; it
		// returns a non zero FS status to fail the call, or 0 to queue the command
		template<typename TSetup>
		sint32 SubmitCommand(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 errorMask, FSAsyncParams* params, TSetup&& setup)
		{
			FSClientBody_t* client = GetClientBody(fsClient);
			FSCmdBlockBody* cmd = GetCmdBody(fsCmdBlock);
			sint32 status = BeginCommand(client, cmd, errorMask, params);
			if (status != 0)
				return status;
			status = setup(client, cmd);
			if (status != 0)
				return status;
			QueueCommand(client, cmd);
			return (sint32)FS_RESULT::SUCCESS;
		}

		sint32 BuildStatus(FSA_RESULT r)
		{
			return r == FSA_RESULT::OK ? 0 : (sint32)ToFSResult(r);
		}

		// prepares the async parameters a blocking call passes to its async version
		void PrepareBlockingCall(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSAsyncParams* params)
		{
			if (!fsClient || !fsCmdBlock || !params)
				assert_dbg();
			FSCmdBlockBody* cmd = GetCmdBody(fsCmdBlock);
			OSInitMessageQueue(&cmd->syncQueue, cmd->syncQueueStorage, 1);
			params->userCallback = nullptr;
			params->userContext = nullptr;
			params->ioMsgQueue = &cmd->syncQueue;
		}

		sint32 WaitForBlockingCall(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, sint32 submitStatus, uint32 errorMask)
		{
			if (submitStatus < 0)
			{
				// error masks are not implemented, a rejected request reads as success
				cemuLog_log(LogType::Force, "FS: blocking request was rejected (error mask {:08x})", errorMask);
				cemu_assert_debug(false);
				return 0;
			}
			FSCmdBlockBody* cmd = GetCmdBody(fsCmdBlock);
			OSMessage msg;
			OSReceiveMessage(&cmd->syncQueue, &msg, OS_MESSAGE_BLOCK);
			CheckCmdIntegrity("wait", cmd);
			FSAsyncResult* result = (FSAsyncResult*)memory_getPointerFromVirtualOffset(msg.message);
			return result->status;
		}

		template<typename TAsync>
		sint32 RunBlocking(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 errorMask, TAsync&& async)
		{
			StackAllocator<FSAsyncParams> params;
			PrepareBlockingCall(fsClient, fsCmdBlock, params.GetPointer());
			sint32 status = async(params.GetPointer());
			return WaitForBlockingCall(fsClient, fsCmdBlock, status, errorMask);
		}

		// --- request builders ------------------------------------------------------------------------

		void BeginShim(FSAShimBuffer* shim, IOSDevHandle fsaHandle, FSA_CMD_OPERATION_TYPE op, uint16 ipcType = 0)
		{
			shim->fsaDevHandle = fsaHandle;
			shim->ipcReqType = ipcType;
			shim->operationType = (uint32)op;
		}

		// path, cut to fit with its terminator
		void StorePath(uint8* dst, const char* src)
		{
			size_t length = strlen(src);
			if (length >= FSA_CMD_PATH_MAX_LENGTH)
			{
				cemu_assert_debug(false);
				length = FSA_CMD_PATH_MAX_LENGTH - 1;
			}
			memcpy(dst, src, length);
			dst[length] = '\0';
		}

		// string of at most maxLength characters, rest of the field zero filled
		void StoreZeroPadded(uint8* dst, size_t fieldSize, const char* src, size_t maxLength, bool assertOnCut)
		{
			size_t length = strlen(src);
			if (length > maxLength)
			{
				if (assertOnCut)
					cemu_assert_debug(false);
				length = maxLength;
			}
			memcpy(dst, src, length);
			memset(dst + length, 0, fieldSize - length);
		}

		FSA_RESULT BuildOpenFile(FSAShimBuffer* shim, IOSDevHandle fsaHandle, const char* path, const char* mode, uint32 createMode, uint32 openFlags, uint32 preallocSize)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!path)
				return FSA_RESULT::INVALID_PATH;
			if (!mode)
				return FSA_RESULT::INVALID_PARAM;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::OPENFILE);
			auto& req = shim->request.cmdOpenFile;
			StoreZeroPadded(req.path, sizeof(req.path), path, FSA_CMD_PATH_MAX_LENGTH - 1, true);
			StoreZeroPadded(req.mode, sizeof(req.mode), mode, sizeof(req.mode) - 1, true);
			req.createMode = createMode;
			req.openFlags = openFlags;
			req.preallocSize = preallocSize;
			shim->response.cmdOpenFile.fileHandleOutput = 0xFFFFFFFF;
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildOpenDir(FSAShimBuffer* shim, IOSDevHandle fsaHandle, const char* path)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!path)
				return FSA_RESULT::INVALID_PATH;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::OPENDIR);
			StoreZeroPadded(shim->request.cmdOpenDir.path, FSA_CMD_PATH_MAX_LENGTH, path, FSA_CMD_PATH_MAX_LENGTH - 1, true);
			shim->response.cmdOpenDir.dirHandleOutput = -1;
			return FSA_RESULT::OK;
		}

		// requests that carry only a file or directory handle
		FSA_RESULT BuildHandleOp(FSAShimBuffer* shim, IOSDevHandle fsaHandle, FSA_CMD_OPERATION_TYPE op, uint32 handle)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			BeginShim(shim, fsaHandle, op);
			auto& req = shim->request;
			switch (op)
			{
			case FSA_CMD_OPERATION_TYPE::CLOSEFILE: req.cmdCloseFile.fileHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::FLUSHFILE: req.cmdFlushFile.fileHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::GETPOS: req.cmdGetPosFile.fileHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::ISEOF: req.cmdIsEof.fileHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::TRUNCATEFILE: req.cmdTruncateFile.fileHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::GETSTATFILE: req.cmdGetStatFile.fileHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::READDIR: req.cmdReadDir.dirHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::CLOSEDIR: req.cmdCloseDir.dirHandle = handle; break;
			case FSA_CMD_OPERATION_TYPE::REWINDDIR: req.cmdRewindDir.dirHandle = handle; break;
			default: cemu_assert_error(); break;
			}
			return FSA_RESULT::OK;
		}

		// READ and WRITE go out as Ioctlv: the request, the data buffer and the response
		FSA_RESULT BuildTransfer(FSAShimBuffer* shim, IOSDevHandle fsaHandle, bool isWrite, void* buffer, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag)
		{
			if (!shim || !buffer)
				return FSA_RESULT::INVALID_BUFFER;
			MPTR bufferAddr = memory_getVirtualOffsetFromPointer(buffer);
			if ((bufferAddr & 0x3F) != 0)
				return FSA_RESULT::INVALID_ALIGNMENT;
			BeginShim(shim, fsaHandle, isWrite ? FSA_CMD_OPERATION_TYPE::WRITE : FSA_CMD_OPERATION_TYPE::READ, 1);
			shim->ioctlvVecIn = isWrite ? 2 : 1;
			shim->ioctlvVecOut = isWrite ? 1 : 2;
			shim->ioctlvVec[0].baseVirt = shim;
			shim->ioctlvVec[0].size = sizeof(iosu::fsa::FSARequest);
			shim->ioctlvVec[1].baseVirt = bufferAddr;
			shim->ioctlvVec[1].size = size * count;
			shim->ioctlvVec[2].baseVirt = &shim->response;
			shim->ioctlvVec[2].size = sizeof(iosu::fsa::FSAResponse);
			auto fill = [&](auto& req) {
				req.dest = buffer;
				req.size = size;
				req.count = count;
				req.filePos = filePos;
				req.fileHandle = fileHandle;
				req.flag = flag;
			};
			if (isWrite)
				fill(shim->request.cmdWriteFile);
			else
				fill(shim->request.cmdReadFile);
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildSetPos(FSAShimBuffer* shim, IOSDevHandle fsaHandle, uint32 fileHandle, uint32 filePos)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::SETPOS);
			shim->request.cmdSetPosFile.fileHandle = fileHandle;
			shim->request.cmdSetPosFile.filePos = filePos;
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildAppend(FSAShimBuffer* shim, IOSDevHandle fsaHandle, uint32 size, uint32 count, uint32 fileHandle, uint32 extra)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::APPENDFILE);
			auto& req = shim->request.cmdAppendFile;
			req.fileHandle = fileHandle;
			req.count = count;
			req.size = size;
			req.uknParam = extra;
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildRename(FSAShimBuffer* shim, IOSDevHandle fsaHandle, const char* from, const char* to)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!from || !to)
				return FSA_RESULT::INVALID_PATH;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::RENAME);
			StorePath(shim->request.cmdRename.srcPath, from);
			StorePath(shim->request.cmdRename.dstPath, to);
			return FSA_RESULT::OK;
		}

		// requests that carry only a path
		FSA_RESULT BuildPathOp(FSAShimBuffer* shim, IOSDevHandle fsaHandle, FSA_CMD_OPERATION_TYPE op, const char* path)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!path)
				return FSA_RESULT::INVALID_PATH;
			BeginShim(shim, fsaHandle, op);
			switch (op)
			{
			case FSA_CMD_OPERATION_TYPE::REMOVE: StorePath(shim->request.cmdRemove.path, path); break;
			case FSA_CMD_OPERATION_TYPE::CHANGEDIR: StorePath(shim->request.cmdChangeDir.path, path); break;
			case FSA_CMD_OPERATION_TYPE::FLUSHQUOTA: StorePath(shim->request.cmdFlushQuota.path, path); break;
			default: cemu_assert_error(); break;
			}
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildMakeDir(FSAShimBuffer* shim, IOSDevHandle fsaHandle, const char* path, uint32 permissions)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!path)
				return FSA_RESULT::INVALID_PATH;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::MAKEDIR);
			StorePath(shim->request.cmdMakeDir.path, path);
			shim->request.cmdMakeDir.uknParam = permissions;
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildGetCwd(FSAShimBuffer* shim, IOSDevHandle fsaHandle)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::GETCWD);
			return FSA_RESULT::OK;
		}

		FSA_RESULT BuildQueryInfo(FSAShimBuffer* shim, IOSDevHandle fsaHandle, const char* query, uint32 queryType)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!query)
				return FSA_RESULT::INVALID_PATH;
			if (queryType > 8)
				return FSA_RESULT::INVALID_PARAM;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::QUERYINFO);
			StorePath(shim->request.cmdQueryInfo.query, query);
			shim->request.cmdQueryInfo.queryType = queryType;
			return FSA_RESULT::OK;
		}

		// note: goes out with the REWINDDIR operation id, as it always has here
		FSA_RESULT BuildChangeMode(FSAShimBuffer* shim, IOSDevHandle fsaHandle, const char* path, uint32 mode, uint32 modeMask)
		{
			if (!shim)
				return FSA_RESULT::INVALID_BUFFER;
			if (!path)
				return FSA_RESULT::INVALID_PATH;
			BeginShim(shim, fsaHandle, FSA_CMD_OPERATION_TYPE::REWINDDIR);
			StoreZeroPadded(shim->request.cmdChangeMode.path, FSA_CMD_PATH_MAX_LENGTH, path, FSA_CMD_PATH_MAX_LENGTH, false);
			shim->request.cmdChangeMode.mode1 = mode;
			shim->request.cmdChangeMode.mode2 = modeMask;
			return FSA_RESULT::OK;
		}

		sint32 SubmitTransfer(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, bool isWrite, void* buffer, uint32 size, uint32 count, bool usePos, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* params)
		{
			return SubmitCommand(fsClient, fsCmdBlock, errorMask, params, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
				if (size == 0 || count == 0 || !buffer)
					buffer = s_emptyTransferBuffer.GetPtr();
				sint64 total = (sint64)size * (sint64)count;
				if (total < 0 || total >= kMaxTransferSize)
				{
					cemu_assert(false);
					return kStatusBadRequest;
				}
				// coreinit splits large transfers into chunks so they can be canceled; we send one request
				if (usePos)
					flag |= FSA_CMD_FLAG_SET_POS;
				else
					flag &= ~FSA_CMD_FLAG_SET_POS;
				return BuildStatus(BuildTransfer(&cmd->shim, client->fsaHandle, isWrite, buffer, size, count, filePos, fileHandle, flag));
			});
		}

		// --- FSA shim buffers ------------------------------------------------------------------------

		bool IsFSAClient(FSAClientHandle client)
		{
			std::lock_guard lock(s_fsaClientsLock);
			return std::find(s_fsaClients.begin(), s_fsaClients.end(), client) != s_fsaClients.end();
		}

		FSA_RESULT AllocShim(MEMPTR<FSAShimBuffer>* out)
		{
			if (!s_shimPoolReady)
				return FSA_RESULT::NOT_INIT;
			s_shimPoolLock.lock();
			uint8* mem = IPCBufPoolAllocate(s_shimPool, sizeof(FSAShimBuffer));
			s_shimPoolLock.unlock();
			if (!mem)
				return FSA_RESULT::OUT_OF_RESOURCES;
			memset(mem, 0, sizeof(FSAShimBuffer));
			*out = reinterpret_cast<FSAShimBuffer*>(mem);
			return FSA_RESULT::OK;
		}

		void FreeShim(FSAShimBuffer* shim)
		{
			s_shimPoolLock.lock();
			IPCBufPoolFree(s_shimPool, (uint8*)shim);
			s_shimPoolLock.unlock();
		}

		// Runs one synchronous FSA request. build() fills the shim buffer; on success onSuccess() may
		// copy results out and returns the final status
		template<typename TBuild, typename TSuccess>
		FSA_RESULT RunShim(TBuild&& build, TSuccess&& onSuccess)
		{
			StackAllocator<MEMPTR<FSAShimBuffer>> slot;
			FSA_RESULT r = AllocShim(slot.GetPointer());
			if (r != FSA_RESULT::OK)
				return r;
			FSAShimBuffer* shim = slot->GetPtr();
			r = build(shim);
			if (r == FSA_RESULT::OK)
			{
				r = SendShim(shim);
				if (r == FSA_RESULT::OK)
					r = onSuccess(shim);
			}
			FreeShim(shim);
			return r;
		}

		template<typename TBuild>
		FSA_RESULT RunShim(TBuild&& build)
		{
			return RunShim(std::forward<TBuild>(build), [](FSAShimBuffer*) { return FSA_RESULT::OK; });
		}

		void MountSDCard()
		{
			if (s_sdCardMounted)
				return;
			std::error_code ec;
			const auto hostPath = ActiveSettings::GetUserDataPath("sdcard/");
			fs::create_directories(hostPath, ec);
			FSCDeviceHostFS_Mount("/vol/external01", _pathToUtf8(hostPath), FSC_PRIORITY_BASE);
			s_sdCardMounted = true;
		}
	} // namespace

	// ---------------------------------------------------------------------------------------------
	// mounting

	enum class MOUNT_TYPE : uint32
	{
		SD = 0,
	};

	struct FS_MOUNT_SOURCE
	{
		uint32be sourceType;
		char path[128];
	};

	// The SD card is the only mount source we offer, so after FSGetMountSource there is never a next one
	FS_RESULT FSGetMountSourceNext(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, MOUNT_TYPE mountSourceType, FS_MOUNT_SOURCE* mountSourceInfo, FS_ERROR_MASK errMask)
	{
		if (mountSourceType == MOUNT_TYPE::SD)
			cemu_assert_suspicious(); // only expected after a successful FSGetMountSource
		else
			cemu_assert_unimplemented();
		return FS_RESULT::END_ITERATION;
	}

	FS_RESULT FSGetMountSource(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, MOUNT_TYPE mountSourceType, FS_MOUNT_SOURCE* mountSourceInfo, FS_ERROR_MASK errMask)
	{
		if (mountSourceType != MOUNT_TYPE::SD)
		{
			cemu_assert_unimplemented();
			return FS_RESULT::END_ITERATION;
		}
		// titles without the SD card capability in cos.xml see no SD card (One Piece loops forever otherwise)
		CosCapabilityBitsFS caps = static_cast<CosCapabilityBitsFS>(CafeSystem::GetForegroundTitleCosCapabilities(CosCapabilityGroup::FS));
		if (!HAS_FLAG(caps, CosCapabilityBitsFS::SDCARD_MOUNT))
		{
			cemuLog_logOnce(LogType::Force, "FS: title asked for the SD card mount source but lacks the SD card capability (this can be intended)");
			return FS_RESULT::END_ITERATION;
		}
		mountSourceInfo->sourceType = 0;
		strcpy(mountSourceInfo->path, "/sd");
		return FS_RESULT::SUCCESS;
	}

	FS_RESULT FSMount(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FS_MOUNT_SOURCE* mountSourceInfo, char* mountPathOut, sint32 mountPathMaxLength, FS_ERROR_MASK errMask)
	{
		if (mountSourceInfo->sourceType != 0)
			return FS_RESULT::ERR_PLACEHOLDER;
		if (strcmp(mountSourceInfo->path, "/sd") == 0)
		{
			if (!CopyStringIfFits(mountPathOut, mountPathMaxLength, "/vol/external01"))
				return FS_RESULT::ERR_PLACEHOLDER;
			MountSDCard();
		}
		return FS_RESULT::SUCCESS;
	}

	FS_RESULT FSBindMount(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* mountPathSrc, char* mountPathOut, FS_ERROR_MASK errMask)
	{
		if (strcmp(mountPathSrc, "/dev/sdcard01") == 0)
		{
			if (s_sdCardMounted)
				return FS_RESULT::ERR_PLACEHOLDER;
			std::error_code ec;
			const auto hostPath = ActiveSettings::GetUserDataPath("sdcard/");
			fs::create_directories(hostPath, ec);
			if (!FSCDeviceHostFS_Mount(mountPathOut, _pathToUtf8(hostPath), FSC_PRIORITY_BASE))
				return FS_RESULT::ERR_PLACEHOLDER;
			s_sdCardMounted = true;
			return FS_RESULT::SUCCESS;
		}
		if (strcmp(mountPathSrc, "/dev/mlc01") == 0)
		{
			if (s_mlcMounted)
				return FS_RESULT::ERR_PLACEHOLDER;
			if (!FSCDeviceHostFS_Mount(mountPathOut, _pathToUtf8(ActiveSettings::GetMlcPath()), FSC_PRIORITY_BASE))
				return FS_RESULT::ERR_PLACEHOLDER;
			s_mlcMounted = true;
			return FS_RESULT::SUCCESS;
		}
		return FS_RESULT::ERR_PLACEHOLDER;
	}

	// ---------------------------------------------------------------------------------------------
	// library and clients

	void FSInit()
	{
		s_fsInitialized = true;
	}

	void FSShutdown()
	{
		cemuLog_log(LogType::Force, "FSShutdown called");
	}

	sint32 FSGetClientNum()
	{
		sint32 count = 0;
		for (FSClientBody_t* it = s_clientList; it; it = it->nextClient.GetPtr())
			count++;
		return count;
	}

	FS_RESULT FSAddClientEx(FSClient_t* fsClient, uint32 uknR4, uint32 errHandling)
	{
		if (!s_fsInitialized || s_fsShutdown || !fsClient)
			HaltOnFatalError("FSAddClient(Ex) called with bad arguments or while FS is unavailable");
		LockFS();
		if (uknR4 != 0 && memory_readU32(uknR4) == 0)
		{
			UnlockFS();
			HaltOnFatalError("FSAddClientEx got an invalid attach parameter block");
		}
		if (IsClientRegistered(GetClientBody(fsClient)))
		{
			UnlockFS();
			HaltOnFatalError("FSAddClient(Ex) called for a client that is already registered");
		}
		FSClientBody_t* body = GetClientBody(fsClient);
		InitCmdQueue(&body->cmdQueue, MPTR_NULL, 1);
		IOSDevHandle fsaHandle = IOS_Open("/dev/fsa", 0);
		if (IOS_ResultIsError((IOS_ERROR)fsaHandle))
		{
			cemuLog_log(LogType::Force, "FSAddClientEx: no /dev/fsa handle available");
			UnlockFS();
			return FS_RESULT::FATAL_ERROR;
		}
		body->fsaHandle = fsaHandle;
		body->nextClient = s_clientList;
		s_clientList = body;
		UnlockFS();
		return FS_RESULT::SUCCESS;
	}

	FS_RESULT FSAddClient(FSClient_t* fsClient, uint32 errHandling)
	{
		return FSAddClientEx(fsClient, 0, errHandling);
	}

	FS_RESULT FSDelClient(FSClient_t* fsClient, uint32 errHandling)
	{
		if (!s_fsInitialized || s_fsShutdown || !fsClient)
			HaltOnFatalError("FSDelClient called with bad arguments or while FS is unavailable");
		FSClientBody_t* body = GetClientBody(fsClient);
		LockFS();
		IOS_Close(body->fsaHandle);
		// todo: commands still in flight are not waited for
		if (s_clientList == body)
		{
			s_clientList = body->nextClient.GetPtr();
		}
		else
		{
			FSClientBody_t* prev = s_clientList;
			FSClientBody_t* it = s_clientList;
			while (it && it != body)
			{
				prev = it;
				it = it->nextClient.GetPtr();
			}
			if (it)
				prev->nextClient = body->nextClient;
			else
				cemu_assert_debug(false); // client was not registered
		}
		UnlockFS();
		return FS_RESULT::SUCCESS;
	}

	// ---------------------------------------------------------------------------------------------
	// command blocks

	void FSInitCmdBlock(FSCmdBlock_t* fsCmdBlock)
	{
		memset(fsCmdBlock, 0, sizeof(FSCmdBlock_t));
		FSCmdBlockBody* cmd = GetCmdBody(fsCmdBlock);
		cmd->state = kCmdStateIdle;
		cmd->priority = kDefaultCmdPriority;
	}

	FSAsyncResult* FSGetAsyncResult(OSMessage* msg)
	{
		return (FSAsyncResult*)memory_getPointerFromVirtualOffset(msg->message);
	}

	void FSSetUserData(FSCmdBlock_t* fsCmdBlock, void* userData)
	{
		if (FSCmdBlockBody* cmd = GetCmdBody(fsCmdBlock))
			cmd->userData = userData;
	}

	void* FSGetUserData(FSCmdBlock_t* fsCmdBlock)
	{
		FSCmdBlockBody* cmd = GetCmdBody(fsCmdBlock);
		return cmd ? cmd->userData.GetPtr() : nullptr;
	}

	FSCmdBlock_t* FSGetCurrentCmdBlock(FSClient_t* fsClient)
	{
		FSClientBody_t* client = GetClientBody(fsClient);
		if (!client)
			return nullptr;
		FSCmdBlockBody* cmd = client->activeCmd.GetPtr();
		return cmd ? cmd->owner.GetPtr() : nullptr;
	}

	FS_VOLSTATE FSGetVolumeState(FSClient_t* fsClient)
	{
		return FS_VOLSTATE_READY; // volumes never go away here
	}

	sint32 FSGetErrorCodeForViewer(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock)
	{
		return 0;
	}

	sint32 FSGetLastErrorCodeForViewer(FSClient_t* fsClient)
	{
		if (FSGetCurrentCmdBlock(fsClient))
			cemu_assert_unimplemented();
		return 0;
	}

	// ---------------------------------------------------------------------------------------------
	// files

	sint32 FSOpenFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, char* mode, FSFileHandlePtr outFileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			if (!outFileHandle || !path || !mode)
				return kStatusBadRequest;
			cmd->out.fileHandleOut = outFileHandle;
			return BuildStatus(BuildOpenFile(&cmd->shim, client->fsaHandle, path, mode, 0x660, 0, 0));
		});
	}

	sint32 FSOpenFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, char* mode, FSFileHandlePtr outFileHandle, uint32 errHandling)
	{
		return RunBlocking(fsClient, fsCmdBlock, errHandling, [&](FSAsyncParams* p) { return FSOpenFileAsync(fsClient, fsCmdBlock, path, mode, outFileHandle, errHandling, p); });
	}

	sint32 FSOpenFileExAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, char* mode, uint32 createMode, uint32 openFlag, uint32 preallocSize, FSFileHandlePtr outFileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		if (openFlag != 0)
		{
			cemuLog_log(LogType::Force, "FSOpenFileEx: open flags {:#x} are not supported", openFlag);
			cemu_assert_debug(false);
		}
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			if (!outFileHandle || !path || !mode)
				return kStatusBadRequest;
			cmd->out.fileHandleOut = outFileHandle;
			return BuildStatus(BuildOpenFile(&cmd->shim, client->fsaHandle, path, mode, createMode, openFlag, preallocSize));
		});
	}

	sint32 FSOpenFileEx(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, char* mode, uint32 createMode, uint32 openFlag, uint32 preallocSize, FSFileHandlePtr outFileHandle, uint32 errHandling)
	{
		return RunBlocking(fsClient, fsCmdBlock, errHandling, [&](FSAsyncParams* p) { return FSOpenFileExAsync(fsClient, fsCmdBlock, path, mode, createMode, openFlag, preallocSize, outFileHandle, errHandling, p); });
	}

	// async operations that send nothing but a handle
	static sint32 SubmitHandleOp(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSA_CMD_OPERATION_TYPE op, uint32 handle, uint32 errorMask, FSAsyncParams* params)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, params, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			return BuildStatus(BuildHandleOp(&cmd->shim, client->fsaHandle, op, handle));
		});
	}

	sint32 FSCloseFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitHandleOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::CLOSEFILE, fileHandle, errorMask, fsAsyncParams);
	}

	sint32 FSCloseFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errHandling)
	{
		return RunBlocking(fsClient, fsCmdBlock, errHandling, [&](FSAsyncParams* p) { return FSCloseFileAsync(fsClient, fsCmdBlock, fileHandle, errHandling, p); });
	}

	sint32 FSFlushFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitHandleOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::FLUSHFILE, fileHandle, errorMask, fsAsyncParams);
	}

	sint32 FSFlushFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errHandling)
	{
		return RunBlocking(fsClient, fsCmdBlock, errHandling, [&](FSAsyncParams* p) { return FSFlushFileAsync(fsClient, fsCmdBlock, fileHandle, errHandling, p); });
	}

	sint32 FSReadFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		cemu_assert_debug(flag == 0);
		return SubmitTransfer(fsClient, fsCmdBlock, false, dst, size, count, false, 0, fileHandle, flag, errorMask, fsAsyncParams);
	}

	sint32 FSReadFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSReadFileAsync(fsClient, fsCmdBlock, dst, size, count, fileHandle, flag, errorMask, p); });
	}

	sint32 FSReadFileWithPosAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		cemu_assert_debug(flag == 0);
		return SubmitTransfer(fsClient, fsCmdBlock, false, dst, size, count, true, filePos, fileHandle, flag, errorMask, fsAsyncParams);
	}

	sint32 FSReadFileWithPos(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* dst, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSReadFileWithPosAsync(fsClient, fsCmdBlock, dst, size, count, filePos, fileHandle, flag, errorMask, p); });
	}

	sint32 FSWriteFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitTransfer(fsClient, fsCmdBlock, true, src, size, count, false, 0, fileHandle, flag, errorMask, fsAsyncParams);
	}

	sint32 FSWriteFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 fileHandle, uint32 flag, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSWriteFileAsync(fsClient, fsCmdBlock, src, size, count, fileHandle, flag, errorMask, p); });
	}

	sint32 FSWriteFileWithPosAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitTransfer(fsClient, fsCmdBlock, true, src, size, count, true, filePos, fileHandle, flag, errorMask, fsAsyncParams);
	}

	sint32 FSWriteFileWithPos(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, void* src, uint32 size, uint32 count, uint32 filePos, uint32 fileHandle, uint32 flag, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSWriteFileWithPosAsync(fsClient, fsCmdBlock, src, size, count, filePos, fileHandle, flag, errorMask, p); });
	}

	sint32 FSSetPosFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 filePos, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			return BuildStatus(BuildSetPos(&cmd->shim, client->fsaHandle, fileHandle, filePos));
		});
	}

	sint32 FSSetPosFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 filePos, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSSetPosFileAsync(fsClient, fsCmdBlock, fileHandle, filePos, errorMask, p); });
	}

	sint32 FSGetPosFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32be* returnedFilePos, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			cmd->out.filePosOut = returnedFilePos;
			return BuildStatus(BuildHandleOp(&cmd->shim, client->fsaHandle, FSA_CMD_OPERATION_TYPE::GETPOS, fileHandle));
		});
	}

	sint32 FSGetPosFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32be* returnedFilePos, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSGetPosFileAsync(fsClient, fsCmdBlock, fileHandle, returnedFilePos, errorMask, p); });
	}

	sint32 FSAppendFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 size, uint32 count, uint32 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			return BuildStatus(BuildAppend(&cmd->shim, client->fsaHandle, size, count, fileHandle, 0));
		});
	}

	sint32 FSAppendFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 size, uint32 count, uint32 fileHandle, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSAppendFileAsync(fsClient, fsCmdBlock, size, count, fileHandle, errorMask, p); });
	}

	sint32 FSTruncateFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSFileHandle2 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitHandleOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::TRUNCATEFILE, fileHandle, errorMask, fsAsyncParams);
	}

	sint32 FSTruncateFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSFileHandle2 fileHandle, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSTruncateFileAsync(fsClient, fsCmdBlock, fileHandle, errorMask, p); });
	}

	sint32 FSIsEofAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitHandleOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::ISEOF, fileHandle, errorMask, fsAsyncParams);
	}

	sint32 FSIsEof(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint32 fileHandle, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSIsEofAsync(fsClient, fsCmdBlock, fileHandle, errorMask, p); });
	}

	sint32 FSGetStatFileAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSFileHandle2 fileHandle, FSStat_t* statOut, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			cemu_assert(statOut);
			cmd->out.statOut = statOut;
			return BuildStatus(BuildHandleOp(&cmd->shim, client->fsaHandle, FSA_CMD_OPERATION_TYPE::GETSTATFILE, fileHandle));
		});
	}

	sint32 FSGetStatFile(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSFileHandle2 fileHandle, FSStat_t* statOut, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSGetStatFileAsync(fsClient, fsCmdBlock, fileHandle, statOut, errorMask, p); });
	}

	// ---------------------------------------------------------------------------------------------
	// paths and directories

	// async operations that send nothing but a path; a null path is rejected up front
	static sint32 SubmitPathOp(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSA_CMD_OPERATION_TYPE op, const char* path, bool rejectNullPath, uint32 errorMask, FSAsyncParams* params)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, params, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			if (rejectNullPath && !path)
			{
				cemu_assert_debug(false);
				return kStatusBadRequest;
			}
			return BuildStatus(BuildPathOp(&cmd->shim, client->fsaHandle, op, path));
		});
	}

	sint32 FSRenameAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* srcPath, char* dstPath, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			if (!srcPath || !dstPath)
			{
				cemu_assert_debug(false);
				return kStatusBadRequest;
			}
			return BuildStatus(BuildRename(&cmd->shim, client->fsaHandle, srcPath, dstPath));
		});
	}

	sint32 FSRename(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* srcPath, char* dstPath, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSRenameAsync(fsClient, fsCmdBlock, srcPath, dstPath, errorMask, p); });
	}

	sint32 FSRemoveAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint8* filePath, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitPathOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::REMOVE, (const char*)filePath, true, errorMask, fsAsyncParams);
	}

	sint32 FSRemove(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint8* filePath, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSRemoveAsync(fsClient, fsCmdBlock, filePath, errorMask, p); });
	}

	sint32 FSMakeDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* dirPath, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			if (!dirPath)
			{
				cemu_assert_debug(false);
				return kStatusBadRequest;
			}
			return BuildStatus(BuildMakeDir(&cmd->shim, client->fsaHandle, dirPath, 0x660));
		});
	}

	sint32 FSMakeDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSMakeDirAsync(fsClient, fsCmdBlock, path, errorMask, p); });
	}

	sint32 FSChangeDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitPathOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::CHANGEDIR, path, true, errorMask, fsAsyncParams);
	}

	sint32 FSChangeDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSChangeDirAsync(fsClient, fsCmdBlock, path, errorMask, p); });
	}

	sint32 FSGetCwdAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* dirPathOut, sint32 dirPathMaxLen, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			cmd->out.cwdOut.buffer = dirPathOut;
			cmd->out.cwdOut.length = dirPathMaxLen;
			return BuildStatus(BuildGetCwd(&cmd->shim, client->fsaHandle));
		});
	}

	sint32 FSGetCwd(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* dirPathOut, sint32 dirPathMaxLen, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSGetCwdAsync(fsClient, fsCmdBlock, dirPathOut, dirPathMaxLen, errorMask, p); });
	}

	sint32 FSFlushQuotaAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitPathOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::FLUSHQUOTA, path, false, errorMask, fsAsyncParams);
	}

	sint32 FSFlushQuota(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSFlushQuotaAsync(fsClient, fsCmdBlock, path, errorMask, p); });
	}

	sint32 FSOpenDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, FSDirHandlePtr dirHandleOut, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			cemu_assert(dirHandleOut && path);
			cmd->out.dirHandleOut = dirHandleOut.GetMPTR();
			return BuildStatus(BuildOpenDir(&cmd->shim, client->fsaHandle, path));
		});
	}

	sint32 FSOpenDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, char* path, FSDirHandlePtr dirHandleOut, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSOpenDirAsync(fsClient, fsCmdBlock, path, dirHandleOut, errorMask, p); });
	}

	sint32 FSReadDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, FSDirEntry_t* dirEntryOut, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			sint32 status = BuildStatus(BuildHandleOp(&cmd->shim, client->fsaHandle, FSA_CMD_OPERATION_TYPE::READDIR, dirHandle));
			if (status == 0)
				cmd->out.dirEntryOut = dirEntryOut;
			return status;
		});
	}

	// the export keeps its historical extra (unused) parameter
	sint32 FSReadDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, FSDirEntry_t* dirEntryOut, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSReadDirAsync(fsClient, fsCmdBlock, dirHandle, dirEntryOut, errorMask, p); });
	}

	sint32 FSCloseDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitHandleOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::CLOSEDIR, dirHandle, errorMask, fsAsyncParams);
	}

	sint32 FSCloseDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSCloseDirAsync(fsClient, fsCmdBlock, dirHandle, errorMask, p); });
	}

	sint32 FSRewindDirAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitHandleOp(fsClient, fsCmdBlock, FSA_CMD_OPERATION_TYPE::REWINDDIR, dirHandle, errorMask, fsAsyncParams);
	}

	sint32 FSRewindDir(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, FSDirHandle2 dirHandle, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSRewindDirAsync(fsClient, fsCmdBlock, dirHandle, errorMask, p); });
	}

	// ---------------------------------------------------------------------------------------------
	// queries

	sint32 __FSQueryInfoAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, uint8* queryString, uint32 queryType, void* queryResult, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return SubmitCommand(fsClient, fsCmdBlock, errorMask, fsAsyncParams, [&](FSClientBody_t* client, FSCmdBlockBody* cmd) -> sint32 {
			cemu_assert(queryString && queryResult);
			cmd->out.queryOut = queryResult;
			return BuildStatus(BuildQueryInfo(&cmd->shim, client->fsaHandle, (const char*)queryString, queryType));
		});
	}

	sint32 FSGetStatAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, FSStat_t* statOut, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return __FSQueryInfoAsync(fsClient, fsCmdBlock, (uint8*)path, FSA_QUERY_TYPE_STAT, statOut, errorMask, fsAsyncParams);
	}

	sint32 FSGetStat(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, FSStat_t* statOut, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSGetStatAsync(fsClient, fsCmdBlock, path, statOut, errorMask, p); });
	}

	sint32 FSGetFreeSpaceSizeAsync(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, FSLargeSize* returnedFreeSize, uint32 errorMask, FSAsyncParams* fsAsyncParams)
	{
		return __FSQueryInfoAsync(fsClient, fsCmdBlock, (uint8*)path, FSA_QUERY_TYPE_FREESPACE, returnedFreeSize, errorMask, fsAsyncParams);
	}

	sint32 FSGetFreeSpaceSize(FSClient_t* fsClient, FSCmdBlock_t* fsCmdBlock, const char* path, FSLargeSize* returnedFreeSize, uint32 errorMask)
	{
		return RunBlocking(fsClient, fsCmdBlock, errorMask, [&](FSAsyncParams* p) { return FSGetFreeSpaceSizeAsync(fsClient, fsCmdBlock, path, returnedFreeSize, errorMask, p); });
	}

	// ---------------------------------------------------------------------------------------------
	// FSA

	FSAClientHandle FSAAddClientEx(void* data)
	{
		if (data)
			cemu_assert_unimplemented(); // attach parameters are ignored
		IOSDevHandle handle = IOS_Open("/dev/fsa", 0);
		// note: IOSDevHandle is unsigned, so an IOS_Open error is not caught here and the error
		// value is registered and returned as the client handle (unchanged behaviour)
		std::lock_guard lock(s_fsaClientsLock);
		s_fsaClients.push_back((FSAClientHandle)handle);
		return (FSAClientHandle)handle;
	}

	FSAClientHandle FSAAddClient(void* data)
	{
		return FSAAddClientEx(data);
	}

	FSA_RESULT FSADelClient(FSAClientHandle clientHandle)
	{
		if (clientHandle == 0)
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		std::lock_guard lock(s_fsaClientsLock);
		auto it = std::find(s_fsaClients.begin(), s_fsaClients.end(), clientHandle);
		if (it != s_fsaClients.end())
		{
			IOS_Close(clientHandle);
			s_fsaClients.erase(it);
		}
		return FSA_RESULT::OK;
	}

	void FSAInit()
	{
		if (s_shimPoolReady)
			return;
		s_shimPool = IPCBufPoolCreate(s_shimPoolMemory.GetPtr(), s_shimPoolMemory.GetByteSize(), sizeof(FSAShimBuffer), &s_shimPoolEntryCount, 0);
		s_shimPoolReady = true;
	}

	FSA_RESULT FSACloseFile(FSAClientHandle client, uint32 fileHandle)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::CLOSEFILE, fileHandle); });
	}

	FSA_RESULT FSAFlushFile(FSAClientHandle client, uint32 fileHandle)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::FLUSHFILE, fileHandle); });
	}

	FSA_RESULT FSAMakeDir(FSAClientHandle client, const char* path, uint32 uknVal660)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildMakeDir(shim, client, path, uknVal660); });
	}

	FSA_RESULT FSARename(FSAClientHandle client, char* oldPath, char* newPath)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildRename(shim, client, oldPath, newPath); });
	}

	FSA_RESULT FSAChangeDir(FSAClientHandle client, char* path)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildPathOp(shim, client, FSA_CMD_OPERATION_TYPE::CHANGEDIR, path); });
	}

	FSA_RESULT FSAReadDir(FSAClientHandle client, FSDirHandle2 dirHandle, MEMPTR<FSDirEntry_t> directoryEntry)
	{
		if (directoryEntry.IsNull())
			return FSA_RESULT::INVALID_BUFFER;
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim(
			[&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::READDIR, dirHandle); },
			[&](FSAShimBuffer* shim) {
				*directoryEntry = shim->response.cmdReadDir.dirEntry;
				return FSA_RESULT::OK;
			});
	}

	FSA_RESULT FSAOpenDir(FSAClientHandle client, char* path, MEMPTR<uint32be> dirHandle)
	{
		if (dirHandle.IsNull())
			return FSA_RESULT::INVALID_BUFFER;
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim(
			[&](FSAShimBuffer* shim) { return BuildOpenDir(shim, client, path); },
			[&](FSAShimBuffer* shim) {
				*dirHandle = shim->response.cmdOpenDir.dirHandleOutput;
				return FSA_RESULT::OK;
			});
	}

	FSA_RESULT FSACloseDir(FSAClientHandle client, FSDirHandle2 dirHandle)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::CLOSEDIR, dirHandle); });
	}

	FSA_RESULT FSARewindDir(FSAClientHandle client, FSDirHandle2 dirHandle)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::REWINDDIR, dirHandle); });
	}

	FSA_RESULT FSAOpenFileEx(FSAClientHandle client, char* path, char* mode, uint32 createMode, uint32 openFlag, uint32 preallocSize, MEMPTR<uint32be> outFileHandle)
	{
		if (outFileHandle.IsNull())
			return FSA_RESULT::INVALID_BUFFER;
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim(
			[&](FSAShimBuffer* shim) { return BuildOpenFile(shim, client, path, mode, createMode, openFlag, preallocSize); },
			[&](FSAShimBuffer* shim) {
				*outFileHandle = shim->response.cmdOpenFile.fileHandleOutput;
				return FSA_RESULT::OK;
			});
	}

	FSA_RESULT FSAGetStatFile(FSAClientHandle client, FSFileHandle2 fileHandle, MEMPTR<FSStat_t> outStat)
	{
		if (outStat.IsNull())
			return FSA_RESULT::INVALID_BUFFER;
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim(
			[&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::GETSTATFILE, fileHandle); },
			[&](FSAShimBuffer* shim) {
				*outStat = shim->response.cmdStatFile.statOut;
				return FSA_RESULT::OK;
			});
	}

	FSA_RESULT FSASetPosFile(FSAClientHandle client, FSFileHandle2 fileHandle, uint32 pos)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildSetPos(shim, client, fileHandle, pos); });
	}

	FSA_RESULT FSATruncateFile(FSAClientHandle client, FSFileHandle2 fileHandle)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildHandleOp(shim, client, FSA_CMD_OPERATION_TYPE::TRUNCATEFILE, fileHandle); });
	}

	FSA_RESULT FSARemove(FSAClientHandle client, char* path)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildPathOp(shim, client, FSA_CMD_OPERATION_TYPE::REMOVE, path); });
	}

	FSA_RESULT FSAChangeMode(FSAClientHandle client, const char* path, uint32 permission)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildChangeMode(shim, client, path, permission, 0x666); });
	}

	// flag 0x2 is not passed on; the file position is always sent as 0
	FSA_RESULT FSAReadFile(FSAClientHandle client, void* buffer, uint32 size, uint32 count, FSFileHandle2 handle, uint32 flags)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildTransfer(shim, client, false, buffer, size, count, 0, handle, flags & ~0x2); });
	}

	FSA_RESULT FSAWriteFile(FSAClientHandle client, void* buffer, uint32 size, uint32 count, FSFileHandle2 handle, uint32 flags)
	{
		if (!IsFSAClient(client))
			return FSA_RESULT::INVALID_CLIENT_HANDLE;
		return RunShim([&](FSAShimBuffer* shim) { return BuildTransfer(shim, client, true, buffer, size, count, 0, handle, flags & ~0x2); });
	}

	// note: unlike the other FSA calls this does not check the client handle
	FSA_RESULT FSAGetInfoByQuery(FSAClientHandle client, char* path, uint32 queryType, MEMPTR<void> outData)
	{
		if (outData.IsNull())
			return FSA_RESULT::INVALID_BUFFER;
		return RunShim(
			[&](FSAShimBuffer* shim) { return BuildQueryInfo(shim, client, path, queryType); },
			[&](FSAShimBuffer* shim) {
				auto& answer = shim->response.cmdQueryInfo;
				switch (queryType)
				{
				case FSA_QUERY_TYPE_FREESPACE:
					*MEMPTR<uint64be>(outData.GetMPTR()) = answer.queryFreeSpace.freespace;
					return FSA_RESULT::OK;
				case FSA_QUERY_TYPE_DEVICE_INFO:
					*MEMPTR<FSADeviceInfo_t>(outData.GetMPTR()) = answer.queryDeviceInfo.info;
					return FSA_RESULT::OK;
				case FSA_QUERY_TYPE_STAT:
					*MEMPTR<FSStat_t>(outData.GetMPTR()) = answer.queryStat.stat;
					return FSA_RESULT::OK;
				default:
					cemu_assert_unimplemented();
					return FSA_RESULT::FATAL_ERROR;
				}
			});
	}

	FSA_RESULT FSAGetStat(FSAClientHandle client, char* path, FSStat_t* outStat)
	{
		return FSAGetInfoByQuery(client, path, FSA_QUERY_TYPE_STAT, outStat);
	}

	// note: these two pass each other's query type; kept as it always was
	FSA_RESULT FSAGetFreeSpaceSize(FSAClientHandle client, char* path, uint64* outSize)
	{
		return FSAGetInfoByQuery(client, path, FSA_QUERY_TYPE_DEVICE_INFO, outSize);
	}

	FSA_RESULT FSAGetDeviceInfo(FSAClientHandle client, char* path, void* outSize)
	{
		return FSAGetInfoByQuery(client, path, FSA_QUERY_TYPE_FREESPACE, outSize);
	}

	const char* FSAGetStatusStr(FSA_RESULT status)
	{
		switch (status)
		{
		case FSA_RESULT::OK: return s_strOK.GetPtr();
		case FSA_RESULT::NOT_INIT: return s_strNotInit.GetPtr();
		case FSA_RESULT::END_OF_DIRECTORY: return s_strEndOfDir.GetPtr();
		case FSA_RESULT::END_OF_FILE: return s_strEndOfFile.GetPtr();
		case FSA_RESULT::MAX_CLIENTS: return s_strMaxClients.GetPtr();
		case FSA_RESULT::MAX_FILES: return s_strMaxFiles.GetPtr();
		case FSA_RESULT::MAX_DIRS: return s_strMaxDirs.GetPtr();
		case FSA_RESULT::ALREADY_EXISTS: return s_strAlreadyExists.GetPtr();
		case FSA_RESULT::NOT_FOUND: return s_strNotFound.GetPtr();
		case FSA_RESULT::PERMISSION_ERROR: return s_strPermission.GetPtr();
		case FSA_RESULT::INVALID_PARAM: return s_strInvalidParam.GetPtr();
		case FSA_RESULT::INVALID_PATH: return s_strInvalidPath.GetPtr();
		case FSA_RESULT::INVALID_BUFFER: return s_strInvalidBuffer.GetPtr();
		case FSA_RESULT::INVALID_ALIGNMENT: return s_strInvalidAlignment.GetPtr();
		case FSA_RESULT::INVALID_CLIENT_HANDLE: return s_strInvalidClient.GetPtr();
		case FSA_RESULT::INVALID_FILE_HANDLE: return s_strInvalidFile.GetPtr();
		case FSA_RESULT::INVALID_DIR_HANDLE: return s_strInvalidDir.GetPtr();
		case FSA_RESULT::NOT_FILE: return s_strNotFile.GetPtr();
		case FSA_RESULT::NOT_DIR: return s_strNotDir.GetPtr();
		case FSA_RESULT::OUT_OF_RESOURCES: return s_strOutOfResources.GetPtr();
		case FSA_RESULT::FATAL_ERROR: return s_strUnknown.GetPtr();
		}
		cemu_assert_unimplemented();
		return s_strUnknown.GetPtr();
	}

	// only the SD card mount is supported
	FSA_RESULT FSAMount(FSAClientHandle client, const char* source, const char* target, uint32 flags, void* arg_buf, uint32 arg_len)
	{
		if (std::string_view(source) == "/dev/sdcard01" && std::string_view(target) == "/vol/external01" && flags == 0 && !arg_buf && arg_len == 0)
		{
			MountSDCard();
			return FSA_RESULT::OK;
		}
		cemu_assert_unimplemented();
		return FSA_RESULT::FATAL_ERROR;
	}

	FSA_RESULT FSAUnmount(FSAClientHandle client, const char* mountedTarget, uint32 flags)
	{
		return FSA_RESULT::OK;
	}

	void InitializeFS()
	{
		OSInitMutex(&s_fsMutex);

		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::CoreinitFile)
		EXPORT_FN(FSInit); EXPORT_FN(FSShutdown);

		EXPORT_FN(FSGetMountSource); EXPORT_FN(FSGetMountSourceNext); EXPORT_FN(FSMount);
		EXPORT_FN(FSBindMount);

		EXPORT_FN(FSAddClientEx); EXPORT_FN(FSAddClient); EXPORT_FN(FSDelClient);
		EXPORT_FN(FSGetClientNum);

		EXPORT_FN(FSInitCmdBlock); EXPORT_FN(FSGetAsyncResult);

		EXPORT_FN(FSOpenFileAsync); EXPORT_FN(FSOpenFile); EXPORT_FN(FSOpenFileExAsync);
		EXPORT_FN(FSOpenFileEx); EXPORT_FN(FSCloseFileAsync); EXPORT_FN(FSCloseFile);

		EXPORT_FN(FSReadFileAsync); EXPORT_FN(FSReadFile); EXPORT_FN(FSReadFileWithPosAsync);
		EXPORT_FN(FSReadFileWithPos);

		EXPORT_FN(FSWriteFileAsync); EXPORT_FN(FSWriteFile); EXPORT_FN(FSWriteFileWithPosAsync);
		EXPORT_FN(FSWriteFileWithPos);

		EXPORT_FN(FSSetPosFileAsync); EXPORT_FN(FSSetPosFile); EXPORT_FN(FSGetPosFileAsync);
		EXPORT_FN(FSGetPosFile);

		EXPORT_FN(FSAppendFileAsync); EXPORT_FN(FSAppendFile); EXPORT_FN(FSTruncateFileAsync);
		EXPORT_FN(FSTruncateFile);

		EXPORT_FN(FSRenameAsync); EXPORT_FN(FSRename); EXPORT_FN(FSRemoveAsync);
		EXPORT_FN(FSRemove); EXPORT_FN(FSMakeDirAsync); EXPORT_FN(FSMakeDir);
		EXPORT_FN(FSChangeDirAsync); EXPORT_FN(FSChangeDir); EXPORT_FN(FSGetCwdAsync);
		EXPORT_FN(FSGetCwd);

		EXPORT_FN(FSIsEofAsync); EXPORT_FN(FSIsEof);

		EXPORT_FN(FSOpenDirAsync); EXPORT_FN(FSOpenDir); EXPORT_FN(FSReadDirAsync);
		EXPORT_FN(FSReadDir); EXPORT_FN(FSCloseDirAsync); EXPORT_FN(FSCloseDir);

		EXPORT_FN(FSGetFreeSpaceSizeAsync); EXPORT_FN(FSGetFreeSpaceSize); EXPORT_FN(FSGetStatAsync);
		EXPORT_FN(FSGetStat); EXPORT_FN(FSGetStatFileAsync); EXPORT_FN(FSGetStatFile);

		EXPORT_FN(FSFlushQuotaAsync); EXPORT_FN(FSFlushQuota);

		EXPORT_FN(FSSetUserData); EXPORT_FN(FSGetUserData); EXPORT_FN(FSGetCurrentCmdBlock);
		EXPORT_FN(FSGetVolumeState);
#undef EXPORT_FN
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::Placeholder)
		EXPORT_FN(FSGetErrorCodeForViewer); EXPORT_FN(FSGetLastErrorCodeForViewer);

		EXPORT_FN(FSAMakeDir); EXPORT_FN(FSAInit); EXPORT_FN(FSAAddClient);
		EXPORT_FN(FSADelClient); EXPORT_FN(FSARewindDir); EXPORT_FN(FSAGetDeviceInfo);
		EXPORT_FN(FSARename); EXPORT_FN(FSAChangeDir); EXPORT_FN(FSAMount);
		EXPORT_FN(FSAUnmount); EXPORT_FN(FSAChangeMode); EXPORT_FN(FSAReadDir);
		EXPORT_FN(FSAOpenDir); EXPORT_FN(FSACloseDir); EXPORT_FN(FSACloseFile);
		EXPORT_FN(FSAFlushFile); EXPORT_FN(FSAOpenFileEx); EXPORT_FN(FSAGetStatFile);
		EXPORT_FN(FSAGetFreeSpaceSize); EXPORT_FN(FSASetPosFile); EXPORT_FN(FSATruncateFile);
		EXPORT_FN(FSARemove); EXPORT_FN(FSAReadFile); EXPORT_FN(FSAWriteFile);
		EXPORT_FN(FSAGetStat); EXPORT_FN(FSAGetStatusStr);
#undef EXPORT_FN

		s_clientList = nullptr;
	}
} // namespace coreinit
