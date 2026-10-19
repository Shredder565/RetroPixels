// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// The AX frame thread ("IST"). Each 3 ms frame it
//   1. exchanges parameters between the PPC and DSP copies of every voice and
//      links the voices that need rendering into a play list,
//   2. renders them (ax_mix.cpp), which also runs the title's frame callbacks,
//   3. brings the device mixes to 48 kHz, around the title's final mix callbacks,
//   4. applies remix matrices and hands the result to ax_out.cpp.
// The thread is a guest thread so title callbacks can run on it; ax_out.cpp
// wakes it through a message queue at the host audio rate.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"

namespace snd_core
{
	namespace
	{
		// result codes
		constexpr sint32 kErrBadDevice = -1;
		constexpr sint32 kErrRemixInputs = -7;
		constexpr sint32 kErrRemixOutputs = -8;
		constexpr sint32 kErrRemixTableFull = -9;
		constexpr sint32 kErrRemixNotFound = -10;
		constexpr sint32 kErrCallbackTableFull = -15;
		constexpr sint32 kErrCallbackNotFound = -16;
		constexpr sint32 kErrNullCallback = -17;

		constexpr uint32 kMsgFrame = 1;
		constexpr uint32 kMsgQuit = 2;
		constexpr sint32 kThreadPriority = 14;
		constexpr uint32 kThreadStackSize = 0x4000;

		constexpr sint32 kOut48 = AX_SAMPLES_PER_3MS_48KHZ;
		constexpr sint32 kDrcPads = 2;
		constexpr sint32 kRemixSlots = 12;

		// what a final mix callback receives: void cb(AXFINALMIXCBPARAM*)
		struct AXFINALMIXCBPARAM
		{
			/* +0x00 */ MEMPTR<MEMPTR<sint32be>> data; // one pointer per channel
			/* +0x04 */ uint16be numChannelInput;
			/* +0x06 */ uint16be numSamples;
			/* +0x08 */ uint16be numDevices;
			/* +0x0A */ uint16be numChannelOutput;
		};
		static_assert(offsetof(AXFINALMIXCBPARAM, data) == 0x00);
		static_assert(offsetof(AXFINALMIXCBPARAM, numChannelInput) == 0x04);
		static_assert(offsetof(AXFINALMIXCBPARAM, numSamples) == 0x06);
		static_assert(offsetof(AXFINALMIXCBPARAM, numDevices) == 0x08);
		static_assert(offsetof(AXFINALMIXCBPARAM, numChannelOutput) == 0x0A);
		static_assert(sizeof(AXFINALMIXCBPARAM) == 0x0C);

		// per device: the renderer's mix, the copy the final mix callback works
		// on, and the 48 kHz result. Guest memory, the callbacks get pointers into it.
		template<sint32 TChannels>
		struct DeviceBuffers
		{
			sint32be mix[TChannels * AX_SAMPLES_MAX];
			sint32be finalMix[TChannels * AX_SAMPLES_MAX];
			sint32be out48[TChannels * kOut48];
			MEMPTR<sint32be> channelPtrs[TChannels];
			AXFINALMIXCBPARAM param;
		};
		using TvBuffers = DeviceBuffers<AX_TV_CHANNEL_COUNT>;
		using DrcBuffers = DeviceBuffers<AX_DRC_CHANNEL_COUNT * kDrcPads>;
		SysAllocator<TvBuffers> s_tv;
		SysAllocator<DrcBuffers> s_drc;

		struct RemixEntry
		{
			uint32 inputs = 0;
			uint32 outputs = 0;
			MEMPTR<float32be> matrix;
		};

		struct IstState
		{
			sint32 upsampleStage[AX_DEV_COUNT] = {};
			// last input sample per channel of the 32 -> 48 kHz converters
			sint32 tvHistory[AX_TV_CHANNEL_COUNT] = {};
			sint32 drcHistory[kDrcPads][AX_DRC_CHANNEL_COUNT] = {};

			MPTR frameCallback = MPTR_NULL;
			MPTR appFrameCallbacks[AX_APP_FRAME_CALLBACK_MAX] = {};
			MPTR finalMixCallbacks[AX_DEV_COUNT] = {};

			RemixEntry remix[kRemixSlots][AX_DEV_COUNT];
		};
		IstState s_ist;

		SysAllocator<coreinit::OSMutex> s_appCallbackMutex;
		SysAllocator<OSThread_t> s_thread;
		SysAllocator<uint8, kThreadStackSize> s_threadStack;
		SysAllocator<coreinit::OSMessage, 16> s_queueStorage;
		SysAllocator<coreinit::OSMessageQueue> s_queue;
		SysAllocator<coreinit::OSMessage> s_outgoing;

		std::atomic_bool s_inFrame = false;

		bool IsTvOrDrc(sint32 device)
		{
			return device == AX_DEV_TV || device == AX_DEV_DRC;
		}

		bool Renders48k()
		{
			return sndGeneric.initParam.rendererFreq == AX_RENDERER_FREQ_48KHZ;
		}

		sint32 ValidateRemix(sint32 device, uint32 inputs, uint32 outputs)
		{
			if (device == AX_DEV_TV)
			{
				if (inputs > (uint32)AX_TV_CHANNEL_COUNT)
					return kErrRemixInputs;
				if (outputs != 1 && outputs != 2 && outputs != 6)
					return kErrRemixOutputs;
				return 0;
			}
			if (device == AX_DEV_DRC)
			{
				if (inputs > (uint32)AX_DRC_CHANNEL_COUNT)
					return kErrRemixInputs;
				if (outputs != 1 && outputs != 2 && outputs != 4)
					return kErrRemixOutputs;
				return 0;
			}
			return kErrBadDevice;
		}

		RemixEntry* FindRemix(sint32 device, uint32 inputs, uint32 outputs)
		{
			for (auto& slot : s_ist.remix)
			{
				RemixEntry& e = slot[device];
				if (e.inputs == inputs && e.outputs == outputs)
					return &e;
			}
			return nullptr;
		}

		// --- 32 kHz -> 48 kHz -------------------------------------------------------------

		// Linear interpolation, two input samples -> three output samples:
		// out0 = 2/3 prev + 1/3 a, out1 = a, out2 = 2/3 b + 1/3 a.
		// 'narrow' converts 24-bit mix samples to 16-bit on the way.
		sint32 Upsample32To48(const sint32be* in, sint32be* out, sint32* history, sint32 inCount, bool narrow, sint32 channels)
		{
			constexpr float kTwoThirds = 2.0f / 3.0f;
			constexpr float kOneThird = 1.0f - kTwoThirds; // so the weights sum to exactly 1
			static_assert(kTwoThirds == 0.66666669f && kOneThird == 0.33333331f);
			const sint32 shift = narrow ? 8 : 0;
			for (sint32 ch = 0; ch < channels; ch++)
			{
				float prev = (float)history[ch];
				for (sint32 i = 0; i < inCount; i += 2)
				{
					const float a = (float)(sint32)in[0];
					const float b = (float)(sint32)in[1];
					in += 2;
					out[0] = (sint32)(prev * kTwoThirds + a * kOneThird) >> shift;
					out[1] = (sint32)a >> shift;
					out[2] = (sint32)(b * kTwoThirds + a * kOneThird) >> shift;
					out += 3;
					prev = b;
				}
				history[ch] = (sint32)prev;
			}
			return inCount / 2 * 3;
		}

		template<sint32 TChannels>
		void PointChannels(DeviceBuffers<TChannels>& d, sint32be* base, sint32 samplesPerChannel)
		{
			for (sint32 ch = 0; ch < TChannels; ch++)
				d.channelPtrs[ch] = base + ch * samplesPerChannel;
			d.param.data = d.channelPtrs;
			d.param.numSamples = (uint16)samplesPerChannel;
		}

		template<sint32 TChannels>
		void ResetParam(DeviceBuffers<TChannels>& d, uint16 channels, uint16 devices)
		{
			d.param.data = nullptr;
			d.param.numChannelInput = channels;
			d.param.numChannelOutput = channels;
			d.param.numSamples = (uint16)AXGetInputSamplesPerFrame();
			d.param.numDevices = devices;
		}

		// the final mix callback sees 16-bit samples: at 48 kHz (renderer rate,
		// or upsampled before the callback) or at 32 kHz (upsampled after it)
		bool UpsamplesBeforeCallback(sint32 device)
		{
			return !Renders48k() && s_ist.upsampleStage[device] == AX_UPSAMPLE_STAGE_BEFORE_FINALMIX;
		}

		template<sint32 TChannels>
		void PrepareForCallback(DeviceBuffers<TChannels>& d, sint32 count)
		{
			for (sint32 i = 0; i < TChannels * count; i++)
				d.finalMix[i] = (sint32)d.mix[i] >> 8;
			PointChannels(d, d.finalMix, count);
		}

		void FinalMixStage()
		{
			const sint32 count = AXGetInputSamplesPerFrame();
			TvBuffers& tv = *s_tv.GetPtr();
			DrcBuffers& drc = *s_drc.GetPtr();

			if (UpsamplesBeforeCallback(AX_DEV_TV))
			{
				const sint32 n = Upsample32To48(tv.mix, tv.out48, s_ist.tvHistory, count, true, AX_TV_CHANNEL_COUNT);
				PointChannels(tv, tv.out48, n);
			}
			else
				PrepareForCallback(tv, count);

			if (UpsamplesBeforeCallback(AX_DEV_DRC))
			{
				sint32 n = 0;
				for (sint32 pad = 0; pad < kDrcPads; pad++)
				{
					const sint32 first = pad * AX_DRC_CHANNEL_COUNT;
					n = Upsample32To48(drc.mix + first * count, drc.out48 + first * kOut48, s_ist.drcHistory[pad], count, true, AX_DRC_CHANNEL_COUNT);
				}
				PointChannels(drc, drc.out48, n);
			}
			else
				PrepareForCallback(drc, count);

			for (sint32 device : { AX_DEV_TV, AX_DEV_DRC })
			{
				const MPTR cb = s_ist.finalMixCallbacks[device];
				if (cb == MPTR_NULL)
					continue;
				AXFINALMIXCBPARAM* param = device == AX_DEV_TV ? &tv.param : &drc.param;
				PPCCoreCallback(cb, param);
			}

			// bring what the callback saw to 48 kHz
			if (Renders48k())
			{
				std::copy_n(tv.finalMix, AX_TV_CHANNEL_COUNT * kOut48, tv.out48);
				std::copy_n(drc.finalMix, AX_DRC_CHANNEL_COUNT * kOut48, drc.out48); // first GamePad
				return;
			}
			if (!UpsamplesBeforeCallback(AX_DEV_TV))
				Upsample32To48(tv.finalMix, tv.out48, s_ist.tvHistory, count, false, AX_TV_CHANNEL_COUNT);
			if (!UpsamplesBeforeCallback(AX_DEV_DRC))
				Upsample32To48(drc.finalMix, drc.out48, s_ist.drcHistory[0], count, false, AX_DRC_CHANNEL_COUNT);
		}

		// --- remix ------------------------------------------------------------------------

		// out[j] = sum_k in[k] * m[j][k], each product truncated to an integer
		void Remix(sint32be* samples, const float32be* matrix, sint32 inputs, sint32 outputs, sint32 count)
		{
			for (sint32 i = 0; i < count; i++)
			{
				float in[AX_TV_CHANNEL_COUNT] = {};
				for (sint32 k = 0; k < inputs; k++)
					in[k] = (float)(sint32)samples[k * count + i];
				sint32 out[AX_TV_CHANNEL_COUNT] = {};
				const float32be* row = matrix;
				for (sint32 j = 0; j < outputs; j++)
				{
					for (sint32 k = 0; k < inputs; k++)
						out[j] += (sint32)(in[k] * (float)*row++);
				}
				for (sint32 j = 0; j < outputs; j++)
					samples[j * count + i] = out[j];
			}
		}

		void RemixStage()
		{
			struct Target { sint32 device; sint32be* samples; const AXFINALMIXCBPARAM* param; };
			const Target targets[] = {
				{ AX_DEV_TV, s_tv->out48, &s_tv->param },
				{ AX_DEV_DRC, s_drc->out48, &s_drc->param },
			};
			for (const Target& t : targets)
			{
				const uint32 inputs = t.param->numChannelInput;
				const uint32 outputs = (uint32)AXOut_OutputChannelCount(t.device);
				const RemixEntry* e = FindRemix(t.device, inputs, outputs);
				if (!e || e->matrix.IsNull())
					continue;
				Remix(t.samples, e->matrix.GetPtr(), (sint32)inputs, (sint32)outputs, kOut48);
			}
		}

		// --- PPC <-> DSP parameter exchange ---------------------------------------------

		// DSP -> PPC: playback progress, unless the title just replaced that group
		void PullProgress(AXVPB* vpb, AXVPBInternal_t* ppc, const AXVPBInternal_t* dsp, uint32 dirty)
		{
			if (!(dirty & VoiceDirty::State))
			{
				vpb->playbackState = _swapEndianU16(dsp->playbackState);
				ppc->playbackState = dsp->playbackState;
			}
			if (!(dirty & (VoiceDirty::CurOffset | VoiceDirty::Offsets)))
			{
				ppc->addr.curHi = dsp->addr.curHi;
				ppc->addr.curLo = dsp->addr.curLo;
			}
			if (!(dirty & VoiceDirty::Ve))
				ppc->veVolume = dsp->veVolume;
			if (!(dirty & VoiceDirty::Adpcm))
				ppc->adpcm = dsp->adpcm;
			if (!(dirty & VoiceDirty::Src))
			{
				ppc->src.currentFrac = dsp->src.currentFrac;
				std::copy(std::begin(dsp->src.historySamples), std::end(dsp->src.historySamples), ppc->src.historySamples);
			}
		}

		// PPC -> DSP: every parameter group the title changed
		void PushChanges(AXVPBInternal_t* dsp, const AXVPBInternal_t* ppc, uint32 dirty)
		{
			if (dirty & VoiceDirty::SrcType)
			{
				dsp->srcMode = ppc->srcMode;
				dsp->srcTap = ppc->srcTap;
			}
			if (dirty & VoiceDirty::MixMask)
			{
				std::copy(std::begin(ppc->mixMaskTV), std::end(ppc->mixMaskTV), dsp->mixMaskTV);
				std::copy(std::begin(ppc->mixMaskDRC), std::end(ppc->mixMaskDRC), dsp->mixMaskDRC);
				std::copy(std::begin(ppc->mixMaskRMT), std::end(ppc->mixMaskRMT), dsp->mixMaskRMT);
			}
			if (dirty & VoiceDirty::Mix)
			{
				std::copy(std::begin(ppc->deviceMixTV), std::end(ppc->deviceMixTV), dsp->deviceMixTV);
				std::copy(std::begin(ppc->deviceMixDRC), std::end(ppc->deviceMixDRC), dsp->deviceMixDRC);
				std::copy(std::begin(ppc->deviceMixRMT), std::end(ppc->deviceMixRMT), dsp->deviceMixRMT);
			}
			if (dirty & VoiceDirty::State)
				dsp->playbackState = ppc->playbackState;
			if (dirty & VoiceDirty::Type)
				dsp->voiceType = ppc->voiceType;
			if (dirty & VoiceDirty::ItdTarget)
			{
				dsp->itdState[2] = ppc->itdState[2];
				dsp->itdState[3] = ppc->itdState[3];
			}
			if (dirty & VoiceDirty::VeDelta)
				dsp->veDelta = ppc->veDelta;
			else if (dirty & VoiceDirty::Ve)
			{
				dsp->veVolume = ppc->veVolume;
				dsp->veDelta = ppc->veDelta;
			}
			if (dirty & VoiceDirty::Offsets)
				dsp->addr = ppc->addr;
			else
			{
				if (dirty & VoiceDirty::LoopFlag)
					dsp->addr.loopFlag = ppc->addr.loopFlag;
				if (dirty & VoiceDirty::LoopOffset)
					dsp->addr.SetLoop(ppc->addr.Loop());
				if (dirty & VoiceDirty::EndOffset)
					dsp->addr.SetEnd(ppc->addr.End());
				if (dirty & VoiceDirty::CurOffset)
					dsp->addr.SetCur(ppc->addr.Cur());
			}
			if (dirty & VoiceDirty::Adpcm)
			{
				// the decoder history stays the DSP's
				std::copy(std::begin(ppc->adpcm.a), std::end(ppc->adpcm.a), dsp->adpcm.a);
				dsp->adpcm.gain = ppc->adpcm.gain;
				dsp->adpcm.scale = ppc->adpcm.scale;
			}
			if (dirty & VoiceDirty::Src)
				dsp->src = ppc->src;
			else if (dirty & VoiceDirty::SrcRatio)
			{
				dsp->src.ratioHigh = ppc->src.ratioHigh;
				dsp->src.ratioLow = ppc->src.ratioLow;
			}
			if (dirty & VoiceDirty::AdpcmLoop)
				dsp->adpcmLoop = ppc->adpcmLoop;
			if (dirty & VoiceDirty::LpfCoefs)
			{
				dsp->lpf.a0 = ppc->lpf.a0;
				dsp->lpf.b0 = ppc->lpf.b0;
			}
			else if (dirty & VoiceDirty::Lpf)
				dsp->lpf = ppc->lpf;
			if (dirty & VoiceDirty::BiquadCoefs)
			{
				dsp->biquad.b0 = ppc->biquad.b0;
				dsp->biquad.b1 = ppc->biquad.b1;
				dsp->biquad.b2 = ppc->biquad.b2;
				dsp->biquad.a1 = ppc->biquad.a1;
				dsp->biquad.a2 = ppc->biquad.a2;
			}
			else if (dirty & VoiceDirty::Biquad)
				dsp->biquad = ppc->biquad;
			if (dirty & VoiceDirty::RmtOn)
				dsp->rmtOn = ppc->rmtOn;
			// ItdOn, RmtSrc and RmtIir have nothing to render here
		}

		void ExchangeVoice(AXVPB* vpb)
		{
			const uint32 index = vpb->index;
			const uint32 dirty = vpb->sync;
			AXVPBInternal_t* ppc = PpcBlock(vpb);
			AXVPBInternal_t* dsp = DspBlock(index);

			dsp->nextHi = ppc->nextHi;
			dsp->nextLo = ppc->nextLo;
			AXVPBInternal_t* nextPpc = ppc->nextToProcess.GetPtr();
			dsp->nextToProcess = nextPpc ? DspBlock((uint32)nextPpc->index) : nullptr;
			dsp->mixerSelect = ppc->mixerSelect;
			dsp->listState = ppc->listState;

			PullProgress(vpb, ppc, dsp, dirty);
			// a voice in the middle of a protected update keeps its old parameters
			if (AXIsVoiceProtected(vpb))
				return;
			PushChanges(dsp, ppc, dirty);
		}

		constexpr uint16 kListed = 1;
		constexpr uint16 kIdle = 2;

		// Builds this frame's play list and returns its first DSP block. Voices
		// are linked from the highest priority down, each new one pointing at
		// the previous, so rendering starts with the last voice linked.
		AXVPBInternal_t* BuildPlayList()
		{
			g_axVoiceListLock.lock();
			AXVPBInternal_t* last = nullptr;
			for (sint32 priority = AX_PRIORITY_MAX - 1; priority >= AX_PRIORITY_LOWEST; priority--)
			{
				for (AXVPB* vpb : AXVoicesOfPriority(priority))
				{
					AXVPBInternal_t* ppc = PpcBlock(vpb);
					AXVPBInternal_t* dsp = DspBlock(vpb->index);
					ppc->mixerSelect = (uint16)(uint32)vpb->mixerSelect;
					vpb->depop = 0; // no depop ramp is rendered
					const bool running = _swapEndianU16(ppc->playbackState) == 1;
					if (!running && vpb->sync == 0u)
					{
						ppc->listState = kIdle;
						dsp->listState = kIdle;
						ppc->nextHi = 0;
						ppc->nextLo = 0;
						continue;
					}
					ppc->listState = kListed;
					ppc->nextHi = last ? (uint16)last->selfHi : (uint16)0;
					ppc->nextLo = last ? (uint16)last->selfLo : (uint16)0;
					ppc->nextToProcess = last;
					last = ppc;
					ExchangeVoice(vpb);
					if (!AXIsVoiceProtected(vpb))
						vpb->sync = 0;
				}
			}
			for (AXVPB* vpb : AXFreeVoices())
			{
				AXVPBInternal_t* ppc = PpcBlock(vpb);
				AXVPBInternal_t* dsp = DspBlock(vpb->index);
				vpb->depop = 0;
				vpb->playbackState = 0;
				ppc->listState = kIdle;
				dsp->listState = kIdle;
				ppc->playbackState = 0;
				dsp->playbackState = 0;
			}
			AXVPBInternal_t* first = last ? DspBlock((uint32)last->index) : nullptr;
			g_axVoiceListLock.unlock();
			return first;
		}

		// --- the frame ----------------------------------------------------------------------

		void GenerateFrame()
		{
			s_inFrame.store(true);
			std::fill(std::begin(s_tv->mix), std::end(s_tv->mix), 0);
			std::fill(std::begin(s_drc->mix), std::end(s_drc->mix), 0);

			AXMix_process(BuildPlayList());

			ResetParam(*s_tv.GetPtr(), AX_TV_CHANNEL_COUNT, 1);
			ResetParam(*s_drc.GetPtr(), AX_DRC_CHANNEL_COUNT, kDrcPads);
			FinalMixStage();
			RemixStage();

			AXOut_SubmitTVFrame();
			AXOut_SubmitDRCFrame();
			s_inFrame.store(false);
		}

		uint32 FrameThreadMain(uint32 argc, void* argv)
		{
			while (true)
			{
				StackAllocator<coreinit::OSMessage> msg;
				coreinit::OSReceiveMessage(s_queue.GetPtr(), msg.GetPointer(), OS_MESSAGE_BLOCK);
				const uint32 what = msg->message;
				if (what == kMsgQuit)
					break;
				if (what != kMsgFrame)
				{
					cemuLog_logDebug(LogType::Force, "AX frame thread: unknown message {}", what);
					continue;
				}
				GenerateFrame();
				AXOut_FrameDone();
			}
			cemuLog_logDebug(LogType::Force, "AX frame thread stopped");
			return 0;
		}

		void Post(uint32 what)
		{
			coreinit::OSMessage* msg = s_outgoing.GetPtr();
			msg->message = what;
			msg->data0 = 0;
			msg->data1 = 0;
			msg->data2 = 0;
			coreinit::OSSendMessage(s_queue.GetPtr(), msg, 0);
		}
	}

	sint32be* AXIst_TVMix() { return s_tv->mix; }
	sint32be* AXIst_DRCMix() { return s_drc->mix; }
	sint32be* AXIst_TVOutput48() { return s_tv->out48; }
	sint32be* AXIst_DRCOutput48() { return s_drc->out48; }

	sint32 AXGetInputSamplesPerFrame()
	{
		return Renders48k() ? AX_SAMPLES_PER_3MS_48KHZ : AX_SAMPLES_PER_3MS_32KHZ;
	}

	sint32 AXGetInputSamplesPerSec()
	{
		return AXGetInputSamplesPerFrame() * 1000 / 3;
	}

	sint32 AXSetDeviceUpsampleStage(sint32 device, int upsampleStage)
	{
		if (!IsTvOrDrc(device))
			return kErrBadDevice;
		s_ist.upsampleStage[device] = upsampleStage;
		return 0;
	}

	sint32 AXGetDeviceUpsampleStage(sint32 device, uint32be* upsampleStage)
	{
		if (!IsTvOrDrc(device))
			return kErrBadDevice;
		*upsampleStage = (uint32)s_ist.upsampleStage[device];
		return 0;
	}

	void AXIst_Init()
	{
		for (auto& stage : s_ist.upsampleStage)
			stage = AX_UPSAMPLE_STAGE_BEFORE_FINALMIX;
		std::fill(std::begin(s_ist.tvHistory), std::end(s_ist.tvHistory), 0);
		for (auto& pad : s_ist.drcHistory)
			std::fill(std::begin(pad), std::end(pad), 0);
		AXResetCallbacks();
	}

	void AXResetCallbacks()
	{
		s_ist.frameCallback = MPTR_NULL;
		std::fill(std::begin(s_ist.appFrameCallbacks), std::end(s_ist.appFrameCallbacks), MPTR_NULL);
		std::fill(std::begin(s_ist.finalMixCallbacks), std::end(s_ist.finalMixCallbacks), MPTR_NULL);
		coreinit::OSInitMutexEx(s_appCallbackMutex.GetPtr(), nullptr);
	}

	void AXIst_ResetRemixMatrices()
	{
		for (auto& slot : s_ist.remix)
		{
			for (auto& e : slot)
				e = RemixEntry{};
		}
	}

	// --- callbacks ------------------------------------------------------------------------

	sint32 AXRegisterAppFrameCallback(MPTR funcAddr)
	{
		if (funcAddr == MPTR_NULL)
			return kErrNullCallback;
		coreinit::OSLockMutex(s_appCallbackMutex.GetPtr());
		sint32 result = kErrCallbackTableFull;
		for (MPTR& slot : s_ist.appFrameCallbacks)
		{
			if (slot == MPTR_NULL)
			{
				slot = funcAddr;
				result = 0;
				break;
			}
		}
		coreinit::OSUnlockMutex(s_appCallbackMutex.GetPtr());
		return result;
	}

	sint32 AXDeregisterAppFrameCallback(MPTR funcAddr)
	{
		if (funcAddr == MPTR_NULL)
			return kErrNullCallback;
		coreinit::OSLockMutex(s_appCallbackMutex.GetPtr());
		sint32 result = kErrCallbackNotFound;
		for (MPTR& slot : s_ist.appFrameCallbacks)
		{
			if (slot == funcAddr)
			{
				slot = MPTR_NULL;
				result = 0;
				break;
			}
		}
		coreinit::OSUnlockMutex(s_appCallbackMutex.GetPtr());
		return result;
	}

	MPTR AXRegisterFrameCallback(MPTR funcAddr)
	{
		return std::exchange(s_ist.frameCallback, funcAddr);
	}

	sint32 AXGetDeviceFinalMixCallback(sint32 device, uint32be* funcAddrPtr)
	{
		if (!IsTvOrDrc(device))
			return kErrBadDevice;
		*funcAddrPtr = s_ist.finalMixCallbacks[device];
		return 0;
	}

	sint32 AXRegisterDeviceFinalMixCallback(sint32 device, MPTR funcAddr)
	{
		if (!IsTvOrDrc(device))
			return kErrBadDevice;
		s_ist.finalMixCallbacks[device] = funcAddr;
		return 0;
	}

	// run from the mixer after the voices are rendered
	void AXIst_HandleFrameCallbacks()
	{
		if (s_ist.frameCallback != MPTR_NULL)
			PPCCore_executeCallbackInternal(s_ist.frameCallback);
		for (MPTR cb : s_ist.appFrameCallbacks)
		{
			if (cb != MPTR_NULL)
				PPCCore_executeCallbackInternal(cb);
		}
	}

	// --- remix matrices ---------------------------------------------------------------------

	sint32 AXSetDeviceRemixMatrix(sint32 deviceId, uint32 inputChannelCount, uint32 outputChannelCount, const MEMPTR<float32be>& matrix)
	{
		if (sint32 r = ValidateRemix(deviceId, inputChannelCount, outputChannelCount); r != 0)
		{
			cemuLog_log(LogType::APIErrors, "AXSetDeviceRemixMatrix: device {} with {} inputs and {} outputs is not supported", deviceId, inputChannelCount, outputChannelCount);
			return r;
		}
		RemixEntry* e = FindRemix(deviceId, inputChannelCount, outputChannelCount);
		if (!e)
			e = FindRemix(deviceId, 0, 0); // a free slot
		if (!e)
			return kErrRemixTableFull;
		e->inputs = inputChannelCount;
		e->outputs = outputChannelCount;
		e->matrix = matrix;
		return 0;
	}

	sint32 AXGetDeviceRemixMatrix(uint32 deviceId, uint32 inputChannelCount, uint32 outputChannelCount, MEMPTR<MEMPTR<float32be>>& matrix)
	{
		if (sint32 r = ValidateRemix((sint32)deviceId, inputChannelCount, outputChannelCount); r != 0)
			return r;
		const RemixEntry* e = FindRemix((sint32)deviceId, inputChannelCount, outputChannelCount);
		if (!e)
			return kErrRemixNotFound;
		*matrix = e->matrix;
		return 0;
	}

	// --- thread -------------------------------------------------------------------------------

	void AXIst_InitThread()
	{
		static const MPTR entry = makeCallableExport<&FrameThreadMain>();
		s_inFrame = false;
		coreinit::OSInitMessageQueue(s_queue.GetPtr(), s_queueStorage.GetPtr(), s_queueStorage.GetCount());
		uint8* stackTop = s_threadStack.GetPtr() + kThreadStackSize;
		coreinit::__OSCreateThreadType(s_thread.GetPtr(), entry, 0, nullptr, stackTop, kThreadStackSize, kThreadPriority, 0, OSThread_t::THREAD_TYPE::TYPE_DRIVER);
		coreinit::OSResumeThread(s_thread.GetPtr());
	}

	void AXIst_StopThread()
	{
		if (coreinit::OSIsThreadTerminated(s_thread.GetPtr()))
			return;
		Post(kMsgQuit);
		while (!coreinit::OSIsThreadTerminated(s_thread.GetPtr()))
			PPCCore_switchToScheduler();
	}

	void AXIst_QueueFrame()
	{
		Post(kMsgFrame);
	}

	bool AXIst_IsFrameBeingProcessed()
	{
		return s_inFrame.load();
	}
}
