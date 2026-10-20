// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GPFifo.h"

#include <cstddef>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

namespace GPFifo
{
namespace
{
// Appends a value to the pipe in guest (big-endian) byte order.
template <typename T>
void Append(PowerPC::PowerPCState& ppc_state, T value)
{
  const T big_endian = Common::FromBigEndian(value);
  std::memcpy(ppc_state.gather_pipe_ptr, &big_endian, sizeof(T));
  ppc_state.gather_pipe_ptr += sizeof(T);
}
}  // namespace

GPFifoManager::GPFifoManager(Core::System& system) : m_system(system)
{
}

size_t GPFifoManager::GetGatherPipeCount()
{
  return m_system.GetPPCState().gather_pipe_ptr - m_gather_pipe;
}

void GPFifoManager::SetGatherPipeCount(size_t size)
{
  m_system.GetPPCState().gather_pipe_ptr = m_gather_pipe + size;
}

void GPFifoManager::DoState(PointerWrap& p)
{
  p.DoArray(m_gather_pipe);
  u32 count = static_cast<u32>(GetGatherPipeCount());
  p.Do(count);
  SetGatherPipeCount(count);
}

void GPFifoManager::Init()
{
  ResetGatherPipe();
  m_system.GetPPCState().gather_pipe_base_ptr = m_gather_pipe;
  std::memset(m_gather_pipe, 0, sizeof(m_gather_pipe));
}

bool GPFifoManager::IsBNE() const
{
  // When the hardware sets BNE is not known. Display list code
  // (GXRedirectWriteGatherPipe and friends) waits for it to clear, so
  // reporting an empty buffer avoids hangs.
  return false;
}

void GPFifoManager::ResetGatherPipe()
{
  SetGatherPipeCount(0);
}

void GPFifoManager::UpdateGatherPipe()
{
  auto& memory = m_system.GetMemory();
  auto& pi = m_system.GetProcessorInterface();
  auto& command_processor = m_system.GetCommandProcessor();

  const size_t count = GetGatherPipeCount();
  size_t sent = 0;
  while (count - sent >= GATHER_PIPE_SIZE)
  {
    memory.CopyToEmu(pi.m_fifo_cpu_write_pointer, m_gather_pipe + sent, GATHER_PIPE_SIZE);
    sent += GATHER_PIPE_SIZE;

    // The write pointer wraps from the FIFO end back to its base.
    if (pi.m_fifo_cpu_write_pointer == pi.m_fifo_cpu_end)
      pi.m_fifo_cpu_write_pointer = pi.m_fifo_cpu_base;
    else
      pi.m_fifo_cpu_write_pointer += GATHER_PIPE_SIZE;

    command_processor.GatherPipeBursted();
  }

  // Keep the incomplete rest at the start of the buffer.
  const size_t rest = count - sent;
  std::memmove(m_gather_pipe, m_gather_pipe + sent, rest);
  SetGatherPipeCount(rest);
}

void GPFifoManager::FastCheckGatherPipe()
{
  if (GetGatherPipeCount() >= GATHER_PIPE_SIZE)
    UpdateGatherPipe();
}

void GPFifoManager::CheckGatherPipe()
{
  if (GetGatherPipeCount() < GATHER_PIPE_SIZE)
    return;
  UpdateGatherPipe();
  // Lets the JIT recompile this code with FIFO checks inlined.
  m_system.GetJitInterface().CompileExceptionCheck(JitInterface::ExceptionType::FIFOWrite);
}

void GPFifoManager::Write8(const u8 value)
{
  FastWrite8(value);
  CheckGatherPipe();
}

void GPFifoManager::Write16(const u16 value)
{
  FastWrite16(value);
  CheckGatherPipe();
}

void GPFifoManager::Write32(const u32 value)
{
  FastWrite32(value);
  CheckGatherPipe();
}

void GPFifoManager::Write64(const u64 value)
{
  FastWrite64(value);
  CheckGatherPipe();
}

void GPFifoManager::FastWrite8(const u8 value)
{
  Append(m_system.GetPPCState(), value);
}

void GPFifoManager::FastWrite16(u16 value)
{
  Append(m_system.GetPPCState(), value);
}

void GPFifoManager::FastWrite32(u32 value)
{
  Append(m_system.GetPPCState(), value);
}

void GPFifoManager::FastWrite64(u64 value)
{
  Append(m_system.GetPPCState(), value);
}

void UpdateGatherPipe(GPFifoManager& gpfifo)
{
  gpfifo.UpdateGatherPipe();
}

void FastCheckGatherPipe(GPFifoManager& gpfifo)
{
  gpfifo.FastCheckGatherPipe();
}
}  // namespace GPFifo
