// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Models.cpp
//
// Turning Real3D models into triangles, caching them, and the draw lists.
//
// A model is a run of polygons, each a 7-word header followed by its new
// vertices (4 words each); header word 0's low 4 bits pick which corners of
// the previous polygon it reuses. Triangles are built in two buffers (opaque
// and translucent) and then copied into the cache's vertex buffer, opaque
// first. Models are cached per address and texture-offset state. The VROM
// cache keeps its models until it fills up; the polygon RAM cache is
// emptied every frame and also takes VROM models that read the colour table
// in polygon RAM.
//
// Each cache has two draw lists, one per pass. A viewport adds an item to
// both lists of both caches, carrying its projection and lighting; a model
// instance adds its modelview matrix to the lists of the passes it has
// triangles in. If a cache fills up mid-frame, everything listed so far is
// drawn and both caches start again empty.

#include "Legacy3D.h"
#include "Supermodel.h"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace Legacy3D {

static_assert(sizeof(float) * 26 == 104, "vertex size assumption");

namespace {

constexpr int At(int r, int c)
{
  return c * 4 + r;
}

constexpr GLfloat kByte = 1.0f / 255.0f;

} // namespace

/******************************************************************************
 Draw lists
******************************************************************************/

void CLegacy3D::Link(ModelCache &cache, int pass, int item)
{
  cache.items[item].next = -1;
  if (cache.head[pass] < 0)
    cache.head[pass] = item;
  else
    cache.items[cache.tail[pass]].next = item;
  cache.tail[pass] = item;
}

void CLegacy3D::ClearItems(ModelCache &cache)
{
  cache.items.clear();
  for (int p = 0; p < 2; p++)
    cache.head[p] = cache.tail[p] = -1;
}

Result CLegacy3D::AddViewportItem(ModelCache &cache)
{
  if (cache.items.size() + 2 > cache.itemsMax)
    return Result::FAIL;
  for (int pass = 0; pass < 2; pass++)
  {
    cache.items.emplace_back();
    DrawItem &d = cache.items.back();
    d.isViewport = true;
    glGetFloatv(GL_PROJECTION_MATRIX, d.matrix);
    memcpy(d.lighting, m_lighting, sizeof(d.lighting));
    memcpy(d.spotEllipse, m_spotEllipse, sizeof(d.spotEllipse));
    memcpy(d.spotRange, m_spotRange, sizeof(d.spotRange));
    memcpy(d.spotColour, m_spotColour, sizeof(d.spotColour));
    memcpy(d.fog, m_fog, sizeof(d.fog));
    d.vpX = m_vpX;
    d.vpY = m_vpY;
    d.vpW = m_vpW;
    d.vpH = m_vpH;
    Link(cache, pass, int(cache.items.size() - 1));
  }
  return Result::OKAY;
}

// GL culls by the screen winding but the Real3D by the polygon normal. A
// matrix that mirrors an axis flips one and not the other, so compare the
// winding of the unit X, Y edges after the matrix with the Z normal after
// the normal matrix (inverse transpose); the coordinate system's Z sign
// (matrix 0, as when the triangles were built) sets the normal's direction.
GLint CLegacy3D::FrontFaceFor(const GLfloat mv[16]) const
{
  auto a = [mv](int r, int c) { return mv[At(r, c)]; };
  GLfloat det = a(0, 0) * (a(2, 2) * a(1, 1) - a(2, 1) * a(1, 2)) - a(1, 0) * (a(2, 2) * a(0, 1) - a(2, 1) * a(0, 2)) +
                a(2, 0) * (a(1, 2) * a(0, 1) - a(1, 1) * a(0, 2));
  GLfloat inv = 1.0f / det;
  // Bottom row of the inverse transpose
  GLfloat n0 = inv * (a(1, 2) * a(0, 1) - a(1, 1) * a(0, 2));
  GLfloat n1 = inv * (-(a(1, 2) * a(0, 0) - a(1, 0) * a(0, 2)));
  GLfloat n2 = inv * (a(1, 1) * a(0, 0) - a(1, 0) * a(0, 1));
  GLfloat zSign = m_matrices ? -1.0f * m_matrices[5] : 0.0f;
  GLfloat normalZ = n0 * 0.0f + n1 * 0.0f + n2 * zSign;

  // Edges: the matrix's first two columns (by multiplying, so that
  // infinities spread the same way)
  GLfloat ex0 = a(0, 0) * 1.0f + a(0, 1) * 0.0f + a(0, 2) * 0.0f;
  GLfloat ex1 = a(1, 0) * 1.0f + a(1, 1) * 0.0f + a(1, 2) * 0.0f;
  GLfloat ey0 = a(0, 0) * 0.0f + a(0, 1) * 1.0f + a(0, 2) * 0.0f;
  GLfloat ey1 = a(1, 0) * 0.0f + a(1, 1) * 1.0f + a(1, 2) * 0.0f;
  GLfloat windingZ = ex0 * ey1 - ex1 * ey0;

  GLfloat product = normalZ * windingZ;
  if (product > 0.0f)
    return GL_CW;
  if (product < 0.0f)
    return GL_CCW;
  return kNoCulling;   // edge-on, or not a number
}

Result CLegacy3D::AddModelItems(ModelCache &cache, const CachedModel &model)
{
  if (cache.items.size() + 2 > cache.itemsMax)
    return Result::FAIL;
  for (int pass = 0; pass < 2; pass++)
  {
    if (model.count[pass] == 0)
      continue;
    cache.items.emplace_back();
    DrawItem &d = cache.items.back();
    d.isViewport = false;
    d.first = model.first[pass];
    d.count = model.count[pass];
    d.stencil = model.stencil;
    glGetFloatv(GL_MODELVIEW_MATRIX, d.matrix);
    d.frontFace = FrontFaceFor(d.matrix);
    Link(cache, pass, int(cache.items.size() - 1));
  }
  return Result::OKAY;
}

void CLegacy3D::DrawItems(ModelCache &cache, Pass pass)
{
  const GLsizei stride = sizeof(GpuVertex);
  auto at = [](size_t offset) { return reinterpret_cast<const GLvoid *>(offset); };
  glBindBuffer(GL_ARRAY_BUFFER, cache.vbo);
  glVertexPointer(3, GL_FLOAT, stride, at(offsetof(GpuVertex, pos)));
  glNormalPointer(GL_FLOAT, stride, at(offsetof(GpuVertex, normal)));
  glTexCoordPointer(2, GL_FLOAT, stride, at(offsetof(GpuVertex, uv)));
  glColorPointer(3, GL_FLOAT, stride, at(offsetof(GpuVertex, colour)));
  struct { GLint loc; GLint size; size_t offset; } attributes[] = {
    { m_aSubTexture, 4, offsetof(GpuVertex, subTexture) },
    { m_aTexParams, 4, offsetof(GpuVertex, texParams) },
    { m_aTexFormat, 1, offsetof(GpuVertex, texFormat) },
    { m_aTexMap, 1, offsetof(GpuVertex, texMap) },
    { m_aTransLevel, 1, offsetof(GpuVertex, transLevel) },
    { m_aLightEnable, 1, offsetof(GpuVertex, lightEnable) },
    { m_aShininess, 1, offsetof(GpuVertex, shininess) },
    { m_aSpecular, 1, offsetof(GpuVertex, specular) },
    { m_aFogIntensity, 1, offsetof(GpuVertex, fogIntensity) },
  };
  for (const auto &a : attributes)
    if (a.loc != -1)
      glVertexAttribPointer(a.loc, a.size, GL_FLOAT, GL_FALSE, stride, at(a.offset));

  if (pass == kTranslucent)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  else
    glDisable(GL_BLEND);
  bool stencil = false;
  glDisable(GL_STENCIL_TEST);

  for (int i = cache.head[pass]; i >= 0; i = cache.items[i].next)
  {
    const DrawItem &d = cache.items[i];
    if (d.isViewport)
    {
      // Only worth setting up if a model of this pass follows.
      if (d.next < 0 || cache.items[d.next].isViewport)
        continue;
      if (m_uLighting != -1)
        glUniform3fv(m_uLighting, 2, d.lighting);
      if (m_uProjection != -1)
        glUniformMatrix4fv(m_uProjection, 1, GL_FALSE, d.matrix);
      glFogf(GL_FOG_DENSITY, d.fog[3]);
      glFogf(GL_FOG_START, d.fog[4]);
      glFogfv(GL_FOG_COLOR, d.fog);
      if (m_uSpotEllipse != -1)
        glUniform4fv(m_uSpotEllipse, 1, d.spotEllipse);
      if (m_uSpotRange != -1)
        glUniform2fv(m_uSpotRange, 1, d.spotRange);
      if (m_uSpotColour != -1)
        glUniform3fv(m_uSpotColour, 1, d.spotColour);
      glViewport(d.vpX, d.vpY, d.vpW, d.vpH);
      continue;
    }

    if (d.stencil != stencil)
    {
      stencil = d.stencil;
      if (stencil)
        glEnable(GL_STENCIL_TEST);
      else
        glDisable(GL_STENCIL_TEST);
    }
    if (d.frontFace == kNoCulling)
      glDisable(GL_CULL_FACE);
    else
      glFrontFace(GLenum(d.frontFace));
    if (m_uModelView != -1)
      glUniformMatrix4fv(m_uModelView, 1, GL_FALSE, d.matrix);
    glDrawArrays(GL_TRIANGLES, GLint(d.first), GLsizei(d.count));
    if (d.frontFace == kNoCulling)
      glEnable(GL_CULL_FACE);
  }
}

void CLegacy3D::DrawAllLists()
{
  DrawItems(m_romCache, kOpaque);
  DrawItems(m_ramCache, kOpaque);
  DrawItems(m_romCache, kTranslucent);
  DrawItems(m_ramCache, kTranslucent);
}

/******************************************************************************
 Building models
******************************************************************************/

int CLegacy3D::TexturePosX(const uint32_t *h) const
{
  int x = 32 * int(((h[4] & 0x7F) << 1) | ((h[5] >> 7) & 1));
  return (x + m_texOffset.x) & 2047;
}

int CLegacy3D::TexturePosY(const uint32_t *h) const
{
  int y = 32 * int(h[5] & 0x7F);
  int bank = int((h[4] & 0x40) << 4);
  return ((y + m_texOffset.y) & 1023) + (bank ^ m_texOffset.bankFlip);
}

void CLegacy3D::AddCorner(ModelCache &cache, const Corner &c, const Polygon &poly, float normalSign)
{
  const uint32_t *h = poly.header;
  bool textured = (h[6] & 0x400) != 0;
  unsigned format = (h[6] >> 7) & 7;
  const TexSheet &sheet = *m_sheetOfFormat[format];
  bool lit = (h[6] & 0x10000) == 0;
  bool fixedShading = (h[1] & 0x20) != 0;
  bool smoothShading = (h[1] & 0x08) != 0;

  // Colour: from the colour table in polygon RAM, or the header's RGB.
  GLfloat r, g, b;
  if ((h[1] & 2) == 0)
  {
    uint32_t rgb = m_polyRAM[m_colourTable + ((h[4] >> 8) & 0xFFF)];
    r = GLfloat((rgb >> 16) & 0xFF) * kByte;
    g = GLfloat((rgb >> 8) & 0xFF) * kByte;
    b = GLfloat(rgb & 0xFF) * kByte;
  }
  else
  {
    r = GLfloat(h[4] >> 24) * kByte;
    g = GLfloat((h[4] >> 16) & 0xFF) * kByte;
    b = GLfloat((h[4] >> 8) & 0xFF) * kByte;
  }
  // Textured polygons using a translator map are not tinted.
  if (textured && (h[4] & 0x80))
    r = g = b = 1.0f;

  // Lit polygons use the vertex normals (smooth), a fixed shade per vertex
  // (then drawn unlit), or the polygon normal (flat).
  const GLfloat *normal = nullptr;
  GLfloat shade = 1.0f;
  if (lit)
  {
    if (smoothShading)
      normal = c.n;
    else if (fixedShading)
    {
      shade = c.shade;
      lit = false;
    }
    else
      normal = poly.normal;
  }
  r *= shade;
  g *= shade;
  b *= shade;

  GLfloat specular = 0.0f, shininess = -1.0f;
  if (h[0] & 0x80)
  {
    specular = GLfloat((h[0] >> 26) & 0x3F) * (1.0f / 63.0f);
    shininess = GLfloat(2 << ((h[6] >> 5) & 3));
  }
  GLfloat trans = (h[6] & 0x800000) ? 1.0f : GLfloat((h[6] >> 18) & 0x1F) * (1.0f / 31.0f);
  GLfloat fog = GLfloat((h[6] >> 11) & 0x1F) * (1.0f / 15.0f);
  // Cut out (near) zero alpha for contour textures and the alpha formats.
  bool contour = (h[6] & 0x80000000) || format == 7 || (format == 1 && (h[6] & 2)) || (format == 3 && (h[6] & 4));

  GpuVertex v;
  v.pos[0] = c.x;
  v.pos[1] = c.y;
  v.pos[2] = c.z;
  for (int i = 0; i < 3; i++)
    v.normal[i] = fixedShading ? 0.0f : (normal ? normal[i] : 0.0f) * normalSign;
  v.colour[0] = r;
  v.colour[1] = g;
  v.colour[2] = b;
  v.uv[0] = c.u;
  v.uv[1] = c.v;
  v.subTexture[0] = GLfloat(sheet.originX + unsigned(TexturePosX(h)));
  v.subTexture[1] = GLfloat(sheet.originY + unsigned(TexturePosY(h)));
  v.subTexture[2] = GLfloat(32 << ((h[3] >> 3) & 7));
  v.subTexture[3] = GLfloat(32 << (h[3] & 7));
  v.texParams[0] = textured ? 1.0f : 0.0f;
  v.texParams[1] = contour ? 1.0f : -1.0f;
  v.texParams[2] = (h[2] & 2) ? 1.0f : 0.0f;
  v.texParams[3] = (h[2] & 1) ? 1.0f : 0.0f;
  v.texFormat = GLfloat(format);
  v.texMap = GLfloat(sheet.map);
  v.transLevel = trans;
  v.lightEnable = lit ? 1.0f : 0.0f;
  v.specular = specular;
  v.shininess = shininess;
  v.fogIntensity = fog;

  cache.building[poly.pass].push_back(v);
  cache.vboUsed += sizeof(GpuVertex);
}

Result CLegacy3D::AddPolygon(ModelCache &cache, const Polygon &poly)
{
  // A double-sided quad is the most: 4 triangles.
  if (cache.building[poly.pass].size() + 12 >= cache.buildingMax)
    return ReportLocalOverflow();
  if (cache.vboUsed + 12 * sizeof(GpuVertex) >= cache.vboBytes)
    return Result::FAIL;   // the cache is full

  // Triangles are stored clockwise: compare the corners' winding with the
  // polygon normal (taking the coordinate system's Z direction into
  // account) and reverse the corners if they disagree.
  const Corner *k = poly.corner;
  GLfloat e1[3] = { k[0].x - k[1].x, k[0].y - k[1].y, k[0].z - k[1].z };
  GLfloat e2[3] = { k[2].x - k[1].x, k[2].y - k[1].y, k[2].z - k[1].z };
  GLfloat cross[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
  GLfloat zSign = m_matrices ? -1.0f * m_matrices[5] : 0.0f;
  bool clockwise = zSign * (cross[0] * poly.normal[0] + cross[1] * poly.normal[1] + cross[2] * poly.normal[2]) >= 0.0;
  bool twoSided = (poly.header[1] & 0x10) != 0;
  bool quad = poly.corners == 4;

  static constexpr int kFront[2][2][3] = { { { 2, 1, 0 }, { 0, 3, 2 } },     // reversed: first, second triangle
                                           { { 0, 1, 2 }, { 0, 2, 3 } } };   // as given
  for (int t = 0; t < (quad ? 2 : 1); t++)
  {
    const int *front = kFront[clockwise][t];
    for (int i = 0; i < 3; i++)
      AddCorner(cache, k[front[i]], poly, 1.0f);
    if (twoSided)   // the back face, wound the other way
    {
      const int *back = kFront[!clockwise][t];
      for (int i = 0; i < 3; i++)
        AddCorner(cache, k[back[i]], poly, -1.0f);
    }
  }
  return Result::OKAY;
}

// A VROM model that takes any polygon's colour from the colour table must
// be rebuilt each frame, since the table lives in polygon RAM.
bool CLegacy3D::UsesColourTable(const uint32_t *words)
{
  static constexpr unsigned kReused[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
  for (;;)
  {
    if ((words[1] & 2) == 0)
      return true;
    if (words[6] == 0)
      return false;
    unsigned fresh = ((words[0] & 0x40) ? 4 : 3) - kReused[words[0] & 0xF];
    bool last = (words[1] & 4) != 0;
    words += 7 + 4 * fresh;
    if (last)
      return false;
  }
}

void CLegacy3D::FinishModel(ModelCache &cache, CachedModel &model, uint32_t addr, uint16_t offsetState, bool stencil)
{
  for (int p = 0; p < 2; p++)
    model.count[p] = unsigned(cache.building[p].size());
  model.first[kTranslucent] = model.first[kOpaque] + model.count[kOpaque];

  glBindBuffer(GL_ARRAY_BUFFER, cache.vbo);
  for (int p = 0; p < 2; p++)
    if (model.count[p] > 0)
      glBufferSubData(GL_ARRAY_BUFFER, GLintptr(model.first[p]) * sizeof(GpuVertex), GLsizeiptr(model.count[p]) * sizeof(GpuVertex),
                      cache.building[p].data());

  model.address = addr;
  model.offsetState = offsetState;
  model.stencil = stencil;
  int index = int(cache.modelCount++);
  auto it = cache.newest.find(addr);
  model.older = it != cache.newest.end() ? it->second : -1;
  cache.newest[addr] = index;
}

CLegacy3D::CachedModel *CLegacy3D::BuildModel(ModelCache &cache, uint32_t addr, uint16_t offsetState, const uint32_t *words)
{
  if (cache.modelCount >= cache.modelsMax)
    return nullptr;
  CachedModel &model = cache.models[cache.modelCount];
  for (int p = 0; p < 2; p++)
    cache.building[p].clear();
  model.textures.Clear();
  model.first[kOpaque] = model.first[kTranslucent] = unsigned(cache.vboUsed / sizeof(GpuVertex));
  model.count[kOpaque] = model.count[kTranslucent] = 0;
  model.older = -1;
  model.stencil = false;

  // The polygon record carries over from one polygon to the next: a
  // triangle leaves the fourth corner as it was.
  Polygon poly{};
  Corner previous[4]{};
  bool stencil = true;   // only if every polygon is layered or a shadow
  bool last = false;
  while (!last)
  {
    const uint32_t *h = words;
    words += 7;
    if (h[6] == 0)
      break;
    poly.header = h;
    bool discarded = (h[0] & 0x300) == 0x300;
    last = (h[1] & 4) != 0;
    poly.corners = (h[0] & 0x40) ? 4 : 3;

    bool textured = (h[6] & 0x400) != 0;
    unsigned format = (h[6] >> 7) & 7;
    bool translucent = (h[6] & 0x00800000) == 0;
    if (format == 1)
      poly.pass = (h[6] & 2) ? kTranslucent : kOpaque;
    else if (format == 3)
      poly.pass = (h[6] & 4) ? kTranslucent : kOpaque;
    else
      poly.pass = (translucent || format == 7 || format == 4) ? kTranslucent : kOpaque;

    // Layered polygons, and what look like shadows (unlit, untextured and
    // translucent), are drawn with the stencil test.
    bool shadow = (h[6] & 0x10000) && translucent && !textured;
    stencil = stencil && ((h[6] & 0x8) || shadow);

    if (textured)
    {
      int x = TexturePosX(h), y = TexturePosY(h);
      int w = 32 << ((h[3] >> 3) & 7), ht = 32 << (h[3] & 7);
      if (cache.dynamic)
        DecodeTexture(int(format), x, y, w, ht);
      else
        model.textures.AddRef(format, unsigned(x), unsigned(y), unsigned(w), unsigned(ht));
    }

    // Normal: sign + 1.22 fixed point in the top 24 bits of words 1-3.
    for (int i = 0; i < 3; i++)
      poly.normal[i] = GLfloat(int32_t(h[1 + i]) >> 8) * (1.0f / 4194304.0f);

    unsigned n = 0;
    for (int i = 0; i < 4; i++)
      if (h[0] & (1u << i))
        poly.corner[n++] = previous[i];
    GLfloat uvScale = (h[1] & 0x40) ? 1.0f : (1.0f / 8.0f);
    for (; n < poly.corners; n++, words += 4)
    {
      Corner &c = poly.corner[n];
      c.x = GLfloat(int32_t(words[0]) >> 8) * m_vertexScale;
      c.y = GLfloat(int32_t(words[1]) >> 8) * m_vertexScale;
      c.z = GLfloat(int32_t(words[2]) >> 8) * m_vertexScale;
      c.n[0] = GLfloat(int8_t(words[0] & 0xFF));
      c.n[1] = GLfloat(int8_t(words[1] & 0xFF));
      c.n[2] = GLfloat(int8_t(words[2] & 0xFF));
      c.u = GLfloat(uint16_t(words[3] >> 16)) * uvScale;
      c.v = GLfloat(uint16_t(words[3] & 0xFFFF)) * uvScale;
      c.shade = GLfloat((words[0] + 128) & 0xFF) / 255.0f;   // signed byte, -128 black .. 127 white
      GLfloat length = std::sqrt(c.n[0] * c.n[0] + c.n[1] * c.n[1] + c.n[2] * c.n[2]);
      for (int i = 0; i < 3; i++)
        c.n[i] /= length;
    }

    if (!discarded)
    {
      for (int i = 0; i < 4; i++)
        previous[i] = poly.corner[i];
      if (AddPolygon(cache, poly) != Result::OKAY)
        return nullptr;
    }
  }

  FinishModel(cache, model, addr, offsetState, stencil);
  return &model;
}

CLegacy3D::CachedModel *CLegacy3D::FindModel(ModelCache &cache, uint32_t addr, uint16_t offsetState)
{
  auto it = cache.newest.find(addr);
  if (it == cache.newest.end())
    return nullptr;
  for (int i = it->second; i >= 0; i = cache.models[i].older)
    if (cache.models[i].offsetState == offsetState)
      return &cache.models[i];
  return nullptr;
}

void CLegacy3D::EmptyCache(ModelCache &cache)
{
  cache.vboUsed = 0;
  for (int p = 0; p < 2; p++)
    cache.building[p].clear();
  cache.newest.clear();
  cache.modelCount = 0;
  ClearItems(cache);
}

Result CLegacy3D::DrawModel(uint32_t addr)
{
  if (addr == 0x200000)   // Virtual On 2 boot data, very slow and not a model
    return Result::OKAY;
  const uint32_t *words = ModelWords(addr);
  uint32_t key = addr & 0xFFFFFF;
  uint16_t state = m_texOffset.state;

  ModelCache *cache = addr >= 0x100000 ? &m_romCache : &m_ramCache;
  CachedModel *model = FindModel(*cache, key, state);
  if (!model && cache == &m_romCache)
  {
    model = FindModel(m_ramCache, key, state);
    if (model)
      cache = &m_ramCache;
  }
  if (!model)
  {
    if (cache == &m_romCache && UsesColourTable(words))
      cache = &m_ramCache;
    model = BuildModel(*cache, key, state, words);
    if (!model)
    {
      // Out of room: draw what is listed, empty both caches and retry.
      DrawAllLists();
      EmptyCache(m_romCache);
      EmptyCache(m_ramCache);
      model = BuildModel(*cache, key, state, words);
      if (!model)
        return ReportUncachable(addr);
    }
  }

  if (!cache->dynamic)
    model->textures.DecodeAllTextures(this);
  return AddModelItems(*cache, *model);
}

/******************************************************************************
 Cache setup
******************************************************************************/

Result CLegacy3D::CreateCache(ModelCache &cache, size_t vboVerts, size_t localVerts, size_t maxModels, size_t maxItems, bool dynamic)
{
  cache.dynamic = dynamic;
  const char *kind = dynamic ? "dynamic" : "static";

  // Ask for the full buffer, then less a model's worth at a time.
  glGetError();
  glGenBuffers(1, &cache.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, cache.vbo);
  GLenum usage = dynamic ? GL_STREAM_DRAW : GL_STATIC_DRAW;
  size_t step = localVerts * sizeof(GpuVertex);
  size_t bytes = vboVerts * sizeof(GpuVertex);
  bool made = false;
  for (; bytes >= step; bytes -= step)
  {
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), nullptr, usage);
    if (glGetError() == GL_NO_ERROR)
    {
      made = true;
      break;
    }
  }
  if (!made)
  {
    bytes = step;
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), nullptr, usage);
    if (glGetError() != GL_NO_ERROR)
      return ErrorLog("Legacy3D: OpenGL could not make the %s vertex buffer.", kind);
  }
  InfoLog("Legacy3D: %s vertex buffer of %1.2f MB.", kind, double(bytes) / double(0x100000));

  cache.vboBytes = bytes;
  cache.vboUsed = 0;
  cache.buildingMax = localVerts;
  for (int p = 0; p < 2; p++)
  {
    cache.building[p].clear();
    cache.building[p].reserve(localVerts);
  }
  cache.models.clear();
  cache.models.resize(maxModels);
  cache.modelsMax = maxModels;
  cache.modelCount = 0;
  cache.newest.clear();
  cache.itemsMax = maxItems;
  cache.items.reserve(maxItems);
  ClearItems(cache);
  return Result::OKAY;
}

void CLegacy3D::DestroyCache(ModelCache &cache)
{
  if (cache.vbo && glDeleteBuffers)
    glDeleteBuffers(1, &cache.vbo);
  cache = ModelCache();
}

} // Legacy3D
