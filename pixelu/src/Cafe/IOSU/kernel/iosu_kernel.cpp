// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Minimal IOSU kernel: message queues, timers, resource manager registration
// and routing of PPC IPC requests (open/close/ioctl/ioctlv) to the services
// that registered a device path.
#include "iosu_kernel.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

namespace iosu
{
	namespace kernel
	{
		namespace
		{
			// handles carry a slot index in the low 12 bits and a generation above it
			constexpr uint32 kSlotMask = 0xFFF;

			uint32 ComposeHandle(size_t slot, uint32 generation)
			{
				return (uint32)slot | ((generation << 12) & 0x7FFFFFFF);
			}

			// guards message queues, resource managers and device handles
			std::mutex g_kernelLock;

			/* ---- message queues ---- */

			struct MessageQueue
			{
				IOSMsgQueueId id = 0;
				uint32 capacity = 0; // zero while the slot is unused
				uint32 head = 0;
				uint32 count = 0;
				betype<IOSMessage>* ring = nullptr; // storage supplied by the creator
				std::condition_variable spaceAvailable;
				std::condition_variable messageAvailable;
			};

			constexpr size_t kQueueSlots = 750;
			std::array<MessageQueue, kQueueSlots> g_queues;
			uint32 g_queueGeneration = 0;

			// g_kernelLock must be held
			MessageQueue* LookupQueue(IOSMsgQueueId id)
			{
				const uint32 slot = id & kSlotMask;
				if (slot >= g_queues.size())
					return nullptr;
				MessageQueue& q = g_queues[slot];
				return q.id == id ? &q : nullptr;
			}

			/* ---- resource managers (registered device paths) ---- */

			struct DeviceRegistration
			{
				bool used = false;
				std::string path;
				IOSMsgQueueId queue = 0;
			};

			std::array<DeviceRegistration, 512> g_devices;

			// g_kernelLock must be held
			DeviceRegistration* LookupDevice(std::string_view path)
			{
				for (DeviceRegistration& d : g_devices)
					if (d.used && d.path == path)
						return &d;
				return nullptr;
			}

			/* ---- open device handles ---- */

			struct OpenHandle
			{
				bool used = false;
				uint32 handle = 0;
				std::string path;
				IOSMsgQueueId queue = 0;
				bool hasServiceHandle = false;
				IOSDevHandle serviceHandle = 0; // handle the service returned for IOS_OPEN
			};

			constexpr size_t kMaxOpenHandles = 96;
			OpenHandle g_handles[kMaxOpenHandles];
			uint32 g_handleGeneration = 1;

			// g_kernelLock must be held
			void DropHandlesOfQueue(IOSMsgQueueId queue)
			{
				for (OpenHandle& h : g_handles)
				{
					if (h.used && h.queue == queue)
						h = OpenHandle{};
				}
			}

			// g_kernelLock must be held
			void DropDevicesOfQueue(IOSMsgQueueId queue)
			{
				DropHandlesOfQueue(queue);
				for (DeviceRegistration& d : g_devices)
				{
					if (d.used && d.queue == queue)
						d = DeviceRegistration{};
				}
			}

			// g_kernelLock must be held. Returns the slot or nullptr after logging
			OpenHandle* ValidateHandle(IOSDevHandle handle)
			{
				const uint32 slot = handle & kSlotMask;
				cemu_assert(slot < kMaxOpenHandles);
				OpenHandle& h = g_handles[slot];
				if (!h.used)
				{
					cemuLog_log(LogType::Force, "IOSU-Kernel: device handle {:08x} is no longer open", handle);
					return nullptr;
				}
				if (h.handle != handle)
				{
					cemuLog_log(LogType::Force, "IOSU-Kernel: stale device handle {:08x}", handle);
					return nullptr;
				}
				return &h;
			}
		}

		IOSMsgQueueId IOS_CreateMessageQueue(IOSMessage* messageArray, uint32 messageCount)
		{
			std::lock_guard lock(g_kernelLock);
			cemu_assert(messageCount != 0);
			for (size_t slot = 0; slot < g_queues.size(); slot++)
			{
				MessageQueue& q = g_queues[slot];
				if (q.capacity != 0)
					continue;
				q.id = ComposeHandle(slot, g_queueGeneration++);
				q.capacity = messageCount;
				q.ring = (betype<IOSMessage>*)messageArray;
				q.head = 0;
				q.count = 0;
				return q.id;
			}
			cemu_assert_suspicious();
			return (IOSMsgQueueId)IOS_ERROR_MAXIMUM_REACHED;
		}

		IOS_ERROR IOS_DestroyMessageQueue(IOSMsgQueueId msgQueueId)
		{
			std::lock_guard lock(g_kernelLock);
			MessageQueue* q = LookupQueue(msgQueueId);
			if (!q)
				return IOS_ERROR_INVALID;
			q->capacity = 0;
			q->id = 0;
			// anyone still blocked on this queue sees the id change and fails
			q->spaceAvailable.notify_all();
			q->messageAvailable.notify_all();
			DropDevicesOfQueue(msgQueueId);
			return IOS_ERROR_OK;
		}

		IOS_ERROR IOS_SendMessage(IOSMsgQueueId msgQueueId, IOSMessage message, uint32 flags)
		{
			std::unique_lock lock(g_kernelLock);
			cemu_assert_debug(flags <= 1);
			const bool nonBlocking = (flags & 1) != 0;
			MessageQueue* q = LookupQueue(msgQueueId);
			if (!q)
				return IOS_ERROR_INVALID;
			while (q->count == q->capacity)
			{
				if (nonBlocking)
					return IOS_ERROR_WOULD_BLOCK;
				q->spaceAvailable.wait(lock);
				if (q->id != msgQueueId)
					return IOS_ERROR_INVALID;
			}
			uint32 tail = q->head + q->count;
			if (tail >= q->capacity)
				tail -= q->capacity;
			q->ring[tail] = message;
			q->count++;
			q->messageAvailable.notify_one();
			return IOS_ERROR_OK;
		}

		IOS_ERROR IOS_ReceiveMessage(IOSMsgQueueId msgQueueId, IOSMessage* messageOut, uint32 flags)
		{
			std::unique_lock lock(g_kernelLock);
			cemu_assert_debug(flags <= 1);
			const bool nonBlocking = (flags & 1) != 0;
			MessageQueue* q = LookupQueue(msgQueueId);
			if (!q)
				return IOS_ERROR_INVALID;
			while (q->count == 0)
			{
				if (nonBlocking)
					return IOS_ERROR_NONE_AVAILABLE;
				q->messageAvailable.wait(lock);
				if (q->id != msgQueueId)
					return IOS_ERROR_INVALID;
			}
			*messageOut = q->ring[q->head];
			q->head++;
			if (q->head >= q->capacity)
				q->head -= q->capacity;
			q->count--;
			q->spaceAvailable.notify_one();
			return IOS_ERROR_OK;
		}

		/* ---- timers ---- */

		namespace
		{
			struct Timer
			{
				bool valid = false;
				IOSMsgQueueId queue = 0;
				uint32 message = 0;
				HRTick due = 0; // 0 = not armed
				HRTick period = 0;
			};

			std::mutex g_timerLock;
			std::condition_variable g_timerWake;
			std::atomic_bool g_timerShutdown{ false };
			std::vector<Timer> g_timers;
			std::vector<IOSTimerId> g_recycledTimers;
			std::set<std::pair<HRTick, IOSTimerId>> g_armedTimers; // ordered by due time, then id

			// g_timerLock must be held
			void Rearm(IOSTimerId id, HRTick due)
			{
				Timer& t = g_timers[id];
				if (t.due != 0)
					g_armedTimers.erase({ t.due, id });
				t.due = due;
				if (due != 0)
					g_armedTimers.insert({ due, id });
			}

			void TimerThreadMain()
			{
				SetThreadName("IOS-Timer");
				std::unique_lock lock(g_timerLock);
				while (!g_timerShutdown)
				{
					if (g_armedTimers.empty())
					{
						g_timerWake.wait_for(lock, std::chrono::seconds(10));
						continue;
					}
					const auto [due, id] = *g_armedTimers.begin();
					const HRTick now = HighResolutionTimer::now().getTick();
					if (now < due)
					{
						g_timerWake.wait_for(lock, std::chrono::microseconds(HighResolutionTimer::ticksToMicroseconds(due - now)));
						continue;
					}
					const Timer& t = g_timers[id];
					Rearm(id, t.period != 0 ? due + t.period : 0);
					const IOSMsgQueueId queue = t.queue;
					const uint32 message = t.message;
					lock.unlock();
					IOS_SendMessage(queue, message, 1);
					lock.lock();
				}
			}
		}

		IOS_ERROR IOS_CreateTimer(uint32 startMicroseconds, uint32 repeatMicroseconds, uint32 queueId, uint32 message)
		{
			std::lock_guard lock(g_timerLock);
			IOSTimerId id;
			if (!g_recycledTimers.empty())
			{
				id = g_recycledTimers.back();
				g_recycledTimers.pop_back();
			}
			else
			{
				id = (IOSTimerId)g_timers.size();
				g_timers.emplace_back();
			}
			Timer& t = g_timers[id];
			t.queue = queueId;
			t.message = message;
			t.period = HighResolutionTimer::microsecondsToTicks(repeatMicroseconds);
			Rearm(id, HighResolutionTimer::now().getTick() + HighResolutionTimer::microsecondsToTicks(startMicroseconds));
			t.valid = true;
			g_timerWake.notify_one();
			return (IOS_ERROR)id;
		}

		IOS_ERROR IOS_StopTimer(IOSTimerId timerId)
		{
			std::lock_guard lock(g_timerLock);
			if (timerId >= g_timers.size() || !g_timers[timerId].valid)
				return IOS_ERROR_INVALID;
			Rearm(timerId, 0);
			return IOS_ERROR_OK;
		}

		IOS_ERROR IOS_DestroyTimer(IOSTimerId timerId)
		{
			std::lock_guard lock(g_timerLock);
			if (timerId >= g_timers.size() || !g_timers[timerId].valid)
				return IOS_ERROR_INVALID;
			Rearm(timerId, 0);
			g_timers[timerId].valid = false;
			g_recycledTimers.push_back(timerId);
			return IOS_ERROR_OK;
		}

		/* ---- resource managers ---- */

		IOS_ERROR IOS_RegisterResourceManager(const char* devicePath, IOSMsgQueueId msgQueueId)
		{
			std::lock_guard lock(g_kernelLock);
			if (LookupDevice(devicePath))
			{
				cemu_assert_suspicious();
				return IOS_ERROR_INVALID;
			}
			if (!LookupQueue(msgQueueId))
				return IOS_ERROR_INVALID;
			for (DeviceRegistration& d : g_devices)
			{
				if (d.used)
					continue;
				d.used = true;
				d.path = devicePath;
				d.queue = msgQueueId;
				return IOS_ERROR_OK;
			}
			return IOS_ERROR_MAXIMUM_REACHED;
		}

		IOS_ERROR IOS_DeviceAssociateId(const char* devicePath, uint32 id)
		{
			return IOS_ERROR_OK; // no id association is tracked
		}

		/* ---- IPC routing ---- */

		namespace
		{
			// Kernel side copy of a request. 'request' must stay the first member:
			// services receive a pointer to it and hand the same pointer back to
			// IOS_ResourceReply.
			struct InFlightRequest
			{
				IPCCommandBody request;
				IPCIoctlVector vectorsCopy[8];
				IPCCommandBody* sender; // the PPC side command block
				uint32 coreIndex;
				IOSDevHandle clientHandle; // the kernel handle the caller knows
				bool inUse{ false };
			};

			SysAllocator<InFlightRequest, 96> g_requestPool;
			std::queue<InFlightRequest*> g_idleRequests;
			FSpinlock g_requestPoolLock;
			std::mutex g_replyLock[3];

			bool IsPoolEntry(const InFlightRequest* r)
			{
				return r >= g_requestPool.GetPtr() && r < g_requestPool.GetPtr() + g_requestPool.GetCount();
			}

			void ResetRequestPool()
			{
				g_requestPoolLock.lock();
				g_idleRequests = {};
				for (size_t i = 0; i < g_requestPool.GetCount(); i++)
					g_idleRequests.push(g_requestPool.GetPtr() + i);
				g_requestPoolLock.unlock();
			}

			InFlightRequest* TakeRequest()
			{
				g_requestPoolLock.lock();
				InFlightRequest* r = nullptr;
				if (!g_idleRequests.empty())
				{
					r = g_idleRequests.front();
					g_idleRequests.pop();
					cemu_assert_debug(!r->inUse);
					r->inUse = true;
				}
				g_requestPoolLock.unlock();
				if (!r)
					cemuLog_log(LogType::Force, "IOSU-Kernel: no free IPC request slots");
				return r;
			}

			void ReturnRequest(InFlightRequest* r)
			{
				g_requestPoolLock.lock();
				cemu_assert_debug(r->inUse);
				r->inUse = false;
				g_idleRequests.push(r);
				g_requestPoolLock.unlock();
			}

			// writes the result into the PPC command block, hands it back to COS and frees the slot
			void Complete(InFlightRequest* r, uint32 result)
			{
				cemu_assert(r->coreIndex < 3);
				std::lock_guard lock(g_replyLock[r->coreIndex]);
				cemu_assert(IsPoolEntry(r));
				r->sender->result = result;
				IPCCommandBody* done[1] = { r->sender };
				coreinit::IPCDriver_NotifyResponses(r->coreIndex, done, 1);
				ReturnRequest(r);
			}

			IOS_ERROR OpenHandleForPath(const std::string& path, IOSDevHandle& handleOut)
			{
				std::lock_guard lock(g_kernelLock);
				const DeviceRegistration* dev = LookupDevice(path);
				if (!dev)
				{
					cemuLog_log(LogType::Force, "IOSU-Kernel: IOS_Open() could not open {}", path);
					return IOS_ERROR_INVALID;
				}
				for (size_t slot = 0; slot < kMaxOpenHandles; slot++)
				{
					OpenHandle& h = g_handles[slot];
					if (h.used)
						continue;
					h.used = true;
					h.handle = ComposeHandle(slot, g_handleGeneration++);
					h.path = path;
					h.queue = dev->queue;
					h.hasServiceHandle = false;
					handleOut = h.handle;
					return IOS_ERROR_OK;
				}
				cemu_assert_debug(false);
				return IOS_ERROR_MAXIMUM_REACHED;
			}

			IOS_ERROR CloseHandle(IOSDevHandle handle)
			{
				std::lock_guard lock(g_kernelLock);
				OpenHandle* h = ValidateHandle(handle);
				if (!h)
					return IOS_ERROR_INVALID;
				h->used = false;
				h->handle = 0;
				h->hasServiceHandle = false;
				return IOS_ERROR_OK;
			}

			IOS_ERROR BindServiceHandle(IOSDevHandle handle, IOSDevHandle serviceHandle)
			{
				std::lock_guard lock(g_kernelLock);
				OpenHandle* h = ValidateHandle(handle);
				if (!h)
					return IOS_ERROR_INVALID;
				cemu_assert_debug(!h->hasServiceHandle);
				h->hasServiceHandle = true;
				h->serviceHandle = serviceHandle;
				return IOS_ERROR_OK;
			}

			// translates the handle to the service's own handle and queues the request
			IOS_ERROR Forward(IOSDevHandle handle, InFlightRequest* r)
			{
				IOSMsgQueueId queue;
				{
					std::lock_guard lock(g_kernelLock);
					OpenHandle* h = ValidateHandle(handle);
					if (!h)
						return IOS_ERROR_INVALID;
					queue = h->queue;
					if (r->request.cmdId == IPCCommandId::IOS_OPEN)
					{
						cemu_assert(!h->hasServiceHandle);
						r->request.devHandle = 0;
					}
					else
					{
						cemu_assert(h->hasServiceHandle);
						r->request.devHandle = h->serviceHandle;
					}
				}
				const IOS_ERROR err = IOS_SendMessage(queue, MEMPTR<InFlightRequest>(r).GetMPTR(), 1);
				if (err != IOS_ERROR_OK)
					cemuLog_log(LogType::Force, "IOSU-Kernel: could not queue IPC request ({})", (sint32)err);
				return err;
			}

			IOS_ERROR ForwardOpen(InFlightRequest* r)
			{
				const char* name = MEMPTR<const char>(r->request.args[0]).GetPtr();
				const uint32 nameSize = r->request.args[1]; // includes the terminator
				cemu_assert(nameSize > 0);
				cemu_assert_debug(r->request.args[2] == 0); // open mode
				IOSDevHandle handle;
				IOS_ERROR err = OpenHandleForPath(std::string(name, nameSize - 1), handle);
				if (err != IOS_ERROR_OK)
					return err;
				r->clientHandle = handle;
				r->request.devHandle = 0;
				return Forward(handle, r);
			}

			// COS normally fills in the physical addresses. Vectors stay in guest memory
			// and the service reads them from there.
			IOS_ERROR PrepareVectors(InFlightRequest* r)
			{
				const uint32 count = r->request.args[1] + r->request.args[2];
				IPCIoctlVector* vectors = MEMPTR<IPCIoctlVector>(r->request.args[3]).GetPtr();
				for (uint32 i = 0; i < count; i++)
				{
					if (vectors[i].baseVirt == nullptr && vectors[i].size != 0)
					{
						cemuLog_logDebug(LogType::Force, "IPC Ioctlv rejected: vector {} has a size but no address", i);
						return IOS_ERROR_INVALID;
					}
					vectors[i].basePhys = vectors[i].baseVirt;
				}
				if (count <= 8)
					std::copy(vectors, vectors + count, r->vectorsCopy);
				else
					cemuLog_log(LogType::Force, "Info: Ioctlv command with more than 8 vectors");
				return IOS_ERROR_OK;
			}
		}

		void IPCSubmitFromCOS(uint32 ppcCoreIndex, IPCCommandBody* cmd)
		{
			InFlightRequest* r = TakeRequest();
			cemu_assert(r != nullptr);
			r->request = *cmd;
			r->sender = cmd;
			r->coreIndex = ppcCoreIndex;
			r->clientHandle = cmd->devHandle;
			IOS_ERROR err;
			switch ((IPCCommandId)cmd->cmdId)
			{
			case IPCCommandId::IOS_OPEN:
				r->clientHandle = 0;
				err = ForwardOpen(r);
				break;
			case IPCCommandId::IOS_CLOSE:
			case IPCCommandId::IOS_IOCTL:
				err = Forward(r->request.devHandle, r);
				break;
			case IPCCommandId::IOS_IOCTLV:
				err = PrepareVectors(r);
				if (err < 0)
					cemuLog_log(LogType::Force, "Ioctlv error");
				else
					err = Forward(r->request.devHandle, r);
				break;
			default:
				cemuLog_log(LogType::Force, "Invalid IPC command {}", (uint32)(IPCCommandId)cmd->cmdId);
				err = IOS_ERROR_INVALID;
				break;
			}
			// on success the service answers later through IOS_ResourceReply
			if (err < 0)
			{
				cemuLog_log(LogType::Force, "Error occurred while trying to dispatch IPC");
				Complete(r, err);
			}
		}

		IOS_ERROR IOS_ResourceReply(IPCCommandBody* cmd, IOS_ERROR result)
		{
			InFlightRequest* r = (InFlightRequest*)cmd;
			cemu_assert(IsPoolEntry(r));
			cemu_assert_debug(r->inUse);
			r->sender->result = result;
			const IPCCommandId kind = r->sender->cmdId;
			if (kind == IPCCommandId::IOS_OPEN)
			{
				if (IOS_ResultIsError(result))
				{
					cemuLog_log(LogType::Force, "IOS_ResourceReply(): service rejected IOS_OPEN");
					cemu_assert(CloseHandle(r->clientHandle) == IOS_ERROR_OK);
				}
				else
				{
					// the caller gets the kernel handle, the service's handle is remembered
					cemu_assert(BindServiceHandle(r->clientHandle, (IOSDevHandle)result) == IOS_ERROR_OK);
					result = (IOS_ERROR)(uint32)r->clientHandle;
				}
			}
			else if (kind == IPCCommandId::IOS_CLOSE)
			{
				if (IOS_ResultIsError(result))
					cemuLog_log(LogType::Force, "IOS_ResourceReply(): service reported an error for IOS_CLOSE");
				const IOSDevHandle handle = r->clientHandle;
				Complete(r, result);
				const IOS_ERROR closeErr = CloseHandle(handle);
				cemu_assert_debug(closeErr == IOS_ERROR_OK);
				return IOS_ERROR_OK;
			}
			Complete(r, result);
			return IOS_ERROR_OK;
		}

		namespace
		{
			class KernelModule : public ::IOSUModule
			{
			public:
				void SystemLaunch() override
				{
					ResetRequestPool();
					g_timerShutdown = false;
					m_timerThread = std::thread(TimerThreadMain);
				}

				void SystemExit() override
				{
					g_timerShutdown = true;
					g_timerWake.notify_one();
					m_timerThread.join();
				}

			private:
				std::thread m_timerThread;
			};

			KernelModule g_kernelModule;
		}

		IOSUModule* GetModule()
		{
			return &g_kernelModule;
		}
	}
}
