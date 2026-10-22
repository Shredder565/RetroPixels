// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// A type-erased value holder for the configuration tree (Util/NewConfig.h).
// A value remembers the C++ type it was stored as. Value<T>() reads it back
// as exactly that type; ValueAs<T>() converts:
//   - stored as T: returned as is;
//   - stored as a string, T an integer of 16 bits or more: decimal, or hex
//     written 0x.../-0x.../+0x...;
//   - stored as a string, T bool: true/on/yes and false/off/no (any case),
//     otherwise read as a number (0 or 1);
//   - anything else: written to a stream and read back as T.
// Values may carry a ValueRange, which the configuration GUI uses to know a
// setting's group and its allowed values.

#ifndef INCLUDED_UTIL_GENERICVALUE_H
#define INCLUDED_UTIL_GENERICVALUE_H

#include "Util/Format.h"
#include <cctype>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <variant>
#include <vector>

namespace Util
{
  namespace detail
  {
    // Integer types that ValueAs<T>() parses itself (so hex is accepted).
    // 8-bit types are left to the stream since they read as characters.
    template <typename T>
    struct IntegerEncodableAsHex
    {
      static const bool value = std::is_integral<T>::value && sizeof(T) >= 2 && sizeof(T) <= 8;
    };

    inline int HexDigitValue(char c)
    {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    }

    // Reads "[+|-]0x<hex digits>" into 'bits' (modulo 2^64, negated for '-').
    // Returns false if the text is not entirely of that form.
    inline bool ReadHexInteger(const std::string &text, uint64_t &bits)
    {
      if (text.size() < 3)
        return false;
      size_t at = (text[0] == '+' || text[0] == '-') ? 1 : 0;
      if (text[at] != '0' || (text[at + 1] != 'x' && text[at + 1] != 'X'))
        return false;
      at += 2;
      if (at >= text.size())
        return false;
      uint64_t acc = 0;
      for (; at < text.size(); at++)
      {
        int d = HexDigitValue(text[at]);
        if (d < 0)
          return false;
        acc = (acc << 4) | uint64_t(d);
      }
      bits = text[0] == '-' ? uint64_t(0) - acc : acc;
      return true;
    }

    template <typename T>
    T ReadFromStream(const std::string &text)
    {
      T out{};
      std::istringstream in(text);
      in >> out;
      return out;
    }

    template <typename T>
    typename std::enable_if<IntegerEncodableAsHex<T>::value, T>::type ParseInteger(const std::string &str)
    {
      uint64_t bits;
      if (ReadHexInteger(str, bits))
        return T(bits);
      return ReadFromStream<T>(str);
    }

    // Only instantiated for types that are not parsed here; never called.
    template <typename T>
    typename std::enable_if<!IntegerEncodableAsHex<T>::value, T>::type ParseInteger(const std::string &)
    {
      return T();
    }

    // Only the bool specialization is used.
    template <typename T>
    inline T ParseBool(const std::string &)
    {
      return T();
    }

    template <>
    inline bool ParseBool<bool>(const std::string &str)
    {
      const char *s = str.c_str();
      if (!Util::Stricmp(s, "true") || !Util::Stricmp(s, "on") || !Util::Stricmp(s, "yes"))
        return true;
      if (!Util::Stricmp(s, "false") || !Util::Stricmp(s, "off") || !Util::Stricmp(s, "no"))
        return false;
      return ReadFromStream<bool>(str);
    }
  } // detail

  // A setting's GUI group and allowed values: either a min/max pair or a
  // list of choices.
  class ValueRange
  {
  public:
    using Variant = std::variant<bool, unsigned, int, float, std::string>;

    template <typename T>
    ValueRange(const std::string &group, T min = 0, T max = 0, const std::vector<T> &list = std::vector<T>{})
      : m_group(group), m_min(min), m_max(max)
    {
      for (const auto &item : list)
        m_list.emplace_back(item);
    }

    ValueRange(const std::string &group, const std::vector<std::string> &list = std::vector<std::string>{})
      : m_group(group), m_min(std::string()), m_max(std::string())
    {
      for (const auto &item : list)
        m_list.emplace_back(item);
    }

    // Scratch space where the GUI keeps the value being edited.
    Variant tempValue;

    const std::string &GetGroup() { return m_group; }
    bool HasMinMax() { return m_min != m_max; }
    Variant GetMin() { return m_min; }
    Variant GetMax() { return m_max; }
    std::vector<Variant> &GetList() { return m_list; }

    // Which of the Variant's types the range is for (0 bool ... 4 string).
    int GetIndex() { return int(m_min.index()); }

  private:
    std::string m_group;
    Variant m_min;
    Variant m_max;
    std::vector<Variant> m_list;
  };

  class GenericValue
  {
  public:
    explicit GenericValue(std::type_index type)
      : m_type(type)
    {}

    virtual ~GenericValue() = default;

    std::shared_ptr<ValueRange> GetValueRange()
    {
      return m_range;
    }

    void SetValueRange(std::shared_ptr<ValueRange> range)
    {
      m_range = range;
    }

    template <typename T>
    bool Is() const
    {
      return m_type == std::type_index(typeid(T));
    }

    template <typename T>
    const T &Value() const
    {
      if (!Is<T>())
        throw std::logic_error(Util::Format() << "GenericValue::Value(): cannot get value as " << std::type_index(typeid(T)).name() << " because it is stored as " << m_type.name());
      return *static_cast<const T *>(Data());
    }

    template <typename T>
    T ValueAs() const
    {
      if (Is<T>())
        return *static_cast<const T *>(Data());
      if (Is<std::string>())
      {
        if (detail::IntegerEncodableAsHex<T>::value)
          return detail::ParseInteger<T>(Value<std::string>());
        if (std::is_same<T, bool>::value)
          return detail::ParseBool<T>(Value<std::string>());
      }
      std::ostringstream text;
      Serialize(&text);
      return detail::ReadFromStream<T>(text.str());
    }

    template <typename T>
    void Set(const T &value)
    {
      if (!Is<T>())
        throw std::logic_error(Util::Format() << "GenericValue::Set(): cannot set value as " << std::type_index(typeid(T)).name() << " because it is stored as " << m_type.name());
      *static_cast<T *>(Data()) = value;
    }

    void Set(const char *value)
    {
      Set<std::string>(value);
    }

    // Writes the value with operator<<.
    virtual void Serialize(std::ostream *os) const = 0;

    // A new value of the same type and contents (without the range).
    virtual std::shared_ptr<GenericValue> MakeCopy() const = 0;

  private:
    virtual void *Data() = 0;
    virtual const void *Data() const = 0;

    std::type_index m_type;
    std::shared_ptr<ValueRange> m_range;
  };

  template <typename T>
  struct ValueInstance : public GenericValue
  {
    ValueInstance()
      : GenericValue(std::type_index(typeid(T))), m_data()
    {}

    ValueInstance(const T &data)
      : GenericValue(std::type_index(typeid(T))), m_data(data)
    {}

    ValueInstance(const ValueInstance<T> &other)
      : GenericValue(std::type_index(typeid(T))), m_data(other.m_data)
    {}

    void Serialize(std::ostream *os) const override
    {
      *os << m_data;
    }

    std::shared_ptr<GenericValue> MakeCopy() const override
    {
      return std::make_shared<ValueInstance<T>>(*this);
    }

  private:
    void *Data() override { return &m_data; }
    const void *Data() const override { return &m_data; }

    T m_data;
  };
} // Util

#endif  // INCLUDED_UTIL_GENERICVALUE_H
