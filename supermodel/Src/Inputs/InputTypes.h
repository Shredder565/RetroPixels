// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The kinds of input: switches (buttons), analog controls (pedals), axes
// (steering, sticks, gun positions), a 4-gear shifter built from buttons and
// a light-gun trigger with optional automatic reload.

#ifndef INCLUDED_INPUTTYPES_H
#define INCLUDED_INPUTTYPES_H

#include "Input.h"
#include "Types.h"
#include <memory>

// On or off: value is onVal or offVal.
class CSwitchInput : public CInput
{
private:
	UINT16 m_offVal;
	UINT16 m_onVal;

public:
	CSwitchInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, const char *defaultMapping,
		UINT16 offVal = 0x00, UINT16 onVal = 0x01);

	void Poll();

	// Went from off to on in the last poll.
	bool Pressed() const;

	// Went from on to off in the last poll.
	bool Released() const;
};

// From minVal at rest up to maxVal.
class CAnalogInput : public CInput
{
private:
	UINT16 m_minVal;
	UINT16 m_maxVal;

public:
	CAnalogInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, const char *defaultMapping,
		UINT16 minVal = 0x00, UINT16 maxVal = 0xFF);

	void Poll();

	// Away from rest.
	bool HasValue() const;

	// How far from rest, 0 to 1.
	double ValueAsFraction() const;
};

// From minVal to maxVal with rest at offVal (maxVal may be below minVal for
// an axis that runs the other way). Besides its own mapping it can be driven
// by two analog inputs, one for each direction, which take precedence.
class CAxisInput : public CInput
{
private:
	std::shared_ptr<CAnalogInput> m_negInput;
	std::shared_ptr<CAnalogInput> m_posInput;
	UINT16 m_minVal;
	UINT16 m_offVal;
	UINT16 m_maxVal;

public:
	CAxisInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, const char *defaultMapping, std::shared_ptr<CAnalogInput> negInput, std::shared_ptr<CAnalogInput> posInput,
		UINT16 minVal = 0x00, UINT16 offVal = 0x80, UINT16 maxVal = 0xFF);

	void Poll();

	// Away from rest.
	bool HasValue() const;

	// (value - min) / (max - min), as a magnitude.
	double ValueAsFraction() const;
};

// Gear 0 (neutral) to 4. A gear button selects its gear (the lowest pressed
// wins, neutral first) and the gear stays until changed; shift up/down step
// through the gears.
class CGearShift4Input : public CInput
{
private:
	std::shared_ptr<CSwitchInput> m_shift1Input;
	std::shared_ptr<CSwitchInput> m_shift2Input;
	std::shared_ptr<CSwitchInput> m_shift3Input;
	std::shared_ptr<CSwitchInput> m_shift4Input;
	std::shared_ptr<CSwitchInput> m_shiftNInput;
	std::shared_ptr<CSwitchInput> m_shiftUpInput;
	std::shared_ptr<CSwitchInput> m_shiftDownInput;

public:
	CGearShift4Input(const char *inputId, const char *inputLabel, unsigned inputGameFlags,
		std::shared_ptr<CSwitchInput> shift1Input, std::shared_ptr<CSwitchInput> shift2Input, std::shared_ptr<CSwitchInput> shift3Input, std::shared_ptr<CSwitchInput> shift4Input, std::shared_ptr<CSwitchInput> shiftNInput,
		std::shared_ptr<CSwitchInput> shiftUpInput, std::shared_ptr<CSwitchInput> shiftDownInput);

	void Poll();
};

// A light-gun trigger and its "point off-screen" button. With auto trigger
// on (Input<id> = 1 in the ini), pressing off-screen points the gun away
// and then pulls the trigger by itself, which is how the games reload.
class CTriggerInput : public CInput
{
private:
	std::shared_ptr<CSwitchInput> m_triggerInput;
	std::shared_ptr<CSwitchInput> m_offscreenInput;
	UINT16 m_offVal;
	UINT16 m_onVal;
	bool m_autoTrigger;
	int m_reloadFrames;  // frames left of an automatic reload

public:
	UINT16 offscreenValue;

	CTriggerInput(const char *inputId, const char *inputLabel, unsigned inputGameFlags, std::shared_ptr<CSwitchInput> triggerInput, std::shared_ptr<CSwitchInput> offscreenInput,
		UINT16 offVal = 0x00, UINT16 onVal = 0x01);

	void LoadFromConfig(const Util::Config::Node &config);

	void StoreToConfig(Util::Config::Node *config);

	void Poll();
};

#endif	// INCLUDED_INPUTTYPES_H
