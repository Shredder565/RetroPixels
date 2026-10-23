// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Fragment.glsl
//
// A copy of the built-in program in Shaders3D.h, for use as a starting point
// with the VertexShader / FragmentShader settings (-vert-shader, -frag-shader).

#version 120

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


// Legacy3D fragment shader, one texture map.

uniform sampler2D textureMap;

vec4 FetchTexel(vec2 at)
{
  return texture2D(textureMap, at);
}

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
