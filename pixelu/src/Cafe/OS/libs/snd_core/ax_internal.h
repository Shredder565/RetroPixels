// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Shared state of the AX implementation files.
//
// Every voice has two copies of its DSP parameter block (AXVPBInternal_t):
// the "PPC copy" that the AXSetVoice* functions write, and the "DSP copy"
// that the renderer reads and updates while it plays the voice. At the start
// of each frame the frame thread exchanges the two (ax_ist.cpp): parameters
// the title changed (AXVPB.sync bits) go PPC -> DSP, playback progress
// (position, decoder history, envelope) comes back DSP -> PPC.
#pragma once
#include "Cafe/OS/libs/snd_core/ax.h"

namespace snd_core
{
	struct sndGeneric_t
	{
		bool isInitialized{false};
		bool isSoundCore2{false};
		struct
		{
			uint32 rendererFreq{0}; // AX_RENDERER_FREQ_*
			uint32 frameLength{0};  // AX_FRAMELENGTH_*
			uint32 pipelineMode{0};
		}initParam;
	};
	extern sndGeneric_t sndGeneric;

	// AXVPB.sync: which parameter groups the title changed since the last frame
	namespace VoiceDirty
	{
		constexpr uint32 SrcType      = 1u << 0;  // AXSetVoiceSrcType
		constexpr uint32 MixMask      = 1u << 1;  // AXSetVoiceDeviceMix
		constexpr uint32 State        = 1u << 2;  // AXSetVoiceState
		constexpr uint32 Type         = 1u << 3;  // AXSetVoiceType
		constexpr uint32 Mix          = 1u << 4;  // AXSetVoiceDeviceMix
		constexpr uint32 ItdOn        = 1u << 5;
		constexpr uint32 ItdTarget    = 1u << 6;
		constexpr uint32 Ve           = 1u << 8;  // AXSetVoiceVe
		constexpr uint32 VeDelta      = 1u << 9;
		constexpr uint32 Offsets      = 1u << 10; // AXSetVoiceOffsets
		constexpr uint32 LoopFlag     = 1u << 11; // AXSetVoiceLoop
		constexpr uint32 LoopOffset   = 1u << 12; // AXSetVoiceLoopOffset
		constexpr uint32 EndOffset    = 1u << 13; // AXSetVoiceEndOffset
		constexpr uint32 CurOffset    = 1u << 14; // AXSetVoiceCurrentOffset
		constexpr uint32 Adpcm        = 1u << 15; // AXSetVoiceAdpcm
		constexpr uint32 Src          = 1u << 16; // AXSetVoiceSrc
		constexpr uint32 SrcRatio     = 1u << 17; // AXSetVoiceSrcRatio
		constexpr uint32 AdpcmLoop    = 1u << 18; // AXSetVoiceAdpcmLoop
		constexpr uint32 Lpf          = 1u << 19; // AXSetVoiceLpf
		constexpr uint32 LpfCoefs     = 1u << 20; // AXSetVoiceLpfCoefs
		constexpr uint32 Biquad       = 1u << 21; // AXSetVoiceBiquad
		constexpr uint32 BiquadCoefs  = 1u << 22; // AXSetVoiceBiquadCoefs
		constexpr uint32 RmtOn        = 1u << 23;
		constexpr uint32 RmtSrc       = 1u << 27;
		constexpr uint32 RmtIir       = 1u << 28;
	}

	// sample position block. Addresses are absolute "sample word" addresses:
	// bytes for PCM8, halfwords for PCM16, nibbles for ADPCM. 'addrExt'
	// holds bits 29-31 of the byte address the 29-bit fields cannot carry.
	struct AXVoiceAddr
	{
		/* +0x00 */ uint16be loopFlag;
		/* +0x02 */ uint16be format;
		/* +0x04 */ uint16be addrExt;
		/* +0x06 */ uint16be loopHi;
		/* +0x08 */ uint16be loopLo;
		/* +0x0A */ uint16be endHi;
		/* +0x0C */ uint16be endLo;
		/* +0x0E */ uint16be curHi;
		/* +0x10 */ uint16be curLo;

		static uint32 Join(const uint16be& hi, const uint16be& lo) { return ((uint32)hi.value() << 16) | lo.value(); }
		static void Split(uint16be& hi, uint16be& lo, uint32 v) { hi = (uint16)(v >> 16); lo = (uint16)v; }

		uint32 Loop() const { return Join(loopHi, loopLo); }
		uint32 End() const { return Join(endHi, endLo); }
		uint32 Cur() const { return Join(curHi, curLo); }
		void SetLoop(uint32 v) { Split(loopHi, loopLo, v); }
		void SetEnd(uint32 v) { Split(endHi, endLo, v); }
		void SetCur(uint32 v) { Split(curHi, curLo, v); }
	};
	static_assert(sizeof(AXVoiceAddr) == 0x12);

	// one voice parameter block as the DSP sees it. Fields the audio debugger
	// window reads (playbackState, src, lpf, biquad, deviceMixTV) use the raw
	// convention, the rest are typed big-endian.
	struct AXVPBInternal_t
	{
		/* +0x000 */ uint16be nextHi;          // physical address of the next DSP block in the play list
		/* +0x002 */ uint16be nextLo;
		/* +0x004 */ uint16be selfHi;          // physical address of this voice's DSP block
		/* +0x006 */ uint16be selfLo;
		/* +0x008 */ uint16be srcMode;         // AX_FILTER_MODE_*
		/* +0x00A */ uint16be srcTap;          // AX_FILTER_LOWPASS_*
		/* +0x00C */ uint16be mixerSelect;
		/* +0x00E */ uint16be voiceType;
		/* +0x010 */ uint16be mixMaskTV[AX_MAX_NUM_BUS];       // per bus: 2 bits per channel (1 = volume, 3 = ramping)
		/* +0x018 */ uint16be mixMaskDRC[2 * AX_MAX_NUM_BUS];
		/* +0x028 */ AXCHMIX_DEPR deviceMixTV[AX_TV_CHANNEL_COUNT * AX_MAX_NUM_BUS];       // [channel][bus]
		/* +0x088 */ AXCHMIX_DEPR deviceMixDRC[2 * AX_DRC_CHANNEL_COUNT * AX_MAX_NUM_BUS]; // [gamepad][channel][bus]
		/* +0x108 */ AXCHMIX_DEPR deviceMixRMT[16];
		/* +0x148 */ uint16be rmtOn;
		/* +0x14A */ uint16be mixMaskRMT[16];
		/* +0x16A */ uint16 playbackState;
		/* +0x16C */ uint16be itdOn;
		/* +0x16E */ uint16be itdAddrHi;
		/* +0x170 */ uint16be itdAddrLo;
		/* +0x172 */ uint16be itdState[4];
		/* +0x17A */ uint16be veVolume;
		/* +0x17C */ uint16be veDelta;
		/* +0x17E */ AXVoiceAddr addr;
		/* +0x190 */ AXPBADPCM_t adpcm;
		/* +0x1B8 */ AXPBSRC_t src;
		/* +0x1C6 */ AXPBADPCMLOOP_t adpcmLoop;
		/* +0x1CC */ AXPBLPF_t lpf;
		/* +0x1D4 */ AXPBBIQUAD_t biquad;
		/* +0x1E8 */ uint16 tvVolumeNow[AX_MAX_NUM_BUS * AX_TV_CHANNEL_COUNT]; // TV volume after this frame's ramps
		/* +0x218 */ uint16 reserved218[0x20];
		/* +0x258 */ uint16 reserved258[0x10];
		/* +0x278 */ uint16be rmtSrcState[6];
		/* +0x284 */ uint32 reserved284[4];
		/* +0x294 */ uint16 reserved294[6];
		/* +0x2A0 */ uint16be index;
		/* +0x2A2 */ uint16be listState;        // 1 = in this frame's play list, 2 = idle
		/* +0x2A4 */ uint16 reserved2A4[4];
		/* +0x2AC */ MEMPTR<AXVPBInternal_t> nextToProcess; // host-side play list (DSP copies)
		/* +0x2B0 */ uint32 reserved2B0[4];
	};
	static_assert(offsetof(AXVPBInternal_t, srcMode) == 0x008);
	static_assert(offsetof(AXVPBInternal_t, mixMaskTV) == 0x010);
	static_assert(offsetof(AXVPBInternal_t, mixMaskDRC) == 0x018);
	static_assert(offsetof(AXVPBInternal_t, deviceMixTV) == 0x028);
	static_assert(offsetof(AXVPBInternal_t, deviceMixDRC) == 0x088);
	static_assert(offsetof(AXVPBInternal_t, deviceMixRMT) == 0x108);
	static_assert(offsetof(AXVPBInternal_t, rmtOn) == 0x148);
	static_assert(offsetof(AXVPBInternal_t, mixMaskRMT) == 0x14A);
	static_assert(offsetof(AXVPBInternal_t, playbackState) == 0x16A);
	static_assert(offsetof(AXVPBInternal_t, itdAddrHi) == 0x16E);
	static_assert(offsetof(AXVPBInternal_t, itdState) == 0x172);
	static_assert(offsetof(AXVPBInternal_t, veVolume) == 0x17A);
	static_assert(offsetof(AXVPBInternal_t, addr) == 0x17E);
	static_assert(offsetof(AXVPBInternal_t, adpcm) == 0x190);
	static_assert(offsetof(AXVPBInternal_t, src) == 0x1B8);
	static_assert(offsetof(AXVPBInternal_t, adpcmLoop) == 0x1C6);
	static_assert(offsetof(AXVPBInternal_t, lpf) == 0x1CC);
	static_assert(offsetof(AXVPBInternal_t, biquad) == 0x1D4);
	static_assert(offsetof(AXVPBInternal_t, tvVolumeNow) == 0x1E8);
	static_assert(offsetof(AXVPBInternal_t, reserved218) == 0x218);
	static_assert(offsetof(AXVPBInternal_t, reserved258) == 0x258);
	static_assert(offsetof(AXVPBInternal_t, rmtSrcState) == 0x278);
	static_assert(offsetof(AXVPBInternal_t, index) == 0x2A0);
	static_assert(offsetof(AXVPBInternal_t, listState) == 0x2A2);
	static_assert(offsetof(AXVPBInternal_t, nextToProcess) == 0x2AC);
	static_assert(sizeof(AXVPBInternal_t) == 0x2C0);

	// voice storage (guest memory). __AXVPBInternalVoiceArray holds the PPC
	// copies, the DSP copies are reached through DspBlock().
	extern AXVPBInternal_t* __AXVPBInternalVoiceArray;
	extern AXVPB* __AXVPBArrayPtr;
	AXVPBInternal_t* DspBlock(uint32 voiceIndex);

	inline AXVPBInternal_t* PpcBlock(const AXVPB* vpb)
	{
		return __AXVPBInternalVoiceArray + (uint32)vpb->index;
	}

	// ax_voice.cpp
	void AXVPB_Init();      // AXInit: build the voice pool
	void AXVPB_Reset();     // drop every voice
	extern FSpinlock g_axVoiceListLock;
	const std::vector<AXVPB*>& AXVoicesOfPriority(uint32 priority);
	const std::vector<AXVPB*>& AXFreeVoices();
	bool AXIsVoiceProtected(const AXVPB* vpb); // inside an AXVoiceBegin/AXUserBegin block of any thread
	bool AXVoiceProtectedForCaller(const AXVPB* vpb); // AXVoiceIsProtected

	// ax_mix.cpp
	void AXMix_Init();
	void AXMix_process(AXVPBInternal_t* firstDspBlock); // render one frame of all listed voices

	// ax_ist.cpp: the frame thread and everything after the mixer
	void AXIst_Init();
	void AXIst_InitThread();
	void AXIst_StopThread();
	void AXIst_QueueFrame();
	bool AXIst_IsFrameBeingProcessed();
	void AXIst_HandleFrameCallbacks();
	void AXResetCallbacks();
	void AXIst_ResetRemixMatrices();
	// main mix of the current frame, [channel][sample] at the renderer rate,
	// 24-bit samples. TV: 6 channels; GamePad: 4 channels of gamepad 0, then 4 of gamepad 1
	sint32be* AXIst_TVMix();
	sint32be* AXIst_DRCMix();
	// 48 kHz result handed to ax_out, [channel][144], 16-bit range
	sint32be* AXIst_TVOutput48();
	sint32be* AXIst_DRCOutput48();

	// ax_aux.cpp
	void AXAux_incrementBufferIndex();

	// ax_out.cpp
	void AXOut_Init();                    // AXInit: pick the device modes
	sint32 AXOut_OutputChannelCount(sint32 device); // channels the host device is fed
	void AXOut_SubmitTVFrame();
	void AXOut_SubmitDRCFrame();
	sint32 AIGetSamplesPerChannel(uint32 device);
	sint32 AIGetChannelCount(uint32 device);
	sint16* AIGetCurrentDMABuffer(uint32 device); // last frame as sent to the host, sint16 big endian, interleaved
	void resetNumProcessedFrames();
	uint32 getNumProcessedFrames();
	void AXOut_FrameDone();
}
