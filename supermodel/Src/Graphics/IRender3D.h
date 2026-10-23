// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// IRender3D.h
//
// What the Real3D emulation needs from a 3D renderer. The renderer reads
// the Real3D memories it is given in AttachMemory() directly.

#ifndef INCLUDED_IRENDER3D_H
#define INCLUDED_IRENDER3D_H

#include <cstdint>
#include "Types.h"

class IRender3D
{
public:
  virtual ~IRender3D() {}

  // Called once per frame, in this order: BeginFrame, RenderFrame, EndFrame.
  virtual void RenderFrame(void) = 0;
  virtual void BeginFrame(void) = 0;
  virtual void EndFrame(void) = 0;

  // A rectangle of texture RAM changed (y counts both 1024-line sheets;
  // a 2048x2048 rectangle means all of it).
  virtual void UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height) = 0;

  virtual void AttachMemory(const uint32_t *cullingRAMLoPtr, const uint32_t *cullingRAMHiPtr, const uint32_t *polyRAMPtr, const uint32_t *vromPtr, const uint16_t *textureRAMPtr) = 0;
  virtual void SetStepping(int stepping) = 0;   // 0x10, 0x15, 0x20 or 0x21

  // xRes x yRes is the Model 3 screen's area (at xOffset, yOffset) inside a
  // totalXRes x totalYRes window; aaTarget is the frame buffer to draw into
  // (0: the default one).
  virtual Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget) = 0;

  virtual void SetSunClamp(bool enable) = 0;
  virtual void SetBlockCulling(bool enable) = 0;   // no 3D output this frame
  virtual float GetLosValue(int layer) = 0;        // line-of-sight result for a priority layer
};

#endif  // INCLUDED_IRENDER3D_H
