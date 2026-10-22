// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SaveImportWindow.h"

#include "pugixml.hpp"
#include <wx/sizer.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/frame.h>
#include <wx/filepicker.h>
#include <wx/msgdlg.h>
#include "zip.h"

#include "Cafe/Account/Account.h"
#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "util/helpers/helpers.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "wxgui/helpers/wxHelpers.h"

namespace
{
	// name of the archive entry the exporter writes the title id into ("titleId = 0x...")
	constexpr const char* kMetaEntryName = "cemu_meta";
	constexpr std::string_view kMetaTitleIdKey = "titleId = ";
	constexpr uint64 kMaxImportedFileSize = 128ull * 1024 * 1024;

	enum class EntryRead
	{
		CannotOpen,
		Short,
		Complete,
	};

	// reads a whole archive entry into out
	EntryRead ReadZipEntry(zip_t* archive, sint32 index, uint64 size, std::unique_ptr<char[]>& out)
	{
		zip_file_t* entry = zip_fopen_index(archive, index, 0);
		if (entry == nullptr)
			return EntryRead::CannotOpen;
		out = std::make_unique<char[]>(size);
		const bool complete = zip_fread(entry, out.get(), size) == (zip_int64_t)size;
		zip_fclose(entry);
		return complete ? EntryRead::Complete : EntryRead::Short;
	}

	// entry names that would land outside the destination folder
	bool IsUnsafeEntryName(std::string_view name)
	{
		if (name.starts_with("../") || name.starts_with("..\\"))
			return true;
		const fs::path p{std::string(name)};
		if (p.has_root_name() || p.has_root_directory())
			return true;
		for (const fs::path& part : p)
		{
			if (part == "..")
				return true;
		}
		return false;
	}
}

SaveImportWindow::SaveImportWindow(wxWindow* parent, uint64 title_id)
	: wxDialog(parent, wxID_ANY, _("Import save entry"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxFRAME_TOOL_WINDOW | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_title_id(title_id)
{
	auto* outer = new wxBoxSizer(wxVERTICAL);

	auto* fields = new wxFlexGridSizer(0, 2, 0, 0);
	fields->AddGrowableCol(1);
	fields->Add(new wxStaticText(this, wxID_ANY, _("Source")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_sourcePicker = new wxFilePickerCtrl(this, wxID_ANY, wxEmptyString, _("Select a zipped save file"), formatWxString("{}|*.zip", _("Save entry (*.zip)")));
	m_sourcePicker->SetMinSize({270, -1});
	fields->Add(m_sourcePicker, 1, wxALL | wxEXPAND, 5);

	fields->Add(new wxStaticText(this, wxID_ANY, _("Target")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_targetCombo = new wxComboBox(this, wxID_ANY);
	for (const Account& acc : Account::GetAccounts())
	{
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
	ok->Bind(wxEVT_BUTTON, &SaveImportWindow::OnImport, this);
	buttons->Add(ok, 0, wxALL, 5);
	auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
	cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Finish(wxCANCEL); });
	buttons->Add(cancel, 0, wxALIGN_RIGHT | wxALL, 5);
	outer->Add(buttons, 1, wxEXPAND, 5);

	SetSizerAndFit(outer);
	Centre(wxBOTH);
}

void SaveImportWindow::EndModal(int retCode)
{
	wxDialog::EndModal(retCode);
	SetReturnCode(m_result);
}

void SaveImportWindow::Finish(int result)
{
	m_result = result;
	Close();
}

// Looks for the exporter's meta entry; when it names another title, the user decides.
// Returns false when the import should stop (the dialog is then already closed).
bool SaveImportWindow::ConfirmTitleOfArchive(const std::string& zipPath)
{
	int err = 0;
	zip_t* archive = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
	if (archive == nullptr)
		return true;
	bool proceed = true;
	const sint32 count = (sint32)zip_get_num_entries(archive, 0);
	for (sint32 idx = 0; idx < count; idx++)
	{
		zip_stat_t st{};
		if (zip_stat_index(archive, idx, 0, &st) != 0 || !boost::equals(st.name, kMetaEntryName))
			continue;
		std::unique_ptr<char[]> content;
		const EntryRead got = ReadZipEntry(archive, idx, st.size, content);
		if (got == EntryRead::CannotOpen)
			continue;
		if (got == EntryRead::Complete)
		{
			const std::string_view text(content.get(), st.size);
			if (text.starts_with(kMetaTitleIdKey))
			{
				// value is written as {:#016x}, skip the "0x" as well
				const size_t valueStart = kMetaTitleIdKey.size() + 2;
				const uint64 archiveTitle = text.size() > valueStart ? ConvertString<uint64>(text.substr(valueStart), 16) : 0;
				if (archiveTitle != 0 && archiveTitle != m_title_id)
				{
					const wxString question = formatWxString(_("You are trying to import a savegame for a different title than your currently selected one: {:016x} vs {:016x}\nAre you sure that you want to continue?"), archiveTitle, m_title_id);
					if (wxMessageBox(question, _("Error"), wxYES_NO | wxCENTRE | wxICON_WARNING, this) == wxNO)
					{
						Finish(wxCANCEL);
						proceed = false;
					}
				}
			}
		}
		break; // only the first meta entry counts
	}
	zip_close(archive);
	return proceed;
}

bool SaveImportWindow::ExtractArchive(const std::string& zipPath, const fs::path& destination)
{
	int err = 0;
	zip_t* archive = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
	if (archive == nullptr)
		return false;
	const sint32 count = (sint32)zip_get_num_entries(archive, 0);
	for (sint32 idx = 0; idx < count; idx++)
	{
		zip_stat_t st{};
		if (zip_stat_index(archive, idx, 0, &st) != 0)
		{
			cemuLog_log(LogType::Force, "zip stat index failed on {} entry", idx);
			continue;
		}
		const std::string_view name(st.name);
		if (name.empty() || IsUnsafeEntryName(name) || name == kMetaEntryName)
			continue;

		const fs::path outPath = fs::path(destination).append(st.name);
		if (name.back() == '/')
		{
			std::error_code ec;
			fs::create_directories(outPath, ec);
			if (ec)
				cemuLog_log(LogType::Force, "can't create directory {}: {}", st.name, ec.message());
			continue;
		}
		// empty files are not recreated, oversized ones are ignored
		if (st.size == 0 || st.size > kMaxImportedFileSize)
			continue;

		std::unique_ptr<char[]> content;
		if (ReadZipEntry(archive, idx, st.size, content) != EntryRead::Complete)
			continue;
		std::ofstream out(outPath, std::ios::out | std::ios::binary);
		if (out.is_open())
			out.write(content.get(), st.size);
	}
	zip_close(archive);
	return true;
}

// Makes sure meta/saveinfo.xml has an <account> entry for the account (with the current time).
void SaveImportWindow::AddSaveInfoEntry(uint32 persistentId)
{
	const fs::path saveinfo = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/meta/saveinfo.xml", (uint32)(m_title_id >> 32), (uint32)(m_title_id & 0xFFFFFFFF));
	if (!fs::exists(saveinfo) && !fs::is_regular_file(saveinfo))
		return;
	pugi::xml_document doc;
	if (!doc.load_file(saveinfo.c_str()))
		return;
	pugi::xml_node info = doc.child("info");
	if (!info)
		return;
	const std::string idText = fmt::format("{:08x}", persistentId);
	const pugi::xml_node present = info.find_child([&idText](const pugi::xml_node& n) {
		return boost::iequals(n.attribute("persistentId").as_string(), idText);
	});
	if (present)
		return;
	pugi::xml_node entry = info.append_child("account");
	entry.append_attribute("persistentId").set_value(idText.c_str());
	entry.append_child("timestamp").text().set(fmt::format("{:016x}", coreinit::OSGetTime() / ESPRESSO_TIMER_CLOCK).c_str());
	if (!doc.save_file(saveinfo.c_str()))
		cemuLog_log(LogType::Force, "couldn't insert save entry in saveinfo.xml: {}", _pathToUtf8(saveinfo));
}

void SaveImportWindow::OnImport(wxCommandEvent& /*event*/)
{
	const wxString picked = m_sourcePicker->GetPath();
	if (picked.empty())
		return;
	const std::string zipPath = picked.ToUTF8().data();

	if (!ConfirmTitleOfArchive(zipPath))
		return;

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

	const fs::path destination = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user/{:08x}", (uint32)(m_title_id >> 32), (uint32)(m_title_id & 0xFFFFFFFF), targetId);
	if (fs::exists(destination))
	{
		if (!fs::is_directory(destination))
		{
			wxMessageBox(formatWxString(_("There's already a file at the target directory:\n{}"), _pathToUtf8(destination)), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			Finish(wxCANCEL);
			return;
		}
		const wxString question = _("There's already a save game available for the target account, do you want to overwrite it?\nThis will delete the existing save files for the account and replace them.");
		if (wxMessageBox(question, _("Error"), wxYES_NO | wxCENTRE | wxICON_WARNING, this) == wxNO)
		{
			Finish(wxCANCEL);
			return;
		}
		// remove the old save; retry until the folder is really gone
		std::error_code ec;
		while (fs::exists(destination, ec) || ec)
		{
			fs::remove_all(destination, ec);
			if (ec)
			{
				wxMessageBox(formatWxString(_("Error when trying to delete the former save game:\n{}"), GetSystemErrorMessage(ec)), _("Error"), wxOK | wxCENTRE, this);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	std::error_code ec;
	fs::create_directories(destination, ec);
	if (ec)
	{
		wxMessageBox(formatWxString(_("Error when creating the extraction path:\n{}"), GetSystemErrorMessage(ec)), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	if (!ExtractArchive(zipPath, destination))
	{
		wxMessageBox(formatWxString(_("Error when opening the import zip file:\n{}"), GetSystemErrorMessage(ec)), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	AddSaveInfoEntry(targetId);

	m_target_id = targetId;
	Finish(wxOK);
}
