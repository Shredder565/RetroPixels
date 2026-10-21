// PixelU-Wii built-in output pass.
// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Runs before the user's post-processing shader (or on its own when there is none) whenever the
// picture needs resampling to the window, colour correction or HDR output. It is not offered in
// the shader list. Steps:
//   1. read the game picture at this output pixel with the chosen resampling method, in linear
//      light (the game's gamma, game_gamma, is undone first);
//   2. convert the game's colour space to the display's (BT.709/sRGB primaries);
//   3. for HDR output, scale SDR white to the chosen paper white;
//   4. encode: linear for scRGB/HDR, else sRGB or a custom display gamma when gamma correction
//      is on, else back to the game's own gamma.
// resampling_method values follow OutputResamplingMode in VideoCommon/VideoConfig.h.

// ---------------------------------------------------------------- colour spaces

// Matrices from linear game RGB to linear BT.709 RGB, computed as inverse(M_709) * M_source,
// where M is the usual RGB->XYZ matrix built from a set of primaries and a white point:
//   BT.709:            R (0.640, 0.330)  G (0.300, 0.600)  B (0.150, 0.060)  white D65
//   SMPTE 170M (NTSC): R (0.630, 0.340)  G (0.310, 0.595)  B (0.155, 0.070)  white D65
//   EBU 3213 (PAL):    R (0.640, 0.330)  G (0.290, 0.600)  B (0.150, 0.060)  white D65
//   NTSC-J:            SMPTE 170M primaries with the 9300K+27MPCD white of Japanese sets,
//                      (0.2838, 0.2984), reproduced as it looked (no white adaptation), so
//                      that white stays bluish; its luminance is kept at 1.
// Each matrix is given by its rows.

float3 GamutRow(int space, int row)
{
  if (space == 0)  // NTSC-M
  {
    if (row == 0)
      return float3(0.939542, 0.050181, 0.010277);
    if (row == 1)
      return float3(0.017772, 0.965793, 0.016435);
    return float3(-0.001622, -0.004370, 1.005991);
  }
  if (space == 1)  // NTSC-J
  {
    if (row == 0)
      return float3(0.782561, 0.050697, 0.013633);
    if (row == 1)
      return float3(0.014803, 0.975723, 0.021803);
    return float3(-0.001351, -0.004415, 1.334598);
  }
  // PAL
  if (row == 0)
    return float3(1.044043, -0.044043, 0.0);
  if (row == 1)
    return float3(0.0, 1.0, 0.0);
  return float3(0.0, 0.011793, 0.988207);
}

float3 ToDisplayGamut(float3 c)
{
  return float3(dot(GamutRow(game_color_space, 0), c), dot(GamutRow(game_color_space, 1), c),
                dot(GamutRow(game_color_space, 2), c));
}

// ---------------------------------------------------------------- transfer functions

float3 DecodeGamma(float3 c, float g)
{
  return pow(max(c, float3(0.0, 0.0, 0.0)), float3(g, g, g));
}

float3 EncodeGamma(float3 c, float g)
{
  float inv = 1.0 / g;
  return pow(max(c, float3(0.0, 0.0, 0.0)), float3(inv, inv, inv));
}

float EncodeSRGBChannel(float v)
{
  v = max(v, 0.0);
  if (v < 0.0031308)
    return 12.92 * v;
  return 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float3 EncodeSRGB(float3 c)
{
  return float3(EncodeSRGBChannel(c.r), EncodeSRGBChannel(c.g), EncodeSRGBChannel(c.b));
}

// ---------------------------------------------------------------- source texels

// The source region in texels: x, y = first texel, z, w = one past the last.
float4 SourceTexelBounds()
{
  float2 size = GetResolution();
  float2 lo = src_rect.xy * size;
  return float4(lo, lo + src_rect.zw * size);
}

// One source texel (integer coordinates, clamped to the source region), in linear light.
float4 Texel(float2 xy)
{
  float4 bounds = SourceTexelBounds();
  float2 p = clamp(floor(xy), bounds.xy, bounds.zw - float2(1.0, 1.0));
  float4 c = texture(samp1, float3((p + float2(0.5, 0.5)) * GetInvResolution(), v_tex0.z));
  return float4(DecodeGamma(c.rgb, game_gamma), c.a);
}

// How many source texels one output pixel covers, per axis.
float2 TexelsPerPixel()
{
  return src_rect.zw * GetResolution() * GetInvWindowResolution();
}

// ---------------------------------------------------------------- resampling methods

// Linear-light bilinear between the four texel centres around t (in texel units).
float4 BilinearAt(float2 t)
{
  float2 base = floor(t - float2(0.5, 0.5));
  float2 f = t - float2(0.5, 0.5) - base;
  float4 top = mix(Texel(base), Texel(base + float2(1.0, 0.0)), f.x);
  float4 bottom = mix(Texel(base + float2(0.0, 1.0)), Texel(base + float2(1.0, 1.0)), f.x);
  return mix(top, bottom, f.y);
}

// The Mitchell-Netravali family of cubic filters, k(x) for distance x from a texel centre.
// (B, C) = (1, 0) is the cubic B-spline, (1/3, 1/3) Mitchell-Netravali, (0, 1/2) Catmull-Rom.
float CubicWeight(float x, float b, float c)
{
  x = abs(x);
  float x2 = x * x;
  float x3 = x2 * x;
  if (x < 1.0)
    return ((12.0 - 9.0 * b - 6.0 * c) * x3 + (-18.0 + 12.0 * b + 6.0 * c) * x2 + (6.0 - 2.0 * b)) /
           6.0;
  if (x < 2.0)
    return ((-b - 6.0 * c) * x3 + (6.0 * b + 30.0 * c) * x2 + (-12.0 * b - 48.0 * c) * x +
            (8.0 * b + 24.0 * c)) /
           6.0;
  return 0.0;
}

float4 CubicAt(float2 t, float b, float c)
{
  float2 base = floor(t - float2(0.5, 0.5));
  float2 f = t - float2(0.5, 0.5) - base;
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  float total = 0.0;
  for (int j = -1; j <= 2; ++j)
  {
    float wy = CubicWeight(float(j) - f.y, b, c);
    for (int i = -1; i <= 2; ++i)
    {
      float w = wy * CubicWeight(float(i) - f.x, b, c);
      sum += w * Texel(base + float2(float(i), float(j)));
      total += w;
    }
  }
  // Sharpening filters can overshoot below zero; light cannot.
  float4 c4 = sum / total;
  return float4(max(c4.rgb, float3(0.0, 0.0, 0.0)), c4.a);
}

// Nearest-texel scaling by the largest whole factor that fits, then a narrow linear blend at the
// texel edges: sharp blocks without the uneven widths of plain nearest-neighbour scaling.
float4 SharpBilinearAt(float2 t)
{
  float2 per_pixel = TexelsPerPixel();
  float2 factor = max(floor(float2(1.0, 1.0) / per_pixel), float2(1.0, 1.0));
  float scale = min(factor.x, factor.y);
  // The nearest texel edge, and how far the blend from the texel before it to the one after it
  // has got at t: 0 over most of a texel, rising across a band of 1/scale texels at the edge.
  float2 edge = floor(t + float2(0.5, 0.5));
  float2 w = clamp((t - edge) * scale + float2(0.5, 0.5), float2(0.0, 0.0), float2(1.0, 1.0));
  // Texel centres sit at edge - 0.5 and edge + 0.5.
  return BilinearAt(edge - float2(0.5, 0.5) + w);
}

// The average of every texel under the output pixel, each weighted by how much of it the pixel
// covers (a box filter): exact for downscaling, a soft-edged sharp scale for upscaling.
float4 AreaAt(float2 t)
{
  float2 half_size = TexelsPerPixel() * 0.5;
  float2 lo = t - half_size;
  float2 hi = t + half_size;
  float2 first = floor(lo);
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  float total = 0.0;
  // Up to 16 texels per axis are read (a downscale of 15:1 or less is exact).
  for (int j = 0; j < 16; ++j)
  {
    float y = first.y + float(j);
    float wy = min(hi.y, y + 1.0) - max(lo.y, y);
    if (wy > 0.0)
    {
      for (int i = 0; i < 16; ++i)
      {
        float x = first.x + float(i);
        float wx = min(hi.x, x + 1.0) - max(lo.x, x);
        if (wx > 0.0)
        {
          sum += (wx * wy) * Texel(float2(x, y));
          total += wx * wy;
        }
      }
    }
  }
  return sum / max(total, 1e-6);
}

float4 ResampledColor()
{
  float2 t = v_tex0.xy * GetResolution();
  float2 per_pixel = TexelsPerPixel();
  bool same_size = all(lessThan(abs(per_pixel - float2(1.0, 1.0)), float2(0.001, 0.001)));

  if (resampling_method <= 0 || same_size)
  {
    // "Default": the GPU's own bilinear filter on the gamma-encoded picture (the same as
    // nearest when nothing is scaled).
    float4 c = same_size ? texture(samp1, v_tex0) : texture(samp0, v_tex0);
    return float4(DecodeGamma(c.rgb, game_gamma), c.a);
  }
  if (resampling_method == 1)
    return BilinearAt(t);
  if (resampling_method == 2)
    return CubicAt(t, 1.0, 0.0);
  if (resampling_method == 3)
    return CubicAt(t, 1.0 / 3.0, 1.0 / 3.0);
  if (resampling_method == 4)
    return CubicAt(t, 0.0, 0.5);
  if (resampling_method == 5)
    return SharpBilinearAt(t);
  return AreaAt(t);
}

// ---------------------------------------------------------------- the pass

void main()
{
  float4 color = ResampledColor();

  if (OptionEnabled(correct_color_space))
    color.rgb = ToDisplayGamut(color.rgb);

  if (OptionEnabled(hdr_output))
    color.rgb *= hdr_paper_white_nits / hdr_sdr_white_nits;

  if (OptionEnabled(linear_space_output))
  {
    // scRGB: stays linear.
  }
  else if (OptionEnabled(correct_gamma))
  {
    color.rgb = OptionEnabled(sdr_display_gamma_sRGB) ? EncodeSRGB(color.rgb) :
                                                        EncodeGamma(color.rgb, sdr_display_custom_gamma);
  }
  else
  {
    color.rgb = EncodeGamma(color.rgb, game_gamma);
  }

  SetOutput(color);
}
