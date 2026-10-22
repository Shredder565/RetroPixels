// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/TransformUnit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "VideoBackends/Software/NativeVertexFormat.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/XFMemory.h"

namespace TransformUnit
{
namespace
{
using Common::Vec3;
using Common::Vec4;

// One row of a 3x3 matrix applied to v.
float Dot3(const float* row, const Vec3& v)
{
  return row[0] * v.x + row[1] * v.y + row[2] * v.z;
}

// One row of a 3x4 matrix applied to (v, 1).
float Dot4(const float* row, const Vec3& v)
{
  return row[0] * v.x + row[1] * v.y + row[2] * v.z + row[3];
}

Vec3 Affine3x4(const float* m, const Vec3& v)
{
  return Vec3(Dot4(m, v), Dot4(m + 4, v), Dot4(m + 8, v));
}

Vec3 Load3(const float (&values)[3])
{
  return Vec3(values[0], values[1], values[2]);
}

// The projection registers hold the six non-trivial terms of the GX projection matrix.
Vec4 Project(const Vec3& eye)
{
  const auto& p = xfmem.projection.rawProjection;
  if (xfmem.projection.type == ProjectionType::Perspective)
  {
    // z is pulled in very slightly so that geometry exactly at the far plane stays inside.
    constexpr float FAR_FUDGE = 1.0f - static_cast<float>(1e-7);
    return Vec4(p[0] * eye.x + p[1] * eye.z, p[2] * eye.y + p[3] * eye.z,
                (p[4] * eye.z + p[5]) * FAR_FUDGE, -eye.z);
  }
  return Vec4(p[0] * eye.x + p[1], p[2] * eye.y + p[3], p[4] * eye.z + p[5], 1);
}

// n / d, with 0/0 and x/0 resolved the way the lighting hardware does.
float LightDivide(float n, float d)
{
  if (d == 0)
    return n > 0 ? 1 : 0;
  return n / d;
}

// Attenuation of one light at a vertex. `to_light` comes in as the unnormalised vector from the
// vertex to the light and leaves as the direction used for the diffuse term.
float Attenuation(const Light& light, Vec3& to_light, const Vec3& normal, const LitChannel& chan)
{
  const Vec3 light_dir = Load3(light.ddir);
  const Vec3 cos_att = Load3(light.cosatt);
  Vec3 dist_att = Load3(light.distatt);

  switch (chan.attnfunc)
  {
  case AttenuationFunc::None:
  case AttenuationFunc::Dir:
    to_light = to_light.Normalized();
    if (to_light == Vec3(0.0f, 0.0f, 0.0f))
      to_light = normal;
    return 1.0f;

  case AttenuationFunc::Spec:
  {
    // Specular: the attenuation polynomials are evaluated on the half-angle cosine.
    to_light = to_light.Normalized();
    const float cosine = to_light.Dot(normal) >= 0.0 ? std::max(0.0f, light_dir.Dot(normal)) : 0;
    const Vec3 powers(1.0f, cosine, cosine * cosine);
    if (chan.diffusefunc != DiffuseFunc::None)
      dist_att = dist_att.Normalized();
    return LightDivide(std::max(0.0f, powers.Dot(cos_att)), powers.Dot(dist_att));
  }

  case AttenuationFunc::Spot:
  {
    // Spot: angular falloff over a quadratic in the distance.
    const float dist_sq = to_light.LengthSquared();
    const float dist = sqrtf(dist_sq);
    to_light = to_light / dist;
    const float cosine = std::max(0.0f, to_light.Dot(light_dir));
    const float angular = cos_att.x + (cos_att.y * cosine) + (cos_att.z * cosine * cosine);
    const float distance = dist_att.x + (dist_att.y * dist) + (dist_att.z * dist_sq);
    return LightDivide(std::max(0.0f, angular), distance);
  }
  }
  PanicAlertFmt("Invalid attnfunc: {}", chan.attnfunc);
  return 1.0f;
}

// Diffuse factor of one light: attenuation times the clamped or signed cosine. Returns false
// for an invalid diffuse function (nothing is added then).
bool LightFactor(u32 light_index, const Vec3& pos, const Vec3& normal, const LitChannel& chan,
                 float* factor)
{
  const Light& light = xfmem.lights[light_index];
  Vec3 to_light = Load3(light.dpos) - pos;
  const float attn = Attenuation(light, to_light, normal, chan);
  const float cosine = to_light.Dot(normal);

  switch (chan.diffusefunc)
  {
  case DiffuseFunc::None:
    *factor = attn;
    return true;
  case DiffuseFunc::Sign:
    *factor = attn * cosine;
    return true;
  case DiffuseFunc::Clamp:
    *factor = attn * std::max(0.0f, cosine);
    return true;
  default:
    // (The message has always shown the attenuation function.)
    PanicAlertFmt("Invalid diffusefunc: {}", chan.attnfunc);
    return false;
  }
}

// Light colours and the colour registers are stored with alpha in byte 0, then B, G, R.
void AccumulateColorLights(const LitChannel& chan, const Vec3& pos, const Vec3& normal, Vec3& sum)
{
  const u32 mask = chan.GetFullLightMask();
  for (u32 i = 0; i < 8; ++i)
  {
    if ((mask & (1u << i)) == 0)
      continue;
    float factor;
    if (!LightFactor(i, pos, normal, chan, &factor))
      continue;
    const u8* color = xfmem.lights[i].color;
    sum.x += color[1] * factor;
    sum.y += color[2] * factor;
    sum.z += color[3] * factor;
  }
}

void AccumulateAlphaLights(const LitChannel& chan, const Vec3& pos, const Vec3& normal, float& sum)
{
  const u32 mask = chan.GetFullLightMask();
  for (u32 i = 0; i < 8; ++i)
  {
    if ((mask & (1u << i)) == 0)
      continue;
    const u8 alpha = xfmem.lights[i].color[0];
    // Same product order as the colour channels: (alpha * attenuation) * cosine.
    const Light& light = xfmem.lights[i];
    Vec3 to_light = Load3(light.dpos) - pos;
    const float attn = Attenuation(light, to_light, normal, chan);
    const float cosine = to_light.Dot(normal);
    switch (chan.diffusefunc)
    {
    case DiffuseFunc::None:
      sum += alpha * attn;
      break;
    case DiffuseFunc::Sign:
      sum += alpha * attn * cosine;
      break;
    case DiffuseFunc::Clamp:
      sum += alpha * attn * std::max(0.0f, cosine);
      break;
    default:
      PanicAlertFmt("Invalid diffusefunc: {}", chan.attnfunc);
      break;
    }
  }
}

// material * light / 255, with the light level stretched from 0-255 to 0-256.
u8 Modulate(u8 material, float light)
{
  const int level = std::clamp(static_cast<int>(light), 0, 255);
  return static_cast<u8>((material * (level + (level >> 7))) >> 8);
}

void GenerateRegular(const TexMtxInfo& info, u32 coord, const InputVertexData& in,
                     OutputVertexData& out)
{
  Vec3 source;
  switch (info.sourcerow)
  {
  case SourceRow::Geom:
    source = in.position;
    break;
  case SourceRow::Normal:
    source = in.normal[0];
    break;
  case SourceRow::BinormalT:
    source = in.normal[1];
    break;
  case SourceRow::BinormalB:
    source = in.normal[2];
    break;
  default:
  {
    ASSERT(info.sourcerow >= SourceRow::Tex0 && info.sourcerow <= SourceRow::Tex7);
    const u32 tex = static_cast<u32>(info.sourcerow.Value()) - static_cast<u32>(SourceRow::Tex0);
    source = Vec3(in.texCoords[tex].x, in.texCoords[tex].y, 1.0f);
    break;
  }
  }

  // NaN inputs become 1 (Shadow the Hedgehog's eyelids in cutscenes need this).
  for (float* c : {&source.x, &source.y, &source.z})
  {
    if (std::isnan(*c))
      *c = 1;
  }

  // Two-component inputs take 1 as their third component.
  const Vec3 input = info.inputform == TexInputForm::AB11 ? Vec3(source.x, source.y, 1.0f) : source;
  const float* m = &xfmem.posMatrices[in.texMtx[coord] * 4];
  Vec3& result = out.texCoords[coord];
  result.x = Dot4(m, input);
  result.y = Dot4(m + 4, input);
  result.z = info.projection == TexSize::ST ? 1.0f : Dot4(m + 8, input);

  if (xfmem.dualTexTrans.enabled)
  {
    const PostMtxInfo& post = xfmem.postMtxInfo[coord];
    const Vec3 first = post.normalize ? result.Normalized() : result;
    result = Affine3x4(&xfmem.postMatrices[post.index * 4], first);
  }

  // A zero q is special-cased by the hardware (devkitPro's neheGX lesson 8, Rogue Squadron 3's
  // Hoth sky, The Last Story's shadows).
  if (result.z == 0.0f)
  {
    result.x = std::clamp(result.x / 2.0f, -1.0f, 1.0f);
    result.y = std::clamp(result.y / 2.0f, -1.0f, 1.0f);
  }
}
}  // namespace

void TransformPosition(const InputVertexData* src, OutputVertexData* dst)
{
  dst->mvPosition = Affine3x4(&xfmem.posMatrices[src->posMtx * 4], src->position);
  dst->projectedPosition = Project(dst->mvPosition);
}

void TransformNormal(const InputVertexData* src, OutputVertexData* dst)
{
  const float* m = &xfmem.normalMatrices[(src->posMtx & 31) * 3];
  for (size_t i = 0; i < dst->normal.size(); ++i)
    dst->normal[i] =
        Vec3(Dot3(m, src->normal[i]), Dot3(m + 3, src->normal[i]), Dot3(m + 6, src->normal[i]));
  // Only the normal is renormalised. The binormals keep the matrix's scale, which is how games
  // size the emboss effect while lighting still gets a unit normal.
  dst->normal[0] = dst->normal[0].Normalized();
}

void TransformColor(const InputVertexData* src, OutputVertexData* dst)
{
  for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; ++chan)
  {
    const LitChannel& color_chan = xfmem.color[chan];
    const LitChannel& alpha_chan = xfmem.alpha[chan];

    // A, B, G, R
    std::array<u8, 4> material;
    if (color_chan.matsource == MatSource::Vertex)
      material = src->color[chan];
    else
      std::memcpy(material.data(), &xfmem.matColor[chan], sizeof(u32));

    std::array<u8, 4> lit = material;
    if (color_chan.enablelighting)
    {
      Vec3 light;
      if (color_chan.ambsource == AmbSource::Vertex)
      {
        light = Vec3(src->color[chan][1], src->color[chan][2], src->color[chan][3]);
      }
      else
      {
        std::array<u8, 4> ambient;
        std::memcpy(ambient.data(), &xfmem.ambColor[chan], sizeof(u32));
        light = Vec3(ambient[1], ambient[2], ambient[3]);
      }
      AccumulateColorLights(color_chan, dst->mvPosition, dst->normal[0], light);
      lit[1] = Modulate(material[1], light.x);
      lit[2] = Modulate(material[2], light.y);
      lit[3] = Modulate(material[3], light.z);
    }

    const u8 alpha_material = alpha_chan.matsource == MatSource::Vertex ?
                                  src->color[chan][0] :
                                  static_cast<u8>(xfmem.matColor[chan] & 0xff);
    if (alpha_chan.enablelighting)
    {
      float light = alpha_chan.ambsource == AmbSource::Vertex ?
                        src->color[chan][0] :
                        static_cast<float>(xfmem.ambColor[chan] & 0xff);
      AccumulateAlphaLights(alpha_chan, dst->mvPosition, dst->normal[0], light);
      lit[0] = Modulate(alpha_material, light);
    }
    else
    {
      lit[0] = alpha_material;
    }

    // The output order is R, G, B, A.
    dst->color[chan] = {lit[3], lit[2], lit[1], lit[0]};
  }
}

void TransformTexCoord(const InputVertexData* src, OutputVertexData* dst)
{
  const u32 count = xfmem.numTexGen.numTexGens;
  for (u32 coord = 0; coord < count; ++coord)
  {
    const TexMtxInfo& info = xfmem.texMtxInfo[coord];
    Vec3& out = dst->texCoords[coord];
    switch (info.texgentype)
    {
    case TexGenType::Regular:
      GenerateRegular(info, coord, *src, *dst);
      break;

    case TexGenType::EmbossMap:
    {
      // Offsets an earlier coordinate along the light direction projected on the binormals.
      const Light& light = xfmem.lights[info.embosslightshift];
      const Vec3 to_light = (Load3(light.dpos) - dst->mvPosition).Normalized();
      const Vec3 base = dst->texCoords[info.embosssourceshift];
      out.x = base.x + to_light.Dot(dst->normal[1]);
      out.y = base.y + to_light.Dot(dst->normal[2]);
      out.z = base.z;
      break;
    }

    case TexGenType::Color0:
    case TexGenType::Color1:
    {
      ASSERT(info.inputform == TexInputForm::AB11);
      const auto& color = dst->color[info.texgentype == TexGenType::Color0 ? 0 : 1];
      out.x = static_cast<float>(color[0]) / 255.0f;
      out.y = static_cast<float>(color[1]) / 255.0f;
      out.z = 1.0f;
      break;
    }

    default:
      ERROR_LOG_FMT(VIDEO, "Bad tex gen type {}", info.texgentype);
      break;
    }
  }

  // Scale to texels by the BP texture coordinate scale.
  for (u32 coord = 0; coord < count; ++coord)
  {
    dst->texCoords[coord].x *= static_cast<float>(bpmem.texcoords[coord].s.scale_minus_1 + 1);
    dst->texCoords[coord].y *= static_cast<float>(bpmem.texcoords[coord].t.scale_minus_1 + 1);
  }
}
}  // namespace TransformUnit
