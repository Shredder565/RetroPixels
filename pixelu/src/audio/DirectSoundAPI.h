// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#define DIRECTSOUND_VERSION 0x0800
#include <mmsystem.h>
#include <dsound.h>
#include <wrl/client.h>

#include "IAudioAPI.h"

// Output through DirectSound 8. The secondary buffer is a ring of kBufferCount blocks
// played in a loop; whenever playback enters a block, a thread refills the block
// after it with the oldest queued one (or silence).
class DirectSoundAPI : public IAudioAPI
{
public:
	class DirectSoundDeviceDescription : public DeviceDescription
	{
	public:
		DirectSoundDeviceDescription(const std::wstring& name, GUID* guid)
			: DeviceDescription(name), m_guid(guid) {}

		std::wstring GetIdentifier() const override;
		GUID* GetGUID() const { return m_guid; } // null: the primary device

	private:
		GUID* m_guid;
	};
	using DirectSoundDeviceDescriptionPtr = std::shared_ptr<DirectSoundDeviceDescription>;

	DirectSoundAPI(GUID* guid, sint32 samplerate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample);
	~DirectSoundAPI();

	AudioAPI GetType() const override { return DirectSound; }

	bool Play() override;
	bool Stop() override;
	bool FeedBlock(sint16* data) override;
	void SetVolume(sint32 volume) override;
	bool NeedAdditionalBlocks() const override;

	static std::vector<DeviceDescriptionPtr> GetDevices();
	static std::vector<DeviceDescriptionPtr> GetInputDevices();

private:
	static constexpr uint32 kBufferCount = 4; // blocks in the DirectSound ring

	void RefillThread();
	void WriteBlock(uint32 ring_index);

	Microsoft::WRL::ComPtr<IDirectSound8> m_direct_sound;
	Microsoft::WRL::ComPtr<IDirectSoundBuffer8> m_sound_buffer;
	Microsoft::WRL::ComPtr<IDirectSoundNotify8> m_notify;
	DWORD m_sound_buffer_size = 0;

	std::array<HANDLE, kBufferCount> m_notify_event{}; // set when playback reaches block i

	mutable std::shared_mutex m_mutex;
	std::queue<std::unique_ptr<uint8[]>> m_buffer; // blocks waiting for the ring

	std::thread m_thread;
	std::atomic_bool m_running = false;
};
