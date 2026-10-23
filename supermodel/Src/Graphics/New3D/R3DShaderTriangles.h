// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DShaderTriangles.h
//
// Vertex and fragment shader for drawing Real3D polygons as triangles (the
// default pipeline). The fragment part is completed by
// fragmentShaderR3DCommon.

#ifndef INCLUDED_NEW3D_R3DSHADERTRIANGLES_H
#define INCLUDED_NEW3D_R3DSHADERTRIANGLES_H

static const char *vertexShaderR3D = R"glsl(

#version 410 core

uniform float uModelScale;
uniform float uNodeAlpha;
uniform float uCota;
uniform mat4  uModelMat;
uniform mat4  uProjMat;
uniform bool  uTranslatorMap;

in vec4  aPosition;
in vec3  aNormal;
in vec2  aTexCoord;
in vec3  aFaceNormal;
in float aFixedShade;
in vec4  aColour;
in float aTextureNP;

out vec3  vViewPos;
out vec3  vViewNormal;
out vec2  vTexCoord;
out vec4  vColour;
out float vFixedShade;
out float vFacing;      // > 0: the polygon faces away and is not drawn
out float vLodBase;

void main(void)
{
  vViewPos = (uModelMat * aPosition).xyz;
  vViewNormal = (mat3(uModelMat) / uModelScale) * aNormal;
  vFacing = dot(vViewPos, mat3(uModelMat) * aFaceNormal);

  vColour = aColour;
  if (uTranslatorMap)
    vColour.rgb *= 16.0;
  vColour.a *= uNodeAlpha;

  vTexCoord = aTexCoord;
  vFixedShade = aFixedShade;
  vLodBase = vFacing * -uCota * aTextureNP;
  gl_Position = (uProjMat * uModelMat) * aPosition;
}
)glsl";

static const char *fragmentShaderR3D = R"glsl(

#version 410 core

in vec3  vViewPos;
in vec3  vViewNormal;
in vec2  vTexCoord;
in vec4  vColour;
in float vFixedShade;
in float vFacing;
in float vLodBase;

uniform mat4 uProjMat;

void ShadeFragment();

void main(void)
{
  if (vFacing > 0.0)
    discard;      // the Real3D culls back faces by the polygon's own normal

  // Reversed depth: near / -z.
  gl_FragDepth = uProjMat[3][2] * gl_FragCoord.w;
  ShadeFragment();
}
)glsl";

#endif  // INCLUDED_NEW3D_R3DSHADERTRIANGLES_H
