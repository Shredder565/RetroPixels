// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class System;
}

// The CPU's write gather pipe. Stores to the gather pipe address collect in
// a buffer; each full 32-byte burst goes to the CPU FIFO in main memory
// (PI FIFO pointers) and is announced to the command processor. The JITs
// write to the buffer directly through PowerPCState::gather_pipe_ptr.
namespace GPFifo
{
// The address is set through the WPAR SPR, but every game uses this one.
constexpr u32 GATHER_PIPE_PHYSICAL_ADDRESS = 0x0C008000;

constexpr u32 GATHER_PIPE_SIZE = 32;
// Room for many writes between burst checks.
constexpr u32 GATHER_PIPE_EXTRA_SIZE = GATHER_PIPE_SIZE * 16;

class GPFifoManager final
{
public:
  explicit GPFifoManager(Core::System& system);

  void Init();
  void DoState(PointerWrap& p);

  void ResetGatherPipe();
  // Sends every complete burst.
  void UpdateGatherPipe();
  // UpdateGatherPipe() if a burst is complete, and tells the JIT this code
  // writes to the FIFO.
  void CheckGatherPipe();
  // UpdateGatherPipe() if a burst is complete.
  void FastCheckGatherPipe();

  // The WPAR "buffer not empty" bit.
  bool IsBNE() const;

  // Big-endian stores to the pipe, followed by a burst check.
  void Write8(u8 value);
  void Write16(u16 value);
  void Write32(u32 value);
  void Write64(u64 value);

  // Big-endian stores without the check. The buffer allows about 512 bytes
  // of these between checks.
  void FastWrite8(u8 value);
  void FastWrite16(u16 value);
  void FastWrite32(u32 value);
  void FastWrite64(u64 value);

private:
  size_t GetGatherPipeCount();
  void SetGatherPipeCount(size_t size);

  alignas(GATHER_PIPE_SIZE) u8 m_gather_pipe[GATHER_PIPE_EXTRA_SIZE]{};

  Core::System& m_system;
};

// For the JITs.
void UpdateGatherPipe(GPFifoManager& gpfifo);
void FastCheckGatherPipe(GPFifoManager& gpfifo);
}  // namespace GPFifo
