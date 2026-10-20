// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/canvas/IRenderCanvas.h"

#include <wx/frame.h>

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include <set>

// A plain native window the Vulkan renderer presents into. The main window's canvas creates the renderer.
class VulkanCanvas : public IRenderCanvas, public wxWindow
{
#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
	std::unique_ptr<class wxWlSubsurface> m_subsurface; // on Wayland the renderer draws into a subsurface
#endif
public:
	VulkanCanvas(wxWindow* parent, const wxSize& size, bool is_main_window);
	~VulkanCanvas();

private:
	void OnPaint(wxPaintEvent& event);
	void OnResize(wxSizeEvent& event);
};
