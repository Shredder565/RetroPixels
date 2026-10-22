// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#undef GetHwnd
#include <dxgi1_4.h>
#include <wrl/client.h>

// Windows only: finds a DXGI adapter (the one with the given LUID, or the
// first one) to query video memory usage. dxgi.dll is loaded at runtime.
// The constructor throws std::runtime_error if DXGI is unusable.
class DXGIWrapper
{
public:
	DXGIWrapper() : DXGIWrapper(nullptr) {}

	explicit DXGIWrapper(uint8* deviceLUID)
	{
		m_dxgiModule = LoadLibraryA("dxgi.dll");
		if (!m_dxgiModule)
			throw std::runtime_error("can't load dxgi module");

		using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
		auto createFactory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(m_dxgiModule, "CreateDXGIFactory1"));
		Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
		if (!createFactory || FAILED(createFactory(IID_PPV_ARGS(&factory))))
		{
			Release();
			throw std::runtime_error("can't create a DXGI factory");
		}

		Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
		for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; i++)
		{
			DXGI_ADAPTER_DESC1 desc{};
			if (FAILED(candidate->GetDesc1(&desc)))
				continue;
			if (deviceLUID && memcmp(&desc.AdapterLuid, deviceLUID, sizeof(LUID)) != 0)
				continue;
			if (FAILED(candidate.As(&m_adapter)))
			{
				Release();
				throw std::runtime_error("can't create dxgi adapter");
			}
			break;
		}
	}

	~DXGIWrapper()
	{
		Release();
	}

	DXGIWrapper(const DXGIWrapper&) = delete;
	DXGIWrapper& operator=(const DXGIWrapper&) = delete;

	// local (dedicated) video memory budget and usage; false if unavailable
	bool QueryVideoMemoryInfo(DXGI_QUERY_VIDEO_MEMORY_INFO& info) const
	{
		if (!m_adapter)
			return false;
		return m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info) == S_OK;
	}

private:
	void Release()
	{
		m_adapter.Reset(); // must go before the module is unloaded
		if (m_dxgiModule)
		{
			FreeLibrary(m_dxgiModule);
			m_dxgiModule = nullptr;
		}
	}

	HMODULE m_dxgiModule = nullptr;
	Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter;
};
