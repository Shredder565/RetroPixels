// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/PerfQueryBase.h"

namespace OGL
{
// Picks the occlusion query flavour the context supports.
std::unique_ptr<PerfQueryBase> GetPerfQuery(bool is_gles);

// Counts the pixels that pass the depth test with occlusion queries. Queries go into a ring
// and are read back in order; results are scaled to the native EFB size. The subclasses supply
// the GL calls for one kind of query object.
class PerfQuery : public PerfQueryBase
{
public:
  PerfQuery();
  ~PerfQuery() override = default;

  void EnableQuery(PerfQueryGroup group) override;
  void DisableQuery(PerfQueryGroup group) override;
  void ResetQuery() override;
  u32 GetQueryResult(PerfQueryType type) override;
  void FlushResults() override;
  bool IsFlushed() const override;

protected:
  struct PendingQuery
  {
    GLuint id = 0;
    PerfQueryGroup group{};
  };

  // 64 was measured as too few in Super Mario Sunshine, 128 as enough.
  static constexpr u32 PERF_QUERY_BUFFER_SIZE = 512;

  virtual void BeginGLQuery(GLuint id) = 0;
  virtual void EndGLQuery() = 0;
  virtual bool IsGLResultReady(GLuint id) = 0;
  // The sample count, already referenced to the native EFB resolution.
  virtual u32 ReadGLResult(GLuint id) = 0;

  // Query objects whose results have not been collected yet, starting at m_read_pos.
  std::array<PendingQuery, PERF_QUERY_BUFFER_SIZE> m_ring{};
  u32 m_read_pos = 0;

private:
  static bool CountsPixels(PerfQueryGroup group);

  // Collects results that are already available, stopping at the first that is not.
  void CollectReady();
  // Waits for and collects the oldest result. The ring must not be empty.
  void CollectOldest();
};

// Core GL / GLES occlusion queries of the given target.
class PerfQueryGL : public PerfQuery
{
public:
  explicit PerfQueryGL(GLenum query_type);
  ~PerfQueryGL() override;

protected:
  void BeginGLQuery(GLuint id) override;
  void EndGLQuery() override;
  bool IsGLResultReady(GLuint id) override;
  u32 ReadGLResult(GLuint id) override;

private:
  GLenum m_target;
};

// GLES with NV_occlusion_query_samples, which reports sample counts unlike ANY_SAMPLES_PASSED.
class PerfQueryGLESNV : public PerfQuery
{
public:
  PerfQueryGLESNV();
  ~PerfQueryGLESNV() override;

protected:
  void BeginGLQuery(GLuint id) override;
  void EndGLQuery() override;
  bool IsGLResultReady(GLuint id) override;
  u32 ReadGLResult(GLuint id) override;
};

}  // namespace OGL
