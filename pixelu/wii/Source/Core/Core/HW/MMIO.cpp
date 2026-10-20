// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/MMIO.h"

#include <functional>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/MMIOHandlers.h"

namespace MMIO
{
template <typename T>
using ReadFunction = std::function<T(Core::System&, u32)>;
template <typename T>
using WriteFunction = std::function<void(Core::System&, u32, T)>;

// A read method is one of three kinds; the JIT sees the same three through
// the visitor.
template <typename T>
class ReadHandlingMethod
{
public:
  enum class Kind
  {
    Constant,
    Direct,
    Complex,
  };

  static ReadHandlingMethod* MakeConstant(T value)
  {
    auto* method = new ReadHandlingMethod(Kind::Constant);
    method->m_value = value;
    return method;
  }

  static ReadHandlingMethod* MakeDirect(const T* source, u32 mask)
  {
    auto* method = new ReadHandlingMethod(Kind::Direct);
    method->m_source = source;
    method->m_mask = mask;
    return method;
  }

  static ReadHandlingMethod* MakeComplex(ReadFunction<T> function)
  {
    auto* method = new ReadHandlingMethod(Kind::Complex);
    method->m_function = std::move(function);
    return method;
  }

  T Read(Core::System& system, u32 addr) const
  {
    switch (m_kind)
    {
    case Kind::Constant:
      return m_value;
    case Kind::Direct:
      return static_cast<T>(*m_source & m_mask);
    case Kind::Complex:
    default:
      return m_function(system, addr);
    }
  }

  void Accept(ReadHandlingMethodVisitor<T>& visitor) const
  {
    switch (m_kind)
    {
    case Kind::Constant:
      visitor.VisitConstant(m_value);
      break;
    case Kind::Direct:
      visitor.VisitDirect(m_source, m_mask);
      break;
    case Kind::Complex:
      visitor.VisitComplex(&m_function);
      break;
    }
  }

private:
  explicit ReadHandlingMethod(Kind kind) : m_kind(kind) {}

  Kind m_kind;
  T m_value{};
  const T* m_source = nullptr;
  u32 m_mask = 0;
  ReadFunction<T> m_function;
};

template <typename T>
class WriteHandlingMethod
{
public:
  enum class Kind
  {
    Nop,
    Direct,
    Complex,
  };

  static WriteHandlingMethod* MakeNop() { return new WriteHandlingMethod(Kind::Nop); }

  static WriteHandlingMethod* MakeDirect(T* target, u32 mask)
  {
    auto* method = new WriteHandlingMethod(Kind::Direct);
    method->m_target = target;
    method->m_mask = mask;
    return method;
  }

  static WriteHandlingMethod* MakeComplex(WriteFunction<T> function)
  {
    auto* method = new WriteHandlingMethod(Kind::Complex);
    method->m_function = std::move(function);
    return method;
  }

  void Write(Core::System& system, u32 addr, T value) const
  {
    switch (m_kind)
    {
    case Kind::Nop:
      break;
    case Kind::Direct:
      // Bits outside the mask are written as zero, not preserved.
      *m_target = static_cast<T>(value & m_mask);
      break;
    case Kind::Complex:
      m_function(system, addr, value);
      break;
    }
  }

  void Accept(WriteHandlingMethodVisitor<T>& visitor) const
  {
    switch (m_kind)
    {
    case Kind::Nop:
      visitor.VisitNop();
      break;
    case Kind::Direct:
      visitor.VisitDirect(m_target, m_mask);
      break;
    case Kind::Complex:
      visitor.VisitComplex(&m_function);
      break;
    }
  }

private:
  explicit WriteHandlingMethod(Kind kind) : m_kind(kind) {}

  Kind m_kind;
  T* m_target = nullptr;
  u32 m_mask = 0;
  WriteFunction<T> m_function;
};

template <typename T>
ReadHandlingMethod<T>* Constant(T value)
{
  return ReadHandlingMethod<T>::MakeConstant(value);
}

template <typename T>
WriteHandlingMethod<T>* Nop()
{
  return WriteHandlingMethod<T>::MakeNop();
}

template <typename T>
ReadHandlingMethod<T>* DirectRead(const T* addr, u32 mask)
{
  return ReadHandlingMethod<T>::MakeDirect(addr, mask);
}

template <typename T>
WriteHandlingMethod<T>* DirectWrite(T* addr, u32 mask)
{
  return WriteHandlingMethod<T>::MakeDirect(addr, mask);
}

template <typename T>
ReadHandlingMethod<T>* ComplexRead(ReadFunction<T> lambda)
{
  return ReadHandlingMethod<T>::MakeComplex(std::move(lambda));
}

template <typename T>
WriteHandlingMethod<T>* ComplexWrite(WriteFunction<T> lambda)
{
  return WriteHandlingMethod<T>::MakeComplex(std::move(lambda));
}

namespace
{
template <typename T>
T LogInvalidRead(Core::System&, u32 addr)
{
  ERROR_LOG_FMT(MEMMAP, "Unhandled {}-bit MMIO read at {:08x}", 8 * sizeof(T), addr);
  return 0;
}

template <typename T>
void LogInvalidWrite(Core::System&, u32 addr, T val)
{
  ERROR_LOG_FMT(MEMMAP, "Unhandled {}-bit MMIO write at {:08x} (value {:08x})", 8 * sizeof(T),
                addr, val);
}

// Method used by slots nothing was registered for. It lives forever, so the
// function pointer handed to the JIT for such a slot never dangles.
template <typename T>
const ReadHandlingMethod<T>& UnregisteredRead()
{
  static const ReadHandlingMethod<T>* const method =
      ReadHandlingMethod<T>::MakeComplex(&LogInvalidRead<T>);
  return *method;
}

template <typename T>
const WriteHandlingMethod<T>& UnregisteredWrite()
{
  static const WriteHandlingMethod<T>* const method =
      WriteHandlingMethod<T>::MakeComplex(&LogInvalidWrite<T>);
  return *method;
}

template <typename T>
struct Half;
template <>
struct Half<u16>
{
  using type = u8;
};
template <>
struct Half<u32>
{
  using type = u16;
};

template <typename T>
struct Double;
template <>
struct Double<u8>
{
  using type = u16;
};
template <>
struct Double<u16>
{
  using type = u32;
};
}  // namespace

template <typename T>
ReadHandlingMethod<T>* InvalidRead()
{
  return ComplexRead<T>(&LogInvalidRead<T>);
}

template <typename T>
WriteHandlingMethod<T>* InvalidWrite()
{
  return ComplexWrite<T>(&LogInvalidWrite<T>);
}

template <typename T>
ReadHandlingMethod<T>* ReadToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr)
{
  using Part = typename Half<T>::type;
  ReadHandler<Part>* high = &mmio->GetHandlerForRead<Part>(high_part_addr);
  ReadHandler<Part>* low = &mmio->GetHandlerForRead<Part>(low_part_addr);

  return ComplexRead<T>([=](Core::System& system, u32) -> T {
    // The high half (lower address) is read first.
    const T upper = high->Read(system, high_part_addr);
    const T lower = low->Read(system, low_part_addr);
    return static_cast<T>((upper << (8 * sizeof(Part))) | lower);
  });
}

template <typename T>
WriteHandlingMethod<T>* WriteToSmaller(Mapping* mmio, u32 high_part_addr, u32 low_part_addr)
{
  using Part = typename Half<T>::type;
  WriteHandler<Part>* high = &mmio->GetHandlerForWrite<Part>(high_part_addr);
  WriteHandler<Part>* low = &mmio->GetHandlerForWrite<Part>(low_part_addr);

  return ComplexWrite<T>([=](Core::System& system, u32, T val) {
    high->Write(system, high_part_addr, static_cast<Part>(val >> (8 * sizeof(Part))));
    low->Write(system, low_part_addr, static_cast<Part>(val));
  });
}

template <typename T>
ReadHandlingMethod<T>* ReadToLarger(Mapping* mmio, u32 larger_addr, u32 shift)
{
  using Wide = typename Double<T>::type;
  ReadHandler<Wide>* wide = &mmio->GetHandlerForRead<Wide>(larger_addr);

  return ComplexRead<T>([wide, shift](Core::System& system, u32 addr) -> T {
    const u32 aligned = addr & ~static_cast<u32>(sizeof(Wide) - 1);
    return static_cast<T>(wide->Read(system, aligned) >> shift);
  });
}

template <typename T>
ReadHandler<T>::ReadHandler() = default;

template <typename T>
ReadHandler<T>::ReadHandler(ReadHandlingMethod<T>* method) : m_method(method)
{
}

template <typename T>
ReadHandler<T>::~ReadHandler() = default;

template <typename T>
void ReadHandler<T>::Visit(ReadHandlingMethodVisitor<T>& visitor)
{
  (m_method ? *m_method : UnregisteredRead<T>()).Accept(visitor);
}

template <typename T>
T ReadHandler<T>::Read(Core::System& system, u32 addr)
{
  if (!m_method) [[unlikely]]
    return LogInvalidRead<T>(system, addr);
  return m_method->Read(system, addr);
}

template <typename T>
void ReadHandler<T>::ResetMethod(ReadHandlingMethod<T>* method)
{
  m_method.reset(method);
}

template <typename T>
WriteHandler<T>::WriteHandler() = default;

template <typename T>
WriteHandler<T>::WriteHandler(WriteHandlingMethod<T>* method) : m_method(method)
{
}

template <typename T>
WriteHandler<T>::~WriteHandler() = default;

template <typename T>
void WriteHandler<T>::Visit(WriteHandlingMethodVisitor<T>& visitor)
{
  (m_method ? *m_method : UnregisteredWrite<T>()).Accept(visitor);
}

template <typename T>
void WriteHandler<T>::Write(Core::System& system, u32 addr, T val)
{
  if (!m_method) [[unlikely]]
  {
    LogInvalidWrite<T>(system, addr, val);
    return;
  }
  m_method->Write(system, addr, val);
}

template <typename T>
void WriteHandler<T>::ResetMethod(WriteHandlingMethod<T>* method)
{
  m_method.reset(method);
}

// The explicit instantiations promised by MMIOHandlers.h.
#define MMIO_DEFINE_SIZE(T)                                                                        \
  template ReadHandlingMethod<T>* Constant<T>(T);                                                  \
  template WriteHandlingMethod<T>* Nop<T>();                                                       \
  template ReadHandlingMethod<T>* DirectRead<T>(const T*, u32);                                    \
  template WriteHandlingMethod<T>* DirectWrite<T>(T*, u32);                                        \
  template ReadHandlingMethod<T>* ComplexRead<T>(ReadFunction<T>);                                 \
  template WriteHandlingMethod<T>* ComplexWrite<T>(WriteFunction<T>);                              \
  template ReadHandlingMethod<T>* InvalidRead<T>();                                                \
  template WriteHandlingMethod<T>* InvalidWrite<T>();                                              \
  template class ReadHandler<T>;                                                                   \
  template class WriteHandler<T>

MMIO_DEFINE_SIZE(u8);
MMIO_DEFINE_SIZE(u16);
MMIO_DEFINE_SIZE(u32);
#undef MMIO_DEFINE_SIZE

template ReadHandlingMethod<u16>* ReadToSmaller<u16>(Mapping*, u32, u32);
template ReadHandlingMethod<u32>* ReadToSmaller<u32>(Mapping*, u32, u32);
template WriteHandlingMethod<u16>* WriteToSmaller<u16>(Mapping*, u32, u32);
template WriteHandlingMethod<u32>* WriteToSmaller<u32>(Mapping*, u32, u32);
template ReadHandlingMethod<u8>* ReadToLarger<u8>(Mapping*, u32, u32);
template ReadHandlingMethod<u16>* ReadToLarger<u16>(Mapping*, u32, u32);
}  // namespace MMIO
