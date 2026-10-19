// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "GX2_Command.h"
#include "GX2_Event.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"
#include "util/helpers/ConcurrentQueue.h"

// GPU fences, user timestamps, event callbacks and the vsync/flip waits.
// Event callbacks are never run on the GPU thread: the GPU side queues the event and a
// dedicated guest thread ("GX2 event callback") calls the registered guest function.

namespace GX2
{
	namespace
	{
		struct CallbackSlot
		{
			MEMPTR<void> func;
			MEMPTR<void> userData;
		};

		CallbackSlot s_callbacks[GX2CallbackEventTypeCount]{};

		// threads blocked in GX2WaitForVsync / GX2WaitForFlip
		SysAllocator<coreinit::OSThreadQueue> s_vsyncWaiters;
		SysAllocator<coreinit::OSThreadQueue> s_flipWaiters;

		// callback dispatch thread
		bool s_dispatchThreadStarted{};
		SysAllocator<OSThread_t> s_dispatchThread;
		SysAllocator<uint8, 0x2000> s_dispatchThreadStack;
		SysAllocator<char, 64> s_dispatchThreadName;
		SysAllocator<coreinit::OSSemaphore> s_pendingEventsSem; // counts queued events
		ConcurrentQueue<GX2CallbackEventType> s_pendingEvents;

		bool IsValidEventType(GX2CallbackEventType t)
		{
			return (size_t)t < GX2CallbackEventTypeCount;
		}

		void WakeAll(coreinit::OSThreadQueue* queue)
		{
			__OSLockScheduler();
			queue->wakeupEntireWaitQueue(false);
			__OSUnlockScheduler();
		}

		void WaitOn(coreinit::OSThreadQueue* queue)
		{
			__OSLockScheduler();
			queue->queueAndWait(coreinit::OSGetCurrentThread());
			__OSUnlockScheduler();
		}

		// guest thread entry: run callbacks for queued events, forever
		void DispatchThreadMain(PPCInterpreter_t* hCPU)
		{
			while (coreinit::OSWaitSemaphore(s_pendingEventsSem))
			{
				GX2CallbackEventType ev;
				if (!s_pendingEvents.peek2(ev))
					continue;
				const CallbackSlot& slot = s_callbacks[(size_t)ev];
				if (slot.func)
					PPCCoreCallback(slot.func, (sint32)ev, slot.userData);
			}
			osLib_returnFromFunction(hCPU, 0);
		}
	}

	// WAIT_REG_MEM on a 32-bit memory value: wait until (value & mask) <op> compareValue
	void GX2SetGPUFence(uint32be* fencePtr, uint32 mask, uint32 compareOp, uint32 compareValue)
	{
		// GX2 compare op -> WAIT_REG_MEM function
		static constexpr uint8 kWaitFunction[8] = { 0x7, 0x1, 0x3, 0x2, 0x6, 0x4, 0x5, 0x0 };
		GX2ReserveCmdSpace(7);
		gx2WriteGather_submit(pm4HeaderType3(IT_WAIT_REG_MEM, 6),
			(uint32)kWaitFunction[compareOp & 7] | 0x10, // 0x10: poll memory instead of a register
			memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(fencePtr)) | 2,
			0, compareValue, mask,
			0xA); // poll interval
	}

	enum class GX2PipeEventType : uint32
	{
		TOP = 0,
		BOTTOM = 1,
		BOTTOM_AFTER_FLUSH = 2
	};

	void GX2SubmitUserTimeStamp(uint64* timestampOut, uint64 value, GX2PipeEventType eventType, uint32 triggerInterrupt)
	{
		GX2ReserveCmdSpace(11);
		const MPTR dst = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(timestampOut));
		const uint32 lo = (uint32)value;
		const uint32 hi = (uint32)(value >> 32);
		const bool irq = triggerInterrupt != 0;

		// 64-bit memory write of the timestamp
		auto writeValue = [&]() { gx2WriteGather_submit(pm4HeaderType3(IT_MEM_WRITE, 4), dst | 2, 0, lo, hi); };
		// HLE packet: write the value at the bottom of the pipe, then run the TIMESTAMP_BOTTOM callback
		auto writeValueWithCallback = [&]() { gx2WriteGather_submit(pm4HeaderType3(IT_HLE_BOTTOM_OF_PIPE_CB, 3), dst, lo, hi); };
		// The two words after a MEM_WRITE that request the top/bottom interrupt are not a complete
		// packet. They are kept exactly as before because the GPU side has always seen them like this.
		auto interruptWords = [](uint32 eventId) { gx2WriteGather_submit(eventId, 0x40000000); };

		switch (eventType)
		{
		case GX2PipeEventType::TOP:
			writeValue();
			if (irq)
				interruptWords(0x304A);
			break;
		case GX2PipeEventType::BOTTOM_AFTER_FLUSH:
			writeValue();
			if (irq)
			{
				interruptWords(0x304B);
				writeValueWithCallback();
			}
			break;
		case GX2PipeEventType::BOTTOM:
			if (irq)
				writeValueWithCallback();
			else
				writeValue();
			break;
		default:
			cemu_assert_debug(false);
			break;
		}
	}

	void GX2SetEventCallback(GX2CallbackEventType eventType, void* callbackFuncPtr, void* userData)
	{
		if (!IsValidEventType(eventType))
		{
			cemuLog_log(LogType::Force, "GX2SetEventCallback(): invalid event type {}", (uint32)eventType);
			return;
		}
		s_callbacks[(size_t)eventType].func = callbackFuncPtr;
		s_callbacks[(size_t)eventType].userData = userData;
	}

	void GX2GetEventCallback(GX2CallbackEventType eventType, MEMPTR<void>* callbackFuncPtrOut, MEMPTR<void>* userDataOut)
	{
		if (!IsValidEventType(eventType))
		{
			cemuLog_log(LogType::Force, "GX2GetEventCallback(): invalid event type {}", (uint32)eventType);
			return;
		}
		const CallbackSlot& slot = s_callbacks[(size_t)eventType];
		if (callbackFuncPtrOut)
			*callbackFuncPtrOut = slot.func;
		if (userDataOut)
			*userDataOut = slot.userData;
	}

	void __GX2NotifyEvent(GX2CallbackEventType eventType)
	{
		if (!IsValidEventType(eventType))
		{
			cemu_assert_debug(false);
			return;
		}
		if (s_callbacks[(size_t)eventType].func)
		{
			s_pendingEvents.push(eventType);
			coreinit::OSSignalSemaphore(s_pendingEventsSem);
		}
		if (eventType == GX2CallbackEventType::VSYNC)
			WakeAll(s_vsyncWaiters.GetPtr());
		else if (eventType == GX2CallbackEventType::FLIP)
			WakeAll(s_flipWaiters.GetPtr());
	}

	void GX2WaitForVsync()
	{
		WaitOn(s_vsyncWaiters.GetPtr());
	}

	void GX2WaitForFlip()
	{
		// nothing to wait for when every requested flip has already been executed
		const uint32 requested = _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE);
		const uint32 executed = _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE);
		if (requested == executed)
			return;
		WaitOn(s_flipWaiters.GetPtr());
	}

	bool GX2DrawDone()
	{
		// Vulkan always does a full sync here (needed for texture readback and occlusion queries),
		// the other backends only when the option is enabled
		const bool fullSync = (g_renderer && g_renderer->GetType() == RendererAPI::Vulkan) || ActiveSettings::WaitForGX2DrawDoneEnabled();
		if (fullSync)
		{
			GX2ReserveCmdSpace(2);
			gx2WriteGather_submit(pm4HeaderType3(IT_HLE_SYNC_ASYNC_OPERATIONS, 1), 0);
		}
		GX2Command_Flush(0x100, true);
		return GX2WaitTimeStamp(GX2GetLastSubmittedTimeStamp());
	}

	void GX2Init_event()
	{
		if (s_dispatchThreadStarted)
			return;
		s_dispatchThreadStarted = true;
		strcpy(s_dispatchThreadName.GetPtr(), "GX2 event callback");
		const sint32 stackSize = (sint32)s_dispatchThreadStack.GetByteSize();
		uint8* stackTop = (uint8*)s_dispatchThreadStack.GetPtr() + stackSize;
		coreinit::OSCreateThreadType(s_dispatchThread, PPCInterpreter_makeCallableExportDepr(DispatchThreadMain), 0, nullptr,
			stackTop, stackSize, 16, OSThread_t::ATTR_DETACHED, OSThread_t::THREAD_TYPE::TYPE_IO);
		coreinit::OSSetThreadName(s_dispatchThread, s_dispatchThreadName);
		coreinit::OSResumeThread(s_dispatchThread);
	}

	void GX2EventInit()
	{
		cafeExportRegister("gx2", GX2SetGPUFence, LogType::GX2);
		cafeExportRegister("gx2", GX2SubmitUserTimeStamp, LogType::GX2);

		cafeExportRegister("gx2", GX2SetEventCallback, LogType::GX2);
		cafeExportRegister("gx2", GX2GetEventCallback, LogType::GX2);

		cafeExportRegister("gx2", GX2WaitForVsync, LogType::GX2);
		cafeExportRegister("gx2", GX2WaitForFlip, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawDone, LogType::GX2);

		coreinit::OSInitThreadQueue(s_vsyncWaiters.GetPtr());
		coreinit::OSInitThreadQueue(s_flipWaiters.GetPtr());
		coreinit::OSInitSemaphore(s_pendingEventsSem, 0);
	}

	void GX2EventResetToDefaultState()
	{
		s_dispatchThreadStarted = false;
		for (CallbackSlot& slot : s_callbacks)
		{
			slot.func = nullptr;
			slot.userData = nullptr;
		}
	}
}
