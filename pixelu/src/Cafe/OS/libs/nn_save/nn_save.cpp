// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_save gives a title access to its save data without it having to know
// where that data lives. Every SAVE* call names an account slot (1..12, or
// 0xFF for data shared by all accounts) and a path relative to that account's
// save folder; we turn it into an absolute FS path and hand it to coreinit FS.
//
//   own title     /vol/save/<persistent id>/<path>   or /vol/save/common/<path>
//   other titles  /vol/storage_mlc01/usr/save/<hi>/<lo>/user/<id or common>/<path>
//
// /vol/save itself is mapped to the running title's MLC save folder by
// nn_acp (ACPMountSaveDir), so these paths decide where save files land on the
// host. They must not change, or existing saves stop loading.
//
// The blocking variants issue the async request with our own completion
// callback and sleep on an event until FS reports back.

#include "Cafe/OS/common/OSCommon.h"
#include "nn_save.h"
#include "Cafe/OS/libs/nn_acp/nn_acp.h"
#include "Cafe/OS/libs/nn_act/nn_act.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/ActiveSettings.h"

namespace nn::save
{
	using SAVEStatus = FSStatus;
	using coreinit::FSClient_t;
	using coreinit::FSCmdBlock_t;

	namespace
	{
		constexpr SAVEStatus kOk = (SAVEStatus)FS_RESULT::SUCCESS;
		constexpr SAVEStatus kNotFound = (SAVEStatus)FS_RESULT::NOT_FOUND;
		constexpr SAVEStatus kFatal = (SAVEStatus)FS_RESULT::FATAL_ERROR;
		constexpr SAVEStatus kNoPermission = (SAVEStatus)FS_RESULT::PERMISSION_ERROR;

		constexpr uint8 kCommonSlot = 0xFF;
		constexpr uint8 kSlotCount = 12; // account slots are numbered 1..12
		constexpr size_t kPathMax = FSA_CMD_PATH_MAX_LENGTH;
		constexpr uint32 kAppBoxMlc = 3;

		struct SaveLibState
		{
			bool ready;
			coreinit::OSMutex lock;
			FSClient_t fsClient;
			FSCmdBlock_t fsCmd;
			uint32 slotPersistentId[kSlotCount];
		};
		SysAllocator<SaveLibState> s_lib;

		// Lives in guest memory: FS writes the result through userContext.
		struct BlockingWait
		{
			coreinit::OSEvent done;
			betype<SAVEStatus> status;
		};

		void OnBlockingRequestDone(FSClient_t* client, FSCmdBlock_t* cmd, sint32 status, BlockingWait* wait)
		{
			wait->status = status;
			coreinit::OSSignalEvent(&wait->done);
		}

		// Calls issue(asyncParams) and, if the request was queued, waits for it.
		template<typename TIssue>
		SAVEStatus RunBlocking(TIssue&& issue)
		{
			static const MPTR s_doneCallback = makeCallableExport<&OnBlockingRequestDone>();
			StackAllocator<BlockingWait> wait;
			StackAllocator<FSAsyncParams> params;
			coreinit::OSInitEvent(&wait->done, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			wait->status = kOk;
			params->userCallback = MEMPTR<void>(s_doneCallback);
			params->userContext = wait.GetPointer();
			params->ioMsgQueue = nullptr;
			SAVEStatus queued = issue(params.GetPointer());
			if (queued != kOk)
				return queued;
			coreinit::OSWaitEvent(&wait->done);
			return wait->status;
		}

		uint32 CachedPersistentId(uint8 slot)
		{
			if (slot < 1 || slot > kSlotCount)
				return 0;
			return s_lib->slotPersistentId[slot - 1];
		}

		// Formats into out (kPathMax bytes). False if the result would not fit.
		template<typename... TArgs>
		bool FormatPath(char* out, const char* fmt, TArgs... args)
		{
			int n = snprintf(out, kPathMax - 1, fmt, args...);
			return n >= 0 && (size_t)n < kPathMax - 1;
		}

		bool OwnSavePath(uint32 persistentId, const char* rel, char* out)
		{
			const char* sub = rel ? rel : "";
			if (persistentId == 0)
				return FormatPath(out, "/vol/save/common/%s", sub);
			return FormatPath(out, "/vol/save/%08x/%s", persistentId, sub);
		}

		FS_RESULT OtherTitleSavePath(uint32 persistentId, uint64 titleId, const char* rel, char* out)
		{
			uint32be box;
			if (acp::ACPGetApplicationBox(&box, titleId) != acp::ACPStatus::SUCCESS)
				return FS_RESULT::NOT_FOUND;
			if ((uint32)box != kAppBoxMlc)
			{
				// titles on USB storage are never reported by our ACP
				cemuLog_log(LogType::Save, "nn_save: title {:016x} is in application box {} which is not supported", titleId, (uint32)box);
				return FS_RESULT::NOT_FOUND;
			}
			const char* sub = rel ? rel : "";
			const uint32 hi = (uint32)(titleId >> 32), lo = (uint32)titleId;
			bool fits = persistentId != 0
				? FormatPath(out, "/vol/storage_mlc01/usr/save/%08x/%08x/user/%08x/%s", hi, lo, persistentId, sub)
				: FormatPath(out, "/vol/storage_mlc01/usr/save/%08x/%08x/user/common/%s", hi, lo, sub);
			return fits ? FS_RESULT::SUCCESS : FS_RESULT::FATAL_ERROR;
		}

		// Title id of another application from its unique id (bits 8..27 of the low word).
		uint64 TitleFromUniqueId(uint32 uniqueId, uint8 variation, bool demo)
		{
			uint64 hi = 0x00050000u | (demo ? 0x2u : 0x0u) | (uniqueId >> 24);
			uint32 lo = 0x10000000u | (uint32)(uniqueId << 8) | variation;
			return (hi << 32) | lo;
		}

		// Resolves (slot, rel) for the running title under the library lock and
		// passes the absolute path to issue(). Path errors never reach FS.
		template<typename TIssue>
		SAVEStatus WithOwnPath(uint8 slot, const char* rel, TIssue&& issue)
		{
			coreinit::OSLockMutex(&s_lib->lock);
			SAVEStatus result;
			uint32 persistentId;
			char path[kPathMax];
			if (!GetPersistentIdEx(slot, &persistentId))
				result = kNotFound;
			else if (!OwnSavePath(persistentId, rel, path))
				result = kFatal;
			else
				result = issue(path, persistentId);
			coreinit::OSUnlockMutex(&s_lib->lock);
			return result;
		}

		template<typename TIssue>
		SAVEStatus WithOtherTitlePath(uint64 titleId, uint8 slot, const char* rel, TIssue&& issue)
		{
			coreinit::OSLockMutex(&s_lib->lock);
			SAVEStatus result = kFatal;
			uint32 persistentId;
			char path[kPathMax];
			if (!GetPersistentIdEx(slot, &persistentId))
				result = kNotFound;
			else if (OtherTitleSavePath(persistentId, titleId, rel, path) == FS_RESULT::SUCCESS)
				result = issue(path);
			coreinit::OSUnlockMutex(&s_lib->lock);
			return result;
		}

		void CreateTitleSaveFolders(uint64 titleId)
		{
			// the save folder lives under the base title id (update/DLC bits cleared)
			const uint32 hi = (uint32)(titleId >> 32) & ~0xCu;
			const uint32 lo = (uint32)titleId;
			char path[256];
			sint32 fscStatus;
			snprintf(path, sizeof(path), "/vol/storage_mlc01/usr/save/%08x/", hi);
			fsc_createDir(path, &fscStatus);
			snprintf(path, sizeof(path), "/vol/storage_mlc01/usr/save/%08x/%08x/", hi, lo);
			fsc_createDir(path, &fscStatus);
			snprintf(path, sizeof(path), "/vol/storage_mlc01/usr/save/%08x/%08x/meta/", hi, lo);
			fsc_createDir(path, &fscStatus);
		}
	}

	bool GetPersistentIdEx(uint8 accountSlot, uint32* persistentId)
	{
		if (accountSlot == kCommonSlot)
		{
			*persistentId = 0;
			return true;
		}
		*persistentId = CachedPersistentId(accountSlot);
		return *persistentId != 0;
	}

	// --- setup -------------------------------------------------------------

	SAVEStatus SAVEInit()
	{
		if (s_lib->ready)
			return kOk;
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		coreinit::OSInitMutexEx(&s_lib->lock, nullptr);
		act::Initialize();
		coreinit::FSAddClientEx(&s_lib->fsClient, 0, 0);
		coreinit::FSInitCmdBlock(&s_lib->fsCmd);
		for (uint8 slot = 1; slot <= kSlotCount; slot++)
			s_lib->slotPersistentId[slot - 1] = act::GetPersistentIdEx(slot);
		acp::ACPMountSaveDir();
		s_lib->ready = true;
		CreateTitleSaveFolders(titleId);
		iosu::acp::CreateSaveMetaFiles(ActiveSettings::GetPersistentId(), titleId);
		return kOk;
	}

	SAVEStatus SAVEInitSaveDir(uint8 accountSlot)
	{
		coreinit::OSLockMutex(&s_lib->lock);
		SAVEStatus result = kNotFound;
		uint32 persistentId;
		if (GetPersistentIdEx(accountSlot, &persistentId))
		{
			acp::ACPStatus acpResult = acp::ACPCreateSaveDir(persistentId, iosu::acp::ACPDeviceType::InternalDeviceType);
			if (acpResult != acp::ACPStatus::SUCCESS)
				cemuLog_log(LogType::Save, "SAVEInitSaveDir: ACPCreateSaveDir failed for {:08x}", persistentId);
			result = kOk; // like the console, a folder that already exists is not an error
		}
		coreinit::OSUnlockMutex(&s_lib->lock);
		return result;
	}

	SAVEStatus SAVEGetSharedDataTitlePath(uint64 titleId, const char* dataFileName, char* output, sint32 outputLength)
	{
		int n = snprintf(output, outputLength, "/vol/storage_mlc01/sys/title/%08x/%08x/content/%s", (uint32)(titleId >> 32), (uint32)titleId, dataFileName);
		return (n >= 0 && n < outputLength) ? kOk : kFatal;
	}

	SAVEStatus SAVEGetSharedSaveDataPath(uint64 titleId, const char* dataFileName, char* output, uint32 outputLength)
	{
		int n = snprintf(output, outputLength, "/vol/storage_mlc01/usr/save/%08x/%08x/user/common/%s", (uint32)(titleId >> 32), (uint32)titleId, dataFileName);
		return (n >= 0 && (uint32)n < outputLength) ? kOk : kFatal;
	}

	// --- own title: files and folders ---------------------------------------

	SAVEStatus SAVEOpenFileAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, path, [&](char* full, uint32) {
			return coreinit::FSOpenFileAsync(client, block, full, (char*)mode, outFileHandle, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEOpenFile(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEOpenFileAsync(client, block, accountSlot, path, mode, outFileHandle, errHandling, p); });
	}

	SAVEStatus SAVEOpenDirAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, path, [&](char* full, uint32) {
			return coreinit::FSOpenDirAsync(client, block, full, hDir, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEOpenDir(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEOpenDirAsync(client, block, accountSlot, path, hDir, errHandling, p); });
	}

	SAVEStatus SAVEGetStatAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, path, [&](char* full, uint32) {
			return coreinit::__FSQueryInfoAsync(client, block, (uint8*)full, FSA_QUERY_TYPE_STAT, stat, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEGetStat(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEGetStatAsync(client, block, accountSlot, path, stat, errHandling, p); });
	}

	SAVEStatus SAVEGetFreeSpaceSizeAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, FSLargeSize* freeSize, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, nullptr, [&](char* full, uint32) {
			return coreinit::FSGetFreeSpaceSizeAsync(client, block, full, freeSize, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEGetFreeSpaceSize(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, FSLargeSize* freeSize, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEGetFreeSpaceSizeAsync(client, block, accountSlot, freeSize, errHandling, p); });
	}

	SAVEStatus SAVEMakeDirAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, path, [&](char* full, uint32) {
			return coreinit::FSMakeDirAsync(client, block, full, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEMakeDir(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEMakeDirAsync(client, block, accountSlot, path, errHandling, p); });
	}

	SAVEStatus SAVERemoveAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, path, [&](char* full, uint32) {
			return coreinit::FSRemoveAsync(client, block, (uint8*)full, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVERemove(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVERemoveAsync(client, block, accountSlot, path, errHandling, p); });
	}

	SAVEStatus SAVEChangeDirAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, path, [&](char* full, uint32) {
			return coreinit::FSChangeDirAsync(client, block, full, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEChangeDir(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* path, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEChangeDirAsync(client, block, accountSlot, path, errHandling, p); });
	}

	SAVEStatus SAVERenameAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* oldPath, const char* newPath, FS_ERROR_MASK errHandling, FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, oldPath, [&](char* fullOld, uint32 persistentId) {
			char fullNew[kPathMax];
			if (!OwnSavePath(persistentId, newPath, fullNew))
				return kFatal;
			return (SAVEStatus)coreinit::FSRenameAsync(client, block, fullOld, fullNew, errHandling, asyncParams);
		});
	}

	SAVEStatus SAVERename(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, const char* oldPath, const char* newPath, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVERenameAsync(client, block, accountSlot, oldPath, newPath, errHandling, p); });
	}

	SAVEStatus SAVEFlushQuotaAsync(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOwnPath(accountSlot, nullptr, [&](char* full, uint32 persistentId) {
			SAVEStatus r = coreinit::FSFlushQuotaAsync(client, block, full, errHandling, (FSAsyncParams*)asyncParams);
			// committing the save is what the system menu shows as "last played/saved"
			acp::ACPUpdateSaveTimeStamp(persistentId, CafeSystem::GetForegroundTitleId(), iosu::acp::ACPDeviceType::InternalDeviceType);
			return r;
		});
	}

	SAVEStatus SAVEFlushQuota(FSClient_t* client, FSCmdBlock_t* block, uint8 accountSlot, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEFlushQuotaAsync(client, block, accountSlot, errHandling, p); });
	}

	// --- other titles (read only) --------------------------------------------

	SAVEStatus SAVEOpenFileOtherApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint64 titleId, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		if (strcmp(mode, "r") != 0)
			return kNoPermission;
		return WithOtherTitlePath(titleId, accountSlot, path, [&](char* full) {
			return coreinit::FSOpenFileAsync(client, block, full, (char*)mode, outFileHandle, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEOpenFileOtherApplication(FSClient_t* client, FSCmdBlock_t* block, uint64 titleId, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEOpenFileOtherApplicationAsync(client, block, titleId, accountSlot, path, mode, outFileHandle, errHandling, p); });
	}

	SAVEStatus SAVEOpenFileOtherNormalApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEOpenFileOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, 0, false), accountSlot, path, mode, outFileHandle, errHandling, asyncParams);
	}

	SAVEStatus SAVEOpenFileOtherNormalApplication(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling)
	{
		return SAVEOpenFileOtherApplication(client, block, TitleFromUniqueId(uniqueId, 0, false), accountSlot, path, mode, outFileHandle, errHandling);
	}

	SAVEStatus SAVEOpenFileOtherNormalApplicationVariationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEOpenFileOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, variation, false), accountSlot, path, mode, outFileHandle, errHandling, asyncParams);
	}

	SAVEStatus SAVEOpenFileOtherNormalApplicationVariation(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, const char* mode, FSFileHandlePtr outFileHandle, FS_ERROR_MASK errHandling)
	{
		return SAVEOpenFileOtherApplication(client, block, TitleFromUniqueId(uniqueId, variation, false), accountSlot, path, mode, outFileHandle, errHandling);
	}

	SAVEStatus SAVEOpenDirOtherApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint64 titleId, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOtherTitlePath(titleId, accountSlot, path, [&](char* full) {
			return coreinit::FSOpenDirAsync(client, block, full, hDir, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEOpenDirOtherApplication(FSClient_t* client, FSCmdBlock_t* block, uint64 titleId, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEOpenDirOtherApplicationAsync(client, block, titleId, accountSlot, path, hDir, errHandling, p); });
	}

	SAVEStatus SAVEOpenDirOtherNormalApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEOpenDirOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, 0, false), accountSlot, path, hDir, errHandling, asyncParams);
	}

	SAVEStatus SAVEOpenDirOtherNormalApplication(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling)
	{
		return SAVEOpenDirOtherApplication(client, block, TitleFromUniqueId(uniqueId, 0, false), accountSlot, path, hDir, errHandling);
	}

	SAVEStatus SAVEOpenDirOtherNormalApplicationVariationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEOpenDirOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, variation, false), accountSlot, path, hDir, errHandling, asyncParams);
	}

	SAVEStatus SAVEOpenDirOtherNormalApplicationVariation(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, FSDirHandlePtr hDir, FS_ERROR_MASK errHandling)
	{
		return SAVEOpenDirOtherApplication(client, block, TitleFromUniqueId(uniqueId, variation, false), accountSlot, path, hDir, errHandling);
	}

	SAVEStatus SAVEGetStatOtherApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint64 titleId, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return WithOtherTitlePath(titleId, accountSlot, path, [&](char* full) {
			return coreinit::__FSQueryInfoAsync(client, block, (uint8*)full, FSA_QUERY_TYPE_STAT, stat, errHandling, (FSAsyncParams*)asyncParams);
		});
	}

	SAVEStatus SAVEGetStatOtherApplication(FSClient_t* client, FSCmdBlock_t* block, uint64 titleId, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling)
	{
		return RunBlocking([&](FSAsyncParams* p) { return SAVEGetStatOtherApplicationAsync(client, block, titleId, accountSlot, path, stat, errHandling, p); });
	}

	SAVEStatus SAVEGetStatOtherNormalApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEGetStatOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, 0, false), accountSlot, path, stat, errHandling, asyncParams);
	}

	SAVEStatus SAVEGetStatOtherNormalApplication(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling)
	{
		return SAVEGetStatOtherApplication(client, block, TitleFromUniqueId(uniqueId, 0, false), accountSlot, path, stat, errHandling);
	}

	SAVEStatus SAVEGetStatOtherNormalApplicationVariationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEGetStatOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, variation, false), accountSlot, path, stat, errHandling, asyncParams);
	}

	SAVEStatus SAVEGetStatOtherNormalApplicationVariation(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling)
	{
		return SAVEGetStatOtherApplication(client, block, TitleFromUniqueId(uniqueId, variation, false), accountSlot, path, stat, errHandling);
	}

	SAVEStatus SAVEGetStatOtherDemoApplicationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEGetStatOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, 0, true), accountSlot, path, stat, errHandling, asyncParams);
	}

	SAVEStatus SAVEGetStatOtherDemoApplicationVariationAsync(FSClient_t* client, FSCmdBlock_t* block, uint32 uniqueId, uint8 variation, uint8 accountSlot, const char* path, FSStat_t* stat, FS_ERROR_MASK errHandling, const FSAsyncParams* asyncParams)
	{
		return SAVEGetStatOtherApplicationAsync(client, block, TitleFromUniqueId(uniqueId, variation, true), accountSlot, path, stat, errHandling, asyncParams);
	}

	// --- module ----------------------------------------------------------------

	void ResetToDefaultState()
	{
		if (!s_lib->ready)
			return;
		acp::ACPUnmountSaveDir();
		s_lib->ready = false;
	}

	class SaveModule : public COSModule
	{
	public:
		std::string_view GetName() override { return "nn_save"; }

		void RPLMapped() override
		{
			// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("nn_save", fn, LogType::Save)
			EXPORT_FN(SAVEInit); EXPORT_FN(SAVEInitSaveDir); EXPORT_FN(SAVEGetSharedDataTitlePath);
			EXPORT_FN(SAVEGetSharedSaveDataPath);

			EXPORT_FN(SAVEOpenFile); EXPORT_FN(SAVEOpenFileAsync); EXPORT_FN(SAVEOpenDir);
			EXPORT_FN(SAVEOpenDirAsync); EXPORT_FN(SAVEGetStat); EXPORT_FN(SAVEGetStatAsync);
			EXPORT_FN(SAVEGetFreeSpaceSize); EXPORT_FN(SAVEGetFreeSpaceSizeAsync); EXPORT_FN(SAVEMakeDir);
			EXPORT_FN(SAVEMakeDirAsync); EXPORT_FN(SAVERemove); EXPORT_FN(SAVERemoveAsync);
			EXPORT_FN(SAVEChangeDir); EXPORT_FN(SAVEChangeDirAsync); EXPORT_FN(SAVERename);
			EXPORT_FN(SAVERenameAsync); EXPORT_FN(SAVEFlushQuota); EXPORT_FN(SAVEFlushQuotaAsync);

			EXPORT_FN(SAVEOpenFileOtherApplication); EXPORT_FN(SAVEOpenFileOtherApplicationAsync);
			EXPORT_FN(SAVEOpenFileOtherNormalApplication); EXPORT_FN(SAVEOpenFileOtherNormalApplicationAsync);
			EXPORT_FN(SAVEOpenFileOtherNormalApplicationVariation); EXPORT_FN(SAVEOpenFileOtherNormalApplicationVariationAsync);
			EXPORT_FN(SAVEOpenDirOtherApplication); EXPORT_FN(SAVEOpenDirOtherApplicationAsync);
			EXPORT_FN(SAVEOpenDirOtherNormalApplication); EXPORT_FN(SAVEOpenDirOtherNormalApplicationAsync);
			EXPORT_FN(SAVEOpenDirOtherNormalApplicationVariation); EXPORT_FN(SAVEOpenDirOtherNormalApplicationVariationAsync);
			EXPORT_FN(SAVEGetStatOtherApplication); EXPORT_FN(SAVEGetStatOtherApplicationAsync);
			EXPORT_FN(SAVEGetStatOtherNormalApplication); EXPORT_FN(SAVEGetStatOtherNormalApplicationAsync);
			EXPORT_FN(SAVEGetStatOtherNormalApplicationVariation); EXPORT_FN(SAVEGetStatOtherNormalApplicationVariationAsync);
			EXPORT_FN(SAVEGetStatOtherDemoApplicationAsync); EXPORT_FN(SAVEGetStatOtherDemoApplicationVariationAsync);
#undef EXPORT_FN
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded || reason == coreinit::RplEntryReason::Unloaded)
				ResetToDefaultState();
		}
	};

	SaveModule s_saveModule;

	COSModule* GetModule()
	{
		return &s_saveModule;
	}
}
