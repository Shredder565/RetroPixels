// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "wxgui/components/TextList.h"

// Hex/ASCII view of one MMU region, 16 bytes per line. Double-click a byte to
// edit it; the context menu writes bytes, 16/32-bit values, floats or strings.
// Ctrl+G jumps to an address (and switches region when needed).
class DumpCtrl : public TextList
{
public:
	DumpCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size, long style);

	void Init();
	wxSize DoGetBestSize() const override;

protected:
	void GoToAddressDialog();
	void CenterOffset(uint32 offset);
	uint32 LineToOffset(uint32 line);
	uint32 OffsetToLine(uint32 offset);
	// address of the hex byte under position (unscrolled), MPTR_NULL outside the hex columns
	uint32 PositionToAddress(const wxPoint& position, uint32 line);

	void OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position) override;
	void OnMouseDClick(const wxPoint& position, uint32 line) override;
	void OnKeyPressed(sint32 key_code, const wxPoint& position) override;
	void OnContextMenu(const wxPoint& position, uint32 line) override;
	void OnMenuSelected(wxCommandEvent& event);

	template <typename T>
	bool WriteNumericDialog(uint32 address);
	bool WriteString(uint32 address);

private:
	void ShowRegionOf(uint32 address);
	void RefreshRow(uint32 line);

	uint32 m_regionBase = 0;
	uint32 m_regionSize = 0;
	uint32 m_gotoMarker = 0;  // address from the goto dialog, marked in red
	uint32 m_editAddress = 0; // context menu target
	uint32 m_editLine = 0;
};
