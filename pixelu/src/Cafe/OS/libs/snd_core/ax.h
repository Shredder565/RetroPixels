// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Public side of the AX audio library (snd_core for older titles, sndcore2
// for newer ones). Titles allocate voices (AXVPB), point them at ADPCM or PCM
// sample data in their own memory and set volumes, filters and a resampling
// ratio. Every 3 ms the library renders one frame: each playing voice is
// decoded, resampled, filtered and mixed into the main and aux buses of the
// TV and the GamePad, and the result goes to the host audio device.
//
// Guest structures below are read and written by titles, so their layout is
// fixed. Several of them keep big-endian values in plain integer fields (the
// "raw" convention): code that reads them byte-swaps explicitly.
#pragma once

#include "util/helpers/fspinlock.h"
#include "Cafe/OS/RPL/COSModule.h"

struct PPCInterpreter_t;

namespace snd_core
{
	// --- configuration constants -------------------------------------------------

	// AXINITPARAM.freq
	const int AX_RENDERER_FREQ_32KHZ = 0;
	const int AX_RENDERER_FREQ_48KHZ = 1;
	// AXINITPARAM.frameLength
	const int AX_FRAMELENGTH_3MS = 0;
	// AXINITPARAM.pipelineMode
	const int AX_PIPELINE_SINGLE = 0;

	// samples per channel in one 3 ms frame
	const int AX_SAMPLES_PER_3MS_48KHZ = 48000 * 3 / 1000; // 144
	const int AX_SAMPLES_PER_3MS_32KHZ = 32000 * 3 / 1000; // 96
	const int AX_SAMPLES_MAX = AX_SAMPLES_PER_3MS_48KHZ;

	// output devices
	const int AX_DEV_TV = 0;
	const int AX_DEV_DRC = 1; // GamePad
	const int AX_DEV_RMT = 2; // Wii remote speakers
	const int AX_DEV_COUNT = 3;

	// channels each device can carry
	const int AX_TV_CHANNEL_COUNT = 6;
	const int AX_DRC_CHANNEL_COUNT = 4;
	const int AX_RMT_CHANNEL_COUNT = 1;

	// where 32 kHz output is converted to 48 kHz (AXSetDeviceUpsampleStage)
	const int AX_UPSAMPLE_STAGE_BEFORE_FINALMIX = 0;
	const int AX_UPSAMPLE_STAGE_AFTER_FINALMIX = 1;

	const int AX_APP_FRAME_CALLBACK_MAX = 64;

	// device modes (AXGetDeviceMode)
	const int AX_MODE_STEREO = 0;
	const int AX_MODE_SURROUND = 1;
	const int AX_MODE_DPL2 = 2;
	const int AX_MODE_6CH = 3;
	const int AX_MODE_MONO = 5;

	// voice priorities: 0 marks a free voice, 31 is never dropped
	const int AX_PRIORITY_MAX = 32;
	const int AX_PRIORITY_FREE = 0;
	const int AX_PRIORITY_NODROP = 31;
	const int AX_PRIORITY_LOWEST = 1;
	const int AX_MAX_VOICES = 96;

	// buses: main + aux A, B, C
	const int AX_AUX_BUS_COUNT = 3;
	const int AX_MAX_NUM_BUS = 4;

	// sample formats (AXPBOFFSET_t.format)
	const int AX_FORMAT_ADPCM = 0x0;
	const int AX_FORMAT_PCM16 = 0xA;
	const int AX_FORMAT_PCM8 = 0x19;

	const int AX_LPF_OFF = 0x0;
	const int AX_BIQUAD_OFF = 0x0;

	// AXSetVoiceSrcType
	const int AX_SRC_TYPE_NONE = 0x0;
	const int AX_SRC_TYPE_LINEAR = 0x1;
	const int AX_SRC_TYPE_LOWPASS1 = 0x2;
	const int AX_SRC_TYPE_LOWPASS2 = 0x3;
	const int AX_SRC_TYPE_LOWPASS3 = 0x4;

	// how the DSP resamples a voice (value kept in the voice block)
	const int AX_FILTER_MODE_TAP = 0x0;
	const int AX_FILTER_MODE_LINEAR = 0x1;
	const int AX_FILTER_MODE_NONE = 0x2;

	// tap filter selection for AX_FILTER_MODE_TAP
	const int AX_FILTER_LOWPASS_8K = 0x0;
	const int AX_FILTER_LOWPASS_12K = 0x1;
	const int AX_FILTER_LOWPASS_16K = 0x2;

	// AXSetDefaultMixerSelect / AXGetDefaultMixerSelect
	const int AX_MIXER_SELECT_DSP = 0;
	const int AX_MIXER_SELECT_PPC = 1;
	const int AX_MIXER_SELECT_BOTH = 2;

	// --- guest structures -------------------------------------------------------

	struct AXINITPARAM
	{
		/* +0x00 */ uint32be freq;         // AX_RENDERER_FREQ_*
		/* +0x04 */ uint32be frameLength;  // AX_FRAMELENGTH_*
		/* +0x08 */ uint32be pipelineMode; // AX_PIPELINE_*
	};
	static_assert(sizeof(AXINITPARAM) == 0x0C);

	// passed to aux effect callbacks
	struct AXAUXCBCHANNELINFO
	{
		/* +0x00 */ uint32be numChannels;
		/* +0x04 */ uint32be numSamples;
	};
	static_assert(sizeof(AXAUXCBCHANNELINFO) == 0x08);

	// sample buffer description (raw convention). The offsets count samples
	// (PCM8/PCM16) or nibbles (ADPCM) from 'samples'; end is inclusive.
	struct AXPBOFFSET_t
	{
		/* +0x00 */ uint16 format;
		/* +0x02 */ uint16 loopFlag;
		/* +0x04 */ uint32 loopOffset;
		/* +0x08 */ uint32 endOffset;
		/* +0x0C */ uint32 currentOffset;
		/* +0x10 */ MPTR   samples;
	};
	static_assert(sizeof(AXPBOFFSET_t) == 0x14);

	// volume envelope: 1.15 fixed point volume, per-sample delta
	struct AXPBVE
	{
		/* +0x00 */ uint16be currentVolume;
		/* +0x02 */ sint16be currentDelta;
	};
	static_assert(sizeof(AXPBVE) == 0x04);

	// initial time delay state, owned by the DSP
	struct AXVPBItd
	{
		uint8 data[64];
	};
	static_assert(sizeof(AXVPBItd) == 0x40);

	// the voice handle titles get from AXAcquireVoice
	struct AXVPB
	{
		/* +0x00 */ uint32be          index;
		/* +0x04 */ uint32be          playbackState; // 0 stopped, 1 running
		/* +0x08 */ uint32be          reserved08;
		/* +0x0C */ uint32be          mixerSelect;
		/* +0x10 */ MEMPTR<AXVPB>     next;
		/* +0x14 */ MEMPTR<AXVPB>     prev;
		/* +0x18 */ uint32be          reserved18;
		/* +0x1C */ uint32be          priority;
		/* +0x20 */ uint32be          callback;      // drop callback (AXAcquireVoice)
		/* +0x24 */ uint32be          userParam;
		/* +0x28 */ uint32be          sync;          // parameters changed since the last frame
		/* +0x2C */ uint32be          depop;
		/* +0x30 */ MEMPTR<AXVPBItd>  itd;
		/* +0x34 */ AXPBOFFSET_t      offsets;
		/* +0x48 */ uint32be          callbackEx;    // drop callback (AXAcquireVoiceEx)
		/* +0x4C */ uint32be          dropReason;
		/* +0x50 */ float32be         dspLoad;
		/* +0x54 */ float32be         ppcLoad;
	};
	static_assert(offsetof(AXVPB, playbackState) == 0x04);
	static_assert(offsetof(AXVPB, mixerSelect) == 0x0C);
	static_assert(offsetof(AXVPB, priority) == 0x1C);
	static_assert(offsetof(AXVPB, callback) == 0x20);
	static_assert(offsetof(AXVPB, sync) == 0x28);
	static_assert(offsetof(AXVPB, itd) == 0x30);
	static_assert(offsetof(AXVPB, offsets) == 0x34);
	static_assert(offsetof(AXVPB, callbackEx) == 0x48);
	static_assert(offsetof(AXVPB, dspLoad) == 0x50);
	static_assert(sizeof(AXVPB) == 0x58);

	// one-pole low pass filter (raw convention)
	struct AXPBLPF_t
	{
		/* +0x00 */ uint16 on;
		/* +0x02 */ sint16 yn1;
		/* +0x04 */ sint16 a0;
		/* +0x06 */ sint16 b0;
	};
	static_assert(sizeof(AXPBLPF_t) == 0x08);

	// biquad filter, 2.14 fixed point coefficients (raw convention)
	struct AXPBBIQUAD_t
	{
		/* +0x00 */ uint16 on;
		/* +0x02 */ sint16 xn1;
		/* +0x04 */ sint16 xn2;
		/* +0x06 */ sint16 yn1;
		/* +0x08 */ sint16 yn2;
		/* +0x0A */ uint16 b0;
		/* +0x0C */ uint16 b1;
		/* +0x0E */ uint16 b2;
		/* +0x10 */ uint16 a1;
		/* +0x12 */ uint16 a2;
	};
	static_assert(sizeof(AXPBBIQUAD_t) == 0x14);

	// ADPCM decoder state: 8 coefficient pairs, gain, predictor/scale, history (raw convention)
	struct AXPBADPCM_t
	{
		/* +0x00 */ uint16 a[16];
		/* +0x20 */ uint16 gain;
		/* +0x22 */ uint16 scale;
		/* +0x24 */ uint16 yn1;
		/* +0x26 */ uint16 yn2;
	};
	static_assert(sizeof(AXPBADPCM_t) == 0x28);

	// decoder state to use when playback jumps to the loop start (raw convention)
	struct AXPBADPCMLOOP_t
	{
		/* +0x00 */ uint16 loopScale;
		/* +0x02 */ uint16 loopYn1;
		/* +0x04 */ uint16 loopYn2;
	};
	static_assert(sizeof(AXPBADPCMLOOP_t) == 0x06);

	// resampler state: 16.16 ratio, fractional position, last input samples (raw convention)
	struct AXPBSRC_t
	{
		/* +0x00 */ uint16 ratioHigh;
		/* +0x02 */ uint16 ratioLow;
		/* +0x04 */ uint16 currentFrac;
		/* +0x06 */ uint16 historySamples[4];

		uint32 Ratio() const
		{
			return ((uint32)_swapEndianU16(ratioHigh) << 16) | (uint32)_swapEndianU16(ratioLow);
		}
	};
	static_assert(sizeof(AXPBSRC_t) == 0x0E);

	// one bus/channel volume of a device mix (raw convention)
	struct AXCHMIX_DEPR
	{
		uint16 vol;
		sint16 delta;
	};
	static_assert(sizeof(AXCHMIX_DEPR) == 0x04);

	// the same with typed big-endian fields
	struct AXCHMIX2
	{
		uint16be vol;
		sint16be delta;
	};
	static_assert(sizeof(AXCHMIX2) == 0x04);

	// a group of voices playing the channels of one multichannel stream
	struct AXVPBMULTI
	{
		/* +0x00 */ betype<uint32> isUsed;
		/* +0x04 */ betype<uint32> channelCount;
		/* +0x08 */ MEMPTR<AXVPB> voice[6];
	};
	static_assert(sizeof(AXVPBMULTI) == 0x20);

	// the part of a DSPADPCM header AXAcquireMultiVoice looks at
	struct AXMULTIVOICEUKNSTRUCT
	{
		uint8 ukn[0x4A];
		betype<sint16> channelCount;
	};
	static_assert(offsetof(AXMULTIVOICEUKNSTRUCT, channelCount) == 0x4A);

	// DSPADPCM stream header (one per channel)
	struct AXDSPADPCM
	{
		/* +0x00 */ uint32be numSamples;
		/* +0x04 */ uint32be numNibbles;
		/* +0x08 */ uint32be sampleRate;
		/* +0x0C */ uint16be isLooped;
		/* +0x0E */ uint16be format;
		/* +0x10 */ uint32be loopStart;
		/* +0x14 */ uint32be loopEnd;
		/* +0x18 */ uint32be currentAddress;
		/* +0x1C */ uint16be coef[16];
		/* +0x3C */ uint16be gain;
		/* +0x3E */ uint16be scale;
		/* +0x40 */ uint16be yn1;
		/* +0x42 */ uint16be yn2;
		/* +0x44 */ uint16be loopScale;
		/* +0x46 */ uint16be loopYn1;
		/* +0x48 */ uint16be loopYn2;
		/* +0x4A */ uint16be channelCount;
		/* +0x4C */ uint16be blockSize;
		/* +0x4E */ uint8 _padding4E[0x12];
	};
	static_assert(offsetof(AXDSPADPCM, coef) == 0x1C);
	static_assert(offsetof(AXDSPADPCM, channelCount) == 0x4A);
	static_assert(sizeof(AXDSPADPCM) == 0x60);

	// --- library state ----------------------------------------------------------

	bool isInitialized();
	bool IsSndCore2();
	void reset(); // emulator shutdown: drop host devices and all state

	COSModule* GetModuleSndCore1();
	COSModule* GetModuleSndCore2();

	sint32 AXGetInputSamplesPerFrame();
	sint32 AXGetInputSamplesPerSec();
	sint32 AXIsValidDevice(sint32 device, sint32 deviceIndex);
	sint32 AXGetDeviceMode(sint32 device);

	// --- voices (ax_voice.cpp) ----------------------------------------------------

	AXVPB* AXAcquireVoiceEx(uint32 priority, MPTR callbackEx, MPTR userParam);
	void AXFreeVoice(AXVPB* vpb);

	bool AXUserIsProtected();
	sint32 AXUserBegin();
	sint32 AXUserEnd();
	sint32 AXVoiceBegin(AXVPB* voice);
	sint32 AXVoiceEnd(AXVPB* voice);

	sint32 AXSetVoiceDeviceMix(AXVPB* vpb, sint32 device, sint32 deviceIndex, AXCHMIX_DEPR* mix);
	void AXSetVoiceState(AXVPB* vpb, sint32 voiceState);
	sint32 AXIsVoiceRunning(AXVPB* vpb);
	void AXSetVoiceType(AXVPB* vpb, uint16 voiceType);
	void AXSetVoiceAdpcm(AXVPB* vpb, AXPBADPCM_t* adpcm);
	void AXSetVoiceAdpcmLoop(AXVPB* vpb, AXPBADPCMLOOP_t* adpcmLoop);
	void AXSetVoiceSrc(AXVPB* vpb, AXPBSRC_t* src);
	void AXSetVoiceSrcType(AXVPB* vpb, uint32 srcType);
	sint32 AXSetVoiceSrcRatio(AXVPB* vpb, float ratio);
	void AXSetVoiceVe(AXVPB* vpb, AXPBVE* ve);
	void AXComputeLpfCoefs(uint32 freq, uint16be* a0, uint16be* b0);
	void AXSetVoiceLpf(AXVPB* vpb, AXPBLPF_t* lpf);
	void AXSetVoiceLpfCoefs(AXVPB* vpb, uint16 a0, uint16 b0);
	void AXSetVoiceBiquad(AXVPB* vpb, AXPBBIQUAD_t* biquad);
	void AXSetVoiceBiquadCoefs(AXVPB* vpb, uint16 b0, uint16 b1, uint16 b2, uint16 a1, uint16 a2);
	void AXSetVoiceOffsets(AXVPB* vpb, AXPBOFFSET_t* pbOffset);
	void AXSetVoiceOffsetsEx(AXVPB* vpb, AXPBOFFSET_t* pbOffset, void* sampleBase);
	void AXGetVoiceOffsets(AXVPB* vpb, AXPBOFFSET_t* pbOffset);
	void AXGetVoiceOffsetsEx(AXVPB* vpb, AXPBOFFSET_t* pbOffset, MPTR sampleBase);
	void AXSetVoiceSamplesAddr(AXVPB* vpb, void* sampleBase);
	void AXSetVoiceCurrentOffset(AXVPB* vpb, uint32 currentOffset);
	void AXSetVoiceLoopOffset(AXVPB* vpb, uint32 loopOffset);
	void AXSetVoiceEndOffset(AXVPB* vpb, uint32 endOffset);
	void AXSetVoiceCurrentOffsetEx(AXVPB* vpb, uint32 currentOffset, MPTR sampleBase);
	void AXSetVoiceLoopOffsetEx(AXVPB* vpb, uint32 loopOffset, MPTR sampleBase);
	void AXSetVoiceEndOffsetEx(AXVPB* vpb, uint32 endOffset, MPTR sampleBase);
	uint32 AXGetVoiceCurrentOffsetEx(AXVPB* vpb, MPTR sampleBase);
	void AXSetVoiceLoop(AXVPB* vpb, uint16 loopState);
	sint32 AXGetVoiceLoopCount(AXVPB* vpb);

	// --- frame callbacks, final mix, remix (ax_ist.cpp) ---------------------------

	sint32 AXSetDeviceUpsampleStage(sint32 device, int upsampleStage);
	sint32 AXGetDeviceUpsampleStage(sint32 device, uint32be* upsampleStage);
	sint32 AXGetDeviceFinalMixCallback(sint32 device, uint32be* funcAddrPtr);
	sint32 AXRegisterDeviceFinalMixCallback(sint32 device, MPTR funcAddr);
	sint32 AXSetDeviceRemixMatrix(sint32 deviceId, uint32 inputChannelCount, uint32 outputChannelCount, const MEMPTR<float32be>& matrix);
	sint32 AXGetDeviceRemixMatrix(uint32 deviceId, uint32 inputChannelCount, uint32 outputChannelCount, MEMPTR<MEMPTR<float32be>>& matrix);
	sint32 AXRegisterAppFrameCallback(MPTR funcAddr);
	sint32 AXDeregisterAppFrameCallback(MPTR funcAddr);
	MPTR AXRegisterFrameCallback(MPTR funcAddr);

	// --- mixer (ax_mix.cpp) -------------------------------------------------------

	void AXSetDefaultMixerSelect(uint32 mixerSelect);
	uint32 AXGetDefaultMixerSelect();

	// per aux bus return volume of the TV mix (1.15 fixed point)
	extern uint16 __AXTVAuxReturnVolume[AX_AUX_BUS_COUNT];

	// --- aux buses (ax_aux.cpp) ---------------------------------------------------

	void AXAux_Init();
	void AXAux_Process();
	sint32be* AXAux_GetInputBuffer(sint32 device, sint32 deviceIndex, sint32 auxBus);
	sint32be* AXAux_GetOutputBuffer(sint32 device, sint32 deviceIndex, sint32 auxBus);
	sint32 AXGetAuxCallback(sint32 device, sint32 deviceIndex, uint32 auxBusIndex, MEMPTR<uint32be> funcPtrOut, MEMPTR<uint32be> contextPtrOut);
	sint32 AXRegisterAuxCallback(sint32 device, sint32 deviceIndex, uint32 auxBusIndex, MPTR funcMPTR, MPTR userParam);
	sint32 AXSetAuxReturnVolume(uint32 device, uint32 deviceIndex, uint32 auxBus, uint16 volume);

	// --- multichannel voices (ax_multivoice.cpp) ----------------------------------

	void AXMultiVoice_Init();
	sint32 AXAcquireMultiVoice(sint32 voicePriority, void* cbFunc, void* cbData, AXMULTIVOICEUKNSTRUCT* uknR6, MEMPTR<AXVPBMULTI>* multiVoiceOut);
	void AXFreeMultiVoice(AXVPBMULTI* multiVoice);
	sint32 AXGetMultiVoiceReformatBufferSize(sint32 voiceFormat, uint32 channelCount, uint32 sizeInBytes, uint32be* sizeOutput);
	void AXSetMultiVoiceType(AXVPBMULTI* mv, uint16 type);
	void AXSetMultiVoiceAdpcm(AXVPBMULTI* mv, AXDSPADPCM* data);
	void AXSetMultiVoiceSrcType(AXVPBMULTI* mv, uint32 type);
	void AXSetMultiVoiceOffsets(AXVPBMULTI* mv, AXPBOFFSET_t* offsets);
	void AXSetMultiVoiceVe(AXVPBMULTI* mv, AXPBVE* ve);
	void AXSetMultiVoiceSrcRatio(AXVPBMULTI* mv, float ratio);
	void AXSetMultiVoiceSrc(AXVPBMULTI* mv, AXPBSRC_t* src);
	void AXSetMultiVoiceLoop(AXVPBMULTI* mv, uint16 loop);
	void AXSetMultiVoiceState(AXVPBMULTI* mv, uint16 state);
	void AXSetMultiVoiceAdpcmLoop(AXVPBMULTI* mv, AXPBADPCMLOOP_t* loops);
	sint32 AXIsMultiVoiceRunning(AXVPBMULTI* mv);

	// --- host output (ax_out.cpp) -------------------------------------------------

	void AXOut_init();   // emulator boot: open the host TV and GamePad devices
	void AXOut_reset();  // emulator shutdown: close them
	void AXOut_update(); // called by the scheduler; queues a frame every 3 ms
}
