// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// PixelU: a fresh (portable) PixelU has no controller profiles, so no game can be played.
// When controllerProfiles/controller0.xml (or the old controller0.txt) doesn't exist yet, this
// writes one for player 1: an emulated Wii U GamePad driven by the first game controller
// (XInput for Xbox-type pads, SDL for other pads) plus the keyboard. An existing profile is never touched.
// Call it before InputManager::load().
void DefaultControllerProfile_CreateIfMissing();
