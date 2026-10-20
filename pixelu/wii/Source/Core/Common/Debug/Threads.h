// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Common::Debug
{
// Saved CPU registers of a guest thread. A field is empty when the thread structure does not
// record that register.
struct PartialContext
{
  std::optional<std::array<u32, 32>> gpr;
  std::optional<u32> cr;
  std::optional<u32> lr;
  std::optional<u32> ctr;
  std::optional<u32> xer;
  std::optional<std::array<double, 32>> fpr;
  std::optional<u64> fpscr;
  std::optional<u32> srr0;
  std::optional<u32> srr1;
  std::optional<u16> dummy;
  std::optional<u16> state;
  std::optional<std::array<u32, 8>> gqr;
  std::optional<std::array<double, 32>> psf;
};

// The debugger's view of one guest thread (an OS or libogc thread control block in memory).
class ThreadView
{
public:
  virtual ~ThreadView() = default;

  enum class API
  {
    OSThread,   // Nintendo SDK thread
    LWPThread,  // devkitPro libogc thread
  };

  virtual PartialContext GetContext(const Core::CPUThreadGuard& guard) const = 0;
  virtual u32 GetAddress() const = 0;
  virtual u16 GetState() const = 0;
  virtual bool IsSuspended() const = 0;
  virtual bool IsDetached() const = 0;
  virtual s32 GetBasePriority() const = 0;
  virtual s32 GetEffectivePriority() const = 0;
  virtual u32 GetStackStart() const = 0;
  virtual u32 GetStackEnd() const = 0;
  virtual std::size_t GetStackSize() const = 0;
  virtual s32 GetErrno() const = 0;
  // Extra, implementation-defined text about the thread.
  virtual std::string GetSpecific(const Core::CPUThreadGuard& guard) const = 0;
  virtual bool IsValid(const Core::CPUThreadGuard& guard) const = 0;
};

using Threads = std::vector<std::unique_ptr<ThreadView>>;
}  // namespace Common::Debug
