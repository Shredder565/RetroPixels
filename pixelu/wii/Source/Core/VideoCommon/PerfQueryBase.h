// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "Common/CommonTypes.h"

// Pixel engine performance counters a game can read.
enum PerfQueryType
{
  PQ_ZCOMP_INPUT_ZCOMPLOC = 0,
  PQ_ZCOMP_OUTPUT_ZCOMPLOC,
  PQ_ZCOMP_INPUT,
  PQ_ZCOMP_OUTPUT,
  PQ_BLEND_INPUT,
  PQ_EFB_COPY_CLOCKS,
  PQ_NUM_MEMBERS
};

// Host query groups that the counters are derived from.
enum PerfQueryGroup
{
  PQG_ZCOMP_ZCOMPLOC,
  PQG_ZCOMP,
  PQG_EFB_COPY_CLOCKS,
  PQG_NUM_MEMBERS,
};

// Host occlusion queries backing the PE counters. The base class counts nothing.
class PerfQueryBase
{
public:
  PerfQueryBase() : m_query_count(0) {}
  virtual ~PerfQueryBase() {}

  virtual bool Initialize() { return true; }

  // Whether the game INI asks for the counters (called from the CPU and GPU threads).
  static bool ShouldEmulate();

  // Opens a query for the following draws. Call it right before the draw: a query left open
  // across a command buffer submission can reset the GPU on D3D12 and Vulkan.
  virtual void EnableQuery(PerfQueryGroup /*type*/) {}

  // Closes the open query.
  virtual void DisableQuery(PerfQueryGroup /*type*/) {}

  // Zeroes the counters and forgets pending queries.
  virtual void ResetQuery() {}

  // Current value of one counter (called from the CPU thread).
  virtual u32 GetQueryResult(PerfQueryType /*type*/) { return 0; }

  // Waits for pending queries; stalls the GPU pipeline.
  virtual void FlushResults() {}

  // True when no query results are outstanding (called from the CPU thread).
  virtual bool IsFlushed() const { return true; }

protected:
  std::atomic<u32> m_query_count;
  std::array<std::atomic<u32>, PQG_NUM_MEMBERS> m_results;
};

extern std::unique_ptr<PerfQueryBase> g_perf_query;
