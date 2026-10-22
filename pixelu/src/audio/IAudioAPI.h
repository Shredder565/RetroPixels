// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Sound output. The emulated audio hardware hands finished blocks of 16 bit PCM to
// one IAudioAPI per output (TV, GamePad, Skylander portal); each backend queues them
// and plays them in order. Recording taps the blocks in snd_core, not here.

#if BOOST_OS_WINDOWS
#include <mmreg.h>
#endif

#include "config/CemuConfig.h"

class IAudioAPI
{
	friend class GeneralSettings2; // edits s_audioDelay live

public:
	// one output device a backend can open; equal when the identifiers match
	class DeviceDescription
	{
	public:
		explicit DeviceDescription(std::wstring name)
			: m_name(std::move(name)) {}
		virtual ~DeviceDescription() = default;

		// what the config stores to find the device again
		virtual std::wstring GetIdentifier() const = 0;
		const std::wstring& GetName() const { return m_name; }

		bool operator==(const DeviceDescription& o) const { return GetIdentifier() == o.GetIdentifier(); }

	private:
		std::wstring m_name;
	};
	using DeviceDescriptionPtr = std::shared_ptr<DeviceDescription>;

	enum AudioType
	{
		TV = 0,
		Gamepad,
		Portal
	};

	// stored in the config as a number, keep the order
	enum AudioAPI
	{
		DirectSound = 0,
		XAudio27,
		XAudio2,
		Cubeb,

		AudioAPIEnd,
	};

	// most blocks a backend keeps queued
	static constexpr uint32 kBlockCount = 24;

	IAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	virtual ~IAudioAPI() = default;
	virtual AudioAPI GetType() const = 0;

	sint32 GetChannels() const { return m_channels; }

	virtual sint32 GetVolume() const { return m_volume; }
	virtual void SetVolume(sint32 volume) { m_volume = volume; } // 0..100
	virtual void SetInputVolume(sint32 volume) { m_inputVolume = volume; }

	// fewer blocks queued than the audio delay asks for
	virtual bool NeedAdditionalBlocks() const = 0;
	// queues one block of m_bytesPerBlock bytes; false when it was dropped
	virtual bool FeedBlock(sint16* data) = 0;
	virtual bool Play() = 0;
	virtual bool Stop() = 0;

	// a per device delay (in blocks) instead of the global one; 0 to use the global one
	void SetAudioDelayOverride(uint32 delay);
	uint32 GetAudioDelay() const;

	static void PrintLogging();
	static void InitializeStatic();
	static bool IsAudioAPIAvailable(AudioAPI api);

	// opens the device the config selects for `type`; null when none is selected
	static std::unique_ptr<IAudioAPI> CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 samples_per_block, sint32 bits_per_sample);
	static std::unique_ptr<IAudioAPI> CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample);
	static std::unique_ptr<IAudioAPI> CreateDevice(AudioAPI api, const DeviceDescriptionPtr& device, sint32 samplerate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample);
	static std::vector<DeviceDescriptionPtr> GetDevices(AudioAPI api);

protected:
#if BOOST_OS_WINDOWS
	WAVEFORMATEXTENSIBLE m_wfx{}; // the PCM format, for the Windows backends
#endif

	uint32 m_samplerate, m_channels, m_samplesPerBlock, m_bitsPerSample;
	uint32 m_bytesPerBlock;

	sint32 m_volume = 0, m_inputVolume = 0;
	bool m_playing = false;

	static std::array<bool, AudioAPIEnd> s_availableApis;
	uint32 m_audioDelayOverride = 0;

private:
	static uint32 s_audioDelay; // in blocks
	void InitWFX(sint32 samplerate, sint32 channels, sint32 bits_per_sample);
	static AudioChannels AudioTypeToChannels(AudioType type);
	static std::wstring GetDeviceFromType(AudioType type);
	static sint32 GetVolumeFromType(AudioType type);
};

using AudioAPIPtr = std::unique_ptr<IAudioAPI>;
extern std::shared_mutex g_audioMutex; // guards the three device pointers
extern AudioAPIPtr g_tvAudio;

extern AudioAPIPtr g_padAudio;
extern std::atomic_int32_t g_padVolume;

extern AudioAPIPtr g_portalAudio;
