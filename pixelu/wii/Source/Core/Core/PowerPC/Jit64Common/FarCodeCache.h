// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/x64Emitter.h"

// Far code holds the rarely executed paths (slow memory accesses, exception
// exits) so the hot code stays dense. With the MMU on, most loads and stores
// get such a path, hence the larger size.
constexpr size_t FARCODE_SIZE = 1024 * 1024 * 64;
constexpr size_t FARCODE_SIZE_MMU = 1024 * 1024 * 128;

class FarCodeCache : public Gen::X64CodeBlock
{
public:
  void Init() { m_enabled = true; }
  void Shutdown() { m_enabled = false; }

  bool Enabled() const { return m_enabled; }

private:
  bool m_enabled = false;
};
