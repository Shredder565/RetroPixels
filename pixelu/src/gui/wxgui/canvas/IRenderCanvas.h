// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wxprec.h>

// Shared part of the render canvases (OpenGL, Vulkan): whether this canvas shows the TV picture
// in the main window (true) or the GamePad picture in its own window (false).
class IRenderCanvas
{
public:
	IRenderCanvas(bool is_main_window)
		: m_is_main_window(is_main_window) {}

protected:
	bool m_is_main_window;
};
