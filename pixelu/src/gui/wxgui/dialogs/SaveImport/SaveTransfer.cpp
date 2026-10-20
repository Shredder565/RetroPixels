// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SaveTransfer.h"

#include "pugixml.hpp"
#include <wx/sizer.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>

#include "Cafe/Account/Account.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "wxgui/helpers/wxHelpers.h"

SaveTransfer::SaveTransfer(wxWindow* parent, uint64 title_id, const wxString& source_account, uint32 source_id)
	: wxDialog(parent, wxID_ANY, _("Save transfer"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxFRAME_TOOL_WINDOW | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_title_id(title_id), m_source_id(source_id)
{
	auto* outer = new wxBoxSizer(wxVERTICAL);

	auto* fields = new wxFlexGridSizer(0, 2, 0, 0);
	fields->AddGrowableCol(1);
	fields->Add(new wxStaticText(this, wxID_ANY, _("Source")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	// the source is fixed, the choice only shows it
	auto* sourceView = new wxChoice(this, wxID_ANY);
	sourceView->SetMinSize({170, -1});
	sourceView->Append(source_account);
	sourceView->SetSelection(0);
	fields->Add(sourceView, 1, wxALL | wxEXPAND, 5);

	fields->Add(new wxStaticText(this, wxID_ANY, _("Target")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_targetCombo = new wxComboBox(this, wxID_ANY);
	for (const Account& acc : Account::GetAccounts())
	{
		if (acc.GetPersistentId() == m_source_id)
			continue;
		const auto& mii = acc.GetMiiName();
		const std::string label = fmt::format("{:x} ({})", acc.GetPersistentId(), boost::nowide::narrow(mii.data(), mii.size()));
		m_targetCombo->Append(label, (void*)(uintptr_t)acc.GetPersistentId());
	}
	fields->Add(m_targetCombo, 1, wxALL | wxEXPAND, 5);
	outer->Add(fields, 0, wxEXPAND, 5);

	auto* buttons = new wxFlexGridSizer(0, 2, 0, 0);
	buttons->AddGrowableCol(1);
	buttons->SetFlexibleDirection(wxBOTH);
	buttons->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
	auto* ok = new wxButton(this, wxID_ANY, _("OK"));
	ok->Bind(wxEVT_BUTTON, &SaveTransfer::OnTransfer, this);
	buttons->Add(ok, 0, wxALL, 5);
	auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
	cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Finish(wxCANCEL); });
	buttons->Add(cancel, 0, wxALIGN_RIGHT | wxALL, 5);
	outer->Add(buttons, 1, wxEXPAND, 5);

	SetSizerAndFit(outer);
	Centre(wxBOTH);
}

void SaveTransfer::EndModal(int retCode)
{
	wxDialog::EndModal(retCode);
	SetReturnCode(m_result);
}

void SaveTransfer::Finish(int result)
{
	m_result = result;
	Close();
}

fs::path SaveTransfer::UserSavePath(uint32 persistentId) const
{
	return ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user/{:08x}", (uint32)(m_title_id >> 32), (uint32)(m_title_id & 0xFFFFFFFF), persistentId);
}

// In meta/saveinfo.xml: drop the target's old <account> entry and relabel the
// source's entry with the target id. Returns whether the file was written.
bool SaveTransfer::RewriteSaveInfo(uint32 targetId)
{
	const fs::path saveinfo = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/meta/saveinfo.xml", (uint32)(m_title_id >> 32), (uint32)(m_title_id & 0xFFFFFFFF));
	bool written = false;
	if (fs::exists(saveinfo) || fs::is_regular_file(saveinfo))
	{
		pugi::xml_document doc;
		pugi::xml_node info;
		if (doc.load_file(saveinfo.c_str()))
			info = doc.child("info");
		if (info)
		{
			auto entryFor = [&info](const std::string& idText) {
				return info.find_child([&idText](const pugi::xml_node& n) {
					return boost::iequals(n.attribute("persistentId").as_string(), idText);
				});
			};
			const std::string targetText = fmt::format("{:08x}", targetId);
			if (pugi::xml_node stale = entryFor(targetText))
				info.remove_child(stale);
			if (pugi::xml_node moved = entryFor(fmt::format("{:08x}", m_source_id)))
				moved.attribute("persistentId").set_value(targetText.c_str());
			written = doc.save_file(saveinfo.c_str());
		}
	}
	if (!written)
		cemuLog_log(LogType::Force, "SaveTransfer::OnTransfer: couldn't update save entry in saveinfo.xml: {}", _pathToUtf8(saveinfo));
	return written;
}

void SaveTransfer::OnTransfer(wxCommandEvent& /*event*/)
{
	// account: a listed one, or a hex id typed into the combo box
	uint32 targetId = 0;
	const int chosen = m_targetCombo->GetCurrentSelection();
	if (chosen != wxNOT_FOUND)
		targetId = (uint32)(uintptr_t)m_targetCombo->GetClientData(chosen);
	if (targetId == 0)
	{
		targetId = ConvertString<uint32>(m_targetCombo->GetValue().ToStdString(), 16);
		if (targetId < Account::kMinPersistendId)
		{
			wxMessageBox(formatWxString(_("The given account id is not valid!\nIt must be a hex number bigger or equal than {:08x}"), Account::kMinPersistendId),
				_("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
	}

	const fs::path from = UserSavePath(m_source_id);
	if (!fs::exists(from) || !fs::is_directory(from))
		return;

	const fs::path to = UserSavePath(targetId);
	if (fs::exists(to))
	{
		if (!fs::is_directory(to))
		{
			wxMessageBox(formatWxString(_("There's already a file at the target directory:\n{}"), _pathToUtf8(to)), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			Finish(wxCANCEL);
			return;
		}
		const wxString question = _("There's already a save game available for the target account, do you want to overwrite it?\nThis will delete the existing save files for the account and replace them.");
		if (wxMessageBox(question, _("Error"), wxYES_NO | wxCENTRE | wxICON_WARNING, this) == wxNO)
		{
			Finish(wxCANCEL);
			return;
		}
		// remove the target's save; retry until the folder is really gone
		std::error_code ec;
		while (fs::exists(to, ec) || ec)
		{
			fs::remove_all(to, ec);
			if (ec)
			{
				wxMessageBox(formatWxString(_("Error when trying to delete the former save game:\n{}"), GetSystemErrorMessage(ec)), _("Error"), wxOK | wxCENTRE, this);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	RewriteSaveInfo(targetId);

	std::error_code ec;
	fs::rename(from, to, ec);
	if (ec)
	{
		wxMessageBox(formatWxString(_("Error when trying to move the save game:\n{}"), GetSystemErrorMessage(ec)), _("Error"), wxOK | wxCENTRE, this);
		return;
	}

	m_target_id = targetId;
	Finish(wxOK);
}
