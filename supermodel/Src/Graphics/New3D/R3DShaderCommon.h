// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DShaderCommon.h
//
// Fragment shader code shared by the triangle and quad pipelines: texel
// formats, texture filtering and mipmapping as the Real3D does them,
// spotlight, fog, lighting and the output layers. It is appended to the
// pipeline-specific fragment shader, which provides these values for the
// current fragment before calling ShadeFragment():
//   vec3 vViewPos, vViewNormal; vec2 vTexCoord; vec4 vColour;
//   float vFixedShade, vLodBase;
// and declares uProjMat if it needs it.

#ifndef INCLUDED_NEW3D_R3DSHADERCOMMON_H
#define INCLUDED_NEW3D_R3DSHADERCOMMON_H

static const char *fragmentShaderR3DCommon = R"glsl(

uniform usampler2D uTexSheet[2];

uniform bool  uTexEnabled;
uniform bool  uMicroEnabled;
uniform float uMicroMinLOD;
uniform int   uMicroID;
uniform ivec4 uTexRect;         // x, y in the sheet; width, height
uniform int   uTexFormat;
uniform bool  uTexInvert;
uniform bool  uTexAlpha;
uniform bool  uContour;
uniform bool  uOpaquePass;      // first pass: only fully opaque texels
uniform ivec2 uTexWrap;         // 0 repeat, 1 repeat+clamp, 2/3 mirror
uniform int   uTexPage;

uniform vec3  uFogColour;
uniform vec4  uSpotShape;       // window x, y, half width, half height
uniform vec2  uSpotRange;       // start, extent (view space Z)
uniform vec3  uSpotColour;
uniform vec3  uSpotFogColour;
uniform vec3  uLighting[2];     // [0] sun direction, [1].x sun intensity, [1].y ambient
uniform bool  uLit;
uniform bool  uSunClamp;
uniform bool  uIntensityClamp;
uniform bool  uSpecular;
uniform float uSpecularValue;
uniform float uShininess;
uniform float uFogIntensity;
uniform float uFogDensity;
uniform float uFogStart;
uniform float uFogAttenuation;
uniform float uFogAmbient;
uniform bool  uFixedShading;
uniform bool  uSmoothShading;
uniform int   uHardwareStep;
uniform int   uOutputLayer;     // 0 opaque, 1 / 2 translucency layer

layout(location = 0) out vec4 outOpaque;
layout(location = 1) out vec4 outTrans1;
layout(location = 2) out vec4 outTrans2;

// ---- texel formats -------------------------------------------------------

// Formats 8-11 are 1-4 with contour processing: one nibble is the grey
// level and the top value becomes transparent.
vec4 NibbleContour(uint nibble)
{
  vec4 c = vec4(float(nibble) / 15.0);
  c.a = (c.a == 1.0) ? 0.0 : 1.0;
  return c;
}

vec4 ByteContour(uint byteValue)
{
  vec4 c = vec4(float(byteValue) / 255.0);
  c.a = (c.a == 1.0) ? 0.0 : 1.0;
  return c;
}

vec4 GreyAlpha(uint grey, uint alpha)
{
  vec4 c = vec4(vec3(float(grey)), float(alpha));
  return c * (1.0 / 15.0);
}

vec4 DecodeTexel(uint t)
{
  switch (uTexFormat)
  {
  case 0: {   // TRGB 1555
    vec4 c;
    c.rgb = vec3(float((t >> 10) & 31u), float((t >> 5) & 31u), float(t & 31u)) * (1.0 / 31.0);
    c.a = 1.0 - float((t >> 15) & 1u);
    return c;
  }
  case 1:  return GreyAlpha(t & 15u, (t >> 4) & 15u);           // low byte: A4 L4
  case 2:  return GreyAlpha((t >> 4) & 15u, t & 15u);           // low byte: L4 A4
  case 3:  return GreyAlpha((t >> 8) & 15u, (t >> 12) & 15u);   // high byte: A4 L4
  case 4:  return GreyAlpha((t >> 12) & 15u, (t >> 8) & 15u);   // high byte: L4 A4
  case 5:  return ByteContour(t & 255u);                        // L8 low byte
  case 6:  return ByteContour((t >> 8) & 255u);                 // L8 high byte
  case 7:  return vec4(float((t >> 12) & 15u), float((t >> 8) & 15u), float((t >> 4) & 15u), float(t & 15u)) * (1.0 / 15.0);
  case 8:  return NibbleContour(t & 15u);
  case 9:  return NibbleContour((t >> 4) & 15u);
  case 10: return NibbleContour((t >> 8) & 15u);
  case 11: return NibbleContour((t >> 12) & 15u);
  default: return vec4(0.0);
  }
}

// ---- filtering -----------------------------------------------------------

// For one axis: the two texel centres (0-1 range within the texture) to
// blend between, and the blend weight.
float FilterTaps(int wrap, float size, float u, out float tap0, out float tap1)
{
  if (wrap == 0)
  {
    float t = u * size - 0.5;
    tap0 = fract((floor(t) + 0.5) / size);
    tap1 = fract(tap0 + 1.0 / size);
    return fract(t);
  }

  if (wrap == 1)
    u = fract(u);
  else if (floor(mod(u, 2.0)) > 0.0)   // mirrored: odd repeats run backwards
    u = 1.0 - fract(u);
  else
    u = fract(u);

  float t = u * size - 0.5;
  tap0 = (floor(t) + 0.5) / size;
  tap1 = tap0 + 1.0 / size;
  if (tap0 < 0.0)
    tap0 = 0.0;
  if (tap1 >= 1.0)
    tap1 = 1.0 - 0.5 / size;
  return fract(t);
}

vec4 FetchTexel(usampler2D sheet, vec2 at, vec2 size, ivec2 origin, int level)
{
  ivec2 p = ivec2(at * size + vec2(origin));
  p.x &= (2047 >> level);
  p.y &= (1023 >> level);
  return DecodeTexel(texelFetch(sheet, p, level).r);
}

vec4 Bilinear(usampler2D sheet, ivec2 wrap, vec2 size, ivec2 origin, vec2 uv, int level)
{
  float u0, u1, v0, v1;
  float wu = FilterTaps(wrap.x, size.x, uv.x, u0, u1);
  float wv = FilterTaps(wrap.y, size.y, uv.y, v0, v1);

  vec4 c00 = FetchTexel(sheet, vec2(u0, v0), size, origin, level);
  vec4 c10 = FetchTexel(sheet, vec2(u1, v0), size, origin, level);
  vec4 c01 = FetchTexel(sheet, vec2(u0, v1), size, origin, level);
  vec4 c11 = FetchTexel(sheet, vec2(u1, v1), size, origin, level);

  // Contour textures: transparent texels take the colour of a more opaque
  // neighbour, so edges do not darken.
  if (uContour)
  {
    if (c00.a > c10.a) c10.rgb = c00.rgb;
    if (c00.a > c01.a) c01.rgb = c00.rgb;
    if (c10.a > c00.a) c00.rgb = c10.rgb;
    if (c10.a > c11.a) c11.rgb = c10.rgb;
    if (c01.a > c00.a) c00.rgb = c01.rgb;
    if (c01.a > c11.a) c11.rgb = c01.rgb;
    if (c11.a > c01.a) c01.rgb = c11.rgb;
    if (c11.a > c10.a) c10.rgb = c11.rgb;
  }

  return mix(mix(c00, c10, wu), mix(c01, c11, wu), wv);
}

ivec2 MicroTextureOrigin(int id)
{
  // Eight 128x128 microtextures in a 2x4 block at the other sheet's top
  // left: bit 0 of the number picks the row, bit 1 the column, bit 2 the
  // lower half of the block.
  int col = (id >> 1) & 1;
  int row = (id & 1) + ((id >> 2) & 1) * 2;
  return ivec2(col * 128, row * 128);
}

vec4 SampleTexture()
{
  // Level of detail from the vertex term and the fragment's 1/w.
  float lod = -log2(gl_FragCoord.w * gl_FragCoord.w * vLodBase);
  // Mipmaps go down to 2 texels on the smaller side.
  float lastLevel = floor(log2(min(float(uTexRect.z), float(uTexRect.w)))) - 1.0;
  float level = clamp(lod, 0.0, lastLevel);
  int base = int(level);

  vec4 c = Bilinear(uTexSheet[uTexPage], uTexWrap, vec2(uTexRect.zw / (1 << base)), uTexRect.xy / (1 << base), vTexCoord, base);
  vec4 second = vec4(0.0);
  float amount = 0.0;

  float between = fract(level);
  if (between > 0.0)
  {
    int next = base + 1;
    second = Bilinear(uTexSheet[uTexPage], uTexWrap, vec2(uTexRect.zw / (1 << next)), uTexRect.xy / (1 << next), vTexCoord, next);
    amount = between;
  }
  else if (uMicroEnabled && lod < 0.0)
  {
    // Close up, a 128x128 detail texture from the other sheet is blended in.
    const float detailScale[4] = float[4](2.0, 4.0, 16.0, 256.0);
    vec2 scale = (vec2(uTexRect.zw) / 128.0) * detailScale[int(uMicroMinLOD)];
    second = Bilinear(uTexSheet[(uTexPage + 1) & 1], ivec2(0), vec2(ivec2(128)), MicroTextureOrigin(uMicroID), vTexCoord * scale, 0);
    amount = min(-lod * exp2(-uMicroMinLOD) * 0.5, 0.5);
  }

  c = mix(c, second, amount);

  if (uTexInvert)
    c.rgb = vec3(1.0) - c.rgb;

  if (uContour && c.a < (32.0 / 255.0))
    discard;

  if (uTexAlpha)
  {
    // Opaque pass keeps only solid texels; translucent pass the rest.
    if (uOpaquePass ? (c.a < 1.0) : (c.a * vColour.a >= 1.0))
      discard;
  }

  if (!uTexAlpha && !uContour)
    c.a = 1.0;

  return c;
}

// ---- lighting and fog ----------------------------------------------------

void SpotlightTerms(out float light, out float fogGlow)
{
  vec2 d = (gl_FragCoord.xy - uSpotShape.xy) / uSpotShape.zw;
  float shape = max(0.0, 1.0 - (d.x * d.x + d.y * d.y));
  float inRange = step(uSpotRange.x, -vViewPos.z);

  float falloff = 0.0;
  if (uSpotRange.y != 0.0)
  {
    float extent = abs(uSpotRange.y);
    float past = min(uSpotRange.x + extent + vViewPos.z, 0.0);
    float k = past * (1.0 / (1.0 + extent)) - 1.0;
    falloff = (1.0 / (k * k)) * inRange;
  }

  light = falloff * shape;
  fogGlow = inRange * shape;
}

void ShadeFragment()
{
  float fogAmount = uFogIntensity * clamp(uFogStart + (-vViewPos.z) * uFogDensity, 0.0, 1.0);
  vec3 fogRGB = uFogColour * uFogAmbient;

  vec4 texel = uTexEnabled ? SampleTexture() : vec4(1.0);

  vec4 colour = vColour;
  // Step 1.5 boards brighten luminous polygons.
  if (uHardwareStep == 0x15 && !uLit)
    colour.rgb *= uFixedShading ? (1.0 + vFixedShade + uLighting[1].y) : 1.5;

  vec4 result = texel * colour;
  if (result.a < (1.0 / 32.0))
    discard;

  float spotLight, spotFog;
  SpotlightTerms(spotLight, spotFog);

  if (uLit)
  {
    vec3 sun = uLighting[0];
    float diffuse = uFixedShading ? vFixedShade : dot(sun, vViewNormal);
    diffuse = clamp(diffuse, -1.0, 1.0);
    if (uSunClamp || result.a < 0.999)
      diffuse = max(diffuse, 0.0);

    vec3 intensity = vec3(diffuse * uLighting[1].x + uLighting[1].y);
    intensity += uSpotColour * spotLight;
    if (uIntensityClamp)
      intensity = min(intensity, 1.0);
    result.rgb *= intensity;

    if (uSpecular)
    {
      float highlight;
      if (uSmoothShading)
      {
        const float power[4] = float[4](8.0, 16.0, 32.0, 64.0);
        const float gain[4] = float[4](1.6, 1.6, 2.4, 3.2);
        int s = int(uShininess);
        highlight = pow(max(0.0, diffuse), power[s]) * gain[s];
      }
      else
      {
        // Flat shading: reflected sun against a viewer straight down -Z.
        highlight = max(0.0, reflect(-sun, vViewNormal).z);
      }
      highlight *= uSpecularValue;
      highlight *= uLighting[1].x;

      if (colour.a < 1.0)
        result.a = max(result.a, highlight);
      result.rgb += highlight;
    }
  }

  result.rgb = min(result.rgb, vec3(1.0));
  vec3 glow = uSpotFogColour * uFogAttenuation * uFogColour * spotFog;
  result.rgb = mix(result.rgb, fogRGB + glow, fogAmount);

  outOpaque = vec4(0.0);
  outTrans1 = vec4(0.0);
  outTrans2 = vec4(0.0);
  if (uOutputLayer == 0)
    outOpaque = vec4(result.rgb, 1.0);
  else if (uOutputLayer == 1)
    outTrans1 = result;
  else if (uOutputLayer == 2)
    outTrans2 = result;
}

)glsl";

#endif  // INCLUDED_NEW3D_R3DSHADERCOMMON_H
