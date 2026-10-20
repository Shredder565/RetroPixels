// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/canvas/MetalCanvas.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"

#include <wx/msgdlg.h>
#include <helpers/wxHelpers.h>

MetalCanvas::MetalCanvas(wxWindow* parent, const wxSize& size, bool is_main_window)
	: IRenderCanvas(is_main_window), wxWindow(parent, wxID_ANY, wxDefaultPosition, size, wxNO_FULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
{
	Bind(wxEVT_PAINT, &MetalCanvas::OnPaint, this);
	Bind(wxEVT_SIZE, &MetalCanvas::OnResize, this);

	auto& windowInfo = WindowSystem::GetWindowInfo();
	WindowSystem::WindowHandleInfo& handles = is_main_window ? windowInfo.canvas_main : windowInfo.canvas_pad;
	handles = initHandleContextFromWxWidgetsWindow(this);

	try
	{
		if (is_main_window)
			g_renderer = std::make_unique<MetalRenderer>();
		MetalRenderer::GetInstance()->InitializeLayer({size.x, size.y}, is_main_window);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Error when initializing Metal renderer: {}", ex.what());
		const wxString text = formatWxString(_("Error when initializing Metal renderer:\n{}"), ex.what());
		wxMessageDialog box(this, text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		box.ShowModal();
		exit(0);
	}

	wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);
}

MetalCanvas::~MetalCanvas()
{
	Unbind(wxEVT_PAINT, &MetalCanvas::OnPaint, this);
	Unbind(wxEVT_SIZE, &MetalCanvas::OnResize, this);

	if (auto* renderer = (MetalRenderer*)g_renderer.get())
		renderer->ShutdownLayer(m_is_main_window);
}

void MetalCanvas::OnPaint(wxPaintEvent& event)
{
	// the renderer draws on its own schedule
}

void MetalCanvas::OnResize(wxSizeEvent& event)
{
	const wxSize clientSize = GetSize();
	if (clientSize.GetWidth() == 0 || clientSize.GetHeight() == 0)
		return;

	RefreshRect(wxRect(clientSize), false);
	MetalRenderer::GetInstance()->ResizeLayer({clientSize.x, clientSize.y}, m_is_main_window);
}
