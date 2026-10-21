// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>

#include "Common/CommonTypes.h"

struct GCPadStatus;

// The USB GameCube controller adapter, used by SI ports set to "GameCube Adapter for Wii U".
namespace GCAdapter
{
// Starts looking for the adapter (if any port wants it) and follows config changes.
void Init();
// Turns all the adapter's motors off.
void ResetRumble();
// Stops looking for the adapter and closes it.
void Shutdown();
// Called whenever the adapter is found, lost or fails to open.
void SetAdapterCallback(std::function<void(void)> func);

// The pad in port `chan` (0-3). After a pad is plugged in, the first call returns its origin
// with PAD_GET_ORIGIN set in the buttons; the SI adapter device and netplay rely on that.
GCPadStatus Input(int chan);

// Sets the motor of port `chan`.
void Output(int chan, u8 rumble_command);
// Whether the adapter is open. When it failed to open, returns false and sets *error_message to
// libusb's description of the error; otherwise sets it to null.
bool IsDetected(const char** error_message);
// Whether a pad is plugged into port `chan`.
bool DeviceConnected(int chan);
// Forgets the pad in port `chan`, so it reports a new connection (and origin) again.
void ResetDeviceType(int chan);

// Reads per second, callable from any thread. 0 while the adapter is not open.
double GetCurrentPollRate();
}  // namespace GCAdapter
