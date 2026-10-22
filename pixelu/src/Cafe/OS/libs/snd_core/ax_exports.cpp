// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// AXInit/AXQuit and the export tables of the two AX libraries: snd_core
// (sound core 1, older titles) and sndcore2. Both expose nearly the same API;
// sndcore2 additionally clamps enveloped samples and offers multichannel voices.
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "OS/libs/coreinit/coreinit_DynLoad.h"

namespace snd_core
{
	sndGeneric_t sndGeneric;

	namespace
	{
		constexpr sint32 kErrBadDevice = -1;
		constexpr sint32 kErrBufferTooSmall = -11;

		// back to the state of a freshly loaded library
		void ResetLibrary()
		{
			sndGeneric = {};
			resetNumProcessedFrames();
			AXVPB_Reset();
			AXIst_ResetRemixMatrices();
		}

		void StartUp(bool soundCore2, uint32 frameLength, uint32 rendererFreq, uint32 pipelineMode)
		{
			if (frameLength != AX_FRAMELENGTH_3MS)
				cemuLog_log(LogType::APIErrors, "AXInit: frame length {} is not supported, using 3 ms", frameLength);
			if (rendererFreq != AX_RENDERER_FREQ_32KHZ && rendererFreq != AX_RENDERER_FREQ_48KHZ)
			{
				cemuLog_log(LogType::APIErrors, "AXInit: renderer rate {} is not supported, using 32 kHz", rendererFreq);
				rendererFreq = AX_RENDERER_FREQ_32KHZ;
			}
			sndGeneric.isSoundCore2 = soundCore2;
			sndGeneric.initParam.frameLength = AX_FRAMELENGTH_3MS;
			sndGeneric.initParam.rendererFreq = rendererFreq;
			sndGeneric.initParam.pipelineMode = pipelineMode;
			// the voice pool takes the mixer default that is current before AXMix_Init
			using InitStep = void (*)();
			static constexpr InitStep s_initSteps[] = {
				AXIst_Init, AXOut_Init, AXVPB_Init, AXAux_Init, AXMix_Init, AXMultiVoice_Init, AXIst_InitThread,
			};
			for (InitStep step : s_initSteps) // in this order
				step();
			sndGeneric.isInitialized = true;
		}

		template<bool TSoundCore2>
		void AXInit()
		{
			if (!sndGeneric.isInitialized)
				StartUp(TSoundCore2, AX_FRAMELENGTH_3MS, AX_RENDERER_FREQ_32KHZ, AX_PIPELINE_SINGLE);
		}

		template<bool TSoundCore2>
		void AXInitEx(uint32 options)
		{
			if (options != 0)
				cemuLog_logDebug(LogType::Force, "AXInitEx: options {:08x} ignored", options);
			AXInit<TSoundCore2>();
		}

		void AXInitWithParams(AXINITPARAM* param)
		{
			if (!sndGeneric.isInitialized)
				StartUp(true, param->frameLength, param->freq, param->pipelineMode);
		}

		bool AXIsInit()
		{
			return sndGeneric.isInitialized;
		}

		void AXQuit()
		{
			AXResetCallbacks();
			AXIst_StopThread();
			AXVPB_Reset();
			sndGeneric.isInitialized = false;
		}

		sint32 AXGetMaxVoices()
		{
			return sndGeneric.isInitialized ? AX_MAX_VOICES : 0;
		}

		AXVPB* AXAcquireVoice(uint32 priority, MPTR callback, MPTR userParam)
		{
			AXVPB* vpb = AXAcquireVoiceEx(priority, MPTR_NULL, MPTR_NULL);
			if (vpb)
			{
				vpb->callback = callback;
				vpb->userParam = userParam;
			}
			return vpb;
		}

		sint32 AXVoiceIsProtected(AXVPB* vpb)
		{
			return AXVoiceProtectedForCaller(vpb) ? 1 : 0;
		}

		uint32 AddressExtension(uint16 format, MPTR samples, uint32 offset)
		{
			const uint32 base = memory_virtualToPhysical(samples);
			switch (format)
			{
			case AX_FORMAT_PCM8: return (base + offset) >> 29;
			case AX_FORMAT_PCM16: return (base + offset * 2) >> 29;
			case AX_FORMAT_ADPCM: return (base + offset / 2) >> 29;
			}
			return 0;
		}

		// a voice can only address one 512 MB window: are loop, end and current in the same one?
		sint32 AXCheckVoiceOffsets(AXPBOFFSET_t* offsets)
		{
			const uint16 format = _swapEndianU16(offsets->format);
			const MPTR samples = _swapEndianU32(offsets->samples);
			const uint32 loop = AddressExtension(format, samples, _swapEndianU32(offsets->loopOffset));
			const uint32 end = AddressExtension(format, samples, _swapEndianU32(offsets->endOffset));
			const uint32 cur = AddressExtension(format, samples, _swapEndianU32(offsets->currentOffset));
			return (loop == end && end == cur) ? 1 : 0;
		}

		sint32 GetDeviceModeExport(sint32 device, uint32be* mode)
		{
			*mode = (uint32)snd_core::AXGetDeviceMode(device);
			return 0;
		}

		sint32 GetDeviceRemixMatrixExport(uint32 device, uint32 inputs, uint32 outputs, MEMPTR<MEMPTR<float32be>> matrixOut)
		{
			return AXGetDeviceRemixMatrix(device, inputs, outputs, matrixOut);
		}

		sint32 SetDeviceRemixMatrixExport(sint32 device, uint32 inputs, uint32 outputs, MEMPTR<float32be> matrix)
		{
			return AXSetDeviceRemixMatrix(device, inputs, outputs, matrix);
		}

		struct AXFinalOutputInfo
		{
			/* +0x00 */ uint32be channelCount;
			/* +0x04 */ uint32be valid;
			/* +0x08 */ uint32be reserved08;
			/* +0x0C */ uint32be reserved0C;
			/* +0x10 */ uint32be size; // bytes written
		};
		static_assert(sizeof(AXFinalOutputInfo) == 0x14);

		// a copy of the last frame sent to a device (16-bit big endian, interleaved)
		sint32 AXGetDeviceFinalOutput(uint32 device, sint16be* buffer, uint32 bufferSize, AXFinalOutputInfo* info)
		{
			if (device != AX_DEV_TV && device != AX_DEV_DRC)
				return kErrBadDevice;
			const uint32 channels = (uint32)AIGetChannelCount(device);
			const uint32 samples = (uint32)AIGetSamplesPerChannel(device) * channels;
			const uint32 bytes = samples * (uint32)sizeof(sint16be);
			if (bufferSize < bytes)
				return kErrBufferTooSmall;
			// the DMA buffer already holds big-endian samples
			memcpy(buffer, AIGetCurrentDMABuffer(device), bytes);
			info->size = bytes;
			info->channelCount = channels;
			info->valid = 1;
			return 0;
		}

		// both libraries export these; the register macros need the library name as a literal
#define AX_REGISTER_COMMON(LIB) \
		cafeExportRegister(LIB, AXIsInit, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXQuit, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetMaxVoices, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetInputSamplesPerFrame, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetInputSamplesPerSec, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetDefaultMixerSelect, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetDefaultMixerSelect, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetDeviceFinalMixCallback, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXRegisterDeviceFinalMixCallback, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXRegisterAppFrameCallback, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXDeregisterAppFrameCallback, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXRegisterFrameCallback, LogType::SoundAPI); \
		cafeExportRegisterFunc(AXRegisterFrameCallback, LIB, "AXRegisterCallback", LogType::SoundAPI); \
		cafeExportRegister(LIB, AXRegisterAuxCallback, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetAuxCallback, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetAuxReturnVolume, LogType::SoundAPI); \
		cafeExportRegisterFunc(GetDeviceModeExport, LIB, "AXGetDeviceMode", LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetDeviceUpsampleStage, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetDeviceUpsampleStage, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXAcquireVoiceEx, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXAcquireVoice, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXFreeVoice, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXUserIsProtected, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXUserBegin, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXUserEnd, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXVoiceBegin, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXVoiceEnd, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXVoiceIsProtected, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXCheckVoiceOffsets, LogType::SoundAPI); \
		cafeExportRegisterFunc(SetDeviceRemixMatrixExport, LIB, "AXSetDeviceRemixMatrix", LogType::SoundAPI); \
		cafeExportRegisterFunc(GetDeviceRemixMatrixExport, LIB, "AXGetDeviceRemixMatrix", LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetDeviceFinalOutput, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceDeviceMix, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXComputeLpfCoefs, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceState, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceType, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceAdpcmLoop, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceSrc, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceSrcType, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceSrcRatio, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceVe, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceAdpcm, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceLoop, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceLpf, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceLpfCoefs, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceBiquad, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceBiquadCoefs, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceOffsets, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceOffsetsEx, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceCurrentOffset, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceCurrentOffsetEx, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceLoopOffset, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceLoopOffsetEx, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceEndOffset, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceEndOffsetEx, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXSetVoiceSamplesAddr, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXIsVoiceRunning, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetVoiceLoopCount, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetVoiceOffsets, LogType::SoundAPI); \
		cafeExportRegister(LIB, AXGetVoiceCurrentOffsetEx, LogType::SoundAPI);

		void ModuleEvent(coreinit::RplEntryReason reason)
		{
			if (reason == coreinit::RplEntryReason::Loaded || reason == coreinit::RplEntryReason::Unloaded)
				ResetLibrary();
		}

		class SoundCore1Module : public COSModule
		{
		public:
			std::string_view GetName() override { return "snd_core"; }

			void RPLMapped() override
			{
				ResetLibrary();
				cafeExportRegisterFunc(AXInit<false>, "snd_core", "AXInit", LogType::SoundAPI);
				cafeExportRegisterFunc(AXInitEx<false>, "snd_core", "AXInitEx", LogType::SoundAPI);
				AX_REGISTER_COMMON("snd_core");
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override { ModuleEvent(reason); }
		};

		class SoundCore2Module : public COSModule
		{
		public:
			std::string_view GetName() override { return "sndcore2"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(AXInitWithParams, "sndcore2", "AXInitWithParams", LogType::SoundAPI);
				cafeExportRegisterFunc(AXInit<true>, "sndcore2", "AXInit", LogType::SoundAPI);
				cafeExportRegisterFunc(AXInitEx<true>, "sndcore2", "AXInitEx", LogType::SoundAPI);
				AX_REGISTER_COMMON("sndcore2");
				// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("sndcore2", fn, LogType::SoundAPI)
				EXPORT_FN(AXAcquireMultiVoice); EXPORT_FN(AXFreeMultiVoice); EXPORT_FN(AXGetMultiVoiceReformatBufferSize);
				EXPORT_FN(AXSetMultiVoiceType); EXPORT_FN(AXSetMultiVoiceAdpcm); EXPORT_FN(AXSetMultiVoiceSrcType);
				EXPORT_FN(AXSetMultiVoiceOffsets); EXPORT_FN(AXSetMultiVoiceVe); EXPORT_FN(AXSetMultiVoiceSrcRatio);
				EXPORT_FN(AXSetMultiVoiceSrc); EXPORT_FN(AXSetMultiVoiceLoop); EXPORT_FN(AXSetMultiVoiceState);
				EXPORT_FN(AXSetMultiVoiceAdpcmLoop); EXPORT_FN(AXIsMultiVoiceRunning);
#undef EXPORT_FN
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override { ModuleEvent(reason); }
		};

		SoundCore1Module s_soundCore1;
		SoundCore2Module s_soundCore2;
	}

	bool isInitialized()
	{
		return sndGeneric.isInitialized;
	}

	bool IsSndCore2()
	{
		return sndGeneric.isSoundCore2;
	}

	void reset()
	{
		AXOut_reset();
		ResetLibrary();
	}

	COSModule* GetModuleSndCore1()
	{
		return &s_soundCore1;
	}

	COSModule* GetModuleSndCore2()
	{
		return &s_soundCore2;
	}
}
