// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/canvas/VulkanCanvas.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
#include "wxgui/helpers/wxWayland.h"
#endif

#include <wx/msgdlg.h>
#include "wxgui/RPEmbed.h"
#include <helpers/wxHelpers.h>

namespace
{
	// the renderer cannot run without its surface: tell the user and quit
	[[noreturn]] void FailVulkanStart(wxWindow* canvas, const std::exception& error)
	{
		cemuLog_log(LogType::Force, "Error when initializing Vulkan renderer: {}", error.what());
		const wxString text = formatWxString(_("Error when initializing Vulkan renderer:\n{}"), error.what());
		if (RPEmbed::Enabled())
		{
			// PixelU: shown over RetroPixels' window
			RPEmbed::ShowMessage(text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		}
		else
		{
			wxMessageDialog box(canvas, text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
			box.ShowModal();
		}
		_exit(0);
	}
}

VulkanCanvas::VulkanCanvas(wxWindow* parent, const wxSize& size, bool is_main_window)
	: IRenderCanvas(is_main_window), wxWindow(parent, wxID_ANY, wxDefaultPosition, size, wxNO_FULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
{
	Bind(wxEVT_PAINT, &VulkanCanvas::OnPaint, this);
	Bind(wxEVT_SIZE, &VulkanCanvas::OnResize, this);
#if __WXMSW__
	// the swapchain presents straight into this window, WS_EX_COMPOSITED would get in its way
	MSWDisableComposited();
#endif

	auto& windowInfo = WindowSystem::GetWindowInfo();
	WindowSystem::WindowHandleInfo& handles = is_main_window ? windowInfo.canvas_main : windowInfo.canvas_pad;
	handles = initHandleContextFromWxWidgetsWindow(this);
#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
	if (handles.backend == WindowSystem::WindowHandleInfo::Backend::Wayland)
	{
		m_subsurface = std::make_unique<wxWlSubsurface>(this);
		handles.surface = m_subsurface->getSurface();
	}
#endif

	cemu_assert(g_vulkan_available);

	try
	{
		if (is_main_window)
			g_renderer = std::make_unique<VulkanRenderer>();
		VulkanRenderer::GetInstance()->InitializeSurface({size.x, size.y}, is_main_window);
	}
	catch (const std::exception& ex)
	{
		FailVulkanStart(this, ex);
	}

	wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);
}

VulkanCanvas::~VulkanCanvas()
{
	Unbind(wxEVT_PAINT, &VulkanCanvas::OnPaint, this);
	Unbind(wxEVT_SIZE, &VulkanCanvas::OnResize, this);

	// the GamePad window going away: the renderer must be done with its swapchain first
	if (!m_is_main_window)
	{
		if (auto* renderer = (VulkanRenderer*)g_renderer.get())
			renderer->StopUsingPadAndWait();
	}
}

void VulkanCanvas::OnPaint(wxPaintEvent& event)
{
	// the renderer draws on its own schedule
}

void VulkanCanvas::OnResize(wxSizeEvent& event)
{
	const wxSize clientSize = GetSize();
	if (clientSize.GetWidth() == 0 || clientSize.GetHeight() == 0)
		return;

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
	if (m_subsurface)
	{
		const wxRect onScreen = GetScreenRect();
		m_subsurface->setSize(onScreen.GetX(), onScreen.GetY(), onScreen.GetWidth(), onScreen.GetHeight());
	}
#endif

	RefreshRect(wxRect(clientSize), false);
}
