// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// AUX effect buses. Each output device has three aux buses (A, B, C) that a
// title can attach an effect callback to (reverb, chorus, ...). The mixer
// writes a bus's send signal into the "input" half of a double buffer; one
// frame later the title's callback processes it in place and the mixer adds
// the result ("output" half) back into the main mix. A bus with no callback
// has no buffer and is skipped by the mixer.
//
// Buffer layout seen by the callback: one sint32be block per channel, the
// channel blocks AXGetInputSamplesPerFrame() samples apart, so the mixer can
// treat a bus as one contiguous run of channels * samples values.
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/Espresso/PPCCallback.h"

namespace snd_core
{
	namespace
	{
		constexpr sint32 kDrcCount = 2;
		constexpr sint32 kErrBadBus = -5;
		constexpr uint16 kUnityReturnVolume = 0x8000;

		template<sint32 TChannels>
		struct BusFrames
		{
			// [frame half][bus][channel][sample]
			sint32be samples[2][AX_AUX_BUS_COUNT][TChannels * AX_SAMPLES_MAX];

			sint32be* Bus(uint32 half, uint32 bus) { return samples[half][bus]; }
		};
		using TvBusFrames = BusFrames<AX_TV_CHANNEL_COUNT>;
		using DrcBusFrames = BusFrames<AX_DRC_CHANNEL_COUNT>;

		struct Effect
		{
			MPTR func = MPTR_NULL;
			MPTR context = MPTR_NULL;
		};

		struct AuxState
		{
			Effect tv[AX_AUX_BUS_COUNT];
			Effect drc[kDrcCount][AX_AUX_BUS_COUNT];
			uint32 inputHalf = 0; // the half the mixer is filling this frame
		};
		AuxState s_aux;

		// guest-visible: the callback receives these
		SysAllocator<TvBusFrames> s_tvFrames;
		SysAllocator<DrcBusFrames, kDrcCount> s_drcFrames;
		SysAllocator<MEMPTR<sint32be>, AX_TV_CHANNEL_COUNT> s_cbChannelPtrs;
		SysAllocator<AXAUXCBCHANNELINFO> s_cbInfo;

		uint32 ProcessedHalf()
		{
			return s_aux.inputHalf ^ 1u;
		}

		// the effect slot for a device/bus, or nullptr when there is none
		Effect* FindEffect(sint32 device, sint32 deviceIndex, uint32 bus)
		{
			if (bus >= (uint32)AX_AUX_BUS_COUNT)
				return nullptr;
			if (device == AX_DEV_TV && deviceIndex == 0)
				return &s_aux.tv[bus];
			if (device == AX_DEV_DRC && deviceIndex >= 0 && deviceIndex < kDrcCount)
				return &s_aux.drc[deviceIndex][bus];
			return nullptr;
		}

		sint32be* BusBuffer(sint32 device, sint32 deviceIndex, sint32 bus, uint32 half)
		{
			if (bus < 0)
				return nullptr;
			Effect* fx = FindEffect(device, deviceIndex, (uint32)bus);
			if (!fx || fx->func == MPTR_NULL)
				return nullptr;
			if (device == AX_DEV_TV)
				return s_tvFrames->Bus(half, bus);
			return s_drcFrames[deviceIndex].Bus(half, bus);
		}

		template<sint32 TChannels>
		void RunEffect(const Effect& fx, sint32be* bus, uint32 samplesPerChannel)
		{
			if (fx.func == MPTR_NULL)
			{
				// nothing will return from this bus; keep the half clean for reuse
				memset(bus, 0, sizeof(sint32be) * TChannels * samplesPerChannel);
				return;
			}
			for (sint32 ch = 0; ch < TChannels; ch++)
				s_cbChannelPtrs[ch] = bus + ch * samplesPerChannel;
			s_cbInfo->numChannels = TChannels;
			s_cbInfo->numSamples = samplesPerChannel;
			// void callback(sint32be** channels, void* context, AXAUXCBCHANNELINFO* info)
			PPCCoreCallback(fx.func, s_cbChannelPtrs.GetMPTR(), fx.context, s_cbInfo.GetMPTR());
		}
	}

	void AXAux_Init()
	{
		s_aux = AuxState{};
		memset(s_tvFrames.GetPtr(), 0, sizeof(TvBusFrames));
		memset(s_drcFrames.GetPtr(), 0, sizeof(DrcBusFrames) * kDrcCount);
		for (auto& v : __AXTVAuxReturnVolume)
			v = kUnityReturnVolume;
	}

	sint32be* AXAux_GetInputBuffer(sint32 device, sint32 deviceIndex, sint32 auxBus)
	{
		return BusBuffer(device, deviceIndex, auxBus, s_aux.inputHalf);
	}

	sint32be* AXAux_GetOutputBuffer(sint32 device, sint32 deviceIndex, sint32 auxBus)
	{
		return BusBuffer(device, deviceIndex, auxBus, ProcessedHalf());
	}

	sint32 AXRegisterAuxCallback(sint32 device, sint32 deviceIndex, uint32 auxBusIndex, MPTR funcMPTR, MPTR userParam)
	{
		if (sint32 r = AXIsValidDevice(device, deviceIndex); r != 0)
			return r;
		if (auxBusIndex >= (uint32)AX_AUX_BUS_COUNT)
			return kErrBadBus;
		Effect* fx = FindEffect(device, deviceIndex, auxBusIndex);
		if (!fx)
		{
			// Wii remote speakers have no aux buses here; accept and ignore
			cemuLog_logDebug(LogType::Force, "AXRegisterAuxCallback: device {} has no aux buses", device);
			return 0;
		}
		fx->func = funcMPTR;
		fx->context = userParam;
		return 0;
	}

	sint32 AXGetAuxCallback(sint32 device, sint32 deviceIndex, uint32 auxBusIndex, MEMPTR<uint32be> funcPtrOut, MEMPTR<uint32be> contextPtrOut)
	{
		if (sint32 r = AXIsValidDevice(device, deviceIndex); r != 0)
			return r;
		if (auxBusIndex >= (uint32)AX_AUX_BUS_COUNT)
			return kErrBadBus;
		const Effect* fx = FindEffect(device, deviceIndex, auxBusIndex);
		*funcPtrOut = fx ? fx->func : MPTR_NULL;
		*contextPtrOut = fx ? fx->context : MPTR_NULL;
		return 0;
	}

	// called by the mixer once per frame, before it fills the next input half
	void AXAux_Process()
	{
		const uint32 half = ProcessedHalf();
		const uint32 samplesPerChannel = AXGetInputSamplesPerFrame();
		for (sint32 bus = 0; bus < AX_AUX_BUS_COUNT; bus++)
			RunEffect<AX_TV_CHANNEL_COUNT>(s_aux.tv[bus], s_tvFrames->Bus(half, bus), samplesPerChannel);
		for (sint32 drc = 0; drc < kDrcCount; drc++)
		{
			for (sint32 bus = 0; bus < AX_AUX_BUS_COUNT; bus++)
				RunEffect<AX_DRC_CHANNEL_COUNT>(s_aux.drc[drc][bus], s_drcFrames[drc].Bus(half, bus), samplesPerChannel);
		}
	}

	void AXAux_incrementBufferIndex()
	{
		s_aux.inputHalf ^= 1u;
	}

	sint32 AXSetAuxReturnVolume(uint32 device, uint32 deviceIndex, uint32 auxBus, uint16 volume)
	{
		if (sint32 r = AXIsValidDevice(device, deviceIndex); r != 0)
			return r;
		if (auxBus >= (uint32)AX_AUX_BUS_COUNT)
			return kErrBadBus;
		if (device != AX_DEV_TV)
		{
			// only the TV mix applies a return volume
			cemuLog_logDebug(LogType::Force, "AXSetAuxReturnVolume: device {} not supported", device);
			return 0;
		}
		__AXTVAuxReturnVolume[auxBus] = volume;
		return 0;
	}
}
