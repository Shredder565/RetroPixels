// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Shaders3D.h
//
// The Legacy3D GLSL 1.20 programs. The renderer looks up the uniforms and
// attributes by these names; shaders loaded from files (VertexShader /
// FragmentShader settings) must use them too, and should keep the fs*
// varyings so a file shader can be paired with a built-in one. The files in
// Shaders/ hold the same programs plus two simpler fragment variants.

#ifndef INCLUDED_SHADERS3D_H
#define INCLUDED_SHADERS3D_H

namespace Legacy3D {

static constexpr char vertexShaderSource[] = R"glsl(
#version 120

// Legacy3D vertex shader: transform, sun light and fog per vertex.

uniform mat4 modelViewMatrix;
uniform mat4 projectionMatrix;
uniform vec3 lighting[2];        // [0] sun direction (Real3D view axes); [1].x sun level, [1].y ambient level

attribute vec4 subTexture;       // texture x, y in its map, width, height (texels)
attribute vec4 texParams;        // textured (1/0), contour test (>0), mirror U, mirror V
attribute float texFormat;
attribute float texMap;
attribute float transLevel;
attribute float lightEnable;
attribute float specular;
attribute float shininess;       // < 0: no highlight
attribute float fogIntensity;

varying vec4 fsSubTexture;
varying vec4 fsTexParams;
varying float fsTexFormat;
varying float fsTexMap;
varying float fsTransLevel;
varying vec3 fsLightIntensity;
varying float fsSpecularTerm;
varying float fsFogFactor;
varying float fsViewZ;           // depth into the screen, view units

void main(void)
{
  vec3 eye = vec3(modelViewMatrix * gl_Vertex);
  gl_Position = projectionMatrix * modelViewMatrix * gl_Vertex;

  // Untextured polygons show their colour; textured ones use it to tint
  // the light that reaches the texels.
  gl_FrontColor = vec4(gl_Color.rgb, 1.0);
  vec3 light = vec3(1.0);
  if (texParams.x > 0.5)
    light *= gl_Color.rgb;

  float highlight = 0.0;
  if (lightEnable > 0.5)
  {
    mat3 rotation = mat3(modelViewMatrix[0].xyz, modelViewMatrix[1].xyz, modelViewMatrix[2].xyz);
    vec3 n = normalize(rotation * gl_Normal);
    vec3 toSun = lighting[0] * vec3(1.0, -1.0, -1.0);
    float facing = max(dot(toSun, n), 0.0);
    light *= min(1.0, facing * lighting[1].x + lighting[1].y);
    if (shininess >= 0.0 && facing > 0.0)
    {
      vec3 halfway = normalize(toSun + normalize(-eye));
      highlight = specular * pow(max(dot(n, halfway), 0.0), shininess);
    }
  }
  fsLightIntensity = light;
  fsSpecularTerm = highlight;

  fsFogFactor = fogIntensity * clamp(gl_Fog.start + length(eye) * gl_Fog.density, 0.0, 1.0);
  fsViewZ = -eye.z;

  gl_TexCoord[0] = gl_MultiTexCoord0;
  fsSubTexture = subTexture;
  fsTexParams = texParams;
  fsTexFormat = texFormat;
  fsTexMap = texMap;
  fsTransLevel = transLevel;
}
)glsl";

// Both fragment shaders are #version 120, these declarations, their own
// FetchTexel() for the texture map(s), then the shared main().
#define LEGACY3D_FRAGMENT_INPUTS R"glsl(
uniform vec4 spotEllipse;        // centre x, y and half sizes, window pixels
uniform vec2 spotRange;          // lit between these view depths
uniform vec3 spotColor;
uniform float mapSize;           // texture map edge in texels

varying vec4 fsSubTexture;
varying vec4 fsTexParams;
varying float fsTexFormat;
varying float fsTexMap;
varying float fsTransLevel;
varying vec3 fsLightIntensity;
varying float fsSpecularTerm;
varying float fsFogFactor;
varying float fsViewZ;

)glsl"

#define LEGACY3D_FRAGMENT_MAIN R"glsl(
// Two texel positions (xy and zw) inside the texture, repeated or mirrored,
// as map coordinates.
vec4 InTexture(vec4 texel)
{
  vec4 size = fsSubTexture.zwzw;
  vec4 wrapped = mod(texel, size);
  vec4 flip = fsTexParams.zwzw * mod(floor(texel / size), 2.0);
  return (flip * (size - wrapped) + (vec4(1.0) - flip) * wrapped + fsSubTexture.xyxy) / mapSize;
}

vec4 Lerp(vec4 a, vec4 b, float t)
{
  return a + (b - a) * t;
}

void main(void)
{
  vec4 colour;
  if (fsTexParams.x < 0.5)
    colour = gl_Color;
  else
  {
    // Bilinear filter by hand so that it wraps inside the texture.
    vec2 base = gl_TexCoord[0].st - vec2(0.5);
    vec2 frac = base - floor(base);
    vec4 low = vec4(floor(base) + vec2(0.5), 0.0, 0.0);
    low.zw = low.xy + vec2(1.0, 0.0);
    vec4 high = low + vec4(0.0, 1.0, 0.0, 1.0);
    low = InTexture(low);
    high = InTexture(high);
    vec4 bottom = Lerp(FetchTexel(low.xy), FetchTexel(low.zw), frac.s);
    vec4 top = Lerp(FetchTexel(high.xy), FetchTexel(high.zw), frac.s);
    colour = Lerp(bottom, top, frac.t);

    if (fsTexParams.y > 0.5 && colour.a < 0.01)
      discard;
    if (fsTexFormat < 0.5)
      colour.a = 1.0;   // T1RGB5: the T bit only cuts out
  }

  vec2 d = (gl_FragCoord.xy - spotEllipse.xy) / spotEllipse.zw;
  float r2 = dot(d, d);
  vec3 light = fsLightIntensity;
  if (r2 <= 1.0 && fsViewZ >= spotRange.x && fsViewZ < spotRange.y)
    light = fsLightIntensity + (1.0 - r2) * spotColor;
  colour.rgb *= light;
  colour.rgb += vec3(fsSpecularTerm);
  colour.a *= fsTransLevel;
  colour.rgb = mix(colour.rgb, gl_Fog.color.rgb, fsFogFactor);
  gl_FragColor = colour;
}
)glsl"

// One map holding every sheet.
static constexpr char fragmentShaderSingleSheetSource[] = "#version 120\n" LEGACY3D_FRAGMENT_INPUTS R"glsl(
// Legacy3D fragment shader, one texture map.

uniform sampler2D textureMap;

vec4 FetchTexel(vec2 at)
{
  return texture2D(textureMap, at);
}
)glsl" LEGACY3D_FRAGMENT_MAIN;

// A map per sheet, picked by the texMap attribute.
static constexpr char fragmentShaderMultiSheetSource[] = "#version 120\n" LEGACY3D_FRAGMENT_INPUTS R"glsl(
// Legacy3D fragment shader, up to eight texture maps.

uniform sampler2D textureMap0;
uniform sampler2D textureMap1;
uniform sampler2D textureMap2;
uniform sampler2D textureMap3;
uniform sampler2D textureMap4;
uniform sampler2D textureMap5;
uniform sampler2D textureMap6;
uniform sampler2D textureMap7;

vec4 FetchTexel(vec2 at)
{
  if (fsTexMap < 0.5) return texture2D(textureMap0, at);
  if (fsTexMap < 1.5) return texture2D(textureMap1, at);
  if (fsTexMap < 2.5) return texture2D(textureMap2, at);
  if (fsTexMap < 3.5) return texture2D(textureMap3, at);
  if (fsTexMap < 4.5) return texture2D(textureMap4, at);
  if (fsTexMap < 5.5) return texture2D(textureMap5, at);
  if (fsTexMap < 6.5) return texture2D(textureMap6, at);
  return texture2D(textureMap7, at);
}
)glsl" LEGACY3D_FRAGMENT_MAIN;

#undef LEGACY3D_FRAGMENT_INPUTS
#undef LEGACY3D_FRAGMENT_MAIN

} // Legacy3D

#endif  // INCLUDED_SHADERS3D_H
