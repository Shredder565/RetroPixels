// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Text form of the cached interpreter's records, used by the JIT block viewer and code logging.

#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

#include <fmt/ostream.h>

#include "Common/Assert.h"
#include "Core/HLE/HLE.h"

namespace
{
template <class T>
constexpr s32 RecordSize()
{
  return static_cast<s32>(sizeof(void*) + sizeof(T));
}
}  // namespace

s32 CachedInterpreterEmitter::PoisonCallback(std::ostream& stream, const void*)
{
  stream << "PoisonCallback()\n";
  return sizeof(AnyCallback);
}

s32 CachedInterpreter::StartProfiledBlock(std::ostream& stream,
                                          const StartProfiledBlockOperands&)
{
  stream << "StartProfiledBlock()\n";
  return RecordSize<StartProfiledBlockOperands>();
}

template <bool profiled>
s32 CachedInterpreter::EndBlock(std::ostream& stream, const EndBlockOperands<profiled>& operands)
{
  fmt::println(stream, "EndBlock<profiled={}>(downcount={}, num_load_stores={}, num_fp_inst={})",
               profiled, operands.downcount, operands.num_load_stores, operands.num_fp_inst);
  return RecordSize<EndBlockOperands<profiled>>();
}

template <bool write_pc>
s32 CachedInterpreter::Interpret(std::ostream& stream, const InterpretOperands& operands)
{
  fmt::println(stream, "Interpret<write_pc={:5}>(current_pc=0x{:08x}, inst=0x{:08x})", write_pc,
               operands.current_pc, operands.inst.hex);
  return RecordSize<InterpretOperands>();
}

template <bool write_pc>
s32 CachedInterpreter::InterpretAndCheckExceptions(
    std::ostream& stream, const InterpretAndCheckExceptionsOperands& operands)
{
  fmt::println(stream,
               "InterpretAndCheckExceptions<write_pc={:5}>(current_pc=0x{:08x}, inst=0x{:08x}, "
               "downcount={})",
               write_pc, operands.current_pc, operands.inst.hex, operands.downcount);
  return RecordSize<InterpretAndCheckExceptionsOperands>();
}

s32 CachedInterpreter::HLEFunction(std::ostream& stream, const HLEFunctionOperands& operands)
{
  fmt::println(stream, "HLEFunction(current_pc=0x{:08x}, hook_index={}) [\"{}\"]",
               operands.current_pc, operands.hook_index,
               HLE::GetHookNameByIndex(operands.hook_index));
  return RecordSize<HLEFunctionOperands>();
}

s32 CachedInterpreter::WriteBrokenBlockNPC(std::ostream& stream,
                                           const WriteBrokenBlockNPCOperands& operands)
{
  fmt::println(stream, "WriteBrokenBlockNPC(current_pc=0x{:08x})", operands.current_pc);
  return RecordSize<WriteBrokenBlockNPCOperands>();
}

s32 CachedInterpreter::CheckFPU(std::ostream& stream, const CheckHaltOperands& operands)
{
  fmt::println(stream, "CheckFPU(current_pc=0x{:08x}, downcount={})", operands.current_pc,
               operands.downcount);
  return RecordSize<CheckHaltOperands>();
}

s32 CachedInterpreter::CheckBreakpoint(std::ostream& stream, const CheckHaltOperands& operands)
{
  fmt::println(stream, "CheckBreakpoint(current_pc=0x{:08x}, downcount={})", operands.current_pc,
               operands.downcount);
  return RecordSize<CheckHaltOperands>();
}

s32 CachedInterpreter::CheckIdle(std::ostream& stream, const CheckIdleOperands& operands)
{
  fmt::println(stream, "CheckIdle(idle_pc=0x{:08x})", operands.idle_pc);
  return RecordSize<CheckIdleOperands>();
}

std::size_t CachedInterpreter::Disassemble(const JitBlock& block, std::ostream& stream)
{
  struct Entry
  {
    AnyCallback run;
    AnyDisassemble print;
  };
#define RECORD_KIND(...) Entry{AnyCallbackCast(__VA_ARGS__), AnyDisassembleCast(__VA_ARGS__)}
  // Callback addresses are only known at run time, so the table is sorted on first use.
  static std::array<Entry, 13> table{
      RECORD_KIND(CachedInterpreter::PoisonCallback),
      RECORD_KIND(CachedInterpreter::StartProfiledBlock),
      RECORD_KIND(CachedInterpreter::EndBlock<false>),
      RECORD_KIND(CachedInterpreter::EndBlock<true>),
      RECORD_KIND(CachedInterpreter::Interpret<false>),
      RECORD_KIND(CachedInterpreter::Interpret<true>),
      RECORD_KIND(CachedInterpreter::InterpretAndCheckExceptions<false>),
      RECORD_KIND(CachedInterpreter::InterpretAndCheckExceptions<true>),
      RECORD_KIND(CachedInterpreter::HLEFunction),
      RECORD_KIND(CachedInterpreter::WriteBrokenBlockNPC),
      RECORD_KIND(CachedInterpreter::CheckFPU),
      RECORD_KIND(CachedInterpreter::CheckBreakpoint),
      RECORD_KIND(CachedInterpreter::CheckIdle),
  };
#undef RECORD_KIND
  static std::once_flag sorted;
  std::call_once(sorted, [] {
    std::ranges::sort(table, {}, &Entry::run);
    ASSERT_MSG(DYNA_REC, std::ranges::adjacent_find(table, {}, &Entry::run) == table.end(),
               "Two record kinds share a callback.");
  });

  std::size_t printed = 0;
  const u8* record = block.normalEntry;
  while (record != block.near_end)
  {
    AnyCallback callback;
    std::memcpy(&callback, record, sizeof(callback));
    const auto it = std::ranges::lower_bound(table, callback, {}, &Entry::run);
    if (it == table.end() || it->run != callback)
    {
      stream << "UNKNOWN OR ILLEGAL CALLBACK\n";
      break;
    }
    record += it->print(stream, record + sizeof(AnyCallback));
    ++printed;
  }
  return printed;
}
