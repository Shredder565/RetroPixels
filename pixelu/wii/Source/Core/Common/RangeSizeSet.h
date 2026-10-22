// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <type_traits>
#include <utility>

namespace Common
{
// A RangeSet that also indexes its ranges by size, largest first, so that the largest free
// block (by_size_begin()) or a block of a given size can be found quickly. Used by the JITs to
// track free space in their code buffers. Ranges of equal size are ordered by when they last
// changed (oldest first).
template <typename T>
class RangeSizeSet
{
private:
  template <typename U, bool IsPointer = std::is_pointer_v<U>>
  struct SizeTypeOf
  {
    using type = std::make_unsigned_t<U>;
  };
  template <typename U>
  struct SizeTypeOf<U, true>
  {
    using type = std::size_t;
  };

public:
  // Big enough for the distance between any two values of T.
  using SizeT = typename SizeTypeOf<T>::type;

private:
  struct Entry;
  using MapT = std::map<T, Entry>;
  using SizeMapT = std::multimap<SizeT, typename MapT::iterator, std::greater<SizeT>>;

  struct Entry
  {
    Entry(T to_) : to(to_) {}

    T to;                              // one past the range's last value
    typename SizeMapT::iterator size;  // this range's node in the size index

    bool operator==(const Entry& other) const { return to == other.to; }
    bool operator!=(const Entry& other) const { return to != other.to; }
  };

public:
  struct by_size_const_iterator;

  struct const_iterator
  {
  public:
    const T& from() const { return m_it->first; }
    const T& to() const { return m_it->second.to; }
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

    by_size_const_iterator to_size_iterator()
    {
      return by_size_const_iterator(typename SizeMapT::const_iterator(m_it->second.size));
    }

  private:
    typename MapT::const_iterator m_it;
    const_iterator(typename MapT::const_iterator it) : m_it(it) {}
    friend class RangeSizeSet;
  };

  struct by_size_const_iterator
  {
  public:
    const T& from() const { return m_it->second->first; }
    const T& to() const { return m_it->second->second.to; }

    by_size_const_iterator& operator++()
    {
      ++m_it;
      return *this;
    }
    by_size_const_iterator operator++(int)
    {
      by_size_const_iterator before = *this;
      ++m_it;
      return before;
    }
    by_size_const_iterator& operator--()
    {
      --m_it;
      return *this;
    }
    by_size_const_iterator operator--(int)
    {
      by_size_const_iterator before = *this;
      --m_it;
      return before;
    }

    bool operator==(const by_size_const_iterator& rhs) const { return m_it == rhs.m_it; }
    bool operator!=(const by_size_const_iterator& rhs) const { return m_it != rhs.m_it; }

    const_iterator to_range_iterator() { return const_iterator(m_it->second); }

  private:
    typename SizeMapT::const_iterator m_it;
    by_size_const_iterator(typename SizeMapT::const_iterator it) : m_it(it) {}
    friend class RangeSizeSet;
  };

  // The two indexes point into each other, so copies are not allowed (moves keep the
  // iterators valid).
  RangeSizeSet() = default;
  RangeSizeSet(const RangeSizeSet<T>&) = delete;
  RangeSizeSet(RangeSizeSet<T>&&) = default;
  RangeSizeSet<T>& operator=(const RangeSizeSet<T>&) = delete;
  RangeSizeSet<T>& operator=(RangeSizeSet<T>&&) = default;

  // Adds [from, to), merging with overlapping or adjacent ranges.
  void insert(T from, T to)
  {
    if (!(from < to))
      return;

    auto next = m_ranges.upper_bound(from);

    typename MapT::iterator target = m_ranges.end();
    if (next != m_ranges.begin())
    {
      const auto previous = std::prev(next);
      if (!(previous->second.to < from))
        target = previous;
    }

    T end = to;
    while (next != m_ranges.end() && !(to < next->first))
    {
      if (end < next->second.to)
        end = next->second.to;
      next = Remove(next);
    }

    if (target != m_ranges.end())
    {
      if (target->second.to < end)
        SetEnd(target, end);
    }
    else
    {
      Add(from, end);
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
      else if (from < previous->second.to)
      {
        const T old_end = previous->second.to;
        SetEnd(previous, from);
        if (to < old_end)
        {
          Add(to, old_end);
          return;
        }
      }
    }

    while (it != m_ranges.end() && it->first < to)
    {
      if (to < it->second.to)
      {
        const T old_end = it->second.to;
        Remove(it);
        Add(to, old_end);
        return;
      }
      it = Remove(it);
    }
  }

  const_iterator erase(const_iterator it)
  {
    m_sizes.erase(it.m_it->second.size);
    return const_iterator(m_ranges.erase(it.m_it));
  }

  by_size_const_iterator erase(by_size_const_iterator it)
  {
    m_ranges.erase(it.m_it->second);
    return by_size_const_iterator(m_sizes.erase(it.m_it));
  }

  void clear()
  {
    m_ranges.clear();
    m_sizes.clear();
  }

  bool contains(T value) const
  {
    auto it = m_ranges.upper_bound(value);
    if (it == m_ranges.begin())
      return false;
    --it;
    return !(value < it->first) && value < it->second.to;
  }

  bool overlaps(T from, T to) const
  {
    if (!(from < to))
      return false;
    auto it = m_ranges.lower_bound(to);
    if (it == m_ranges.begin())
      return false;
    --it;
    return from < it->second.to;
  }

  std::size_t size() const { return m_ranges.size(); }
  bool empty() const { return m_ranges.empty(); }

  // Lookups in the size index.
  std::size_t by_size_count(const SizeT& key) const { return m_sizes.count(key); }
  by_size_const_iterator by_size_find(const SizeT& key) const { return m_sizes.find(key); }
  std::pair<by_size_const_iterator, by_size_const_iterator>
  by_size_equal_range(const SizeT& key) const
  {
    const auto [first, last] = m_sizes.equal_range(key);
    return {by_size_const_iterator(first), by_size_const_iterator(last)};
  }
  // Note the descending order: lower_bound(n) is the first range not larger than n.
  by_size_const_iterator by_size_lower_bound(const SizeT& key) const
  {
    return m_sizes.lower_bound(key);
  }
  by_size_const_iterator by_size_upper_bound(const SizeT& key) const
  {
    return m_sizes.upper_bound(key);
  }

  void swap(RangeSizeSet<T>& other)
  {
    m_ranges.swap(other.m_ranges);
    m_sizes.swap(other.m_sizes);
  }

  const_iterator begin() const { return const_iterator(m_ranges.begin()); }
  const_iterator end() const { return const_iterator(m_ranges.end()); }
  const_iterator cbegin() const { return const_iterator(m_ranges.cbegin()); }
  const_iterator cend() const { return const_iterator(m_ranges.cend()); }

  by_size_const_iterator by_size_begin() const { return by_size_const_iterator(m_sizes.begin()); }
  by_size_const_iterator by_size_end() const { return by_size_const_iterator(m_sizes.end()); }
  by_size_const_iterator by_size_cbegin() const { return by_size_const_iterator(m_sizes.cbegin()); }
  by_size_const_iterator by_size_cend() const { return by_size_const_iterator(m_sizes.cend()); }

  bool operator==(const RangeSizeSet<T>& other) const { return m_ranges == other.m_ranges; }
  bool operator!=(const RangeSizeSet<T>& other) const { return !(*this == other); }

  // Total size, and the share of it outside the largest range (1.0 when empty).
  std::pair<std::size_t, double> get_stats() const
  {
    if (m_ranges.empty())
      return {0, 1.0};
    std::size_t total = 0;
    for (const auto& [first, entry] : m_ranges)
      total += Length(first, entry.to);
    const std::size_t largest = m_sizes.begin()->first;
    return {total, static_cast<double>(total - largest) / total};
  }

private:
  static SizeT Length(T from, T to)
  {
    if constexpr (std::is_pointer_v<T>)
    {
      static_assert(sizeof(T) <= sizeof(SizeT));
      return reinterpret_cast<SizeT>(to) - reinterpret_cast<SizeT>(from);
    }
    else
    {
      return static_cast<SizeT>(to - from);
    }
  }

  typename MapT::iterator Add(T from, T to)
  {
    const auto range = m_ranges.emplace(from, to).first;
    range->second.size = m_sizes.emplace(Length(from, to), range);
    return range;
  }

  typename MapT::iterator Remove(typename MapT::iterator range)
  {
    m_sizes.erase(range->second.size);
    return m_ranges.erase(range);
  }

  // Changes the end of a range and moves it to its new place in the size index.
  void SetEnd(typename MapT::iterator range, T to)
  {
    range->second.to = to;
    m_sizes.erase(range->second.size);
    range->second.size = m_sizes.emplace(Length(range->first, to), range);
  }

  MapT m_ranges;
  SizeMapT m_sizes;
};
}  // namespace Common
