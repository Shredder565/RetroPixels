// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/RegisterWindow.h"

#include <string>

#include "wxgui/debugger/DebuggerWindow2.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/Espresso/EspressoISA.h"
#include "util/helpers/helpers.h"

namespace
{
	constexpr int kWidth = 400;
	constexpr int kHeight = 975;

	wxColour PlainText()
	{
		return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	}

	// borderless read-only text field that blends into the window background
	wxTextCtrl* AddField(wxWindow* owner, wxSizer* grid, const wxString& text, int flags, int border)
	{
		auto* field = new wxTextCtrl(owner, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxTE_READONLY | wxNO_BORDER | wxTE_RICH2);
		field->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
		field->SetForegroundColour(PlainText());
		grid->Add(field, 0, flags, border);
		return field;
	}

	void AddCaption(wxWindow* owner, wxSizer* grid, const wxString& text)
	{
		grid->Add(new wxStaticText(owner, wxID_ANY, text), 0, wxLEFT, 5);
	}

	bool InDataArea(uint32 addr)
	{
		return addr >= MEMORY_DATA_AREA_ADDR && addr < MEMORY_DATA_AREA_ADDR + MEMORY_DATA_AREA_SIZE;
	}

	// a NUL terminated run of printable 8-bit characters starting at addr
	bool ReadNarrowString(uint32 addr, std::string& out)
	{
		const uint32 end = MEMORY_DATA_AREA_ADDR + MEMORY_DATA_AREA_SIZE;
		for (uint32 p = addr; p < end; p++)
		{
			const uint8 ch = memory_readU8(p);
			if (ch == 0)
				return !out.empty();
			if (!isprint(ch))
				return false;
			out.push_back((char)ch);
		}
		return !out.empty();
	}

	// same for big-endian UTF-16
	bool ReadWideString(uint32 addr, std::wstring& out)
	{
		const uint32 end = MEMORY_DATA_AREA_ADDR + MEMORY_DATA_AREA_SIZE - 1;
		for (uint32 p = addr; p < end; p += 2)
		{
			const uint16 ch = memory_readU16(p);
			if (ch == 0)
				return !out.empty();
			if (!iswprint(ch))
				return false;
			out.push_back((wchar_t)ch);
		}
		return !out.empty();
	}

	wxString FormatFloat(bool asDouble, double value, uint64 bits)
	{
		return asDouble ? wxString::Format("%lf", value) : wxString::Format("%016llx", (unsigned long long)bits);
	}

	wxString DescribeCrField(const uint8* bits)
	{
		std::vector<std::string> flags;
		if (bits[Espresso::CR_BIT_INDEX_LT] != 0) flags.emplace_back("LT");
		if (bits[Espresso::CR_BIT_INDEX_GT] != 0) flags.emplace_back("GT");
		if (bits[Espresso::CR_BIT_INDEX_EQ] != 0) flags.emplace_back("EQ");
		if (bits[Espresso::CR_BIT_INDEX_SO] != 0) flags.emplace_back("SO");
		if (flags.empty())
			return "-";
		return fmt::format("{}", fmt::join(flags, ", "));
	}

	template<typename T, size_t N>
	int IndexOf(const std::array<T*, N>& list, const wxObject* obj)
	{
		for (size_t i = 0; i < N; i++)
		{
			if (list[i] == obj)
				return (int)i;
		}
		return -1;
	}
}

RegisterWindow::RegisterWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size)
	: wxFrame(&parent, wxID_ANY, _("Registers"), wxDefaultPosition, wxSize(kWidth, kHeight), wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);
	SetMaxSize(wxSize(kWidth, kHeight));
	SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

	auto* scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
	auto* column = new wxBoxSizer(wxVERTICAL);

	// integer registers: caption | value | what the value points at
	auto* gprGrid = new wxFlexGridSizer(0, 3, 0, 0);
	for (size_t row = 0; row < m_gprValue.size(); row++)
	{
		AddCaption(scroller, gprGrid, row < 32 ? wxString::Format("R%d", (int)row) : wxString("LR"));
		m_gprValue[row] = AddField(scroller, gprGrid, wxString::Format("%08x", 0), wxLEFT | wxRIGHT, 5);
		m_gprValue[row]->Bind(wxEVT_LEFT_DCLICK, &RegisterWindow::OnValueDoubleClick, this);
		m_gprHint[row] = AddField(scroller, gprGrid, wxEmptyString, wxEXPAND, 0);
		m_gprHint[row]->SetMinSize(wxSize(500, -1));
	}
	column->Add(gprGrid, 1, wxEXPAND);

	auto* formatButton = new wxButton(scroller, wxID_ANY, _("FP view mode"));
	formatButton->Bind(wxEVT_BUTTON, &RegisterWindow::OnToggleFloatFormat, this);
	column->Add(formatButton, 0, wxALL, 5);

	// paired singles: caption | ps0 | ps1
	auto* fprGrid = new wxFlexGridSizer(0, 3, 0, 0);
	for (size_t i = 0; i < m_fpr0.size(); i++)
	{
		AddCaption(scroller, fprGrid, wxString::Format("FP%d", (int)i));
		m_fpr0[i] = AddField(scroller, fprGrid, wxString::Format("%lf", 0.0), wxLEFT | wxRIGHT, 5);
		m_fpr1[i] = AddField(scroller, fprGrid, wxString::Format("%lf", 0.0), wxLEFT | wxRIGHT, 5);
		m_fpr0[i]->Bind(wxEVT_LEFT_DCLICK, &RegisterWindow::OnValueDoubleClick, this);
		m_fpr1[i]->Bind(wxEVT_LEFT_DCLICK, &RegisterWindow::OnValueDoubleClick, this);
	}
	column->Add(fprGrid, 0, wxEXPAND);

	auto* crGrid = new wxFlexGridSizer(0, 2, 0, 0);
	for (size_t i = 0; i < m_cr.size(); i++)
	{
		AddCaption(scroller, crGrid, wxString::Format("CR%d", (int)i));
		m_cr[i] = AddField(scroller, crGrid, "-", wxRIGHT, 5);
	}
	column->Add(crGrid, 0, wxEXPAND);

	scroller->SetSizerAndFit(column);
	scroller->SetScrollRate(-1, 10);

	auto* outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(scroller, 1, wxEXPAND);
	SetSizer(outer);
	Layout();

	if (parent.GetConfig().data().pin_to_main)
		OnMainMove(main_position, main_size);
}

void RegisterWindow::OnMainMove(const wxPoint& main_position, const wxSize& main_size)
{
	// right of the main window, reaching 255px above and 250px below it
	SetSize(wxSize(kWidth, main_size.y + 505));
	SetPosition(wxPoint(main_position.x + main_size.x, main_position.y - 255));
}

bool RegisterWindow::SetColourForChange(wxTextCtrl* field, bool changed) const
{
	// returns false when the field is already up to date and needs no new text
	if (changed)
	{
		field->SetForegroundColour(m_highlight);
		return true;
	}
	if (field->GetForegroundColour() == PlainText())
		return false;
	field->SetForegroundColour(PlainText());
	return true;
}

wxString RegisterWindow::DescribeAddress(uint32 value) const
{
	if (InDataArea(value))
	{
		std::string narrow;
		if (ReadNarrowString(value, narrow))
			return wxString::Format("\"%s\"", narrow.c_str());
		std::wstring wide;
		if (ReadWideString(value, wide))
			return wxString::Format(L"ws\"%s\"", wide.c_str());
	}
	if (RPLModule* mod = RPLLoader_FindModuleByCodeAddr(value))
		return wxString::Format("<%s> + %x", mod->moduleName.c_str(), value - mod->regionMappingBase_text.GetMPTR());
	return wxEmptyString;
}

void RegisterWindow::ShowGpr(size_t row, uint32 value, bool changed)
{
	wxTextCtrl* field = m_gprValue[row];
	if (changed)
		field->SetForegroundColour(m_highlight);
	else if (field->GetForegroundColour() != PlainText())
		field->SetForegroundColour(PlainText());
	field->ChangeValue(wxString::Format("%08x", value));

	m_gprHint[row]->SetForegroundColour(changed ? m_highlight : PlainText());
	m_gprHint[row]->ChangeValue(DescribeAddress(value));
}

void RegisterWindow::ShowFpr(wxTextCtrl* field, double asDouble, uint64 asBits)
{
	field->ChangeValue(FormatFloat(m_floatsAsDouble, asDouble, asBits));
}

void RegisterWindow::OnUpdateView()
{
	PPCInterpreter_t* session = debugger_lockDebugSession();
	if (session == nullptr)
		return;
	const PPCSnapshot now = debugger_getSnapshotFromSession(session);
	debugger_unlockDebugSession(session);

	for (size_t r = 0; r < 32; r++)
		ShowGpr(r, now.gpr[r], now.gpr[r] != m_last.gpr[r]);
	ShowGpr(32, now.spr_lr, now.spr_lr != m_last.spr_lr);

	for (size_t i = 0; i < 32; i++)
	{
		if (SetColourForChange(m_fpr0[i], now.fpr[i].fp0int != m_last.fpr[i].fp0int))
			ShowFpr(m_fpr0[i], now.fpr[i].fp0, now.fpr[i].fp0int);
	}
	for (size_t i = 0; i < 32; i++)
	{
		if (SetColourForChange(m_fpr1[i], now.fpr[i].fp1int != m_last.fpr[i].fp1int))
			ShowFpr(m_fpr1[i], now.fpr[i].fp1, now.fpr[i].fp1int);
	}

	for (size_t i = 0; i < 8; i++)
	{
		const uint8* bits = now.cr + i * 4;
		const bool changed = !std::equal(bits, bits + 4, m_last.cr + i * 4);
		if (SetColourForChange(m_cr[i], changed))
			m_cr[i]->ChangeValue(DescribeCrField(bits));
	}

	m_last = now;
}

void RegisterWindow::OnValueDoubleClick(wxMouseEvent& event)
{
	PPCInterpreter_t* session = debugger_lockDebugSession();
	if (session == nullptr)
	{
		event.Skip();
		return;
	}
	const PPCSnapshot snap = debugger_getSnapshotFromSession(session);
	debugger_unlockDebugSession(session);

	const wxObject* source = event.GetEventObject();
	const int gpr = IndexOf(m_gprValue, source);
	const int ps0 = IndexOf(m_fpr0, source);
	const int ps1 = IndexOf(m_fpr1, source);

	if (gpr >= 0 && gpr < 32)
	{
		wxTextEntryDialog ask(this, _("Enter a new value."), wxString::Format(_("Set R%d value"), gpr), wxString::Format("0x%08x", snap.gpr[gpr]));
		if (ask.ShowModal() != wxID_OK)
			return;
		const uint32 v = ConvertString<uint32>(ask.GetValue().ToStdString());
		if (PPCInterpreter_t* s = debugger_lockDebugSession())
		{
			s->gpr[gpr] = v;
			debugger_unlockDebugSession(s);
		}
		OnUpdateView();
		return;
	}

	if (ps0 >= 0 || ps1 >= 0)
	{
		const bool second = ps1 >= 0;
		const int reg = second ? ps1 : ps0;
		const double current = second ? snap.fpr[reg].fp1 : snap.fpr[reg].fp0;
		const wxString title = second ? wxString::Format(_("Set FP1_%d value"), reg) : wxString::Format(_("Set FP0_%d value"), reg);
		wxTextEntryDialog ask(this, _("Enter a new value."), title, wxString::Format("%lf", current));
		if (ask.ShowModal() != wxID_OK)
			return;
		const double v = ConvertString<double>(ask.GetValue().ToStdString());
		if (PPCInterpreter_t* s = debugger_lockDebugSession())
		{
			if (second)
				s->fpr[reg].fp1 = v;
			else
				s->fpr[reg].fp0 = v;
			debugger_unlockDebugSession(s);
		}
		OnUpdateView();
		return;
	}

	// LR is display only
	event.Skip();
}

void RegisterWindow::OnToggleFloatFormat(wxCommandEvent& /*event*/)
{
	m_floatsAsDouble = !m_floatsAsDouble;

	PPCInterpreter_t* session = debugger_lockDebugSession();
	if (session == nullptr)
		return;
	const PPCSnapshot snap = debugger_getSnapshotFromSession(session);
	debugger_unlockDebugSession(session);

	for (size_t i = 0; i < 32; i++)
	{
		ShowFpr(m_fpr0[i], snap.fpr[i].fp0, snap.fpr[i].fp0int);
		ShowFpr(m_fpr1[i], snap.fpr[i].fp1, snap.fpr[i].fp1int);
	}
}
