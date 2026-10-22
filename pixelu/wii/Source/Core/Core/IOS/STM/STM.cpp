// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/STM/STM.h"

#include <functional>
#include <memory>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// The single outstanding event hook request. It is shared by both STM nodes because the
// immediate node can release it.
std::unique_ptr<IOCtlRequest> s_pending_hook;

// Answers the pending hook with `event` in its output word.
void CompleteHook(EmulationKernel& kernel, Memory::MemoryManager& memory, u32 event)
{
  memory.Write_U32(event, s_pending_hook->buffer_out);
  kernel.EnqueueIPCReply(*s_pending_hook, IPC_SUCCESS);
  s_pending_hook.reset();
}
}  // namespace

std::optional<IPCReply> STMImmediateDevice::IOCtl(const IOCtlRequest& request)
{
  const u32 command = request.request;

  if (command == IOCTL_STM_IDLE || command == IOCTL_STM_SHUTDOWN)
  {
    NOTICE_LOG_FMT(IOS_STM, "Shutdown/idle requested ({:#x}); stopping emulation", command);
    Core::QueueHostJob(&Core::Stop, false);
    return IPCReply(IPC_SUCCESS);
  }

  if (command == IOCTL_STM_RELEASE_EH)
  {
    // Cancels the event hook: it is answered with event word 0.
    if (!s_pending_hook)
      return IPCReply(IPC_ENOENT);
    CompleteHook(GetEmulationKernel(), GetSystem().GetMemory(), 0);
    return IPCReply(IPC_SUCCESS);
  }

  switch (command)
  {
  case IOCTL_STM_HOTRESET:
  case IOCTL_STM_VIDIMMING:
  case IOCTL_STM_LEDMODE:
    // Accepted without doing anything.
    INFO_LOG_FMT(IOS_STM, "{}: ignoring ioctl {:#x}", GetDeviceName(), command);
    return IPCReply(IPC_SUCCESS);

  // Known, but not emulated.
  case IOCTL_STM_WAKEUP:
  case IOCTL_STM_HOTRESET_FOR_PD:
  case IOCTL_STM_LEDFLASH:
  case IOCTL_STM_READVER:
  case IOCTL_STM_GET_IDLEMODE:
  case IOCTL_STM_WRITEDMCU:
  case IOCTL_STM_READDDRREG:
  case IOCTL_STM_READDDRREG2:
    ERROR_LOG_FMT(IOS_STM, "{}: ioctl {:#x} is not implemented", GetDeviceName(), command);
    return IPCReply(IPC_SUCCESS);

  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_STM);
    return IPCReply(IPC_UNKNOWN);
  }
}

STMEventHookDevice::~STMEventHookDevice()
{
  s_pending_hook.reset();
}

std::optional<IPCReply> STMEventHookDevice::IOCtl(const IOCtlRequest& request)
{
  if (request.request != IOCTL_STM_EVENTHOOK)
    return IPCReply(IPC_UNKNOWN);

  // Only one hook can wait at a time.
  if (s_pending_hook)
    return IPCReply(IPC_EEXIST);

  // No reply now; it is sent when a button is pressed or the hook is released.
  s_pending_hook = std::make_unique<IOCtlRequest>(GetSystem(), request.address);
  return std::nullopt;
}

void STMEventHookDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);

  // Only the request's address is saved (0 when no hook is waiting). The request is always
  // rebuilt from emulated memory afterwards, on load and on save alike.
  u32 hook_address = s_pending_hook ? s_pending_hook->address : 0;
  p.Do(hook_address);
  if (hook_address == 0)
    s_pending_hook.reset();
  else
    s_pending_hook = std::make_unique<IOCtlRequest>(GetSystem(), hook_address);
}

bool STMEventHookDevice::HasHookInstalled() const
{
  return s_pending_hook != nullptr;
}

void STMEventHookDevice::TriggerEvent(const u32 event) const
{
  // A press is lost when nobody has the node open or no hook is waiting.
  if (m_is_active && s_pending_hook)
    CompleteHook(GetEmulationKernel(), GetSystem().GetMemory(), event);
}

void STMEventHookDevice::ResetButton() const
{
  TriggerEvent(STM_EVENT_RESET);
}

void STMEventHookDevice::PowerButton() const
{
  TriggerEvent(STM_EVENT_POWER);
}
}  // namespace IOS::HLE
