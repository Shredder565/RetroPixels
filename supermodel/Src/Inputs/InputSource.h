// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// CInputSource: something an input reads from (a key, a mouse or joystick
// axis, a button, a POV direction, or a combination of these). A source
// answers as a switch (on/off) or as an analog value scaled to the range the
// input asks for. Sources are reference counted with Acquire()/Release() and
// delete themselves when the last reference goes.

#ifndef INCLUDED_INPUTSOURCE_H
#define INCLUDED_INPUTSOURCE_H

#include <string>

struct ForceFeedbackCmd;

// What kind of values a source gives. Combinations of sources take the kind
// of their parts (see CMultiInputSource::GetCombinedType).
enum ESourceType
{
	SourceInvalid = -1,  // cannot be used (e.g. half and full axes mixed)
	SourceEmpty = 0,     // never active
	SourceSwitch = 1,    // key or button
	SourceHalfAxis = 2,  // one direction of an axis
	SourceFullAxis = 3   // a whole axis
};

class CInputSource
{
protected:
	// Number of references held.
	unsigned m_acquired;

	CInputSource(ESourceType sourceType);
	virtual ~CInputSource() {}

public:
	const ESourceType type;

	virtual void Acquire();

	// Drops a reference; the source deletes itself on the last one.
	virtual void Release();

	// val limited to [minVal, maxVal].
	static int Clamp(int val, int minVal, int maxVal);

	// Maps val linearly from [fromMinVal, fromMaxVal] to [toMinVal, toMaxVal]
	// (each range's minimum is its rest value).
	static int Scale(int val, int fromMinVal, int fromMaxVal, int toMinVal, int toMaxVal);

	// Maps val from the 'from' range to the 'to' range, each with a rest
	// ('off') value somewhere inside it: the part on each side of the rest
	// value is mapped separately, so the rest value maps exactly to toOffVal.
	// Either range may run backwards (max below min).
	static int Scale(int val, int fromMinVal, int fromOffVal, int fromMaxVal, int toMinVal, int toOffVal, int toMaxVal);

	// True if the source is on (as a switch).
	bool IsActive() const;

	// If the source is on, sets val (to true) and returns true.
	virtual bool GetValueAsSwitch(bool &val) const = 0;

	// If the source is away from rest, sets val to its value in the given
	// range and returns true.
	virtual bool GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal) = 0;

	// Passes a force feedback command on to the device; false if it cannot
	// take it.
	virtual bool SendForceFeedbackCmd(ForceFeedbackCmd ffCmd);

	// True for a mouse pointer's X or Y axis, whose value is where the
	// pointer is (so it is off centre even when nobody touches the mouse).
	virtual bool IsPointerAxis() const { return false; }
};

#endif // INCLUDED_INPUTSOURCE_H
