// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

class LibUSBBluetoothAdapter;

// Realtek's own USB vendor ID.
bool IsRealtekVID(u16 vid);
// Adapters with a Realtek chip, including ones sold under other vendor IDs.
bool IsKnownRealtekBluetoothDevice(u16 vid, u16 pid);

// Asks the chip which firmware it runs and uploads Realtek's patch when it needs one.
// Returns false when the adapter could not be set up.
bool InitializeRealtekBluetoothDevice(LibUSBBluetoothAdapter&);
