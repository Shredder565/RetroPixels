// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/clntdata.h>

// Client data holding a value of any type (for SetClientObject / list item data).
template <typename T>
class wxCustomData : public wxClientData
{
public:
	wxCustomData() : m_value({}) {}
	wxCustomData(T data) : m_value(std::move(data)) {}

	const T& GetData() const { return m_value; }

	// moves the value out of the container
	T get() { return std::move(m_value); }

	// direct access, the value stays owned by the container
	T& ref() { return m_value; }

	bool operator==(const T& o) const { return m_value == o.m_value; }
	bool operator!=(const T& o) const { return !(*this == o); }

private:
	T m_value;
};
