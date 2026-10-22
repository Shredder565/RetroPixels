// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/OSThread.h"

#include <fmt/format.h>

#include "Core/PowerPC/MMU.h"

namespace Core::Debug
{
namespace
{
// Reads consecutive big-endian fields of a guest structure. The structures below are declared
// in guest order, so filling them field by field follows the guest layout (the offsets are
// checked by the static_asserts in the header).
class GuestCursor
{
public:
  GuestCursor(const Core::CPUThreadGuard& guard, u32 address) : m_guard(guard), m_address(address)
  {
  }

  template <typename T> T Next()
  {
    const T value = PowerPC::MMU::HostRead<T>(m_guard, m_address);
    m_address += static_cast<u32>(sizeof(T));
    return value;
  }

  // Signed fields are stored as their two's complement bit pattern.
  s32 NextSigned() { return static_cast<s32>(Next<u32>()); }

  template <typename T, size_t N> void Fill(std::array<T, N>& out)
  {
    for (T& element : out)
      element = Next<T>();
  }

  void Skip(u32 bytes) { m_address += bytes; }

private:
  const Core::CPUThreadGuard& m_guard;
  u32 m_address;
};

template <class C> void ReadLink(GuestCursor& cursor, OSLink<C>& link)
{
  link.next = cursor.Next<u32>();
  link.prev = cursor.Next<u32>();
}

template <class C> void ReadQueue(GuestCursor& cursor, OSQueue<C>& queue)
{
  queue.head = cursor.Next<u32>();
  queue.tail = cursor.Next<u32>();
}

// The context layout follows what OSSaveContext / OSLoadContext and the exception vectors store.
void ReadContext(GuestCursor& cursor, OSContext& context)
{
  cursor.Fill(context.gpr);
  context.cr = cursor.Next<u32>();
  context.lr = cursor.Next<u32>();
  context.ctr = cursor.Next<u32>();
  context.xer = cursor.Next<u32>();
  cursor.Fill(context.fpr);
  context.fpscr = cursor.Next<u64>();
  context.srr0 = cursor.Next<u32>();
  context.srr1 = cursor.Next<u32>();
  context.dummy = cursor.Next<u16>();
  context.state = static_cast<OSContext::State>(cursor.Next<u16>());
  cursor.Fill(context.gqr);
  // The word in front of the paired singles only aligns them; it isn't read.
  context.psf_padding = 0;
  cursor.Skip(4);
  cursor.Fill(context.psf);
}
}  // namespace

void OSContext::Read(const Core::CPUThreadGuard& guard, u32 addr)
{
  GuestCursor cursor(guard, addr);
  ReadContext(cursor, *this);
}

// Layout as used by OSInitMutex, OSLockMutex and __OSUnlockAllMutex.
void OSMutex::Read(const Core::CPUThreadGuard& guard, u32 addr)
{
  GuestCursor cursor(guard, addr);
  ReadQueue(cursor, thread_queue);
  owner_addr = cursor.Next<u32>();
  lock_count = cursor.NextSigned();
  ReadLink(cursor, link);
}

// Layout as used by the thread functions of the OS library (OSCreateThread, OSJoinThread,
// OSSetThreadSpecific, ...) and by the socket library for errno.
void OSThread::Read(const Core::CPUThreadGuard& guard, u32 addr)
{
  GuestCursor cursor(guard, addr);
  ReadContext(cursor, context);

  state = cursor.Next<u16>();
  is_detached = cursor.Next<u16>();
  suspend = cursor.NextSigned();
  effective_priority = cursor.NextSigned();
  base_priority = cursor.NextSigned();
  exit_code_addr = cursor.Next<u32>();

  queue_addr = cursor.Next<u32>();
  ReadLink(cursor, queue_link);
  ReadQueue(cursor, join_queue);

  mutex_addr = cursor.Next<u32>();
  ReadQueue(cursor, mutex_queue);
  ReadLink(cursor, thread_link);

  stack_addr = cursor.Next<u32>();
  stack_end = cursor.Next<u32>();
  error = cursor.NextSigned();
  cursor.Fill(specific);
}

bool OSThread::IsValid(const Core::CPUThreadGuard& guard) const
{
  // A live thread has the magic word at the far end of its stack.
  if (!PowerPC::MMU::HostIsRAMAddress(guard, stack_end))
    return false;
  return PowerPC::MMU::HostRead<u32>(guard, stack_end) == STACK_MAGIC;
}

OSThreadView::OSThreadView(const Core::CPUThreadGuard& guard, u32 addr) : m_address(addr)
{
  m_thread.Read(guard, addr);
}

const OSThread& OSThreadView::Data() const
{
  return m_thread;
}

bool OSThreadView::IsValid(const Core::CPUThreadGuard& guard) const
{
  return m_thread.IsValid(guard);
}

Common::Debug::PartialContext OSThreadView::GetContext(const Core::CPUThreadGuard& guard) const
{
  Common::Debug::PartialContext out;
  if (!IsValid(guard))
    return out;

  const OSContext& saved = m_thread.context;
  out.gpr = saved.gpr;
  out.cr = saved.cr;
  out.lr = saved.lr;
  out.ctr = saved.ctr;
  out.xer = saved.xer;
  out.fpr = saved.fpr;
  out.fpscr = saved.fpscr;
  out.srr0 = saved.srr0;
  out.srr1 = saved.srr1;
  out.dummy = saved.dummy;
  out.state = static_cast<u16>(saved.state);
  out.gqr = saved.gqr;
  out.psf = saved.psf;
  return out;
}

u32 OSThreadView::GetAddress() const
{
  return m_address;
}

u16 OSThreadView::GetState() const
{
  return m_thread.state;
}

bool OSThreadView::IsDetached() const
{
  return m_thread.is_detached != 0;
}

bool OSThreadView::IsSuspended() const
{
  // The suspend count can go negative on resume without a matching suspend.
  return m_thread.suspend > 0;
}

s32 OSThreadView::GetEffectivePriority() const
{
  return m_thread.effective_priority;
}

s32 OSThreadView::GetBasePriority() const
{
  return m_thread.base_priority;
}

s32 OSThreadView::GetErrno() const
{
  return m_thread.error;
}

// The stack grows down from stack_addr towards stack_end.
u32 OSThreadView::GetStackStart() const
{
  return m_thread.stack_addr;
}

u32 OSThreadView::GetStackEnd() const
{
  return m_thread.stack_end;
}

std::size_t OSThreadView::GetStackSize() const
{
  return m_thread.stack_addr - m_thread.stack_end;
}

std::string OSThreadView::GetSpecific(const Core::CPUThreadGuard& guard) const
{
  // Thread-specific pointers often point at a name. List them until the first one that doesn't
  // point into RAM.
  std::string text;
  for (const u32 pointer : m_thread.specific)
  {
    if (!PowerPC::MMU::HostIsRAMAddress(guard, pointer))
      return text;
    text += fmt::format("{:08x} \"{}\"\n", pointer, PowerPC::MMU::HostGetString(guard, pointer));
  }
  return text;
}

}  // namespace Core::Debug
