// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "DirectSoundAPI.h"

#include "util/helpers/helpers.h"
#include "WindowSystem.h"

#pragma comment(lib, "Dsound.lib")

namespace
{
	// locks a range of the buffer, restoring it once if it was lost
	HRESULT LockRange(IDirectSoundBuffer8* buffer, DWORD offset, DWORD size, void** p1, DWORD* n1, void** p2, DWORD* n2, DWORD flags = 0)
	{
		HRESULT hr = buffer->Lock(offset, size, p1, n1, p2, n2, flags);
		if (hr == DSERR_BUFFERLOST)
		{
			buffer->Restore();
			hr = buffer->Lock(offset, size, p1, n1, p2, n2, flags);
		}
		return hr;
	}

	struct EnumContext
	{
		std::vector<IAudioAPI::DeviceDescriptionPtr>* list;
	};

	BOOL CALLBACK AddDevice(LPGUID guid, LPCWSTR description, LPCWSTR /*module*/, LPVOID context)
	{
		auto* list = static_cast<EnumContext*>(context)->list;
		list->emplace_back(std::make_shared<DirectSoundAPI::DirectSoundDeviceDescription>(description, guid));
		return TRUE;
	}
}

std::wstring DirectSoundAPI::DirectSoundDeviceDescription::GetIdentifier() const
{
	if (!m_guid)
		return L"default";
	return WStringFromGUID(*m_guid);
}

DirectSoundAPI::DirectSoundAPI(GUID* guid, sint32 samplerate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	if (DirectSoundCreate8(guid, m_direct_sound.GetAddressOf(), nullptr) != DS_OK)
		throw std::runtime_error("can't create directsound device");

	const HWND window = static_cast<HWND>(WindowSystem::GetWindowInfo().window_main.surface);
	if (FAILED(m_direct_sound->SetCooperativeLevel(window, DSSCL_PRIORITY)))
		throw std::runtime_error("can't set directsound priority");

	DSBUFFERDESC desc{};
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLPOSITIONNOTIFY;
	desc.dwBufferBytes = kBufferCount * m_bytesPerBlock;
	desc.lpwfxFormat = reinterpret_cast<LPWAVEFORMATEX>(&m_wfx);

	Microsoft::WRL::ComPtr<IDirectSoundBuffer> buffer;
	if (FAILED(m_direct_sound->CreateSoundBuffer(&desc, buffer.GetAddressOf(), nullptr)))
		throw std::runtime_error("can't create directsound soundbuffer");

	DSBCAPS caps{};
	caps.dwSize = sizeof(caps);
	if (FAILED(buffer->GetCaps(&caps)))
		throw std::runtime_error("can't get directsound soundbuffer size");
	m_sound_buffer_size = caps.dwBufferBytes;

	if (FAILED(buffer->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(m_sound_buffer.GetAddressOf()))))
		throw std::runtime_error("can't get directsound buffer interface");
	if (FAILED(buffer->QueryInterface(IID_IDirectSoundNotify8, reinterpret_cast<void**>(m_notify.GetAddressOf()))))
		throw std::runtime_error("can't get directsound notify interface");

	// start from silence
	void* part1 = nullptr;
	void* part2 = nullptr;
	DWORD size1 = 0, size2 = 0;
	if (SUCCEEDED(m_sound_buffer->Lock(0, m_sound_buffer_size, &part1, &size1, &part2, &size2, 0)))
	{
		std::memset(part1, 0, size1);
		if (part2 && size2)
			std::memset(part2, 0, size2);
		m_sound_buffer->Unlock(part1, size1, part2, size2);
	}
	m_sound_buffer->SetCurrentPosition(0);

	DSBPOSITIONNOTIFY positions[kBufferCount]{};
	for (uint32 i = 0; i < kBufferCount; ++i)
	{
		m_notify_event[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto reset
		positions[i].hEventNotify = m_notify_event[i];
		positions[i].dwOffset = i * m_bytesPerBlock; // start of block i
	}
	if (FAILED(m_notify->SetNotificationPositions(kBufferCount, positions)))
		throw std::runtime_error("can't set directsound notify positions");

	m_running = true;
	m_thread = std::thread(&DirectSoundAPI::RefillThread, this);
	SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
}

DirectSoundAPI::~DirectSoundAPI()
{
	m_running = false;
	DirectSoundAPI::Stop();
	if (m_thread.joinable())
		m_thread.join();

	for (HANDLE event : m_notify_event)
	{
		if (event)
			CloseHandle(event);
	}
}

void DirectSoundAPI::RefillThread()
{
	while (m_running)
	{
		// wake up now and then to notice m_running
		const DWORD signaled = WaitForMultipleObjects(kBufferCount, m_notify_event.data(), FALSE, 10);
		if (signaled >= WAIT_OBJECT_0 && signaled < WAIT_OBJECT_0 + kBufferCount)
		{
			// playback just entered this block: the next one is free to write
			WriteBlock((signaled - WAIT_OBJECT_0 + 1) % kBufferCount);
		}
	}
}

void DirectSoundAPI::WriteBlock(uint32 ring_index)
{
	void* part1 = nullptr;
	void* part2 = nullptr;
	DWORD size1 = 0, size2 = 0;
	if (FAILED(LockRange(m_sound_buffer.Get(), ring_index * m_bytesPerBlock, m_bytesPerBlock, &part1, &size1, &part2, &size2)))
	{
		cemuLog_log(LogType::Force, "DirectSound: Dropped audio block due to locking failure");
		return;
	}

	{
		std::unique_lock lock(m_mutex);
		if (m_buffer.empty())
		{
			std::memset(part1, 0, size1);
			if (part2)
				std::memset(part2, 0, size2);
		}
		else
		{
			const uint8* block = m_buffer.front().get();
			std::memcpy(part1, block, size1);
			if (part2)
				std::memcpy(part2, block + size1, size2);
			m_buffer.pop();
		}
	}
	m_sound_buffer->Unlock(part1, size1, part2, size2);
}

bool DirectSoundAPI::Play()
{
	if (!m_playing)
		m_playing = SUCCEEDED(m_sound_buffer->Play(0, 0, DSBPLAY_LOOPING));
	return m_playing;
}

bool DirectSoundAPI::Stop()
{
	if (!m_playing)
		return true;
	// still "playing" when stopping failed
	m_playing = FAILED(m_sound_buffer->Stop());
	return m_playing;
}

bool DirectSoundAPI::FeedBlock(sint16* data)
{
	std::lock_guard lock(m_mutex);
	if (m_buffer.size() > kBlockCount)
	{
		cemuLog_logDebug(LogType::Force, "dropped direct sound block since too many buffers are queued");
		return false;
	}
	auto block = std::make_unique<uint8[]>(m_bytesPerBlock);
	std::memcpy(block.get(), data, m_bytesPerBlock);
	m_buffer.push(std::move(block));
	return true;
}

void DirectSoundAPI::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);

	// DirectSound volume is attenuation in 1/100 dB; a 5th root makes the
	// slider feel roughly linear
	const float curve = std::pow((float)volume / 100.0f, 0.20f);
	const LONG attenuation = (LONG)(curve * (DSBVOLUME_MAX - DSBVOLUME_MIN) + DSBVOLUME_MIN);
	m_sound_buffer->SetVolume(attenuation);
}

bool DirectSoundAPI::NeedAdditionalBlocks() const
{
	std::shared_lock lock(m_mutex);
	return m_buffer.size() < GetAudioDelay();
}

std::vector<DirectSoundAPI::DeviceDescriptionPtr> DirectSoundAPI::GetDevices()
{
	std::vector<DeviceDescriptionPtr> result;
	EnumContext context{ &result };
	DirectSoundEnumerateW(&AddDevice, &context);

	// only the primary device listed means there is no real output
	if (result.size() == 1 && result.front()->GetIdentifier() == L"default")
		result.clear();
	return result;
}

std::vector<DirectSoundAPI::DeviceDescriptionPtr> DirectSoundAPI::GetInputDevices()
{
	std::vector<DeviceDescriptionPtr> result;
	EnumContext context{ &result };
	DirectSoundCaptureEnumerateW(&AddDevice, &context);
	return result;
}
