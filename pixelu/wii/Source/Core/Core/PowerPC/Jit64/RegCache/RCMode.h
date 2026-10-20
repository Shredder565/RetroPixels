// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// How an instruction accesses a cached guest register.
enum class RCMode
{
  Read,
  Write,
  ReadWrite,
};
