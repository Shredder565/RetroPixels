// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <wrl/client.h>
#include <xaudio2.h>

#ifndef XAUDIO2_DLL
#error wrong <xaudio2.h> included!
#endif

#include "XAudio2API.h"
#include "util/helpers/helpers.h"

#include <WbemCli.h>
#include <OleAuto.h>
#include <system_error>

#pragma comment(lib, "wbemuuid.lib")

static_assert(IAudioAPI::kBlockCount < XAUDIO2_MAX_QUEUED_BUFFERS, "too many xaudio2 buffers");

HMODULE XAudio2API::s_xaudio_dll = nullptr;
std::vector<XAudio2API::DeviceDescriptionPtr> XAudio2API::s_devices;

namespace
{
	// KSCATEGORY_AUDIO render endpoints (DEVINTERFACE_AUDIO_RENDER in mmdeviceapi.h)
	constexpr GUID kAudioRenderInterface = { 0xe6327cad, 0xdcec, 0x4949, { 0xae, 0x8a, 0x99, 0x1e, 0x97, 0x6a, 0x79, 0xd2 } };

	using CreateFn = decltype(&XAudio2Create);

	CreateFn FindCreate(HMODULE dll)
	{
		return dll ? reinterpret_cast<CreateFn>(GetProcAddress(dll, "XAudio2Create")) : nullptr;
	}

	void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr))
			throw std::system_error(hr, std::system_category());
	}

	// a BSTR that frees itself
	struct Bstr
	{
		explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
		~Bstr() { SysFreeString(value); }
		Bstr(const Bstr&) = delete;
		Bstr& operator=(const Bstr&) = delete;
		BSTR value;
	};

	// reads a string property of a WMI object; false when it has none
	bool ReadString(IWbemClassObject* object, const Bstr& property, std::wstring& out)
	{
		VARIANT value;
		VariantInit(&value);
		bool ok = false;
		if (SUCCEEDED(object->Get(property.value, 0, &value, nullptr, nullptr)))
		{
			ok = value.vt == VT_BSTR && value.bstrVal;
			if (ok)
				out = value.bstrVal;
			VariantClear(&value);
		}
		return ok;
	}

	// a PnP id such as SWD\MMDEVAPI\{0.0.0.00000000}.{guid} becomes the interface path
	// XAudio2 takes: \\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{render interface}
	std::wstring ToEndpointPath(std::wstring pnp_id)
	{
		std::replace(pnp_id.begin(), pnp_id.end(), L'\\', L'#');
		return L"\\\\?\\" + pnp_id + L"#{" + WStringFromGUID(kAudioRenderInterface) + L"}";
	}
}

XAudio2API::XAudio2API(std::wstring device_id, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample), m_device_id(std::move(device_id))
{
	const CreateFn create = FindCreate(s_xaudio_dll);
	if (!create)
		throw std::runtime_error("can't find XAudio2Create import");

	HRESULT hr = create(m_xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create xaudio device (hres: {:#x})", (uint32)hr));

	IXAudio2MasteringVoice* mastering = nullptr;
	hr = m_xaudio->CreateMasteringVoice(&mastering, channels, samplerate, 0, m_device_id.empty() ? nullptr : m_device_id.c_str());
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create xaudio mastering voice (hres: {:#x})", (uint32)hr));
	m_mastering_voice.reset(mastering);

	// m_wfx was filled in by IAudioAPI for exactly these parameters
	IXAudio2SourceVoice* source = nullptr;
	hr = m_xaudio->CreateSourceVoice(&source, &m_wfx.Format, 0, 1.0f);
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create xaudio source voice (hres: {:#x})", (uint32)hr));
	m_source_voice.reset(source);

	for (auto& buffer : m_audio_buffer)
		buffer = std::make_unique<uint8[]>(m_bytesPerBlock);

	m_xaudio->StartEngine();
}

void XAudio2API::VoiceDeleter::operator()(IXAudio2Voice* ptr) const
{
	if (ptr)
		ptr->DestroyVoice();
}

XAudio2API::~XAudio2API()
{
	if (m_xaudio)
		m_xaudio->StopEngine();
	XAudio2API::Stop();
}

void XAudio2API::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);
	m_mastering_voice->SetVolume((float)volume / 100.0f);
}

bool XAudio2API::Play()
{
	if (!m_playing)
		m_playing = SUCCEEDED(m_source_voice->Start());
	return m_playing;
}

bool XAudio2API::Stop()
{
	if (!m_playing)
		return true;
	m_playing = FAILED(m_source_voice->Stop());
	m_source_voice->FlushSourceBuffers();
	return m_playing;
}

bool XAudio2API::InitializeStatic()
{
	if (s_xaudio_dll)
		return true;

	// XAUDIO2_DLL names the system copy (xaudio2_9.dll / xaudio2_8.dll)
	s_xaudio_dll = LoadLibraryEx(XAUDIO2_DLL, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!FindCreate(s_xaudio_dll))
	{
		if (s_xaudio_dll)
			FreeLibrary(s_xaudio_dll);
		s_xaudio_dll = nullptr;
		return false;
	}

	RefreshDevices();
	return true;
}

void XAudio2API::Destroy()
{
	if (s_xaudio_dll)
		FreeLibrary(s_xaudio_dll);
}

const std::vector<XAudio2API::DeviceDescriptionPtr>& XAudio2API::RefreshDevices()
{
	s_devices.clear();

	try
	{
		Microsoft::WRL::ComPtr<IWbemLocator> locator;
		ThrowIfFailed(CoCreateInstance(__uuidof(WbemLocator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(locator.GetAddressOf())));

		const Bstr wmi_namespace(LR"(\\.\root\cimv2)");
		const Bstr language(L"WQL");
		const Bstr query(LR"(SELECT Name,DeviceID FROM Win32_PNPEntity WHERE PNPClass = "AudioEndpoint")");
		const Bstr name_property(L"Name");
		const Bstr id_property(L"DeviceID");

		Microsoft::WRL::ComPtr<IWbemServices> services;
		ThrowIfFailed(locator->ConnectServer(wmi_namespace.value, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.GetAddressOf()));
		ThrowIfFailed(CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
		                                RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

		Microsoft::WRL::ComPtr<IEnumWbemClassObject> rows;
		ThrowIfFailed(services->ExecQuery(language.value, query.value, WBEM_FLAG_RETURN_WBEM_COMPLETE | WBEM_FLAG_FORWARD_ONLY, nullptr, rows.GetAddressOf()));

		constexpr ULONG kBatch = 20;
		IWbemClassObject* batch[kBatch];
		ULONG count = 0;
		while (SUCCEEDED(rows->Next(100, kBatch, batch, &count)) && count > 0)
		{
			for (ULONG i = 0; i < count; ++i)
			{
				IWbemClassObject* row = batch[i];
				std::wstring name, device_id;
				bool keep = true;
				if (ReadString(row, name_property, name))
				{
					std::wstring pnp_id;
					if (ReadString(row, id_property, pnp_id))
					{
						// {0.0.0.00000000} marks render (output) endpoints; capture ones are {0.0.1...}
						if (pnp_id.find(L"{0.0.0.00000000}") == std::wstring::npos)
							keep = false;
						else
							device_id = ToEndpointPath(std::move(pnp_id));
					}
				}
				if (keep)
					s_devices.emplace_back(std::make_shared<XAudio2DeviceDescription>(name, device_id));
				row->Release();
			}
		}

		// the default device only makes sense when there is any device
		if (!s_devices.empty())
			s_devices.insert(s_devices.begin(), std::make_shared<XAudio2DeviceDescription>(L"Primary Sound Driver", L""));
	}
	catch (const std::system_error& ex)
	{
		cemuLog_log(LogType::Force, "XAudio2API::RefreshDevices: error while refreshing device list ({} - code: 0x{:08x})", ex.what(), (uint32)ex.code().value());
	}

	// balances the COM initialisation this has always relied on; kept as it was
	CoUninitialize();
	return s_devices;
}

bool XAudio2API::FeedBlock(sint16* data)
{
	if (m_blocks_queued >= kBlockCount)
	{
		// our count only goes up, ask the voice how many it still holds
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

uint32 XAudio2API::GetQueuedBuffers() const
{
	XAUDIO2_VOICE_STATE state{};
	m_source_voice->GetState(&state);
	return state.BuffersQueued;
}

bool XAudio2API::NeedAdditionalBlocks() const
{
	return GetQueuedBuffers() < GetAudioDelay();
}
