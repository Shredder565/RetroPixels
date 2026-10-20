// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "TextList.h"

#include <wx/setup.h>
#include <wx/tooltip.h>

TextList::TextList(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
	: wxControl(parent, id, pos, size, style), wxScrollHelper(this), m_hoverTimer(this)
{
	m_textFont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	wxWindowBase::SetBackgroundStyle(wxBG_STYLE_PAINT);

	// line metrics come from the GUI font
	{
		wxInfoDC measure(this);
		DoPrepareReadOnlyDC(measure);
		measure.SetFont(m_textFont);
		m_line_height = measure.GetCharHeight();
		m_char_width = measure.GetCharWidth();
	}
	m_yScrollPixelsPerLine = m_line_height;
	ShowScrollbars(wxSHOW_SB_DEFAULT, wxSHOW_SB_DEFAULT);

	m_tooltip = new wxToolTip(wxEmptyString);

	Bind(wxEVT_PAINT, &TextList::HandlePaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, &TextList::OnEraseBackground, this);
	Bind(wxEVT_MOTION, &TextList::HandleMotion, this);
	Bind(wxEVT_KEY_DOWN, &TextList::HandleKeyDown, this);
	Bind(wxEVT_KEY_UP, &TextList::HandleKeyUp, this);
	Bind(wxEVT_LEFT_DOWN, &TextList::HandleLeftDown, this);
	Bind(wxEVT_LEFT_UP, &TextList::HandleLeftUp, this);
	Bind(wxEVT_LEFT_DCLICK, &TextList::HandleLeftDClick, this);
	Bind(wxEVT_CONTEXT_MENU, &TextList::HandleContextMenu, this);
	Bind(wxEVT_TIMER, &TextList::HandleHoverTimer, this);

	m_tooltip_window = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
	m_tooltip_window->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOBK));
	m_tooltip_window->Hide();
}

TextList::~TextList()
{
	m_hoverTimer.Stop();
}

wxPoint TextList::ToContent(const wxPoint& client) const
{
	wxPoint p;
	CalcUnscrolledPosition(client.x, client.y, &p.x, &p.y);
	return p;
}

uint32 TextList::LineAt(const wxPoint& content) const
{
	return (uint32)(content.y / m_line_height);
}

void TextList::BeginSelectionAt(const wxPoint& content)
{
	m_selection.SetPosition(content);
	m_selection.SetBottomRight(content);
}

void TextList::RefreshControl(const wxRect* update_region)
{
	const wxRect everything = GetClientRect();
	Refresh(true, update_region ? update_region : &everything);
}

void TextList::RefreshLine(uint32 line)
{
	wxRect area = GetClientRect();
	area.height = m_line_height;
	CalcScrolledPosition(0, (sint32)line * m_line_height, nullptr, &area.y);
	Refresh(true, &area);
}

wxSize TextList::DoGetVirtualSize() const
{
	return wxSize(wxDefaultCoord, (int)m_element_count * m_line_height);
}

void TextList::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
	wxControl::DoSetSize(x, y, width, height, sizeFlags);
	m_elements_visible = (height + m_line_height - 1) / m_line_height;
	Refresh();
}

void TextList::SetScrollPos(int orient, int pos, bool refresh)
{
	wxControl::SetScrollPos(orient, pos, refresh);
}

void TextList::DrawLineBackground(wxDC& dc, uint32 line, const wxColour& colour, uint32 lines) const
{
	DrawLineBackground(dc, wxPoint(GetPosition().x, line * m_line_height), colour, lines);
}

void TextList::DrawLineBackground(wxDC& dc, const wxPoint& position, const wxColour& colour, uint32 lines) const
{
	dc.SetBrush(colour);
	dc.DrawRectangle(wxRect(position.x, position.y, GetSize().x, m_line_height * lines));
}

bool TextList::SetElementCount(size_t element_count)
{
	if (element_count == m_element_count)
		return false;
	m_element_count = std::min<size_t>(element_count, 0x7FFFFFFF);
	AdjustScrollbars();
	return true;
}

uint32 TextList::GetElementCount() const
{
	return (uint32)m_element_count;
}

bool TextList::IsKeyDown(sint32 keycode)
{
	const auto it = m_heldKeys.find(keycode);
	return it != m_heldKeys.end() && it->second;
}

void TextList::WriteText(wxDC& dc, const wxString& text, wxPoint& position) const
{
	// the drawn extent comes from the dc's bounding box
	dc.ResetBoundingBox();
	dc.DrawText(text, position);
	position.x += dc.MaxX() - dc.MinX();
}

void TextList::NextLine(wxPoint& position, const wxPoint* start_position) const
{
	if (start_position)
		position.x = start_position->x; // carriage return
	position.y += m_line_height;
}

void TextList::WriteText(wxDC& dc, const wxString& text, wxPoint& position, const wxColour& color) const
{
	dc.SetTextForeground(color);
	WriteText(dc, text, position);
}

void TextList::OnMouseDown()
{
}

void TextList::OnMouseUp()
{
}

bool TextList::OnShowTooltip(const wxPoint& /*position*/, uint32 /*line*/)
{
	return false;
}

void TextList::HandleMotion(wxMouseEvent& event)
{
	// the hover panel is offered once the mouse rests for 250 ms
	m_hoverTimer.Stop();
	m_hoverTimer.StartOnce(250);

	const wxPoint at = ToContent(event.GetPosition());
	m_lastMouse = at;
	if (m_mouse_down)
		m_selection.SetBottomRight(at);
	OnMouseMove(at, LineAt(at));
}

void TextList::HandleKeyDown(wxKeyEvent& event)
{
	// OnKeyPressed fires once per press, not for auto-repeat
	const sint32 key = event.GetKeyCode();
	bool& held = m_heldKeys[key];
	if (!held)
	{
		held = true;
		OnKeyPressed(key, event.GetPosition());
	}
	event.Skip();
}

void TextList::HandleKeyUp(wxKeyEvent& event)
{
	m_heldKeys[event.GetKeyCode()] = false;
}

void TextList::HandleLeftDown(wxMouseEvent& event)
{
	m_mouse_down = true;
	BeginSelectionAt(ToContent(event.GetPosition()));
	OnMouseDown();
	event.Skip();
}

void TextList::HandleLeftUp(wxMouseEvent& event)
{
	m_mouse_down = false;
	OnMouseUp();
	event.Skip();
}

void TextList::HandleLeftDClick(wxMouseEvent& event)
{
	const wxPoint at = ToContent(event.GetPosition());
	BeginSelectionAt(at);
	OnMouseDClick(at, LineAt(at));
}

void TextList::HandleContextMenu(wxContextMenuEvent& event)
{
	const wxPoint screen = event.GetPosition();
	if (screen == wxDefaultPosition)
		return; // keyboard-invoked menu: no position to act on
	const wxPoint at = ToContent(ScreenToClient(screen));
	BeginSelectionAt(at);
	OnContextMenu(at, LineAt(at));
}

void TextList::HandleHoverTimer(wxTimerEvent& /*event*/)
{
	m_tooltip_window->Hide();
	const wxRect screenArea(GetScreenPosition(), GetSize());
	const wxPoint cursor = wxGetMousePosition();
	if (cursor.x < screenArea.x || cursor.y < screenArea.y || cursor.x > screenArea.GetRight() + 1 || cursor.y > screenArea.GetBottom() + 1)
		return;
	if (!OnShowTooltip(m_lastMouse, LineAt(m_lastMouse)))
		return;
	m_tooltip_window->SetPosition(m_lastMouse + wxPoint(15, 15));
	m_tooltip_window->SendSizeEvent();
	m_tooltip_window->Show();
}

void TextList::HandlePaint(wxPaintEvent& /*event*/)
{
	wxAutoBufferedPaintDC dc(m_targetWindow);
	const wxRect dirty = GetUpdateRegion().GetBox();

	dc.SetFont(m_textFont);
	dc.SetBrush(GetBackgroundColour());
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(dirty);

	// whole lines covering the dirty area; drawing happens in device coordinates
	const uint32 lineCount = (uint32)std::ceil((float)dirty.GetHeight() / m_line_height);
	wxPoint origin = GetPosition();
	origin.y = (dirty.y / m_line_height) * m_line_height;

	sint32 firstLine = 0;
	CalcUnscrolledPosition(dirty.x, dirty.y, nullptr, &firstLine);
	firstLine /= m_line_height;
	m_scrolled_to_end = (firstLine + lineCount) >= m_element_count;

	OnDraw(dc, firstLine, lineCount, origin);
}
