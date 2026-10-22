// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// CInput: one control of the emulated cabinet (or of the emulator's own user
// interface). It has a mapping string, such as "KEY_A,JOY1_BUTTON1", which
// the input system turns into a source, and a value that Poll() updates.
// The mapping is stored in Supermodel.ini as "Input<id> = <mapping>".

#ifndef INCLUDED_INPUT_H
#define INCLUDED_INPUT_H

#include "InputSource.h"
#include "Types.h"
#include "Game.h"
#include "Util/NewConfig.h"
#include <memory>

class CInputSystem;

// Kinds of input
#define INPUT_FLAGS_SWITCH  0x0001
#define INPUT_FLAGS_ANALOG  0x0002
#define INPUT_FLAGS_AXIS    0x0004
#define INPUT_FLAGS_VIRTUAL 0x0008  // computed from other inputs, no mapping

#define MAX_MAPPING_LENGTH 255

#define NUM_FF_EFFECTS 3

enum EForceFeedback
{
	FFStop = -1,
	FFConstantForce = 0,
	FFSelfCenter = 1,
	FFFriction = 2,
	FFVibrate = 3
};

struct ForceFeedbackCmd
{
	EForceFeedback id;
	float force;  // 0 = off, +/-1 = full (the sign is the direction for a constant force)
};

class CInput
{
private:
	char m_mapping[MAX_MAPPING_LENGTH + 1];
	const char *m_defaultMapping;
	std::shared_ptr<CInputSystem> m_system;

	// (Re)builds m_source from the mapping. A mapping that cannot be used is
	// replaced by the default one.
	void CreateSource();

protected:
	CInputSource *m_source;  // null when unmapped

	CInput(const char *inputId, const char *inputLabel, unsigned inputFlags, unsigned inputGameFlags,
		const char *defaultMapping = "NONE", UINT16 initValue = 0);

public:
	virtual ~CInput();

	const char *id;           // ini name (after "Input")
	const char *label;        // shown to the user
	const unsigned flags;     // INPUT_FLAGS_*
	const unsigned gameFlags; // Game::INPUT_* group

	UINT16 value;
	UINT16 prevValue;

	// Must be called before the input is used.
	void Initialize(std::shared_ptr<CInputSystem> system);

	std::shared_ptr<CInputSystem> GetInputSystem();

	// The heading the input is listed under.
	const char* GetInputGroup();

	const char* GetMapping();

	void ClearMapping();

	// Several mappings are separated by ',' (any of them); a '+' joins
	// controls that must be used together.
	void SetMapping(const char *mapping);

	// Adds a mapping after a ','.
	void AppendMapping(const char *mapping);

	void ResetToDefaultMapping();

	virtual void LoadFromConfig(const Util::Config::Node &config);

	virtual void StoreToConfig(Util::Config::Node *config);

	// The input system's settings changed: rebuild the source.
	void InputSystemChanged();

	bool IsUIInput() const;

	// Everything but UI and virtual inputs can be mapped by the user.
	bool IsConfigurable() const;

	bool IsVirtual() const;

	// Waits for the user to operate a control and sets (or, with append,
	// adds) its mapping. False if the escape mapping was used instead.
	bool Configure(bool append, const char *escapeMapping = "KEY_ESCAPE");

	virtual void Poll() = 0;

	bool Changed() const;

	bool SendForceFeedbackCmd(ForceFeedbackCmd ffCmd);
};

inline bool CInput::IsUIInput() const
{
	return gameFlags == Game::INPUT_UI;
}

inline bool CInput::IsConfigurable() const
{
	return !IsUIInput() && !IsVirtual();
}

inline bool CInput::IsVirtual() const
{
	return (flags & INPUT_FLAGS_VIRTUAL) != 0;
}

#endif	// INCLUDED_INPUT_H
