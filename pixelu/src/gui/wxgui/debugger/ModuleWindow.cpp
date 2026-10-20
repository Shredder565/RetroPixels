// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/ModuleWindow.h"

#include "wxgui/debugger/DebuggerWindow2.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"

#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"

#include "Cafe/GraphicPack/GraphicPack2.h"

namespace
{
	enum : long
	{
		COL_NAME,
		COL_ADDRESS,
		COL_SIZE,
	};
	const wxSize kModuleWindowSize(420, 250);
}

ModuleWindow::ModuleWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size)
	: wxFrame(&parent, wxID_ANY, _("Modules"), wxDefaultPosition, kModuleWindowSize, wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	m_list = new wxListView(this, wxID_ANY);
	m_list->InsertColumn(COL_NAME, _("Name"), wxLIST_FORMAT_LEFT, 125);
	m_list->InsertColumn(COL_ADDRESS, _("Address"), wxLIST_FORMAT_RIGHT, 75);
	m_list->InsertColumn(COL_SIZE, _("Size"), wxLIST_FORMAT_RIGHT, 75);
	layout->Add(m_list, 1, wxEXPAND);
	SetSizer(layout);
	wxWindowBase::Layout();
	Centre(wxBOTH);

	if (parent.GetConfig().data().pin_to_main)
		OnMainMove(main_position, main_size);

	m_list->Bind(wxEVT_LEFT_DCLICK, &ModuleWindow::OnLeftDClick, this);
	OnGameLoaded();
}

void ModuleWindow::OnMainMove(const wxPoint& main_position, const wxSize& main_size)
{
	// left of the main window, below its bottom edge
	SetSize(kModuleWindowSize);
	SetPosition(wxPoint(main_position.x - kModuleWindowSize.x, main_position.y + main_size.y));
}

void ModuleWindow::AddRow(const wxString& name, uint32 address, uint32 size)
{
	const long row = m_list->InsertItem(m_list->GetItemCount(), name);
	m_list->SetItem(row, COL_ADDRESS, wxString::Format("%08x", address));
	m_list->SetItem(row, COL_SIZE, wxString::Format("%x", size));
}

void ModuleWindow::OnGameLoaded()
{
	Freeze();
	m_list->DeleteAllItems();
	for (RPLModule* mod : RPLLoader_GetModuleList())
	{
		if (mod != nullptr)
			AddRow(mod->moduleName, mod->regionMappingBase_text.GetMPTR(), mod->regionSize_text);
	}
	for (auto& pack : GraphicPack2::GetGraphicPacks())
	{
		for (auto& group : pack->GetPatchGroups())
		{
			if (group->isApplied())
				AddRow(std::string(group->getName()), group->getCodeCaveBase(), group->getCodeCaveSize());
		}
	}
	Thaw();
}

void ModuleWindow::OnLeftDClick(wxMouseEvent& /*event*/)
{
	const long row = m_list->GetFirstSelected();
	if (row == wxNOT_FOUND)
		return;
	const uint32 address = (uint32)std::stoul(m_list->GetItemText(row, COL_ADDRESS).ToStdString(), nullptr, 16);
	if (address != 0)
		debugger_jumpToAddressInDisasm(address);
}
