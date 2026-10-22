// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "MultiInputSource.h"

#include "Supermodel.h"
#include "Input.h"

ESourceType CMultiInputSource::GetCombinedType(std::vector<CInputSource*> &sources)
{
	if (sources.empty())
		return SourceEmpty;
	bool full = false;
	bool half = false;
	bool other = false;
	for (CInputSource *s : sources)
	{
		switch (s->type)
		{
		case SourceInvalid:  return SourceInvalid;
		case SourceSwitch:   break;
		case SourceFullAxis: full = true; break;
		case SourceHalfAxis: half = true; break;
		default:             other = true; break;
		}
		if (full && half)
			return SourceInvalid;
	}
	if (full)
		return SourceFullAxis;
	if (half)
		return SourceHalfAxis;
	return other ? SourceEmpty : SourceSwitch;
}

CMultiInputSource::CMultiInputSource()
	: CInputSource(SourceEmpty), m_isOr(true)
{
}

CMultiInputSource::CMultiInputSource(bool isOr, std::vector<CInputSource*> &sources)
	: CInputSource(GetCombinedType(sources)), m_isOr(isOr), m_parts(sources)
{
}

CMultiInputSource::~CMultiInputSource()
{
}

void CMultiInputSource::Acquire()
{
	CInputSource::Acquire();
	if (m_acquired == 1)
		for (CInputSource *s : m_parts)
			s->Acquire();
}

void CMultiInputSource::Release()
{
	if (m_acquired == 1)
		for (CInputSource *s : m_parts)
			s->Release();
	CInputSource::Release();
}

bool CMultiInputSource::GetValueAsSwitch(bool &val) const
{
	if (m_isOr)
	{
		for (CInputSource *s : m_parts)
			if (s->GetValueAsSwitch(val))
				return true;
		return false;
	}
	for (CInputSource *s : m_parts)
		if (!s->GetValueAsSwitch(val))
			return false;
	return !m_parts.empty();
}

bool CMultiInputSource::GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal)
{
	if (m_isOr)
	{
		for (CInputSource *s : m_parts)
			if (s->GetValueAsAnalog(val, minVal, offVal, maxVal))
				return true;
		return false;
	}
	// Every switch must be held (each is read, in order, until one is not)...
	for (CInputSource *s : m_parts)
		if (s->type == SourceSwitch && !s->GetValueAsAnalog(val, minVal, offVal, maxVal))
			return false;
	// ...then the first moved axis gives the value.
	for (CInputSource *s : m_parts)
		if (s->type != SourceSwitch && s->GetValueAsAnalog(val, minVal, offVal, maxVal))
			return true;
	return !m_parts.empty() && type == SourceSwitch;
}

bool CMultiInputSource::SendForceFeedbackCmd(ForceFeedbackCmd ffCmd)
{
	bool taken = false;
	for (CInputSource *s : m_parts)
		taken |= s->SendForceFeedbackCmd(ffCmd);
	return taken;
}

CNegInputSource::CNegInputSource(CInputSource *source)
	: CInputSource(source->type), m_source(source)
{
	// This reference lasts as long as the negation exists (it is not given
	// back on the last Release, so the negated source outlives it).
	m_source->Acquire();
}

void CNegInputSource::Acquire()
{
	CInputSource::Acquire();
	if (m_acquired == 1)
		m_source->Acquire();
}

void CNegInputSource::Release()
{
	if (m_acquired == 1)
		m_source->Release();
	CInputSource::Release();
}

bool CNegInputSource::GetValueAsSwitch(bool &val) const
{
	bool kept = val;
	if (m_source->GetValueAsSwitch(val))
	{
		val = kept;
		return false;
	}
	val = true;
	return true;
}

bool CNegInputSource::GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal)
{
	int kept = val;
	if (m_source->GetValueAsAnalog(val, minVal, offVal, maxVal))
	{
		val = kept;
		return false;
	}
	val = maxVal;
	return true;
}
