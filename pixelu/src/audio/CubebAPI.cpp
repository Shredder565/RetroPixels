// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "CubebAPI.h"

#if BOOST_OS_WINDOWS
#include <combaseapi.h>
#include <mmreg.h>
#include <mmsystem.h>
#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "ksuser.lib")
#endif

namespace
{
	void OnStreamState(cubeb_stream*, void*, cubeb_state)
	{
		// nothing to do: playback state is tracked by Play()/Stop()
	}

	cubeb_channel_layout OutputLayout(uint32 channels)
	{
		switch (channels)
		{
		case 2: return CUBEB_LAYOUT_STEREO;
		case 4: return CUBEB_LAYOUT_QUAD;
		case 6: return CUBEB_LAYOUT_3F2_LFE_BACK;
		case 8: return CUBEB_LAYOUT_3F4_LFE;
		default: return CUBEB_LAYOUT_MONO;
		}
	}
}

long CubebAPI::FillOutput(cubeb_stream*, void* user, const void*, void* output, long frames)
{
	auto* self = static_cast<CubebAPI*>(user);
	const size_t wanted = (size_t)frames * self->m_channels * (self->m_bitsPerSample / 8);
	auto* out = static_cast<uint8*>(output);

	size_t taken = 0;
	{
		std::unique_lock lock(self->m_mutex);
		taken = std::min(wanted, self->m_buffer.size());
		if (taken > 0)
		{
			std::memcpy(out, self->m_buffer.data(), taken);
			self->m_buffer.erase(self->m_buffer.begin(), self->m_buffer.begin() + taken);
		}
	}
	// an underrun plays silence
	if (taken < wanted)
		std::memset(out + taken, 0, wanted - taken);
	return frames;
}

CubebAPI::CubebAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	cubeb_stream_params params{};
	params.format = CUBEB_SAMPLE_S16LE;
	params.rate = samplerate;
	params.channels = channels;
	params.layout = OutputLayout(channels);
	params.prefs = CUBEB_STREAM_PREF_NONE;

	uint32 latency = 1; // frames; kept when cubeb can't tell
	cubeb_get_min_latency(s_context, &params, &latency);

	m_bufferLimit = (size_t)m_bytesPerBlock * kBlockCount;
	m_buffer.reserve(m_bufferLimit);

	if (cubeb_stream_init(s_context, &m_stream, "PixelU audio output", nullptr, nullptr, devid, &params, latency,
	                      &CubebAPI::FillOutput, &OnStreamState, this) != CUBEB_OK)
		throw std::runtime_error("can't initialize cubeb device");
}

CubebAPI::~CubebAPI()
{
	if (!m_stream)
		return;
	Stop();
	cubeb_stream_destroy(m_stream);
}

bool CubebAPI::NeedAdditionalBlocks() const
{
	std::shared_lock lock(m_mutex);
	return m_buffer.size() < (size_t)GetAudioDelay() * m_bytesPerBlock;
}

bool CubebAPI::FeedBlock(sint16* data)
{
	std::unique_lock lock(m_mutex);
	if (m_buffer.size() + m_bytesPerBlock >= m_bufferLimit)
	{
		cemuLog_logDebug(LogType::Force, "dropped cubeb block since too many buffers are queued");
		return false;
	}
	const auto* bytes = reinterpret_cast<const uint8*>(data);
	m_buffer.insert(m_buffer.end(), bytes, bytes + m_bytesPerBlock);
	return true;
}

bool CubebAPI::Play()
{
	if (!m_is_playing && cubeb_stream_start(m_stream) == CUBEB_OK)
		m_is_playing = true;
	return m_is_playing;
}

bool CubebAPI::Stop()
{
	if (m_is_playing && cubeb_stream_stop(m_stream) == CUBEB_OK)
		m_is_playing = false;
	return !m_is_playing;
}

void CubebAPI::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);
	cubeb_stream_set_volume(m_stream, (float)volume / 100.0f);
}

bool CubebAPI::InitializeStatic()
{
	if (cubeb_init(&s_context, "PixelU", nullptr) != CUBEB_OK)
	{
		cemuLog_log(LogType::Force, "can't create cubeb audio api");
		return false;
	}
	return true;
}

void CubebAPI::Destroy()
{
	if (s_context)
		cubeb_destroy(s_context);
}

std::vector<IAudioAPI::DeviceDescriptionPtr> CubebAPI::GetDevices()
{
	// the system default comes first and is always there
	std::vector<DeviceDescriptionPtr> result{ std::make_shared<CubebDeviceDescription>(nullptr, "default", L"Default Device") };

	cubeb_device_collection collection{};
	if (cubeb_enumerate_devices(s_context, CUBEB_DEVICE_TYPE_OUTPUT, &collection) != CUBEB_OK)
		return result;

	for (size_t i = 0; i < collection.count; ++i)
	{
		const cubeb_device_info& info = collection.device[i];
		if (info.state != CUBEB_DEVICE_STATE_ENABLED)
			continue;
		result.emplace_back(std::make_shared<CubebDeviceDescription>(info.devid, info.device_id, boost::nowide::widen(info.friendly_name)));
	}
	cubeb_device_collection_destroy(s_context, &collection);
	return result;
}
