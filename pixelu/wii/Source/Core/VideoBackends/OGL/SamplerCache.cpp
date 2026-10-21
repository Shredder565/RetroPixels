// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/SamplerCache.h"

#include <memory>

#include "Common/CommonTypes.h"
#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
std::unique_ptr<SamplerCache> g_sampler_cache;

namespace
{
// A sampler for the utility draws: the given filter both ways, clamped at the edges.
void MakeFixedSampler(GLuint* sampler, GLint filter)
{
  glSamplerParameteri(*sampler, GL_TEXTURE_MIN_FILTER, filter);
  glSamplerParameteri(*sampler, GL_TEXTURE_MAG_FILTER, filter);
  glSamplerParameteri(*sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(*sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

GLint WrapToGL(WrapMode mode)
{
  switch (mode)
  {
  case WrapMode::Repeat:
    return GL_REPEAT;
  case WrapMode::Mirror:
    return GL_MIRRORED_REPEAT;
  default:
    return GL_CLAMP_TO_EDGE;
  }
}

GLint MinFilterToGL(FilterMode min, FilterMode mip)
{
  const bool nearest = min == FilterMode::Near;
  if (mip == FilterMode::Linear)
    return nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
  return nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
}
}  // namespace

SamplerCache::SamplerCache()
{
  // Both names are generated before either is configured.
  glGenSamplers(1, &m_nearest);
  glGenSamplers(1, &m_linear);
  MakeFixedSampler(&m_nearest, GL_NEAREST);
  MakeFixedSampler(&m_linear, GL_LINEAR);
}

SamplerCache::~SamplerCache()
{
  Clear();
  glDeleteSamplers(1, &m_nearest);
  glDeleteSamplers(1, &m_linear);
}

void SamplerCache::BindNearestSampler(int stage)
{
  glBindSampler(static_cast<GLuint>(stage), m_nearest);
}

void SamplerCache::BindLinearSampler(int stage)
{
  glBindSampler(static_cast<GLuint>(stage), m_linear);
}

void SamplerCache::SetSamplerState(u32 stage, const SamplerState& state)
{
  auto& bound = m_bound[stage];
  if (bound.second != 0 && bound.first == state)
    return;

  GLuint sampler = 0;
  if (const auto found = m_samplers.find(state); found != m_samplers.end())
  {
    sampler = found->second;
  }
  else
  {
    glGenSamplers(1, &sampler);
    SetParameters(sampler, state);
    m_samplers.emplace(state, sampler);
  }

  bound = {state, sampler};
  glBindSampler(stage, sampler);
}

void SamplerCache::InvalidateBinding(u32 stage)
{
  m_bound[stage].second = 0;
}

void SamplerCache::SetParameters(GLuint sampler_id, const SamplerState& params)
{
  const auto& tm0 = params.tm0;
  const auto& tm1 = params.tm1;

  glSamplerParameteri(sampler_id, GL_TEXTURE_MIN_FILTER,
                      MinFilterToGL(tm0.min_filter, tm0.mipmap_filter));
  glSamplerParameteri(sampler_id, GL_TEXTURE_MAG_FILTER,
                      tm0.mag_filter == FilterMode::Near ? GL_NEAREST : GL_LINEAR);
  glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_S, WrapToGL(tm0.wrap_u));
  glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_T, WrapToGL(tm0.wrap_v));

  // LODs are stored in 1/16 steps and the bias in 1/256 steps.
  glSamplerParameterf(sampler_id, GL_TEXTURE_MIN_LOD, static_cast<float>(tm1.min_lod) / 16.0f);
  glSamplerParameterf(sampler_id, GL_TEXTURE_MAX_LOD, static_cast<float>(tm1.max_lod) / 16.0f);
  if (g_backend_info.bSupportsLodBiasInSampler)
  {
    glSamplerParameterf(sampler_id, GL_TEXTURE_LOD_BIAS, static_cast<float>(tm0.lod_bias) / 256.0f);
  }

  // The anisotropy field holds log2 of the level.
  const u32 aniso_log2 = tm0.anisotropic_filtering;
  if (aniso_log2 != 0 && g_ogl_config.bSupportsAniso)
  {
    glSamplerParameterf(sampler_id, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        static_cast<float>(1u << aniso_log2));
  }
}

void SamplerCache::Clear()
{
  for (auto& [state, sampler] : m_samplers)
    glDeleteSamplers(1, &sampler);
  for (auto& bound : m_bound)
    bound.second = 0;
  m_samplers.clear();
}
}  // namespace OGL
