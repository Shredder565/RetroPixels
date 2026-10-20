// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "EmulatedUSBDeviceFrame.h"

#include <algorithm>

#include "config/CemuConfig.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/wxHelper.h"
#include "util/helpers/helpers.h"

#include "Cafe/OS/libs/nsyshid/nsyshid.h"
#include "Cafe/OS/libs/nsyshid/Dimensions.h"

#include "Common/FileStream.h"

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>
#include <wx/filedlg.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/stream.h>
#include <wx/textctrl.h>
#include <wx/textentry.h>
#include <wx/valnum.h>
#include <wx/wfstream.h>

#include "resource/embedded/resources.h"

namespace
{
	// client data of the "---Select---" entry of the figure pickers
	constexpr uintptr_t kNoPick = 0xFFFFFFFF;

	void ShowMessage(wxWindow* parent, const wxString& text, const wxString& caption = wxMessageBoxCaptionStr)
	{
		wxMessageDialog(parent, text, caption).ShowModal();
	}

	// file name -> opened figure dump, or null with an error shown
	std::unique_ptr<FileStream> OpenDump(wxWindow* parent, const wxString& path, const wxString& openError)
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(_utf8ToPath(path.utf8_string()), true));
		if (!file)
			ShowMessage(parent, openError);
		return file;
	}

	template<size_t N>
	bool ReadDump(wxWindow* parent, FileStream& file, std::array<uint8, N>& out, const wxString& shortError)
	{
		if (file.readData(out.data(), out.size()) == out.size())
			return true;
		ShowMessage(parent, shortError);
		return false;
	}

	wxCheckBox* AddEnableBox(wxStaticBox* box, wxStaticBoxSizer* boxSizer, const wxString& label, ConfigValue<bool>& setting)
	{
		auto* check = new wxCheckBox(box, wxID_ANY, label);
		check->SetValue(setting.GetValue());
		check->Bind(wxEVT_CHECKBOX, [check, &setting](wxCommandEvent&) {
			setting = check->IsChecked();
			GetConfigHandle().Save();
		});
		auto* row = new wxBoxSizer(wxHORIZONTAL);
		row->Add(check, 1, wxEXPAND | wxALL, 2);
		boxSizer->Add(row, 1, wxEXPAND | wxALL, 2);
		return check;
	}

	// combo box listing figures with auto completion, "---Select---" first
	wxComboBox* AddFigurePicker(wxWindow* parent, wxSizer* sizer, const std::vector<std::pair<wxString, uintptr_t>>& entries)
	{
		auto* combo = new wxComboBox(parent, wxID_ANY);
		combo->Append("---Select---", reinterpret_cast<void*>(kNoPick));
		wxArrayString names;
		for (const auto& [name, data] : entries)
		{
			combo->Append(name, reinterpret_cast<void*>(data));
			names.Add(name);
		}
		combo->SetSelection(0);
		combo->AutoComplete(names);
		sizer->Add(combo, 1, wxEXPAND | wxALL, 2);
		return combo;
	}

	// picked figure's client data, nothing for "---Select---"
	std::optional<uintptr_t> PickedFigure(wxComboBox* combo)
	{
		const uintptr_t v = reinterpret_cast<uintptr_t>(combo->GetClientData(combo->GetSelection()));
		if (v == kNoPick)
			return std::nullopt;
		return v;
	}

	wxTextCtrl* AddNumberField(wxWindow* parent, wxSizer* row, const wxString& caption)
	{
		wxIntegerValidator<uint32> digitsOnly;
		row->Add(new wxStaticText(parent, wxID_ANY, caption), 1, wxALL, 5);
		auto* field = new wxTextCtrl(parent, wxID_ANY, _("0"), wxDefaultPosition, wxDefaultSize, 0, digitsOnly);
		row->Add(field, 1, wxALL, 5);
		return field;
	}

	// create / cancel buttons, the dialog's three rows go into a vertical sizer
	void FinishCreatorLayout(wxDialog* dlg, wxSizer* pickerRow, wxSizer* numberRow, std::function<void()> onCreate)
	{
		auto* buttons = new wxBoxSizer(wxHORIZONTAL);
		auto* create = new wxButton(dlg, wxID_ANY, _("Create"));
		create->Bind(wxEVT_BUTTON, [onCreate](wxCommandEvent&) { onCreate(); });
		auto* cancel = new wxButton(dlg, wxID_ANY, _("Cancel"));
		cancel->Bind(wxEVT_BUTTON, [dlg](wxCommandEvent&) { dlg->EndModal(0); });
		buttons->Add(create, 1, wxALL, 5);
		buttons->Add(cancel, 1, wxALL, 5);

		auto* column = new wxBoxSizer(wxVERTICAL);
		column->Add(pickerRow, 1, wxEXPAND | wxALL, 2);
		column->Add(numberRow, 1, wxEXPAND | wxALL, 2);
		column->Add(buttons, 1, wxEXPAND | wxALL, 2);
		dlg->SetSizer(column);
		dlg->Centre(wxBOTH);
	}

	// which Infinity figures fit a base slot: 0 play set / power disc, 1-2 power discs,
	// 3 and 6 characters, the rest abilities
	bool InfinitySlotAccepts(uint8 slot, uint32 figure)
	{
		const bool playSet = figure > 0x1E8480 && figure < 0x2DC6BF;
		const bool powerDisc = figure > 0x3D0900 && figure < 0x4C4B3F;
		const bool character = figure < 0x1E847F;
		const bool ability = figure > 0x2DC6C0 && figure < 0x3D08FF;
		switch (slot)
		{
		case 0: return playSet || powerDisc;
		case 1:
		case 2: return powerDisc;
		case 3:
		case 6: return character;
		case 4:
		case 5:
		case 7:
		case 8: return ability;
		default: return false;
		}
	}
}

// ---------------------------------------------------------------------------
// frame

EmulatedUSBDeviceFrame::EmulatedUSBDeviceFrame(wxWindow* parent)
	: wxFrame(parent, wxID_ANY, _("Emulated USB Devices"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL)
{
	SetIcon(wxICON(X_BOX));

	auto* book = new wxNotebook(this, wxID_ANY);
	book->AddPage(BuildSkylanderPage(book), _("Skylanders Portal"));
	book->AddPage(BuildInfinityPage(book), _("Infinity Base"));
	book->AddPage(BuildDimensionsPage(book), _("Dimensions Toypad"));

	auto* sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(book, 1, wxEXPAND | wxALL, 2);
	SetSizerAndFit(sizer);
	Layout();
	Centre(wxBOTH);
}

EmulatedUSBDeviceFrame::~EmulatedUSBDeviceFrame() = default;

std::array<std::optional<uint32>, 7> EmulatedUSBDeviceFrame::GetCurrentMinifigs()
{
	return m_dimSlots;
}

wxBoxSizer* EmulatedUSBDeviceFrame::BuildSlotRow(wxStaticBox* box, const wxString& caption, wxTextCtrl*& field, int fieldBorder,
												  std::function<void()> load, std::function<void()> create, std::function<void()> clear)
{
	// caption | figure name | Load | Create | Clear
	auto* row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(new wxStaticText(box, wxID_ANY, caption), 1, wxEXPAND | wxALL, 2);
	field = new wxTextCtrl(box, wxID_ANY, _("None"), wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
	field->SetMinSize(wxSize(150, -1));
	field->Disable();
	row->Add(field, 1, wxEXPAND | wxALL, fieldBorder);
	const std::pair<wxString, std::function<void()>> buttons[] = {{_("Load"), load}, {_("Create"), create}, {_("Clear"), clear}};
	for (const auto& [label, action] : buttons)
	{
		auto* b = new wxButton(box, wxID_ANY, label);
		b->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
		row->Add(b, 1, wxEXPAND | wxALL, 2);
	}
	return row;
}

wxPanel* EmulatedUSBDeviceFrame::BuildSkylanderPage(wxNotebook* book)
{
	auto* page = new wxPanel(book);
	auto* box = new wxStaticBox(page, wxID_ANY, _("Skylanders Manager"));
	auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
	m_emulatePortal = AddEnableBox(box, boxSizer, _("Emulate Skylander Portal"), GetConfig().emulated_usb_devices.emulate_skylander_portal);
	for (uint8 i = 0; i < nsyshid::MAX_SKYLANDERS; i++)
	{
		boxSizer->Add(BuildSlotRow(box, _("Skylander") + wxString::Format(" %d", i + 1), m_skylanderSlots[i], 2,
								   [this, i] { LoadSkylander(i); }, [this, i] { CreateSkylander(i); }, [this, i] { ClearSkylander(i); }),
					  1, wxEXPAND | wxALL, 2);
	}
	auto* pageSizer = new wxBoxSizer(wxVERTICAL);
	pageSizer->Add(boxSizer, 1, wxEXPAND | wxALL, 2);
	page->SetSizerAndFit(pageSizer);
	return page;
}

wxPanel* EmulatedUSBDeviceFrame::BuildInfinityPage(wxNotebook* book)
{
	auto* page = new wxPanel(book);
	auto* box = new wxStaticBox(page, wxID_ANY, _("Infinity Manager"));
	auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
	m_emulateBase = AddEnableBox(box, boxSizer, _("Emulate Infinity Base"), GetConfig().emulated_usb_devices.emulate_infinity_base);
	static const char* const kSlotNames[nsyshid::MAX_FIGURES] = {
		"Play Set/Power Disc", "Power Disc Two", "Power Disc Three",
		"Player One", "Player One Ability One", "Player One Ability Two",
		"Player Two", "Player Two Ability One", "Player Two Ability Two",
	};
	for (uint8 i = 0; i < nsyshid::MAX_FIGURES; i++)
	{
		boxSizer->Add(BuildSlotRow(box, kSlotNames[i], m_infinitySlots[i], 5,
								   [this, i] { LoadFigure(i); }, [this, i] { CreateFigure(i); }, [this, i] { ClearFigure(i); }),
					  1, wxEXPAND | wxALL, 2);
	}
	auto* pageSizer = new wxBoxSizer(wxVERTICAL);
	pageSizer->Add(boxSizer, 1, wxEXPAND | wxALL, 2);
	page->SetSizerAndFit(pageSizer);
	return page;
}

wxBoxSizer* EmulatedUSBDeviceFrame::BuildToypadSpot(wxStaticBox* box, uint8 pad, uint8 index)
{
	// figure name | Move, then Clear | Create | Load
	auto* top = new wxBoxSizer(wxHORIZONTAL);
	m_dimensionSlots[index] = new wxTextCtrl(box, wxID_ANY, _("None"), wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
	top->Add(m_dimensionSlots[index], 1, wxEXPAND | wxALL, 2);
	auto* move = new wxButton(box, wxID_ANY, _("Move"));
	move->Bind(wxEVT_BUTTON, [this, pad, index](wxCommandEvent&) { MoveMinifig(pad, index); });
	top->Add(move, 1, wxEXPAND | wxALL, 2);

	auto* bottom = new wxBoxSizer(wxHORIZONTAL);
	const std::pair<wxString, std::function<void()>> buttons[] = {
		{_("Clear"), [this, pad, index] { ClearMinifig(pad, index); }},
		{_("Create"), [this, pad, index] { CreateMinifig(pad, index); }},
		{_("Load"), [this, pad, index] { LoadMinifig(pad, index); }},
	};
	for (const auto& [label, action] : buttons)
	{
		auto* b = new wxButton(box, wxID_ANY, label);
		b->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
		bottom->Add(b, 1, wxEXPAND | wxALL, 2);
	}

	auto* spot = new wxBoxSizer(wxVERTICAL);
	spot->Add(top, 1, wxEXPAND | wxALL, 2);
	spot->Add(bottom, 1, wxEXPAND | wxALL, 2);
	return spot;
}

wxPanel* EmulatedUSBDeviceFrame::BuildDimensionsPage(wxNotebook* book)
{
	auto* page = new wxPanel(book);
	auto* box = new wxStaticBox(page, wxID_ANY, _("Dimensions Manager"));
	auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
	m_emulateToypad = AddEnableBox(box, boxSizer, _("Emulate Dimensions Toypad"), GetConfig().emulated_usb_devices.emulate_dimensions_toypad);

	// the toypad's 7 spots as on the device: left pad 2, centre pad 1, right pad 3 on top;
	// two more spots of the left and right pads below
	auto* upper = new wxBoxSizer(wxHORIZONTAL);
	upper->Add(BuildToypadSpot(box, 2, 0), 1, wxEXPAND | wxALL, 2);
	upper->Add(0, 0, 1, wxEXPAND | wxLEFT | wxRIGHT, 2);
	upper->Add(BuildToypadSpot(box, 1, 1), 1, wxEXPAND | wxALL, 2);
	upper->Add(0, 0, 1, wxEXPAND | wxLEFT | wxRIGHT, 2);
	upper->Add(BuildToypadSpot(box, 3, 2), 1, wxEXPAND | wxALL, 2);

	auto* lower = new wxBoxSizer(wxHORIZONTAL);
	lower->Add(BuildToypadSpot(box, 2, 3), 1, wxEXPAND | wxALL, 2);
	lower->Add(BuildToypadSpot(box, 2, 4), 1, wxEXPAND | wxALL, 2);
	lower->Add(0, 0, 1, wxEXPAND | wxLEFT | wxRIGHT, 0);
	lower->Add(BuildToypadSpot(box, 3, 5), 1, wxEXPAND | wxALL, 2);
	lower->Add(BuildToypadSpot(box, 3, 6), 1, wxEXPAND | wxALL, 2);

	boxSizer->Add(upper, 1, wxEXPAND | wxALL, 2);
	boxSizer->Add(lower, 1, wxEXPAND | wxALL, 2);
	auto* pageSizer = new wxBoxSizer(wxVERTICAL);
	pageSizer->Add(boxSizer, 1, wxEXPAND | wxALL, 2);
	page->SetSizerAndFit(pageSizer);
	return page;
}

// ---------------------------------------------------------------------------
// Skylanders

void EmulatedUSBDeviceFrame::LoadSkylander(uint8 slot)
{
	wxFileDialog pick(this, _("Open Skylander dump"), "", "", "Skylander files (*.sky;*.bin;*.dump;*.dmp)|*.sky;*.bin;*.dump;*.dmp", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (pick.ShowModal() == wxID_OK && !pick.GetPath().empty())
		LoadSkylanderPath(slot, pick.GetPath());
}

void EmulatedUSBDeviceFrame::LoadSkylanderPath(uint8 slot, wxString path)
{
	std::unique_ptr<FileStream> file = OpenDump(this, path, "Error Opening File: " + path);
	if (!file)
		return;
	std::array<uint8, nsyshid::SKY_FIGURE_SIZE> dump;
	if (!ReadDump(this, *file, dump, "Failed to read file! File was too small"))
		return;
	ClearSkylander(slot);
	// id and variant are little endian at 0x10 and 0x1C
	const uint16 id = uint16(dump[0x11]) << 8 | uint16(dump[0x10]);
	const uint16 variant = uint16(dump[0x1D]) << 8 | uint16(dump[0x1C]);
	const uint8 portalSlot = nsyshid::g_skyportal.LoadSkylander(dump.data(), std::move(file));
	m_skySlots[slot] = std::tuple(portalSlot, id, variant);
	RefreshSkylanderNames();
}

void EmulatedUSBDeviceFrame::CreateSkylander(uint8 slot)
{
	CreateSkylanderDialog dlg(this, slot);
	dlg.ShowModal();
	if (dlg.GetReturnCode() == 1)
		LoadSkylanderPath(slot, dlg.GetFilePath());
}

void EmulatedUSBDeviceFrame::ClearSkylander(uint8 slot)
{
	if (!m_skySlots[slot])
		return;
	nsyshid::g_skyportal.RemoveSkylander(std::get<0>(*m_skySlots[slot]));
	m_skySlots[slot].reset();
	RefreshSkylanderNames();
}

void EmulatedUSBDeviceFrame::RefreshSkylanderNames()
{
	for (size_t i = 0; i < m_skylanderSlots.size(); i++)
	{
		std::string name = "None";
		if (m_skySlots[i])
		{
			const auto& [portalSlot, id, variant] = *m_skySlots[i];
			name = nsyshid::g_skyportal.FindSkylander(id, variant);
		}
		m_skylanderSlots[i]->ChangeValue(name);
	}
}

CreateSkylanderDialog::CreateSkylanderDialog(wxWindow* parent, uint8 /*slot*/)
	: wxDialog(parent, wxID_ANY, _("Skylander Figure Creator"), wxDefaultPosition, wxSize(500, 150))
{
	// list entries carry (id << 16) | variant
	std::vector<std::pair<wxString, uintptr_t>> entries;
	for (const auto& [key, name] : nsyshid::g_skyportal.GetListSkylanders())
		entries.emplace_back(name, (uintptr_t)((uint32(key.first) << 16) | uint32(key.second)));
	auto* pickerRow = new wxBoxSizer(wxHORIZONTAL);
	wxComboBox* picker = AddFigurePicker(this, pickerRow, entries);

	auto* numbers = new wxBoxSizer(wxHORIZONTAL);
	wxTextCtrl* idField = AddNumberField(this, numbers, "ID:");
	wxTextCtrl* variantField = AddNumberField(this, numbers, "Variant:");

	picker->Bind(wxEVT_COMBOBOX, [picker, idField, variantField](wxCommandEvent&) {
		if (const auto v = PickedFigure(picker))
		{
			idField->SetValue(wxString::Format(wxT("%i"), (int)(uint16)(*v >> 16)));
			variantField->SetValue(wxString::Format(wxT("%i"), (int)(uint16)(*v & 0xFFFF)));
		}
	});

	FinishCreatorLayout(this, pickerRow, numbers, [this, idField, variantField] {
		long id, variant;
		if (!idField->GetValue().ToLong(&id) || id > 0xFFFF)
		{
			ShowMessage(this, "Error Converting ID!", "ID Entered is Invalid");
			return;
		}
		if (!variantField->GetValue().ToLong(&variant) || variant > 0xFFFF)
		{
			ShowMessage(this, "Error Converting Variant!", "Variant Entered is Invalid");
			return;
		}
		const uint16 skyId = id & 0xFFFF, skyVar = variant & 0xFFFF;
		wxFileDialog saveAs(this, _("Create Skylander file"), "", nsyshid::g_skyportal.FindSkylander(skyId, skyVar) + ".sky", "SKY files (*.sky)|*.sky", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
		if (saveAs.ShowModal() == wxID_CANCEL)
			return;
		m_filePath = saveAs.GetPath();
		if (!nsyshid::g_skyportal.CreateSkylander(_utf8ToPath(m_filePath.utf8_string()), skyId, skyVar))
		{
			ShowMessage(this, "Failed to create file");
			EndModal(0);
			return;
		}
		EndModal(1);
	});
}

wxString CreateSkylanderDialog::GetFilePath() const
{
	return m_filePath;
}

// ---------------------------------------------------------------------------
// Infinity

void EmulatedUSBDeviceFrame::LoadFigure(uint8 slot)
{
	wxFileDialog pick(this, _("Open Infinity Figure dump"), "", "", "BIN files (*.bin)|*.bin", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (pick.ShowModal() == wxID_OK && !pick.GetPath().empty())
		LoadFigurePath(slot, pick.GetPath());
}

void EmulatedUSBDeviceFrame::LoadFigurePath(uint8 slot, wxString path)
{
	std::unique_ptr<FileStream> file = OpenDump(this, path, "File Open Error");
	if (!file)
		return;
	std::array<uint8, nsyshid::INF_FIGURE_SIZE> dump;
	if (!ReadDump(this, *file, dump, "Failed to read file! File was too small"))
		return;
	ClearFigure(slot);
	const uint32 number = nsyshid::g_infinitybase.LoadFigure(dump, std::move(file), slot);
	m_infinitySlots[slot]->ChangeValue(nsyshid::g_infinitybase.FindFigure(number).second);
}

void EmulatedUSBDeviceFrame::CreateFigure(uint8 slot)
{
	cemuLog_log(LogType::Force, "Create Figure: {}", slot);
	CreateInfinityFigureDialog dlg(this, slot);
	dlg.ShowModal();
	if (dlg.GetReturnCode() == 1)
		LoadFigurePath(slot, dlg.GetFilePath());
}

void EmulatedUSBDeviceFrame::ClearFigure(uint8 slot)
{
	m_infinitySlots[slot]->ChangeValue("None");
	nsyshid::g_infinitybase.RemoveFigure(slot);
}

CreateInfinityFigureDialog::CreateInfinityFigureDialog(wxWindow* parent, uint8 slot)
	: wxDialog(parent, wxID_ANY, _("Infinity Figure Creator"), wxDefaultPosition, wxSize(500, 150))
{
	std::vector<std::pair<wxString, uintptr_t>> entries;
	for (const auto& [figure, info] : nsyshid::g_infinitybase.GetFigureList())
	{
		if (InfinitySlotAccepts(slot, figure))
			entries.emplace_back(info.second, (uintptr_t)figure);
	}
	auto* pickerRow = new wxBoxSizer(wxHORIZONTAL);
	wxComboBox* picker = AddFigurePicker(this, pickerRow, entries);

	auto* numbers = new wxBoxSizer(wxHORIZONTAL);
	wxTextCtrl* numberField = AddNumberField(this, numbers, "Figure Number:");

	picker->Bind(wxEVT_COMBOBOX, [picker, numberField](wxCommandEvent&) {
		if (const auto v = PickedFigure(picker))
			numberField->SetValue(wxString::Format(wxT("%i"), (int)(uint32)*v));
	});

	FinishCreatorLayout(this, pickerRow, numbers, [this, numberField] {
		long number;
		if (!numberField->GetValue().ToLong(&number))
		{
			ShowMessage(this, "Error Converting Figure Number!", "Number Entered is Invalid");
			EndModal(0);
			return;
		}
		const uint32 figNum = number & 0xFFFFFFFF;
		const auto figure = nsyshid::g_infinitybase.FindFigure(figNum);
		wxFileDialog saveAs(this, _("Create Infinity Figure file"), "", figure.second + ".bin", "BIN files (*.bin)|*.bin", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
		if (saveAs.ShowModal() == wxID_CANCEL)
		{
			EndModal(0);
			return;
		}
		m_filePath = saveAs.GetPath();
		nsyshid::g_infinitybase.CreateFigure(_utf8ToPath(m_filePath.utf8_string()), figNum, figure.first);
		EndModal(1);
	});
}

wxString CreateInfinityFigureDialog::GetFilePath() const
{
	return m_filePath;
}

// ---------------------------------------------------------------------------
// Dimensions

void EmulatedUSBDeviceFrame::LoadMinifig(uint8 pad, uint8 index)
{
	wxFileDialog pick(this, _("Load Dimensions Figure"), "", "", "Dimensions files (*.bin)|*.bin", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (pick.ShowModal() == wxID_OK && !pick.GetPath().empty())
		LoadMinifigPath(pick.GetPath(), pad, index);
}

void EmulatedUSBDeviceFrame::LoadMinifigPath(wxString path_name, uint8 pad, uint8 index)
{
	std::unique_ptr<FileStream> file = OpenDump(this, path_name, "Failed to open minifig file");
	if (!file)
		return;
	nsyshid::DimensionsUSB::TagData dump;
	if (!ReadDump(this, *file, dump, "Failed to read minifig file data"))
		return;
	ClearMinifig(pad, index);
	const uint32 id = nsyshid::g_dimensionstoypad.LoadFigure(dump, std::move(file), pad, index);
	m_dimensionSlots[index]->ChangeValue(nsyshid::g_dimensionstoypad.FindFigure(id));
	m_dimSlots[index] = id;
}

void EmulatedUSBDeviceFrame::ClearMinifig(uint8 pad, uint8 index)
{
	nsyshid::g_dimensionstoypad.RemoveFigure(pad, index, true);
	m_dimensionSlots[index]->ChangeValue("None");
	m_dimSlots[index].reset();
}

void EmulatedUSBDeviceFrame::CreateMinifig(uint8 pad, uint8 index)
{
	CreateDimensionFigureDialog dlg(this);
	dlg.ShowModal();
	if (dlg.GetReturnCode() == 1)
		LoadMinifigPath(dlg.GetFilePath(), pad, index);
}

void EmulatedUSBDeviceFrame::MoveMinifig(uint8 pad, uint8 index)
{
	if (!m_dimSlots[index])
		return;

	// the figure is lifted while the target is chosen and put back if the move is cancelled
	MoveDimensionFigureDialog dlg(this, index);
	nsyshid::g_dimensionstoypad.TempRemove(index);
	dlg.ShowModal();
	if (dlg.GetReturnCode() != 1)
	{
		nsyshid::g_dimensionstoypad.CancelRemove(index);
		return;
	}
	const uint8 newIndex = dlg.GetNewIndex();
	nsyshid::g_dimensionstoypad.MoveFigure(dlg.GetNewPad(), newIndex, pad, index);
	if (newIndex == index)
		return;
	m_dimSlots[newIndex] = m_dimSlots[index];
	m_dimensionSlots[newIndex]->ChangeValue(m_dimensionSlots[index]->GetValue());
	m_dimSlots[index].reset();
	m_dimensionSlots[index]->ChangeValue("None");
}

CreateDimensionFigureDialog::CreateDimensionFigureDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Dimensions Figure Creator"), wxDefaultPosition, wxSize(500, 200))
{
	std::vector<std::pair<wxString, uintptr_t>> entries;
	for (const auto& [figure, name] : nsyshid::g_dimensionstoypad.GetListMinifigs())
		entries.emplace_back(name, (uintptr_t)figure);
	auto* pickerRow = new wxBoxSizer(wxHORIZONTAL);
	wxComboBox* picker = AddFigurePicker(this, pickerRow, entries);

	auto* numbers = new wxBoxSizer(wxHORIZONTAL);
	wxTextCtrl* numberField = AddNumberField(this, numbers, "Figure Number:");

	picker->Bind(wxEVT_COMBOBOX, [picker, numberField](wxCommandEvent&) {
		if (const auto v = PickedFigure(picker))
			numberField->SetValue(wxString::Format(wxT("%i"), (int)(uint16)(*v & 0xFFFF)));
	});

	FinishCreatorLayout(this, pickerRow, numbers, [this, numberField] {
		long number;
		if (!numberField->GetValue().ToLong(&number) || number > 0xFFFF)
		{
			ShowMessage(this, "Error Converting Figure Number!", "Number Entered is Invalid");
			EndModal(0);
			return;
		}
		const uint16 figNum = number & 0xFFFF;
		wxFileDialog saveAs(this, _("Create Dimensions Figure file"), "", nsyshid::g_dimensionstoypad.FindFigure(figNum) + ".bin", "BIN files (*.bin)|*.bin", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
		if (saveAs.ShowModal() == wxID_CANCEL)
		{
			EndModal(0);
			return;
		}
		m_filePath = saveAs.GetPath();
		nsyshid::g_dimensionstoypad.CreateFigure(_utf8ToPath(m_filePath.utf8_string()), figNum);
		EndModal(1);
	});
}

wxString CreateDimensionFigureDialog::GetFilePath() const
{
	return m_filePath;
}

MoveDimensionFigureDialog::MoveDimensionFigureDialog(EmulatedUSBDeviceFrame* parent, uint8 currentIndex)
	: wxDialog(parent, wxID_ANY, _("Dimensions Figure Mover"), wxDefaultPosition, wxSize(700, 300))
{
	const std::array<std::optional<uint32>, 7> ids = parent->GetCurrentMinifigs();
	auto* grid = new wxGridSizer(2, 5, 10, 10);
	auto gap = [this, grid] { grid->Add(new wxStaticText(this, wxID_ANY, ""), 1, wxALL, 5); };
	auto spot = [&](uint8 pad, uint8 index) { grid->Add(AddMinifigSlot(pad, index, currentIndex, ids[index]), 1, wxALL, 5); };
	// same arrangement as the toypad page
	spot(2, 0);
	gap();
	spot(1, 1);
	gap();
	spot(3, 2);
	spot(2, 3);
	spot(2, 4);
	gap();
	spot(3, 5);
	spot(3, 6);
	SetSizer(grid);
	Centre(wxBOTH);
}

wxBoxSizer* MoveDimensionFigureDialog::AddMinifigSlot(uint8 pad, uint8 index, uint8 oldIndex, std::optional<uint32> currentId)
{
	auto* column = new wxBoxSizer(wxVERTICAL);
	auto* name = new wxStaticText(this, wxID_ANY, currentId ? wxString(nsyshid::g_dimensionstoypad.FindFigure(*currentId)) : wxString("None"));
	auto* target = new wxButton(this, wxID_ANY, _("Move Here"));
	if (index == oldIndex)
		target->SetLabelText("Pick up and Place");
	target->Bind(wxEVT_BUTTON, [this, pad, index](wxCommandEvent&) {
		m_newPad = pad;
		m_newIndex = index;
		EndModal(1);
	});
	column->Add(name, 1, wxALL, 5);
	column->Add(target, 1, wxALL, 5);
	return column;
}

uint8 MoveDimensionFigureDialog::GetNewPad() const
{
	return m_newPad;
}

uint8 MoveDimensionFigureDialog::GetNewIndex() const
{
	return m_newIndex;
}
