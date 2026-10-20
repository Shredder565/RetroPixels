// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// PointerWrap: symmetric (de)serialisation into a flat byte buffer, used for savestates.
//
// The same Do() calls are run for every mode, so one function both saves and loads an object:
//   Write   copies values into the buffer,
//   Read    copies them back out,
//   Measure only advances the position (to size a buffer before writing),
//   Verify  compares the values with the buffer (debug builds).
// Running past the end of the buffer switches the wrap to Measure mode, which callers check to
// detect failure. The format is raw native-endian memory with u32 counts in front of containers;
// there is no versioning beyond what callers add.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Flag.h"
#include "Common/Inline.h"

class PointerWrap
{
public:
  enum class Mode
  {
    Read,
    Write,
    Measure,
    Verify,
  };

private:
  u8** m_ptr_current;
  u8* m_ptr_end;
  Mode m_mode;

public:
  // *ptr is the buffer position; it is advanced as data is processed.
  PointerWrap(u8** ptr, size_t size, Mode mode)
      : m_ptr_current(ptr), m_ptr_end(*ptr + size), m_mode(mode)
  {
  }

  void SetMeasureMode() { m_mode = Mode::Measure; }
  void SetVerifyMode() { m_mode = Mode::Verify; }
  bool IsReadMode() const { return m_mode == Mode::Read; }
  bool IsWriteMode() const { return m_mode == Mode::Write; }
  bool IsMeasureMode() const { return m_mode == Mode::Measure; }
  bool IsVerifyMode() const { return m_mode == Mode::Verify; }

  // ---- Containers: u32 element count, then the elements --------------------------------------

  template <typename K, class V>
  void Do(std::map<K, V>& x)
  {
    u32 count = static_cast<u32>(x.size());
    Do(count);
    if (IsReadMode())
    {
      x.clear();
      for (; count != 0; --count)
      {
        std::pair<K, V> entry;
        Do(entry.first);
        Do(entry.second);
        x.insert(std::move(entry));
      }
    }
    else
    {
      for (auto& [key, value] : x)
      {
        Do(const_cast<K&>(key));  // only read from in these modes
        Do(value);
      }
    }
  }

  template <typename V>
  void Do(std::set<V>& x)
  {
    u32 count = static_cast<u32>(x.size());
    Do(count);
    if (IsReadMode())
    {
      x.clear();
      for (; count != 0; --count)
      {
        V value = {};
        Do(value);
        x.insert(std::move(value));
      }
    }
    else
    {
      for (const V& value : x)
        Do(const_cast<V&>(value));  // only read from in these modes
    }
  }

  template <typename T>
  void Do(std::vector<T>& x)
  {
    DoContiguousContainer(x);
  }

  template <typename T>
  void Do(std::list<T>& x)
  {
    DoContainer(x);
  }

  template <typename T>
  void Do(std::deque<T>& x)
  {
    DoContainer(x);
  }

  template <typename T>
  void Do(std::basic_string<T>& x)
  {
    DoContiguousContainer(x);
  }

  template <typename T, typename U>
  void Do(std::pair<T, U>& x)
  {
    Do(x.first);
    Do(x.second);
  }

  // A presence flag (one byte), then the value if present.
  template <typename T>
  void Do(std::optional<T>& x)
  {
    bool present = x.has_value();
    Do(present);
    if (IsReadMode())
    {
      if (!present)
      {
        x.reset();
        return;
      }
      x.emplace();
    }
    if (present)
      Do(*x);
  }

  // ---- Fixed-size arrays (no count) ----------------------------------------------------------

  template <typename T, std::size_t N>
  void DoArray(std::array<T, N>& x)
  {
    DoArray(x.data(), static_cast<u32>(N));
  }

  template <typename V, auto last_member, typename = decltype(last_member)>
  void DoArray(Common::EnumMap<V, last_member>& x)
  {
    DoArray(x.data(), static_cast<u32>(x.size()));
  }

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  void DoArray(T* x, u32 count)
  {
    DoVoid(x, count * sizeof(T));
  }

  template <typename T>
  requires(!std::is_trivially_copyable_v<T>)
  void DoArray(T* x, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      Do(x[i]);
  }

  template <typename T, std::size_t N>
  void DoArray(T (&arr)[N])
  {
    DoArray(arr, static_cast<u32>(N));
  }

  // ---- Raw access ----------------------------------------------------------------------------

  // Processes `count` (a u32), then skips `count` bytes and returns where they start. The caller
  // copies the data itself, depending on the mode.
  [[nodiscard]] u8* DoExternal(u32& count)
  {
    Do(count);
    u8* const start = *m_ptr_current;
    *m_ptr_current += count;
    if (!IsMeasureMode() && *m_ptr_current > m_ptr_end)
      SetMeasureMode();
    return start;
  }

  // Processes a u32 zero and returns its position, so the caller can patch the value in later.
  [[nodiscard]] u8* ReserveU32()
  {
    u8* const position = *m_ptr_current;
    u32 placeholder = 0;
    Do(placeholder);
    return position;
  }

  u32 GetOffsetFromPreviousPosition(u8* previous_pointer)
  {
    return static_cast<u32>(*m_ptr_current - previous_pointer);
  }

  // ---- Single values -------------------------------------------------------------------------

  void Do(Common::Flag& flag)
  {
    bool value = flag.IsSet();
    Do(value);
    if (IsReadMode())
      flag.Set(value);
  }

  template <typename T>
  void Do(std::atomic<T>& atomic)
  {
    T value = atomic.load(std::memory_order_relaxed);
    Do(value);
    if (IsReadMode())
      atomic.store(value, std::memory_order_relaxed);
  }

  // Any trivially copyable value is stored as its raw bytes (this also covers unions containing
  // BitFields, which can't simply be assigned). T may be const: save-only code passes const
  // values, which are only read from in Write, Measure and Verify modes.
  template <typename T>
  requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
  void Do(T& x)
  {
    DoVoid(const_cast<void*>(static_cast<const volatile void*>(std::addressof(x))), sizeof(x));
  }

  // Always one byte, whatever sizeof(bool) is.
  void Do(bool& x)
  {
    u8 byte = static_cast<u8>(x);
    Do(byte);
    if (IsReadMode())
      x = byte != 0;
  }

  // Stores a pointer into [base, base + count] as an offset. On load, an out-of-range offset
  // fails (and switches to Measure mode).
  template <typename T>
  bool DoPointer(T*& x, T* const base, size_t count)
  {
    ptrdiff_t offset = x - base;
    Do(offset);
    if (IsReadMode())
    {
      if (offset < 0 || static_cast<size_t>(offset) > count)
      {
        SetMeasureMode();
        return false;
      }
      x = base + offset;
    }
    return true;
  }

  bool DoPointer(void*& x, void* const base, size_t count)
  {
    u8* byte_pointer = static_cast<u8*>(x);
    if (!DoPointer(byte_pointer, static_cast<u8*>(base), count))
      return false;
    x = byte_pointer;
    return true;
  }

  // A u32 cookie that must read back unchanged; catches sections that went out of sync.
  void DoMarker(const std::string& prevName, u32 arbitraryNumber = 0x42)
  {
    u32 cookie = arbitraryNumber;
    Do(cookie);
    if (IsReadMode() && cookie != arbitraryNumber)
    {
      PanicAlertFmtT(
          "Error: After \"{0}\", found {1} ({2:#x}) instead of save marker {3} ({4:#x}). Aborting "
          "savestate load...",
          prevName, cookie, cookie, arbitraryNumber, arbitraryNumber);
      SetMeasureMode();
    }
  }

  // u32 size, then `member(*this, element)` for each element (the container is resized first).
  template <typename T, typename Functor>
  void DoEachElement(T& container, Functor member)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);
    for (auto& element : container)
      member(*this, element);
  }

private:
  template <typename T>
  void DoContiguousContainer(T& container)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);
    if (size != 0)
      DoArray(container.data(), size);
  }

  template <typename T>
  void DoContainer(T& x)
  {
    DoEachElement(x, [](PointerWrap& p, typename T::value_type& element) { p.Do(element); });
  }

  DOLPHIN_FORCE_INLINE void DoVoid(void* data, u32 size)
  {
    if (!IsMeasureMode() && *m_ptr_current + size > m_ptr_end)
      SetMeasureMode();  // would overrun the buffer

    switch (m_mode)
    {
    case Mode::Read:
      std::memcpy(data, *m_ptr_current, size);
      break;
    case Mode::Write:
      std::memcpy(*m_ptr_current, data, size);
      break;
    case Mode::Measure:
      break;
    case Mode::Verify:
      DEBUG_ASSERT_MSG(COMMON, !std::memcmp(data, *m_ptr_current, size),
                       "Savestate verification failure: buf {} != {} (size {}).\n", fmt::ptr(data),
                       fmt::ptr(*m_ptr_current), size);
      break;
    }
    *m_ptr_current += size;
  }
};
