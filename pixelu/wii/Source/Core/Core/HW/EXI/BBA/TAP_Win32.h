// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// The few facts PixelU-Wii needs to talk to an installed TAP-Windows network adapter: its
// device control codes, where Windows lists network adapters in the registry, and how the
// adapter's device is named.

#pragma once

#include <windows.h>
#include <winioctl.h>

// Oldest driver release with the controls used here.
constexpr int TAP_WIN32_MIN_MAJOR = 9;
constexpr int TAP_WIN32_MIN_MINOR = 0;

// Driver requests are numbered from 1 and sent buffered.
constexpr DWORD TapControl(DWORD request)
{
  return CTL_CODE(FILE_DEVICE_UNKNOWN, request, METHOD_BUFFERED, FILE_ANY_ACCESS);
}
// Reply: three ULONGs, major, minor and debug flag.
constexpr DWORD TAP_IOCTL_GET_VERSION = TapControl(2);
// Input: a ULONG, non-zero to report the cable as connected.
constexpr DWORD TAP_IOCTL_SET_MEDIA_STATUS = TapControl(6);

// The network adapter device class, in Windows' list of device classes and in its network
// connection settings.
#define TAP_NET_CLASS L"{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define ADAPTER_KEY L"SYSTEM\\CurrentControlSet\\Control\\Class\\" TAP_NET_CLASS
#define NETWORK_CONNECTIONS_KEY L"SYSTEM\\CurrentControlSet\\Control\\Network\\" TAP_NET_CLASS

// An adapter is opened as \\.\Global\<adapter GUID>.tap.
#define USERMODEDEVICEDIR L"\\\\.\\Global\\"
#define TAPSUFFIX L".tap"

// The ComponentId the TAP-Windows 9 driver registers its adapters under.
#define TAP_COMPONENT_ID L"tap0901"
