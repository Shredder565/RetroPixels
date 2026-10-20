// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "interface/WindowSystem.h"
#include "wxgui/wxgui.h"
#include "wxgui/PadViewFrame.h"

#include <wx/display.h>

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "input/InputManager.h"
#ifdef ENABLE_OPENGL
#include "wxgui/canvas/OpenGLCanvas.h"
#endif
#ifdef ENABLE_VULKAN
#include "wxgui/canvas/VulkanCanvas.h"
#endif
#include "wxgui/MainWindow.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxHelper.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

extern WindowSystem::WindowInfo g_window_info;

namespace
{
	constexpr int kMinWidth = 320;
	constexpr int kMinHeight = 180;
	const wxSize kDefaultSize(854, 480);
}

PadViewFrame::PadViewFrame(wxFrame* parent)
	: wxFrame(nullptr, wxID_ANY, _("GamePad View"), wxDefaultPosition, wxDefaultSize,
			  wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxCLOSE_BOX | wxWANTS_CHARS)
{
	g_window_info.window_pad = initHandleContextFromWxWidgetsWindow(this);

	SetIcon(wxICON(M_WND_ICON128));
	EnableTouchEvents(wxTOUCH_PAN_GESTURES);
	SetMinClientSize({kMinWidth, kMinHeight});

	// where and how big it was last time (a remembered size below the minimum is ignored)
	SetPosition({g_window_info.restored_pad_x, g_window_info.restored_pad_y});
	const int w = g_window_info.restored_pad_width;
	const int h = g_window_info.restored_pad_height;
	SetClientSize(w >= kMinWidth && h >= kMinHeight ? wxSize(w, h) : kDefaultSize);
	if (g_window_info.pad_maximized)
		Maximize();

	Bind(wxEVT_SIZE, &PadViewFrame::OnSizeEvent, this);
	Bind(wxEVT_DPI_CHANGED, &PadViewFrame::OnDPIChangedEvent, this);
	Bind(wxEVT_MOVE, &PadViewFrame::OnMoveEvent, this);
	Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);
	Bind(wxEVT_SET_WINDOW_TITLE, &PadViewFrame::OnSetWindowTitle, this);

	g_window_info.pad_open = true;
}

PadViewFrame::~PadViewFrame()
{
	g_window_info.pad_open = false;
}

bool PadViewFrame::Initialize()
{
	const wxSize area = GetClientSize();
	g_window_info.pad_width = area.GetWidth();
	g_window_info.pad_height = area.GetHeight();
	g_window_info.phys_pad_width = ToPhys(area.GetWidth());
	g_window_info.phys_pad_height = ToPhys(area.GetHeight());
	return true;
}

void PadViewFrame::InitializeRenderCanvas()
{
	const auto api = ActiveSettings::GetGraphicsAPI();
#ifdef ENABLE_VULKAN
	if (api == kVulkan)
		m_render_canvas = new VulkanCanvas(this, kDefaultSize, false);
#endif
#ifdef ENABLE_OPENGL
	if (api == kOpenGL)
		m_render_canvas = GLCanvas_Create(this, kDefaultSize, false);
#endif
	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(m_render_canvas, 1, wxEXPAND);
	cemu_assert(m_render_canvas != nullptr);
	SetSizer(layout);
	Layout();

	m_render_canvas->Bind(wxEVT_KEY_UP, &PadViewFrame::OnKeyUp, this);
	m_render_canvas->Bind(wxEVT_CHAR, &PadViewFrame::OnChar, this);
	m_render_canvas->Bind(wxEVT_MOTION, &PadViewFrame::OnMouseMove, this);
	for (const auto& type : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP})
		m_render_canvas->Bind(type, &PadViewFrame::OnMouseLeft, this);
	for (const auto& type : {wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP})
		m_render_canvas->Bind(type, &PadViewFrame::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_GESTURE_PAN, &PadViewFrame::OnGesturePan, this);

	m_render_canvas->SetFocus();
	SendSizeEvent();
}

void PadViewFrame::DestroyCanvas()
{
	if (!m_render_canvas)
		return;
	m_render_canvas->Destroy();
	m_render_canvas = nullptr;
}

void PadViewFrame::OnSizeEvent(wxSizeEvent& event)
{
	const bool fullscreen = IsFullScreen();
	if (!IsMaximized() && !fullscreen)
	{
		const wxSize outer = GetSize();
		g_window_info.restored_pad_width = outer.x;
		g_window_info.restored_pad_height = outer.y;
	}
	g_window_info.pad_maximized = IsMaximized() && !fullscreen;

	const wxSize area = GetClientSize();
	g_window_info.pad_width = area.GetWidth();
	g_window_info.pad_height = area.GetHeight();
	g_window_info.phys_pad_width = ToPhys(area.GetWidth());
	g_window_info.phys_pad_height = ToPhys(area.GetHeight());
	g_window_info.pad_dpi_scale = GetDPIScaleFactor();

	event.Skip();
}

void PadViewFrame::OnDPIChangedEvent(wxDPIChangedEvent& event)
{
	event.Skip();
	const wxSize area = GetClientSize();
	g_window_info.pad_width = area.GetWidth();
	g_window_info.pad_height = area.GetHeight();
	g_window_info.phys_pad_width = ToPhys(area.GetWidth());
	g_window_info.phys_pad_height = ToPhys(area.GetHeight());
	g_window_info.pad_dpi_scale = GetDPIScaleFactor();
}

void PadViewFrame::OnMoveEvent(wxMoveEvent&)
{
	if (IsMaximized() || IsFullScreen())
		return;
	const wxPoint where = GetPosition();
	g_window_info.restored_pad_x = where.x;
	g_window_info.restored_pad_y = where.y;
}

void PadViewFrame::OnKeyUp(wxKeyEvent& event)
{
	event.Skip();
	if (swkbd_hasKeyboardInputHook())
		return;

	// Esc leaves fullscreen, F11 or Alt+Enter toggles it
	const int key = event.GetKeyCode();
	if (key == WXK_ESCAPE)
		ShowFullScreen(false);
	else if (key == WXK_F11 || (key == WXK_RETURN && event.AltDown()))
		ShowFullScreen(!IsFullScreen());
}

void PadViewFrame::OnChar(wxKeyEvent& event)
{
	if (swkbd_hasKeyboardInputHook())
		swkbd_keyInput(event.GetUnicodeKey());
	event.Skip();
}

void PadViewFrame::OnGesturePan(wxPanGestureEvent& event)
{
	auto& input = InputManager::instance();
	std::scoped_lock guard(input.m_pad_touch.m_mutex);
	const wxPoint phys = ToPhys(event.GetPosition());
	const bool touching = event.IsGestureStart() || !event.IsGestureEnd();
	input.m_pad_touch.position = {phys.x, phys.y};
	input.m_pad_touch.left_down = touching;
	if (touching)
		input.m_pad_touch.left_down_toggle = true;
}

void PadViewFrame::OnMouseMove(wxMouseEvent& event)
{
	auto& input = InputManager::instance();
	{
		std::scoped_lock guard(input.m_pad_mouse.m_mutex);
		const wxPoint phys = ToPhys(event.GetPosition());
		input.m_pad_mouse.position = {phys.x, phys.y};
	}
	event.Skip();
}

void PadViewFrame::OnMouseLeft(wxMouseEvent& event)
{
	auto& input = InputManager::instance();
	std::scoped_lock guard(input.m_pad_mouse.m_mutex);
	const bool down = event.ButtonDown(wxMOUSE_BTN_LEFT);
	const wxPoint phys = ToPhys(event.GetPosition());
	input.m_pad_mouse.left_down = down;
	input.m_pad_mouse.position = {phys.x, phys.y};
	if (down)
		input.m_pad_mouse.left_down_toggle = true;
}

void PadViewFrame::OnMouseRight(wxMouseEvent& event)
{
	auto& input = InputManager::instance();
	std::scoped_lock guard(input.m_pad_mouse.m_mutex);
	const wxPoint phys = ToPhys(event.GetPosition());
	// a right-button event never has the left button going down, so the held state stays off here
	// and only the toggle reports the click (unchanged behaviour)
	input.m_pad_mouse.right_down = event.ButtonDown(wxMOUSE_BTN_LEFT);
	input.m_pad_mouse.position = {phys.x, phys.y};
	if (event.ButtonDown(wxMOUSE_BTN_RIGHT))
		input.m_pad_mouse.right_down_toggle = true;
}

void PadViewFrame::OnSetWindowTitle(wxCommandEvent& event)
{
	SetTitle(event.GetString());
}

void PadViewFrame::AsyncSetTitle(std::string_view windowTitle)
{
	auto* request = new wxCommandEvent(wxEVT_SET_WINDOW_TITLE);
	request->SetString(wxString::FromUTF8(windowTitle));
	QueueEvent(request);
}
