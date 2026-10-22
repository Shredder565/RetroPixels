// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Building blocks for MMIO register handlers.
//
// Each MMIO address has, per access size (8, 16 or 32 bits), a read handler
// and a write handler. A handler runs one "handling method":
//
//   Constant     reads always return the same value
//   Nop          writes are ignored silently
//   Direct       the register is a variable in memory (read/written with a mask)
//   Complex      a function decides what a read returns or a write does
//   Invalid      logs an error (reads return 0); the default for every address
//
// The JIT inspects read methods through ReadHandlingMethodVisitor to inline
// constant and direct reads, so a register should use the simplest method
// that describes it.

#include <functional>
#include <memory>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace MMIO
{
class Mapping;

// Defined in MMIO.cpp; only handled through pointers outside of it.
template <typename T>
class ReadHandlingMethod;
template <typename T>
class WriteHandlingMethod;

// Factory functions. The returned object is owned by whichever handler it is
// registered with.
template <typename T>
ReadHandlingMethod<T>* Constant(T value);

template <typename T>
WriteHandlingMethod<T>* Nop();

template <typename T>
ReadHandlingMethod<T>* DirectRead(const T* addr, u32 mask = 0xFFFFFFFF);
template <typename T>
WriteHandlingMethod<T>* DirectWrite(T* addr, u32 mask = 0xFFFFFFFF);

template <typename T>
ReadHandlingMethod<T>* ComplexRead(std::function<T(Core::System&, u32)>);
template <typename T>
WriteHandlingMethod<T>* ComplexWrite(std::function<void(Core::System&, u32, T)>);

template <typename T>
ReadHandlingMethod<T>* InvalidRead();
template <typename T>
WriteHandlingMethod<T>* InvalidWrite();

// Size adapters, so a block can describe its registers at one size and have
// the other sizes forwarded:
//  - ReadToSmaller/WriteToSmaller (T = u16 or u32) split an access into two
//    accesses of half the size, high half first.
//  - ReadToLarger (T = u8 or u16) reads the enclosing access of twice the
//    size and shifts the wanted part down.
// They forward to whatever handlers are registered at the time of the access.
template <typename T>
ReadHandlingMethod<T>* ReadToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr);
template <typename T>
WriteHandlingMethod<T>* WriteToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr);
template <typename T>
ReadHandlingMethod<T>* ReadToLarger(Mapping* mmio, u32 larger_addr, u32 shift);

// Inspection of a handler's method (used by the JIT to generate code).
template <typename T>
class ReadHandlingMethodVisitor
{
public:
  virtual void VisitConstant(T value) = 0;
  virtual void VisitDirect(const T* addr, u32 mask) = 0;
  // The pointed-to function stays valid as long as the method stays registered.
  virtual void VisitComplex(const std::function<T(Core::System&, u32)>* lambda) = 0;
};

template <typename T>
class WriteHandlingMethodVisitor
{
public:
  virtual void VisitNop() = 0;
  virtual void VisitDirect(T* addr, u32 mask) = 0;
  virtual void VisitComplex(const std::function<void(Core::System&, u32, T)>* lambda) = 0;
};

// One read slot of the MMIO map. Internal to the MMIO code; it is only in
// this header so Mapping can hold arrays of them.
template <typename T>
class ReadHandler
{
public:
  ReadHandler();
  // Takes ownership of `method`.
  ReadHandler(ReadHandlingMethod<T>* method);
  ~ReadHandler();

  void Visit(ReadHandlingMethodVisitor<T>& visitor);
  T Read(Core::System& system, u32 addr);

  // Replaces the method (taking ownership of the new one).
  void ResetMethod(ReadHandlingMethod<T>* method);

private:
  // Null until something is registered; an empty slot behaves as Invalid.
  std::unique_ptr<ReadHandlingMethod<T>> m_method;
};

// One write slot of the MMIO map.
template <typename T>
class WriteHandler
{
public:
  WriteHandler();
  // Takes ownership of `method`.
  WriteHandler(WriteHandlingMethod<T>* method);
  ~WriteHandler();

  void Visit(WriteHandlingMethodVisitor<T>& visitor);
  void Write(Core::System& system, u32 addr, T val);

  // Replaces the method (taking ownership of the new one).
  void ResetMethod(WriteHandlingMethod<T>* method);

private:
  // Null until something is registered; an empty slot behaves as Invalid.
  std::unique_ptr<WriteHandlingMethod<T>> m_method;
};

// The templates above are instantiated once, in MMIO.cpp, for u8, u16 and u32.
#define MMIO_DECLARE_SIZE(T)                                                                       \
  extern template ReadHandlingMethod<T>* Constant<T>(T);                                           \
  extern template WriteHandlingMethod<T>* Nop<T>();                                                \
  extern template ReadHandlingMethod<T>* DirectRead<T>(const T*, u32);                             \
  extern template WriteHandlingMethod<T>* DirectWrite<T>(T*, u32);                                 \
  extern template ReadHandlingMethod<T>* ComplexRead<T>(std::function<T(Core::System&, u32)>);    \
  extern template WriteHandlingMethod<T>* ComplexWrite<T>(                                         \
      std::function<void(Core::System&, u32, T)>);                                                 \
  extern template ReadHandlingMethod<T>* InvalidRead<T>();                                         \
  extern template WriteHandlingMethod<T>* InvalidWrite<T>();                                       \
  extern template class ReadHandler<T>;                                                            \
  extern template class WriteHandler<T>

MMIO_DECLARE_SIZE(u8);
MMIO_DECLARE_SIZE(u16);
MMIO_DECLARE_SIZE(u32);
#undef MMIO_DECLARE_SIZE

extern template ReadHandlingMethod<u16>* ReadToSmaller<u16>(Mapping*, u32, u32);
extern template ReadHandlingMethod<u32>* ReadToSmaller<u32>(Mapping*, u32, u32);
extern template WriteHandlingMethod<u16>* WriteToSmaller<u16>(Mapping*, u32, u32);
extern template WriteHandlingMethod<u32>* WriteToSmaller<u32>(Mapping*, u32, u32);
extern template ReadHandlingMethod<u8>* ReadToLarger<u8>(Mapping*, u32, u32);
extern template ReadHandlingMethod<u16>* ReadToLarger<u16>(Mapping*, u32, u32);
}  // namespace MMIO
