// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// What DSPCore needs from a DSP recompiler. The x86-64 one lives in x64/; on
// hosts without one, DSPEmitterNull stands in and DSPCore runs nothing on it.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace DSP
{
class DSPCore;
}

namespace DSP::JIT
{
class DSPEmitter
{
public:
  virtual ~DSPEmitter();

  // Runs compiled code for about the given number of cycles; returns what is left.
  virtual u16 RunCycles(u16 cycles) = 0;
  // Throws away the code compiled from IRAM (new microcode was loaded).
  virtual void ClearIRAM() = 0;

  virtual void DoState(PointerWrap& p) = 0;
};

class DSPEmitterNull final : public DSPEmitter
{
public:
  u16 RunCycles(u16) override { return 0; }
  void ClearIRAM() override {}
  void DoState(PointerWrap&) override {}
};

// The recompiler for this host, bound to the given core.
std::unique_ptr<DSPEmitter> CreateDSPEmitter(DSPCore& dsp);
}  // namespace DSP::JIT
