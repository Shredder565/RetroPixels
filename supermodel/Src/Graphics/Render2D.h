// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Render2D.h
//
// Shows the tile generator's output. TileGen draws its layers in software
// into two 496x384 images (the layers under the 3D scene and those over
// it); this class uploads them as textures and draws them over the Model 3
// screen area, the bottom one before the 3D scene and the top one after.
// All members other than the constructor need the GL context current.

#ifndef INCLUDED_RENDER2D_H
#define INCLUDED_RENDER2D_H

#include <GL/glew.h>
#include <memory>
#include "Supermodel.h"
#include "Util/NewConfig.h"
#include "New3D/GLSLShader.h"
#include "FBO.h"
#include "../Model3/TileGenBuffer.h"

class CRender2D
{
public:
  explicit CRender2D(const Util::Config::Node &config);
  ~CRender2D(void);

  // xRes x yRes: the Model 3 screen area at (xOffset, yOffset) inside the
  // totalXRes x totalYRes window. aaTarget: frame buffer to draw into
  // (0: the window). Bilinear-type upscaling is skipped at native size.
  Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget, UpscaleMode upscaleMode);

  // The images to show (bottom: under the 3D scene, top: over it).
  void AttachDrawBuffers(std::shared_ptr<TileGenBuffer> bottom, std::shared_ptr<TileGenBuffer> top);

  void BeginFrame(void);
  void PreRenderFrame(void);      // uploads both images
  void RenderFrameBottom(void);   // clears the window, then draws the bottom image
  void RenderFrameTop(void);      // blends the top image over what is there
  void EndFrame(void);

  // Kept for callers; the images carry everything now.
  void WriteVRAM(unsigned addr, uint32_t data);
  void AttachRegisters(const uint32_t *regPtr);
  void AttachPalette(const uint32_t *palPtr[2]);
  void AttachVRAM(const uint8_t *vramPtr);

private:
  void PrepareLayer(bool bottom);
  void DrawLayer(GLuint texture);
  void UseTarget(bool on) const;

  const Util::Config::Node &m_config;

  unsigned m_xOffset = 0;
  unsigned m_yOffset = 0;
  unsigned m_xRes = 496;
  unsigned m_yRes = 384;
  unsigned m_totalXRes = 0;
  unsigned m_totalYRes = 0;
  unsigned m_shift = 0;       // lines the layers are moved to line up with the 3D scene
  GLuint m_aaTarget = 0;
  UpscaleMode m_upscaleMode = UpscaleMode::Biquintic;

  GLuint m_vao = 0;
  GLuint m_textures[2] = { 0, 0 };
  GLSLShader m_shader;
  std::shared_ptr<TileGenBuffer> m_images[2];
};

#endif  // INCLUDED_RENDER2D_H
