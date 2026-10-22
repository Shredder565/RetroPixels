// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Host side of AX output: device modes, the per-frame hand-off of the 48 kHz
// TV and GamePad mixes to the host audio devices (and to the PixelU
// recorder), and the timer that paces the frame thread.
//
// A frame is clamped to 16 bits and interleaved into a "DMA buffer" (big
// endian, as the audio interface would read it). Host devices take blocks of
// four frames, so frames are collected in native byte order until a block is full.
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

namespace snd_core
{
	namespace
	{
		constexpr sint32 kFrameSamples = AX_SAMPLES_PER_3MS_48KHZ;
		constexpr sint32 kFramesPerBlock = 4;
		constexpr sint32 kMaxInterleaved = 6;

		struct DeviceOut
		{
			sint32 mode = AX_MODE_STEREO;   // AX_MODE_*
			sint32 outputChannels = 2;      // channels the mix is reduced to
			sint16 dma[kFrameSamples * kMaxInterleaved] = {};             // last frame, big endian
			sint16 block[kFrameSamples * kMaxInterleaved * kFramesPerBlock] = {}; // native, for the host device
			sint32 framesInBlock = 0;
		};
		DeviceOut s_tvOut;
		DeviceOut s_drcOut;
		sint32 s_rmtMode = AX_MODE_STEREO;

		std::atomic<uint32> s_framesRendered = 0;
		uint32 s_framesQueued = 0;

		sint16 ClampBE(sint32 v)
		{
			return _swapEndianS16((sint16)std::clamp(v, -32768, 32767));
		}

		// channel order of the 48 kHz mix: L, R, surround L, surround R, centre, LFE.
		// Host devices want L, R, centre, LFE, surround L, surround R.
		constexpr sint32 kTvSurroundOrder[6] = { 0, 1, 4, 5, 2, 3 };

		// interleave 'channels' channels of the planar mix into the DMA buffer;
		// map[i] is the mix channel for interleaved slot i, -1 for silence
		void Interleave(DeviceOut& d, const sint32be* mix, const sint32* map, sint32 slots)
		{
			for (sint32 i = 0; i < kFrameSamples; i++)
			{
				for (sint32 s = 0; s < slots; s++)
					d.dma[i * slots + s] = map[s] < 0 ? (sint16)0 : ClampBE(mix[map[s] * kFrameSamples + i]);
			}
		}

		// queue one DMA frame of 'slots' interleaved channels for the host device
		void Feed(DeviceOut& d, AudioAPIPtr& device, sint32 hostDefaultChannels, sint32 slots)
		{
			const sint32 n = kFrameSamples * slots;
			std::shared_lock lock(g_audioMutex);
			const sint32 hostChannels = device ? device->GetChannels() : hostDefaultChannels;
			sint16* dst = d.block + kFrameSamples * d.framesInBlock * hostChannels;
			for (sint32 i = 0; i < n; i++)
				dst[i] = _swapEndianS16(d.dma[i]);
			if (++d.framesInBlock == kFramesPerBlock)
			{
				if (device)
					device->FeedBlock(d.block);
				d.framesInBlock = 0;
			}
		}

		void SetPlaying(bool playing)
		{
			std::shared_lock lock(g_audioMutex);
			for (AudioAPIPtr* dev : { &g_tvAudio, &g_padAudio, &g_portalAudio })
			{
				if (!*dev)
					continue;
				if (playing)
					(*dev)->Play();
				else
					(*dev)->Stop();
			}
		}

		void OpenDevice(AudioAPIPtr& device, IAudioAPI::AudioType type, const char* what)
		{
			if (device)
				return;
			try
			{
				device = IAudioAPI::CreateDeviceFromConfig(type, 48000, kFrameSamples * kFramesPerBlock, 16);
			}
			catch (std::runtime_error& ex)
			{
				cemuLog_log(LogType::Force, "can't initialize {} audio: {}", what, ex.what());
			}
		}

		void CloseDevice(AudioAPIPtr& device)
		{
			if (!device)
				return;
			device->Stop();
			device.reset();
		}
	}

	// --- device modes ---------------------------------------------------------------------

	// the console's audio settings decide the modes: TV from the emulator's
	// TV channel setting, the GamePad is always stereo
	void AXOut_Init()
	{
		switch (GetConfig().tv_channels)
		{
		case kMono:
			s_tvOut.mode = AX_MODE_MONO;
			s_tvOut.outputChannels = 1;
			break;
		case kSurround:
			s_tvOut.mode = AX_MODE_6CH;
			s_tvOut.outputChannels = 6;
			break;
		default:
			s_tvOut.mode = AX_MODE_STEREO;
			s_tvOut.outputChannels = 2;
			break;
		}
		// Monster Hunter 3 Ultimate refuses to start unless the GamePad reports stereo
		s_drcOut.mode = AX_MODE_STEREO;
		s_drcOut.outputChannels = 2;
	}

	sint32 AXGetDeviceMode(sint32 device)
	{
		switch (device)
		{
		case AX_DEV_TV: return s_tvOut.mode;
		case AX_DEV_DRC: return s_drcOut.mode;
		case AX_DEV_RMT: return s_rmtMode;
		}
		return 0;
	}

	sint32 AXOut_OutputChannelCount(sint32 device)
	{
		return device == AX_DEV_TV ? s_tvOut.outputChannels : s_drcOut.outputChannels;
	}

	sint32 AIGetSamplesPerChannel(uint32 device)
	{
		return kFrameSamples; // both devices run at 48 kHz
	}

	sint32 AIGetChannelCount(uint32 device)
	{
		const sint32 mode = AXGetDeviceMode((sint32)device);
		if (mode == AX_MODE_6CH)
			return 6;
		if (mode == AX_MODE_STEREO)
			return 2;
		return 1;
	}

	sint16* AIGetCurrentDMABuffer(uint32 device)
	{
		if (device == AX_DEV_TV)
			return s_tvOut.dma;
		if (device == AX_DEV_DRC)
			return s_drcOut.dma;
		return nullptr;
	}

	// --- per frame ------------------------------------------------------------------------

	void AXOut_SubmitTVFrame()
	{
		static constexpr sint32 kStereo[2] = { 0, 1 };
		static constexpr sint32 kMono[1] = { 0 };
		const sint32be* mix = AXIst_TVOutput48();
		sint32 slots;
		switch (s_tvOut.mode)
		{
		case AX_MODE_6CH: slots = 6; Interleave(s_tvOut, mix, kTvSurroundOrder, slots); break;
		case AX_MODE_MONO: slots = 1; Interleave(s_tvOut, mix, kMono, slots); break;
		default: slots = 2; Interleave(s_tvOut, mix, kStereo, slots); break;
		}
		// PixelU recorder: the TV mix exactly as it goes to the host device (before host volume)
		if (LatteRecorder::IsRecording())
			LatteRecorder::SubmitTVAudioBE(s_tvOut.dma, kFrameSamples, slots);
		Feed(s_tvOut, g_tvAudio, AX_TV_CHANNEL_COUNT, slots);
	}

	void AXOut_SubmitDRCFrame()
	{
		// GamePad speakers are stereo; a mono mix is sent on both sides
		static constexpr sint32 kSurround[6] = { 0, 1, -1, -1, -1, -1 };
		static constexpr sint32 kStereo[2] = { 0, 1 };
		static constexpr sint32 kMonoAsStereo[2] = { 0, 0 };
		const sint32be* mix = AXIst_DRCOutput48();
		sint32 slots = 2;
		switch (s_drcOut.mode)
		{
		case AX_MODE_6CH: slots = 6; Interleave(s_drcOut, mix, kSurround, slots); break;
		case AX_MODE_MONO: Interleave(s_drcOut, mix, kMonoAsStereo, slots); break;
		default: Interleave(s_drcOut, mix, kStereo, slots); break;
		}
		Feed(s_drcOut, g_padAudio, AX_DRC_CHANNEL_COUNT, slots);
	}

	void resetNumProcessedFrames()
	{
		s_framesRendered = 0;
	}

	uint32 getNumProcessedFrames()
	{
		return s_framesRendered;
	}

	void AXOut_FrameDone()
	{
		s_framesRendered++;
	}

	// --- host devices ---------------------------------------------------------------------

	void AXOut_init()
	{
		s_framesQueued = 0;
		std::unique_lock lock(g_audioMutex);
		OpenDevice(g_tvAudio, IAudioAPI::AudioType::TV, "tv");
		g_padVolume = GetConfig().pad_volume;
		OpenDevice(g_padAudio, IAudioAPI::AudioType::Gamepad, "pad");
	}

	void AXOut_reset()
	{
		std::unique_lock lock(g_audioMutex);
		CloseDevice(g_tvAudio);
		CloseDevice(g_padAudio);
		CloseDevice(g_portalAudio);
	}

	// Called often by the scheduler. Queues the next frame every 3 ms (2.9 ms
	// while a host device is short of data), never faster than every 1.7 ms,
	// and never while the previous frame is still being rendered. After a
	// stall longer than about three quarters of the host buffer, the timer
	// skips ahead instead of rendering the backlog.
	void AXOut_update()
	{
		using namespace std::chrono;
		constexpr auto kPeriod = duration_cast<nanoseconds>(milliseconds(3));
		constexpr auto kPeriodHurry = duration_cast<nanoseconds>(microseconds(2900));
		constexpr auto kMinGap = duration_cast<nanoseconds>(microseconds(1700));
		constexpr auto kMaxLag = duration_cast<nanoseconds>(milliseconds((IAudioAPI::kBlockCount * 3 / 4) * (kFramesPerBlock * 3)));

		bool hurry = false;
		{
			const std::shared_lock lock(g_audioMutex, std::try_to_lock);
			if (lock)
				hurry = (g_tvAudio && g_tvAudio->NeedAdditionalBlocks()) || (g_padAudio && g_padAudio->NeedAdditionalBlocks());
		}
		const auto period = hurry ? kPeriodHurry : kPeriod;

		static auto s_due = now_cached() - kPeriod; // start of the current period
		static auto s_lastFire = now_cached();
		const auto now = now_cached();
		const auto behind = now - s_due;
		if (behind < period || (now - s_lastFire) < kMinGap)
			return;
		s_lastFire = now;
		if (behind >= kMaxLag)
			s_due = now - period;
		else
			s_due += period;

		if (!isInitialized() || s_framesQueued != getNumProcessedFrames())
			return;
		SetPlaying(true);
		AXIst_QueueFrame();
		s_framesQueued++;
	}
}
