// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The CPU context a fault handler receives, as the JIT's fastmem and stack
// fault handling sees it: `SContext` is the platform's saved-register
// structure, `ctx->CTX_PC` is its instruction pointer, and ContextRN(ctx, n)
// points at general register n in x86-64 encoding order (RAX, RCX, RDX, RBX,
// RSP, RBP, RSI, RDI, R8..R15).
//
// PixelU-Wii is built for Windows x64. The Linux variant only exists so the
// core still compiles there.

#include "Common/CommonTypes.h"

#if defined(_M_GENERIC)

// No JIT backend: nothing ever looks inside the context.
struct FakeGenericContext;
typedef FakeGenericContext SContext;

#elif defined(_WIN32)

#include <windows.h>
typedef CONTEXT SContext;
#define CTX_PC Rip
#define PIXELU_CTX_GPR(name) offsetof(SContext, name)
#define PIXELU_CTX_GPR_TABLE                                                                       \
  PIXELU_CTX_GPR(Rax), PIXELU_CTX_GPR(Rcx), PIXELU_CTX_GPR(Rdx), PIXELU_CTX_GPR(Rbx),             \
      PIXELU_CTX_GPR(Rsp), PIXELU_CTX_GPR(Rbp), PIXELU_CTX_GPR(Rsi), PIXELU_CTX_GPR(Rdi),         \
      PIXELU_CTX_GPR(R8), PIXELU_CTX_GPR(R9), PIXELU_CTX_GPR(R10), PIXELU_CTX_GPR(R11),           \
      PIXELU_CTX_GPR(R12), PIXELU_CTX_GPR(R13), PIXELU_CTX_GPR(R14), PIXELU_CTX_GPR(R15)

#elif defined(__linux__)

#include <signal.h>
#include <ucontext.h>
typedef mcontext_t SContext;
#define CTX_PC gregs[REG_RIP]
#define PIXELU_CTX_GPR(name) offsetof(SContext, gregs) + sizeof(greg_t) * (name)
#define PIXELU_CTX_GPR_TABLE                                                                       \
  PIXELU_CTX_GPR(REG_RAX), PIXELU_CTX_GPR(REG_RCX), PIXELU_CTX_GPR(REG_RDX),                       \
      PIXELU_CTX_GPR(REG_RBX), PIXELU_CTX_GPR(REG_RSP), PIXELU_CTX_GPR(REG_RBP),                   \
      PIXELU_CTX_GPR(REG_RSI), PIXELU_CTX_GPR(REG_RDI), PIXELU_CTX_GPR(REG_R8),                    \
      PIXELU_CTX_GPR(REG_R9), PIXELU_CTX_GPR(REG_R10), PIXELU_CTX_GPR(REG_R11),                    \
      PIXELU_CTX_GPR(REG_R12), PIXELU_CTX_GPR(REG_R13), PIXELU_CTX_GPR(REG_R14),                   \
      PIXELU_CTX_GPR(REG_R15)

#else
#error PixelU-Wii has no machine context definition for this platform
#endif

#if defined(_M_X86_64) && !defined(_M_GENERIC)
#include <cstddef>

static inline u64* ContextRN(SContext* ctx, int n)
{
  // Byte offset of each general register inside SContext, by x86 register number.
  static constexpr std::size_t gpr_offsets[16] = {PIXELU_CTX_GPR_TABLE};
  return reinterpret_cast<u64*>(reinterpret_cast<char*>(ctx) + gpr_offsets[n]);
}
#endif

#undef PIXELU_CTX_GPR
#undef PIXELU_CTX_GPR_TABLE
