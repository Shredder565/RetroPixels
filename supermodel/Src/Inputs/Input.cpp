// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Input.h"

#include "Supermodel.h"
#include "InputSystem.h"
#include "Util/Format.h"
#include <cstdio>
#include <cstring>

static bool IsUnmapped(const char *mapping)
{
	return mapping[0] == '\0' || Util::Stricmp(mapping, "NONE") == 0;
}

CInput::CInput(const char *inputId, const char *inputLabel, unsigned inputFlags, unsigned inputGameFlags, const char *defaultMapping, UINT16 initValue)
	: m_defaultMapping(defaultMapping), m_source(nullptr),
	  id(inputId), label(inputLabel), flags(inputFlags), gameFlags(inputGameFlags), value(initValue), prevValue(initValue)
{
	m_mapping[0] = '\0';
	ResetToDefaultMapping();
}

CInput::~CInput()
{
	if (m_source)
		m_source->Release();
}

void CInput::CreateSource()
{
	if (m_source)
		m_source->Release();
	m_source = nullptr;
	if (!m_system || IsUnmapped(m_mapping))
		return;

	m_source = m_system->ParseSource(m_mapping, (flags & INPUT_FLAGS_AXIS) != 0);
	if (m_source)
	{
		m_source->Acquire();
		return;
	}
	if (Util::Stricmp(m_mapping, m_defaultMapping) != 0)
	{
		ErrorLog("Unable to map input %s to [%s] - switching to default [%s].\n", id, m_mapping, m_defaultMapping);
		ResetToDefaultMapping();
	}
}

void CInput::Initialize(std::shared_ptr<CInputSystem> system)
{
	m_system = system;
	CreateSource();
}

std::shared_ptr<CInputSystem> CInput::GetInputSystem()
{
	return m_system;
}

const char* CInput::GetInputGroup()
{
	switch (gameFlags)
	{
	case Game::INPUT_UI:              return "User Interface Controls";
	case Game::INPUT_COMMON:          return "Common Controls";
	case Game::INPUT_JOYSTICK1:
	case Game::INPUT_JOYSTICK2:       return "4-Way Joysticks";
	case Game::INPUT_FIGHTING:        return "Fighting Game Buttons";
	case Game::INPUT_SPIKEOUT:        return "Spikeout Buttons";
	case Game::INPUT_SOCCER:          return "Virtua Striker Buttons";
	case Game::INPUT_VEHICLE:         return "Racing Game Steering Controls";
	case Game::INPUT_SHIFT4:          return "Racing Game Gear 4-Way Shift";
	case Game::INPUT_SHIFTUPDOWN:     return "Racing Game Gear Up/Down Shift";
	case Game::INPUT_VR4:             return "Racing Game 4 VR View Buttons";
	case Game::INPUT_VIEWCHANGE:      return "Racing Game View Change";
	case Game::INPUT_HANDBRAKE:       return "Racing Game Handbrake";
	case Game::INPUT_HARLEY:          return "Harley Davidson Controls";
	case Game::INPUT_TWIN_JOYSTICKS:  return "Virtual On Controls";
	case Game::INPUT_ANALOG_JOYSTICK: return "Analog Joystick";
	case Game::INPUT_GUN1:
	case Game::INPUT_GUN2:            return "Light Guns";
	case Game::INPUT_ANALOG_GUN1:
	case Game::INPUT_ANALOG_GUN2:     return "Analog Guns";
	case Game::INPUT_SKI:             return "Ski Controls";
	case Game::INPUT_MAGTRUCK:        return "Magical Truck Controls";
	case Game::INPUT_FISHING:         return "Fishing Controls";
	default:                          return "Misc";
	}
}

const char *CInput::GetMapping()
{
	return m_mapping;
}

void CInput::ClearMapping()
{
	SetMapping("NONE");
}

void CInput::SetMapping(const char *mapping)
{
	// Longer mappings are cut to MAX_MAPPING_LENGTH characters.
	size_t n = strnlen(mapping, MAX_MAPPING_LENGTH);
	memmove(m_mapping, mapping, n);
	m_mapping[n] = '\0';
	CreateSource();
}

void CInput::AppendMapping(const char *mapping)
{
	if (IsUnmapped(m_mapping))
	{
		SetMapping(mapping);
		return;
	}
	size_t used = strlen(m_mapping);
	snprintf(m_mapping + used, sizeof(m_mapping) - used, ",%s", mapping);
	CreateSource();
}

void CInput::ResetToDefaultMapping()
{
	SetMapping(m_defaultMapping);
}

void CInput::LoadFromConfig(const Util::Config::Node &config)
{
	if (IsConfigurable())
	{
		if (const Util::Config::Node *node = config.TryGet(std::string("Input") + id))
		{
			SetMapping(node->ValueAs<std::string>().c_str());
			return;
		}
	}
	// The input system's settings may have changed even if the mapping has not.
	CreateSource();
}

void CInput::StoreToConfig(Util::Config::Node *config)
{
	if (IsConfigurable())
		config->Set(std::string("Input") + id, std::string(m_mapping));
}

void CInput::InputSystemChanged()
{
	CreateSource();
}

bool CInput::Configure(bool append, const char *escapeMapping)
{
	char mapping[MAX_MAPPING_LENGTH];
	if (!m_system->ReadMapping(mapping, sizeof(mapping), (flags & INPUT_FLAGS_AXIS) != 0, READ_ALL, escapeMapping))
		return false;
	if (append)
		AppendMapping(mapping);
	else
		SetMapping(mapping);
	return true;
}

bool CInput::Changed() const
{
	return value != prevValue;
}

bool CInput::SendForceFeedbackCmd(ForceFeedbackCmd ffCmd)
{
	return m_source ? m_source->SendForceFeedbackCmd(ffCmd) : false;
}
