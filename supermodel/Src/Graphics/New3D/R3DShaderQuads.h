// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DShaderQuads.h
//
// Shaders for the QuadRendering option: Real3D quads are drawn as real
// quads instead of two triangles. Each quad arrives as a lines-adjacency
// primitive (its 4 corners); the geometry shader covers it with a 2-triangle
// strip and hands every fragment what it needs to interpolate the 4 corners
// with mean value coordinates (Floater; Hormann & Floater 2006), which give
// a smooth, split-free interpolation across the whole quad. Interpolation is
// perspective correct: corner values are divided by w and the result by the
// interpolated 1/w.

#ifndef INCLUDED_NEW3D_R3DSHADERQUADS_H
#define INCLUDED_NEW3D_R3DSHADERQUADS_H

static const char *vertexShaderR3DQuads = R"glsl(

#version 450 core

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

out Corner
{
  vec3  viewPos;
  vec3  viewNormal;
  vec2  texCoord;
  vec4  colour;
  float fixedShade;
  float facing;
  float lodBase;
} corner;

void main(void)
{
  corner.viewPos = (uModelMat * aPosition).xyz;
  corner.viewNormal = (mat3(uModelMat) / uModelScale) * aNormal;
  corner.facing = dot(corner.viewPos, mat3(uModelMat) * aFaceNormal);

  corner.colour = aColour;
  if (uTranslatorMap)
    corner.colour.rgb *= 16.0;
  corner.colour.a *= uNodeAlpha;

  corner.texCoord = aTexCoord;
  corner.fixedShade = aFixedShade;
  corner.lodBase = corner.facing * -uCota * aTextureNP;
  gl_Position = (uProjMat * uModelMat) * aPosition;
}
)glsl";

static const char *geometryShaderR3DQuads = R"glsl(

#version 450 core

layout(lines_adjacency) in;
layout(triangle_strip, max_vertices = 4) out;

in Corner
{
  vec3  viewPos;
  vec3  viewNormal;
  vec2  texCoord;
  vec4  colour;
  float fixedShade;
  float facing;
  float lodBase;
} corners[4];

out QuadData
{
  noperspective vec2  toCorner[4];    // corner minus fragment, in NDC
  noperspective float wedge[4];       // twice the signed area of (fragment, corner i, corner i+1)
  vec3  triWeight;                    // perspective-correct weights in the drawn triangle: corner 0,
                                      // corner 1 minus corner 3 (one of them is always 0), corner 2
  flat float invW[4];
  flat vec3  viewPosW[4];             // corner values already divided by w
  flat vec3  viewNormalW[4];
  flat vec2  texCoordW[4];
  flat float fixedShadeW[4];
  flat vec4  colour;
  flat float lodBase;
} quad;

// a*b - c*d without cancellation trouble (Kahan's method).
float DiffOfProducts(float a, float b, float c, float d)
{
  precise float cd = c * d;
  precise float err = fma(-c, d, cd);
  precise float r = fma(a, b, -cd);
  return r + err;
}

void main(void)
{
  if (corners[0].facing > 0.0)
    return;   // back facing (all four corners share the polygon normal)

  vec2 ndc[4];
  for (int i = 0; i < 4; i++)
  {
    float iw = 1.0 / gl_in[i].gl_Position.w;
    ndc[i] = gl_in[i].gl_Position.xy * iw;
    quad.invW[i] = iw;
    quad.viewPosW[i] = corners[i].viewPos * iw;
    quad.viewNormalW[i] = corners[i].viewNormal * iw;
    quad.texCoordW[i] = corners[i].texCoord * iw;
    quad.fixedShadeW[i] = corners[i].fixedShade * iw;
  }
  quad.colour = corners[0].colour;
  quad.lodBase = corners[0].lodBase;

  // det(ndc[i], ndc[j]), computed from clip coordinates for precision.
  precise float det[4][4];
  for (int i = 0; i < 4; i++)
  {
    det[i][i] = 0.0;
    for (int j = i + 1; j < 4; j++)
    {
      vec4 a = gl_in[i].gl_Position;
      vec4 b = gl_in[j].gl_Position;
      det[i][j] = DiffOfProducts(a.x, b.y, b.x, a.y) / (a.w * b.w);
      det[j][i] = -det[i][j];
    }
  }

  // Strip order 1, 0, 2, 3: triangles (1,0,2) and (0,2,3).
  const int order[4] = int[4](1, 0, 2, 3);
  for (int n = 0; n < 4; n++)
  {
    int c = order[n];
    for (int i = 0; i < 4; i++)
    {
      int k = (i + 1) & 3;
      quad.toCorner[i] = ndc[i] - ndc[c];
      quad.wedge[i] = det[i][k] + det[k][c] + det[c][i];
    }
    quad.triWeight = vec3(c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : (c == 3 ? -1.0 : 0.0), c == 2 ? 1.0 : 0.0);
    gl_Position = gl_in[c].gl_Position;
    EmitVertex();
  }
}

)glsl";

static const char *fragmentShaderR3DQuads = R"glsl(

#version 450 core

in QuadData
{
  noperspective vec2  toCorner[4];
  noperspective float wedge[4];
  vec3  triWeight;
  flat float invW[4];
  flat vec3  viewPosW[4];
  flat vec3  viewNormalW[4];
  flat vec2  texCoordW[4];
  flat float fixedShadeW[4];
  flat vec4  colour;
  flat float lodBase;
} quad;

uniform mat4 uProjMat;

vec3  vViewPos;
vec3  vViewNormal;
vec2  vTexCoord;
vec4  vColour;
float vFixedShade;
float vLodBase;

void ShadeFragment();

// Weights for the 4 corners at this fragment. Near an edge (nearEdge set)
// they are the drawn triangle's own perspective-correct weights times w,
// so a weighted sum of the w-divided corner values is already the final
// value. Otherwise they are mean value weights (to be normalised by the
// weighted 1/w). Returns false if the fragment lies outside the quad.
bool QuadWeights(out float weight[4], out bool nearEdge)
{
  // On (or extremely near) one of the quad's own edges the mean value
  // weights become unstable; there the quad is linear along the edge, so
  // the triangle's weights give the same answer.
  const float kEdge = 0.00006103515625;   // smallest normal half float
  nearEdge = quad.triWeight.x <= kEdge || quad.triWeight.z <= kEdge;
  if (nearEdge)
  {
    // Fragments just outside the triangle (helpers feeding derivatives)
    // can see negative weights; their magnitudes are used.
    float a = abs(quad.triWeight.x);
    float m = quad.triWeight.y;
    float b = abs(quad.triWeight.z);
    weight[0] = (a > kEdge) ? a / quad.invW[0] : 0.0;
    weight[1] = (m > kEdge) ? m / quad.invW[1] : 0.0;
    weight[2] = (b > kEdge) ? b / quad.invW[2] : 0.0;
    weight[3] = (-m > kEdge) ? -m / quad.invW[3] : 0.0;
    return true;
  }

  float dist[4];
  for (int i = 0; i < 4; i++)
    dist[i] = length(quad.toCorner[i]) * sign(quad.invW[i]);   // corners behind the eye count as negative

  // tan(half the angle at the fragment between corners i and i+1).
  precise float halfTan[4];
  for (int i = 0; i < 4; i++)
  {
    int k = (i + 1) & 3;
    halfTan[i] = (quad.wedge[i] == 0.0) ? 0.0 : fma(dist[i], dist[k], -dot(quad.toCorner[i], quad.toCorner[k])) / quad.wedge[i];
  }

  int negatives = 0;
  for (int i = 0; i < 4; i++)
  {
    int p = (i + 3) & 3;
    weight[i] = (halfTan[p] + halfTan[i]) / dist[i];
    if (halfTan[p] < -halfTan[i])
      negatives++;
  }
  return negatives != 2;
}

void main(void)
{
  float w[4];
  bool nearEdge;
  // Helper invocations outside the quad keep running: their values feed
  // the texture derivatives of their neighbours.
  if (!QuadWeights(w, nearEdge) && !gl_HelperInvocation)
    discard;

  vViewPos = vec3(0.0);
  vViewNormal = vec3(0.0);
  vTexCoord = vec2(0.0);
  vFixedShade = 0.0;
  float invW = 0.0;
  float total = 0.0;
  if (!nearEdge)
  {
    for (int i = 0; i < 4; i++)
    {
      invW = fma(quad.invW[i], w[i], invW);
      total += w[i];
    }
  }
  for (int i = 0; i < 4; i++)
  {
    vViewPos = fma(quad.viewPosW[i], vec3(w[i]), vViewPos);
    vViewNormal = fma(quad.viewNormalW[i], vec3(w[i]), vViewNormal);
    vTexCoord = fma(quad.texCoordW[i], vec2(w[i]), vTexCoord);
    vFixedShade = fma(quad.fixedShadeW[i], w[i], vFixedShade);
  }

  if (nearEdge)
  {
    // Already final; 1/w comes from the interpolated position.
    invW = 1.0 / (uProjMat * vec4(vViewPos, 1.0)).w;
    total = 1.0;
  }
  else
  {
    // (Σ value_i/w_i * weight_i) / (Σ weight_i/w_i)
    float scale = 1.0 / invW;
    vViewPos *= scale;
    vViewNormal *= scale;
    vTexCoord *= scale;
    vFixedShade *= scale;
  }

  // Depth. A quad facing the screen squarely gets exactly its corners'
  // depth, so coplanar polygons (decals) depth test equal.
  if (quad.invW[0] == quad.invW[1] && quad.invW[1] == quad.invW[2] && quad.invW[2] == quad.invW[3])
  {
    vViewPos.z = quad.viewPosW[0].z / quad.invW[0];
    vec4 clip = uProjMat * vec4(vViewPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
  }
  else
  {
    float clipZ = uProjMat[2][2] * vViewPos.z + uProjMat[3][2];
    gl_FragDepth = clipZ * (invW / total);
  }

  vColour = quad.colour;
  vLodBase = quad.lodBase;
  ShadeFragment();
}
)glsl";

#endif  // INCLUDED_NEW3D_R3DSHADERQUADS_H
