// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#define DINPUT_SOURCE_NAME "DInput"

#include <list>
#include <string>

#include <windows.h>

#include "InputCommon/ControllerInterface/DInput/DInput8.h"

// The DirectInput backend: the system keyboard and mouse as one device, and the game
// controllers XInput does not handle. Used by the Win32 backend.
namespace ciface::DInput
{
// Enumeration callbacks that append each object or device to the std::list given as pvRef.
BOOL CALLBACK DIEnumDeviceObjectsCallback(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef);
BOOL CALLBACK DIEnumDevicesCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef);

// The device's product name, trimmed; empty if it can't be read.
std::string GetDeviceName(const LPDIRECTINPUTDEVICE8 device);

// Drops joysticks that went away and adds new devices. The window must be the one of the
// previous call.
void PopulateDevices(HWND hwnd);
// Re-creates the joysticks for a new window (exclusive mode is tied to the window).
void ChangeWindow(HWND hwnd);
void DeInit();
}  // namespace ciface::DInput
