// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "IAudioAPI.h"

#include <cubeb/cubeb.h>

#include <memory>

// Output through cubeb (all platforms). Cubeb pulls audio from its own thread, so
// blocks wait in a byte queue until its callback takes them.
class CubebAPI : public IAudioAPI
{
public:
	class CubebDeviceDescription : public DeviceDescription
	{
	public:
		CubebDeviceDescription(cubeb_devid devid, std::string device_id, const std::wstring& name)
			: DeviceDescription(name), m_devid(devid), m_device_id(std::move(device_id)) {}

		std::wstring GetIdentifier() const override { return boost::nowide::widen(m_device_id); }
		cubeb_devid GetDeviceId() const { return m_devid; } // null: the system default

	private:
		cubeb_devid m_devid;
		std::string m_device_id;
	};
	using CubebDeviceDescriptionPtr = std::shared_ptr<CubebDeviceDescription>;

	CubebAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	~CubebAPI();

	AudioAPI GetType() const override { return Cubeb; }
	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	void SetVolume(sint32 volume) override;

	static std::vector<DeviceDescriptionPtr> GetDevices();

	static bool InitializeStatic();
	static void Destroy();

private:
	inline static cubeb* s_context = nullptr;

	cubeb_stream* m_stream = nullptr;
	bool m_is_playing = false;

	mutable std::shared_mutex m_mutex;
	std::vector<uint8> m_buffer; // queued PCM bytes, oldest first
	size_t m_bufferLimit = 0;    // bytes; a block that would reach it is dropped

	static long FillOutput(cubeb_stream* stream, void* user, const void* input, void* output, long frames);
};
