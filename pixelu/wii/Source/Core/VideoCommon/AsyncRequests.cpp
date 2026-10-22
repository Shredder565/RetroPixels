// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/AsyncRequests.h"

#include "Core/System.h"

#include "VideoCommon/Fifo.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

AsyncRequests AsyncRequests::s_singleton;

AsyncRequests::AsyncRequests() = default;

void AsyncRequests::PullEvents()
{
  if (m_queue.Empty())
    return;

  // Requests (EFB peeks, bounding box reads, ...) must observe every draw issued before them,
  // so submit the pending batch first.
  g_vertex_manager->Flush();

  // Each entry is popped only after it has run, so WaitForEmptyQueue() on the CPU thread cannot
  // return while the last request is still executing.
  do
  {
    std::invoke(std::move(m_queue.Front()));
    m_queue.Pop();
  } while (!m_queue.Empty());
}

void AsyncRequests::QueueEvent(Event&& event)
{
  m_queue.Push(std::move(event));
  // Make sure the video thread wakes up to service it.
  Core::System::GetInstance().GetFifo().RunGpu();
}

void AsyncRequests::WaitForEmptyQueue()
{
  m_queue.WaitForEmpty();
}

void AsyncRequests::SetPassthrough(bool enable)
{
  m_passthrough = enable;
}
