// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

class InputConfig;
enum class KeyboardGroup;
struct KeyboardStatus;

namespace ControllerEmu
{
class ControlGroup;
}

// The four emulated GameCube keyboards and their input configuration.
namespace Keyboard
{
void Shutdown();
void Initialize();
void LoadConfig();
void GenerateDynamicInputTextures();

InputConfig* GetConfig();
ControllerEmu::ControlGroup* GetGroup(int port, KeyboardGroup group);

KeyboardStatus GetStatus(int port);
}  // namespace Keyboard
