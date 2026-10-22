// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Sources made of other sources:
//   CMultiInputSource  "A,B" (either one: the first that is active counts)
//                      or "A+B" (all of the switches held together)
//   CNegInputSource    "!A" (active while A is not)

#ifndef INCLUDED_MULTIINPUTSOURCE_H
#define INCLUDED_MULTIINPUTSOURCE_H

#include "InputSource.h"
#include <vector>

class CMultiInputSource : public CInputSource
{
private:
	bool m_isOr;                         // true: "A,B"; false: "A+B"
	std::vector<CInputSource *> m_parts;

public:
	// The kind of source a combination of these parts makes: empty if there
	// are none, a switch if all are switches, else a full or a half axis
	// (mixing the two, or any invalid part, is invalid).
	static ESourceType GetCombinedType(std::vector<CInputSource*> &sources);

	// A source that is never active.
	CMultiInputSource();

	// isOr: the value is that of the first active part. Otherwise all switch
	// parts must be active, and the value is that of the first active
	// non-switch part (or the switches' if there are only switches).
	CMultiInputSource(bool isOr, std::vector<CInputSource*> &sources);

	~CMultiInputSource();

	// The parts are held for as long as this source is.
	void Acquire();
	void Release();

	bool GetValueAsSwitch(bool &val) const;

	bool GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal);

	// Sent to every part.
	bool SendForceFeedbackCmd(ForceFeedbackCmd ffCmd);
};

class CNegInputSource : public CInputSource
{
private:
	CInputSource *m_source;

public:
	CNegInputSource(CInputSource *source);

	void Acquire();
	void Release();

	bool GetValueAsSwitch(bool &val) const;

	// While the negated source is at rest this gives maxVal.
	bool GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal);
};

#endif	// INCLUDED_MULTIINPUTSOURCE_H
