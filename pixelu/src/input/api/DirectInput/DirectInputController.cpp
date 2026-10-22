// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/DirectInput/DirectInputController.h"
#include "WindowSystem.h"

namespace
{
	// product GUID of the GameCube adapter's DirectInput driver; releasing its device crashes
	constexpr GUID kGameCubeAdapterProduct = { 0x18440079, 0, 0, { 0, 0, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 } };

	// values closer to the centre than this count as centred
	constexpr float kAxisNoise = 0.001f;

	float NormalizeAxis(LONG value, LONG min, LONG max)
	{
		return (float(value - min) / float(max - min)) * 2.0f - 1.0f;
	}

	void SetIfMoved(float& target, float value)
	{
		if (std::abs(value) >= kAxisNoise)
			target = value;
	}

	// POV hat angles in hundredths of a degree, clockwise from up
	struct HatDirection
	{
		DWORD angle;
		uint32 first;
		uint32 second; // kNoButton when there is only one
	};
	constexpr uint32 kNoButton = std::numeric_limits<uint32>::max();
	constexpr HatDirection kHatDirections[] = {
		{ 0, kButtonUp, kNoButton },
		{ 4500, kButtonUp, kButtonRight },
		{ 9000, kButtonRight, kNoButton },
		{ 13500, kButtonRight, kButtonDown },
		{ 18000, kButtonDown, kNoButton },
		{ 22500, kButtonDown, kButtonLeft },
		{ 27000, kButtonLeft, kNoButton },
		{ 31500, kButtonLeft, kButtonUp },
	};

	bool ReadDeviceInfo(IDirectInputDevice8W* device, DIDEVICEINSTANCEW& info)
	{
		info = {};
		info.dwSize = sizeof(DIDEVICEINSTANCEW);
		return SUCCEEDED(device->GetDeviceInfo(&info));
	}
}

DirectInputController::DirectInputController(const GUID& guid)
	: DirectInputController(guid, fmt::format("[{}]", StringFromGUID(guid)))
{
}

DirectInputController::DirectInputController(const GUID& guid, std::string_view display_name)
	: Controller<DirectInputControllerProvider>(StringFromGUID(guid), display_name), m_guid(guid)
{
}

DirectInputController::~DirectInputController()
{
	if (!m_device)
		return;
	m_device->Unacquire();

	if (m_product_guid == GUID{})
	{
		DIDEVICEINSTANCEW info;
		if (ReadDeviceInfo(m_device.Get(), info))
			m_product_guid = info.guidProduct;
	}
	// leak the GameCube adapter's device on purpose (see above)
	if (m_product_guid == kGameCubeAdapterProduct)
		m_device.Detach();
}

void DirectInputController::save(pugi::xml_node& node)
{
	Controller<DirectInputControllerProvider>::save(node);
	node.append_child("product_guid").append_child(pugi::node_pcdata).set_value(StringFromGUID(m_product_guid).c_str());
}

void DirectInputController::load(const pugi::xml_node& node)
{
	Controller<DirectInputControllerProvider>::load(node);

	const auto value = node.child("product_guid");
	if (!value || !GUIDFromString(value.child_value(), m_product_guid) || m_product_guid == GUID{} || is_connected())
		return;

	// the saved device isn't there: take a connected device of the same model instead
	for (const auto& candidate : m_provider->get_controllers())
	{
		const auto other = std::dynamic_pointer_cast<DirectInputController>(candidate);
		if (!other || !other->is_connected() || other->get_product_guid() != m_product_guid)
			continue;
		const GUID saved = m_guid;
		m_guid = other->get_guid();
		if (connect())
			break;
		m_guid = saved;
	}
}

bool DirectInputController::connect()
{
	if (is_connected())
		return true;

	m_effect = nullptr;

	std::scoped_lock lock(m_mutex);
	if (FAILED(m_provider->get_dinput()->CreateDevice(m_guid, &m_device, nullptr)) || !m_device)
		return false;

	DIDEVICEINSTANCEW info;
	if (ReadDeviceInfo(m_device.Get(), info))
		m_display_name = boost::nowide::narrow(info.tszProductName);

	if (FAILED(m_device->SetDataFormat(m_provider->get_data_format())))
		return false;

	// exclusive access is needed for force feedback; without it the pad still works
	const HWND window = static_cast<HWND>(WindowSystem::GetWindowInfo().window_main.surface);
	if (SUCCEEDED(m_device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_EXCLUSIVE)))
		create_rumble_effect();
	else if (FAILED(m_device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
		return false;

	if (ReadDeviceInfo(m_device.Get(), info))
		m_product_guid = info.guidProduct;

	read_axis_ranges();
	m_device->Acquire();
	return true;
}

void DirectInputController::create_rumble_effect()
{
	GUID effect_guid = GUID_NULL;
	const auto take_first = [](LPCDIEFFECTINFOW effect, LPVOID context) -> BOOL
	{
		*static_cast<GUID*>(context) = effect->guid;
		return DIENUM_STOP;
	};
	if (FAILED(m_device->EnumEffects(take_first, &effect_guid, DIEFT_CONSTANTFORCE)) || effect_guid == GUID_NULL)
		return;

	DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
	LONG direction[2] = { 1, 0 };
	DICONSTANTFORCE force{ DI_FFNOMINALMAX };

	DIEFFECT effect{};
	effect.dwSize = sizeof(DIEFFECT);
	effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	effect.dwDuration = INFINITE;
	effect.dwGain = DI_FFNOMINALMAX;
	effect.dwTriggerButton = DIEB_NOTRIGGER;
	effect.cAxes = 2;
	effect.rgdwAxes = axes;
	effect.rglDirection = direction;
	effect.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
	effect.lpvTypeSpecificParams = &force;
	m_device->CreateEffect(effect_guid, &effect, &m_effect, nullptr);
}

void DirectInputController::read_axis_ranges()
{
	m_min_axis.fill(0);
	m_max_axis.fill(std::numeric_limits<uint16>::max());
	const auto read_range = [](LPCDIDEVICEOBJECTINSTANCEW object, LPVOID context) -> BOOL
	{
		auto& self = *static_cast<DirectInputController*>(context);
		const auto axis = DIDFT_GETINSTANCE(object->dwType);
		// devices with extra sliders report more than the six axes we use
		if (axis >= self.m_min_axis.size())
			return DIENUM_CONTINUE;

		DIPROPRANGE range{};
		range.diph.dwSize = sizeof(range);
		range.diph.dwHeaderSize = sizeof(range.diph);
		range.diph.dwHow = DIPH_BYID;
		range.diph.dwObj = object->dwType;
		if (self.m_device->GetProperty(DIPROP_RANGE, &range.diph) == DI_OK)
		{
			self.m_min_axis[axis] = range.lMin;
			self.m_max_axis[axis] = range.lMax;
		}
		return DIENUM_CONTINUE;
	};
	m_device->EnumObjects(read_range, this, DIDFT_AXIS);
}

bool DirectInputController::is_connected()
{
	std::shared_lock lock(m_mutex);
	return m_device != nullptr;
}

bool DirectInputController::has_rumble()
{
	return m_effect != nullptr;
}

// the effect is created, but playing it was never wired up
void DirectInputController::start_rumble()
{
}

void DirectInputController::stop_rumble()
{
}

std::string DirectInputController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kAxisXP: return "X+";
	case kAxisXN: return "X-";
	case kAxisYP: return "Y+";
	case kAxisYN: return "Y-";
	case kRotationXP: return "RX+";
	case kRotationXN: return "RX-";
	case kRotationYP: return "RY+";
	case kRotationYN: return "RY-";
	case kTriggerXP: return "Z+";
	case kTriggerXN: return "Z-";
	case kTriggerYP: return "RZ+";
	case kTriggerYN: return "RZ-";
	default: return ControllerBase::get_button_name(button);
	}
}

ControllerState DirectInputController::raw_state()
{
	ControllerState result{};
	if (!is_connected())
		return result;

	// after losing focus or access the device has to be acquired again
	const auto failed = [&](HRESULT hr)
	{
		if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
		{
			result.last_state = hr;
			m_device->Acquire();
		}
	};

	HRESULT hr = m_device->Poll();
	if (FAILED(hr))
	{
		failed(hr);
		return result;
	}
	DIJOYSTATE state{};
	hr = m_device->GetDeviceState(sizeof(state), &state);
	if (FAILED(hr))
	{
		failed(hr);
		return result;
	}
	result.last_state = hr;

	for (size_t i = 0; i < std::size(state.rgbButtons); ++i)
	{
		if (state.rgbButtons[i] & 0x80)
			result.buttons.SetButtonState((uint32)i, true);
	}

	// DirectInput's y axes point down
	SetIfMoved(result.axis.x, NormalizeAxis(state.lX, m_min_axis[0], m_max_axis[0]));
	SetIfMoved(result.axis.y, -NormalizeAxis(state.lY, m_min_axis[1], m_max_axis[1]));
	SetIfMoved(result.rotation.x, NormalizeAxis(state.lRx, m_min_axis[3], m_max_axis[3]));
	SetIfMoved(result.rotation.y, -NormalizeAxis(state.lRy, m_min_axis[4], m_max_axis[4]));
	SetIfMoved(result.trigger.x, NormalizeAxis(state.lZ, m_min_axis[2], m_max_axis[2]));
	SetIfMoved(result.trigger.y, -NormalizeAxis(state.lRz, m_min_axis[5], m_max_axis[5]));

	const DWORD hat = state.rgdwPOV[0];
	for (const HatDirection& d : kHatDirections)
	{
		if (d.angle != hat)
			continue;
		result.buttons.SetButtonState(d.first, true);
		if (d.second != kNoButton)
			result.buttons.SetButtonState(d.second, true);
		break;
	}
	return result;
}
