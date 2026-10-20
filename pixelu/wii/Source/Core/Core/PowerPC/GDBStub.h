// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// A remote debugging server that speaks the GDB remote serial protocol, so a
// debugger can attach to the emulated Broadway CPU over TCP (or, outside
// Windows, over a local socket).

#pragma once

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"

namespace GDBStub
{
// Signal numbers reported to the debugger in stop replies.
enum class Signal
{
  Sigtrap = 5,
  Sigterm = 15,
};

// Both block until a debugger connects.
void Init(u32 port);
void InitLocal(const char* socket);
void Deinit();
bool IsActive();
bool HasControl();
void TakeControl();
bool JustConnected();

// Serves requests from the debugger. With loop_until_continue set this keeps
// waiting for input until the debugger resumes, steps or detaches; otherwise it
// returns as soon as no more input is pending.
void ProcessCommands(bool loop_until_continue);
void SendSignal(Signal signal);
}  // namespace GDBStub
