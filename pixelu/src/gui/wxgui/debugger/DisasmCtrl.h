// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "wxgui/components/TextList.h"

// Sent to the parent after the goto dialog moved the view; GetExtraLong() is the target address.
wxDECLARE_EVENT(wxEVT_DISASMCTRL_NOTIFY_GOTO_ADDRESS, wxCommandEvent);

// Disassembly view of one MMU region, four bytes per line plus a label line
// before every address that has a symbol. Columns: address | module offset |
// instruction | comment. Double-click toggles a breakpoint (address columns),
// patches the instruction or edits the comment. F9 toggles a breakpoint,
// F5/F10/F11 run/step, Ctrl+G opens the goto dialog.
class DisasmCtrl : public TextList
{
public:
	DisasmCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size, long style);

	void Init();
	wxSize DoGetBestSize() const override;

	void OnUpdateView();

	uint32 GetViewBaseAddress() const;
	// address shown on the line at view-relative pixel row posY, if any
	std::optional<MPTR> LinePixelPosToAddress(sint32 posY);

	uint32 AddressToScrollPos(uint32 offset) const;
	void CenterOffset(uint32 offset);
	void GoToAddressDialog();

protected:
	void OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position) override;
	void OnKeyPressed(sint32 key_code, const wxPoint& position) override;
	void OnMouseDClick(const wxPoint& position, uint32 line) override;
	void OnContextMenu(const wxPoint& position, uint32 line) override;
	bool OnShowTooltip(const wxPoint& position, uint32 line) override;
	void ScrollWindow(int dx, int dy, const wxRect* prect) override;

	void SelectCodeRegion(uint32 newAddress);

private:
	void DrawInstructionLine(wxDC& dc, const wxPoint& linePos, MPTR address, struct RPLModule* viewModule);
	void DrawSymbolLine(wxDC& dc, const wxPoint& linePos, struct RPLStoredSymbol* symbol);
	void OnMenuCommand(wxCommandEvent& event);
	void NotifyBreakpointsChanged();
	void JumpTo(uint32 address);

	sint32 m_highlightLine = -1;  // line last centred on, repainted when it moves
	uint32 m_gotoTarget = 0;      // address from the goto dialog, drawn highlighted
	uint32 m_menuAddress = 0;     // address the context menu was opened on
	uint32 m_regionBegin = 0;
	uint32 m_regionEnd = 0;
	std::vector<std::optional<MPTR>> m_visibleAddresses; // per view line, filled while drawing
};
