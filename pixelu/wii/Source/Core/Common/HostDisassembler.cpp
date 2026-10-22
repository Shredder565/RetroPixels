// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/HostDisassembler.h"

#include <span>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#if defined(HAVE_LLVM)
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>
#elif defined(_M_X86_64)
#include <disasm.h>  // Bochs
#endif

namespace
{
// Address of an instruction, then a tab and the bytes in hex.
void PrintHex(std::ostream& stream, const u8* at, std::span<const u8> bytes, const char* prefix)
{
  fmt::println(stream, "{}\t{}{:02x}", fmt::ptr(at), prefix, fmt::join(bytes, ""));
}

#if defined(HAVE_LLVM)
// Disassembly through LLVM's C API, for any target LLVM was built with.
class LLVMDisassembler final : public HostDisassembler
{
public:
  // fixed_size is the instruction size of a fixed-width ISA (0 for x86), which lets the output
  // continue after an undecodable instruction.
  LLVMDisassembler(const char* triple, const char* cpu, std::size_t fixed_size)
      : m_fixed_size(fixed_size)
  {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllDisassemblers();

    m_context = LLVMCreateDisasmCPU(triple, cpu, nullptr, 0, nullptr, nullptr);
    if (m_context != nullptr)
    {
      LLVMSetDisasmOptions(m_context, LLVMDisassembler_Option_AsmPrinterVariant |
                                          LLVMDisassembler_Option_PrintLatency);
    }
  }

  ~LLVMDisassembler() override
  {
    if (m_context != nullptr)
      LLVMDisasmDispose(m_context);
  }

  LLVMDisassembler(const LLVMDisassembler&) = delete;
  LLVMDisassembler& operator=(const LLVMDisassembler&) = delete;

private:
  std::size_t Disassemble(const u8* begin, const u8* end, std::ostream& stream) override
  {
    if (m_context == nullptr)
      return 0;

    std::size_t count = 0;
    const u8* at = begin;
    while (at < end)
    {
      char text[256];
      const std::size_t size = LLVMDisasmInstruction(
          m_context, const_cast<u8*>(at), static_cast<u64>(end - at), reinterpret_cast<u64>(at),
          text, sizeof(text));

      if (size != 0)
      {
        // LLVM's text starts with a tab.
        fmt::println(stream, "{}{}", fmt::ptr(at), text);
        at += size;
      }
      else if (m_fixed_size != 0)
      {
        PrintHex(stream, at, std::span{at, m_fixed_size}, "Invalid inst: ");
        at += m_fixed_size;
      }
      else
      {
        // With variable-length instructions there is no telling where the next one starts.
        // (The dump is not counted as an instruction.)
        PrintHex(stream, at, std::span{at, end}, "Invalid inst: ");
        break;
      }
      count++;
    }
    return count;
  }

  LLVMDisasmContextRef m_context = nullptr;
  std::size_t m_fixed_size;
};

#elif defined(_M_X86_64)
// The bundled Bochs x86 disassembler, in Intel syntax.
class BochsDisassembler final : public HostDisassembler
{
public:
  BochsDisassembler() { m_disasm.set_syntax_intel(); }

private:
  std::size_t Disassemble(const u8* begin, const u8* end, std::ostream& stream) override
  {
    std::size_t count = 0;
    for (const u8* at = begin; at < end; count++)
    {
      char text[256];
      const unsigned size = m_disasm.disasm64(0, reinterpret_cast<bx_address>(at), at, text);
      fmt::println(stream, "{}\t{}", fmt::ptr(at), text);
      at += size;
    }
    return count;
  }

  disassembler m_disasm;
};
#endif
}  // namespace

std::unique_ptr<HostDisassembler> HostDisassembler::Factory(Platform arch)
{
#if defined(HAVE_LLVM)
  if (arch == Platform::x86_64)
    return std::make_unique<LLVMDisassembler>("x86_64-none-unknown", "", 0);
  if (arch == Platform::aarch64)
    return std::make_unique<LLVMDisassembler>("aarch64-none-unknown", "cortex-a57", 4);
#elif defined(_M_X86_64)
  if (arch == Platform::x86_64)
    return std::make_unique<BochsDisassembler>();
#else
  (void)arch;
#endif
  return std::make_unique<HostDisassembler>();
}

std::size_t HostDisassembler::Disassemble(const u8* begin, const u8* end, std::ostream& stream)
{
  PrintHex(stream, begin, std::span{begin, end}, "");
  return 0;
}
