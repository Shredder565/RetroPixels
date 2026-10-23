// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Vertex.glsl
//
// A copy of the built-in program in Shaders3D.h, for use as a starting point
// with the VertexShader / FragmentShader settings (-vert-shader, -frag-shader).

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
