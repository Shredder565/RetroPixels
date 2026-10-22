// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "InputTypes.h"

#include "Supermodel.h"
#include <string>

//
// CSwitchInput
//

CSwitchInput::CSwitchInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, const char *defaultMapping, UINT16 offVal, UINT16 onVal)
	: CInput(inputId, inputLabel, INPUT_FLAGS_SWITCH, inputGameFlags, defaultMapping), m_offVal(offVal), m_onVal(onVal)
{
}

void CSwitchInput::Poll()
{
	prevValue = value;
	bool on = value != 0;
	bool active = m_source && m_source->GetValueAsSwitch(on);
	value = (active && on) ? m_onVal : m_offVal;
}

bool CSwitchInput::Pressed() const
{
	return prevValue == m_offVal && value == m_onVal;
}

bool CSwitchInput::Released() const
{
	return prevValue == m_onVal && value == m_offVal;
}

//
// CAnalogInput
//

CAnalogInput::CAnalogInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, const char *defaultMapping, UINT16 minVal, UINT16 maxVal)
	: CInput(inputId, inputLabel, INPUT_FLAGS_ANALOG, inputGameFlags, defaultMapping, minVal), m_minVal(minVal), m_maxVal(maxVal)
{
}

void CAnalogInput::Poll()
{
	prevValue = value;
	int v = value;
	if (m_source && m_source->GetValueAsAnalog(v, m_minVal, m_minVal, m_maxVal))
		value = UINT16(v);
	else
		value = m_minVal;
}

bool CAnalogInput::HasValue() const
{
	return value > m_minVal;
}

double CAnalogInput::ValueAsFraction() const
{
	double f = (double)(value - m_minVal) / (double)(m_maxVal - m_minVal);
	return f < 0.0 ? -f : f;
}

//
// CAxisInput
//

CAxisInput::CAxisInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, const char *defaultMapping,
	std::shared_ptr<CAnalogInput> negInput, std::shared_ptr<CAnalogInput> posInput, UINT16 minVal, UINT16 offVal, UINT16 maxVal)
	: CInput(inputId, inputLabel, INPUT_FLAGS_AXIS, inputGameFlags, defaultMapping, offVal),
	  m_negInput(negInput), m_posInput(posInput), m_minVal(minVal), m_offVal(offVal), m_maxVal(maxVal)
{
}

void CAxisInput::Poll()
{
	prevValue = value;

	bool negMoved = m_negInput && m_negInput->HasValue();
	bool posMoved = m_posInput && m_posInput->HasValue();
	if (negMoved || posMoved)
	{
		// The two direction inputs push the axis away from rest by their
		// fraction of each side's travel (both may be held; they offset).
		bool forward = m_maxVal > m_minVal;
		int posTravel = forward ? m_maxVal - m_offVal : m_offVal - m_maxVal;
		int negTravel = forward ? m_offVal - m_minVal : m_minVal - m_offVal;
		int sign = forward ? 1 : -1;
		value = m_offVal;
		if (m_posInput)
			value = UINT16(value + sign * (int)(m_posInput->ValueAsFraction() * (double)posTravel));
		if (m_negInput)
			value = UINT16(value - sign * (int)(m_negInput->ValueAsFraction() * (double)negTravel));
		return;
	}
	int v = value;
	if (m_source && m_source->GetValueAsAnalog(v, m_minVal, m_offVal, m_maxVal))
		value = UINT16(v);
	else
		value = m_offVal;
}

bool CAxisInput::HasValue() const
{
	return value != m_offVal;
}

double CAxisInput::ValueAsFraction() const
{
	double f = (double)(value - m_minVal) / (double)(m_maxVal - m_minVal);
	return f < 0.0 ? -f : f;
}

//
// CGearShift4Input
//

CGearShift4Input::CGearShift4Input(const char *inputId, const char *inputLabel, unsigned inputGameFlags,
	std::shared_ptr<CSwitchInput> shift1Input, std::shared_ptr<CSwitchInput> shift2Input, std::shared_ptr<CSwitchInput> shift3Input, std::shared_ptr<CSwitchInput> shift4Input, std::shared_ptr<CSwitchInput> shiftNInput,
	std::shared_ptr<CSwitchInput> shiftUpInput, std::shared_ptr<CSwitchInput> shiftDownInput)
	: CInput(inputId, inputLabel, INPUT_FLAGS_VIRTUAL, inputGameFlags),
	  m_shift1Input(shift1Input), m_shift2Input(shift2Input), m_shift3Input(shift3Input), m_shift4Input(shift4Input), m_shiftNInput(shiftNInput),
	  m_shiftUpInput(shiftUpInput), m_shiftDownInput(shiftDownInput)
{
	value = prevValue = 1;  // start in first gear
}

void CGearShift4Input::Poll()
{
	prevValue = value;

	const std::shared_ptr<CSwitchInput> byPriority[] = { m_shiftNInput, m_shift1Input, m_shift2Input, m_shift3Input, m_shift4Input };
	for (UINT16 gear = 0; gear < 5; gear++)
	{
		if (byPriority[gear]->Pressed())
		{
			value = gear;
			break;
		}
	}

	if (m_shiftUpInput->Pressed())
		value = UINT16(CInputSource::Clamp(value + 1, 0, 4));
	else if (m_shiftDownInput->Pressed())
		value = UINT16(CInputSource::Clamp(value - 1, 0, 4));
}

//
// CTriggerInput
//

CTriggerInput::CTriggerInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags,
	std::shared_ptr<CSwitchInput> triggerInput, std::shared_ptr<CSwitchInput> offscreenInput, UINT16 offVal, UINT16 onVal)
	: CInput(inputId, inputLabel, INPUT_FLAGS_VIRTUAL, inputGameFlags),
	  m_triggerInput(triggerInput), m_offscreenInput(offscreenInput), m_offVal(offVal), m_onVal(onVal),
	  m_autoTrigger(false), m_reloadFrames(0), offscreenValue(0)
{
}

void CTriggerInput::LoadFromConfig(const Util::Config::Node &config)
{
	if (const Util::Config::Node *node = config.TryGet(std::string("Input") + id))
		m_autoTrigger = node->ValueAs<unsigned>() != 0;
}

void CTriggerInput::StoreToConfig(Util::Config::Node *config)
{
	config->Set(std::string("Input") + id, (unsigned)m_autoTrigger);
}

void CTriggerInput::Poll()
{
	prevValue = value;

	if (!m_autoTrigger)
	{
		value = m_triggerInput->value;
		offscreenValue = m_offscreenInput->value;
		return;
	}

	// Automatic reload: once off-screen is pressed, point off-screen for ten
	// frames and pull the trigger during the last four of them.
	if (m_reloadFrames > 0)
	{
		value = m_reloadFrames < 5 ? 1 : 0;
		offscreenValue = m_onVal;
		m_reloadFrames--;
		return;
	}
	value = m_triggerInput->value;
	if (m_offscreenInput->Pressed())
	{
		offscreenValue = m_onVal;
		m_reloadFrames = 10;
	}
	else
		offscreenValue = m_offVal;
}
