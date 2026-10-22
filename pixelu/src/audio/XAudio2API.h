// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#define DIRECTSOUND_VERSION 0x0800
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <wrl/client.h>

#include "IAudioAPI.h"

struct IXAudio2;
struct IXAudio2Voice;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

// Output through XAudio 2.8+ (the copy that ships with Windows 8 and later). Blocks
// are copied into a ring of kBlockCount buffers and submitted to one source voice.
class XAudio2API : public IAudioAPI
{
public:
	class XAudio2DeviceDescription : public DeviceDescription
	{
	public:
		XAudio2DeviceDescription(const std::wstring& name, std::wstring device_id)
			: DeviceDescription(name), m_device_id(std::move(device_id)) {}

		std::wstring GetIdentifier() const override { return m_device_id.empty() ? L"default" : m_device_id; }
		const std::wstring& GetDeviceId() const { return m_device_id; } // empty: the default device

	private:
		std::wstring m_device_id;
	};
	using XAudio2DeviceDescriptionPtr = std::shared_ptr<XAudio2DeviceDescription>;

	AudioAPI GetType() const override { return XAudio2; }

	XAudio2API(std::wstring device_id, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	~XAudio2API();
	void SetVolume(sint32 volume) override;

	bool Play() override;
	bool Stop() override;
	bool FeedBlock(sint16* data) override;
	bool NeedAdditionalBlocks() const override;

	static bool InitializeStatic();
	static void Destroy();
	static const std::vector<DeviceDescriptionPtr>& GetDevices() { return s_devices; }

private:
	uint32 GetQueuedBuffers() const;

	// lists the audio endpoints (via WMI) under the ids XAudio2 wants
	static const std::vector<DeviceDescriptionPtr>& RefreshDevices();

	struct VoiceDeleter
	{
		void operator()(IXAudio2Voice* ptr) const;
	};

	static HMODULE s_xaudio_dll;
	static std::vector<DeviceDescriptionPtr> s_devices;

	// destroyed bottom up: the voices go before the engine
	Microsoft::WRL::ComPtr<IXAudio2> m_xaudio;
	std::wstring m_device_id;
	std::unique_ptr<IXAudio2MasteringVoice, VoiceDeleter> m_mastering_voice;
	std::unique_ptr<IXAudio2SourceVoice, VoiceDeleter> m_source_voice;

	std::unique_ptr<uint8[]> m_audio_buffer[kBlockCount];
	uint32 m_next_buffer = 0;   // ring slot the next block goes to
	uint32 m_blocks_queued = 0; // our count, refreshed from the voice when it looks full
};
