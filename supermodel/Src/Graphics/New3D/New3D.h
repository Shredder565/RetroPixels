// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// New3D.h
//
// The default Real3D renderer. Each frame it walks the scene database in
// culling RAM (viewports, culling nodes, LOD tables, model pointers),
// decodes the models it reaches from polygon RAM or VROM into vertex buffer
// data (VROM models are decoded once and kept), and draws every viewport
// priority with OpenGL 4.1 shaders that model the Real3D's texturing,
// lighting, fog, translucency layers and line-of-sight readback.

#ifndef INCLUDED_NEW3D_H
#define INCLUDED_NEW3D_H

#include <GL/glew.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Types.h"
#include "Graphics/IRender3D.h"
#include "Util/NewConfig.h"
#include "GLSLShader.h"
#include "Mat4.h"
#include "Model.h"
#include "PolyHeader.h"
#include "R3DData.h"
#include "R3DFrameBuffers.h"
#include "R3DScrollFog.h"
#include "R3DShader.h"
#include "TextureBank.h"
#include "VBO.h"
#include "Vec.h"

namespace New3D {

class CNew3D final : public IRender3D
{
public:
  CNew3D(const Util::Config::Node &config, const std::string &gameName);
  ~CNew3D(void);

  // IRender3D
  void RenderFrame(void) override;
  void BeginFrame(void) override;
  void EndFrame(void) override;
  void UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height) override;
  void AttachMemory(const UINT32 *cullingRAMLoPtr, const UINT32 *cullingRAMHiPtr, const UINT32 *polyRAMPtr, const UINT32 *vromPtr, const UINT16 *textureRAMPtr) override;
  void SetStepping(int stepping) override;
  Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget) override;
  void SetSunClamp(bool enable) override;
  void SetBlockCulling(bool enable) override;
  float GetLosValue(int layer) override;

private:
  // Memory
  const uint32_t *CullingWords(uint32_t addr) const;
  const uint32_t *ModelWords(uint32_t addr) const;

  // Building the frame
  void WalkViewports(uint32_t addr);
  void ReadViewport(const uint32_t *vp, ViewportState &out);
  void PlaceViewport(ViewportState &vp);
  void VisitNode(uint32_t addr, int depth);
  void DrawCullingNode(const uint32_t *node, int depth);
  void VisitPointer(uint32_t ptr, int depth);
  void VisitPointerList(uint32_t addr, int depth);
  void AddModel(uint32_t addr);
  void BuildModel(ModelInstance &model, const uint32_t *data);
  void SetGroupState(DrawGroup &group, const PolyHeader &ph) const;
  void EmitPoly(const Poly &p, std::vector<GPUVertex> &out) const;
  bool UsesColourTable(const uint32_t *data) const;

  // Matrices
  void StartMatrixStack(uint32_t matrixBase);
  void ApplyNodeMatrix(uint32_t index);
  void RemoveRotation();

  // Drawing
  bool LayerHasModels(int priority) const;
  bool DrawPriority(int priority, bool highPriorityPass, Layer layer);
  void BeginPolygonPass();
  void EndPolygonPass();
  void DrawBackgroundFog();
  void DrawScrollFog();
  void ReadLineOfSight(int priority);

  // Configuration
  int m_vertsPerPoly = 3;
  GLenum m_primitive = GL_TRIANGLES;
  bool m_wideScreen = false;
  bool m_noWhiteFlash = false;
  std::atomic_bool m_sunClamp;
  std::atomic_bool m_blockCulling;

  // Stepping dependent formats
  int m_step = 0x10;
  int m_nodeShift = 2;            // culling nodes lack words 1-2 before Step 1.5
  float m_vertexScale = 1.0f / 128.0f;
  float m_textureNPScale = 1.0f / 4096.0f;

  // Real3D memory
  const uint32_t *m_cullLo = nullptr;
  const uint32_t *m_cullHi = nullptr;
  const uint32_t *m_polyRAM = nullptr;
  const uint32_t *m_vrom = nullptr;
  const uint16_t *m_texRAM = nullptr;

  // Output geometry
  float m_xRatio = 0.0f, m_yRatio = 0.0f;
  unsigned m_xOffs = 0, m_yOffs = 0;
  unsigned m_xRes = 0, m_yRes = 0;
  unsigned m_totalXRes = 0, m_totalYRes = 0;
  GLuint m_aaTarget = 0;

  // Traversal state
  const float *m_matrixBase = nullptr;
  const LODBlendTable *m_lodTable = nullptr;
  uint32_t m_colourTable = 0x400;     // polygon RAM word address of the colour table
  NodeStateStack m_state;
  Mat4 m_modelMat;
  std::vector<const uint32_t *> m_siblings;

  // Frustum test planes of the current viewport (side plane normals in
  // the Real3D's U/V/W space) and the wide-screen X correction.
  struct { float lu, lv, tu, tw, ru, rv, bu, bw, xScale; } m_planes;

  // Vertices carried from one polygon to the next. They persist across
  // models: some games start a model with vertices shared from the last.
  VertexBase m_lastVerts[4];
  uint16_t m_lastUV[4][2];

  // The frame
  std::vector<ViewportNode> m_frame;
  std::vector<GPUVertex> m_ramVerts;     // rebuilt every frame
  std::vector<GPUVertex> m_romVerts;     // grows as VROM models are first seen
  std::unordered_map<uint32_t, std::shared_ptr<std::vector<DrawGroup>>> m_romModels;

  // Line of sight results, double buffered between render and CPU threads.
  float m_los[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
  int m_losRead = 0;
  std::mutex m_losLock;

  // OpenGL
  TextureBank m_sheets[2];
  VBO m_vbo;                // ROM vertices from the start, RAM vertices after them
  GLuint m_vao = 0;
  R3DShader m_shader;
  R3DScrollFog m_fog;
  R3DFrameBuffers m_targets;
};

} // New3D

#endif  // INCLUDED_NEW3D_H
