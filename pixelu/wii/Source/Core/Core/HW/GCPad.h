// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

class InputConfig;
enum class PadGroup;
struct GCPadStatus;

namespace ControllerEmu
{
class ControlGroup;
}

// The four emulated GameCube controllers and their input configuration.
namespace Pad
{
void Shutdown();
void Initialize();
void LoadConfig();
void GenerateDynamicInputTextures();
bool IsInitialized();

InputConfig* GetConfig();

GCPadStatus GetStatus(int pad_num);
ControllerEmu::ControlGroup* GetGroup(int pad_num, PadGroup group);
void Rumble(int pad_num, ControlState strength);
void ResetRumble(int pad_num);

bool GetMicButton(int pad_num);
}  // namespace Pad
