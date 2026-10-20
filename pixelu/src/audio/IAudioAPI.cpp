// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IAudioAPI.h"

#if BOOST_OS_WINDOWS
#include "XAudio2API.h"
#include "XAudio27API.h"
#include "DirectSoundAPI.h"
#endif
#include "config/CemuConfig.h"
#if HAS_CUBEB
#include "CubebAPI.h"
#endif

std::array<bool, IAudioAPI::AudioAPIEnd> IAudioAPI::s_availableApis{};
uint32 IAudioAPI::s_audioDelay = 2;

// one output per device, all guarded by g_audioMutex
AudioAPIPtr g_tvAudio, g_padAudio, g_portalAudio;
std::shared_mutex g_audioMutex;
std::atomic_int32_t g_padVolume = 0;

namespace
{
	const char* Availability(bool available)
	{
		return available ? "available" : "not supported";
	}

	// the device a backend description really is, or an error for a mismatched one
	template<typename TDescription>
	std::shared_ptr<TDescription> As(const IAudioAPI::DeviceDescriptionPtr& device)
	{
		auto result = std::dynamic_pointer_cast<TDescription>(device);
		if (!result)
			throw std::runtime_error("audio device does not belong to the selected audio api");
		return result;
	}

	[[noreturn]] void InvalidApi(IAudioAPI::AudioAPI api)
	{
		throw std::runtime_error(fmt::format("invalid audio api: {}", (int)api));
	}
}

IAudioAPI::IAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: m_samplerate(samplerate), m_channels(channels), m_samplesPerBlock(samples_per_block), m_bitsPerSample(bits_per_sample),
	  m_bytesPerBlock(samples_per_block * channels * (bits_per_sample / 8))
{
	InitWFX(m_samplerate, m_channels, m_bitsPerSample);
}

void IAudioAPI::PrintLogging()
{
	cemuLog_log(LogType::Force, "------- Init Audio backend -------");
	cemuLog_log(LogType::Force, "DirectSound: {}", Availability(s_availableApis[DirectSound]));
	cemuLog_log(LogType::Force, "XAudio 2.8: {}", Availability(s_availableApis[XAudio2]));
	if (!s_availableApis[XAudio2]) // 2.7 is only tried without 2.8
		cemuLog_log(LogType::Force, "XAudio 2.7: {}", Availability(s_availableApis[XAudio27]));
	cemuLog_log(LogType::Force, "Cubeb: {}", Availability(s_availableApis[Cubeb]));
}

void IAudioAPI::InitWFX(sint32 samplerate, sint32 channels, sint32 bits_per_sample)
{
#if BOOST_OS_WINDOWS
	WAVEFORMATEX& format = m_wfx.Format;
	format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	format.nChannels = (WORD)channels;
	format.nSamplesPerSec = (DWORD)samplerate;
	format.wBitsPerSample = (WORD)bits_per_sample;
	format.nBlockAlign = (WORD)((channels * bits_per_sample) / 8); // one frame
	format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
	format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

	m_wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	m_wfx.Samples.wValidBitsPerSample = (WORD)bits_per_sample;

	constexpr DWORD kStereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	constexpr DWORD kQuad = kStereo | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
	constexpr DWORD kSurround51 = kQuad | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY;
	constexpr DWORD kSurround71 = kSurround51 | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER;
	switch (channels)
	{
	case 2: m_wfx.dwChannelMask = kStereo; break;
	case 4: m_wfx.dwChannelMask = kQuad; break;
	case 6: m_wfx.dwChannelMask = kSurround51; break;
	case 8: m_wfx.dwChannelMask = kSurround71; break;
	default: m_wfx.dwChannelMask = 0; break; // mono and odd counts: let the system pick
	}
#endif
}

void IAudioAPI::InitializeStatic()
{
	s_audioDelay = GetConfig().audio_delay;

#if BOOST_OS_WINDOWS
	s_availableApis[DirectSound] = true;
	s_availableApis[XAudio2] = XAudio2API::InitializeStatic();
	// XAudio 2.7 (DirectX SDK runtime) is the fallback for systems without 2.8
	if (!s_availableApis[XAudio2])
		s_availableApis[XAudio27] = XAudio27API::InitializeStatic();
#endif
#if HAS_CUBEB
	s_availableApis[Cubeb] = CubebAPI::InitializeStatic();
#endif
}

bool IAudioAPI::IsAudioAPIAvailable(AudioAPI api)
{
	if ((size_t)api >= s_availableApis.size())
	{
		cemu_assert_debug(false);
		return false;
	}
	return s_availableApis[api];
}

AudioAPIPtr IAudioAPI::CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 samples_per_block, sint32 bits_per_sample)
{
	const sint32 channels = CemuConfig::AudioChannelsToNChannels(AudioTypeToChannels(type));
	return CreateDeviceFromConfig(type, rate, channels, samples_per_block, bits_per_sample);
}

AudioAPIPtr IAudioAPI::CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample)
{
	const auto api = (AudioAPI)GetConfig().audio_api;
	const std::wstring wanted = GetDeviceFromType(type);
	if (wanted.empty())
		return {}; // this output is switched off

	DeviceDescriptionPtr device;
	if (IsAudioAPIAvailable(api))
	{
		for (const auto& candidate : GetDevices(api))
		{
			if (candidate->GetIdentifier() == wanted)
			{
				device = candidate;
				break;
			}
		}
	}
	if (!device)
		throw std::runtime_error("failed to find selected device while trying to create audio device");

	AudioAPIPtr result = CreateDevice(api, device, rate, channels, samples_per_block, bits_per_sample);
	result->SetVolume(GetVolumeFromType(type));
	return result;
}

AudioAPIPtr IAudioAPI::CreateDevice(AudioAPI api, const DeviceDescriptionPtr& device, sint32 samplerate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample)
{
	if (!IsAudioAPIAvailable(api))
		return {};

	switch (api)
	{
#if BOOST_OS_WINDOWS
	case DirectSound:
		return std::make_unique<DirectSoundAPI>(As<DirectSoundAPI::DirectSoundDeviceDescription>(device)->GetGUID(), samplerate, channels, samples_per_block, bits_per_sample);
	case XAudio27:
		return std::make_unique<XAudio27API>(As<XAudio27API::XAudio27DeviceDescription>(device)->GetDeviceId(), samplerate, channels, samples_per_block, bits_per_sample);
	case XAudio2:
		return std::make_unique<XAudio2API>(As<XAudio2API::XAudio2DeviceDescription>(device)->GetDeviceId(), samplerate, channels, samples_per_block, bits_per_sample);
#endif
#if HAS_CUBEB
	case Cubeb:
		return std::make_unique<CubebAPI>(As<CubebAPI::CubebDeviceDescription>(device)->GetDeviceId(), samplerate, channels, samples_per_block, bits_per_sample);
#endif
	default:
		InvalidApi(api);
	}
}

std::vector<IAudioAPI::DeviceDescriptionPtr> IAudioAPI::GetDevices(AudioAPI api)
{
	if (!IsAudioAPIAvailable(api))
		return {};

	switch (api)
	{
#if BOOST_OS_WINDOWS
	case DirectSound:
		return DirectSoundAPI::GetDevices();
	case XAudio27:
		return XAudio27API::GetDevices();
	case XAudio2:
		return XAudio2API::GetDevices();
#endif
#if HAS_CUBEB
	case Cubeb:
		return CubebAPI::GetDevices();
#endif
	default:
		InvalidApi(api);
	}
}

void IAudioAPI::SetAudioDelayOverride(uint32 delay)
{
	m_audioDelayOverride = delay;
}

uint32 IAudioAPI::GetAudioDelay() const
{
	if (m_audioDelayOverride != 0)
		return m_audioDelayOverride;
	return s_audioDelay;
}

AudioChannels IAudioAPI::AudioTypeToChannels(AudioType type)
{
	const auto& config = GetConfig();
	if (type == TV)
		return config.tv_channels;
	if (type == Gamepad)
		return config.pad_channels;
	return kMono; // the portal speaker is mono
}

std::wstring IAudioAPI::GetDeviceFromType(AudioType type)
{
	const auto& config = GetConfig();
	switch (type)
	{
	case TV: return config.tv_device;
	case Gamepad: return config.pad_device;
	case Portal: return config.portal_device;
	}
	return {};
}

sint32 IAudioAPI::GetVolumeFromType(AudioType type)
{
	const auto& config = GetConfig();
	switch (type)
	{
	case TV: return config.tv_volume;
	case Gamepad: return config.pad_volume;
	case Portal: return config.portal_volume;
	}
	return 0;
}
