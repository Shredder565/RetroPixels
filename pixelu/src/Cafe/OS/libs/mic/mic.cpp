// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// mic is the GamePad microphone library. A game hands MICInit a ring buffer
// of big-endian 16-bit PCM samples at 32 kHz; the system keeps writing into
// it and the game reports how much it has read with MICSetDataConsumed.
//
// PixelU fills the ring buffer once per AX audio frame (3 ms, 96 samples)
// from the host microphone chosen in the settings (cubeb). Without a host
// microphone, holding the GamePad's "mic" button sends a loud test tone so
// blow/shout prompts can still be passed; otherwise the buffer gets silence.
// Only the first GamePad (DRC 0) has a microphone.

#include "Cafe/OS/common/OSCommon.h"
#include "input/InputManager.h"
#include "audio/IAudioInputAPI.h"
#include "config/CemuConfig.h"
#include "mic.h"

namespace
{
	constexpr sint32 kSampleRate = 32000;
	constexpr sint32 kSamplesPerFrame = kSampleRate * 3 / 1000; // one AX frame

	// MIC error codes
	constexpr sint32 kOk = 0;
	constexpr sint32 kBadParam = -2;
	constexpr sint32 kAlreadyOpen = -5;
	constexpr sint32 kNotOpen = -6;
	constexpr sint32 kNotInitialized = -7;
	constexpr sint32 kNotConnected = -8;
	constexpr sint32 kOverConsumed = -81;

	// MICStatus.flags
	constexpr uint32 kFlagPcm16 = 1 << 0;
	constexpr uint32 kFlagOpen = 1 << 1;
	constexpr uint32 kFlagConnected = 1 << 2;

	// MICGetState / MICSetState ids
	enum : uint32
	{
		kStateSampleRate = 0,
		kStateGainDb = 1,
		kStateGainMin = 2,
		kStateGainMax = 3,
		kStateGainStep = 4,
		kStateMute = 5,
		kStateEchoCancellation = 7,
		kStateAutoSelection = 8,
	};

	struct GameRingBuffer
	{
		uint32be sampleCount;
		MEMPTR<uint16be> samples;
	};
	static_assert(sizeof(GameRingBuffer) == 8);

	struct GameStatus
	{
		uint32be flags;
		uint32be available;
		uint32be readPosition;
	};
	static_assert(sizeof(GameStatus) == 0xC);

	struct Microphone
	{
		bool initialized = false;
		bool open = false;
		uint16be* ring = nullptr;
		uint32 ringSize = 0; // in samples
		uint32 readPos = 0;
		uint32 writePos = 0;
		uint32 echoCancellation = 0;
		uint32 autoSelection = 0;
		uint32 gainDb = 0; // signed 7.8 fixed point
	};

	Microphone g_mics[2];

	bool HasGamePad(uint32 index)
	{
		return InputManager::instance().get_vpad_controller(index) != nullptr;
	}

	// Only GamePad 0 has a usable microphone.
	bool IsConnected(uint32 index)
	{
		return index == 0 && HasGamePad(0);
	}

	uint32 AvailableSamples(const Microphone& mic)
	{
		if (mic.ringSize == 0)
			return 0;
		return (mic.writePos + mic.ringSize - mic.readPos) % mic.ringSize;
	}

	// Handles are the GamePad index. Returns nullptr and sets the error for a
	// bad or uninitialised handle.
	Microphone* Lookup(uint32 handle, sint32& error)
	{
		if (handle > 1)
		{
			error = kBadParam;
			return nullptr;
		}
		if (!g_mics[handle].initialized)
		{
			error = kNotInitialized;
			return nullptr;
		}
		return &g_mics[handle];
	}

	void PushSamples(Microphone& mic, const sint16* samples, sint32 count)
	{
		if (!mic.ring || mic.ringSize == 0)
			return;
		uint32 pos = mic.writePos;
		for (sint32 i = 0; i < count; i++)
		{
			mic.ring[pos] = (uint16)samples[i];
			pos = (pos + 1) % mic.ringSize;
		}
		mic.writePos = pos;
	}

	// Opens the host microphone picked in the settings, once.
	void OpenHostMicrophone()
	{
		constexpr auto api = IAudioInputAPI::Cubeb;
		const auto& config = GetConfig();
		std::unique_lock lock(g_audioInputMutex);
		if (g_inputAudio || !IAudioInputAPI::IsAudioInputAPIAvailable(api))
			return;
		IAudioInputAPI::DeviceDescriptionPtr chosen;
		for (const auto& device : IAudioInputAPI::GetDevices(api))
		{
			if (device->GetIdentifier() == config.input_device)
			{
				chosen = device;
				break;
			}
		}
		if (!chosen)
			return;
		try
		{
			g_inputAudio = IAudioInputAPI::CreateDevice(api, chosen, kSampleRate, 1, kSamplesPerFrame, 16);
			g_inputAudio->SetVolume(config.input_volume);
		}
		catch (const std::runtime_error& e)
		{
			cemuLog_log(LogType::Force, "mic: could not open the host microphone: {}", e.what());
			g_inputAudio.reset();
		}
	}

	void SetHostMicrophoneRunning(bool running)
	{
		std::shared_lock lock(g_audioInputMutex);
		if (!g_inputAudio)
			return;
		if (running)
			g_inputAudio->Play();
		else
			g_inputAudio->Stop();
	}

	sint32 MICInit(uint32 index, uint32 unused, GameRingBuffer* ringBuffer, uint32be* errorOut)
	{
		if (!IsConnected(index))
		{
			*errorOut = (uint32)kNotConnected;
			return -1;
		}
		Microphone& mic = g_mics[index];
		if (mic.initialized)
		{
			*errorOut = (uint32)kAlreadyOpen;
			return -1;
		}
		mic = Microphone{};
		mic.ring = ringBuffer->samples.GetPtr();
		mic.ringSize = ringBuffer->sampleCount;
		mic.echoCancellation = 1;
		mic.autoSelection = 1;
		mic.initialized = true;
		*errorOut = (uint32)kOk;
		OpenHostMicrophone();
		return (sint32)index;
	}

	sint32 MICOpen(uint32 handle)
	{
		sint32 error = kOk;
		Microphone* mic = Lookup(handle, error);
		if (!mic)
			return error;
		if (mic->open)
			return kAlreadyOpen;
		if (!HasGamePad(handle))
			return kNotConnected;
		mic->open = true;
		SetHostMicrophoneRunning(true);
		return kOk;
	}

	sint32 MICClose(uint32 handle)
	{
		sint32 error = kOk;
		Microphone* mic = Lookup(handle, error);
		if (!mic)
			return error;
		if (!mic->open)
			return kAlreadyOpen; // what the system returns for closing twice
		mic->open = false;
		SetHostMicrophoneRunning(false);
		return kOk;
	}

	sint32 MICGetStatus(uint32 handle, GameStatus* status)
	{
		sint32 error = kOk;
		Microphone* mic = Lookup(handle, error);
		if (!mic)
			return error;
		uint32 flags = kFlagPcm16;
		if (IsConnected(handle))
			flags |= kFlagConnected;
		if (mic->open)
			flags |= kFlagOpen;
		status->flags = flags;
		status->available = AvailableSamples(*mic);
		status->readPosition = mic->readPos;
		return kOk;
	}

	sint32 MICGetState(uint32 handle, uint32 state, uint32be* value)
	{
		sint32 error = kOk;
		Microphone* mic = Lookup(handle, error);
		if (!mic)
			return error;
		switch (state)
		{
		case kStateSampleRate: *value = (uint32)kSampleRate; break;
		case kStateGainDb: *value = mic->gainDb; break;
		case kStateGainMin: *value = 0x0000; break; // 0 dB
		case kStateGainMax: *value = 0x0200; break; // 2 dB
		case kStateGainStep: *value = 0x0001; break;
		case kStateEchoCancellation: *value = mic->echoCancellation; break;
		case kStateAutoSelection: *value = mic->autoSelection; break;
		default:
			cemuLog_logDebug(LogType::Force, "MICGetState: state {} not handled", state);
			break;
		}
		return kOk;
	}

	sint32 MICSetState(uint32 handle, uint32 state, uint32 value)
	{
		sint32 error = kOk;
		Microphone* mic = Lookup(handle, error);
		if (!mic)
			return error;
		switch (state)
		{
		case kStateGainDb: mic->gainDb = value; break;
		case kStateEchoCancellation: mic->echoCancellation = value ? 1 : 0; break;
		case kStateAutoSelection: mic->autoSelection = value ? 1 : 0; break;
		default:
			cemuLog_logDebug(LogType::Force, "MICSetState: state {} not handled", state);
			break;
		}
		return kOk;
	}

	sint32 MICSetDataConsumed(uint32 handle, sint32 consumed)
	{
		sint32 error = kOk;
		Microphone* mic = Lookup(handle, error);
		if (!mic)
			return error;
		if (!mic->open)
			return kNotOpen;
		if ((sint32)AvailableSamples(*mic) < consumed)
		{
			mic->readPos = mic->writePos; // drop everything
			return kOverConsumed;
		}
		if (mic->ringSize)
			mic->readPos = (mic->readPos + (uint32)consumed) % mic->ringSize;
		return kOk;
	}

	// A loud two-tone warble, recognisable as "someone is blowing".
	void MakeTestTone(sint16* out, sint32 count)
	{
		static uint32 s_phase = 0;
		for (sint32 i = 0; i < count; i++, s_phase++)
		{
			const double t = (double)s_phase / kSampleRate;
			const double freq = 440.0 + 220.0 * std::sin(t * 6.0);
			out[i] = (sint16)(std::sin(t * freq * 6.283185307179586) * 30000.0);
		}
	}
}

bool mic_isActive(uint32 drcIndex)
{
	return drcIndex < 2 && g_mics[drcIndex].open;
}

// Called by AX once per audio frame.
void mic_updateOnAXFrame()
{
	sint32 active = -1;
	if (mic_isActive(0))
		active = 0;
	else if (mic_isActive(1))
		active = 1;

	std::shared_lock lock(g_audioInputMutex);
	if (active < 0)
	{
		if (g_inputAudio)
			g_inputAudio->Stop();
		return;
	}

	sint16 samples[kSamplesPerFrame];
	if (g_inputAudio)
	{
		g_inputAudio->Play();
		g_inputAudio->ConsumeBlock(samples);
	}
	else
	{
		auto pad = InputManager::instance().get_vpad_controller(active);
		if (pad && pad->is_mic_active())
			MakeTestTone(samples, kSamplesPerFrame);
		else
			memset(samples, 0, sizeof(samples));
	}
	PushSamples(g_mics[0], samples, kSamplesPerFrame);
}

namespace mic
{
	namespace
	{
		class MicLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "mic"; }

			void RPLMapped() override
			{
				cafeExportRegister("mic", MICInit, LogType::Placeholder);
				cafeExportRegister("mic", MICOpen, LogType::Placeholder);
				cafeExportRegister("mic", MICClose, LogType::Placeholder);
				cafeExportRegister("mic", MICGetStatus, LogType::Placeholder);
				cafeExportRegister("mic", MICGetState, LogType::Placeholder);
				cafeExportRegister("mic", MICSetState, LogType::Placeholder);
				cafeExportRegister("mic", MICSetDataConsumed, LogType::Placeholder);
			}

			void RPLUnmapped() override
			{
				g_mics[0] = Microphone{};
				g_mics[1] = Microphone{};
			}
		};

		MicLibrary g_micLibrary;
	}

	COSModule* GetModule()
	{
		return &g_micLibrary;
	}
}
