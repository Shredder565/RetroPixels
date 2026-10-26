// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// New3D.cpp
//
// Frame building (scene database walk and model decoding) and drawing for
// the New3D renderer. See New3D.h for the overview.

#include "New3D.h"
#include "R3DFloat.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace New3D {

namespace
{
  // The vertex buffer: VROM models (kept between frames) from the start,
  // then this frame's polygon RAM models.
  constexpr size_t kRomVerts = 1500000;
  constexpr size_t kRamVerts = 300000;

  constexpr float kNear = 1e-3f;

  // Guards against malformed (cyclic) scene data.
  constexpr int kMaxNodeDepth = 1024;
  constexpr size_t kMaxSiblingRun = 1 << 16;
  constexpr int kMaxViewports = 4096;

  float AsFloat(uint32_t bits)
  {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  uint32_t AsBits(float f)
  {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
  }

  // A signed 8-bit normal component or shade value, mapped so -128..127
  // spans -1..1 without a zero step.
  float SignedByte(uint32_t word)
  {
    int8_t b = int8_t(word & 0xFF);
    return (2.0f * float(b) + 1.0f) * float(1.0 / 255.0);
  }

  // Vertex positions sit in the top 24 bits of each word, signed.
  float VertexCoord(uint32_t word, float scale)
  {
    return float(int32_t(word) >> 8) * scale;
  }

  // The 1-bit spotlight colours (RGB, one bit each).
  void ColourFromIndex(int index, float out[3])
  {
    out[0] = (index & 4) ? 1.0f : 0.0f;
    out[1] = (index & 2) ? 1.0f : 0.0f;
    out[2] = (index & 1) ? 1.0f : 0.0f;
  }

  float Unit8(uint32_t word, unsigned shift)
  {
    return float((word >> shift) & 0xFF) * float(1.0 / 255.0);
  }

  // Texture formats 1-4 have a 4-bit alpha that contour processing replaces;
  // those variants are numbered 8-11 for the shader.
  int ShaderTexFormat(int format, bool contour)
  {
    return (contour && format >= 1 && format <= 4) ? format + 7 : format;
  }

  GPUVertex MakeVertex(const Poly &p, const VertexBase &v)
  {
    GPUVertex out;
    out.base = v;
    std::copy(p.faceNormal, p.faceNormal + 3, out.faceNormal);
    std::copy(p.faceColour, p.faceColour + 4, out.faceColour);
    out.textureNP = p.textureNP;
    return out;
  }

  VertexBase Midpoint(const VertexBase &a, const VertexBase &b)
  {
    VertexBase m;
    for (int i = 0; i < 3; i++)
    {
      m.pos[i] = 0.5f * (a.pos[i] + b.pos[i]);
      m.normal[i] = 0.5f * (a.normal[i] + b.normal[i]);
    }
    m.pos[3] = 1.0f;
    m.texcoords[0] = 0.5f * (a.texcoords[0] + b.texcoords[0]);
    m.texcoords[1] = 0.5f * (a.texcoords[1] + b.texcoords[1]);
    m.fixedShade = 0.5f * (a.fixedShade + b.fixedShade);
    return m;
  }

  bool SamePosition(const VertexBase &a, const VertexBase &b)
  {
    return a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] && a.pos[2] == b.pos[2];
  }
}

/******************************************************************************
 Set-up
******************************************************************************/

CNew3D::CNew3D(const Util::Config::Node &config, const std::string &gameName)
  : m_sunClamp(true),
    m_blockCulling(false),
    m_shader(config)
{
  (void)gameName;
  std::memset(&m_planes, 0, sizeof(m_planes));
  std::memset(m_lastVerts, 0, sizeof(m_lastVerts));
  std::memset(m_lastUV, 0, sizeof(m_lastUV));

  if (config["QuadRendering"].ValueAs<bool>())
  {
    m_vertsPerPoly = 4;
    m_primitive = GL_LINES_ADJACENCY;   // 4 vertices per primitive for the quad geometry shader
  }
  m_wideScreen = config["WideScreen"].ValueAs<bool>();
  m_noWhiteFlash = config["NoWhiteFlash"].ValueAs<bool>();

  m_shader.LoadShader();
  glUseProgram(0);

  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);
  m_vbo.Create(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW, GLsizeiptr(sizeof(GPUVertex) * (kRamVerts + kRomVerts)));
  m_vbo.Bind(true);

  struct Attrib { const char *name; GLint size; GLenum type; GLboolean norm; size_t offset; };
  const Attrib attribs[] =
  {
    { "aPosition",   4, GL_FLOAT,         GL_FALSE, offsetof(GPUVertex, base.pos) },
    { "aNormal",     3, GL_FLOAT,         GL_FALSE, offsetof(GPUVertex, base.normal) },
    { "aTexCoord",   2, GL_FLOAT,         GL_FALSE, offsetof(GPUVertex, base.texcoords) },
    { "aColour",     4, GL_UNSIGNED_BYTE, GL_TRUE,  offsetof(GPUVertex, faceColour) },
    { "aFaceNormal", 3, GL_FLOAT,         GL_FALSE, offsetof(GPUVertex, faceNormal) },
    { "aFixedShade", 1, GL_FLOAT,         GL_FALSE, offsetof(GPUVertex, base.fixedShade) },
    { "aTextureNP",  1, GL_FLOAT,         GL_FALSE, offsetof(GPUVertex, textureNP) },
  };
  for (const Attrib &a : attribs)
  {
    GLint loc = m_shader.GetVertexAttribPos(a.name);
    if (loc < 0)
      continue;
    glEnableVertexAttribArray(GLuint(loc));
    glVertexAttribPointer(GLuint(loc), a.size, a.type, a.norm, sizeof(GPUVertex), reinterpret_cast<const void *>(a.offset));
  }

  glBindVertexArray(0);
  m_vbo.Bind(false);
}

CNew3D::~CNew3D(void)
{
  m_vbo.Destroy();
  if (m_vao)
    glDeleteVertexArrays(1, &m_vao);
  m_vao = 0;
  m_shader.UnloadShader();
}

void CNew3D::AttachMemory(const UINT32 *cullingRAMLoPtr, const UINT32 *cullingRAMHiPtr, const UINT32 *polyRAMPtr, const UINT32 *vromPtr, const UINT16 *textureRAMPtr)
{
  m_cullLo = cullingRAMLoPtr;
  m_cullHi = cullingRAMHiPtr;
  m_polyRAM = polyRAMPtr;
  m_vrom = vromPtr;
  m_texRAM = textureRAMPtr;
  m_sheets[0].AttachMemory(textureRAMPtr);
  m_sheets[1].AttachMemory(textureRAMPtr + 2048 * 1024);
}

void CNew3D::SetStepping(int stepping)
{
  if (stepping != 0x10 && stepping != 0x15 && stepping != 0x20 && stepping != 0x21)
    stepping = 0x10;
  m_step = stepping;

  if (m_step > 0x10)
  {
    m_nodeShift = 0;                        // 10-word culling nodes
    m_vertexScale = 1.0f / 2048.0f;         // 13.11 vertices
    m_textureNPScale = 1.0f / 16384.0f;     // 10.14
  }
  else
  {
    m_nodeShift = 2;                        // 8-word culling nodes
    m_vertexScale = 1.0f / 128.0f;          // 17.7
    m_textureNPScale = 1.0f / 4096.0f;      // 12.12
  }
}

Result CNew3D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget)
{
  // The Model 3 screen is 496x384.
  m_xRatio = float(xRes) * float(1.0 / 496.0);
  m_yRatio = float(yRes) * float(1.0 / 384.0);
  m_xOffs = xOffset;
  m_yOffs = yOffset;
  m_xRes = xRes;
  m_yRes = yRes;
  m_totalXRes = totalXRes;
  m_totalYRes = totalYRes;
  m_aaTarget = aaTarget;

  m_targets.DestroyFBO();
  return m_targets.CreateFBO(int(totalXRes), int(totalYRes));
}

void CNew3D::SetSunClamp(bool enable)
{
  m_sunClamp = enable;
}

void CNew3D::SetBlockCulling(bool enable)
{
  m_blockCulling = enable;
}

float CNew3D::GetLosValue(int layer)
{
  std::lock_guard<std::mutex> lock(m_losLock);
  return m_los[m_losRead][layer];
}

void CNew3D::UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height)
{
  if (width == 2048 && height == 2048)
  {
    // Whole of texture RAM: refresh every level of both sheets.
    static constexpr int levelX[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
    static constexpr int levelY[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023 };
    unsigned w = 2048, h = 1024;
    const int levels = std::min(m_sheets[0].GetNumberOfLevels(), int(sizeof(levelY) / sizeof(levelY[0])));
    for (int i = 0; i < levels; i++)
    {
      m_sheets[0].UploadTextures(i, levelX[i], levelY[i], int(w), int(h));
      m_sheets[1].UploadTextures(i, levelX[i], levelY[i], int(w), int(h));
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    return;
  }

  unsigned sheet = y / 1024;
  y -= sheet * 1024;
  m_sheets[sheet].UploadTextures(int(level), int(x), int(y), int(width), int(height));
}

void CNew3D::BeginFrame(void)
{
}

void CNew3D::EndFrame(void)
{
}

/******************************************************************************
 Memory
******************************************************************************/

// Culling RAM: low at word addresses 0-0xFFFFF, high at 0x800000-0x83FFFF.
const uint32_t *CNew3D::CullingWords(uint32_t addr) const
{
  addr &= 0xFFFFFF;
  if (addr >= 0x800000 && addr < 0x840000)
    return m_cullHi + (addr & 0x3FFFF);
  if (addr < 0x100000)
    return m_cullLo + addr;
  return nullptr;
}

// Models below word 0x100000 are in polygon RAM, the rest in VROM.
const uint32_t *CNew3D::ModelWords(uint32_t addr) const
{
  addr &= 0xFFFFFF;
  if (addr < 0x100000)
    return m_polyRAM + addr;
  return m_vrom ? m_vrom + addr : nullptr;
}

/******************************************************************************
 Scene database walk

 Viewport node (word: contents)
   0x00  bit 5 disabled, bits 4-3 priority, bits 9-8 select, 10+ number
   0x01  next viewport (0x01000000 ends the list)
   0x02  first child pointer
   0x03  cot(fov/2)
   0x04-0x07  sun vector Z, X, Y (floats) and sun intensity
   0x08-0x0B  frustum: 1/(right-left), 1/(top-bottom), top and left offsets
   0x0C-0x13  side plane normals (left, top, right, bottom; 2 floats each)
   0x14  height (14.2) << 16 | width (14.2)
   0x16  matrix base   0x17  LOD table
   0x1A  Y (12.4) << 16 | X (12.4)
   0x1C  line of sight Y << 16 | X (12.4)
   0x1D, 0x1E  spotlight size << 16 | position (13.3), Y then X
   0x1F  spotlight extent   0x21  1 / spotlight start
   0x20  bits 13-11 spot colour, 10-8 spot-on-fog colour, 7-0 scroll fog
   0x22  fog colour (RGB)   0x23  fog density
   0x24  bits 23-16 fog attenuation, 15-8 ambient light, 7-0 scroll attenuation
   0x25  bits 23-16 fog ambient, 15-0 fog start

 Culling node (Step 1.0 nodes lack words 1 and 2; later words move down)
   0x00  bits 1-0 type (0 viewport), bit 2 colour table valid / no siblings
         when bits 2-0 = 6, bit 3 four-entry LOD table, bit 4 translate,
         bit 7 reset rotation, bits 9-8 discard
   0x01  model scale (float, low 2 bits: bit 0 valid, bit 1 no culling)
   0x02  bit 15 texture offset valid, bit 14 page, bits 12-7 X, 4-0 Y
   0x03  bits 31-19 colour table address, bit 29 LOD entries are nodes,
         bits 18-12 LOD table entry, bits 11-0 matrix
   0x04-0x06  translation
   0x07  child pointer (bits 31-28 colour table address)
   0x08  sibling pointer (bit 24 none, bits 31-25 colour table address)
   0x09  blend radius << 16 | culling radius (16-bit Real3D floats)
******************************************************************************/

void CNew3D::WalkViewports(uint32_t addr)
{
  for (int count = 0; count < kMaxViewports && (addr & 0xFFFFFF) != 0; count++)
  {
    const uint32_t *vp = CullingWords(addr);
    if (vp == nullptr)
      return;

    m_frame.emplace_back();
    m_frame.back().models.reserve(2048);
    ReadViewport(vp, m_frame.back().viewport);

    m_state.Reset();
    StartMatrixStack(vp[0x16] & 0xFFFFFF);

    // The first child must be a culling node: it gives the culling radius.
    const bool disabled = (vp[0] & 0x20) != 0;
    if (!disabled && ((vp[0x02] >> 24) & 5) == 0)
      VisitPointer(vp[0x02], 0);

    if (vp[0x01] == 0x01000000)
      return;
    addr = vp[0x01];
  }
}

void CNew3D::ReadViewport(const uint32_t *vp, ViewportState &out)
{
  out.priority = int((vp[0] >> 3) & 3);
  out.select = int((vp[0] >> 8) & 3);
  out.number = int(vp[0] >> 10);

  out.vpX = float(vp[0x1A] & 0xFFFF) * (1.0f / 16.0f);
  out.vpY = float(vp[0x1A] >> 16) * (1.0f / 16.0f);
  out.vpWidth = float(vp[0x14] & 0xFFFF) * (1.0f / 4.0f);
  out.vpHeight = float(vp[0x14] >> 16) * (1.0f / 4.0f);

  m_lodTable = reinterpret_cast<const LODBlendTable *>(CullingWords(vp[0x17] & 0xFFFFFF));

  // Frustum edges as tangents of the view angles.
  const float invWidth = AsFloat(vp[0x08]);
  const float invHeight = AsFloat(vp[0x09]);
  const float topOffset = AsFloat(vp[0x0A]);
  const float leftOffset = AsFloat(vp[0x0B]);
  out.angle_left = -leftOffset / invWidth;
  out.angle_right = (1.0f - leftOffset) / invWidth;
  out.angle_bottom = -(1.0f - topOffset) / invHeight;
  out.angle_top = topOffset / invHeight;

  m_planes.lu = AsFloat(vp[0x0C]);
  m_planes.lv = AsFloat(vp[0x0D]);
  m_planes.tu = AsFloat(vp[0x0E]);
  m_planes.tw = AsFloat(vp[0x0F]);
  m_planes.ru = AsFloat(vp[0x10]);
  m_planes.rv = AsFloat(vp[0x11]);
  m_planes.bu = AsFloat(vp[0x12]);
  m_planes.bw = AsFloat(vp[0x13]);
  m_planes.xScale = 1.0f;

  out.cota = AsFloat(vp[0x03]);
  PlaceViewport(out);

  // The sun vector points toward the sun. Real3D Y and Z are flipped
  // relative to OpenGL.
  out.lightingParams[0] = AsFloat(vp[0x05]);
  out.lightingParams[1] = -AsFloat(vp[0x06]);
  out.lightingParams[2] = -AsFloat(vp[0x04]);
  out.lightingParams[3] = std::max(0.0f, std::min(AsFloat(vp[0x07]), 1.0f));
  out.lightingParams[4] = Unit8(vp[0x24], 8);
  out.lightingParams[5] = 0.0f;
  out.sunClamp = m_sunClamp;
  out.intensityClamp = m_step == 0x10;
  out.hardwareStep = m_step;

  // Spotlight, in Model 3 screen pixels with Y measured from the top.
  float spotX = float(int16_t(vp[0x1E] & 0xFFFF)) * (1.0f / 8.0f);
  float spotY = 384.0f - float(int16_t(vp[0x1D] & 0xFFFF)) * (1.0f / 8.0f);
  float sizeX = std::roundf(2047.0f / std::max(1.0f, float(vp[0x1E] >> 16)));
  float sizeY = std::roundf(2047.0f / std::max(1.0f, float(vp[0x1D] >> 16)));
  out.spotEllipse[0] = spotX * m_xRatio + float(m_xOffs);
  out.spotEllipse[1] = spotY * m_yRatio + float(m_yOffs);
  out.spotEllipse[2] = sizeX * m_xRatio;
  out.spotEllipse[3] = sizeY * m_yRatio;
  out.spotRange[0] = 1.0f / AsFloat(vp[0x21]);
  out.spotRange[1] = AsFloat(vp[0x1F]);
  ColourFromIndex(int((vp[0x20] >> 11) & 7), out.spotColor);
  ColourFromIndex(int((vp[0x20] >> 8) & 7), out.spotFogColor);

  out.losPosX = int(float(vp[0x1C] & 0xFFFF) / 16.0f + 0.5f);
  out.losPosY = int(float(vp[0x1C] >> 16) / 16.0f + 0.5f);

  out.fogColour[0] = Unit8(vp[0x22], 16);
  out.fogColour[1] = Unit8(vp[0x22], 8);
  out.fogColour[2] = Unit8(vp[0x22], 0);
  out.fogDensity = std::abs(AsFloat(vp[0x23]));      // some games store it negative
  if (!std::isfinite(out.fogDensity))
    out.fogDensity = 0.0f;
  out.fogStart = float(int16_t(vp[0x25] & 0xFFFF)) * float(1.0 / 255.0);
  out.fogAttenuation = Unit8(vp[0x24], 16);
  out.fogAmbient = Unit8(vp[0x25], 16);
  out.scrollFog = Unit8(vp[0x20], 0);
  out.scrollAtt = Unit8(vp[0x24], 0);
}

// The OpenGL viewport rectangle and projection. A full-screen viewport in
// wide-screen mode is widened to the whole window, keeping its aspect.
void CNew3D::PlaceViewport(ViewportState &vp)
{
  float left = vp.angle_left;
  float right = vp.angle_right;
  vp.projectionMatrix.LoadIdentity();
  vp.y = int(m_yOffs) + int((384.0f - (vp.vpY + vp.vpHeight)) * m_yRatio);
  vp.height = int(vp.vpHeight * m_yRatio);

  const bool fullScreen = vp.vpX == 0 && vp.vpWidth >= 495 && vp.vpY == 0 && vp.vpHeight >= 383;
  if (m_wideScreen && fullScreen)
  {
    float widen = (float(m_totalXRes) / float(m_totalYRes)) / (float(m_xRes) / float(m_yRes));
    m_planes.xScale = 1.0f / widen;
    left *= widen;
    right *= widen;
    vp.x = 0;
    vp.width = int(m_totalXRes);
  }
  else
  {
    vp.x = int(m_xOffs) + int(vp.vpX * m_xRatio);
    vp.width = int(vp.vpWidth * m_xRatio);
  }
  vp.projectionMatrix.FrustumRZ(left, right, vp.angle_bottom, vp.angle_top, kNear);
}

// Visits a culling node and the run of siblings after it. Siblings are
// handled before the node that links to them, so the run is gathered
// first and then handled from its end.
void CNew3D::VisitNode(uint32_t addr, int depth)
{
  if (depth > kMaxNodeDepth)
    return;

  const size_t base = m_siblings.size();
  for (;;)
  {
    const uint32_t *node = CullingWords(addr);
    if (node == nullptr)
      break;
    if ((node[0] & 3) == 0)                 // viewport nodes draw nothing
      break;
    if ((node[0] & 0x300) == 0x300)         // both discard bits: invisible
      break;
    m_siblings.push_back(node);
    if (m_siblings.size() - base >= kMaxSiblingRun)
      break;
    if ((node[0] & 7) == 6)                 // no siblings
      break;
    uint32_t sibling = node[0x08 - m_nodeShift] & 0x1FFFFFF;
    if (sibling == 0 || (sibling & 0x1000000))
      break;
    addr = sibling;
  }

  for (size_t i = m_siblings.size(); i-- > base;)
    DrawCullingNode(m_siblings[i], depth);
  m_siblings.resize(base);
}

void CNew3D::DrawCullingNode(const uint32_t *node, int depth)
{
  const int s = m_nodeShift;
  const uint32_t w3 = node[0x03 - s];
  const uint32_t child = node[0x07 - s] & 0x7FFFFFF;

  if (node[0] & 0x04)
  {
    // The colour table address is spread over the top bits of three words.
    m_colourTable = (w3 >> 19) | ((node[0x07 - s] >> 28) << 13) | ((node[0x08 - s] >> 25) << 17);
    m_colourTable &= 0xFFFFF;
  }

  m_state.Push();

  if (s == 0)
  {
    if (node[0x01] & 1)
      m_state.modelScale = AsFloat(node[0x01] & ~3u);
    if (node[0x01] & 2)
      m_state.disableCulling = true;
    if (node[0x02] & 0x8000)
    {
      m_state.texOffsetX = int(32 * ((node[0x02] >> 7) & 0x3F));
      m_state.texOffsetY = int(32 * (node[0x02] & 0x1F));
      m_state.page = int((node[0x02] & 0x4000) >> 14);
    }
  }

  m_modelMat.PushMatrix();
  if (node[0] & 0x10)
    m_modelMat.Translate(AsFloat(node[0x04 - s]), AsFloat(node[0x05 - s]), AsFloat(node[0x06 - s]));
  else if (w3 & 0xFFF)
    ApplyNodeMatrix(w3 & 0xFFF);
  if (node[0] & 0x80)
    RemoveRotation();

  // View space position of the node centre.
  const float x = m_modelMat.currentMatrix[12];
  const float y = m_modelMat.currentMatrix[13];
  const float z = m_modelMat.currentMatrix[14];

  const uint32_t radii = node[0x09 - s];
  const float cullRadius = R3DFloat::GetFloat16(uint16_t(radii & 0xFFFF)) * m_state.modelScale;
  const float blendRadius = R3DFloat::GetFloat16(uint16_t(radii >> 16)) * m_state.modelScale;

  const bool outside =
    (z * m_planes.lu - x * m_planes.lv * m_planes.xScale) > cullRadius ||
    (z * m_planes.tu + y * m_planes.tw) > cullRadius ||
    (z * m_planes.ru - x * m_planes.rv * m_planes.xScale) > cullRadius ||
    (z * m_planes.bu + y * m_planes.bw) > cullRadius;

  // How big the node looks: decides the LOD and its fade.
  float size = m_state.disableCulling ? std::numeric_limits<float>::max() : blendRadius / std::hypot(x, y, z);
  size = std::clamp(size, 0.0f, std::numeric_limits<float>::max());

  if (m_lodTable != nullptr)
  {
    const LOD *lod = m_lodTable->table[(w3 >> 12) & 0x7F].lod;

    if (m_state.disableCulling || (!outside && size >= lod[3].deleteSize))
    {
      if (node[0] & 0x08)
      {
        // Four LODs, each a node or a model; the chosen one cross-fades
        // into the next while it is near its delete size.
        const uint32_t *lods = CullingWords(child);
        if (lods != nullptr)
        {
          int n = 0;
          while (n < 3 && !(size >= lod[n].deleteSize && (lods[n] & 0x1000000)))
            n++;

          const float parentAlpha = m_state.modelAlpha;
          float fade = std::clamp(lod[n].blendFactor * (size - lod[n].deleteSize), 0.0f, 1.0f);
          if (fade > float(31.0 / 32.0))       // the shader drops alpha under 1/32
            fade = 1.0f;
          else if (fade < float(1.0 / 32.0))
            fade = 0.0f;
          m_state.modelAlpha *= fade;

          const bool entriesAreNodes = (w3 & 0x20000000) != 0;
          auto visit = [&](uint32_t entry) {
            if (entriesAreNodes)
              VisitNode(entry & 0xFFFFFF, depth + 1);
            else
              AddModel(entry & 0xFFFFFF);
          };

          visit(lods[n]);
          if (fade < 1.0f && n != 3)
          {
            m_state.modelAlpha = (1.0f - fade) * parentAlpha;
            visit(lods[n + 1]);
          }
        }
      }
      else
      {
        m_state.modelAlpha *= std::clamp(lod[3].blendFactor * (size - lod[3].deleteSize), 0.0f, 1.0f);
        VisitPointer(child, depth + 1);
      }
    }
  }

  m_modelMat.PopMatrix();
  m_state.Pop();
}

// A child pointer: bits 24 and 26 give its kind.
void CNew3D::VisitPointer(uint32_t ptr, int depth)
{
  if ((ptr & 0xFFFFFF) == 0)
    return;
  switch ((ptr >> 24) & 5)
  {
  case 0: VisitNode(ptr & 0xFFFFFF, depth); break;
  case 1: AddModel(ptr & 0xFFFFFF); break;
  case 4: VisitPointerList(ptr & 0xFFFFFF, depth); break;
  default: break;
  }
}

// A list of culling node addresses: bit 24 marks an empty entry (stop),
// bit 25 the last entry.
void CNew3D::VisitPointerList(uint32_t addr, int depth)
{
  const uint32_t *list = CullingWords(addr);
  if (list == nullptr)
    return;
  addr &= 0xFFFFFF;
  const uint32_t room = (addr >= 0x800000) ? 0x40000 - (addr & 0x3FFFF) : 0x100000 - addr;

  for (uint32_t i = 0; i < room; i++)
  {
    if (list[i] & 0x1000000)
      break;
    VisitNode(list[i] & 0xFFFFFF, depth);
    if (list[i] & 0x2000000)
      break;
  }
}

/******************************************************************************
 Models
******************************************************************************/

// Does any polygon take its colour from the colour table? Those models can
// change from frame to frame, so they are never kept.
bool CNew3D::UsesColourTable(const uint32_t *data) const
{
  if (data == nullptr)
    return false;
  PolyHeader ph(data);
  do
  {
    if ((ph.header[1] & 2) == 0)
      return true;
  } while (ph.NextPoly());
  return false;
}

void CNew3D::AddModel(uint32_t addr)
{
  const uint32_t *data = ModelWords(addr);

  m_frame.back().models.emplace_back();
  ModelInstance &model = m_frame.back().models.back();

  bool built = false;
  if (addr >= 0x100000 && !UsesColourTable(data))
  {
    // VROM models are decoded once.
    auto &kept = m_romModels[addr];
    if (kept)
      built = true;
    else
      kept = std::make_shared<std::vector<DrawGroup>>();
    model.groups = kept;
    model.dynamic = false;
  }
  else
    model.groups = std::make_shared<std::vector<DrawGroup>>();

  std::copy(m_modelMat.currentMatrix, m_modelMat.currentMatrix + 16, model.modelMat);
  model.textureOffsetX = m_state.texOffsetX;
  model.textureOffsetY = m_state.texOffsetY;
  model.page = m_state.page;
  model.scale = m_state.modelScale;
  model.alpha = m_state.modelAlpha;

  if (!built)
    BuildModel(model, data);
}

void CNew3D::SetGroupState(DrawGroup &g, const PolyHeader &ph) const
{
  g.textured = ph.TexEnabled();
  g.alphaTest = ph.AlphaTest();
  g.textureAlpha = ph.TextureAlpha();
  g.polyAlpha = ph.PolyAlpha();
  g.lighting = ph.LightEnabled();
  g.fixedShading = ph.FixedShading() && !ph.SmoothShading();
  g.highPriority = ph.HighPriority();
  g.transLSelect = ph.TranslucencyPatternSelect();
  g.layered = ph.Layered();
  g.specular = ph.SpecularEnabled();
  g.shininess = ph.Shininess();
  g.specularValue = ph.SpecularValue();
  g.fogIntensity = ph.LightModifier();
  g.translatorMap = ph.TranslatorMap();
  g.noLosReturn = ph.NoLosReturn();
  g.smoothShading = ph.SmoothShading();

  if (!g.textured)
    return;

  g.format = ShaderTexFormat(ph.TexFormat(), ph.AlphaTest());
  if (g.format == 7)
    g.alphaTest = false;    // 4-bit alpha format: the 1-bit contour test does not apply
  g.x = ph.X();
  g.y = ph.Y();
  g.width = ph.TexWidth();
  g.height = ph.TexHeight();
  g.microTexture = ph.MicroTexture();
  g.inverted = ph.TranslatorMapOffset() == 2;
  g.page = ph.Page();

  // Contour textures are drawn unfiltered at their edges, so no smoothing.
  const bool smoothU = ph.TexSmoothU() && !ph.AlphaTest();
  const bool smoothV = ph.TexSmoothV() && !ph.AlphaTest();
  auto wrap = [](bool mirror, bool smooth) {
    if (mirror)
      return smooth ? DrawGroup::mirror : DrawGroup::mirrorClamp;
    return smooth ? DrawGroup::repeat : DrawGroup::repeatClamp;
  };
  g.wrapModeU = wrap(ph.TexUMirror(), smoothU);
  g.wrapModeV = wrap(ph.TexVMirror(), smoothV);

  if (g.microTexture)
  {
    g.microTextureID = ph.MicroTextureID();
    g.microTextureMinLOD = float(ph.MicroTextureMinLOD());
  }
}

// Appends a polygon's vertices: triangles (a quad as two), or for the quad
// pipeline 4 vertices per polygon (a triangle gets a 4th vertex midway
// between its first and last; a quad with two equal neighbouring corners
// has the second moved to between its neighbours).
void CNew3D::EmitPoly(const Poly &p, std::vector<GPUVertex> &out) const
{
  // Fully transparent polygons are not drawn (some games put one first in
  // a viewport, and it would spoil the depth buffer).
  if (p.faceColour[3] == 0)
    return;

  if (m_vertsPerPoly == 4)
  {
    if (p.count == 4)
    {
      VertexBase v[4] = { p.v[0], p.v[1], p.v[2], p.v[3] };
      for (int i = 0; i < 4; i++)
      {
        int a = (i + 1) % 4, b = (i + 2) % 4;
        if (SamePosition(v[i], v[a]))
        {
          v[a] = Midpoint(v[a], v[b]);
          break;
        }
      }
      for (const VertexBase &vb : v)
        out.push_back(MakeVertex(p, vb));
    }
    else
    {
      out.push_back(MakeVertex(p, p.v[0]));
      out.push_back(MakeVertex(p, p.v[1]));
      out.push_back(MakeVertex(p, p.v[2]));
      out.push_back(MakeVertex(p, Midpoint(p.v[0], p.v[2])));
    }
    return;
  }

  static const int kTri[] = { 0, 1, 2 };
  static const int kQuad[] = { 0, 1, 2, 0, 2, 3 };
  const int *order = (p.count == 4) ? kQuad : kTri;
  const int n = (p.count == 4) ? 6 : 3;
  for (int i = 0; i < n; i++)
    out.push_back(MakeVertex(p, p.v[order[i]]));
}

void CNew3D::BuildModel(ModelInstance &model, const uint32_t *data)
{
  if (data == nullptr)
    return;

  PolyHeader ph(data);
  const size_t reserveVerts = size_t(ph.NumTrianglesTotal()) * 3;

  // Polygons are grouped by render state. The groups' order in the vertex
  // buffer (and when drawn) is the iteration order of this map.
  std::unordered_map<uint64_t, BuildingGroup> groups;
  BuildingGroup *group = nullptr;
  uint64_t lastKey = ~uint64_t(0);
  uint16_t uv[4][2];
  std::memcpy(uv, m_lastUV, sizeof(uv));

  do
  {
    if (ph.header[6] == 0)
      break;

    const uint64_t key = ph.Hash();
    const bool newState = key != lastKey;
    if (newState)
    {
      auto found = groups.find(key);
      if (found == groups.end())
      {
        group = &groups.insert({ key, BuildingGroup() }).first->second;
        group->verts.reserve(reserveVerts);
        SetGroupState(*group, ph);
      }
      else
        group = &found->second;
    }
    lastKey = key;

    Poly p{};
    p.count = ph.NumVerts();
    const float uvScale = ph.UVScale();
    ph.FaceNormal(p.faceNormal);

    auto texCoord = [&](uint16_t u, uint16_t v, float out[2]) {
      out[0] = (float(u) * uvScale) / float(group->width);
      out[1] = (float(v) * uvScale) / float(group->height);
    };

    // Shared vertices first, in slot order. Their texture coordinates are
    // redone when the texture may differ.
    int j = 0;
    for (int i = 0; i < 4; i++)
    {
      if (!ph.SharedVertex(i))
        continue;
      p.v[j] = m_lastVerts[i];
      uv[j][0] = m_lastUV[i][0];
      uv[j][1] = m_lastUV[i][1];
      if (newState && group->textured)
        texCoord(uv[j][0], uv[j][1], p.v[j].texcoords);
      j++;
    }

    if (ph.PolyColor())
    {
      p.faceColour[0] = uint8_t(ph.header[4] >> 24);
      p.faceColour[1] = uint8_t(ph.header[4] >> 16);
      p.faceColour[2] = uint8_t(ph.header[4] >> 8);
    }
    else
    {
      const uint32_t rgb = m_polyRAM[m_colourTable + uint32_t(ph.ColorIndex())];
      p.faceColour[0] = uint8_t(rgb >> 16);
      p.faceColour[1] = uint8_t(rgb >> 8);
      p.faceColour[2] = uint8_t(rgb);
    }
    p.faceColour[3] = ph.Transparency();
    if (ph.Discard1() && !ph.Discard2())
      p.faceColour[3] /= 2;

    // Flat shading uses the face normal everywhere, shared vertices too.
    if (!ph.SmoothShading())
      for (int i = 0; i < p.count; i++)
        V3::Copy(p.faceNormal, p.v[i].normal);

    p.textureNP = ph.TextureNP() * m_textureNPScale;

    const uint32_t *w = ph.StartOfData();
    for (; j < p.count; j++, w += 4)
    {
      VertexBase &v = p.v[j];
      v.pos[0] = VertexCoord(w[0], m_vertexScale);
      v.pos[1] = VertexCoord(w[1], m_vertexScale);
      v.pos[2] = VertexCoord(w[2], m_vertexScale);
      v.pos[3] = 1.0f;

      if (ph.SmoothShading())
      {
        v.normal[0] = SignedByte(w[0]);
        v.normal[1] = SignedByte(w[1]);
        v.normal[2] = SignedByte(w[2]);
      }
      else if (ph.FixedShading())
      {
        // Specular polygons keep their fixed shade unsigned.
        v.fixedShade = ph.SpecularEnabled() ? float(w[0] & 0xFF) * float(1.0 / 255.0) : SignedByte(w[0]);
      }

      uv[j][0] = uint16_t(w[3] >> 16);
      uv[j][1] = uint16_t(w[3] & 0xFFFF);
      v.texcoords[0] = v.texcoords[1] = 0.0f;
      if (group->textured)
        texCoord(uv[j][0], uv[j][1], v.texcoords);
    }

    if (!ph.Discard())
    {
      if (ph.DoubleSided())
      {
        // The back face, lit from the other side.
        Poly back = p;
        V3::Negate(back.faceNormal);
        for (int i = 0; i < back.count; i++)
          V3::Negate(back.v[i].normal);
        EmitPoly(back, group->verts);
      }
      EmitPoly(p, group->verts);
    }

    // Keep this polygon's vertices for the next one to share (a triangle
    // leaves the fourth slot as it was).
    for (int i = 0; i < p.count; i++)
    {
      m_lastVerts[i] = p.v[i];
      m_lastUV[i][0] = uv[i][0];
      m_lastUV[i][1] = uv[i][1];
    }
  } while (ph.NextPoly());

  std::vector<GPUVertex> &dest = model.dynamic ? m_ramVerts : m_romVerts;
  const int base = model.dynamic ? int(kRomVerts) : 0;
  model.groups->reserve(groups.size());
  for (auto &entry : groups)
  {
    BuildingGroup &g = entry.second;
    g.vboOffset = base + int(dest.size());
    g.vertexCount = int(g.verts.size());
    dest.insert(dest.end(), g.verts.begin(), g.verts.end());
    model.groups->push_back(static_cast<const DrawGroup &>(g));
  }
}

/******************************************************************************
 Matrices
******************************************************************************/

// Real3D matrices are 12 floats: translation X, Y, Z, then the 3x3 rotation
// row by row.
void CNew3D::ApplyNodeMatrix(uint32_t index)
{
  if (m_matrixBase == nullptr)
    return;
  const float *src = m_matrixBase + index * 12;
  const float m[16] =
  {
    src[3], src[6], src[9],  0.0f,      // column 0
    src[4], src[7], src[10], 0.0f,      // column 1
    src[5], src[8], src[11], 0.0f,      // column 2
    src[0], src[1], src[2],  1.0f,      // column 3: translation
  };
  m_modelMat.MultMatrix(m);
}

// The Real3D takes vectors as (Z, X, Y) with Y and Z the opposite way to
// OpenGL; the first matrix maps them back. Matrix 0 of the viewport's
// matrix table (the camera) follows.
void CNew3D::StartMatrixStack(uint32_t matrixBase)
{
  const float axes[16] =
  {
    0.0f, 0.0f, -1.0f, 0.0f,
    1.0f, 0.0f,  0.0f, 0.0f,
    0.0f, -1.0f, 0.0f, 0.0f,
    0.0f, 0.0f,  0.0f, 1.0f,
  };
  m_modelMat.LoadMatrix(axes);
  m_matrixBase = reinterpret_cast<const float *>(CullingWords(matrixBase));
  ApplyNodeMatrix(0);
}

// Cancels the current rotation (and scale) while keeping the position:
// multiplies by the transposed, column-normalised rotation part.
void CNew3D::RemoveRotation()
{
  const float *c = m_modelMat.currentMatrix;
  float m[16] =
  {
    c[0], c[4], c[8],  0.0f,
    c[1], c[5], c[9],  0.0f,
    c[2], c[6], c[10], 0.0f,
    0.0f, 0.0f, 0.0f,  1.0f,
  };
  for (int col = 0; col < 3; col++)
  {
    float *v = m + col * 4;
    float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
  }
  m_modelMat.MultMatrix(m);
}

/******************************************************************************
 Drawing
******************************************************************************/

bool CNew3D::LayerHasModels(int priority) const
{
  for (const ViewportNode &n : m_frame)
    if (n.viewport.priority == priority && !n.models.empty())
      return true;
  return false;
}

// Draws one pass of a priority layer. Returns whether any high priority
// polygons were seen (they need a second pass of their own).
bool CNew3D::DrawPriority(int priority, bool highPriorityPass, Layer layer)
{
  glActiveTexture(GL_TEXTURE0);
  m_sheets[0].Bind();
  glActiveTexture(GL_TEXTURE1);
  m_sheets[1].Bind();
  glActiveTexture(GL_TEXTURE0);

  bool sawHighPriority = false;
  for (ViewportNode &n : m_frame)
  {
    if (n.viewport.priority != priority || n.models.empty())
      continue;

    glViewport(n.viewport.x, n.viewport.y, n.viewport.width, n.viewport.height);
    m_shader.SetViewportUniforms(&n.viewport);

    for (const ModelInstance &m : n.models)
    {
      bool modelSent = false;
      for (const DrawGroup &g : *m.groups)
      {
        if (g.highPriority)
          sawHighPriority = true;
        if (!g.DrawnIn(layer, m.alpha) || g.highPriority != highPriorityPass)
          continue;
        if (!modelSent)
        {
          m_shader.SetModelStates(&m);
          modelSent = true;
        }
        m_shader.SetMeshUniforms(&g);
        glDrawArrays(m_primitive, g.vboOffset, g.vertexCount);
      }
    }
  }
  return sawHighPriority;
}

void CNew3D::BeginPolygonPass()
{
  m_vbo.Bind(true);
  glBindVertexArray(m_vao);
  m_shader.SetShader(true);

  glDepthFunc(GL_GEQUAL);       // reversed Z
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_CULL_FACE);      // back faces are handled in the shader
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0xFF);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_BLEND);
}

void CNew3D::EndPolygonPass()
{
  m_vbo.Bind(false);
  glBindVertexArray(0);
  m_shader.SetShader(false);
  glDisable(GL_STENCIL_TEST);
}

// Fog ambient also darkens the 2D background. The lowest priority layer
// that has viewports decides: its darkest fog ambient among viewports with
// fog (density or start) and no scroll fog.
void CNew3D::DrawBackgroundFog()
{
  const ViewportState *chosen = nullptr;
  float ambient = 1.0f;

  for (int pri = 0; pri < 4; pri++)
  {
    bool any = false;
    for (const ViewportNode &n : m_frame)
    {
      const ViewportState &vp = n.viewport;
      if (vp.priority != pri)
        continue;
      any = true;
      if (vp.scrollFog > 0.0f || (vp.fogDensity <= 0.0f && vp.fogStart <= 0.0f))
        continue;
      if (vp.fogAmbient < ambient)
      {
        chosen = &vp;
        ambient = vp.fogAmbient;
      }
    }
    if (any)
      break;
  }

  if (chosen == nullptr)
    return;
  float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f - ambient };
  float spotColour[3] = { chosen->spotFogColor[0], chosen->spotFogColor[1], chosen->spotFogColor[2] };
  float spotEllipse[4] = { chosen->spotEllipse[0], chosen->spotEllipse[1], chosen->spotEllipse[2], chosen->spotEllipse[3] };
  glViewport(chosen->x, chosen->y, chosen->width, chosen->height);
  m_fog.DrawScrollFog(rgba, 0.0f, 1.0f, spotColour, spotEllipse);   // no spotlight here
}

// Scroll fog covers the 2D background with fog colour. The first priority
// layer (from 0) with any scroll fog supplies the strongest value; it only
// shows if some viewport with the same fog colour has fog or spotlight on
// fog switched on, and that viewport supplies the fog's other settings.
void CNew3D::DrawScrollFog()
{
  const ViewportState *source = nullptr;
  for (int pri = 0; pri < 4 && source == nullptr; pri++)
  {
    for (const ViewportNode &n : m_frame)
    {
      const ViewportState &vp = n.viewport;
      if (vp.priority == pri && vp.scrollFog > 0.0f && (source == nullptr || vp.scrollFog > source->scrollFog))
        source = &vp;
    }
  }
  if (source == nullptr)
    return;

  for (const ViewportNode &n : m_frame)
  {
    const ViewportState &vp = n.viewport;
    if (vp.fogColour[0] != source->fogColour[0] || vp.fogColour[1] != source->fogColour[1] || vp.fogColour[2] != source->fogColour[2])
      continue;

    const bool spotColoured = vp.spotFogColor[0] > 0.0f || vp.spotFogColor[1] > 0.0f || vp.spotFogColor[2] > 0.0f;
    const bool fog = vp.fogDensity > 0.0f || vp.fogStart > 0.0f || vp.scrollAtt > 0.0f;
    const bool spotFog = vp.fogAttenuation > 0.0f && spotColoured;
    if (!fog && !spotFog)
      continue;

    float rgba[4] = { source->fogColour[0], source->fogColour[1], source->fogColour[2], source->scrollFog };
    float attenuation = (fog && spotFog) ? 0.0f : vp.fogAttenuation;   // with regular fog on, spotlight-on-fog is dropped
    float spotColour[3] = { vp.spotFogColor[0], vp.spotFogColor[1], vp.spotFogColor[2] };
    float spotEllipse[4] = { vp.spotEllipse[0], vp.spotEllipse[1], vp.spotEllipse[2], vp.spotEllipse[3] };
    glViewport(source->x, source->y, source->width, source->height);
    m_fog.DrawScrollFog(rgba, attenuation, vp.fogAmbient, spotColour, spotEllipse);
    return;
  }
}

// Line of sight: depth (in units of the near plane) at the viewport's test
// pixel, with the low mantissa bit set when nothing that returns line of
// sight was hit. The first viewport of the layer with a test position is
// used.
void CNew3D::ReadLineOfSight(int priority)
{
  for (const ViewportNode &n : m_frame)
  {
    const ViewportState &vp = n.viewport;
    if (vp.priority != priority || (vp.losPosX == 0 && vp.losPosY == 0))
      continue;

    const int px = int(m_xOffs) + int(float(vp.losPosX) * m_xRatio);
    const int py = int(m_yOffs) + int(float(384 - vp.losPosY) * m_yRatio);

    float pixel[2] = { 0.0f, 0.0f };    // stays 0 if the point is off the frame buffer
#ifdef SUPERMODEL_GLES
    uint32_t stencilBits = 0;
    m_targets.ReadDepthStencil(px, py, &pixel[0], &stencilBits);
    pixel[1] = AsFloat(stencilBits);
#else
    glReadPixels(px, py, 1, 1, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, pixel);
#endif
    const uint8_t stencil = uint8_t(AsBits(pixel[1]) & 0x80);   // bit 7: polygon with line-of-sight return

    uint32_t bits = AsBits(pixel[0] / kNear);
    bits = stencil ? (bits & ~1u) : (bits | 1u);

    std::lock_guard<std::mutex> lock(m_losLock);
    m_los[m_losRead ^ 1][priority] = AsFloat(bits);
    return;
  }
}

void CNew3D::RenderFrame(void)
{
  {
    // Last frame's results become readable; this frame's start at zero.
    std::lock_guard<std::mutex> lock(m_losLock);
    m_losRead ^= 1;
    std::fill(std::begin(m_los[m_losRead ^ 1]), std::end(m_los[m_losRead ^ 1]), 0.0f);
  }

  m_ramVerts.clear();
  m_frame.clear();
  m_siblings.clear();
  m_modelMat.Release();
  m_state.Reset();

  if (m_blockCulling && !m_noWhiteFlash)
  {
    // With 3D output blocked the board shows a white screen.
    if (m_aaTarget)
      glBindFramebuffer(GL_FRAMEBUFFER, m_aaTarget);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_aaTarget)
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }

  WalkViewports(0x800000);

  m_vbo.Bind(true);
  m_vbo.BufferSubData(GLintptr(kRomVerts * sizeof(GPUVertex)), GLsizeiptr(m_ramVerts.size() * sizeof(GPUVertex)), m_ramVerts.data());

  // Send VROM vertices added this frame. If the ROM area is full, start
  // again (models drawn this frame still find their vertices in the
  // buffer; later frames decode them anew).
  const size_t sentVerts = size_t(m_vbo.GetSize()) / sizeof(GPUVertex);
  if (!m_romVerts.empty() && m_romVerts.size() != sentVerts)
  {
    if (m_romVerts.size() >= kRomVerts)
    {
      m_romVerts.clear();
      m_romModels.clear();
      m_vbo.Reset();
    }
    else
      m_vbo.AppendData(GLsizeiptr((m_romVerts.size() - sentVerts) * sizeof(GPUVertex)), &m_romVerts[sentVerts]);
  }

  // The colour clear reaches all three targets.
  m_targets.SetFBO(Layer::colour);
  glClear(GL_COLOR_BUFFER_BIT);

  DrawBackgroundFog();
  DrawScrollFog();

  for (int pri = 0; pri < 4; pri++)
  {
    if (!LayerHasModels(pri))
      continue;

    // Normal polygons, then (if there are any) high priority ones, which
    // get a fresh depth buffer and so draw over the rest.
    for (int pass = 0; pass < 2; pass++)
    {
      const bool highPriority = pass == 1;
      BeginPolygonPass();

      m_targets.SetFBO(Layer::colour);
      glClearDepth(0.0);
      glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

      m_shader.DiscardAlpha(true);
      m_shader.SetLayer(Layer::colour);
      const bool hasHighPriority = DrawPriority(pri, highPriority, Layer::colour);
      if (!highPriority)
        ReadLineOfSight(pri);

      // Each translucency layer is tested against the opaque depth only.
      glDepthFunc(GL_GREATER);
      m_shader.DiscardAlpha(false);

      m_targets.StoreDepth();
      m_shader.SetLayer(Layer::trans1);
      m_targets.SetFBO(Layer::trans1);
      DrawPriority(pri, highPriority, Layer::trans1);

      m_targets.RestoreDepth();
      m_shader.SetLayer(Layer::trans2);
      m_targets.SetFBO(Layer::trans2);
      DrawPriority(pri, highPriority, Layer::trans2);

      EndPolygonPass();
      if (!hasHighPriority)
        break;
    }
  }

  m_targets.SetFBO(Layer::none);
  if (m_aaTarget)
    glBindFramebuffer(GL_FRAMEBUFFER, m_aaTarget);
  m_targets.Draw();
  if (m_aaTarget)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // New3D
