// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/BreakpointWindow.h"

#include <sstream>

#include "wxgui/debugger/DebuggerWindow2.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"

#include "Cemu/ExpressionParser/ExpressionParser.h"

namespace
{
	enum
	{
		ID_NEW_EXECUTE_BP = 1,
		ID_NEW_LOGGING_BP,
		ID_NEW_READ_BP,
		ID_NEW_WRITE_BP,
		ID_DELETE_BP,
	};

	enum : long
	{
		COL_ENABLED,
		COL_ADDRESS,
		COL_TYPE,
		COL_COMMENT,
	};

	const wxSize kBreakpointWindowSize(420, 250);

	const char* TypeTag(uint8 bpType)
	{
		switch (bpType)
		{
		case DEBUGGER_BP_T_NORMAL: return "X";
		case DEBUGGER_BP_T_LOGGING: return "LOG";
		case DEBUGGER_BP_T_ONE_SHOT: return "XS";
		case DEBUGGER_BP_T_MEMORY_READ: return "R";
		case DEBUGGER_BP_T_MEMORY_WRITE: return "W";
		default: return "UKN";
		}
	}
}

BreakpointWindow::BreakpointWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size)
	: wxFrame(&parent, wxID_ANY, _("Breakpoints"), wxDefaultPosition, kBreakpointWindowSize, wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	m_list = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	m_list->EnableCheckBoxes(true);
	m_list->InsertColumn(COL_ENABLED, _("On"), wxLIST_FORMAT_LEFT, 32);
	m_list->InsertColumn(COL_ADDRESS, _("Address"), wxLIST_FORMAT_LEFT, 75);
	m_list->InsertColumn(COL_TYPE, _("Type"), wxLIST_FORMAT_LEFT, 42);
	m_list->InsertColumn(COL_COMMENT, _("Comment"), wxLIST_FORMAT_LEFT, 250);
	layout->Add(m_list, 1, wxEXPAND);
	SetSizer(layout);
	wxWindowBase::Layout();
	Centre(wxBOTH);

	if (parent.GetConfig().data().pin_to_main)
		OnMainMove(main_position, main_size);

	m_list->Bind(wxEVT_LIST_ITEM_CHECKED, &BreakpointWindow::OnBreakpointToggled, this);
	m_list->Bind(wxEVT_LIST_ITEM_UNCHECKED, &BreakpointWindow::OnBreakpointToggled, this);
	m_list->Bind(wxEVT_LEFT_DCLICK, &BreakpointWindow::OnLeftDClick, this);
	m_list->Bind(wxEVT_RIGHT_DOWN, &BreakpointWindow::OnRightDown, this);

	OnUpdateView();
}

BreakpointWindow::~BreakpointWindow()
{
	m_list->Unbind(wxEVT_LIST_ITEM_CHECKED, &BreakpointWindow::OnBreakpointToggled, this);
	m_list->Unbind(wxEVT_LIST_ITEM_UNCHECKED, &BreakpointWindow::OnBreakpointToggled, this);
	m_list->Unbind(wxEVT_LEFT_DCLICK, &BreakpointWindow::OnLeftDClick, this);
	m_list->Unbind(wxEVT_RIGHT_DOWN, &BreakpointWindow::OnRightDown, this);
}

void BreakpointWindow::OnMainMove(const wxPoint& main_position, const wxSize& main_size)
{
	// left of the main window, bottom edges aligned
	SetSize(kBreakpointWindowSize);
	SetPosition(wxPoint(main_position.x - kBreakpointWindowSize.x, main_position.y + main_size.y - kBreakpointWindowSize.y));
}

void BreakpointWindow::OnUpdateView()
{
	Freeze();
	m_list->DeleteAllItems();
	// every address holds a chain of breakpoints; one row each, keyed by breakpoint id
	std::vector<DebuggerBreakpoint*>& chains = debugger_lockBreakpoints();
	long row = 0;
	for (DebuggerBreakpoint* head : chains)
	{
		for (DebuggerBreakpoint* bp = head; bp != nullptr; bp = bp->next, row++)
		{
			const long index = m_list->InsertItem(row, wxEmptyString);
			m_list->SetItemPtrData(index, (wxUIntPtr)bp->id);
			m_list->SetItem(index, COL_ADDRESS, wxString::Format("%08x", bp->address));
			m_list->SetItem(index, COL_TYPE, TypeTag(bp->bpType));
			m_list->SetItem(index, COL_COMMENT, bp->comment);
			m_list->CheckItem(index, bp->enabled);
		}
	}
	debugger_unlockBreakpoints();
	Thaw();
}

void BreakpointWindow::OnGameLoaded()
{
	OnUpdateView();
}

void BreakpointWindow::OnBreakpointToggled(wxListEvent& event)
{
	const long index = event.GetIndex();
	const BreakpointId id = (BreakpointId)event.GetData();
	debugger_lockBreakpoints();
	if (DebuggerBreakpoint* bp = debugger_getBreakpointById(id))
		debugger_toggleBreakpoint(bp->address, m_list->IsItemChecked(index), bp);
	debugger_unlockBreakpoints();
}

void BreakpointWindow::OnLeftDClick(wxMouseEvent& event)
{
	const wxPoint at = event.GetPosition();
	int hitFlags = 0;
	const long row = m_list->HitTest(at, hitFlags);
	if (row == wxNOT_FOUND)
		return;

	// which column was clicked
	sint32 x = at.x - m_list->GetColumnWidth(COL_ENABLED);
	if (x <= 0)
		return;
	if (x <= m_list->GetColumnWidth(COL_ADDRESS))
	{
		const uint32 address = (uint32)std::stoul(m_list->GetItemText(row, COL_ADDRESS).ToStdString(), nullptr, 16);
		g_debuggerDispatcher.MoveToAddressInDisassembly(address);
		return;
	}
	x -= m_list->GetColumnWidth(COL_ADDRESS);
	if (x <= m_list->GetColumnWidth(COL_TYPE))
		return;
	x -= m_list->GetColumnWidth(COL_TYPE);
	if (x <= m_list->GetColumnWidth(COL_COMMENT))
		EditComment(row);
}

void BreakpointWindow::EditComment(long row)
{
	const BreakpointId id = (BreakpointId)m_list->GetItemData(row);
	bool isLogging = false;
	uint32 address = 0;
	wxString current;
	debugger_lockBreakpoints();
	DebuggerBreakpoint* bp = debugger_getBreakpointById(id);
	if (bp != nullptr)
	{
		isLogging = bp->bpType == DEBUGGER_BP_T_LOGGING;
		address = bp->address;
		current = bp->comment;
	}
	debugger_unlockBreakpoints();
	if (bp == nullptr)
		return;

	// logging breakpoints print their comment as log message
	const wxString title = isLogging ? _("Enter a new logging message") : _("Enter a new comment");
	const wxString message = wxString::Format(isLogging ? _("Set logging message when code at address %08x is ran.\nUse placeholders like {r3} or {f3} to log register values") : _("Set comment for breakpoint at address %08x"), address);
	wxTextEntryDialog ask(this, message, title, current);
	if (ask.ShowModal() != wxID_OK)
		return;

	debugger_lockBreakpoints();
	if (DebuggerBreakpoint* target = debugger_getBreakpointById(id))
	{
		target->comment = ask.GetValue().ToStdWstring();
		m_list->SetItem(row, COL_COMMENT, ask.GetValue());
	}
	debugger_unlockBreakpoints();
}

void BreakpointWindow::OnRightDown(wxMouseEvent& event)
{
	int hitFlags = 0;
	const long row = m_list->HitTest(event.GetPosition(), hitFlags);
	wxMenu menu;
	if (row == wxNOT_FOUND || row < 0 || row >= m_list->GetItemCount())
	{
		menu.Append(ID_NEW_EXECUTE_BP, _("Create execution breakpoint"));
		menu.Append(ID_NEW_LOGGING_BP, _("Create logging breakpoint"));
		menu.Append(ID_NEW_READ_BP, _("Create memory breakpoint (read)"));
		menu.Append(ID_NEW_WRITE_BP, _("Create memory breakpoint (write)"));
		menu.Bind(wxEVT_MENU, &BreakpointWindow::OnContextMenuClick, this);
	}
	else
	{
		m_list->Focus(row);
		m_list->Select(row);
		menu.Append(ID_DELETE_BP, _("Delete breakpoint"));
		menu.Bind(wxEVT_MENU, &BreakpointWindow::OnContextMenuClickSelected, this);
	}
	PopupMenu(&menu);
}

void BreakpointWindow::OnContextMenuClickSelected(wxCommandEvent& evt)
{
	std::vector<DebuggerBreakpoint*>& chains = debugger_lockBreakpoints();
	const long row = m_list->GetFirstSelected();
	DebuggerBreakpoint* bp = nullptr;
	if (row != wxNOT_FOUND && row >= 0 && row < (long)chains.size())
		bp = debugger_getBreakpointById((BreakpointId)m_list->GetItemData(row));
	if (bp != nullptr && evt.GetId() == ID_DELETE_BP)
	{
		debugger_deleteBreakpoint(bp->id);
		wxCommandEvent changed(wxEVT_BREAKPOINT_CHANGE);
		wxPostEvent(GetParent(), changed);
	}
	debugger_unlockBreakpoints();
}

void BreakpointWindow::OnContextMenuClick(wxCommandEvent& evt)
{
	wxTextEntryDialog ask(this, _("Enter a memory address"), _("Set breakpoint"), wxEmptyString);
	if (ask.ShowModal() != wxID_OK)
		return;

	std::string input = ask.GetValue().ToStdString();
	std::transform(input.begin(), input.end(), input.begin(), tolower);

	uint32 address = 0;
	try
	{
		ExpressionParser parser;
		debugger_addParserSymbols(parser);
		// a bare number is read as hex
		const std::string asHex = "0x" + input;
		address = parser.IsConstantExpression(asHex) ? (uint32)parser.Evaluate(asHex) : (uint32)parser.Evaluate(input);
	}
	catch (const std::exception& ex)
	{
		wxMessageBox(ex.what(), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	switch (evt.GetId())
	{
	case ID_NEW_EXECUTE_BP: debugger_createCodeBreakpoint(address, DEBUGGER_BP_T_NORMAL); break;
	case ID_NEW_LOGGING_BP: debugger_createCodeBreakpoint(address, DEBUGGER_BP_T_LOGGING); break;
	case ID_NEW_READ_BP: debugger_createMemoryBreakpoint(address, true, false); break;
	case ID_NEW_WRITE_BP: debugger_createMemoryBreakpoint(address, false, true); break;
	default: break;
	}
	OnUpdateView();
}
