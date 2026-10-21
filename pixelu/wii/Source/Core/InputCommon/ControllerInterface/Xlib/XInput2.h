// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "InputCommon/ControllerInterface/InputBackend.h"

// Keyboard and mouse on X11 (also XWayland), through the X Input Extension 2. Each master
// pointer and its paired master keyboard become one device, source "XInput2", named after the
// pointer ("Virtual core pointer" on a desktop with one seat). Only used when the render window
// is an X11 window. The X11 headers stay inside XInput2.cpp: their macros (None, Bool, Status...)
// clash with names elsewhere.
namespace ciface::XInput2
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);
}  // namespace ciface::XInput2
