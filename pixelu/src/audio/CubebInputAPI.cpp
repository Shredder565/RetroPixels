// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "CubebInputAPI.h"

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
	}

	cubeb_channel_layout InputLayout(uint32 channels)
	{
		switch (channels)
		{
		case 2: return CUBEB_LAYOUT_STEREO;
		case 4: return CUBEB_LAYOUT_QUAD;
		case 6: return CUBEB_LAYOUT_QUAD_LFE | CHANNEL_FRONT_CENTER;
		case 8: return CUBEB_LAYOUT_3F4_LFE;
		default: return CUBEB_LAYOUT_MONO;
		}
	}
}

long CubebInputAPI::TakeInput(cubeb_stream*, void* user, const void* input, void*, long frames)
{
	auto* self = static_cast<CubebInputAPI*>(user);
	const size_t size = (size_t)frames * self->m_channels * (self->m_bitsPerSample / 8);

	std::unique_lock lock(self->m_mutex);
	if (self->m_buffer.size() + size >= self->m_bufferLimit)
	{
		cemuLog_logDebug(LogType::Force, "dropped input sound block since too many buffers are queued");
		return frames; // keep the stream running, lose this part
	}
	const auto* bytes = static_cast<const uint8*>(input);
	self->m_buffer.insert(self->m_buffer.end(), bytes, bytes + size);
	return frames;
}

CubebInputAPI::CubebInputAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioInputAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	cubeb_stream_params params{};
	params.format = CUBEB_SAMPLE_S16LE;
	params.rate = samplerate;
	params.channels = channels;
	params.layout = InputLayout(channels);
	params.prefs = CUBEB_STREAM_PREF_NONE;

	uint32 latency = 1;
	cubeb_get_min_latency(s_context, &params, &latency);

	m_bufferLimit = (size_t)m_bytesPerBlock * kBlockCount;
	m_buffer.reserve(m_bufferLimit);

	if (cubeb_stream_init(s_context, &m_stream, "Cemu Cubeb input", devid, &params, nullptr, nullptr, latency,
	                      &CubebInputAPI::TakeInput, &OnStreamState, this) != CUBEB_OK)
		throw std::runtime_error("can't initialize cubeb device");
}

CubebInputAPI::~CubebInputAPI()
{
	if (!m_stream)
		return;
	Stop();
	cubeb_stream_destroy(m_stream);
}

bool CubebInputAPI::ConsumeBlock(sint16* data)
{
	auto* out = reinterpret_cast<uint8*>(data);
	size_t taken = 0;
	{
		std::unique_lock lock(m_mutex);
		taken = std::min<size_t>(m_bytesPerBlock, m_buffer.size());
		if (taken > 0)
		{
			std::memcpy(out, m_buffer.data(), taken);
			m_buffer.erase(m_buffer.begin(), m_buffer.begin() + taken);
		}
	}
	if (taken < m_bytesPerBlock)
		std::memset(out + taken, 0, m_bytesPerBlock - taken); // not enough recorded yet
	return true;
}

bool CubebInputAPI::Play()
{
	if (!m_is_playing && cubeb_stream_start(m_stream) == CUBEB_OK)
		m_is_playing = true;
	return m_is_playing;
}

bool CubebInputAPI::Stop()
{
	if (m_is_playing && cubeb_stream_stop(m_stream) == CUBEB_OK)
		m_is_playing = false;
	return !m_is_playing;
}

void CubebInputAPI::SetVolume(sint32 volume)
{
	IAudioInputAPI::SetVolume(volume);
	cubeb_stream_set_volume(m_stream, (float)volume / 100.0f);
}

bool CubebInputAPI::InitializeStatic()
{
	if (cubeb_init(&s_context, "Cemu Input Cubeb", nullptr) != CUBEB_OK)
	{
		cemuLog_log(LogType::Force, "can't create cubeb audio api");
		return false;
	}
	return true;
}

void CubebInputAPI::Destroy()
{
	if (s_context)
		cubeb_destroy(s_context);
}

std::vector<IAudioInputAPI::DeviceDescriptionPtr> CubebInputAPI::GetDevices()
{
	std::vector<DeviceDescriptionPtr> result{ std::make_shared<CubebDeviceDescription>(nullptr, "default", L"Default Device") };

	cubeb_device_collection collection{};
	if (cubeb_enumerate_devices(s_context, CUBEB_DEVICE_TYPE_INPUT, &collection) != CUBEB_OK)
		return result;

	for (size_t i = 0; i < collection.count; ++i)
	{
		const cubeb_device_info& info = collection.device[i];
		if (info.state == CUBEB_DEVICE_STATE_ENABLED)
			result.emplace_back(std::make_shared<CubebDeviceDescription>(info.devid, info.device_id, boost::nowide::widen(info.friendly_name)));
	}
	cubeb_device_collection_destroy(s_context, &collection);
	return result;
}
