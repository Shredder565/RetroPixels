// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLPerfQuery.h"

#include <memory>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
// Scales a sample count at the internal resolution to what it would be at native resolution.
// The EFB area product is taken in 32 bits.
u64 ToNativeResolution(GLuint samples)
{
  const u32 efb_area = g_framebuffer_manager->GetEFBWidth() * g_framebuffer_manager->GetEFBHeight();
  return static_cast<u64>(samples) * EFB_WIDTH * EFB_HEIGHT / efb_area;
}
}  // namespace

std::unique_ptr<PerfQueryBase> GetPerfQuery(bool is_gles)
{
  if (!is_gles)
    return std::make_unique<PerfQueryGL>(GL_SAMPLES_PASSED);
  // Plain GLES only has the boolean any-samples query; the NV extension gives real counts.
  if (GLExtensions::Supports("GL_NV_occlusion_query_samples"))
    return std::make_unique<PerfQueryGLESNV>();
  return std::make_unique<PerfQueryGL>(GL_ANY_SAMPLES_PASSED);
}

// ---------------------------------------------------------------------------------------------
// Shared ring handling

PerfQuery::PerfQuery()
{
  ResetQuery();
}

bool PerfQuery::CountsPixels(PerfQueryGroup group)
{
  // Only the depth-test groups are measured with queries.
  return group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP;
}

void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  // Keep room in the ring: past half full, take whatever is ready; when completely full,
  // wait for the oldest.
  if (m_query_count.load(std::memory_order_relaxed) > PERF_QUERY_BUFFER_SIZE / 2)
    CollectReady();
  if (m_query_count.load(std::memory_order_relaxed) == PERF_QUERY_BUFFER_SIZE)
    CollectOldest();

  if (!CountsPixels(group))
    return;

  const u32 pending = m_query_count.load(std::memory_order_relaxed);
  PendingQuery& slot = m_ring[(m_read_pos + pending) % PERF_QUERY_BUFFER_SIZE];
  BeginGLQuery(slot.id);
  slot.group = group;
  m_query_count.fetch_add(1, std::memory_order_relaxed);
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  if (CountsPixels(group))
    EndGLQuery();
}

void PerfQuery::ResetQuery()
{
  m_query_count.store(0, std::memory_order_relaxed);
  for (std::atomic<u32>& total : m_results)
    total.store(0, std::memory_order_relaxed);
}

u32 PerfQuery::GetQueryResult(PerfQueryType type)
{
  const auto total = [this](PerfQueryGroup group) {
    return m_results[group].load(std::memory_order_relaxed);
  };

  u32 count = 0;
  switch (type)
  {
  case PQ_ZCOMP_INPUT_ZCOMPLOC:
  case PQ_ZCOMP_OUTPUT_ZCOMPLOC:
    count = total(PQG_ZCOMP_ZCOMPLOC);
    break;
  case PQ_ZCOMP_INPUT:
  case PQ_ZCOMP_OUTPUT:
    count = total(PQG_ZCOMP);
    break;
  case PQ_BLEND_INPUT:
    count = total(PQG_ZCOMP) + total(PQG_ZCOMP_ZCOMPLOC);
    break;
  case PQ_EFB_COPY_CLOCKS:
    count = total(PQG_EFB_COPY_CLOCKS);
    break;
  default:
    break;
  }
  // The hardware counters advance once per 2x2 quad.
  return count / 4;
}

void PerfQuery::FlushResults()
{
  while (!IsFlushed())
    CollectOldest();
}

bool PerfQuery::IsFlushed() const
{
  return m_query_count.load(std::memory_order_relaxed) == 0;
}

void PerfQuery::CollectReady()
{
  while (!IsFlushed() && IsGLResultReady(m_ring[m_read_pos].id))
    CollectOldest();
}

void PerfQuery::CollectOldest()
{
  const PendingQuery& oldest = m_ring[m_read_pos];
  const u32 samples = ReadGLResult(oldest.id);
  m_results[oldest.group].fetch_add(samples, std::memory_order_relaxed);

  m_read_pos = (m_read_pos + 1) % PERF_QUERY_BUFFER_SIZE;
  m_query_count.fetch_sub(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Core queries

PerfQueryGL::PerfQueryGL(GLenum query_type) : m_target(query_type)
{
  for (PendingQuery& query : m_ring)
    glGenQueries(1, &query.id);
}

PerfQueryGL::~PerfQueryGL()
{
  for (PendingQuery& query : m_ring)
    glDeleteQueries(1, &query.id);
}

void PerfQueryGL::BeginGLQuery(GLuint id)
{
  glBeginQuery(m_target, id);
}

void PerfQueryGL::EndGLQuery()
{
  glEndQuery(m_target);
}

bool PerfQueryGL::IsGLResultReady(GLuint id)
{
  GLuint available = GL_FALSE;
  glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
  return available == GL_TRUE;
}

u32 PerfQueryGL::ReadGLResult(GLuint id)
{
  GLuint samples = 0;
  glGetQueryObjectuiv(id, GL_QUERY_RESULT, &samples);
  u32 native = static_cast<u32>(ToNativeResolution(samples));
  // Each pixel counts once per sample when multisampling.
  if (g_ActiveConfig.iMultisamples > 1)
    native /= g_ActiveConfig.iMultisamples;
  return native;
}

// ---------------------------------------------------------------------------------------------
// NV occlusion queries (GLES)

PerfQueryGLESNV::PerfQueryGLESNV()
{
  for (PendingQuery& query : m_ring)
    glGenOcclusionQueriesNV(1, &query.id);
}

PerfQueryGLESNV::~PerfQueryGLESNV()
{
  for (PendingQuery& query : m_ring)
    glDeleteOcclusionQueriesNV(1, &query.id);
}

void PerfQueryGLESNV::BeginGLQuery(GLuint id)
{
  glBeginOcclusionQueryNV(id);
}

void PerfQueryGLESNV::EndGLQuery()
{
  glEndOcclusionQueryNV();
}

bool PerfQueryGLESNV::IsGLResultReady(GLuint id)
{
  GLuint available = GL_FALSE;
  glGetOcclusionQueryuivNV(id, GL_PIXEL_COUNT_AVAILABLE_NV, &available);
  return available == GL_TRUE;
}

u32 PerfQueryGLESNV::ReadGLResult(GLuint id)
{
  GLuint samples = 0;
  glGetOcclusionQueryuivNV(id, GL_OCCLUSION_TEST_RESULT_HP, &samples);
  // No multisample adjustment on this path.
  return static_cast<u32>(ToNativeResolution(samples));
}

}  // namespace OGL
