// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"

namespace TCL
{
	// ---------------------------------------------------------------------------------
	// Retire timestamps
	//
	// Every submission is followed by an end-of-pipe event that makes the GPU store a
	// 64-bit counter into guest memory once it has finished everything before it. The
	// counter value of the newest submission is handed back to GX2, which compares it
	// against the stored value to find out whether the GPU has caught up.
	// ---------------------------------------------------------------------------------

	SysAllocator<uint64be> g_retiredStamp; // written by the GPU (EOP event)
	SysAllocator<coreinit::OSEvent> g_retireEvent; // signalled whenever the GPU stores a new stamp
	uint64 g_issuedStamp = 0; // stamp of the newest submission (CPU side only)

	static uint64 LoadRetiredStamp()
	{
		stdx::atomic_ref<uint64be> stamp(*g_retiredStamp.GetPtr());
		return stamp.load();
	}

	void TCLGPUNotifyNewRetirementTimestamp()
	{
		__OSLockScheduler();
		coreinit::OSSignalEventAllInternal(g_retireEvent.GetPtr());
		__OSUnlockScheduler();
	}

	int TCLTimestamp(TCLTimestampId id, uint64be* timestampOut)
	{
		if (id != TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			cemuLog_log(LogType::Force, "TCLTimestamp(): unknown timestamp id {}", (uint32)id);
			*timestampOut = 0;
			return 0;
		}
		*timestampOut = LoadRetiredStamp();
		return 0;
	}

	int TCLWaitTimestamp(TCLTimestampId id, uint64 waitTs, uint64 timeout)
	{
		if (id != TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			cemuLog_log(LogType::Force, "TCLWaitTimestamp(): unknown timestamp id {}", (uint32)id);
			return 0;
		}
		// the timeout is not enforced, the GPU always catches up eventually
		while (LoadRetiredStamp() < waitTs)
			coreinit::OSWaitEvent(g_retireEvent.GetPtr());
		return 0;
	}

	// ---------------------------------------------------------------------------------
	// Ring buffer
	//
	// Single producer (the CPU thread submitting through TCLSubmitToRing) and a single
	// consumer (the Latte command processor). One slot always stays unused so that
	// "read == write" can only mean empty.
	// ---------------------------------------------------------------------------------

	constexpr uint32 kRingWords = 4096; // power of two
	constexpr uint32 kRingMask = kRingWords - 1;

	std::atomic<uint32> g_ring[kRingWords];
	std::atomic<uint32> g_ringRead{0}; // next word the GPU will read
	std::atomic<uint32> g_ringWrite{0}; // next word the CPU will write

	bool TCLGPUReadRBWord(uint32& cmdWord)
	{
		const uint32 rd = g_ringRead.load(std::memory_order_relaxed);
		if (rd == g_ringWrite.load(std::memory_order_acquire))
			return false; // nothing queued
		cmdWord = g_ring[rd].load(std::memory_order_relaxed);
		g_ringRead.store((rd + 1) & kRingMask, std::memory_order_release);
		return true;
	}

	static uint32 RingFreeWords()
	{
		const uint32 wr = g_ringWrite.load(std::memory_order_relaxed);
		const uint32 rd = g_ringRead.load(std::memory_order_acquire);
		if (wr == rd)
			return kRingWords;
		return (rd - wr) & kRingMask;
	}

	static void RingWaitForSpace(uint32 wordCount)
	{
		// keep at least one slot free (see above)
		while (RingFreeWords() < wordCount + 1)
			coreinit::OSSleepTicks(coreinit::EspressoTime::ConvertNsToTimerTicks(500000)); // 0.5ms
	}

	// caller must have made sure the words fit
	static void RingPush(const uint32be* words, uint32 wordCount)
	{
		uint32 wr = g_ringWrite.load(std::memory_order_relaxed);
		for (uint32 i = 0; i < wordCount; i++)
		{
			g_ring[wr].store((uint32)words[i], std::memory_order_relaxed);
			wr = (wr + 1) & kRingMask;
		}
		g_ringWrite.store(wr, std::memory_order_release); // publish all words at once
	}

	constexpr uint32 kEopPacketWords = 6;

	// end-of-pipe event that stores the next retire stamp (64-bit) into g_retiredStamp
	static void RingPushRetireMarker(bool raiseInterrupt)
	{
		constexpr uint32 kEventTimestamp = 5; // EVENT_TYPE: cache flush and timestamp
		constexpr uint32 kEventIndexEop = 4;
		constexpr uint32 kDataSel64Bit = 2u << 29;
		constexpr uint32 kIntSelOnWrite = 2u << 24;

		g_issuedStamp++;
		const MPTR stampAddr = MEMPTR<void>(g_retiredStamp.GetPtr()).GetMPTR();
		uint32be packet[kEopPacketWords];
		packet[0] = pm4HeaderType3(IT_EVENT_WRITE_EOP, 5);
		packet[1] = kEventIndexEop | (kEventTimestamp << 8);
		packet[2] = stampAddr; // low address bits
		packet[3] = kDataSel64Bit | (raiseInterrupt ? kIntSelOnWrite : 0); // upper address bits are zero
		packet[4] = (uint32)g_issuedStamp;
		packet[5] = (uint32)(g_issuedStamp >> 32);
		RingPush(packet, kEopPacketWords);
	}

	int TCLSubmitToRing(uint32be* cmd, uint32 cmdLen, betype<TCLSubmissionFlag>* controlFlags, uint64be* timestampValueOut)
	{
		const TCLSubmissionFlag flags = *controlFlags;
		// the commands and the marker go in together so the GPU never sees half a submission
		RingWaitForSpace(cmdLen + kEopPacketWords);
		RingPush(cmd, cmdLen);
		if (!HAS_FLAG(flags, TCLSubmissionFlag::USE_RETIRED_MARKER))
		{
			cemuLog_logDebug(LogType::Force, "TCLSubmitToRing(): submission without retire marker is not supported");
			return 0;
		}
		RingPushRetireMarker(!HAS_FLAG(flags, TCLSubmissionFlag::NO_MARKER_INTERRUPT));
		if (timestampValueOut)
			*timestampValueOut = g_issuedStamp;
		return 0;
	}

	class TCLModule : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "tcl";
		}

		void RPLMapped() override
		{
			cafeExportRegister("TCL", TCLSubmitToRing, LogType::Placeholder);
			cafeExportRegister("TCL", TCLTimestamp, LogType::Placeholder);
			cafeExportRegister("TCL", TCLWaitTimestamp, LogType::Placeholder);
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason != coreinit::RplEntryReason::Loaded && reason != coreinit::RplEntryReason::Unloaded)
				return;
			g_issuedStamp = 0;
			*g_retiredStamp.GetPtr() = 0;
			if (reason == coreinit::RplEntryReason::Loaded)
				coreinit::OSInitEvent(g_retireEvent.GetPtr(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
		}
	};

	TCLModule g_tclModule;

	COSModule* GetModule()
	{
		return &g_tclModule;
	}
}
