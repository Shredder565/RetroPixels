// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The interface every CPU execution engine (interpreter, cached interpreter,
// JIT) implements so the CPU thread can drive it without knowing which one
// is in use.
class CPUCoreBase
{
public:
  virtual ~CPUCoreBase() = default;

  virtual void Init() = 0;
  virtual void Shutdown() = 0;
  // Throws away anything the engine has cached about guest code.
  virtual void ClearCache() = 0;
  // Executes until the CPU is asked to stop.
  virtual void Run() = 0;
  // Executes exactly one guest instruction.
  virtual void SingleStep() = 0;
  virtual const char* GetName() const = 0;
};
