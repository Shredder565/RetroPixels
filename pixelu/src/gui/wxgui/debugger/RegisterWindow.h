// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include <wx/frame.h>

#include <array>

class DebuggerWindow2;
class wxTextCtrl;

// Floating debugger window showing the GPRs, LR, paired-single FPRs and CR
// fields of the current debug session. Values that changed since the last
// update are drawn red; double-clicking a GPR or FPR edits it.
class RegisterWindow : public wxFrame
{
public:
	RegisterWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnUpdateView();

private:
	void OnValueDoubleClick(wxMouseEvent& event);
	void OnToggleFloatFormat(wxCommandEvent& event);

	void ShowGpr(size_t row, uint32 value, bool changed);
	void ShowFpr(wxTextCtrl* field, double asDouble, uint64 asBits);
	wxString DescribeAddress(uint32 value) const;
	bool SetColourForChange(wxTextCtrl* field, bool changed) const;

	// rows 0..31 are r0..r31, row 32 is LR
	std::array<wxTextCtrl*, 33> m_gprValue{};
	std::array<wxTextCtrl*, 33> m_gprHint{};
	std::array<wxTextCtrl*, 32> m_fpr0{};
	std::array<wxTextCtrl*, 32> m_fpr1{};
	std::array<wxTextCtrl*, 8> m_cr{};

	PPCSnapshot m_last{};
	bool m_floatsAsDouble = true;
	wxColour m_highlight{0xFF0000FF};
};
