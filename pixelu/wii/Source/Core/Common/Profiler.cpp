// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Profiler.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include <fmt/format.h>

#include "Common/Timer.h"

namespace Common
{
namespace
{
constexpr int INT_COLUMN = 8;     // calls, sum, min, max
constexpr int FLOAT_COLUMN = 11;  // rel, avg, stdev
constexpr int REBUILD_INTERVAL = 60;
constexpr u64 NO_MIN = ~u64{0};
}  // namespace

std::list<Profiler*> Profiler::s_all_profilers;
std::mutex Profiler::s_mutex;
u32 Profiler::s_max_length = 0;
u64 Profiler::s_frame_time = 0;
u64 Profiler::s_usecs_frame = 0;
std::string Profiler::s_lazy_result;
int Profiler::s_lazy_delay = 0;

Profiler::Profiler(std::string name) : m_name(std::move(name))
{
  std::lock_guard guard(s_mutex);
  s_max_length = std::max(s_max_length, static_cast<u32>(m_name.size()));
  s_all_profilers.push_back(this);
}

Profiler::~Profiler()
{
  std::lock_guard guard(s_mutex);
  s_all_profilers.remove(this);
}

bool Profiler::operator<(const Profiler& b) const
{
  return m_usecs < b.m_usecs;
}

std::string Profiler::ToString()
{
  if (s_lazy_delay > 0)
  {
    --s_lazy_delay;
    return s_lazy_result;
  }
  s_lazy_delay = REBUILD_INTERVAL - 1;

  std::lock_guard guard(s_mutex);
  if (s_all_profilers.empty())
    return "";

  const u64 now = Timer::NowUs();
  s_usecs_frame = now - s_frame_time;
  s_frame_time = now;

  std::string table;
  auto out = std::back_inserter(table);
  fmt::format_to(out, "{:<{}} {:>{}} {:>{}} {:>{}} {:>{}} {:>{}} {:>{}} {:>{}} / usec\n", "",
                 s_max_length, "calls", INT_COLUMN, "sum", INT_COLUMN, "rel", FLOAT_COLUMN, "min",
                 INT_COLUMN, "avg", FLOAT_COLUMN, "stdev", FLOAT_COLUMN, "max", INT_COLUMN);

  // Most expensive first.
  s_all_profilers.sort([](const Profiler* a, const Profiler* b) { return *b < *a; });
  for (Profiler* profiler : s_all_profilers)
  {
    table += profiler->Read();
    table += '\n';
  }

  s_lazy_result = std::move(table);
  return s_lazy_result;
}

void Profiler::Start(u64* time, int* depth)
{
  if ((*depth)++ == 0)
    *time = Timer::NowUs();
}

void Profiler::Stop(u64* time, int* depth)
{
  if (--(*depth) != 0)
    return;

  const u64 elapsed = Timer::NowUs() - *time;
  std::lock_guard guard(m_mutex);
  m_usecs += elapsed;
  m_usecs_min = std::min(m_usecs_min, elapsed);
  m_usecs_max = std::max(m_usecs_max, elapsed);
  m_usecs_quad += elapsed * elapsed;
  ++m_calls;
}

std::string Profiler::Read()
{
  std::lock_guard guard(m_mutex);

  double average = 0;
  double deviation = 0;
  if (m_calls != 0)
  {
    const double n = static_cast<double>(m_calls);
    average = static_cast<double>(m_usecs) / n;
    deviation = std::sqrt(static_cast<double>(m_usecs_quad) / n - average * average);
  }
  const u64 minimum = m_calls != 0 ? m_usecs_min : 0;
  const double share =
      s_usecs_frame != 0 ? static_cast<double>(m_usecs) * 100 / static_cast<double>(s_usecs_frame) :
                           0.0;

  std::string row = fmt::format("{:<{}} {:>{}} {:>{}} {:>{}.6g} {:>{}} {:>{}.2f} {:>{}.2f} {:>{}}",
                                m_name, s_max_length, m_calls, INT_COLUMN, m_usecs, INT_COLUMN,
                                share, FLOAT_COLUMN, minimum, INT_COLUMN, average, FLOAT_COLUMN,
                                deviation, FLOAT_COLUMN, m_usecs_max, INT_COLUMN);

  m_usecs = 0;
  m_usecs_min = NO_MIN;
  m_usecs_max = 0;
  m_usecs_quad = 0;
  m_calls = 0;
  return row;
}
}  // namespace Common
