// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Multi-channel voices: a "multi voice" is a group of up to six ordinary AX
// voices (one per channel of an interleaved/multichannel stream) that the
// title drives as a unit. Every setter here just forwards to each member.
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/MMU/MMU.h"

namespace snd_core
{
	namespace
	{
		constexpr sint32 kMaxGroupChannels = 6;

		// result codes of AXAcquireMultiVoice / AXGetMultiVoiceReformatBufferSize
		constexpr sint32 kErrNoFreeGroup = -20;
		constexpr sint32 kErrBadChannelCount = -21;
		constexpr sint32 kErrNoFreeVoice = -22;
		constexpr sint32 kErrBadFormat = -23;

		// the groups live in guest memory: titles receive pointers to them
		SysAllocator<AXVPBMULTI, AX_MAX_VOICES> s_groups;

		template<typename TFn>
		void ForEachMember(AXVPBMULTI* group, TFn&& fn)
		{
			const uint32 n = std::min<uint32>(group->channelCount, kMaxGroupChannels);
			for (uint32 ch = 0; ch < n; ch++)
				fn(ch, group->voice[ch].GetPtr());
		}
	}

	void AXMultiVoice_Init()
	{
		AXVPBMULTI* groups = s_groups.GetPtr();
		for (sint32 i = 0; i < AX_MAX_VOICES; i++)
		{
			groups[i].isUsed = 0;
			groups[i].channelCount = 0;
			for (auto& v : groups[i].voice)
				v = nullptr;
		}
	}

	// the 4th argument is the stream's DSPADPCM header; only its channel count matters here
	sint32 AXAcquireMultiVoice(sint32 voicePriority, void* cbFunc, void* cbData, AXMULTIVOICEUKNSTRUCT* uknR6, MEMPTR<AXVPBMULTI>* multiVoiceOut)
	{
		AXVPBMULTI* groups = s_groups.GetPtr();
		AXVPBMULTI* group = nullptr;
		for (sint32 i = 0; i < AX_MAX_VOICES && !group; i++)
		{
			if (groups[i].isUsed == 0u)
				group = groups + i;
		}
		if (!group)
			return kErrNoFreeGroup;

		const sint32 channels = uknR6->channelCount;
		if (channels < 1 || channels > kMaxGroupChannels)
			return kErrBadChannelCount;

		const MPTR callback = memory_getVirtualOffsetFromPointer(cbFunc);
		const MPTR context = memory_getVirtualOffsetFromPointer(cbData);
		// the count is set up front so a partial acquisition can be undone by AXFreeMultiVoice
		group->isUsed = 1;
		group->channelCount = channels;
		for (sint32 ch = 0; ch < kMaxGroupChannels; ch++)
			group->voice[ch] = nullptr;
		for (sint32 ch = 0; ch < channels; ch++)
		{
			AXVPB* voice = AXAcquireVoiceEx(voicePriority, callback, context);
			if (!voice)
			{
				AXFreeMultiVoice(group);
				return kErrNoFreeVoice;
			}
			group->voice[ch] = voice;
		}
		*multiVoiceOut = group;
		return 0;
	}

	void AXFreeMultiVoice(AXVPBMULTI* multiVoice)
	{
		if (multiVoice->isUsed == 0u)
			cemuLog_logDebug(LogType::Force, "AXFreeMultiVoice: group is not in use");
		for (uint32 ch = 0; ch < (uint32)kMaxGroupChannels; ch++)
		{
			AXVPB* voice = multiVoice->voice[ch].GetPtr();
			if (voice && ch < multiVoice->channelCount)
				AXFreeVoice(voice);
			multiVoice->voice[ch] = nullptr;
		}
		multiVoice->isUsed = 0;
	}

	// size of the buffer a title needs to de-interleave a multichannel stream of sizeInBytes
	sint32 AXGetMultiVoiceReformatBufferSize(sint32 voiceFormat, uint32 channelCount, uint32 sizeInBytes, uint32be* sizeOutput)
	{
		switch (voiceFormat)
		{
		case AX_FORMAT_ADPCM:
			// every channel's share starts on an 8-byte ADPCM frame boundary
			*sizeOutput = ((sizeInBytes + 7u) / 8u * 8u) * channelCount;
			return 0;
		case AX_FORMAT_PCM16:
			*sizeOutput = sizeInBytes;
			return 0;
		case AX_FORMAT_PCM8:
			// 8-bit input is widened to 16-bit
			*sizeOutput = sizeInBytes * 2u;
			return 0;
		default:
			return kErrBadFormat;
		}
	}

	void AXSetMultiVoiceType(AXVPBMULTI* mv, uint16 type)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceType(v, type); });
	}

	// one DSPADPCM header per channel; the decoder state is the coef..yn2 run of each
	void AXSetMultiVoiceAdpcm(AXVPBMULTI* mv, AXDSPADPCM* data)
	{
		static_assert(offsetof(AXDSPADPCM, gain) - offsetof(AXDSPADPCM, coef) == offsetof(AXPBADPCM_t, gain));
		static_assert(offsetof(AXDSPADPCM, scale) - offsetof(AXDSPADPCM, coef) == offsetof(AXPBADPCM_t, scale));
		static_assert(offsetof(AXDSPADPCM, yn1) - offsetof(AXDSPADPCM, coef) == offsetof(AXPBADPCM_t, yn1));
		static_assert(offsetof(AXDSPADPCM, yn2) - offsetof(AXDSPADPCM, coef) == offsetof(AXPBADPCM_t, yn2));
		static_assert(sizeof(AXPBADPCM_t) == 0x28);
		ForEachMember(mv, [&](uint32 ch, AXVPB* v) {
			// AXPBADPCM_t holds guest (big endian) halfwords, so this is a straight byte copy
			AXPBADPCM_t state;
			memcpy(&state, data[ch].coef, sizeof(state));
			AXSetVoiceAdpcm(v, &state);
		});
	}

	void AXSetMultiVoiceSrcType(AXVPBMULTI* mv, uint32 type)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceSrcType(v, type); });
	}

	void AXSetMultiVoiceOffsets(AXVPBMULTI* mv, AXPBOFFSET_t* offsets)
	{
		ForEachMember(mv, [&](uint32 ch, AXVPB* v) { AXSetVoiceOffsets(v, offsets + ch); });
	}

	void AXSetMultiVoiceVe(AXVPBMULTI* mv, AXPBVE* ve)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceVe(v, ve); });
	}

	void AXSetMultiVoiceSrcRatio(AXVPBMULTI* mv, float ratio)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceSrcRatio(v, ratio); });
	}

	void AXSetMultiVoiceSrc(AXVPBMULTI* mv, AXPBSRC_t* src)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceSrc(v, src); });
	}

	void AXSetMultiVoiceLoop(AXVPBMULTI* mv, uint16 loop)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceLoop(v, loop); });
	}

	void AXSetMultiVoiceState(AXVPBMULTI* mv, uint16 state)
	{
		ForEachMember(mv, [&](uint32, AXVPB* v) { AXSetVoiceState(v, state); });
	}

	void AXSetMultiVoiceAdpcmLoop(AXVPBMULTI* mv, AXPBADPCMLOOP_t* loops)
	{
		ForEachMember(mv, [&](uint32 ch, AXVPB* v) { AXSetVoiceAdpcmLoop(v, loops + ch); });
	}

	// all members start and stop together, so the first one speaks for the group
	sint32 AXIsMultiVoiceRunning(AXVPBMULTI* mv)
	{
		AXVPB* first = mv->voice[0].GetPtr();
		if (!first)
			return 0;
		return AXIsVoiceRunning(first);
	}
}
