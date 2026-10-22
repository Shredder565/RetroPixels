// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Flags reported in GCPadStatus::button besides the buttons themselves.
enum
{
  PAD_USE_ORIGIN = 0x0080,
  // The pad was just connected and the game should read its origin (resting) values.
  PAD_GET_ORIGIN = 0x2000,
  // No pad in this port.
  PAD_ERR_STATUS = 0x8000,
};

// Bits of GCPadStatus::button.
enum PadButton
{
  PAD_BUTTON_LEFT = 0x0001,
  PAD_BUTTON_RIGHT = 0x0002,
  PAD_BUTTON_DOWN = 0x0004,
  PAD_BUTTON_UP = 0x0008,
  PAD_TRIGGER_Z = 0x0010,
  PAD_TRIGGER_R = 0x0020,
  PAD_TRIGGER_L = 0x0040,
  PAD_BUTTON_A = 0x0100,
  PAD_BUTTON_B = 0x0200,
  PAD_BUTTON_X = 0x0400,
  PAD_BUTTON_Y = 0x0800,
  PAD_BUTTON_START = 0x1000,
};

// Bits of GCPadStatus::switches (Triforce cabinet switches).
enum SwitchButton
{
  SWITCH_TEST = 0x0001,
  SWITCH_SERVICE = 0x0002,
  SWITCH_COIN = 0x0004,
};

// The state of one GameCube controller. Every analog value is a full 0-255 byte.
struct GCPadStatus
{
  // Resting values and reach of the two sticks.
  static const u8 MAIN_STICK_CENTER_X = 0x80;
  static const u8 MAIN_STICK_CENTER_Y = 0x80;
  static const u8 MAIN_STICK_RADIUS = 0x7f;
  static const u8 C_STICK_CENTER_X = 0x80;
  static const u8 C_STICK_CENTER_Y = 0x80;
  static const u8 C_STICK_RADIUS = 0x7f;

  // PadButton bits, plus the flags above.
  u16 button = 0;
  // Main stick.
  u8 stickX = MAIN_STICK_CENTER_X;
  u8 stickY = MAIN_STICK_CENTER_Y;
  // C stick.
  u8 substickX = C_STICK_CENTER_X;
  u8 substickY = C_STICK_CENTER_Y;
  // Analog trigger and button travel.
  u8 triggerLeft = 0;
  u8 triggerRight = 0;
  u8 analogA = 0;
  u8 analogB = 0;
  // SwitchButton bits.
  u8 switches = 0;
  bool isConnected = true;
};
