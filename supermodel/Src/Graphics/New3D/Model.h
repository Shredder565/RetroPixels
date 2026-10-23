// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Model.h
//
// The data the New3D engine builds from the Real3D scene database each
// frame: vertices as uploaded to the GPU, groups of polygons that share the
// same render state, model instances and viewports.

#ifndef INCLUDED_NEW3D_MODEL_H
#define INCLUDED_NEW3D_MODEL_H

#include <cstdint>
#include <memory>
#include <vector>

#include "Mat4.h"

namespace New3D {

// Per-vertex values decoded from a polygon.
struct VertexBase
{
  float pos[4];
  float normal[3];
  float texcoords[2];
  float fixedShade;
};

// A decoded polygon (3 or 4 vertices).
struct Poly
{
  VertexBase v[4];
  float faceNormal[3];
  uint8_t faceColour[4];
  float textureNP;
  int count = 4;
};

// A vertex as stored in the vertex buffer: the vertex plus its polygon's
// face values. The attribute pointers are set from this layout.
struct GPUVertex
{
  VertexBase base;
  float faceNormal[3];
  uint8_t faceColour[4];
  float textureNP;
};

// Which pass a polygon is drawn in.
enum class Layer { colour, trans1, trans2, trans12, all, none };

// Polygons of one model that share a render state, and where their vertices
// are in the vertex buffer.
struct DrawGroup
{
  enum TexWrapMode : int { repeat = 0, repeatClamp, mirror, mirrorClamp };

  // Is this group drawn in the given pass? nodeAlpha is the model's fade.
  bool DrawnIn(Layer layer, float nodeAlpha) const
  {
    const bool faded = nodeAlpha < 1.0f;
    const bool seeThrough = textureAlpha || polyAlpha || faded;
    switch (layer)
    {
    case Layer::colour: return !polyAlpha && !faded;
    case Layer::trans1: return seeThrough && !transLSelect;
    case Layer::trans2: return seeThrough && transLSelect;
    default:            return false;
    }
  }

  // texture
  int format = 0;
  int x = 0, y = 0;
  int width = 0, height = 0;
  int page = 0;
  bool inverted = false;
  TexWrapMode wrapModeU = repeat;
  TexWrapMode wrapModeV = repeat;
  bool microTexture = false;
  int microTextureID = 0;
  float microTextureMinLOD = 0.0f;

  // state
  bool textured = false;
  bool polyAlpha = false;       // polygon transparency below 100%
  bool textureAlpha = false;    // texture alpha is used
  bool alphaTest = false;       // contour (1-bit alpha) processing
  bool layered = false;         // stencil layered polygon
  bool highPriority = false;    // drawn over everything else in its priority
  bool transLSelect = false;    // translucency layer 0 / 1
  bool translatorMap = false;   // colours scaled by 16
  bool noLosReturn = false;     // invisible to line-of-sight tests

  // lighting
  bool fixedShading = false;
  bool smoothShading = false;
  bool lighting = false;
  bool specular = false;
  float shininess = 0.0f;
  float specularValue = 0.0f;
  float fogIntensity = 1.0f;

  // vertex buffer range
  int vboOffset = 0;
  int vertexCount = 0;
};

// A group under construction, with its vertices.
struct BuildingGroup : public DrawGroup
{
  std::vector<GPUVertex> verts;
};

// One drawn model.
struct ModelInstance
{
  std::shared_ptr<std::vector<DrawGroup>> groups;   // shared between instances of the same ROM model
  bool dynamic = true;        // vertices are rebuilt every frame
  int textureOffsetX = 0;
  int textureOffsetY = 0;
  int page = 0;
  float modelMat[16];
  float scale = 1.0f;         // Step 1.5+ model scale
  float alpha = 1.0f;         // fade from LOD blending
};

struct ViewportState
{
  // hardware values
  float vpX, vpY, vpWidth, vpHeight;
  float angle_left, angle_right, angle_top, angle_bottom;
  float cota;

  Mat4 projectionMatrix;

  // lighting: sun direction (3), sun intensity, ambient intensity, unused
  float lightingParams[6];
  bool sunClamp;
  bool intensityClamp;

  // spotlight: window position x/y and half size x/y, then Z start / extent
  float spotEllipse[4];
  float spotRange[2];
  float spotColor[3];
  float spotFogColor[3];

  float fogColour[3];
  float fogStart;
  float fogDensity;
  float fogAttenuation;
  float fogAmbient;
  float scrollFog;            // how much fog covers the 2D background
  float scrollAtt;

  int losPosX, losPosY;       // line of sight test position (Model 3 pixels)

  // OpenGL viewport
  int x, y, width, height;

  int priority;
  int select;
  int number;
  int hardwareStep;
};

// Saved per-node values that children inherit. Holds at most 128 saved
// levels; deeper pushes are not stored (pops then restore older levels).
class NodeStateStack
{
public:
  NodeStateStack();
  bool Push();
  bool Pop();
  void Reset();

  int texOffsetX;
  int texOffsetY;
  int page;
  float modelScale;
  float modelAlpha;
  bool disableCulling;

private:
  struct Saved
  {
    int texOffsetX, texOffsetY, page;
    float modelScale, modelAlpha;
    bool disableCulling;
  };
  std::vector<Saved> m_saved;
};

struct ViewportNode
{
  ViewportState viewport;
  std::vector<ModelInstance> models;
};

} // New3D

#endif  // INCLUDED_NEW3D_MODEL_H
