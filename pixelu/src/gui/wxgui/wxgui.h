// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Common wxWidgets include for the PixelU GUI. wxString may not be built from or turned into
// narrow strings implicitly: conversions always say which encoding they use.
#define wxNO_UNSAFE_WXSTRING_CONV 1

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

// windows and dialogs
#include <wx/dialog.h>
#include <wx/aboutdlg.h>
#include <wx/display.h>
#include <wx/infobar.h>
#include <wx/panel.h>
#include <wx/splitter.h>
#include <wx/wupdlock.h>
#include <wx/timer.h>
#include <wx/dcbuffer.h>

// layout
#include <wx/sizer.h>
#include <wx/gbsizer.h>
#include <wx/wrapsizer.h>
#include <wx/statline.h>

// controls
#include <wx/combo.h>
#include <wx/combobox.h>
#include <wx/dataview.h>
#include <wx/gauge.h>
#include <wx/listbase.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/radiobox.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/scrolbar.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

// true while an input settings window has the keyboard (hotkeys and game input ignore it then)
extern bool g_inputConfigWindowHasFocus;

// Sets the slider's value the way a user would: sends it a wxEVT_SLIDER with the new value.
// Returns whether the event was handled.
inline bool SendSliderEvent(wxSlider* slider, int new_value)
{
	wxCommandEvent event(wxEVT_SLIDER, slider->GetId());
	event.SetEventObject(slider);
	event.SetInt(new_value);
	return slider->HandleWindowEvent(event);
}
