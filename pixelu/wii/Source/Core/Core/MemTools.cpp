// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/MemTools.h"

#include <cstdint>
#include <cstdlib>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/MsgHandler.h"

#include "Core/MachineContext.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/System.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace EMM
{
namespace
{
JitInterface& Jit()
{
  return Core::System::GetInstance().GetJitInterface();
}
}  // namespace

#if defined(_WIN32)

// Windows: a vectored exception handler, called before any frame-based (SEH)
// handler. Anything the JIT does not claim is passed on unchanged so that
// debuggers and crash reporting still see it.
namespace
{
PVOID s_handler_cookie = nullptr;

// ExceptionInformation[0] of an access violation: 0 read, 1 write, 8 DEP
// (execute) violation.
constexpr ULONG_PTR ACCESS_KIND_EXECUTE = 8;

LONG NTAPI VectoredHandler(PEXCEPTION_POINTERS info)
{
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;

  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
  {
    // Jumps into non-executable memory are never fastmem accesses.
    if (record.ExceptionInformation[0] == ACCESS_KIND_EXECUTE)
      return EXCEPTION_CONTINUE_SEARCH;

    const auto faulting_address = static_cast<uintptr_t>(record.ExceptionInformation[1]);
    return Jit().HandleFault(faulting_address, info->ContextRecord) ?
               EXCEPTION_CONTINUE_EXECUTION :
               EXCEPTION_CONTINUE_SEARCH;
  }

  if (record.ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    return Jit().HandleStackFault() ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;

  // Illegal or privileged instructions, in-page errors, breakpoints and the rest
  // are not ours.
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallExceptionHandler()
{
  ASSERT(!s_handler_cookie);
  // First = TRUE: run ahead of handlers that are already registered.
  s_handler_cookie = AddVectoredExceptionHandler(TRUE, VectoredHandler);
  ASSERT(s_handler_cookie);
}

void UninstallExceptionHandler()
{
  const ULONG removed = RemoveVectoredExceptionHandler(s_handler_cookie);
  ASSERT(removed);
  if (removed != 0)
    s_handler_cookie = nullptr;
}

bool IsExceptionHandlerSupported()
{
  return true;
}

#elif defined(_POSIX_VERSION) && !defined(_M_GENERIC)

// POSIX: a SIGSEGV handler running on its own signal stack (so stack
// overflows can still be reported). Faults the JIT does not claim go to
// whatever handler was installed before ours.
namespace
{
struct sigaction s_previous_segv_action;

void SegvHandler(int signal_number, siginfo_t* info, void* raw_context)
{
  if (signal_number != SIGSEGV)
    return;

  const int reason = info->si_code;
  if (reason != SEGV_MAPERR && reason != SEGV_ACCERR)
    return;

  auto* const user_context = static_cast<ucontext_t*>(raw_context);
  const auto faulting_address = reinterpret_cast<uintptr_t>(info->si_addr);
  if (Jit().HandleFault(faulting_address, &user_context->uc_mcontext))
    return;

  // Not a JIT fault: put the old disposition back and let it handle a re-raise.
  sigaction(signal_number, &s_previous_segv_action, nullptr);
  raise(signal_number);
}
}  // namespace

void InstallExceptionHandler()
{
  stack_t alternate_stack{};
  alternate_stack.ss_sp = std::malloc(static_cast<size_t>(SIGSTKSZ));
  alternate_stack.ss_size = static_cast<size_t>(SIGSTKSZ);
  alternate_stack.ss_flags = 0;
  if (sigaltstack(&alternate_stack, nullptr) != 0)
    PanicAlertFmt("sigaltstack failed: {}", Common::LastStrerrorString());

  struct sigaction action{};
  action.sa_sigaction = &SegvHandler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, &s_previous_segv_action);
}

void UninstallExceptionHandler()
{
  stack_t disable_request{};
  disable_request.ss_flags = SS_DISABLE;
  stack_t previous_stack{};
  const bool got_previous = sigaltstack(&disable_request, &previous_stack) == 0;
  if (got_previous && (previous_stack.ss_flags & SS_DISABLE) == 0)
    std::free(previous_stack.ss_sp);

  sigaction(SIGSEGV, &s_previous_segv_action, nullptr);
}

bool IsExceptionHandlerSupported()
{
  return true;
}

#else

// No way to intercept faults here: the JIT must run without fastmem.
void InstallExceptionHandler()
{
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return false;
}

#endif
}  // namespace EMM
