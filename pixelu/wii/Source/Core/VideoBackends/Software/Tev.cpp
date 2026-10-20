// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/Tev.h"

#include <algorithm>
#include <cmath>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "Core/System.h"

#include "VideoBackends/Software/SWBoundingBox.h"
#include "VideoBackends/Software/SWEfbInterface.h"
#include "VideoBackends/Software/TextureSampler.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace
{
// Component order of the per-channel arrays below.
enum : size_t
{
  CH_R = 0,
  CH_G = 1,
  CH_B = 2,
  CH_A = 3,
};

// Texture coordinates and their offsets are kept in 24 bits.
s32 Wrap24(s32 value)
{
  return static_cast<s32>(static_cast<u32>(value) << 8) >> 8;
}

s32 Low8(s16 value)
{
  return value & 0xff;
}

s32 SignedLow11(s16 value)
{
  return static_cast<s32>(static_cast<u32>(value) << 21) >> 21;
}

s16 ClampResult(s32 value, bool to_unsigned_8bit)
{
  const s16 v = static_cast<s16>(value);
  return to_unsigned_8bit ? std::clamp<s16>(v, 0, 255) : std::clamp<s16>(v, -1024, 1023);
}

// The fixed fractions konst selections 0-7 stand for (1, 7/8, ... 1/8 of 255).
constexpr std::array<s16, 8> KONST_FRACTIONS{255, 223, 191, 159, 128, 96, 64, 32};

// Values of TevBias and TevScale (the compare setting of the bias is handled separately).
constexpr std::array<s32, 4> BIAS_VALUE{0, 128, -128, 0};
constexpr std::array<int, 4> SCALE_UP{0, 1, 2, 0};
constexpr std::array<int, 4> SCALE_DOWN{0, 0, 0, 1};

bool CompareAlpha(int alpha, int ref, CompareMode mode)
{
  switch (mode)
  {
  case CompareMode::Never:
    return false;
  case CompareMode::Less:
    return alpha < ref;
  case CompareMode::Equal:
    return alpha == ref;
  case CompareMode::LEqual:
    return alpha <= ref;
  case CompareMode::Greater:
    return alpha > ref;
  case CompareMode::NEqual:
    return alpha != ref;
  case CompareMode::GEqual:
    return alpha >= ref;
  case CompareMode::Always:
  default:
    return true;
  }
}

bool PassesAlphaTest(int alpha)
{
  const auto& test = bpmem.alpha_test;
  const bool first = CompareAlpha(alpha, static_cast<int>(test.ref0.Value()), test.comp0);
  const bool second = CompareAlpha(alpha, static_cast<int>(test.ref1.Value()), test.comp1);
  switch (test.logic)
  {
  case AlphaTestOp::And:
    return first && second;
  case AlphaTestOp::Or:
    return first || second;
  case AlphaTestOp::Xor:
    return first != second;
  case AlphaTestOp::Xnor:
  default:
    return first == second;
  }
}

// Wraps the regular texture coordinate before the indirect offset is added.
s32 WrapRegularCoord(s32 coord, IndTexWrap wrap)
{
  switch (wrap)
  {
  case IndTexWrap::ITW_OFF:
    return coord;
  case IndTexWrap::ITW_256:
  case IndTexWrap::ITW_128:
  case IndTexWrap::ITW_64:
  case IndTexWrap::ITW_32:
  case IndTexWrap::ITW_16:
  {
    // 256 >> (wrap - 1) texels, with 7 fraction bits.
    const s32 texels = 256 >> (static_cast<u32>(wrap) - 1);
    return coord & ((texels << 7) - 1);
  }
  case IndTexWrap::ITW_0:
    return 0;
  default:
    PanicAlertFmt("Invalid indirect wrap mode {}", wrap);
    return 0;
  }
}

// Eye-space depth for fog, in units where the far plane of the z buffer is 1.
float FogDepth(s32 x, s32 z)
{
  const auto& fog = bpmem.fog;
  float ze;
  if (fog.c_proj_fsel.proj == FogProjection::Perspective)
  {
    // ze = A / (B - (Zs >> B_SHF)), with A and Zs scaled to 24 bits.
    const s32 denom = static_cast<s32>(fog.b_magnitude - static_cast<u32>(z >> fog.b_shift));
    ze = (fog.GetA() * 16777215.0f) / static_cast<float>(denom);
  }
  else
  {
    ze = fog.GetA() * (static_cast<float>(z) / 16777215.0f);
  }

  const auto& range = bpmem.fogRange;
  if (range.Base.Enabled)
  {
    // Range adjustment: points further from the centre column see more fog. The horizontal
    // distance from the centre (normalised by the viewport width) picks a weight between ten
    // k values; ze is then scaled by sqrt(offset^2 + k^2) / k, the reciprocal of a cosine.
    // How hardware normalises the offset and scales k is not known; the factor 4 on k matches
    // Fortune Street.
    const s32 centre = static_cast<s32>(range.Base.Center.Value()) - 342;
    const float offset = static_cast<float>(x - centre) / static_cast<float>(xfmem.viewport.wd);
    const float position = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);

    const int below = static_cast<int>(position);
    const int above = below + 1;
    const auto k_value = [&](int index) { return range.K[index / 2].GetValue(index % 2) * 4.f; };
    const float k_below = k_value(below);
    // Index 10 only occurs with a weight of zero; any finite value gives the same result.
    const float k_above = above < 10 ? k_value(above) : k_below;
    const float weight = static_cast<float>(above) - position;
    const float k = k_below * weight + k_above * (1.f - weight);
    // Unqualified on purpose: this picks the same overload (float on MSVC) as the rest of the
    // video code.
    ze = static_cast<float>(ze * (sqrt(offset * offset + k * k) / k));
  }

  return ze - fog.GetC();
}

// Fog density for the pixel, 0 (none) to 1 (fog colour only).
float FogDensity(float ze)
{
  float f = std::clamp(ze, 0.f, 1.f);
  switch (bpmem.fog.c_proj_fsel.fsel)
  {
  case FogType::Exp:
    return static_cast<float>(1.0f - pow(2.0f, -8.0f * f));
  case FogType::ExpSq:
    return static_cast<float>(1.0f - pow(2.0f, -8.0f * f * f));
  case FogType::BackwardsExp:
    f = 1.0f - f;
    return static_cast<float>(pow(2.0f, -8.0f * f));
  case FogType::BackwardsExpSq:
    f = 1.0f - f;
    return static_cast<float>(pow(2.0f, -8.0f * f * f));
  default:
    return f;
  }
}
}  // namespace

void Tev::SetKonstColors()
{
  const auto& kcolors = Core::System::GetInstance().GetPixelShaderManager().constants.kcolors;
  for (size_t i = 0; i < m_konst.size(); ++i)
  {
    m_konst[i].r = static_cast<s16>(kcolors[i][0]);
    m_konst[i].g = static_cast<s16>(kcolors[i][1]);
    m_konst[i].b = static_cast<s16>(kcolors[i][2]);
    m_konst[i].a = static_cast<s16>(kcolors[i][3]);
  }
}

Tev::Rgba Tev::RasterizedColor(u32 channel_select, u32 swap_table) const
{
  const auto select = static_cast<RasColorChan>(channel_select);
  switch (select)
  {
  case RasColorChan::Color0:
  case RasColorChan::Color1:
  {
    const auto& color = Color[select == RasColorChan::Color0 ? 0 : 1];
    const auto swap = bpmem.tevksel.GetSwapTable(swap_table);
    Rgba out;
    out.r = color[u32(swap[ColorChannel::Red])];
    out.g = color[u32(swap[ColorChannel::Green])];
    out.b = color[u32(swap[ColorChannel::Blue])];
    out.a = color[u32(swap[ColorChannel::Alpha])];
    return out;
  }
  case RasColorChan::AlphaBump:
  case RasColorChan::NormalizedAlphaBump:
  {
    // The normalised form copies the top bits into the low bits a 5-bit bump leaves empty.
    const u8 value = select == RasColorChan::AlphaBump ?
                         m_bump_alpha :
                         static_cast<u8>(m_bump_alpha | (m_bump_alpha >> 5));
    return Rgba{value, value, value, value};
  }
  default:
    if (select != RasColorChan::Zero)
      PanicAlertFmt("Invalid ras color channel: {}", select);
    return Rgba{};
  }
}

void Tev::ApplyIndirect(u32 stage, s32 s, s32 t)
{
  const TevStageIndirect& ind = bpmem.tevind[stage];
  const auto& texel = m_indirect_texel[ind.bt];

  // The indirect texel's alpha, blue and green give the s, t and u offsets.
  const std::array<u8, 3> stu{texel[CH_A], texel[CH_B], texel[CH_G]};

  switch (ind.bs)
  {
  case IndTexBumpAlpha::Off:
    m_bump_alpha = 0;
    break;
  case IndTexBumpAlpha::S:
    m_bump_alpha = stu[0];
    break;
  case IndTexBumpAlpha::T:
    m_bump_alpha = stu[1];
    break;
  case IndTexBumpAlpha::U:
    m_bump_alpha = stu[2];
    break;
  }

  // Offsets keep the top 8, 5, 4 or 3 bits; the bump alpha keeps the rest of the byte.
  int drop = 0;
  switch (ind.fmt)
  {
  case IndTexFormat::ITF_8:
    drop = 0;
    m_bump_alpha &= 0xf8;
    break;
  case IndTexFormat::ITF_5:
    drop = 3;
    m_bump_alpha = static_cast<u8>(m_bump_alpha << 5);
    break;
  case IndTexFormat::ITF_4:
    drop = 4;
    m_bump_alpha = static_cast<u8>(m_bump_alpha << 4);
    break;
  case IndTexFormat::ITF_3:
    drop = 5;
    m_bump_alpha = static_cast<u8>(m_bump_alpha << 3);
    break;
  }

  // 8-bit offsets are biased by -128, the shorter ones by +1.
  const s32 bias_amount = ind.fmt == IndTexFormat::ITF_8 ? -128 : 1;
  const std::array<bool, 3> biased{ind.bias_s.Value(), ind.bias_t.Value(), ind.bias_u.Value()};
  std::array<s32, 3> offset;
  for (size_t i = 0; i < 3; ++i)
    offset[i] = (stu[i] >> drop) + (biased[i] ? bias_amount : 0);

  // Matrix product in s17.7. Intermediate results may overflow; only the low 24 bits matter.
  s32 delta_s = 0;
  s32 delta_t = 0;
  if (ind.matrix_index == IndMtxIndex::Off)
  {
    // Without a matrix the matrix select should be zero as well.
    ASSERT(ind.matrix_id == IndMtxId::Indirect);
  }
  else
  {
    const IND_MTX& mtx = bpmem.indmtx[static_cast<u32>(ind.matrix_index.Value()) - 1];
    switch (ind.matrix_id)
    {
    case IndMtxId::Indirect:
      // s0.10 matrix times the offset vector gives s17.10; drop 3 bits to get s17.7.
      delta_s = (mtx.col0.ma * offset[0] + mtx.col1.mc * offset[1] + mtx.col2.me * offset[2]) >> 3;
      delta_t = (mtx.col0.mb * offset[0] + mtx.col1.md * offset[1] + mtx.col2.mf * offset[2]) >> 3;
      break;
    case IndMtxId::S:
    case IndMtxId::T:
    {
      // Dynamic matrices scale the regular coordinate by the s or t offset (divided by 256).
      const s32 factor = ind.matrix_id == IndMtxId::S ? offset[0] : offset[1];
      delta_s = s * factor / 256;
      delta_t = t * factor / 256;
      break;
    }
    default:
      PanicAlertFmt("Invalid indirect matrix ID {}", ind.matrix_id);
      return;
    }

    const int shift = 17 - mtx.GetScale();
    if (shift >= 0)
    {
      delta_s >>= shift;
      delta_t >>= shift;
    }
    else
    {
      delta_s = static_cast<s32>(static_cast<u32>(delta_s) << -shift);
      delta_t = static_cast<s32>(static_cast<u32>(delta_t) << -shift);
    }
  }

  // Each wrap is evaluated (and may complain) before the next.
  const s32 new_s =
      static_cast<s32>(static_cast<u32>(WrapRegularCoord(s, ind.sw)) + static_cast<u32>(delta_s));
  const s32 new_t =
      static_cast<s32>(static_cast<u32>(WrapRegularCoord(t, ind.tw)) + static_cast<u32>(delta_t));
  if (ind.fb_addprev)
  {
    m_coord.s = Wrap24(static_cast<s32>(static_cast<u32>(m_coord.s) + static_cast<u32>(new_s)));
    m_coord.t = Wrap24(static_cast<s32>(static_cast<u32>(m_coord.t) + static_cast<u32>(new_t)));
  }
  else
  {
    m_coord.s = Wrap24(new_s);
    m_coord.t = Wrap24(new_t);
  }
}

Tev::StageInputs Tev::GatherInputs(u32 stage) const
{
  const auto& cc = bpmem.combiners[stage].colorC;
  const auto& ac = bpmem.combiners[stage].alphaC;

  // The colour arguments come in pairs (rgb, then alpha broadcast) for these six sources.
  const std::array<const Rgba*, 6> pair_source{&m_regs[0], &m_regs[1], &m_regs[2],
                                               &m_regs[3], &m_tex,     &m_ras};
  const auto color_arg = [&](TevColorArg arg) -> std::array<s16, 3> {
    const u32 v = static_cast<u32>(arg);
    if (v < 12)
    {
      const Rgba& src = *pair_source[v >> 1];
      if ((v & 1) != 0)
        return {src.a, src.a, src.a};
      return {src.r, src.g, src.b};
    }
    switch (arg)
    {
    case TevColorArg::One:
      return {255, 255, 255};
    case TevColorArg::Half:
      return {128, 128, 128};
    case TevColorArg::Konst:
      return {m_stage_konst.r, m_stage_konst.g, m_stage_konst.b};
    default:
      return {0, 0, 0};
    }
  };
  const auto alpha_arg = [&](TevAlphaArg arg) -> s16 {
    const u32 v = static_cast<u32>(arg);
    if (v < 4)
      return m_regs[v].a;
    switch (arg)
    {
    case TevAlphaArg::TexAlpha:
      return m_tex.a;
    case TevAlphaArg::RasAlpha:
      return m_ras.a;
    case TevAlphaArg::Konst:
      return m_stage_konst.a;
    default:
      return 0;
    }
  };

  StageInputs in;
  const std::array<std::array<s16, 3>, 4> rgb{color_arg(cc.a), color_arg(cc.b), color_arg(cc.c),
                                              color_arg(cc.d)};
  for (size_t ch = 0; ch < 3; ++ch)
  {
    in.a[ch] = Low8(rgb[0][ch]);
    in.b[ch] = Low8(rgb[1][ch]);
    in.c[ch] = Low8(rgb[2][ch]);
    in.d[ch] = SignedLow11(rgb[3][ch]);
  }
  in.a[CH_A] = Low8(alpha_arg(ac.a));
  in.b[CH_A] = Low8(alpha_arg(ac.b));
  in.c[CH_A] = Low8(alpha_arg(ac.c));
  in.d[CH_A] = SignedLow11(alpha_arg(ac.d));
  return in;
}

namespace
{
// The comparison value of the compare modes: one channel, or two or three channels packed with
// red lowest. per_channel names the component used by the one-channel-each mode.
u32 CompareKey(TevCompareMode mode, const std::array<s32, 4>& v, size_t per_channel)
{
  switch (mode)
  {
  case TevCompareMode::R8:
    return static_cast<u32>(v[CH_R]);
  case TevCompareMode::GR16:
    return static_cast<u32>((v[CH_G] << 8) | v[CH_R]);
  case TevCompareMode::BGR24:
    return static_cast<u32>((v[CH_B] << 16) | (v[CH_G] << 8) | v[CH_R]);
  default:
    return static_cast<u32>(v[per_channel]);
  }
}

// a * (1 - c) + b * c with c taken as c + c/128 over 256, scaled and rounded as the combiner does,
// added to (d + bias). negate_first selects the alpha unit's rounding for subtraction, which
// negates before the shift instead of after it.
s32 RegularCombine(s32 a, s32 b, s32 c, s32 d, TevBias bias, TevOp op, TevScale scale,
                   bool negate_first)
{
  const s32 weight = c + (c >> 7);
  const int up = SCALE_UP[static_cast<u32>(scale)];
  const bool subtract = op == TevOp::Sub;

  s32 lerp = (a * (256 - weight) + b * weight) << up;
  if (scale != TevScale::Divide2)
    lerp += subtract ? 127 : 128;
  if (subtract)
    lerp = negate_first ? (-lerp >> 8) : -(lerp >> 8);
  else
    lerp >>= 8;

  const s32 sum = ((d + BIAS_VALUE[static_cast<u32>(bias)]) << up) + lerp;
  return sum >> SCALE_DOWN[static_cast<u32>(scale)];
}
}  // namespace

void Tev::CombineColor(u32 stage, const StageInputs& in)
{
  const auto& cc = bpmem.combiners[stage].colorC;
  Rgba& dest = m_regs[static_cast<u32>(cc.dest.Value())];
  std::array<s32, 3> result;

  if (cc.bias != TevBias::Compare)
  {
    for (size_t ch = 0; ch < 3; ++ch)
      result[ch] =
          RegularCombine(in.a[ch], in.b[ch], in.c[ch], in.d[ch], cc.bias, cc.op, cc.scale, false);
  }
  else
  {
    for (size_t ch = 0; ch < 3; ++ch)
    {
      const u32 lhs = CompareKey(cc.compare_mode, in.a, ch);
      const u32 rhs = CompareKey(cc.compare_mode, in.b, ch);
      const bool pass = cc.comparison == TevComparison::GT ? lhs > rhs : lhs == rhs;
      result[ch] = in.d[ch] + (pass ? in.c[ch] : 0);
    }
  }

  dest.r = ClampResult(result[CH_R], cc.clamp);
  dest.g = ClampResult(result[CH_G], cc.clamp);
  dest.b = ClampResult(result[CH_B], cc.clamp);
}

void Tev::CombineAlpha(u32 stage, const StageInputs& in)
{
  const auto& ac = bpmem.combiners[stage].alphaC;
  s32 result;

  if (ac.bias != TevBias::Compare)
  {
    result = RegularCombine(in.a[CH_A], in.b[CH_A], in.c[CH_A], in.d[CH_A], ac.bias, ac.op,
                            ac.scale, true);
  }
  else
  {
    const u32 lhs = CompareKey(ac.compare_mode, in.a, CH_A);
    const u32 rhs = CompareKey(ac.compare_mode, in.b, CH_A);
    const bool pass = ac.comparison == TevComparison::GT ? lhs > rhs : lhs == rhs;
    result = in.d[CH_A] + (pass ? in.c[CH_A] : 0);
  }

  m_regs[static_cast<u32>(ac.dest.Value())].a = ClampResult(result, ac.clamp);
}

void Tev::Draw()
{
  ASSERT(Position[0] >= 0 && Position[0] < s32(EFB_WIDTH));
  ASSERT(Position[1] >= 0 && Position[1] < s32(EFB_HEIGHT));

  INCSTAT(g_stats.this_frame.tev_pixels_in);

  // The four registers start each pixel with the values the game loaded.
  const auto& colors = Core::System::GetInstance().GetPixelShaderManager().constants.colors;
  for (size_t i = 0; i < m_regs.size(); ++i)
  {
    m_regs[i].r = static_cast<s16>(colors[i][0]);
    m_regs[i].g = static_cast<s16>(colors[i][1]);
    m_regs[i].b = static_cast<s16>(colors[i][2]);
    m_regs[i].a = static_cast<s16>(colors[i][3]);
  }

  const auto& gen = bpmem.genMode;

  // A texture coordinate number that has no texgen behind it reads coordinate 0. (Luigi's
  // Mansion's Mario portrait relies on this.)
  const auto coord_for = [&](u32 number) { return number < gen.numtexgens ? number : 0u; };

  // Indirect lookups.
  for (u32 stage = 0; stage < gen.numindstages; ++stage)
  {
    const TexCoord& uv = Uv[coord_for(bpmem.tevindref.getTexCoord(static_cast<int>(stage)))];
    const TEXSCALE& scale = bpmem.texscale[stage / 2];
    const bool odd = (stage & 1) != 0;
    const u32 shift_s = odd ? scale.ss1 : scale.ss0;
    const u32 shift_t = odd ? scale.ts1 : scale.ts0;
    TextureSampler::Sample(uv.s >> shift_s, uv.t >> shift_t, IndirectLod[stage],
                           IndirectLinear[stage],
                           static_cast<u8>(bpmem.tevindref.getTexMap(static_cast<int>(stage))),
                           m_indirect_texel[stage].data());
  }

  // TEV stages.
  for (u32 stage = 0; stage <= gen.numtevstages; ++stage)
  {
    const TwoTevStageOrders& order = bpmem.tevorders[stage / 2];
    const int odd = static_cast<int>(stage & 1);
    const auto& ac = bpmem.combiners[stage].alphaC;

    const TexCoord& uv = Uv[coord_for(order.getTexCoord(odd))];
    ApplyIndirect(stage, uv.s, uv.t);

    if (order.getEnable(odd))
    {
      std::array<u8, 4> texel{};  // R G B A; black when there are no texgens at all
      if (gen.numtexgens > 0)
      {
        TextureSampler::Sample(m_coord.s, m_coord.t, TextureLod[stage], TextureLinear[stage],
                               static_cast<u8>(order.getTexMap(odd)), texel.data());
      }
      m_tex_raw = Rgba{texel[CH_R], texel[CH_G], texel[CH_B], texel[CH_A]};
      const auto swap = bpmem.tevksel.GetSwapTable(ac.tswap);
      m_tex.r = texel[u32(swap[ColorChannel::Red])];
      m_tex.g = texel[u32(swap[ColorChannel::Green])];
      m_tex.b = texel[u32(swap[ColorChannel::Blue])];
      m_tex.a = texel[u32(swap[ColorChannel::Alpha])];
    }

    // Konst selections: the colour one sets r, g and b, the alpha one sets a.
    const u32 kcolor = static_cast<u32>(bpmem.tevksel.GetKonstColor(stage));
    const u32 kalpha = static_cast<u32>(bpmem.tevksel.GetKonstAlpha(stage));
    const auto konst_part = [this](u32 sel, size_t comp) -> s16 {
      if (sel < 8)
        return KONST_FRACTIONS[sel];
      if (sel < 12)
        return 0;
      if (sel < 16)
      {
        // Whole konst colour; it has no alpha.
        const Rgba& k = m_konst[sel - 12];
        const std::array<s16, 4> parts{k.r, k.g, k.b, 0};
        return parts[comp];
      }
      // One component of a konst colour, replicated.
      const Rgba& k = m_konst[(sel - 16) % 4];
      const std::array<s16, 4> parts{k.r, k.g, k.b, k.a};
      return parts[(sel - 16) / 4];
    };
    m_stage_konst.r = konst_part(kcolor, CH_R);
    m_stage_konst.g = konst_part(kcolor, CH_G);
    m_stage_konst.b = konst_part(kcolor, CH_B);
    m_stage_konst.a = konst_part(kalpha, CH_A);

    m_ras = RasterizedColor(static_cast<u32>(order.getColorChan(odd)), ac.rswap);

    const StageInputs inputs = GatherInputs(stage);
    CombineColor(stage, inputs);
    CombineAlpha(stage, inputs);
  }

  // The last stage's destinations go to the screen, whichever registers they are.
  const auto& last = bpmem.combiners[gen.numtevstages];
  const Rgba& color_out = m_regs[static_cast<u32>(last.colorC.dest.Value())];
  // Byte order A, B, G, R, as the EFB interface takes it.
  std::array<u8, 4> pixel{static_cast<u8>(m_regs[static_cast<u32>(last.alphaC.dest.Value())].a),
                          static_cast<u8>(color_out.b), static_cast<u8>(color_out.g),
                          static_cast<u8>(color_out.r)};
  constexpr size_t OUT_A = 0, OUT_B = 1, OUT_G = 2, OUT_R = 3;

  if (!PassesAlphaTest(pixel[OUT_A]))
    return;

  // Z textures replace or offset the depth with texture data.
  if (bpmem.ztex2.op != ZTexOp::Disabled)
  {
    u32 depth = bpmem.ztex1.bias;
    const u32 tr = static_cast<u32>(m_tex_raw.r);
    const u32 tg = static_cast<u32>(m_tex_raw.g);
    const u32 tb = static_cast<u32>(m_tex_raw.b);
    const u32 ta = static_cast<u32>(m_tex_raw.a);
    switch (bpmem.ztex2.type)
    {
    case ZTexFormat::U8:
      depth += ta;
      break;
    case ZTexFormat::U16:
      depth += (ta << 8) | tr;
      break;
    case ZTexFormat::U24:
      depth += (tr << 16) | (tg << 8) | tb;
      break;
    default:
      PanicAlertFmt("Invalid ztex format {}", bpmem.ztex2.type);
      break;
    }
    if (bpmem.ztex2.op == ZTexOp::Add)
      depth += static_cast<u32>(Position[2]);
    Position[2] = static_cast<s32>(depth & 0x00ffffff);
  }

  if (bpmem.fog.c_proj_fsel.fsel != FogType::Off)
  {
    const float density = FogDensity(FogDepth(Position[0], Position[2]));
    const u32 fog_weight = static_cast<u32>(density * 256);
    const u32 keep_weight = 256 - fog_weight;
    const auto& fog_color = bpmem.fog.color;
    pixel[OUT_R] = static_cast<u8>((pixel[OUT_R] * keep_weight + fog_weight * fog_color.r) >> 8);
    pixel[OUT_G] = static_cast<u8>((pixel[OUT_G] * keep_weight + fog_weight * fog_color.g) >> 8);
    pixel[OUT_B] = static_cast<u8>((pixel[OUT_B] * keep_weight + fog_weight * fog_color.b) >> 8);
  }

  const u16 x = static_cast<u16>(Position[0]);
  const u16 y = static_cast<u16>(Position[1]);

  if (bpmem.GetEmulatedZ() == EmulatedZ::Late)
  {
    EfbInterface::IncPerfCounterQuadCount(PQ_ZCOMP_INPUT);
    if (!EfbInterface::ZCompare(x, y, static_cast<u32>(Position[2])))
      return;
    EfbInterface::IncPerfCounterQuadCount(PQ_ZCOMP_OUTPUT);
  }

  // The bounding box grows by whole 2x2 quads, the unit the hardware rasterizes in.
  BBoxManager::Update(static_cast<u16>(Position[0] & ~1), static_cast<u16>(Position[0] | 1),
                      static_cast<u16>(Position[1] & ~1), static_cast<u16>(Position[1] | 1));

  INCSTAT(g_stats.this_frame.tev_pixels_out);
  EfbInterface::IncPerfCounterQuadCount(PQ_BLEND_INPUT);

  EfbInterface::BlendTev(x, y, pixel.data());
}
