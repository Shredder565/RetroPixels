// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

namespace Config
{
// Configuration layers, from lowest to highest priority. A value set in a higher layer hides the
// same setting in every layer below it.
enum class LayerType
{
  Base,         // the user's INI files
  CommandLine,  // -C options
  GlobalGame,   // game INIs shipped with the emulator
  LocalGame,    // the user's per-game INIs
  Movie,        // settings stored in a recording being played back
  Netplay,      // settings forced by a NetPlay host
  CurrentRun,   // temporary values for this session only
  Meta,         // not a real layer: "whatever is currently in effect"
};

// The settings groups; each one maps to its own INI file in the Base layer.
enum class System
{
  Main,
  SYSCONF,
  GCPad,
  WiiPad,
  GCKeyboard,
  GFX,
  Logger,
  DualShockUDPClient,
  FreeLook,
  Session,
  GameSettingsOnly,
  Achievements,
};

// Lookup order for a setting's effective value: highest priority first.
constexpr std::array<LayerType, 7> SEARCH_ORDER{{
    LayerType::CurrentRun,
    LayerType::Netplay,
    LayerType::Movie,
    LayerType::LocalGame,
    LayerType::GlobalGame,
    LayerType::CommandLine,
    LayerType::Base,
}};
}  // namespace Config
