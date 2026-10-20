// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "XAudio27API.h"

#include "../dependencies/DirectX_2010/XAudio2.h"

static_assert(IAudioAPI::kBlockCount < XAUDIO2_MAX_QUEUED_BUFFERS, "too many xaudio2 buffers");

HMODULE XAudio27API::s_xaudio_dll = nullptr;
std::unique_ptr<IXAudio2, XAudio27API::XAudioDeleter> XAudio27API::s_xaudio;

namespace
{
	constexpr uint32 kPrimaryDevice = (uint32)-1;

	HMODULE LoadRuntime()
	{
#ifdef _DEBUG
		// the debug runtime, when the DirectX SDK is installed
		if (HMODULE debug = LoadLibraryExW(L"XAudioD2_7.DLL", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
			return debug;
#endif
		return LoadLibraryExW(L"XAudio2_7.DLL", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	}
}

XAudio27API::XAudio27API(uint32 device_id, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	if (!s_xaudio)
		throw std::runtime_error("xaudio 2.7 not initialized!");

	// device 0 is the primary device in XAudio 2.7's numbering
	const uint32 device_index = device_id == kPrimaryDevice ? 0 : device_id;

	IXAudio2* engine = nullptr;
	HRESULT hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create xaudio device (hres: {:#x})", (uint32)hr));
	m_xaudio.reset(engine);

	IXAudio2MasteringVoice* mastering = nullptr;
	hr = m_xaudio->CreateMasteringVoice(&mastering, channels, samplerate, 0, device_index);
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create xaudio mastering voice (hres: {:#x})", (uint32)hr));
	m_mastering_voice.reset(mastering);

	// m_wfx was filled in by IAudioAPI for these parameters
	IXAudio2SourceVoice* source = nullptr;
	hr = m_xaudio->CreateSourceVoice(&source, &m_wfx.Format, 0, 1.0f);
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create xaudio source voice (hres: {:#x})", (uint32)hr));
	m_source_voice.reset(source);

	for (auto& buffer : m_audio_buffer)
		buffer = std::make_unique<uint8[]>(m_bytesPerBlock);

	m_xaudio->StartEngine();
}

XAudio27API::~XAudio27API()
{
	if (m_xaudio)
		m_xaudio->StopEngine();
	XAudio27API::Stop();

	// voices before the engine that owns them
	m_source_voice.reset();
	m_mastering_voice.reset();
	m_xaudio.reset();
}

void XAudio27API::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);
	m_mastering_voice->SetVolume((float)volume / 100.0f);
}

bool XAudio27API::Play()
{
	if (!m_playing)
		m_playing = SUCCEEDED(m_source_voice->Start());
	return m_playing;
}

bool XAudio27API::Stop()
{
	if (!m_playing)
		return true;
	m_playing = FAILED(m_source_voice->Stop());
	m_source_voice->FlushSourceBuffers();
	return m_playing;
}

bool XAudio27API::InitializeStatic()
{
	if (s_xaudio)
		return true;

	// the DLL is only loaded to see whether the runtime is installed; XAudio2Create
	// itself goes through COM
	s_xaudio_dll = LoadRuntime();
	IXAudio2* engine = nullptr;
	if (s_xaudio_dll && SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)))
	{
		s_xaudio.reset(engine);
		return true;
	}

	if (s_xaudio_dll)
		FreeLibrary(s_xaudio_dll);
	s_xaudio_dll = nullptr;
	return false;
}

void XAudio27API::Destroy()
{
	s_xaudio.reset();
	if (s_xaudio_dll)
		FreeLibrary(s_xaudio_dll);
}

std::vector<XAudio27API::DeviceDescriptionPtr> XAudio27API::GetDevices()
{
	std::vector<DeviceDescriptionPtr> result{ std::make_shared<XAudio27DeviceDescription>(L"Primary Sound Driver", L"", kPrimaryDevice) };

	UINT32 count = 0;
	if (FAILED(s_xaudio->GetDeviceCount(&count)))
		return result;

	for (UINT32 index = 0; index < count; ++index)
	{
		XAUDIO2_DEVICE_DETAILS details;
		if (SUCCEEDED(s_xaudio->GetDeviceDetails(index, &details)))
			result.emplace_back(std::make_shared<XAudio27DeviceDescription>(details.DisplayName, details.DeviceID, index));
	}
	return result;
}

// voices are destroyed, the engine itself is a COM object
void XAudio27API::VoiceDeleter::operator()(IXAudio2Voice* ptr) const
{
	if (ptr != nullptr)
		ptr->DestroyVoice();
}

void XAudio27API::XAudioDeleter::operator()(IXAudio2* ptr) const
{
	if (ptr != nullptr)
		ptr->Release();
}

bool XAudio27API::FeedBlock(sint16* data)
{
	if (m_blocks_queued >= kBlockCount)
	{
		m_blocks_queued = GetQueuedBuffers();
		if (m_blocks_queued >= kBlockCount)
		{
			cemuLog_logDebug(LogType::Force, "dropped xaudio2 block since too many buffers are queued");
			return false;
		}
	}

	uint8* slot = m_audio_buffer[m_next_buffer].get();
	std::memcpy(slot, data, m_bytesPerBlock);

	XAUDIO2_BUFFER submit{};
	submit.AudioBytes = m_bytesPerBlock;
	submit.pAudioData = slot;
	m_source_voice->SubmitSourceBuffer(&submit);

	m_next_buffer = (m_next_buffer + 1) % kBlockCount;
	++m_blocks_queued;
	return true;
}

uint32 XAudio27API::GetQueuedBuffers() const
{
	XAUDIO2_VOICE_STATE state{};
	m_source_voice->GetState(&state);
	return state.BuffersQueued;
}

bool XAudio27API::NeedAdditionalBlocks() const
{
	return GetQueuedBuffers() < GetAudioDelay();
}
