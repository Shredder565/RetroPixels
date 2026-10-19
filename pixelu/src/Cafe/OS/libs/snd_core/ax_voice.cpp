// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// The voice pool and the AXSetVoice* parameter API.
//
// A voice is an AXVPB handle (guest memory, given to the title) plus its two
// parameter blocks (see ax_internal.h). Setters write the PPC copy and mark
// the changed group in AXVPB.sync; the frame thread copies marked groups to
// the DSP copy before rendering.
//
// Allocation: free voices sit on a stack; allocated voices are kept per
// priority in allocation order. When the pool is empty, the oldest voice of
// the lowest priority below the requested one is taken away from its owner
// (the owner's drop callback is called first).
//
// Protection: between AXUserBegin/AXUserEnd (per thread, nestable) or
// AXVoiceBegin/AXVoiceEnd (per voice), the frame thread must not pick up a
// half-written set of parameters. A protected voice keeps rendering with its
// old parameters until the owning thread ends the block.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "util/helpers/fspinlock.h"

namespace snd_core
{
	FSpinlock g_axVoiceListLock;
	AXVPBInternal_t* __AXVPBInternalVoiceArray = nullptr;
	AXVPB* __AXVPBArrayPtr = nullptr;

	namespace
	{
		// result codes
		constexpr sint32 kErrBadVoice = -1;
		constexpr sint32 kErrInFrame = -2;
		constexpr sint32 kErrNotProtected = -3;
		constexpr sint32 kErrTableFull = -4;
		constexpr sint32 kErrNullMix = -3;
		constexpr sint32 kErrNullVoice = -4;

		// sync bits every freshly (re)initialised voice starts with
		constexpr uint32 kResetDirty = VoiceDirty::State | VoiceDirty::Src | VoiceDirty::Lpf | VoiceDirty::Biquad |
			VoiceDirty::RmtOn | VoiceDirty::ItdOn | VoiceDirty::RmtSrc | VoiceDirty::RmtIir;

		// storage the title and the DSP can see
		SysAllocator<AXVPB, AX_MAX_VOICES> s_handles;
		SysAllocator<AXVPBInternal_t, AX_MAX_VOICES> s_ppcBlocks;
		SysAllocator<AXVPBInternal_t, AX_MAX_VOICES> s_dspBlocks;
		SysAllocator<AXVPBItd, AX_MAX_VOICES> s_itd;

		struct Pool
		{
			std::vector<AXVPB*> freeStack;
			std::vector<AXVPB*> byPriority[AX_PRIORITY_MAX];
		};
		Pool s_pool;

		// who holds a voice: owner thread (guest address) and nesting depth
		struct Hold
		{
			MPTR thread = MPTR_NULL;
			uint32 depth = 0;
		};
		std::vector<Hold> s_userHolds;    // AXUserBegin, per thread, in begin order
		Hold s_voiceHolds[AX_MAX_VOICES]; // AXVoiceBegin / implicit, per voice

		struct LoopTracker
		{
			uint32 loops = 0;
			uint32 lastPos = 0;
		};
		LoopTracker s_loops[AX_MAX_VOICES];

		MPTR CallerThread()
		{
			return memory_getVirtualOffsetFromPointer(coreinit::OSGetCurrentThread());
		}

		void MarkDirty(AXVPB* vpb, uint32 bits)
		{
			vpb->sync = (uint32)vpb->sync | bits;
		}

		void ClearDirty(AXVPB* vpb, uint32 bits)
		{
			vpb->sync = (uint32)vpb->sync & ~bits;
		}

		Hold* UserHoldOf(MPTR thread)
		{
			for (auto it = s_userHolds.rbegin(); it != s_userHolds.rend(); ++it)
			{
				if (it->thread == thread)
					return &*it;
			}
			return nullptr;
		}

		// a setter touched the voice inside a user block: keep the frame thread off it
		// until that thread's AXUserEnd
		void HoldForCaller(AXVPB* vpb)
		{
			if (s_userHolds.empty() || AXIst_IsFrameBeingProcessed())
				return;
			Hold& h = s_voiceHolds[(uint32)vpb->index];
			if (h.thread == MPTR_NULL)
				h.thread = CallerThread();
		}

		void Touched(AXVPB* vpb, uint32 bits)
		{
			MarkDirty(vpb, bits);
			HoldForCaller(vpb);
		}

		void PushAllocated(AXVPB* vpb, uint32 priority)
		{
			s_pool.byPriority[priority].push_back(vpb);
			vpb->next = nullptr;
			vpb->prev = nullptr;
			vpb->priority = priority;
		}

		void RemoveAllocated(AXVPB* vpb)
		{
			const uint32 priority = vpb->priority;
			if (priority == AX_PRIORITY_FREE || priority >= AX_PRIORITY_MAX)
				return;
			auto& list = s_pool.byPriority[priority];
			list.erase(std::remove(list.begin(), list.end(), vpb), list.end());
		}

		void PushFree(AXVPB* vpb)
		{
			vpb->priority = AX_PRIORITY_FREE;
			vpb->next = nullptr;
			vpb->prev = nullptr;
			s_pool.freeStack.push_back(vpb);
		}

		AXVPB* PopFree()
		{
			if (s_pool.freeStack.empty())
				return nullptr;
			AXVPB* vpb = s_pool.freeStack.back();
			s_pool.freeStack.pop_back();
			return vpb;
		}

		void ResetLoopCount(uint32 index)
		{
			s_loops[index] = LoopTracker{};
		}

		// what a voice looks like right after allocation or release
		void ResetVoice(AXVPB* vpb)
		{
			AXVPBInternal_t* pb = PpcBlock(vpb);
			const uint32 mixer = AXGetDefaultMixerSelect();
			vpb->playbackState = 0;
			vpb->sync = kResetDirty;
			vpb->mixerSelect = mixer;
			pb->mixerSelect = (uint16)mixer;
			pb->playbackState = 0;
			pb->itdOn = 0;
			pb->rmtOn = 0;
			pb->lpf.on = 0;
			pb->biquad.on = 0;
			pb->srcMode = AX_FILTER_MODE_TAP;
			pb->src.currentFrac = 0;
			for (auto& h : pb->src.historySamples)
				h = 0;
			for (auto& v : pb->rmtSrcState)
				v = 0;
			for (auto& m : pb->mixMaskTV)
				m = 0;
			for (auto& m : pb->mixMaskDRC)
				m = 0;
			for (auto& m : pb->mixMaskRMT)
				m = 0;
			memset(pb->tvVolumeNow, 0, sizeof(pb->tvVolumeNow));
			memset(pb->reserved218, 0, sizeof(pb->reserved218));
			memset(pb->reserved258, 0, sizeof(pb->reserved258));
			ResetLoopCount(vpb->index);
		}

		// take the oldest voice of the lowest priority below 'priority' away from its owner
		AXVPB* StealVoice(uint32 priority)
		{
			for (uint32 p = AX_PRIORITY_LOWEST; p < priority; p++)
			{
				auto& list = s_pool.byPriority[p];
				if (list.empty())
					continue;
				AXVPB* victim = list.front();
				cemuLog_logDebug(LogType::Force, "AX: voice {} dropped for a priority {} request", (uint32)victim->index, priority);
				if (victim->playbackState != 0u)
					victim->depop = 1;
				if (victim->callback != 0u)
					PPCCoreCallback(victim->callback, victim);
				victim->dropReason = 0;
				if (victim->callbackEx != 0u)
					PPCCoreCallback(victim->callbackEx, victim, victim->userParam, victim->dropReason);
				RemoveAllocated(victim);
				PushAllocated(victim, priority);
				return victim;
			}
			return nullptr;
		}

		void WipeStorage()
		{
			__AXVPBArrayPtr = s_handles.GetPtr();
			__AXVPBInternalVoiceArray = s_ppcBlocks.GetPtr();
			memset(s_handles.GetPtr(), 0, sizeof(AXVPB) * AX_MAX_VOICES);
			memset(s_ppcBlocks.GetPtr(), 0, sizeof(AXVPBInternal_t) * AX_MAX_VOICES);
			memset(s_dspBlocks.GetPtr(), 0, sizeof(AXVPBInternal_t) * AX_MAX_VOICES);
			memset(s_itd.GetPtr(), 0, sizeof(AXVPBItd) * AX_MAX_VOICES);
		}

		MPTR PhysicalOf(void* p)
		{
			return memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(p));
		}

		// a sample-word address of 'offset' within the buffer at physical 'base'
		// (the low 29 address bits; see AXVoiceAddr)
		uint32 WordAddress(uint16 format, uint32 base, uint32 offset)
		{
			switch (format)
			{
			case AX_FORMAT_PCM8: return base + offset;
			case AX_FORMAT_PCM16: return base / 2 + offset;
			case AX_FORMAT_ADPCM: return base * 2 + offset;
			}
			return 0;
		}

		// the address extension bits (bits 29-31 of the byte address)
		uint32 AddressExtension(uint16 format, uint32 base, uint32 offset)
		{
			switch (format)
			{
			case AX_FORMAT_PCM8: return (base + offset) >> 29;
			case AX_FORMAT_PCM16: return (base + offset * 2) >> 29;
			case AX_FORMAT_ADPCM: return (base + offset / 2) >> 29;
			}
			return 0;
		}

		// high half mask of a word address, the 29-bit byte range in words
		uint32 WordHighMask(uint16 format)
		{
			switch (format)
			{
			case AX_FORMAT_PCM8: return 0x1FFF;
			case AX_FORMAT_PCM16: return 0x0FFF;
			case AX_FORMAT_ADPCM: return 0x3FFF;
			}
			return 0;
		}

		bool IsKnownFormat(uint16 format)
		{
			return format == AX_FORMAT_PCM8 || format == AX_FORMAT_PCM16 || format == AX_FORMAT_ADPCM;
		}

		// absolute word address -> offset relative to the physical sample base
		uint32 RelativeOffset(uint16 format, uint32 word, uint32 ext, uint32 base)
		{
			const uint32 extBytes = ext << 29;
			switch (format)
			{
			case AX_FORMAT_PCM8: return (word | extBytes) - base;
			case AX_FORMAT_PCM16: return (word | (extBytes / 2)) - base / 2;
			case AX_FORMAT_ADPCM: return (word | (extBytes * 2)) - base * 2;
			}
			return 0;
		}

		enum class OffsetField { Cur, Loop, End };

		// AXSetVoiceCurrentOffset & co: move one position of an already configured voice
		void SetOneOffset(AXVPB* vpb, OffsetField field, uint32 offset)
		{
			AXVPBInternal_t* pb = PpcBlock(vpb);
			const uint32 offsetRaw = _swapEndianU32(offset);
			switch (field)
			{
			case OffsetField::Cur: vpb->offsets.currentOffset = offsetRaw; break;
			case OffsetField::Loop: vpb->offsets.loopOffset = offsetRaw; break;
			case OffsetField::End: vpb->offsets.endOffset = offsetRaw; break;
			}
			const uint16 format = pb->addr.format;
			if (!IsKnownFormat(format))
			{
				cemuLog_logDebug(LogType::Force, "AX: offset change on voice {} without a sample format", (uint32)vpb->index);
				return;
			}
			const uint32 base = memory_virtualToPhysical(_swapEndianU32(vpb->offsets.samples)) & 0x1FFFFFFF;
			const uint32 word = WordAddress(format, base, offset);
			switch (field)
			{
			case OffsetField::Cur: pb->addr.SetCur(word); Touched(vpb, VoiceDirty::CurOffset); break;
			case OffsetField::Loop: pb->addr.SetLoop(word); Touched(vpb, VoiceDirty::LoopOffset); break;
			case OffsetField::End: pb->addr.SetEnd(word); Touched(vpb, VoiceDirty::EndOffset); break;
			}
		}

		bool IsValidHandle(AXVPB* voice, const char* what)
		{
			if (voice && (uint32)voice->index < (uint32)AX_MAX_VOICES)
				return true;
			cemuLog_log(LogType::Force, "{}: invalid voice", what);
			return false;
		}
	}

	AXVPBInternal_t* DspBlock(uint32 voiceIndex)
	{
		return s_dspBlocks.GetPtr() + voiceIndex;
	}

	const std::vector<AXVPB*>& AXVoicesOfPriority(uint32 priority)
	{
		return s_pool.byPriority[priority];
	}

	const std::vector<AXVPB*>& AXFreeVoices()
	{
		return s_pool.freeStack;
	}

	// --- pool -------------------------------------------------------------------------

	void AXVPB_Reset()
	{
		s_pool.freeStack.clear();
		for (auto& list : s_pool.byPriority)
			list.clear();
		s_userHolds.clear();
		for (auto& h : s_voiceHolds)
			h = Hold{};
		WipeStorage();
	}

	void AXVPB_Init()
	{
		AXVPB_Reset();
		AXVPB* handles = s_handles.GetPtr();
		for (uint32 i = 0; i < (uint32)AX_MAX_VOICES; i++)
		{
			AXVPB* vpb = handles + i;
			AXVPBInternal_t* ppc = s_ppcBlocks.GetPtr() + i;
			AXVPBInternal_t* dsp = s_dspBlocks.GetPtr() + i;
			vpb->index = i;
			vpb->itd = s_itd.GetPtr() + i;
			vpb->callback = MPTR_NULL;
			vpb->callbackEx = MPTR_NULL;
			ResetVoice(vpb);
			// the DSP blocks form one chain in memory order; the frame thread relinks it
			const MPTR self = PhysicalOf(dsp);
			const MPTR next = (i + 1 < (uint32)AX_MAX_VOICES) ? self + (MPTR)sizeof(AXVPBInternal_t) : 0;
			const MPTR itd = PhysicalOf(s_itd.GetPtr() + i);
			for (AXVPBInternal_t* pb : { ppc, dsp })
			{
				pb->index = (uint16)i;
				AXVoiceAddr::Split(pb->nextHi, pb->nextLo, next);
				AXVoiceAddr::Split(pb->selfHi, pb->selfLo, self);
				AXVoiceAddr::Split(pb->itdAddrHi, pb->itdAddrLo, itd);
			}
			PushFree(vpb);
		}
	}

	AXVPB* AXAcquireVoiceEx(uint32 priority, MPTR callbackEx, MPTR userParam)
	{
		if (priority == AX_PRIORITY_FREE || priority >= AX_PRIORITY_MAX)
		{
			cemuLog_log(LogType::APIErrors, "AXAcquireVoiceEx: invalid priority {}", priority);
			return nullptr;
		}
		g_axVoiceListLock.lock();
		AXVPB* vpb = PopFree();
		if (vpb)
			PushAllocated(vpb, priority);
		else
			vpb = StealVoice(priority);
		if (vpb)
		{
			vpb->userParam = userParam;
			vpb->callback = MPTR_NULL;
			vpb->callbackEx = callbackEx;
			ResetVoice(vpb);
		}
		g_axVoiceListLock.unlock();
		return vpb;
	}

	void AXFreeVoice(AXVPB* vpb)
	{
		if (!vpb)
			return;
		g_axVoiceListLock.lock();
		if ((uint32)vpb->priority == AX_PRIORITY_FREE)
		{
			cemuLog_log(LogType::Force, "AXFreeVoice: voice {} is already free", (uint32)vpb->index);
			g_axVoiceListLock.unlock();
			return;
		}
		s_voiceHolds[(uint32)vpb->index] = Hold{};
		RemoveAllocated(vpb);
		if (vpb->playbackState != 0u)
			vpb->depop = 1;
		ResetVoice(vpb);
		vpb->callback = MPTR_NULL;
		vpb->callbackEx = MPTR_NULL;
		PushFree(vpb);
		g_axVoiceListLock.unlock();
	}

	// --- protection -------------------------------------------------------------------

	bool AXUserIsProtected()
	{
		return !s_userHolds.empty();
	}

	sint32 AXUserBegin()
	{
		// a title that gets descheduled inside its user block can miss frames
		// (seen in Paper Mario: Color Splash), so give it a longer time slice
		PPCCore_boostQuantum(10000);
		if (AXIst_IsFrameBeingProcessed())
			return kErrInFrame;
		const MPTR thread = CallerThread();
		if (Hold* h = UserHoldOf(thread))
			return (sint32)++h->depth;
		if (s_userHolds.size() >= (size_t)AX_MAX_VOICES)
			return kErrTableFull;
		s_userHolds.push_back(Hold{ thread, 1 });
		return 1;
	}

	sint32 AXUserEnd()
	{
		PPCCore_deboostQuantum(10000);
		if (AXIst_IsFrameBeingProcessed())
			return kErrInFrame;
		const MPTR thread = CallerThread();
		Hold* h = UserHoldOf(thread);
		if (!h)
		{
			cemuLog_logDebug(LogType::Force, "AXUserEnd without AXUserBegin");
			return kErrNotProtected;
		}
		const sint32 depth = (sint32)--h->depth;
		if (depth == 0)
		{
			s_userHolds.erase(s_userHolds.begin() + (h - s_userHolds.data()));
			// voices this thread touched implicitly are released now
			for (auto& vh : s_voiceHolds)
			{
				if (vh.thread == thread && vh.depth == 0)
					vh.thread = MPTR_NULL;
			}
		}
		return depth;
	}

	sint32 AXVoiceBegin(AXVPB* voice)
	{
		if (!IsValidHandle(voice, "AXVoiceBegin"))
			return kErrBadVoice;
		if (AXIst_IsFrameBeingProcessed())
			return kErrInFrame;
		const MPTR thread = CallerThread();
		Hold& h = s_voiceHolds[(uint32)voice->index];
		if (h.thread == MPTR_NULL)
		{
			h = Hold{ thread, 1 };
			return 1;
		}
		if (h.thread == thread)
			return (sint32)++h.depth;
		return kErrBadVoice; // held by another thread
	}

	sint32 AXVoiceEnd(AXVPB* voice)
	{
		if (!IsValidHandle(voice, "AXVoiceEnd"))
			return kErrBadVoice;
		if (AXIst_IsFrameBeingProcessed())
			return kErrInFrame;
		const MPTR thread = CallerThread();
		Hold& h = s_voiceHolds[(uint32)voice->index];
		if (h.thread == MPTR_NULL)
			return kErrNotProtected;
		if (h.thread != thread)
			return kErrBadVoice;
		if (h.depth > 0)
			h.depth--;
		// inside a user block the voice stays held until AXUserEnd
		if (h.depth == 0 && !UserHoldOf(thread))
			h.thread = MPTR_NULL;
		return (sint32)h.depth;
	}

	bool AXIsVoiceProtected(const AXVPB* vpb)
	{
		return s_voiceHolds[(uint32)vpb->index].thread != MPTR_NULL;
	}

	// what AXVoiceIsProtected reports. Outside a frame this is "not held by the
	// calling thread", which is also true for a voice nobody holds; titles have
	// only ever seen this answer, so it is kept.
	bool AXVoiceProtectedForCaller(const AXVPB* vpb)
	{
		const MPTR owner = s_voiceHolds[(uint32)vpb->index].thread;
		if (AXIst_IsFrameBeingProcessed())
			return owner != MPTR_NULL;
		return owner != CallerThread();
	}

	// --- parameters -------------------------------------------------------------------

	sint32 AXIsValidDevice(sint32 device, sint32 deviceIndex)
	{
		// remote speakers are not accepted here
		if (device == AX_DEV_TV)
			return deviceIndex == 0 ? 0 : -2;
		if (device == AX_DEV_DRC)
			return (deviceIndex == 0 || deviceIndex == 1) ? 0 : -2;
		return -1;
	}

	// 'mix' holds one AXCHMIX per bus for each channel of the device, channel by channel
	sint32 AXSetVoiceDeviceMix(AXVPB* vpb, sint32 device, sint32 deviceIndex, AXCHMIX_DEPR* mix)
	{
		if (!vpb)
			return kErrNullVoice;
		if (!mix)
			return kErrNullMix;
		if (sint32 r = AXIsValidDevice(device, deviceIndex); r != 0)
			return r;
		AXVPBInternal_t* pb = PpcBlock(vpb);
		sint32 channels;
		AXCHMIX_DEPR* dst;
		uint16be* masks;
		if (device == AX_DEV_TV)
		{
			channels = AX_TV_CHANNEL_COUNT;
			dst = pb->deviceMixTV;
			masks = pb->mixMaskTV;
		}
		else
		{
			channels = AX_DRC_CHANNEL_COUNT;
			dst = pb->deviceMixDRC + deviceIndex * AX_DRC_CHANNEL_COUNT * AX_MAX_NUM_BUS;
			masks = pb->mixMaskDRC + deviceIndex * AX_MAX_NUM_BUS;
		}
		uint16 busMask[AX_MAX_NUM_BUS] = {};
		for (sint32 ch = 0; ch < channels; ch++)
		{
			for (sint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
			{
				const AXCHMIX_DEPR& in = mix[ch * AX_MAX_NUM_BUS + bus];
				dst[ch * AX_MAX_NUM_BUS + bus] = in;
				const uint16 state = in.delta != 0 ? 3 : (in.vol != 0 ? 1 : 0);
				busMask[bus] |= (uint16)(state << (ch * 2));
			}
		}
		for (sint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
			masks[bus] = busMask[bus];
		Touched(vpb, VoiceDirty::MixMask | VoiceDirty::Mix);
		return 0;
	}

	void AXSetVoiceState(AXVPB* vpb, sint32 voiceState)
	{
		if ((uint32)vpb->playbackState == (uint32)voiceState)
			return;
		vpb->playbackState = (uint32)voiceState;
		PpcBlock(vpb)->playbackState = _swapEndianU16((uint16)voiceState);
		Touched(vpb, VoiceDirty::State);
		if (voiceState == 0)
			vpb->depop = 1;
	}

	sint32 AXIsVoiceRunning(AXVPB* vpb)
	{
		return _swapEndianU16(PpcBlock(vpb)->playbackState) == 1 ? 1 : 0;
	}

	void AXSetVoiceType(AXVPB* vpb, uint16 voiceType)
	{
		PpcBlock(vpb)->voiceType = voiceType;
		Touched(vpb, VoiceDirty::Type);
	}

	// only coefficients, gain and predictor/scale; the decoder history stays
	void AXSetVoiceAdpcm(AXVPB* vpb, AXPBADPCM_t* adpcm)
	{
		AXPBADPCM_t& dst = PpcBlock(vpb)->adpcm;
		memcpy(dst.a, adpcm->a, sizeof(dst.a));
		dst.gain = adpcm->gain;
		dst.scale = adpcm->scale;
		Touched(vpb, VoiceDirty::Adpcm);
	}

	void AXSetVoiceAdpcmLoop(AXVPB* vpb, AXPBADPCMLOOP_t* adpcmLoop)
	{
		AXVPBInternal_t* pb = PpcBlock(vpb);
		pb->adpcmLoop.loopScale = adpcmLoop->loopScale;
		// streamed voices (type 1) keep running history across the loop
		if (pb->voiceType == 0)
		{
			pb->adpcmLoop.loopYn1 = adpcmLoop->loopYn1;
			pb->adpcmLoop.loopYn2 = adpcmLoop->loopYn2;
		}
		Touched(vpb, VoiceDirty::AdpcmLoop);
	}

	void AXSetVoiceSrc(AXVPB* vpb, AXPBSRC_t* src)
	{
		PpcBlock(vpb)->src = *src;
		ClearDirty(vpb, VoiceDirty::SrcRatio);
		Touched(vpb, VoiceDirty::Src);
	}

	void AXSetVoiceSrcType(AXVPB* vpb, uint32 srcType)
	{
		AXVPBInternal_t* pb = PpcBlock(vpb);
		switch (srcType)
		{
		case AX_SRC_TYPE_NONE:
			pb->srcMode = AX_FILTER_MODE_NONE;
			break;
		case AX_SRC_TYPE_LINEAR:
			pb->srcMode = AX_FILTER_MODE_LINEAR;
			break;
		case AX_SRC_TYPE_LOWPASS1:
		case AX_SRC_TYPE_LOWPASS2:
		case AX_SRC_TYPE_LOWPASS3:
			pb->srcMode = AX_FILTER_MODE_TAP;
			pb->srcTap = (uint16)(AX_FILTER_LOWPASS_8K + (srcType - AX_SRC_TYPE_LOWPASS1));
			break;
		default:
			cemuLog_log(LogType::Force, "AXSetVoiceSrcType: unknown type {}", srcType);
			break;
		}
		Touched(vpb, VoiceDirty::SrcType);
	}

	// ratio = input rate / output rate, stored as 16.16 fixed point, at most 8.0
	sint32 AXSetVoiceSrcRatio(AXVPB* vpb, float ratio)
	{
		constexpr uint32 kMaxRatio = 8u << 16;
		const float scaled = ratio * 65536.0f;
		uint32 fixed;
		if (!(scaled > 0.0f))
			fixed = 0; // also NaN
		else if (scaled >= (float)kMaxRatio)
			fixed = kMaxRatio;
		else
			fixed = (uint32)scaled;
		AXPBSRC_t& src = PpcBlock(vpb)->src;
		const uint16 hiRaw = _swapEndianU16((uint16)(fixed >> 16));
		const uint16 loRaw = _swapEndianU16((uint16)fixed);
		if (src.ratioHigh != hiRaw || src.ratioLow != loRaw)
		{
			src.ratioHigh = hiRaw;
			src.ratioLow = loRaw;
			Touched(vpb, VoiceDirty::SrcRatio);
		}
		return 0;
	}

	void AXSetVoiceVe(AXVPB* vpb, AXPBVE* ve)
	{
		AXVPBInternal_t* pb = PpcBlock(vpb);
		pb->veVolume = (uint16)ve->currentVolume;
		pb->veDelta = (uint16)(sint16)ve->currentDelta;
		Touched(vpb, VoiceDirty::Ve);
	}

	// one-pole low pass for a cutoff in Hz at the 32 kHz DSP rate:
	// b0 = c - sqrt(c^2 - 1) with c = 2 - cos(w), a0 = 1 - b0, both 0.15 fixed point
	void AXComputeLpfCoefs(uint32 freq, uint16be* a0, uint16be* b0)
	{
		const float w = (float)freq / 32000.0f * 6.2831855f;
		const float c = 2.0f - std::cos(w);
		const float pole = -(std::sqrt(c * c - 1.0f) - c) * 32768.0f;
		const uint32 b = (uint16)(sint32)pole;
		*a0 = (uint16)(0x7FFF - b);
		*b0 = (uint16)b;
	}

	void AXSetVoiceLpf(AXVPB* vpb, AXPBLPF_t* lpf)
	{
		PpcBlock(vpb)->lpf = *lpf;
		Touched(vpb, VoiceDirty::Lpf);
	}

	void AXSetVoiceLpfCoefs(AXVPB* vpb, uint16 a0, uint16 b0)
	{
		AXPBLPF_t& lpf = PpcBlock(vpb)->lpf;
		lpf.a0 = (sint16)_swapEndianU16(a0);
		lpf.b0 = (sint16)_swapEndianU16(b0);
		Touched(vpb, VoiceDirty::LpfCoefs);
	}

	void AXSetVoiceBiquad(AXVPB* vpb, AXPBBIQUAD_t* biquad)
	{
		PpcBlock(vpb)->biquad = *biquad;
		Touched(vpb, VoiceDirty::Biquad);
	}

	void AXSetVoiceBiquadCoefs(AXVPB* vpb, uint16 b0, uint16 b1, uint16 b2, uint16 a1, uint16 a2)
	{
		AXPBBIQUAD_t& bq = PpcBlock(vpb)->biquad;
		bq.b0 = _swapEndianU16(b0);
		bq.b1 = _swapEndianU16(b1);
		bq.b2 = _swapEndianU16(b2);
		bq.a1 = _swapEndianU16(a1);
		bq.a2 = _swapEndianU16(a2);
		Touched(vpb, VoiceDirty::BiquadCoefs);
	}

	// --- sample positions -------------------------------------------------------------

	void AXSetVoiceOffsets(AXVPB* vpb, AXPBOFFSET_t* pbOffset)
	{
		const MPTR samples = _swapEndianU32(pbOffset->samples);
		const uint16 format = _swapEndianU16(pbOffset->format);
		const uint32 cur = _swapEndianU32(pbOffset->currentOffset);
		const uint32 loop = _swapEndianU32(pbOffset->loopOffset);
		const uint32 end = _swapEndianU32(pbOffset->endOffset);
		cemuLog_log(LogType::SoundAPI, "AXSetVoiceOffsets: format {:04x} samples {:08x} cur {:08x} loop {:08x} end {:08x}", format, samples, cur, loop, end);
		if (samples == MPTR_NULL)
		{
			cemuLog_log(LogType::Force, "AXSetVoiceOffsets: no sample buffer");
			return;
		}
		vpb->offsets = *pbOffset;
		if (!IsKnownFormat(format))
		{
			cemuLog_log(LogType::Force, "AXSetVoiceOffsets: unknown format {:04x}", format);
			return;
		}
		const uint32 base = memory_virtualToPhysical(samples);
		const uint32 hiMask = (WordHighMask(format) << 16) | 0xFFFF;
		AXVPBInternal_t* pb = PpcBlock(vpb);
		AXVoiceAddr& a = pb->addr;
		a.format = format;
		a.loopFlag = _swapEndianU16(pbOffset->loopFlag);
		a.SetLoop(WordAddress(format, base, loop) & hiMask);
		a.SetEnd(WordAddress(format, base, end) & hiMask);
		a.SetCur(WordAddress(format, base, cur) & hiMask);
		a.addrExt = (uint16)AddressExtension(format, base, cur);
		if (format != AX_FORMAT_ADPCM)
		{
			// PCM goes through the same decoder stage with a fixed gain. The renderer
			// never reads it; it is kept in host order because that is the byte
			// pattern the block has always held.
			memset(&pb->adpcm, 0, sizeof(pb->adpcm));
			pb->adpcm.gain = (uint16)(format == AX_FORMAT_PCM8 ? 0x0100 : 0x0800);
			MarkDirty(vpb, VoiceDirty::Adpcm);
		}
		// the whole block supersedes single offset changes
		ClearDirty(vpb, VoiceDirty::LoopFlag | VoiceDirty::LoopOffset | VoiceDirty::EndOffset | VoiceDirty::CurOffset);
		Touched(vpb, VoiceDirty::Offsets);
	}

	void AXSetVoiceOffsetsEx(AXVPB* vpb, AXPBOFFSET_t* pbOffset, void* sampleBase)
	{
		if (!vpb || !sampleBase)
		{
			cemuLog_log(LogType::APIErrors, "AXSetVoiceOffsetsEx: null voice or sample base");
			return;
		}
		AXPBOFFSET_t withBase = *pbOffset;
		withBase.samples = _swapEndianU32(memory_getVirtualOffsetFromPointer(sampleBase));
		AXSetVoiceOffsets(vpb, &withBase);
	}

	// positions relative to vpb->offsets.samples, from the PPC copy (which the
	// frame thread keeps up to date with the playback position)
	void AXGetVoiceOffsets(AXVPB* vpb, AXPBOFFSET_t* pbOffset)
	{
		const AXVoiceAddr& a = PpcBlock(vpb)->addr;
		*pbOffset = vpb->offsets;
		const uint16 format = _swapEndianU16(pbOffset->format);
		if (IsKnownFormat(format))
		{
			const uint32 base = memory_virtualToPhysical(_swapEndianU32(vpb->offsets.samples));
			const uint32 ext = a.addrExt;
			pbOffset->loopOffset = _swapEndianU32(RelativeOffset(format, a.Loop(), ext, base));
			pbOffset->endOffset = _swapEndianU32(RelativeOffset(format, a.End(), ext, base));
			pbOffset->currentOffset = _swapEndianU32(RelativeOffset(format, a.Cur(), ext, base));
		}
		cemuLog_log(LogType::SoundAPI, "AXGetVoiceOffsets: voice {} samples {:08x} cur {:08x} loop {:08x} end {:08x}", (uint32)vpb->index,
			_swapEndianU32(pbOffset->samples), _swapEndianU32(pbOffset->currentOffset), _swapEndianU32(pbOffset->loopOffset), _swapEndianU32(pbOffset->endOffset));
	}

	void AXGetVoiceOffsetsEx(AXVPB* vpb, AXPBOFFSET_t* pbOffset, MPTR sampleBase)
	{
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		AXGetVoiceOffsets(vpb, pbOffset);
		vpb->offsets = *pbOffset;
	}

	// re-express the current positions against a new sample base (the voice keeps playing where it is)
	void AXSetVoiceSamplesAddr(AXVPB* vpb, void* sampleBase)
	{
		vpb->offsets.samples = _swapEndianU32(memory_getVirtualOffsetFromPointer(sampleBase));
		AXPBOFFSET_t rebased;
		AXGetVoiceOffsets(vpb, &rebased);
		vpb->offsets = rebased;
	}

	void AXSetVoiceCurrentOffset(AXVPB* vpb, uint32 currentOffset)
	{
		SetOneOffset(vpb, OffsetField::Cur, currentOffset);
	}

	void AXSetVoiceLoopOffset(AXVPB* vpb, uint32 loopOffset)
	{
		SetOneOffset(vpb, OffsetField::Loop, loopOffset);
	}

	void AXSetVoiceEndOffset(AXVPB* vpb, uint32 endOffset)
	{
		SetOneOffset(vpb, OffsetField::End, endOffset);
	}

	void AXSetVoiceCurrentOffsetEx(AXVPB* vpb, uint32 currentOffset, MPTR sampleBase)
	{
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		SetOneOffset(vpb, OffsetField::Cur, currentOffset);
	}

	void AXSetVoiceLoopOffsetEx(AXVPB* vpb, uint32 loopOffset, MPTR sampleBase)
	{
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		SetOneOffset(vpb, OffsetField::Loop, loopOffset);
	}

	void AXSetVoiceEndOffsetEx(AXVPB* vpb, uint32 endOffset, MPTR sampleBase)
	{
		vpb->offsets.samples = _swapEndianU32(sampleBase);
		SetOneOffset(vpb, OffsetField::End, endOffset);
	}

	uint32 AXGetVoiceCurrentOffsetEx(AXVPB* vpb, MPTR sampleBase)
	{
		AXPBOFFSET_t o;
		AXGetVoiceOffsetsEx(vpb, &o, sampleBase);
		return _swapEndianU32(o.currentOffset);
	}

	void AXSetVoiceLoop(AXVPB* vpb, uint16 loopState)
	{
		vpb->offsets.loopFlag = _swapEndianU16(loopState);
		PpcBlock(vpb)->addr.loopFlag = loopState;
		Touched(vpb, VoiceDirty::LoopFlag);
	}

	// The DSP does not count loops for us, so this infers them from how the
	// position moved since the previous call: a jump backwards into the loop
	// region is one loop, and a loop shorter than a frame wraps several times per frame.
	sint32 AXGetVoiceLoopCount(AXVPB* vpb)
	{
		if (!vpb)
			return 0;
		const uint32 index = vpb->index;
		const AXVPBInternal_t* pb = PpcBlock(vpb);
		const uint32 loop = pb->addr.Loop();
		const uint32 end = pb->addr.End();
		const uint32 cur = pb->addr.Cur();
		const uint32 ratio = pb->srcMode == AX_FILTER_MODE_NONE ? 0x10000u : pb->src.Ratio();
		const uint32 framePos = (uint32)AXGetInputSamplesPerFrame();
		LoopTracker& t = s_loops[index];
		const uint64 loopLen = ratio ? ((uint64)(end - loop) << 16) / ratio : 0;
		if (loop < end && loopLen != 0 && loopLen <= framePos)
		{
			t.loops += (uint32)(((t.lastPos - loop) + framePos) / (uint32)loopLen);
		}
		else if (t.lastPos <= end)
		{
			const bool jumpedIntoLoop = loop > end ? cur >= loop : (cur < t.lastPos && cur >= loop);
			if (jumpedIntoLoop)
				t.loops++;
		}
		t.lastPos = cur;
		return (sint32)t.loops;
	}
}
