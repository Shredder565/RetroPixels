// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/EnumMap.h"

// The keys the on-screen UI understands, so the Qt side can describe its key codes without
// including ImGui. A, C, V, X, Y and Z are there for the text-edit shortcuts (select all, copy,
// paste, cut, redo, undo).
enum class DolphinKey
{
  Tab,
  LeftArrow,
  RightArrow,
  UpArrow,
  DownArrow,
  PageUp,
  PageDown,
  Home,
  End,
  Insert,
  Delete,
  Backspace,
  Space,
  Enter,
  Escape,
  KeyPadEnter,
  A,
  C,
  V,
  X,
  Y,
  Z,
};

// Host key code for each DolphinKey.
using DolphinKeyMap = Common::EnumMap<int, DolphinKey::Z>;
