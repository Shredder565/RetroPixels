// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/Jit/DSPEmitterBase.h"

#if defined(_M_X86_64)
#include "Core/DSP/Jit/x64/DSPEmitter.h"
#define PIXELU_DSP_HAS_RECOMPILER 1
#else
#define PIXELU_DSP_HAS_RECOMPILER 0
#endif

namespace DSP::JIT
{
// Out of line so the vtable is emitted in this file only.
DSPEmitter::~DSPEmitter() = default;

std::unique_ptr<DSPEmitter> CreateDSPEmitter([[maybe_unused]] DSPCore& dsp)
{
  // Only x86-64 has a recompiler; every other host gets the do-nothing one.
#if PIXELU_DSP_HAS_RECOMPILER
  std::unique_ptr<DSPEmitter> emitter = std::make_unique<x64::DSPEmitter>(dsp);
#else
  std::unique_ptr<DSPEmitter> emitter = std::make_unique<DSPEmitterNull>();
#endif
  return emitter;
}
}  // namespace DSP::JIT

#undef PIXELU_DSP_HAS_RECOMPILER
