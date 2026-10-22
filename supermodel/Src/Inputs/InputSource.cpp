// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "InputSource.h"

#include "Supermodel.h"
#include "Input.h"
#include "InputSystem.h"

CInputSource::CInputSource(ESourceType sourceType)
	: m_acquired(0), type(sourceType)
{
}

void CInputSource::Acquire()
{
	m_acquired++;
}

void CInputSource::Release()
{
	m_acquired--;
	if (m_acquired == 0)
		delete this;
}

int CInputSource::Clamp(int val, int minVal, int maxVal)
{
	return val > maxVal ? maxVal : (val < minVal ? minVal : val);
}

int CInputSource::Scale(int val, int fromMinVal, int fromMaxVal, int toMinVal, int toMaxVal)
{
	return Scale(val, fromMinVal, fromMinVal, fromMaxVal, toMinVal, toMinVal, toMaxVal);
}

int CInputSource::Scale(int val, int fromMinVal, int fromOffVal, int fromMaxVal, int toMinVal, int toOffVal, int toMaxVal)
{
	// Where val lies between the rest value (0) and the far end of its side
	// (+1 on the side of fromMaxVal, -1 on the side of fromMinVal).
	if (fromMaxVal == fromMinVal)
		return toOffVal;
	bool fromForward = fromMaxVal > fromMinVal;
	val = fromForward ? Clamp(val, fromMinVal, fromMaxVal) : Clamp(val, fromMaxVal, fromMinVal);
	if (val == fromOffVal)
		return toOffVal;
	// The distance from rest, counted positive towards fromMaxVal.
	int distance = fromForward ? val - fromOffVal : fromOffVal - val;
	bool towardsMax = fromForward ? val > fromOffVal : val < fromOffVal;
	int span = towardsMax ? (fromForward ? fromMaxVal - fromOffVal : fromOffVal - fromMaxVal)
	                      : (fromForward ? fromOffVal - fromMinVal : fromMinVal - fromOffVal);
	double frac = (double)distance / (double)span;

	// The same position on the 'to' side.
	bool toForward = toMaxVal > toMinVal;
	int toSpan;
	if (frac >= 0)
		toSpan = toForward ? toMaxVal - toOffVal : toOffVal - toMaxVal;
	else
		toSpan = toForward ? toOffVal - toMinVal : toMinVal - toOffVal;
	int offset = (int)((double)toSpan * frac);
	return toForward ? toOffVal + offset : toOffVal - offset;
}

bool CInputSource::IsActive() const
{
	bool unused = false;
	return GetValueAsSwitch(unused);
}

bool CInputSource::SendForceFeedbackCmd(ForceFeedbackCmd)
{
	return false;
}
