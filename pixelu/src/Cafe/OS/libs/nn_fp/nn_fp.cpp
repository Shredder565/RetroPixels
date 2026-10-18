// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_fp is the friend list library. All the work is done by the friend
// service /dev/fpd (iosu_fpd.cpp); this file turns each nn::fp call into an
// ioctlv on that device and hands the answer back.
//
// Wire format expected by /dev/fpd: the vector list starts with the reply
// buffers followed by the request buffers, and the ioctlv is sent with the
// reply count in the "in" slot and the request count in the "out" slot.
//
// Every request gets its own block in a small PPC-side heap: the vector list
// plus private copies of all buffers. Request data is copied in before
// sending, reply data is copied out to the game's buffers when the answer
// arrives. That keeps asynchronous calls safe even when the game's request
// buffers were on its stack.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/IOSU/legacy/iosu_fpd.h"
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "Cafe/OS/libs/nn_common.h"
#include "util/ChunkedHeap/ChunkedHeap.h"
#include "Common/CafeString.h"
#include "nn_fp.h"

namespace nn::fp
{
	namespace
	{
		namespace fpd = iosu::fpd;
		using RequestId = fpd::FPD_REQUEST_ID;

		constexpr nnResult kResultNotInitialized = 0xC0C00580;
		constexpr nnResult kResultBadArgument = BUILD_NN_RESULT(NN_RESULT_LEVEL_LVL6, NN_RESULT_MODULE_NN_FP, 0x680);
		constexpr uint32 kMaxBuffers = 8;
		constexpr uint32 kMaxNotifications = 256;

		// ---- PPC-side heap for request blocks -------------------------------

		SysAllocator<uint8, 0x12000> g_heapSpace;
		VHeap* g_heap = nullptr;
		std::mutex g_heapLock;

		void* HeapAlloc(uint32 size)
		{
			std::lock_guard lock(g_heapLock);
			void* p = g_heap ? g_heap->alloc(size, 32) : nullptr;
			if (!p)
				cemuLog_log(LogType::Force, "nn_fp: request heap exhausted");
			return p;
		}

		void HeapFree(void* p)
		{
			if (!p)
				return;
			std::lock_guard lock(g_heapLock);
			g_heap->free(p);
		}

		// ---- one /dev/fpd request -------------------------------------------

		struct Buffer
		{
			void* ptr;
			uint32 size;
		};

		struct Request;
		using HostCompletion = void (*)(Request* request);

		// Lives in PPC memory: the vector list must be visible to the IPC layer.
		struct Request
		{
			IPCIoctlVector vectors[kMaxBuffers];
			MEMPTR<void> gameBuffer[kMaxBuffers]; // where reply data goes back to
			uint32 replyCount;
			uint32 argCount;
			MEMPTR<void> storage;
			MEMPTR<void> gameCallback;
			MEMPTR<void> gameCallbackArg;
			HostCompletion hostCompletion;
			betype<nnResult> result; // games' callbacks receive a pointer to this
		};

		struct State
		{
			uint32 initCount = 0;
			bool admin = false;
			IOSDevHandle device = 0;
			MEMPTR<void> notifyHandler;
			MEMPTR<void> notifyHandlerArg;
			bool notifyPending = false;
		};
		State g_state;

		SysAllocator<coreinit::OSMutex> g_apiMutex;
		SysAllocator<fpd::FPDNotification, kMaxNotifications> g_notifications;
		SysAllocator<uint32be> g_notificationCount;

		// The game-visible mutex that serialises nn::fp calls.
		class ApiLock
		{
		public:
			ApiLock() { coreinit::OSLockMutex(g_apiMutex.GetPtr()); }
			~ApiLock() { coreinit::OSUnlockMutex(g_apiMutex.GetPtr()); }
			ApiLock(const ApiLock&) = delete;
			ApiLock& operator=(const ApiLock&) = delete;
		};

		constexpr uint32 AlignUp32(uint32 v)
		{
			return (v + 31) & ~31u;
		}

		Request* BuildRequest(std::initializer_list<Buffer> replies, std::initializer_list<Buffer> args)
		{
			const uint32 total = (uint32)(replies.size() + args.size());
			cemu_assert(total <= kMaxBuffers);
			auto* req = (Request*)HeapAlloc(sizeof(Request));
			if (!req)
				return nullptr;
			memset((void*)req, 0, sizeof(Request));
			req->replyCount = (uint32)replies.size();
			req->argCount = (uint32)args.size();

			uint32 bytes = 0;
			for (const Buffer& b : replies)
				bytes += AlignUp32(b.size);
			for (const Buffer& b : args)
				bytes += AlignUp32(b.size);
			uint8* storage = bytes ? (uint8*)HeapAlloc(bytes) : nullptr;
			if (bytes && !storage)
			{
				HeapFree(req);
				return nullptr;
			}
			req->storage = storage;

			uint32 slot = 0;
			uint32 offset = 0;
			auto place = [&](const Buffer& b, bool copyIn) {
				uint8* copy = storage ? storage + offset : nullptr;
				if (copyIn && b.size)
					memcpy(copy, b.ptr, b.size);
				req->vectors[slot].baseVirt = copy;
				req->vectors[slot].size = b.size;
				req->vectors[slot].basePhys = nullptr;
				req->gameBuffer[slot] = b.ptr;
				offset += AlignUp32(b.size);
				slot++;
			};
			for (const Buffer& b : replies)
				place(b, false);
			for (const Buffer& b : args)
				place(b, true);
			return req;
		}

		void CopyRepliesOut(Request* req)
		{
			for (uint32 i = 0; i < req->replyCount; i++)
			{
				const uint32 size = req->vectors[i].size;
				if (size && req->gameBuffer[i])
					memcpy(req->gameBuffer[i].GetPtr(), req->vectors[i].baseVirt.GetPtr(), size);
			}
		}

		void FreeRequest(Request* req)
		{
			HeapFree(req->storage.GetPtr());
			HeapFree(req);
		}

		nnResult Call(RequestId id, std::initializer_list<Buffer> replies, std::initializer_list<Buffer> args = {})
		{
			Request* req = BuildRequest(replies, args);
			if (!req)
				return BUILD_NN_RESULT(NN_RESULT_LEVEL_FATAL, NN_RESULT_MODULE_NN_FP, 0);
			const nnResult r = (nnResult)coreinit::IOS_Ioctlv(g_state.device, (uint32)id, req->replyCount, req->argCount, req->vectors);
			CopyRepliesOut(req);
			FreeRequest(req);
			return r;
		}

		// Runs on the IPC callback thread once /dev/fpd has answered.
		void OnAsyncReply(uint32 result, Request* req)
		{
			req->result = result;
			CopyRepliesOut(req);
			if (req->hostCompletion)
				req->hostCompletion(req);
			else if (req->gameCallback)
				PPCCoreCallback(req->gameCallback.GetMPTR(), &req->result, req->gameCallbackArg);
			FreeRequest(req);
		}

		nnResult CallAsync(RequestId id, std::initializer_list<Buffer> replies, std::initializer_list<Buffer> args,
						   MEMPTR<void> callback, MEMPTR<void> callbackArg, HostCompletion hostCompletion = nullptr)
		{
			Request* req = BuildRequest(replies, args);
			if (!req)
				return BUILD_NN_RESULT(NN_RESULT_LEVEL_FATAL, NN_RESULT_MODULE_NN_FP, 0);
			req->gameCallback = callback;
			req->gameCallbackArg = callbackArg;
			req->hostCompletion = hostCompletion;
			const MEMPTR<void> onReply{ makeCallableExport<OnAsyncReply>() };
			return (nnResult)coreinit::IOS_IoctlvAsync(g_state.device, (uint32)id, req->replyCount, req->argCount, req->vectors, onReply, MEMPTR<void>(req));
		}

		// Small request values have to sit in PPC memory too.
		template<typename T>
		struct Arg
		{
			StackAllocator<T> mem;
			explicit Arg(T value) { *mem.GetPointer() = value; }
			Buffer buf() { return { mem.GetPointer(), (uint32)sizeof(T) }; }
		};

		// ---- notifications ---------------------------------------------------

		void QueueNotificationQuery();

		void OnNotifications(Request*)
		{
			coreinit::OSLockMutex(g_apiMutex.GetPtr());
			g_state.notifyPending = false;
			const uint32 count = std::min<uint32>(*g_notificationCount.GetPtr(), kMaxNotifications);
			std::vector<fpd::FPDNotification> batch(g_notifications.GetPtr(), g_notifications.GetPtr() + count);
			const MEMPTR<void> handler = g_state.notifyHandler;
			const MEMPTR<void> handlerArg = g_state.notifyHandlerArg;
			coreinit::OSUnlockMutex(g_apiMutex.GetPtr());

			// the handler may call back into nn_fp, so don't hold the lock
			if (handler)
			{
				for (const auto& n : batch)
					PPCCoreCallback(handler.GetMPTR(), (uint32)n.type, (uint32)n.pid, handlerArg);
			}

			coreinit::OSLockMutex(g_apiMutex.GetPtr());
			QueueNotificationQuery();
			coreinit::OSUnlockMutex(g_apiMutex.GetPtr());
		}

		// Keeps exactly one "wait for notifications" request outstanding.
		void QueueNotificationQuery()
		{
			if (g_state.notifyPending || g_state.initCount == 0)
				return;
			g_state.notifyPending = true;
			*g_notificationCount.GetPtr() = 0;
			CallAsync(RequestId::GetNotificationAsync,
					  { { g_notifications.GetPtr(), (uint32)g_notifications.GetByteSize() }, { g_notificationCount.GetPtr(), (uint32)sizeof(uint32be) } },
					  {}, nullptr, nullptr, OnNotifications);
		}

		// ---- exports ---------------------------------------------------------

#define FP_REQUIRE_INIT(failValue) \
		if (g_state.initCount == 0) \
			return failValue; \
		ApiLock apiLock

		nnResult Initialize()
		{
			ApiLock lock;
			if (g_state.initCount++ == 0)
				g_state.device = coreinit::IOS_Open("/dev/fpd", 0);
			return 0;
		}

		nnResult InitializeAdmin()
		{
			ApiLock lock;
			g_state.admin = true;
			return Initialize();
		}

		uint32 IsInitialized()
		{
			ApiLock lock;
			return g_state.initCount != 0 ? 1 : 0;
		}

		uint32 IsInitializedAdmin()
		{
			return IsInitialized();
		}

		nnResult Finalize()
		{
			ApiLock lock;
			if (g_state.initCount == 1)
			{
				g_state.initCount = 0;
				g_state.admin = false;
				g_state.notifyPending = false;
				coreinit::IOS_Close(g_state.device);
			}
			else if (g_state.initCount > 1)
			{
				g_state.initCount--;
			}
			return 0;
		}

		nnResult FinalizeAdmin()
		{
			return Finalize();
		}

		nnResult SetNotificationHandler(uint32 mask, void* handler, void* handlerArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			g_state.notifyHandler = handler;
			g_state.notifyHandlerArg = handlerArg;
			Arg<uint32be> maskArg(mask);
			const nnResult r = Call(RequestId::SetNotificationMask, {}, { maskArg.buf() });
			if (NN_RESULT_IS_SUCCESS(r))
				QueueNotificationQuery();
			return r;
		}

		nnResult LoginAsync(void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return CallAsync(RequestId::LoginAsync, {}, {}, callback, callbackArg);
		}

		// Returns 0/1 from a request that answers with one uint32.
		uint32 QueryFlag(RequestId id)
		{
			StackAllocator<uint32be> answer;
			*answer.GetPointer() = 0;
			Call(id, { { answer.GetPointer(), (uint32)sizeof(uint32be) } });
			return *answer.GetPointer() != 0 ? 1 : 0;
		}

		// Monster Hunter 3 Ultimate needs this to be false until LoginAsync ran,
		// which /dev/fpd takes care of.
		uint32 HasLoggedIn()
		{
			FP_REQUIRE_INIT(0);
			return QueryFlag(RequestId::HasLoggedIn);
		}

		uint32 IsOnline()
		{
			FP_REQUIRE_INIT(0);
			return QueryFlag(RequestId::IsOnline);
		}

		uint32 IsPreferenceValid()
		{
			FP_REQUIRE_INIT(0);
			return QueryFlag(RequestId::IsPreferenceValid);
		}

		nnResult PidListRequest(RequestId id, uint32be* pids, uint32be* countOut, uint32 start, uint32 max)
		{
			Arg<uint32be> startArg(start);
			Arg<uint32be> maxArg(max);
			return Call(id, { { pids, (uint32)sizeof(uint32be) * max }, { countOut, (uint32)sizeof(uint32be) } }, { startArg.buf(), maxArg.buf() });
		}

		nnResult GetFriendList(uint32be* pids, uint32be* countOut, uint32 start, uint32 max)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return PidListRequest(RequestId::GetFriendList, pids, countOut, start, max);
		}

		nnResult GetFriendRequestList(uint32be* pids, uint32be* countOut, uint32 start, uint32 max)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return PidListRequest(RequestId::GetFriendRequestList, pids, countOut, start, max);
		}

		nnResult GetFriendListAll(uint32be* pids, uint32be* countOut, uint32 start, uint32 max)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return PidListRequest(RequestId::GetFriendListAll, pids, countOut, start, max);
		}

		// Requests of the form (per-friend reply array, pid list, count).
		nnResult PerFriendRequest(RequestId id, void* replies, uint32 replySize, uint32be* pids, uint32 count)
		{
			Arg<uint32be> countArg(count);
			return Call(id, { { replies, replySize * count } }, { { pids, (uint32)sizeof(uint32be) * count }, countArg.buf() });
		}

		nnResult GetFriendListEx(fpd::FriendData* data, uint32be* pids, uint32 count)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return PerFriendRequest(RequestId::GetFriendListEx, data, sizeof(fpd::FriendData), pids, count);
		}

		nnResult GetFriendRequestListEx(fpd::FriendRequest* data, uint32be* pids, uint32 count)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return PerFriendRequest(RequestId::GetFriendRequestListEx, data, sizeof(fpd::FriendRequest), pids, count);
		}

		nnResult GetFriendAccountId(uint8be* ids, uint32be* pids, uint32 count)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			if (count == 0)
				return 0;
			return PerFriendRequest(RequestId::GetFriendAccountId, ids, ACT_ACCOUNTID_LENGTH, pids, count);
		}

		nnResult GetFriendMii(FFLData_t* miis, uint32be* pids, uint32 count)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			if (count == 0)
				return 0;
			return PerFriendRequest(RequestId::GetFriendMii, miis, sizeof(FFLData_t), pids, count);
		}

		nnResult GetFriendPresence(fpd::FriendPresence* presence, uint32be* pids, uint32 count)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			if (count == 0)
				return 0;
			return PerFriendRequest(RequestId::GetFriendPresence, presence, sizeof(fpd::FriendPresence), pids, count);
		}

		nnResult GetFriendRelationship(uint8* relationships, uint32be* pids, uint32 count)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			if (count == 0)
				return 0;
			return PerFriendRequest(RequestId::GetFriendRelationship, relationships, 1, pids, count);
		}

		nnResult GetFriendScreenName(uint16be* names, uint32be* pids, uint32 count, uint8 asciiOnly, uint8be* languages)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			if (count == 0)
				return 0;
			Arg<uint32be> countArg(count);
			Arg<uint8be> asciiArg(asciiOnly);
			const uint32 nameBytes = ACT_NICKNAME_SIZE * (uint32)sizeof(uint16be) * count;
			return Call(RequestId::GetFriendScreenName,
						{ { names, nameBytes }, { languages, languages ? count : 0u } },
						{ { pids, (uint32)sizeof(uint32be) * count }, countArg.buf(), asciiArg.buf() });
		}

		nnResult GetBasicInfoAsync(fpd::FriendBasicInfo* info, uint32be* pids, uint32 count, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			Arg<uint32be> countArg(count);
			return CallAsync(RequestId::GetBasicInfoAsync, { { info, (uint32)sizeof(fpd::FriendBasicInfo) * count } },
							 { { pids, (uint32)sizeof(uint32be) * count }, countArg.buf() }, callback, callbackArg);
		}

		uint32 GetMyPrincipalId()
		{
			FP_REQUIRE_INIT(0);
			StackAllocator<uint32be> pid;
			*pid.GetPointer() = 0;
			Call(RequestId::GetMyPrincipalId, { { pid.GetPointer(), (uint32)sizeof(uint32be) } });
			return *pid.GetPointer();
		}

		nnResult GetMyAccountId(uint8be* accountId)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return Call(RequestId::GetMyAccountId, { { accountId, ACT_ACCOUNTID_LENGTH } });
		}

		nnResult GetMyScreenName(uint16be* name)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return Call(RequestId::GetMyScreenName, { { name, ACT_NICKNAME_SIZE * (uint32)sizeof(uint16be) } });
		}

		nnResult GetMyMii(FFLData_t* mii)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return Call(RequestId::GetMyMii, { { mii, (uint32)sizeof(FFLData_t) } });
		}

		nnResult GetMyPlayingGame(fpd::GameKey* game)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return Call(RequestId::GetMyPlayingGame, { { game, (uint32)sizeof(fpd::GameKey) } });
		}

		nnResult GetMyPreference(fpd::FPDPreference* preference)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return Call(RequestId::GetMyPreference, { { preference, (uint32)sizeof(fpd::FPDPreference) } });
		}

		nnResult GetMyComment(uint16be* comment)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return Call(RequestId::GetMyComment, { { comment, (uint32)sizeof(fpd::Comment) } });
		}

		// Can the local user join the game session advertised in a friend's presence?
		uint32 IsJoinable(fpd::FriendPresence* presence, uint64 allowedModes)
		{
			const fpd::GameMode& mode = presence->gameMode;
			if (!presence->isValid || !presence->isOnline)
				return 0;
			if (mode.joinGameId == 0 || mode.matchmakeType == 0 || mode.groupId == 0)
				return 0;
			const uint32 gameMode = mode.joinGameMode;
			if (gameMode >= 64 || ((allowedModes >> gameMode) & 1) == 0)
				return 0;
			switch ((uint32)mode.joinFlagMask)
			{
			case 1: // anyone
				return 1;
			case 2: // friends of the host only
			{
				StackAllocator<uint32be> hostPid;
				*hostPid.GetPointer() = mode.hostPid;
				uint8 relationship = 0;
				GetFriendRelationship(&relationship, hostPid.GetPointer(), 1);
				return relationship == fpd::RELATIONSHIP_FRIEND ? 1 : 0;
			}
			case 0x65:
			case 0x66:
				cemuLog_log(LogType::Force, "nn_fp: joining by invitation is not supported");
				return 0;
			default:
				return 0;
			}
		}

		nnResult CheckSettingStatusAsync(uint8* status, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return CallAsync(RequestId::CheckSettingStatusAsync, { { status, 1 } }, {}, callback, callbackArg);
		}

		nnResult UpdateCommentAsync(uint16be* comment, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			const uint32 len = CafeStringHelpers::Length(comment, fpd::MY_COMMENT_LENGTH);
			if (len >= (uint32)fpd::MY_COMMENT_LENGTH)
			{
				cemuLog_log(LogType::Force, "nn_fp: UpdateCommentAsync comment is too long");
				return kResultBadArgument;
			}
			return CallAsync(RequestId::UpdateCommentAsync, {}, { { comment, (len + 1) * (uint32)sizeof(uint16be) } }, callback, callbackArg);
		}

		nnResult UpdatePreferenceAsync(fpd::FPDPreference* preference, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return CallAsync(RequestId::UpdatePreferenceAsync, {}, { { preference, (uint32)sizeof(fpd::FPDPreference) } }, callback, callbackArg);
		}

		// The three-argument overload ignores its last argument; both go to the same handler.
		nnResult UpdateGameModeEx(fpd::GameMode* mode, uint16be* message, uint32 unused)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			const uint32 len = CafeStringHelpers::Length(message, fpd::GAMEMODE_MAX_MESSAGE_LENGTH);
			if (len >= (uint32)fpd::GAMEMODE_MAX_MESSAGE_LENGTH)
			{
				cemuLog_log(LogType::Force, "nn_fp: UpdateGameMode message is too long");
				return kResultBadArgument;
			}
			return Call(RequestId::UpdateGameModeVariation2, {},
						{ { mode, (uint32)sizeof(fpd::GameMode) }, { message, (len + 1) * (uint32)sizeof(uint16be) } });
		}

		nnResult UpdateGameMode(fpd::GameMode* mode, uint16be* message)
		{
			return UpdateGameModeEx(mode, message, 0);
		}

		nnResult GetRequestBlockSettingAsync(uint8* settings, uint32be* pids, uint32 count, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			Arg<uint32be> countArg(count);
			return CallAsync(RequestId::GetRequestBlockSettingAsync, { { settings, count } },
							 { { pids, (uint32)sizeof(uint32be) * count }, countArg.buf() }, callback, callbackArg);
		}

		nnResult AddFriendAsync(uint32 pid, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			Arg<uint32be> pidArg(pid);
			return CallAsync(RequestId::AddFriendAsyncByPid, {}, { pidArg.buf() }, callback, callbackArg);
		}

		nnResult RemoveFriendAsync(uint32 pid, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			Arg<uint32be> pidArg(pid);
			return CallAsync(RequestId::RemoveFriendAsync, {}, { pidArg.buf() }, callback, callbackArg);
		}

		nnResult DeleteFriendFlagsAsync(uint32be* pids, uint32 count, uint32 flags, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			Arg<uint32be> countArg(count);
			Arg<uint32be> flagsArg(flags);
			return CallAsync(RequestId::DeleteFriendFlagsAsync, {},
							 { { pids, (uint32)sizeof(uint32be) * count }, countArg.buf(), flagsArg.buf() }, callback, callbackArg);
		}

		nnResult AddFriendRequestAsync(fpd::RecentPlayRecordEx* record, uint16be* message, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			uint32 len = 0;
			while (message[len] != 0)
				len++;
			return CallAsync(RequestId::AddFriendRequestByPlayRecordAsync, {},
							 { { record, (uint32)sizeof(fpd::RecentPlayRecordEx) }, { message, (len + 1) * (uint32)sizeof(uint16be) } },
							 callback, callbackArg);
		}

		nnResult MarkFriendRequestsAsReceivedAsync(uint64be* messageIds, uint32 count, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			Arg<uint32be> countArg(count);
			return CallAsync(RequestId::MarkFriendRequestsAsReceivedAsync, {},
							 { { messageIds, (uint32)sizeof(uint64be) * count }, countArg.buf() }, callback, callbackArg);
		}

		nnResult FriendRequestByIdAsync(RequestId id, uint64 requestId, void* callback, void* callbackArg)
		{
			Arg<uint64be> idArg(requestId);
			return CallAsync(id, {}, { idArg.buf() }, callback, callbackArg);
		}

		nnResult CancelFriendRequestAsync(uint64 requestId, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return FriendRequestByIdAsync(RequestId::CancelFriendRequestAsync, requestId, callback, callbackArg);
		}

		nnResult DeleteFriendRequestAsync(uint64 requestId, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return FriendRequestByIdAsync(RequestId::DeleteFriendRequestAsync, requestId, callback, callbackArg);
		}

		nnResult AcceptFriendRequestAsync(uint64 requestId, void* callback, void* callbackArg)
		{
			FP_REQUIRE_INIT(kResultNotInitialized);
			return FriendRequestByIdAsync(RequestId::AcceptFriendRequestAsync, requestId, callback, callbackArg);
		}

#undef FP_REQUIRE_INIT

		class FpLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_fp"; }

			void RPLMapped() override
			{
				g_state = State{};
				coreinit::OSInitMutex(g_apiMutex.GetPtr());
				{
					std::lock_guard lock(g_heapLock);
					delete g_heap;
					g_heap = new VHeap(g_heapSpace.GetPtr(), g_heapSpace.GetByteSize());
				}

				constexpr auto lt = LogType::NN_FP;
				cafeExportRegisterFunc(Initialize, "nn_fp", "Initialize__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(InitializeAdmin, "nn_fp", "InitializeAdmin__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(IsInitialized, "nn_fp", "IsInitialized__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(IsInitializedAdmin, "nn_fp", "IsInitializedAdmin__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(Finalize, "nn_fp", "Finalize__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(FinalizeAdmin, "nn_fp", "FinalizeAdmin__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(SetNotificationHandler, "nn_fp", "SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv", lt);
				cafeExportRegisterFunc(LoginAsync, "nn_fp", "LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(HasLoggedIn, "nn_fp", "HasLoggedIn__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(IsOnline, "nn_fp", "IsOnline__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(GetFriendList, "nn_fp", "GetFriendList__Q2_2nn2fpFPUiT1UiT3", lt);
				cafeExportRegisterFunc(GetFriendRequestList, "nn_fp", "GetFriendRequestList__Q2_2nn2fpFPUiT1UiT3", lt);
				cafeExportRegisterFunc(GetFriendListAll, "nn_fp", "GetFriendListAll__Q2_2nn2fpFPUiT1UiT3", lt);
				cafeExportRegisterFunc(GetFriendListEx, "nn_fp", "GetFriendListEx__Q2_2nn2fpFPQ3_2nn2fp10FriendDataPCUiUi", lt);
				cafeExportRegisterFunc(GetFriendRequestListEx, "nn_fp", "GetFriendRequestListEx__Q2_2nn2fpFPQ3_2nn2fp13FriendRequestPCUiUi", lt);
				cafeExportRegisterFunc(GetBasicInfoAsync, "nn_fp", "GetBasicInfoAsync__Q2_2nn2fpFPQ3_2nn2fp9BasicInfoPCUiUiPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(GetMyPrincipalId, "nn_fp", "GetMyPrincipalId__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(GetMyAccountId, "nn_fp", "GetMyAccountId__Q2_2nn2fpFPc", lt);
				cafeExportRegisterFunc(GetMyScreenName, "nn_fp", "GetMyScreenName__Q2_2nn2fpFPw", lt);
				cafeExportRegisterFunc(GetMyMii, "nn_fp", "GetMyMii__Q2_2nn2fpFP12FFLStoreData", lt);
				cafeExportRegisterFunc(GetMyPlayingGame, "nn_fp", "GetMyPlayingGame__Q2_2nn2fpFPQ3_2nn2fp7GameKey", lt);
				cafeExportRegisterFunc(GetMyPreference, "nn_fp", "GetMyPreference__Q2_2nn2fpFPQ3_2nn2fp10Preference", lt);
				cafeExportRegisterFunc(GetMyComment, "nn_fp", "GetMyComment__Q2_2nn2fpFPQ3_2nn2fp7Comment", lt);
				cafeExportRegisterFunc(GetFriendAccountId, "nn_fp", "GetFriendAccountId__Q2_2nn2fpFPA17_cPCUiUi", lt);
				cafeExportRegisterFunc(GetFriendScreenName, "nn_fp", "GetFriendScreenName__Q2_2nn2fpFPA11_wPCUiUibPUc", lt);
				cafeExportRegisterFunc(GetFriendMii, "nn_fp", "GetFriendMii__Q2_2nn2fpFP12FFLStoreDataPCUiUi", lt);
				cafeExportRegisterFunc(GetFriendPresence, "nn_fp", "GetFriendPresence__Q2_2nn2fpFPQ3_2nn2fp14FriendPresencePCUiUi", lt);
				cafeExportRegisterFunc(GetFriendRelationship, "nn_fp", "GetFriendRelationship__Q2_2nn2fpFPUcPCUiUi", lt);
				cafeExportRegisterFunc(IsJoinable, "nn_fp", "IsJoinable__Q2_2nn2fpFPCQ3_2nn2fp14FriendPresenceUL", lt);
				cafeExportRegisterFunc(CheckSettingStatusAsync, "nn_fp", "CheckSettingStatusAsync__Q2_2nn2fpFPUcPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(IsPreferenceValid, "nn_fp", "IsPreferenceValid__Q2_2nn2fpFv", lt);
				cafeExportRegisterFunc(UpdateCommentAsync, "nn_fp", "UpdateCommentAsync__Q2_2nn2fpFPCwPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(UpdatePreferenceAsync, "nn_fp", "UpdatePreferenceAsync__Q2_2nn2fpFPCQ3_2nn2fp10PreferencePFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(GetRequestBlockSettingAsync, "nn_fp", "GetRequestBlockSettingAsync__Q2_2nn2fpFPUcPCUiUiPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(UpdateGameModeEx, "nn_fp", "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCwUi", lt);
				cafeExportRegisterFunc(UpdateGameMode, "nn_fp", "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw", lt);
				cafeExportRegisterFunc(AddFriendAsync, "nn_fp", "AddFriendAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(AddFriendRequestAsync, "nn_fp", "AddFriendRequestAsync__Q2_2nn2fpFPCQ3_2nn2fp18RecentPlayRecordExPCwPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(DeleteFriendFlagsAsync, "nn_fp", "DeleteFriendFlagsAsync__Q2_2nn2fpFPCUiUiT2PFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(RemoveFriendAsync, "nn_fp", "RemoveFriendAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(MarkFriendRequestsAsReceivedAsync, "nn_fp", "MarkFriendRequestsAsReceivedAsync__Q2_2nn2fpFPCULUiPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(CancelFriendRequestAsync, "nn_fp", "CancelFriendRequestAsync__Q2_2nn2fpFULPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(DeleteFriendRequestAsync, "nn_fp", "DeleteFriendRequestAsync__Q2_2nn2fpFULPFQ2_2nn6ResultPv_vPv", lt);
				cafeExportRegisterFunc(AcceptFriendRequestAsync, "nn_fp", "AcceptFriendRequestAsync__Q2_2nn2fpFULPFQ2_2nn6ResultPv_vPv", lt);
			}
		};

		FpLibrary g_fpLibrary;
	}

	COSModule* GetModule()
	{
		return &g_fpLibrary;
	}
}
