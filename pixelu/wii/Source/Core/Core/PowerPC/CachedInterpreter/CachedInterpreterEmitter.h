// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The cached interpreter's "code" is a packed list of records in its code region. Each record is
// a callback pointer followed by that callback's operand struct. Running a block calls the
// callbacks in order. Each callback returns how many bytes to move forward to reach the next
// record, or 0 to leave the block.

#pragma once

#include <bit>
#include <cstddef>
#include <iosfwd>
#include <type_traits>

#include "Common/CodeBlock.h"
#include "Common/CommonTypes.h"

namespace PowerPC
{
struct PowerPCState;
}

class CachedInterpreterEmitter
{
protected:
  // Execution form of a record. The result is a signed byte distance (s32 is the cheapest width
  // to return), and 0 means the block is finished.
  template <class Operands>
  using Callback = s32 (*)(PowerPC::PowerPCState& ppc_state, const Operands& operands);
  using AnyCallback = s32 (*)(PowerPC::PowerPCState& ppc_state, const void* operands);

  // Selects one overload of an overloaded callback name at compile time.
  template <class Operands>
  static consteval Callback<Operands> CallbackCast(Callback<Operands> callback)
  {
    return callback;
  }
  // Erases the operand type so the callback can be stored in the code region.
  template <class Operands>
  static AnyCallback AnyCallbackCast(Callback<Operands> callback)
  {
    return std::bit_cast<AnyCallback>(callback);
  }
  static consteval AnyCallback AnyCallbackCast(AnyCallback callback) { return callback; }

  // Printing form of a record, used by the disassembler. It always returns the record's size.
  template <class Operands>
  using Disassemble = s32 (*)(std::ostream& stream, const Operands& operands);
  using AnyDisassemble = s32 (*)(std::ostream& stream, const void* operands);

  template <class Operands>
  static AnyDisassemble AnyDisassembleCast(Disassemble<Operands> disassemble)
  {
    return std::bit_cast<AnyDisassemble>(disassemble);
  }
  static consteval AnyDisassemble AnyDisassembleCast(AnyDisassemble disassemble)
  {
    return disassemble;
  }

public:
  CachedInterpreterEmitter() = default;
  explicit CachedInterpreterEmitter(u8* begin, u8* end) : m_code(begin), m_code_end(end) {}

  // Appends one record. Operands are copied byte-wise, so they must be trivially copyable. Their
  // size and alignment must keep the next callback pointer aligned.
  template <class Operands>
  void Write(Callback<Operands> callback, const Operands& operands)
  {
    static_assert(std::is_trivially_copyable_v<Operands>);
    static_assert(std::is_trivially_destructible_v<Operands>);
    static_assert(alignof(Operands) <= alignof(AnyCallback));
    static_assert(sizeof(Operands) % alignof(AnyCallback) == 0);
    Write(AnyCallbackCast(callback), &operands, sizeof(Operands));
  }
  // Appends a record that has no operands.
  void Write(AnyCallback callback) { Write(callback, nullptr, 0); }

  const u8* GetCodePtr() const { return m_code; }
  u8* GetWritableCodePtr() { return m_code; }
  const u8* GetCodeEnd() const { return m_code_end; }
  u8* GetWritableCodeEnd() { return m_code_end; }

  // True once a record did not fit. The block being built must then be thrown away.
  bool HasWriteFailed() const { return m_write_failed; }

  // Points the emitter at a new range and clears the failure flag.
  void SetCodePtr(u8* begin, u8* end)
  {
    m_code = begin;
    m_code_end = end;
    m_write_failed = false;
  }

  // The callback that fills unused and freed code memory. Reaching it is a bug.
  static s32 PoisonCallback(PowerPC::PowerPCState& ppc_state, const void* operands);
  static s32 PoisonCallback(std::ostream& stream, const void* operands);

private:
  void Write(AnyCallback callback, const void* operands, std::size_t size);

  u8* m_code = nullptr;
  u8* m_code_end = nullptr;
  bool m_write_failed = false;
};

// The cached interpreter's code region.
class CachedInterpreterCodeBlock : public Common::CodeBlock<CachedInterpreterEmitter, false>
{
private:
  void PoisonMemory() override;
};
