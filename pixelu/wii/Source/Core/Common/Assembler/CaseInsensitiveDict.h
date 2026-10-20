// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Common::GekkoAssembler::detail
{
// A small read-only map from names to values that ignores ASCII letter case.
//
// Keys may only contain ASCII letters, digits and the characters listed in `Extra`. Looking up a
// string that contains any other character always fails, which lets callers pass raw token text
// (for example "bt+") without pre-filtering it.
template <typename V, char... Extra>
class CaseInsensitiveDict
{
public:
  CaseInsensitiveDict(std::initializer_list<std::pair<std::string_view, V>> entries)
  {
    m_entries.reserve(entries.size());
    for (const auto& [key, value] : entries)
    {
      std::string folded;
      if (Fold(key, &folded))
        m_entries.emplace_back(std::move(folded), value);
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
  }

  template <typename T>
  const V* Find(const T& key) const
  {
    std::string folded;
    if (!Fold(std::string_view(key), &folded))
      return nullptr;
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), folded,
                                     [](const auto& e, const std::string& k) { return e.first < k; });
    if (it == m_entries.end() || it->first != folded)
      return nullptr;
    return &it->second;
  }

private:
  static bool Allowed(char c)
  {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      return true;
    return ((c == Extra) || ...);
  }

  static bool Fold(std::string_view in, std::string* out)
  {
    out->clear();
    out->reserve(in.size());
    for (const char c : in)
    {
      if (!Allowed(c))
        return false;
      *out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return true;
  }

  std::vector<std::pair<std::string, V>> m_entries;
};
}  // namespace Common::GekkoAssembler::detail
