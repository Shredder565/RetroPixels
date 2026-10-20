// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <list>
#include <mutex>
#include <string>

#include "Common/CommonTypes.h"

namespace Common
{
// A named wall-clock accumulator for PROFILE() scopes. ToString() renders a table of all live
// profilers (calls, total, share of the frame, min/avg/stdev/max in microseconds) and resets
// their counters; the table is only rebuilt every 60 calls.
class Profiler
{
public:
  Profiler(std::string name);
  ~Profiler();

  static std::string ToString();

  // Nested Start/Stop pairs on the same thread (same *depth) count as one measurement.
  void Start(u64* time, int* depth);
  void Stop(u64* time, int* depth);

  // This profiler's table row; resets its counters.
  std::string Read();

  // Orders by accumulated time.
  bool operator<(const Profiler& b) const;

private:
  static std::list<Profiler*> s_all_profilers;
  static std::mutex s_mutex;
  static u32 s_max_length;
  static u64 s_frame_time;
  static u64 s_usecs_frame;

  static std::string s_lazy_result;
  static int s_lazy_delay;

  std::mutex m_mutex;
  std::string m_name;
  u64 m_usecs = 0;
  u64 m_usecs_min = ~u64{0};
  u64 m_usecs_max = 0;
  u64 m_usecs_quad = 0;
  u64 m_calls = 0;
};

// Starts a measurement on construction and ends it on destruction.
class ProfilerExecuter
{
public:
  ProfilerExecuter(Profiler* profiler, u64* time, int* depth)
      : m_profiler(profiler), m_time(time), m_depth(depth)
  {
    m_profiler->Start(m_time, m_depth);
  }
  ~ProfilerExecuter() { m_profiler->Stop(m_time, m_depth); }

private:
  Profiler* m_profiler;
  u64* m_time;
  int* m_depth;
};
}  // namespace Common

// Profiles the rest of the enclosing scope under `name`.
#define PROFILE(name)                                                                              \
  static Common::Profiler prof_gen(name);                                                          \
  static thread_local u64 prof_time;                                                               \
  static thread_local int prof_depth;                                                              \
  Common::ProfilerExecuter prof_e(&prof_gen, &prof_time, &prof_depth);
