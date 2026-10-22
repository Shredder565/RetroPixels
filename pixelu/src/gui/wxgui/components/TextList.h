// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wx.h>

#include <unordered_map>
#include <sstream>

// Base for owner-drawn, line based views (log, disassembly, memory dump).
// The control scrolls vertically in whole lines of the GUI font's height and
// has "element count" lines in total. Subclasses paint the lines in OnDraw()
// and get mouse/keyboard hooks with positions in unscrolled coordinates.
//
// OnDraw(dc, start, count, start_position): `start` is the first line that
// needs painting, `count` the number of lines (one more may be partly visible),
// `start_position` the device position of line `start` (the dc is not scrolled).
class TextList : public wxControl, public wxScrollHelper
{
public:
	virtual ~TextList();
	TextList(wxWindow* parent, wxWindowID id, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = 0);

	void RefreshControl(const wxRect* update_region = nullptr);
	void RefreshLine(uint32 line);

	wxSize DoGetVirtualSize() const override;
	void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;
	void SetScrollPos(int orient, int pos, bool refresh) override;
	void DrawLineBackground(wxDC& dc, uint32 line, const wxColour& colour, uint32 lines = 1) const;
	void DrawLineBackground(wxDC& dc, const wxPoint& position, const wxColour& colour, uint32 lines = 1) const;

	// returns false when the count did not change
	bool SetElementCount(size_t element_count);
	uint32 GetElementCount() const;
	bool IsKeyDown(sint32 keycode);

protected:
	// draws text and advances position.x by the drawn width
	void WriteText(wxDC& dc, const wxString& text, wxPoint& position) const;
	void WriteText(wxDC& dc, const wxString& text, wxPoint& position, const wxColour& color) const;
	// moves position one line down (and back to start_position's x when given)
	void NextLine(wxPoint& position, const wxPoint* start_position = nullptr) const;

	virtual void OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position) {}
	virtual void OnMouseMove(const wxPoint& position, uint32 line) {}
	virtual void OnKeyPressed(sint32 key_code, const wxPoint& position) {}
	virtual void OnMouseDown();
	virtual void OnMouseUp();
	virtual void OnMouseDClick(const wxPoint& position, uint32 line) {}
	virtual void OnContextMenu(const wxPoint& position, uint32 line) {}
	virtual bool OnShowTooltip(const wxPoint& position, uint32 line);
	void OnEraseBackground(wxEraseEvent&) {}

	sint32 m_line_height;
	sint32 m_char_width;
	size_t m_elements_visible = 0; // lines that fit into the client height (rounded up)
	size_t m_element_count = 0;
	bool m_scrolled_to_end = true; // the last paint reached the final line

	bool m_mouse_down = false;
	wxRect m_selection; // press position .. current drag position (unscrolled)
	wxPanel* m_tooltip_window; // hover panel subclasses may fill; shown when OnShowTooltip() returns true

private:
	wxPoint ToContent(const wxPoint& client) const;
	uint32 LineAt(const wxPoint& content) const;
	void BeginSelectionAt(const wxPoint& content);

	void HandlePaint(wxPaintEvent& event);
	void HandleMotion(wxMouseEvent& event);
	void HandleKeyDown(wxKeyEvent& event);
	void HandleKeyUp(wxKeyEvent& event);
	void HandleLeftDown(wxMouseEvent& event);
	void HandleLeftUp(wxMouseEvent& event);
	void HandleLeftDClick(wxMouseEvent& event);
	void HandleContextMenu(wxContextMenuEvent& event);
	void HandleHoverTimer(wxTimerEvent& event);

	std::unordered_map<sint32, bool> m_heldKeys;
	wxFont m_textFont;
	wxTimer m_hoverTimer;
	wxPoint m_lastMouse; // unscrolled
};
