// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Shaders2D.h
//
// Shaders that draw a 496x384 tile generator layer over the screen area.
// The fragment shader is compiled with UPSCALEMODE defined to the
// UpscaleMode value: 0 nearest / 2 bilinear (plain sampling with the
// texture's filter), 1 biquintic (smoothed bilinear), 3 bicubic B-spline.

#ifndef INCLUDED_SHADERS2D_H
#define INCLUDED_SHADERS2D_H

// A full-area triangle strip from gl_VertexID. The layer's first line is
// the top of the screen, so V runs downward.
static constexpr char s_vertexShader[] = R"glsl(
#version 410 core

out vec2 vUV;

void main(void)
{
  vec2 corner = vec2(float((gl_VertexID & 2) >> 1), float(gl_VertexID & 1));
  vUV = vec2(corner.x, 1.0 - corner.y);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

static constexpr char s_fragmentShaderHeader[] = R"glsl(
#version 410 core
)glsl";

static constexpr char s_fragmentShader[] = R"glsl(
uniform sampler2D tex1;
in vec2 vUV;
out vec4 fragColor;

#if UPSCALEMODE == 1
// Bilinear sampling with the position inside each texel cell pushed through
// the quintic smootherstep, so texels blend only near their edges.
vec4 SampleBiquintic(sampler2D s, vec2 uv)
{
  vec2 size = vec2(textureSize(s, 0));
  vec2 p = uv * size + 0.5;
  vec2 cell = floor(p);
  vec2 t = p - cell;
  vec2 eased = t * t * t * (t * (t * 6.0 - 15.0) + 10.0) - 0.5;
  return texture(s, (cell + eased) / size);
}
#endif

#if UPSCALEMODE == 3
// Uniform cubic B-spline weights for the 4 texels around a sample at
// fraction f past the second one, from the cubes of the distances to the
// four knots.
vec4 SplineWeights(float f)
{
  vec4 d = vec4(1.0, 2.0, 3.0, 4.0) - f;
  vec4 d3 = d * d * d;
  float w0 = d3.x;
  float w1 = d3.y - 4.0 * d3.x;
  float w2 = d3.z - 4.0 * d3.y + 6.0 * d3.x;
  return vec4(w0, w1, w2, 6.0 - w0 - w1 - w2) * (1.0 / 6.0);
}

// The 4x4 filter as 4 bilinear fetches: each pair of texels on an axis is
// fetched at the point between them that gives their weight ratio.
vec4 SampleBicubic(sampler2D s, vec2 uv)
{
  vec2 size = vec2(textureSize(s, 0));
  vec2 texel = 1.0 / size;
  vec2 p = uv * size - 0.5;
  vec2 cell = floor(p);
  vec2 f = p - cell;
  vec4 wx = SplineWeights(f.x);
  vec4 wy = SplineWeights(f.y);

  // x: low pair, high pair; then y the same
  vec4 pairWeight = vec4(wx.xz + wx.yw, wy.xz + wy.yw);
  vec4 pos = (cell.xxyy + vec2(-0.5, 1.5).xyxy + vec4(wx.yw, wy.yw) / pairWeight) * texel.xxyy;

  vec4 lowLow = texture(s, pos.xz);
  vec4 highLow = texture(s, pos.yz);
  vec4 lowHigh = texture(s, pos.xw);
  vec4 highHigh = texture(s, pos.yw);
  float lowXShare = pairWeight.x / (pairWeight.x + pairWeight.y);
  float lowYShare = pairWeight.z / (pairWeight.z + pairWeight.w);
  vec4 highRow = mix(highHigh, lowHigh, lowXShare);
  vec4 lowRow = mix(highLow, lowLow, lowXShare);
  return mix(highRow, lowRow, lowYShare);
}
#endif

void main(void)
{
#if UPSCALEMODE == 1
  fragColor = vec4(SampleBiquintic(tex1, vUV).rgb, texture(tex1, vUV).a);   // alpha edges stay as sampled
#elif UPSCALEMODE == 3
  fragColor = SampleBicubic(tex1, vUV);
#else
  fragColor = texture(tex1, vUV);
#endif
}
)glsl";

#endif  // INCLUDED_SHADERS2D_H
