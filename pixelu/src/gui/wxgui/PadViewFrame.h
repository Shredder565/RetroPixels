// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/frame.h>

// window messages kept for code that still names them
#define WM_CREATE_PAD	(WM_USER+1)
#define WM_DESTROY_PAD	(WM_USER+2)

wxDECLARE_EVENT(EVT_PAD_CLOSE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SET_WINDOW_TITLE, wxCommandEvent);

// The separate window that shows the GamePad screen ("Separate GamePad view").
// Mouse and touch input on it is GamePad touch input.
class PadViewFrame : public wxFrame
{
public:
	PadViewFrame(wxFrame* parent);
	~PadViewFrame();

	// publishes the client size to the window system; always true
	bool Initialize();
	// creates the GamePad canvas for the active graphics API (once a game runs)
	void InitializeRenderCanvas();
	void DestroyCanvas();

	void OnKeyUp(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);

	// sets the title from any thread
	void AsyncSetTitle(std::string_view windowTitle);

private:
	void OnMouseMove(wxMouseEvent& event);
	void OnMouseLeft(wxMouseEvent& event);
	void OnMouseRight(wxMouseEvent& event);
	void OnSizeEvent(wxSizeEvent& event);
	void OnDPIChangedEvent(wxDPIChangedEvent& event);
	void OnMoveEvent(wxMoveEvent& event);
	void OnGesturePan(wxPanGestureEvent& event);
	void OnSetWindowTitle(wxCommandEvent& event);

	wxWindow* m_render_canvas = nullptr;
};
