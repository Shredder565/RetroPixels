// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <map>
#include <type_traits>
#include <utility>

namespace Common
{
// A set of values stored as half-open ranges [from, to). Overlapping or adjacent ranges are
// merged on insertion, so the stored ranges are always sorted, disjoint and separated by gaps.
template <typename T>
class RangeSet
{
private:
  // Key: first value of a range; mapped value: one past its last value.
  using MapT = std::map<T, T>;

public:
  struct const_iterator
  {
  public:
    const T& from() const { return m_it->first; }
    const T& to() const { return m_it->second; }
    std::pair<T, T> operator*() { return {from(), to()}; }

    const_iterator& operator++()
    {
      ++m_it;
      return *this;
    }
    const_iterator operator++(int)
    {
      const_iterator before = *this;
      ++m_it;
      return before;
    }
    const_iterator& operator--()
    {
      --m_it;
      return *this;
    }
    const_iterator operator--(int)
    {
      const_iterator before = *this;
      --m_it;
      return before;
    }

    bool operator==(const const_iterator& rhs) const { return m_it == rhs.m_it; }
    bool operator!=(const const_iterator& rhs) const { return m_it != rhs.m_it; }

  private:
    typename MapT::const_iterator m_it;
    const_iterator(typename MapT::const_iterator it) : m_it(it) {}
    friend class RangeSet;
  };

  // Adds [from, to).
  void insert(T from, T to)
  {
    if (!(from < to))
      return;

    auto next = m_ranges.upper_bound(from);

    // A range starting at or before `from` that reaches it absorbs the new values.
    typename MapT::iterator target = m_ranges.end();
    if (next != m_ranges.begin())
    {
      const auto previous = std::prev(next);
      if (!(previous->second < from))
        target = previous;
    }

    // Swallow every following range that starts inside or right after [from, to).
    T end = to;
    while (next != m_ranges.end() && !(to < next->first))
    {
      if (end < next->second)
        end = next->second;
      next = m_ranges.erase(next);
    }

    if (target != m_ranges.end())
    {
      if (target->second < end)
        target->second = end;
    }
    else
    {
      m_ranges.emplace_hint(next, from, end);
    }
  }

  // Removes [from, to).
  void erase(T from, T to)
  {
    if (!(from < to))
      return;

    auto it = m_ranges.upper_bound(from);
    if (it != m_ranges.begin())
    {
      const auto previous = std::prev(it);
      if (previous->first == from)
      {
        it = previous;
      }
      else if (from < previous->second)
      {
        // The range starts before `from`: keep its head, and its tail if it extends past `to`.
        const T old_end = previous->second;
        previous->second = from;
        if (to < old_end)
        {
          m_ranges.emplace_hint(it, to, old_end);
          return;
        }
      }
    }

    while (it != m_ranges.end() && it->first < to)
    {
      if (to < it->second)
      {
        // Only the head of this range is removed.
        const T old_end = it->second;
        it = m_ranges.erase(it);
        m_ranges.emplace_hint(it, to, old_end);
        return;
      }
      it = m_ranges.erase(it);
    }
  }

  const_iterator erase(const_iterator it) { return const_iterator(m_ranges.erase(it.m_it)); }

  void clear() { m_ranges.clear(); }

  bool contains(T value) const
  {
    auto it = m_ranges.upper_bound(value);
    if (it == m_ranges.begin())
      return false;
    --it;
    return !(value < it->first) && value < it->second;
  }

  // Whether any value of [from, to) is in the set.
  bool overlaps(T from, T to) const
  {
    if (!(from < to))
      return false;
    auto it = m_ranges.lower_bound(to);  // first range that starts at or after `to`
    if (it == m_ranges.begin())
      return false;
    --it;
    return from < it->second;
  }

  std::size_t size() const { return m_ranges.size(); }
  bool empty() const { return m_ranges.empty(); }
  void swap(RangeSet<T>& other) { m_ranges.swap(other.m_ranges); }

  const_iterator begin() const { return const_iterator(m_ranges.begin()); }
  const_iterator end() const { return const_iterator(m_ranges.end()); }
  const_iterator cbegin() const { return const_iterator(m_ranges.cbegin()); }
  const_iterator cend() const { return const_iterator(m_ranges.cend()); }

  bool operator==(const RangeSet<T>& other) const { return m_ranges == other.m_ranges; }
  bool operator!=(const RangeSet<T>& other) const { return !(*this == other); }

  // Total size of all ranges, and the fragmentation ratio (the share of the total that is not
  // in the largest range; 1.0 for an empty set).
  std::pair<std::size_t, double> get_stats() const
  {
    if (m_ranges.empty())
      return {0, 1.0};
    std::size_t total = 0;
    std::size_t largest = 0;
    for (const auto& [first, last] : m_ranges)
    {
      const std::size_t length = Length(first, last);
      total += length;
      if (length > largest)
        largest = length;
    }
    return {total, static_cast<double>(total - largest) / total};
  }

private:
  static std::size_t Length(T from, T to)
  {
    if constexpr (std::is_pointer_v<T>)
      return reinterpret_cast<std::size_t>(to) - reinterpret_cast<std::size_t>(from);
    else
      return static_cast<std::size_t>(to - from);
  }

  MapT m_ranges;
};
}  // namespace Common
