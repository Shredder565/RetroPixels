// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <utility>

#include "Common/TypeUtils.h"

namespace Common
{
namespace detail
{
// Unbounded lock-free queue for exactly one producer thread and one consumer thread.
//
// It is a singly linked list that always ends in an empty "tail" node. The producer fills the
// tail node, appends a new empty tail and then publishes the new element through the atomic
// count; the consumer only follows `next` pointers of nodes the count says are complete.
template <typename T, bool IncludeWaitFunctionality>
class SPSCQueueBase final
{
public:
  SPSCQueueBase() = default;
  ~SPSCQueueBase()
  {
    Clear();
    delete m_head;
  }

  SPSCQueueBase(const SPSCQueueBase&) = delete;
  SPSCQueueBase& operator=(const SPSCQueueBase&) = delete;

  std::size_t Size() const { return m_count.load(std::memory_order_acquire); }
  bool Empty() const { return Size() == 0; }

  // Producer side.
  void Push(const T& arg) { Emplace(arg); }
  void Push(T&& arg) { Emplace(std::move(arg)); }

  template <typename... Args>
  void Emplace(Args&&... args)
  {
    Node* const filled = m_tail;
    filled->item.Construct(std::forward<Args>(args)...);
    filled->next = new Node;
    m_tail = filled->next;

    m_count.fetch_add(1, std::memory_order_release);
    if constexpr (IncludeWaitFunctionality)
      m_count.notify_one();
  }

  // Blocks until the consumer has taken everything.
  void WaitForEmpty() requires(IncludeWaitFunctionality)
  {
    for (std::size_t seen = Size(); seen != 0; seen = Size())
      m_count.wait(seen, std::memory_order_acquire);
  }

  // Consumer side.
  T& Front() { return m_head->item.Ref(); }
  const T& Front() const { return m_head->item.Ref(); }

  void Pop()
  {
    assert(!Empty());
    Node* const done = m_head;
    m_head = done->next;
    done->item.Destroy();
    delete done;

    m_count.fetch_sub(1, std::memory_order_release);
    if constexpr (IncludeWaitFunctionality)
      m_count.notify_one();
  }

  bool Pop(T& result)
  {
    if (Empty())
      return false;
    result = std::move(Front());
    Pop();
    return true;
  }

  // Blocks while the queue is empty.
  void WaitForData() requires(IncludeWaitFunctionality)
  {
    m_count.wait(0, std::memory_order_acquire);
  }

  void Clear()
  {
    while (!Empty())
      Pop();
  }

private:
  struct Node
  {
    ManuallyConstructedValue<T> item;
    Node* next = nullptr;
  };

  Node* m_tail = new Node;   // producer: the empty node the next element goes into
  Node* m_head = m_tail;     // consumer: the oldest element (or the tail when empty)
  std::atomic<std::size_t> m_count{0};
};
}  // namespace detail

template <typename T>
using SPSCQueue = detail::SPSCQueueBase<T, false>;

template <typename T>
using WaitableSPSCQueue = detail::SPSCQueueBase<T, true>;
}  // namespace Common
