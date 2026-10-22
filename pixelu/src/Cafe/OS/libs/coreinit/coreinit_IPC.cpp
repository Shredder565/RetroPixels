// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/HW/Espresso/Const.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "coreinit_MessageQueue.h"
#include "coreinit_IPC.h"

// PPC side of the IOS IPC channel. Each core owns a driver with a fixed set of request slots; a
// slot holds one 0x80 byte request buffer (the command body plus room for an IOS_Open path).
// Requests go straight to the HLE IOSU kernel. Replies come back through IPCDriver_NotifyResponses
// and either wake the synchronous caller, are posted to a message queue, or are handed to the
// core's IPC thread which runs the async callback.

namespace coreinit
{
	namespace
	{
		constexpr uint32 kSlotsPerCore = 0x30;
		constexpr uint32 kMaxRepliesPerNotify = 11;
		constexpr uint32 kMaxDevicePathLength = 31;

		struct IPCRequestBuffer
		{
			IPCCommandBody cmd;
			uint8 payload[0x80 - sizeof(IPCCommandBody)];
		};
		static_assert(sizeof(IPCCommandBody) == 0x48);
		static_assert(sizeof(IPCRequestBuffer) == 0x80);

		struct IPCRequestSlot
		{
			/* +0x00 */ uint32be inUse;
			/* +0x04 */ MEMPTR<OSMessageQueue> replyQueue; // if set, the reply is posted here
			/* +0x08 */ MEMPTR<void> replyCallback;        // otherwise, if set, the core's IPC thread calls this
			/* +0x0C */ MEMPTR<void> replyContext;
			/* +0x10 */ uint32 reserved10;
			/* +0x14 */ MEMPTR<IPCRequestBuffer> buffer;
			/* +0x18 */ OSEvent replyEvent;                 // signalled for synchronous requests
		};
		static_assert(sizeof(IPCRequestSlot) == 0x3C);

		// FIFO of slot pointers
		struct IPCSlotQueue
		{
			sint32be putPos;
			sint32be takePos; // -1 while empty
			sint32be count;
			sint32be peakCount;
			MEMPTR<IPCRequestSlot> entries[kSlotsPerCore];

			void Reset()
			{
				putPos = 0;
				takePos = -1;
				count = 0;
				peakCount = 0;
				for (auto& e : entries)
					e = nullptr;
			}

			void Add(IPCRequestSlot* slot)
			{
				// there are never more slots than entries, so this cannot overflow
				cemu_assert(takePos != putPos);
				sint32 put = putPos;
				entries[put] = slot;
				if (takePos < 0)
					takePos = put;
				putPos = (put + 1) % (sint32)kSlotsPerCore;
				count = count + 1;
				if (count > peakCount)
					peakCount = (sint32)count;
			}

			IPCRequestSlot* Remove()
			{
				if (count == 0)
					return nullptr;
				sint32 take = takePos;
				IPCRequestSlot* slot = entries[take].GetPtr();
				count = count - 1;
				takePos = (count == 0) ? -1 : (take + 1) % (sint32)kSlotsPerCore;
				return slot;
			}
		};
		static_assert(sizeof(IPCSlotQueue) == 0xD0);

		// reply list the COS kernel would fill in before calling back into the driver
		struct IPCReplyMailbox
		{
			uint32be count;
			MEMPTR<IPCCommandBody> replies[kMaxRepliesPerNotify];
		};
		static_assert(sizeof(IPCReplyMailbox) == 0x30);

		struct alignas(64) IPCCoreDriver
		{
			/* +0x000 */ betype<IPCDriverState> state;
			/* +0x004 */ uint32 reserved004;
			/* +0x008 */ uint32 coreIndex;
			/* +0x00C */ uint32 reserved00C;
			/* +0x010 */ MEMPTR<IPCRequestBuffer> buffers;
			/* +0x014 */ IPCSlotQueue freeSlots;
			/* +0x0E4 */ IPCSlotQueue pendingSlots;
			/* +0x1B4 */ uint32 slotsReady;
			/* +0x1B8 */ IPCReplyMailbox mailbox;
			/* +0x1E8 */ IPCRequestSlot slots[kSlotsPerCore];
		};
		static_assert(offsetof(IPCCoreDriver, slots) == 0x1E8);
		static_assert(sizeof(IPCCoreDriver) == 0xD40);

		SysAllocator<IPCRequestBuffer, kSlotsPerCore * Espresso::CORE_COUNT, 0x40> s_requestBuffers;
		SysAllocator<IPCCoreDriver, Espresso::CORE_COUNT, 0x40> s_coreDrivers;

		SysAllocator<OSThread_t, Espresso::CORE_COUNT> s_ipcThreads;
		SysAllocator<uint8, 0x4000 * Espresso::CORE_COUNT> s_ipcThreadStacks;
		SysAllocator<uint8, 0x18 * Espresso::CORE_COUNT> s_ipcThreadNames;
		SysAllocator<OSMessageQueue, Espresso::CORE_COUNT> s_ipcThreadQueues;
		SysAllocator<OSMessage, Espresso::CORE_COUNT * kSlotsPerCore> s_ipcThreadQueueStorage;

		constexpr uint32 kIpcThreadStackSize = 0x4000;
		constexpr uint32 kIpcThreadNameSize = 0x18;

		// message tags used on the IPC thread queue and on caller supplied reply queues
		constexpr uint32 kReplyMessageQueued = 0;
		constexpr uint32 kReplyMessageCallback = 1;

		IPCCoreDriver& DriverOf(uint32 coreIndex)
		{
			cemu_assert_debug(coreIndex < (uint32)Espresso::CORE_COUNT);
			return s_coreDrivers[coreIndex];
		}

		IPCCoreDriver& CurrentDriver()
		{
			return DriverOf(OSGetCoreId());
		}

		void SetUpDriver(uint32 coreIndex)
		{
			IPCCoreDriver& drv = DriverOf(coreIndex);
			drv.coreIndex = coreIndex;
			drv.state = IPCDriverState::INITIALIZED;
			drv.buffers = s_requestBuffers.GetPtr() + kSlotsPerCore * coreIndex;
			drv.slotsReady = 0;
			for (uint32 i = 0; i < kSlotsPerCore; i++)
			{
				IPCRequestSlot& slot = drv.slots[i];
				slot.buffer = drv.buffers.GetPtr() + i;
				slot.replyCallback = nullptr;
				slot.replyContext = nullptr;
			}
			drv.slotsReady = 1;
			drv.freeSlots.Reset();
			drv.pendingSlots.Reset();
			for (uint32 i = 0; i < kSlotsPerCore; i++)
				drv.freeSlots.Add(drv.slots + i);
		}

		IPCRequestSlot* AcquireSlot(IPCCoreDriver& drv, IOSDevHandle devHandle, IPCCommandId cmdId, OSMessageQueue* replyQueue, MEMPTR<void> replyCallback, MEMPTR<void> replyContext)
		{
			cemu_assert_debug(drv.coreIndex == OSGetCoreId());
			IPCRequestSlot* slot = drv.freeSlots.Remove();
			if (!slot)
			{
				// every slot is waiting for IOSU; let other threads run until one comes back
				cemuLog_log(LogType::Force, "IPC: all request slots of core {} are busy", drv.coreIndex);
				do
				{
					OSYieldThread();
					slot = drv.freeSlots.Remove();
				} while (!slot);
			}
			slot->inUse = 1;
			slot->replyQueue = replyQueue;
			slot->replyCallback = replyCallback;
			slot->replyContext = replyContext;
			IPCCommandBody& cmd = slot->buffer->cmd;
			cmd.cmdId = cmdId;
			cmd.devHandle = devHandle;
			cmd.result = 0;
			cmd.ukn0C = 0;
			cmd.ukn14 = 0;
			return slot;
		}

		void ReturnSlot(IPCCoreDriver& drv, IPCRequestSlot* slot)
		{
			slot->inUse = 0;
			drv.freeSlots.Add(slot);
		}

		// on hardware the kernel translates the PPC pointers of a request; here the virtual
		// addresses are passed on unchanged
		void FillInPointers(IPCCommandBody& cmd)
		{
			switch ((IPCCommandId)cmd.cmdId)
			{
			case IPCCommandId::IOS_OPEN:
				cmd.args[0] = cmd.ppcVirt0.GetMPTR();
				break;
			case IPCCommandId::IOS_CLOSE:
				break;
			case IPCCommandId::IOS_IOCTL:
				cmd.args[1] = cmd.ppcVirt0.GetMPTR();
				cmd.args[3] = cmd.ppcVirt1.GetMPTR();
				break;
			case IPCCommandId::IOS_IOCTLV:
				cmd.args[3] = cmd.ppcVirt0.GetMPTR();
				break;
			default:
				cemuLog_log(LogType::Force, "IPC: unknown command {}", (uint32)(IPCCommandId)cmd.cmdId);
				break;
			}
		}

		void Submit(IPCCoreDriver& drv, IPCRequestSlot* slot)
		{
			if (!slot->replyCallback)
				OSInitEvent(&slot->replyEvent, OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_AUTO);
			drv.pendingSlots.Add(slot);
			cemu_assert_debug(drv.state == IPCDriverState::READY || drv.state == IPCDriverState::INITIALIZED);
			// IOSU has to know the sending core so the reply comes back to the same driver
			drv.state = IPCDriverState::SUBMITTING;
			while (IPCRequestSlot* next = drv.pendingSlots.Remove())
			{
				IPCCommandBody& cmd = next->buffer->cmd;
				FillInPointers(cmd);
				iosu::kernel::IPCSubmitFromCOS(drv.coreIndex, &cmd);
			}
			drv.state = IPCDriverState::READY;
		}

		uint32 WaitForReply(IPCCoreDriver& drv, IPCRequestSlot* slot)
		{
			OSWaitEvent(&slot->replyEvent);
			uint32 result = slot->buffer->cmd.result;
			ReturnSlot(drv, slot);
			return result;
		}

		void DeliverReply(IPCCoreDriver& drv, IPCCommandBody* reply, uint32 coreIndex)
		{
			uint32 index = (uint32)((IPCRequestBuffer*)reply - drv.buffers.GetPtr());
			cemu_assert(index < kSlotsPerCore);
			IPCRequestSlot& slot = drv.slots[index];
			cemu_assert(slot.inUse != 0);
			if (slot.replyQueue)
			{
				OSMessage msg;
				msg.message = 0;
				msg.data0 = reply->result;
				msg.data1 = slot.replyContext.GetMPTR();
				msg.data2 = kReplyMessageQueued;
				cemu_assert(OSSendMessage(slot.replyQueue.GetPtr(), &msg, 0) != 0);
				ReturnSlot(drv, &slot);
			}
			else if (slot.replyCallback)
			{
				OSMessage msg;
				msg.message = slot.replyCallback.GetMPTR();
				msg.data0 = reply->result;
				msg.data1 = slot.replyContext.GetMPTR();
				msg.data2 = kReplyMessageCallback;
				cemu_assert(OSSendMessage(s_ipcThreadQueues.GetPtr() + coreIndex, &msg, 0) != 0);
				ReturnSlot(drv, &slot);
			}
			else
			{
				// synchronous: the waiting caller reads the result and frees the slot
				OSSignalEvent(&slot.replyEvent);
			}
		}

		// runs the async reply callbacks of one core
		void IPCThreadMain(PPCInterpreter_t* hCPU)
		{
			OSMessageQueue* queue = s_ipcThreadQueues.GetPtr() + OSGetCoreId();
			for (;;)
			{
				OSMessage msg;
				OSReceiveMessage(queue, &msg, OS_MESSAGE_BLOCK);
				cemu_assert(msg.data2 == kReplyMessageCallback);
				MPTR callback = msg.message;
				cemu_assert(callback != MPTR_NULL);
				PPCCoreCallback(callback, (uint32)msg.data0, (uint32)msg.data1);
			}
		}

		void StartIPCThread(uint32 coreIndex)
		{
			// the queue can hold a reply for every slot of the core
			OSInitMessageQueue(s_ipcThreadQueues.GetPtr() + coreIndex, s_ipcThreadQueueStorage.GetPtr() + coreIndex * kSlotsPerCore, kSlotsPerCore);
			OSThread_t* thread = s_ipcThreads.GetPtr() + coreIndex;
			uint8* stackTop = s_ipcThreadStacks.GetPtr() + kIpcThreadStackSize * (coreIndex + 1);
			__OSCreateThreadType(thread, PPCInterpreter_makeCallableExportDepr(IPCThreadMain), 0, nullptr, stackTop, kIpcThreadStackSize, 15, 1u << coreIndex, OSThread_t::THREAD_TYPE::TYPE_DRIVER);
			char* name = (char*)s_ipcThreadNames.GetPtr() + coreIndex * kIpcThreadNameSize;
			snprintf(name, kIpcThreadNameSize, "{SYS IPC Core %u}", coreIndex);
			OSSetThreadName(thread, name);
			OSResumeThread(thread);
		}

		void PrepareOpen(IPCRequestSlot* slot, const char* devicePath, uint32 flags)
		{
			IPCRequestBuffer* buf = slot->buffer.GetPtr();
			size_t length = strlen(devicePath);
			if (length > kMaxDevicePathLength)
			{
				cemuLog_log(LogType::Force, "IOS_Open: device path longer than {} characters", kMaxDevicePathLength);
				cemu_assert_error();
			}
			// the path is carried in the request buffer right after the command body
			memcpy(buf->payload, devicePath, length + 1);
			IPCCommandBody& cmd = buf->cmd;
			cmd.ppcVirt0 = MEMPTR<void>(buf->payload).GetMPTR();
			cmd.args[0] = MPTR_NULL;
			cmd.args[1] = (uint32)(length + 1);
			cmd.args[2] = flags;
		}

		void PrepareIoctl(IPCRequestSlot* slot, uint32 requestId, void* ptrIn, uint32 sizeIn, void* ptrOut, uint32 sizeOut)
		{
			IPCCommandBody& cmd = slot->buffer->cmd;
			cmd.args[0] = requestId;
			cmd.args[1] = MPTR_NULL;
			cmd.args[2] = sizeIn;
			cmd.args[3] = MPTR_NULL;
			cmd.args[4] = sizeOut;
			cmd.ppcVirt0 = MEMPTR<void>(ptrIn).GetMPTR();
			cmd.ppcVirt1 = MEMPTR<void>(ptrOut).GetMPTR();
		}

		void PrepareIoctlv(IPCRequestSlot* slot, uint32 requestId, uint32 numIn, uint32 numOut, IPCIoctlVector* vec)
		{
			IPCCommandBody& cmd = slot->buffer->cmd;
			cmd.ppcVirt0 = MEMPTR<void>(vec).GetMPTR();
			cmd.args[0] = requestId;
			cmd.args[1] = numIn;
			cmd.args[2] = numOut;
			cmd.args[3] = MPTR_NULL;
		}
	}

	void IPCDriver_NotifyResponses(uint32 ppcCoreIndex, IPCCommandBody** responseArray, uint32 numResponses)
	{
		cemu_assert(numResponses < kMaxRepliesPerNotify);
		IPCCoreDriver& drv = DriverOf(ppcCoreIndex);
		drv.mailbox.count = numResponses;
		for (uint32 i = 0; i < numResponses; i++)
			drv.mailbox.replies[i] = responseArray[i];
		cemu_assert_debug(numResponses != 0);
		for (uint32 i = 0; i < drv.mailbox.count; i++)
			DeliverReply(drv, drv.mailbox.replies[i].GetPtr(), drv.coreIndex);
		drv.mailbox.count = 0;
	}

	IOSDevHandle IOS_Open(const char* devicePath, uint32 flags)
	{
		IPCCoreDriver& drv = CurrentDriver();
		IPCRequestSlot* slot = AcquireSlot(drv, 0, IPCCommandId::IOS_OPEN, nullptr, nullptr, nullptr);
		PrepareOpen(slot, devicePath, flags);
		Submit(drv, slot);
		return WaitForReply(drv, slot);
	}

	IOS_ERROR IOS_Close(IOSDevHandle devHandle)
	{
		IPCCoreDriver& drv = CurrentDriver();
		IPCRequestSlot* slot = AcquireSlot(drv, devHandle, IPCCommandId::IOS_CLOSE, nullptr, nullptr, nullptr);
		Submit(drv, slot);
		return (IOS_ERROR)WaitForReply(drv, slot);
	}

	IOS_ERROR IOS_Ioctl(IOSDevHandle devHandle, uint32 requestId, void* ptrIn, uint32 sizeIn, void* ptrOut, uint32 sizeOut)
	{
		IPCCoreDriver& drv = CurrentDriver();
		IPCRequestSlot* slot = AcquireSlot(drv, devHandle, IPCCommandId::IOS_IOCTL, nullptr, nullptr, nullptr);
		PrepareIoctl(slot, requestId, ptrIn, sizeIn, ptrOut, sizeOut);
		Submit(drv, slot);
		return (IOS_ERROR)WaitForReply(drv, slot);
	}

	IOS_ERROR IOS_IoctlAsync(IOSDevHandle devHandle, uint32 requestId, void* ptrIn, uint32 sizeIn, void* ptrOut, uint32 sizeOut, MEMPTR<void> asyncResultFunc, MEMPTR<void> asyncResultUserParam)
	{
		IPCCoreDriver& drv = CurrentDriver();
		IPCRequestSlot* slot = AcquireSlot(drv, devHandle, IPCCommandId::IOS_IOCTL, nullptr, asyncResultFunc, asyncResultUserParam);
		PrepareIoctl(slot, requestId, ptrIn, sizeIn, ptrOut, sizeOut);
		Submit(drv, slot);
		return IOS_ERROR_OK;
	}

	IOS_ERROR IOS_Ioctlv(IOSDevHandle devHandle, uint32 requestId, uint32 numIn, uint32 numOut, IPCIoctlVector* vec)
	{
		IPCCoreDriver& drv = CurrentDriver();
		IPCRequestSlot* slot = AcquireSlot(drv, devHandle, IPCCommandId::IOS_IOCTLV, nullptr, nullptr, nullptr);
		PrepareIoctlv(slot, requestId, numIn, numOut, vec);
		Submit(drv, slot);
		return (IOS_ERROR)WaitForReply(drv, slot);
	}

	IOS_ERROR IOS_IoctlvAsync(IOSDevHandle devHandle, uint32 requestId, uint32 numIn, uint32 numOut, IPCIoctlVector* vec, MEMPTR<void> asyncResultFunc, MEMPTR<void> asyncResultUserParam)
	{
		IPCCoreDriver& drv = CurrentDriver();
		IPCRequestSlot* slot = AcquireSlot(drv, devHandle, IPCCommandId::IOS_IOCTLV, nullptr, asyncResultFunc, asyncResultUserParam);
		PrepareIoctlv(slot, requestId, numIn, numOut, vec);
		Submit(drv, slot);
		return IOS_ERROR_OK;
	}

	void MapIPCExports()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::PPC_IPC)
		EXPORT_FN(IOS_Open); EXPORT_FN(IOS_Close); EXPORT_FN(IOS_Ioctl);
		EXPORT_FN(IOS_IoctlAsync); EXPORT_FN(IOS_Ioctlv); EXPORT_FN(IOS_IoctlvAsync);
#undef EXPORT_FN
	}

	void InitializeIPC()
	{
		for (uint32 core = 0; core < Espresso::CORE_COUNT; core++)
		{
			SetUpDriver(core);
			StartIPCThread(core);
		}
	}
}
