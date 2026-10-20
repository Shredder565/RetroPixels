// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "interface/WindowSystem.h"
#include <wx/control.h>
#include <wx/listbase.h>
#include <wx/string.h>

// Lets fmt print a wxString directly (converted through std::string).
template <>
struct fmt::formatter<wxString> : formatter<string_view>
{
	template <typename Ctx>
	auto format(const wxString& text, Ctx& ctx) const
	{
		const std::string narrow = text.ToStdString();
		return formatter<string_view>::format(string_view(narrow), ctx);
	}
};

// Scoped enable/disable of a control: applies `state` now and the opposite on scope exit.
class wxTempEnable
{
public:
	wxTempEnable(wxControl* control, bool state = true)
		: m_target(control), m_applied(state)
	{
		m_target->Enable(m_applied);
	}
	wxTempEnable(const wxTempEnable&) = delete;
	wxTempEnable(wxTempEnable&&) noexcept = default;
	~wxTempEnable()
	{
		if (m_target != nullptr)
			m_target->Enable(!m_applied);
	}

private:
	wxControl* m_target;
	const bool m_applied;
};

class wxTempDisable : wxTempEnable
{
public:
	wxTempDisable(wxControl* control) : wxTempEnable(control, false) {}
};

// fmt-style formatting with a wxString pattern, result converted back from UTF-8.
template <typename... TArgs>
wxString formatWxString(const wxString& format, TArgs&&... args)
{
	const std::string pattern = format.utf8_string();
	return wxString::FromUTF8(fmt::format(fmt::runtime(pattern), std::forward<TArgs>(args)...));
}

// Calls func(args...) when the object goes out of scope.
template <typename TFunction, typename... TArgs>
class wxDTorFunc
{
public:
	wxDTorFunc(TFunction&& func, TArgs&&... args)
		: m_onExit([callee = std::decay_t<TFunction>(func), params = std::make_tuple(std::forward<TArgs>(args)...)]() mutable {
			  std::apply(callee, params);
		  })
	{
	}
	~wxDTorFunc() { m_onExit(); }

private:
	std::function<void()> m_onExit;
};

void wxAutosizeColumn(wxListCtrlBase* ctrl, int col);
void wxAutosizeColumns(wxListCtrlBase* ctrl, int col_start, int col_end);

// Nearest following/preceding sibling window that is of type T (a wxControl pointer type).
template <typename T>
T get_next_sibling(const T element)
{
	static_assert(std::is_pointer_v<T> && std::is_base_of_v<wxControl, std::remove_pointer_t<T>>, "element must be a pointer and inherit from wxControl");
	wxWindow* w = element->GetNextSibling();
	while (w != nullptr)
	{
		if (T match = dynamic_cast<T>(w))
			return match;
		w = w->GetNextSibling();
	}
	return nullptr;
}

template <typename T>
T get_prev_sibling(const T element)
{
	static_assert(std::is_pointer_v<T> && std::is_base_of_v<wxControl, std::remove_pointer_t<T>>, "element must be a pointer and inherit from wxControl");
	wxWindow* w = element->GetPrevSibling();
	while (w != nullptr)
	{
		if (T match = dynamic_cast<T>(w))
			return match;
		w = w->GetPrevSibling();
	}
	return nullptr;
}

// Slider event handler helper: writes the slider value into the wxStaticText bound as wxControlObject user data.
void update_slider_text(wxCommandEvent& event, const wxFormatString& format = "%d%%");

// Windows: tells left/right shift, ctrl and alt apart using the raw key flags.
uint32 fix_raw_keycode(uint32 keycode, uint32 raw_flags);

WindowSystem::WindowHandleInfo initHandleContextFromWxWidgetsWindow(wxWindow* wxw);
