// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Legacy3D.cpp
//
// Setup, texture decoding and the walk over the Real3D scene database.
//
// The walk uses the GL fixed-function matrix stacks: a viewport loads the
// projection and the coordinate-system matrix, each culling node pushes its
// matrix or translation for its child and pops it before its sibling, and a
// model instance records the modelview matrix it was met with. Only what a
// frame shows is kept between viewports: the per-cache draw lists.

#include "Legacy3D.h"

#include "Supermodel.h"
#include "Shaders3D.h"
#include "Graphics/Shader.h"

#include <GL/glu.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Legacy3D {

namespace {

// Cache sizes
constexpr size_t kRomVboVerts = 700000;
constexpr size_t kRamVboVerts = 64000;
constexpr size_t kBuildVerts = 32768;      // one model, per pass
constexpr size_t kRomModels = 10000;
constexpr size_t kRamModels = 1024;
constexpr size_t kDrawItems = 10000;

constexpr int kMaxNodeDepth = 576;
constexpr int kMaxListDepth = 2;
constexpr size_t kMaxViewports = 4096;     // a looping viewport chain stops here

constexpr int kSheetsWanted = 8;           // one per texture format

float AsFloat(uint32_t bits)
{
  float f;
  memcpy(&f, &bits, sizeof(f));
  return f;
}

// Column-major index of row r, column c.
constexpr int At(int r, int c)
{
  return c * 4 + r;
}

} // namespace

/******************************************************************************
 Textures
******************************************************************************/

void CLegacy3D::DecodeTexture(int format, int x, int y, int width, int height)
{
  x &= 2047;
  y &= 2047;
  if (x + width > 2048 || y + height > 2048 || width > 1024 || height > 1024)
    return;

  TexSheet &sheet = *m_sheetOfFormat[format];
  int tx = x / 32, ty = y / 32;
  if (sheet.format[ty][tx] == format && sheet.width[ty][tx] >= width && sheet.height[ty][tx] >= height)
    return;   // already there

  // Each format as RGBA in 0..1 from a 16-bit texel.
  GLfloat *out = m_decoded.data();
  auto put = [&out](GLfloat r, GLfloat g, GLfloat b, GLfloat a) { out[0] = r; out[1] = g; out[2] = b; out[3] = a; out += 4; };
  for (int row = y; row < y + height; row++)
  {
    const uint16_t *src = &m_texRAM[row * 2048 + x];
    for (int i = 0; i < width; i++)
    {
      unsigned t = src[i];
      unsigned lo = t & 0xFF, hi = t >> 8;
      switch (format)
      {
      case 0:   // T1RGB5, T set = see-through
        put(GLfloat((t >> 10) & 0x1F) * (1.0f / 31.0f), GLfloat((t >> 5) & 0x1F) * (1.0f / 31.0f),
            GLfloat(t & 0x1F) * (1.0f / 31.0f), (t & 0x8000) ? 0.0f : 1.0f);
        break;
      case 1:   // low byte, alpha in the high nibble
      case 3:   // high byte, the same
      {
        unsigned b = format == 1 ? lo : hi;
        GLfloat l = GLfloat(b & 0xF) * (1.0f / 15.0f);
        put(l, l, l, GLfloat(b >> 4) * (1.0f / 15.0f));
        break;
      }
      case 2:   // low byte, luminance in the high nibble
      case 4:   // high byte, the same
      {
        unsigned b = format == 2 ? lo : hi;
        GLfloat l = GLfloat(b >> 4) * (1.0f / 15.0f);
        put(l, l, l, GLfloat(b & 0xF) * (1.0f / 15.0f));
        break;
      }
      case 5:   // low byte grey, 0xFF see-through
      case 6:   // high byte grey
      {
        unsigned b = format == 5 ? lo : hi;
        GLfloat l = GLfloat(b) * (1.0f / 255.0f);
        put(l, l, l, b == 0xFF ? 0.0f : 1.0f);
        break;
      }
      default:  // 7: RGBA4
        put(GLfloat((t >> 12) & 0xF) * (1.0f / 15.0f), GLfloat((t >> 8) & 0xF) * (1.0f / 15.0f),
            GLfloat((t >> 4) & 0xF) * (1.0f / 15.0f), GLfloat(t & 0xF) * (1.0f / 15.0f));
        break;
      }
    }
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glActiveTexture(GL_TEXTURE0 + sheet.map);
  glBindTexture(GL_TEXTURE_2D, m_maps[sheet.map]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, sheet.originX + x, sheet.originY + y, width, height, GL_RGBA, GL_FLOAT, m_decoded.data());

  sheet.format[ty][tx] = format;
  sheet.width[ty][tx] = width;
  sheet.height[ty][tx] = height;
}

void CLegacy3D::UploadTextures(unsigned, unsigned x, unsigned y, unsigned width, unsigned height)
{
  // Forget what was decoded at every tile corner in the rectangle.
  for (TexSheet &sheet : m_sheets)
    for (unsigned tx = x / 32; tx < (x + width) / 32; tx++)
      for (unsigned ty = y / 32; ty < (y + height) / 32; ty++)
      {
        sheet.format[ty][tx] = -1;
        sheet.width[ty][tx] = -1;
        sheet.height[ty][tx] = -1;
      }
}

/******************************************************************************
 Memory
******************************************************************************/

const uint32_t *CLegacy3D::CullingWord(uint32_t addr) const
{
  addr &= 0xFFFFFF;
  if (addr >= 0x800000 && addr < 0x840000)
    return &m_cullHi[addr & 0x3FFFF];
  if (addr < 0x100000)
    return &m_cullLo[addr];
  return nullptr;
}

const uint32_t *CLegacy3D::ModelWords(uint32_t addr) const
{
  addr &= 0xFFFFFF;
  return addr < 0x100000 ? &m_polyRAM[addr] : &m_vrom[addr];
}

/******************************************************************************
 Matrices
******************************************************************************/

// Real3D matrices are 12 floats: translation, then the 3x3 rows.
void CLegacy3D::ApplyMatrix(uint32_t index)
{
  if (!m_matrices)
    return;
  const float *src = &m_matrices[index * 12];
  GLfloat m[16];
  for (int r = 0; r < 3; r++)
  {
    for (int c = 0; c < 3; c++)
      m[At(r, c)] = src[3 + r * 3 + c];
    m[At(r, 3)] = src[r];
    m[At(3, r)] = 0.0f;
  }
  m[At(3, 3)] = 1.0f;
  glMultMatrixf(m);
}

// The Real3D views along +X with Y and Z the other way round from GL; the
// coordinate-system matrix (matrix 0) is applied after that change of axes.
void CLegacy3D::LoadCoordinateSystem(uint32_t matrixBase)
{
  GLfloat axes[16] = {};
  axes[At(0, 1)] = 1.0f;
  axes[At(1, 2)] = -1.0f;
  axes[At(2, 0)] = -1.0f;
  axes[At(3, 3)] = 1.0f;
  if (m_step > 0x10)
    glLoadMatrixf(axes);
  else
  {
    // Step 1.0 coordinates are very large; scale them down.
    GLfloat s = 1.0f / 2048.0f;
    glLoadIdentity();
    glScalef(s, s, s);
    glMultMatrixf(axes);
  }
  m_matrices = reinterpret_cast<const float *>(CullingWord(matrixBase));
  ApplyMatrix(0);
}

/******************************************************************************
 Scene walk
******************************************************************************/

void CLegacy3D::VisitPointer(uint32_t ptr)
{
  uint32_t addr = ptr & 0xFFFFFF;
  if (addr == 0)
    return;
  switch (ptr >> 24)
  {
  case 0x00:
    VisitNode(addr);
    break;
  case 0x01:
  case 0x03:
    DrawModel(addr);
    break;
  case 0x04:
    VisitList(addr);
    break;
  default:
    break;
  }
}

void CLegacy3D::VisitList(uint32_t addr)
{
  if (m_listDepth > kMaxListDepth)
    return;
  const uint32_t *list = CullingWord(addr);
  if (!list)
    return;

  // The last entry has bit 25 set; a zero or flagged word before that
  // ends the list early (the entry before it is the last).
  int last = 0;
  for (;; last++)
  {
    uint32_t w = list[last];
    if (w & 0x02000000)
      break;
    if (w == 0 || (w >> 24) != 0)
    {
      last--;
      break;
    }
  }

  m_listDepth++;
  for (int i = last; i >= 0; i--)
  {
    uint32_t w = list[i];
    uint32_t node = w & 0xFFFFFF;
    if ((w & 0x01000000) == 0 && node != 0 && node != 0x800800)
      VisitNode(node);
  }
  m_listDepth--;
}

void CLegacy3D::VisitNode(uint32_t addr)
{
  if (++m_nodeDepth >= kMaxNodeDepth)
  {
    m_nodeDepth--;
    return;
  }
  const uint32_t *node = CullingWord(addr);
  if (!node)
  {
    m_nodeDepth--;
    return;
  }
  // Words 3 and up sit 2 words earlier on Step 1.0.
  const int shift = m_nodeShift;
  auto w = [node, shift](int i) { return node[i - shift]; };

  if (node[0] & 0x04)
    m_colourTable = ((w(3) >> 19) | ((w(7) >> 28) << 13) | ((w(8) >> 25) << 17)) & 0xFFFFF;

  TexOffset saved = m_texOffset;
  if (m_nodeShift == 0 && (node[2] & 0x8000))
  {
    uint32_t t = node[2];
    m_texOffset.x = 32 * int((t >> 7) & 0x7F);
    m_texOffset.y = 32 * int(t & 0x7F);
    m_texOffset.bankFlip = int((t & 0x4000) >> 4);
    m_texOffset.state = uint16_t(t & 0x7FFF);
  }

  glPushMatrix();
  if (node[0] & 0x10)
    glTranslatef(AsFloat(w(4)), AsFloat(w(5)), AsFloat(w(6)));
  else if (w(3) & 0xFFF)
    ApplyMatrix(w(3) & 0xFFF);

  if (node[0] & 0x08)
  {
    // Level-of-detail table: only its first entry is drawn.
    const uint32_t *lod = CullingWord(w(7));
    if (lod)
    {
      if (w(3) & 0x20000000)
        VisitNode(lod[0] & 0xFFFFFF);
      else
        DrawModel(lod[0] & 0xFFFFFF);
    }
  }
  else
    VisitPointer(w(7));
  glPopMatrix();

  if ((node[0] & 0x07) != 0x06)   // 6: no sibling (breaks loops)
    VisitPointer(w(8));
  m_nodeDepth--;
  m_texOffset = saved;
}

void CLegacy3D::DrawViewport(const uint32_t *vp, bool wideScreen)
{
  static constexpr GLfloat kSpotColours[8][3] = {
    { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, 0 }, { 1, 1, 1 }
  };

  int x = int((vp[0x1A] & 0xFFFF) >> 4);   // 12.4
  int y = int((vp[0x1A] >> 20) & 0xFFF);
  int w = int((vp[0x14] & 0xFFFF) >> 2);   // 14.2
  int h = int((vp[0x14] >> 18) & 0x3FFF);
  GLfloat fovY = (asinf(AsFloat(vp[0x0E])) + asinf(AsFloat(vp[0x12]))) * float(180.0 / 3.14159265358979323846);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  m_vpY = GLint(m_offsetY) + GLint(float(384 - (y + h)) * m_scaleY);
  m_vpH = GLint(float(h) * m_scaleY);
  if (wideScreen && x == 0 && w >= 495 && y == 0 && h >= 383)
  {
    // Full-screen viewports widen to the whole window with the same
    // vertical field of view.
    m_vpX = 0;
    m_vpW = GLint(m_totalW);
    gluPerspective(fovY, GLfloat(m_vpW) / GLfloat(m_vpH), 0.1f, 1e5);
  }
  else
  {
    m_vpX = GLint(m_offsetX) + GLint(float(x) * m_scaleX);
    m_vpW = GLint(float(w) * m_scaleX);
    gluPerspective(fovY, GLdouble(w) / GLdouble(h), 0.1, 1e5);
  }

  // Sun (towards the light) and ambient
  m_lighting[0] = AsFloat(vp[0x05]);
  m_lighting[1] = AsFloat(vp[0x06]);
  m_lighting[2] = AsFloat(vp[0x04]);
  m_lighting[3] = AsFloat(vp[0x07]);
  m_lighting[4] = float((vp[0x24] >> 8) & 0xFF) * float(1.0 / 255.0);
  m_lighting[5] = 0.0f;

  // Spotlight: an ellipse on the 496x384 screen, lit between two depths.
  // Its size words hold (screen size / half size)^2.
  float cx = float((vp[0x1E] >> 3) & 0x1FFF);
  float cy = 384.0f - float((vp[0x1D] >> 3) & 0x1FFF);
  float sx = float(496.0 / std::sqrt(double(float((vp[0x1E] >> 16) & 0xFFFF))));
  float sy = float(384.0 / std::sqrt(double(float((vp[0x1D] >> 16) & 0xFFFF))));
  m_spotEllipse[0] = cx * m_scaleX + float(m_offsetX);
  m_spotEllipse[1] = cy * m_scaleY + float(m_offsetY);
  m_spotEllipse[2] = sx * m_scaleX;
  m_spotEllipse[3] = sy * m_scaleY;
  m_spotRange[0] = 1.0f / AsFloat(vp[0x21]);
  m_spotRange[1] = AsFloat(vp[0x1F]) + m_spotRange[0];
  const GLfloat *spot = kSpotColours[(vp[0x20] >> 11) & 7];
  m_spotColour[0] = spot[0];
  m_spotColour[1] = spot[1];
  m_spotColour[2] = spot[2];

  // Fog colour, density, start
  m_fog[0] = float((vp[0x22] >> 16) & 0xFF) * float(1.0 / 255.0);
  m_fog[1] = float((vp[0x22] >> 8) & 0xFF) * float(1.0 / 255.0);
  m_fog[2] = float(vp[0x22] & 0xFF) * float(1.0 / 255.0);
  m_fog[3] = AsFloat(vp[0x23]);
  m_fog[4] = float(int16_t(vp[0x25] & 0xFFFF)) * float(1.0 / 255.0);
  if (!std::isfinite(m_fog[3]) || !std::isfinite(m_fog[4]))
    m_fog[3] = m_fog[4] = 0.0f;

  m_texOffset = TexOffset();
  glMatrixMode(GL_MODELVIEW);
  LoadCoordinateSystem(vp[0x16] & 0xFFFFFF);

  // A coordinate-system matrix that is not a plain change of axes means the
  // game has not set the scene up yet (drawing it can take very long).
  if (m_matrices)
  {
    for (int i : { 6, 10, 5 })
    {
      float sq = m_matrices[i] * m_matrices[i];
      if (sq > 1.05f || sq < 0.95f)
        return;
    }
  }

  AddViewportItem(m_romCache);
  AddViewportItem(m_ramCache);
  m_nodeDepth = 0;
  m_listDepth = 0;
  VisitPointer(vp[0x02]);
}

// Viewports are linked through word 1 (0x01000000 ends the chain); a
// chain is drawn from its far end back to the first viewport.
void CLegacy3D::DrawViewportChain(int priority, bool wideScreen)
{
  std::vector<const uint32_t *> chain;
  uint32_t addr = 0x800000;
  while (chain.size() < kMaxViewports)
  {
    const uint32_t *vp = CullingWord(addr);
    if (!vp || vp[0x01] == 0)   // 0: not set up yet
      break;
    chain.push_back(vp);
    if (vp[0x01] == 0x01000000)
      break;
    addr = vp[0x01];
  }
  for (size_t i = chain.size(); i-- > 0;)
    if (int((chain[i][0x00] >> 3) & 3) == priority)
      DrawViewport(chain[i], wideScreen);
}

/******************************************************************************
 Frame
******************************************************************************/

void CLegacy3D::RenderFrame(void)
{
  bool wideScreen = m_config["WideScreen"].ValueAs<bool>();
  ResetReports();

  if (m_aaTarget)
    glBindFramebuffer(GL_FRAMEBUFFER, m_aaTarget);

  if (m_blockCulling && !m_config["NoWhiteFlash"].ValueAs<bool>())
  {
    // No 3D this frame: the screen flashes white.
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_aaTarget)
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }

  glDepthFunc(GL_LESS);
  glEnable(GL_DEPTH_TEST);
  // Stencilled models draw only where nothing stencilled is yet.
  glStencilFunc(GL_EQUAL, 0, 0xFF);
  glStencilOp(GL_KEEP, GL_INCR, GL_INCR);
  glStencilMask(0xFF);
  glDisable(GL_STENCIL_TEST);

  glUseProgram(m_program);
  for (unsigned i = 0; i < m_mapCount; i++)
  {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, m_maps[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);   // the shader filters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  }

  const GLint attributes[] = { m_aSubTexture, m_aTexParams, m_aTexFormat, m_aTexMap, m_aTransLevel,
                               m_aLightEnable, m_aSpecular, m_aShininess, m_aFogIntensity };
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_NORMAL_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  for (GLint a : attributes)
    if (a != -1)
      glEnableVertexAttribArray(a);

  EmptyCache(m_ramCache);
  for (int priority = 0; priority < 4; priority++)
  {
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    ClearItems(m_ramCache);
    ClearItems(m_romCache);
    DrawViewportChain(priority, wideScreen);
    DrawAllLists();
  }
  glFrontFace(GL_CW);
  glDisable(GL_STENCIL_TEST);

  for (GLint a : attributes)
    if (a != -1)
      glDisableVertexAttribArray(a);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  if (m_aaTarget)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CLegacy3D::BeginFrame(void)
{
}

void CLegacy3D::EndFrame(void)
{
}

/******************************************************************************
 Setup
******************************************************************************/

void CLegacy3D::AttachMemory(const uint32_t *cullingRAMLoPtr, const uint32_t *cullingRAMHiPtr, const uint32_t *polyRAMPtr,
                             const uint32_t *vromPtr, const uint16_t *textureRAMPtr)
{
  m_cullLo = cullingRAMLoPtr;
  m_cullHi = cullingRAMHiPtr;
  m_polyRAM = polyRAMPtr;
  m_vrom = vromPtr;
  m_texRAM = textureRAMPtr;
}

void CLegacy3D::SetStepping(int stepping)
{
  m_step = (stepping == 0x15 || stepping == 0x20 || stepping == 0x21) ? stepping : 0x10;
  if (m_step > 0x10)
  {
    m_nodeShift = 0;                  // 10-word culling nodes
    m_vertexScale = 1.0f / 2048.0f;   // 13.11 vertices
  }
  else
  {
    m_nodeShift = 2;                  // 8-word culling nodes
    m_vertexScale = 1.0f / 128.0f;    // 17.7 vertices
  }
  DebugLog("Legacy3D: Step %d.%d\n", (m_step >> 4) & 0xF, m_step & 0xF);
}

Result CLegacy3D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget)
{
  m_scaleX = GLfloat(xRes) / 496.0f;
  m_scaleY = GLfloat(yRes) / 384.0f;
  m_offsetX = xOffset;
  m_offsetY = yOffset;
  m_totalW = totalXRes;
  m_totalH = totalYRes;
  m_aaTarget = aaTarget;
  return Result::OKAY;
}

void CLegacy3D::SetSunClamp(bool)
{
}

void CLegacy3D::SetBlockCulling(bool enable)
{
  m_blockCulling = enable;
}

float CLegacy3D::GetLosValue(int)
{
  return 0.0f;
}

Result CLegacy3D::CreateGLObjects()
{
  m_decoded.assign(size_t(1024) * 1024 * 4, 0.0f);
  glGetError();

  if (CreateCache(m_romCache, kRomVboVerts, kBuildVerts, kRomModels, kDrawItems, false) != Result::OKAY)
    return Result::FAIL;
  if (CreateCache(m_ramCache, kRamVboVerts, kBuildVerts, kRamModels, kDrawItems, true) != Result::OKAY)
    return Result::FAIL;

  m_lighting[4] = 1.0f;   // full ambient until a viewport says otherwise

  // How many texture maps of which size: each map is a square of
  // extent x extent sheets. Start from the largest the settings and the
  // driver allow and shrink while a smaller map still holds every sheet.
  GLint units = 0, maxSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
  int mapsAllowed = std::max(1, std::min(m_config["MaxTexMaps"].ValueAsDefault<int>(9), int(units)));
  int extent = std::max(1, int(std::min(unsigned(m_config["MaxTexMapExtent"].ValueAsDefault<int>(4)), unsigned(maxSize / 2048))));
  for (; extent > 1; extent--)
  {
    if ((extent - 1) * (extent - 1) >= kSheetsWanted)
      continue;   // one size down still holds all sheets
    GLint size = 2048 * extent, got = 0;
    glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, nullptr);
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &got);
    if (got == size)
      break;
  }

  const char *fragment = m_config["MultiTexture"].ValueAs<bool>() ? fragmentShaderMultiSheetSource : fragmentShaderSingleSheetSource;
  if (LoadShaderProgram(&m_program, &m_vertexShader, &m_fragmentShader, m_config["VertexShader"].ValueAs<std::string>(),
                        m_config["FragmentShader"].ValueAs<std::string>(), vertexShaderSource, fragment) != Result::OKAY)
    return Result::FAIL;

  // Samplers: "textureMap", then "textureMap0".."textureMap7", get texture
  // units in that order.
  glUseProgram(m_program);
  int samplers = 0;
  GLint loc = glGetUniformLocation(m_program, "textureMap");
  if (loc != -1)
    glUniform1i(loc, samplers++);
  for (int i = 0; i < 8 && samplers < mapsAllowed; i++)
  {
    char name[16];
    snprintf(name, sizeof(name), "textureMap%d", i);
    loc = glGetUniformLocation(m_program, name);
    if (loc != -1)
      glUniform1i(loc, samplers++);
  }
  if (samplers == 0)
    return ErrorLog("Legacy3D: the fragment shader has no 'textureMap' sampler.");
  InfoLog("Legacy3D: %d texture map sampler(s) in the fragment shader.", samplers);

  // With several samplers, smaller maps can spread the sheets out.
  while (extent > 1 && samplers * (extent - 1) * (extent - 1) >= kSheetsWanted)
    extent--;

  unsigned perMap = 0;
  for (;;)
  {
    perMap = unsigned(extent * extent);
    m_mapCount = std::min(unsigned(samplers), 1 + (kSheetsWanted - 1) / perMap);
    GLsizei size = 2048 * extent;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(m_mapCount, m_maps);
    bool allMade = true;
    for (unsigned i = 0; i < m_mapCount; i++)
    {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, m_maps[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, nullptr);
      if (glGetError() != GL_NO_ERROR)
      {
        m_mapCount = i;   // out of texture memory
        allMade = false;
        break;
      }
    }
    if (allMade || extent == 1)
      break;
    glDeleteTextures(m_mapCount, m_maps);
    extent--;
  }
  if (m_mapCount == 0)
    return ErrorLog("Legacy3D: OpenGL could not make a single 2048x2048 texture map.");
  int mapSize = 2048 * extent;
  InfoLog("Legacy3D: %u texture map(s) of %dx%d texels.", m_mapCount, mapSize, mapSize);

  // Sheets fill each map row by row; formats share sheets if there are
  // fewer than eight.
  m_sheets.resize(std::min<unsigned>(m_mapCount * perMap, kSheetsWanted));
  for (unsigned i = 0; i < m_sheets.size(); i++)
  {
    unsigned cell = i % perMap;
    m_sheets[i].map = i / perMap;
    m_sheets[i].originX = 2048 * (cell % unsigned(extent));
    m_sheets[i].originY = 2048 * (cell / unsigned(extent));
  }
  for (unsigned fmt = 0; fmt < 8; fmt++)
    m_sheetOfFormat[fmt] = &m_sheets[fmt % m_sheets.size()];

  m_uModelView = glGetUniformLocation(m_program, "modelViewMatrix");
  m_uProjection = glGetUniformLocation(m_program, "projectionMatrix");
  m_uLighting = glGetUniformLocation(m_program, "lighting");
  m_uMapSize = glGetUniformLocation(m_program, "mapSize");
  m_uSpotEllipse = glGetUniformLocation(m_program, "spotEllipse");
  m_uSpotRange = glGetUniformLocation(m_program, "spotRange");
  m_uSpotColour = glGetUniformLocation(m_program, "spotColor");
  m_aSubTexture = glGetAttribLocation(m_program, "subTexture");
  m_aTexParams = glGetAttribLocation(m_program, "texParams");
  m_aTexFormat = glGetAttribLocation(m_program, "texFormat");
  m_aTexMap = glGetAttribLocation(m_program, "texMap");
  m_aTransLevel = glGetAttribLocation(m_program, "transLevel");
  m_aLightEnable = glGetAttribLocation(m_program, "lightEnable");
  m_aSpecular = glGetAttribLocation(m_program, "specular");
  m_aShininess = glGetAttribLocation(m_program, "shininess");
  m_aFogIntensity = glGetAttribLocation(m_program, "fogIntensity");
  if (m_uMapSize != -1)
    glUniform1f(m_uMapSize, GLfloat(mapSize));

  glFrontFace(GL_CW);   // triangles are stored clockwise
  glCullFace(GL_BACK);
  glEnable(GL_CULL_FACE);
  glClearDepth(1.0);
  glEnable(GL_TEXTURE_2D);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  UploadTextures(0, 0, 0, 2048, 2048);   // nothing decoded yet
  return Result::OKAY;
}

CLegacy3D::CLegacy3D(const Util::Config::Node &config)
  : m_config(config)
{
  CreateGLObjects();
}

CLegacy3D::~CLegacy3D()
{
  DestroyShaderProgram(m_program, m_vertexShader, m_fragmentShader);
  if (glBindBuffer)
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteTextures(m_mapCount, m_maps);
  DestroyCache(m_romCache);
  DestroyCache(m_ramCache);
}

} // Legacy3D
