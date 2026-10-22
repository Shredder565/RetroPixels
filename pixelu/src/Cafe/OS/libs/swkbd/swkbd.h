// Original code for RetroPixels (PixelU). Not derived from Cemu.
// swkbd: on-screen keyboard.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

// draws the keyboard while a game shows it; called by the renderer every frame
void swkbd_render(bool mainWindow);
// true while the keyboard is up and wants the host keyboard's keys
bool swkbd_hasKeyboardInputHook();
// one typed character (8 = backspace, 13 = enter)
void swkbd_keyInput(uint32 keyCode);

namespace swkbd
{
	COSModule* GetModule();
}
