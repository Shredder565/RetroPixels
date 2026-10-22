// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <tuple>

#include <wx/dialog.h>
#include <wx/frame.h>

#include "Cafe/OS/libs/nsyshid/Infinity.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"

class wxBoxSizer;
class wxCheckBox;
class wxFlexGridSizer;
class wxNotebook;
class wxPanel;
class wxStaticBox;
class wxString;
class wxTextCtrl;

// Settings window for the emulated toy portals: Skylanders portal, Disney
// Infinity base and LEGO Dimensions toypad. Each page enables the device and
// loads, creates or clears the figures placed on it.
class EmulatedUSBDeviceFrame : public wxFrame
{
  public:
	EmulatedUSBDeviceFrame(wxWindow* parent);
	~EmulatedUSBDeviceFrame();
	// figure id on each of the 7 toypad spots
	std::array<std::optional<uint32>, 7> GetCurrentMinifigs();

  private:
	wxPanel* BuildSkylanderPage(wxNotebook* book);
	wxPanel* BuildInfinityPage(wxNotebook* book);
	wxPanel* BuildDimensionsPage(wxNotebook* book);
	wxBoxSizer* BuildSlotRow(wxStaticBox* box, const wxString& caption, wxTextCtrl*& field, int fieldBorder, std::function<void()> load, std::function<void()> create, std::function<void()> clear);
	wxBoxSizer* BuildToypadSpot(wxStaticBox* box, uint8 pad, uint8 index);

	void LoadSkylander(uint8 slot);
	void LoadSkylanderPath(uint8 slot, wxString path);
	void CreateSkylander(uint8 slot);
	void ClearSkylander(uint8 slot);
	void RefreshSkylanderNames();

	void LoadFigure(uint8 slot);
	void LoadFigurePath(uint8 slot, wxString path);
	void CreateFigure(uint8 slot);
	void ClearFigure(uint8 slot);

	void LoadMinifig(uint8 pad, uint8 index);
	void LoadMinifigPath(wxString path_name, uint8 pad, uint8 index);
	void CreateMinifig(uint8 pad, uint8 index);
	void ClearMinifig(uint8 pad, uint8 index);
	void MoveMinifig(uint8 pad, uint8 index);

	wxCheckBox* m_emulatePortal;
	wxCheckBox* m_emulateBase;
	wxCheckBox* m_emulateToypad;
	std::array<wxTextCtrl*, nsyshid::MAX_SKYLANDERS> m_skylanderSlots{};
	std::array<wxTextCtrl*, nsyshid::MAX_FIGURES> m_infinitySlots{};
	std::array<wxTextCtrl*, 7> m_dimensionSlots{};
	// portal slot, id and variant of the skylander shown in each row
	std::array<std::optional<std::tuple<uint8, uint16, uint16>>, nsyshid::MAX_SKYLANDERS> m_skySlots;
	std::array<std::optional<uint32>, 7> m_dimSlots;
};

// asks for a skylander (from the list or by id/variant) and writes a new dump
class CreateSkylanderDialog : public wxDialog
{
  public:
	explicit CreateSkylanderDialog(wxWindow* parent, uint8 slot);
	wxString GetFilePath() const;

  protected:
	wxString m_filePath;
};

// same for an Infinity figure; the list only offers figures that fit the slot
class CreateInfinityFigureDialog : public wxDialog
{
  public:
	explicit CreateInfinityFigureDialog(wxWindow* parent, uint8 slot);
	wxString GetFilePath() const;

  protected:
	wxString m_filePath;
};

// same for a Dimensions minifig
class CreateDimensionFigureDialog : public wxDialog
{
  public:
	explicit CreateDimensionFigureDialog(wxWindow* parent);
	wxString GetFilePath() const;

  protected:
	wxString m_filePath;
};

// picks the toypad spot a minifig moves to
class MoveDimensionFigureDialog : public wxDialog
{
  public:
	explicit MoveDimensionFigureDialog(EmulatedUSBDeviceFrame* parent, uint8 currentIndex);
	uint8 GetNewPad() const;
	uint8 GetNewIndex() const;

  protected:
	uint8 m_newIndex = 0;
	uint8 m_newPad = 0;

  private:
	wxBoxSizer* AddMinifigSlot(uint8 pad, uint8 index, uint8 oldIndex, std::optional<uint32> currentId);
};
