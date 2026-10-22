// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/control.h>

// wxObject wrapper so a control can travel as event user data (Bind(..., userData)).
class wxControlObject : public wxObject
{
public:
	wxControlObject(wxControl* control) : m_ctrl(control) {}

	template <typename T>
	T* GetControl() const
	{
		static_assert(std::is_base_of_v<wxControl, T>, "T must inherit from wxControl");
		return dynamic_cast<T*>(m_ctrl);
	}

	wxControl* GetControl() const { return m_ctrl; }

private:
	wxControl* m_ctrl;
};

// Same for a list of controls, addressed by index.
class wxControlObjects : public wxObject
{
public:
	wxControlObjects(std::vector<wxControl*> controls) : m_list(std::move(controls)) {}

	template <typename T = wxControl>
	T* GetControl(int index) const
	{
		static_assert(std::is_base_of_v<wxControl, T>, "T must inherit from wxControl");
		if (index < 0 || (size_t)index >= m_list.size())
			return nullptr;
		return dynamic_cast<T*>(m_list[index]);
	}

	const std::vector<wxControl*>& GetControls() const { return m_list; }

private:
	std::vector<wxControl*> m_list;
};
