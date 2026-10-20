// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <wx/window.h>

// Creates the OpenGL render canvas. The main window's canvas also creates the OpenGL renderer.
wxWindow* GLCanvas_Create(wxWindow* parent, const wxSize& size, bool is_main_window);
