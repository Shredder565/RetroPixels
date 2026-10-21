// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/RenderState.h"

namespace OGL
{
// GL sampler objects, one per distinct sampler state seen, kept until Clear(). Also tracks
// what is bound to each texture unit so unchanged states are not rebound.
class SamplerCache
{
public:
  SamplerCache();
  ~SamplerCache();

  SamplerCache(const SamplerCache&) = delete;
  SamplerCache& operator=(const SamplerCache&) = delete;
  SamplerCache(SamplerCache&&) = delete;
  SamplerCache& operator=(SamplerCache&&) = delete;

  void SetSamplerState(u32 stage, const SamplerState& state);
  // Forget the binding on a unit, so the next SetSamplerState binds even if the state matches.
  void InvalidateBinding(u32 stage);

  // Deletes every cached sampler (the two fixed ones stay).
  void Clear();
  void BindNearestSampler(int stage);
  void BindLinearSampler(int stage);

private:
  static void SetParameters(GLuint sampler_id, const SamplerState& params);

  std::map<SamplerState, GLuint> m_samplers;
  // Per unit: the state last bound and its sampler object (0 = unknown).
  std::array<std::pair<SamplerState, GLuint>, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_bound{};

  GLuint m_nearest = 0;
  GLuint m_linear = 0;
};

extern std::unique_ptr<SamplerCache> g_sampler_cache;
}  // namespace OGL
