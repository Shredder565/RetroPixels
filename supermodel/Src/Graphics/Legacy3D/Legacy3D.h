// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Legacy3D.h
//
// The older Real3D renderer: an OpenGL compatibility-profile engine that
// walks the scene database with the GL matrix stack, turns each model into
// triangles once (cached in vertex buffers per texture-offset state) and
// draws the frame's model instances from two lists per cache, opaque first
// and translucent second. Textures are decoded from texture RAM on demand
// into one or more large GL texture maps, one 2048x2048 sheet per format.

#ifndef INCLUDED_LEGACY3D_H
#define INCLUDED_LEGACY3D_H

#include <GL/glew.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Graphics/IRender3D.h"
#include "TextureRefs.h"
#include "Util/NewConfig.h"
#include "Types.h"

namespace Legacy3D {

class CLegacy3D final : public IRender3D
{
public:
  explicit CLegacy3D(const Util::Config::Node &config);
  ~CLegacy3D() override;

  void RenderFrame(void) override;
  void BeginFrame(void) override;
  void EndFrame(void) override;
  void UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height) override;
  void AttachMemory(const uint32_t *cullingRAMLoPtr, const uint32_t *cullingRAMHiPtr, const uint32_t *polyRAMPtr,
                    const uint32_t *vromPtr, const uint16_t *textureRAMPtr) override;
  void SetStepping(int stepping) override;
  Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget) override;
  void SetSunClamp(bool enable) override;
  void SetBlockCulling(bool enable) override;
  float GetLosValue(int layer) override;

  // Brings one texture rectangle of texture RAM into its sheet, unless the
  // sheet already holds it in that format (used by CTextureRefs too).
  void DecodeTexture(int format, int x, int y, int width, int height);

private:
  enum Pass { kOpaque = 0, kTranslucent = 1 };

  // One triangle corner as stored in a vertex buffer.
  struct GpuVertex
  {
    GLfloat pos[3];
    GLfloat normal[3];
    GLfloat colour[3];        // polygon colour (material colour when textured)
    GLfloat uv[2];            // texel coordinates inside the texture
    GLfloat subTexture[4];    // texture's x, y in its map, width, height
    GLfloat texParams[4];     // textured, contour test (>0), mirror U, mirror V
    GLfloat texFormat;
    GLfloat texMap;
    GLfloat transLevel;       // 1 opaque .. 0 invisible
    GLfloat lightEnable;
    GLfloat specular;
    GLfloat shininess;        // < 0: no specular
    GLfloat fogIntensity;
  };

  // A decoded polygon before it becomes triangles.
  struct Corner
  {
    GLfloat x, y, z;
    GLfloat n[3];
    GLfloat u, v;
    GLfloat shade;            // fixed shading level
  };
  struct Polygon
  {
    const uint32_t *header;   // the 7 header words
    Corner corner[4];
    GLfloat normal[3];
    unsigned corners;
    Pass pass;
  };

  // A model turned into triangles for one texture-offset state.
  struct CachedModel
  {
    unsigned first[2];        // first vertex in the buffer, per pass
    unsigned count[2];
    uint32_t address;
    uint16_t offsetState;
    bool stencil;             // drawn with the stencil test (layered/shadow)
    int older;                // same address, another offset state (-1: none)
    CTextureRefs textures;    // static caches decode these on every draw
  };

  // What one item of a draw list records.
  struct DrawItem
  {
    bool isViewport;
    GLfloat matrix[16];       // projection (viewport) or modelview (model)
    // viewport
    GLfloat lighting[6];
    GLfloat spotEllipse[4];
    GLfloat spotRange[2];
    GLfloat spotColour[3];
    GLfloat fog[5];
    GLint vpX, vpY, vpW, vpH;
    // model
    unsigned first, count;
    GLint frontFace;          // GL_CW, GL_CCW or kNoCulling
    bool stencil;
    int next;                 // next item of the same pass (-1: end)
  };

  struct ModelCache
  {
    bool dynamic = false;     // polygon RAM: rebuilt every frame
    GLuint vbo = 0;
    size_t vboBytes = 0;
    size_t vboUsed = 0;       // bytes taken (also by a model left unfinished)
    std::vector<GpuVertex> building[2];
    size_t buildingMax = 0;
    std::vector<CachedModel> models;
    size_t modelsMax = 0;
    unsigned modelCount = 0;
    std::unordered_map<uint32_t, int> newest;   // address -> latest model
    std::vector<DrawItem> items;   // reserved to itemsMax, never more
    size_t itemsMax = 0;
    int head[2] = { -1, -1 }, tail[2] = { -1, -1 };
  };

  struct TexSheet
  {
    unsigned map;             // GL texture map holding the sheet
    unsigned originX, originY;
    // What was last decoded at each 32x32 tile corner (-1: nothing valid)
    int format[64][64];
    int width[64][64];
    int height[64][64];
  };

  struct TexOffset
  {
    int x = 0, y = 0;         // added to the polygon's texture position
    int bankFlip = 0;         // 1024 swaps the two 1024-line banks
    uint16_t state = 0;       // the node word that set them (cache key)
  };

  static constexpr GLint kNoCulling = -GL_CW;

  // Setup
  Result CreateGLObjects();
  Result CreateCache(ModelCache &cache, size_t vboVerts, size_t localVerts, size_t maxModels, size_t maxItems, bool dynamic);
  void DestroyCache(ModelCache &cache);

  // Memory
  const uint32_t *CullingWord(uint32_t addr) const;
  const uint32_t *ModelWords(uint32_t addr) const;

  // Scene walk
  void DrawViewportChain(int priority, bool wideScreen);
  void DrawViewport(const uint32_t *vp, bool wideScreen);
  void LoadCoordinateSystem(uint32_t matrixBase);
  void ApplyMatrix(uint32_t index);
  void VisitPointer(uint32_t ptr);
  void VisitNode(uint32_t addr);
  void VisitList(uint32_t addr);
  Result DrawModel(uint32_t addr);
  static bool UsesColourTable(const uint32_t *words);

  // Model caching
  CachedModel *FindModel(ModelCache &cache, uint32_t addr, uint16_t offsetState);
  CachedModel *BuildModel(ModelCache &cache, uint32_t addr, uint16_t offsetState, const uint32_t *words);
  Result AddPolygon(ModelCache &cache, const Polygon &poly);
  void AddCorner(ModelCache &cache, const Corner &c, const Polygon &poly, float normalSign);
  void FinishModel(ModelCache &cache, CachedModel &model, uint32_t addr, uint16_t offsetState, bool stencil);
  GLint FrontFaceFor(const GLfloat modelView[16]) const;
  void EmptyCache(ModelCache &cache);
  int TexturePosX(const uint32_t *header) const;
  int TexturePosY(const uint32_t *header) const;

  // Draw lists
  Result AddViewportItem(ModelCache &cache);
  Result AddModelItems(ModelCache &cache, const CachedModel &model);
  void Link(ModelCache &cache, int pass, int item);
  void ClearItems(ModelCache &cache);
  void DrawItems(ModelCache &cache, Pass pass);
  void DrawAllLists();

  // Errors (each reported at most once per frame)
  Result ReportLocalOverflow();
  Result ReportUncachable(uint32_t addr);
  void ResetReports();

  const Util::Config::Node &m_config;

  // Stepping
  int m_step = 0x10;
  int m_nodeShift = 2;        // culling node words missing before word 3 on Step 1.0
  GLfloat m_vertexScale = 1.0f / 128.0f;

  // Real3D memory
  const uint32_t *m_cullLo = nullptr;
  const uint32_t *m_cullHi = nullptr;
  const uint32_t *m_polyRAM = nullptr;
  const uint32_t *m_vrom = nullptr;
  const uint16_t *m_texRAM = nullptr;

  // Screen placement
  GLfloat m_scaleX = 1.0f, m_scaleY = 1.0f;
  unsigned m_offsetX = 0, m_offsetY = 0;
  unsigned m_totalW = 0, m_totalH = 0;
  GLuint m_aaTarget = 0;

  // Current viewport while walking
  const float *m_matrices = nullptr;
  GLfloat m_lighting[6] = {};
  GLfloat m_spotEllipse[4] = {};
  GLfloat m_spotRange[2] = {};
  GLfloat m_spotColour[3] = {};
  GLfloat m_fog[5] = {};
  GLint m_vpX = 0, m_vpY = 0, m_vpW = 0, m_vpH = 0;
  int m_nodeDepth = 0;
  int m_listDepth = 0;
  TexOffset m_texOffset;
  uint32_t m_colourTable = 0x400;

  // Textures
  std::vector<GLfloat> m_decoded;   // RGBA, one rectangle at a time
  unsigned m_mapCount = 0;
  GLuint m_maps[9] = {};
  std::vector<TexSheet> m_sheets;
  TexSheet *m_sheetOfFormat[8] = {};

  // Shader program and its inputs
  GLuint m_program = 0, m_vertexShader = 0, m_fragmentShader = 0;
  GLint m_uModelView = -1, m_uProjection = -1, m_uLighting = -1, m_uMapSize = -1;
  GLint m_uSpotEllipse = -1, m_uSpotRange = -1, m_uSpotColour = -1;
  GLint m_aSubTexture = -1, m_aTexParams = -1, m_aTexFormat = -1, m_aTexMap = -1, m_aTransLevel = -1;
  GLint m_aLightEnable = -1, m_aSpecular = -1, m_aShininess = -1, m_aFogIntensity = -1;

  ModelCache m_romCache;    // VROM models
  ModelCache m_ramCache;    // polygon RAM models (and VROM models using its colour table)

  unsigned m_reported = 0;
  bool m_blockCulling = false;
};

} // Legacy3D

#endif  // INCLUDED_LEGACY3D_H
