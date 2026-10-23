// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Draws the light gun crosshairs over the game picture: four small red
// (player 1) or green (player 2) triangles pointing at the aim, or, with
// CrosshairStyle=bmp, the p1crosshair.bmp / p2crosshair.bmp images from the
// Assets folder.

#ifndef INCLUDED_CROSSHAIR_H
#define INCLUDED_CROSSHAIR_H

#include "Supermodel.h"
#include "Graphics/New3D/New3D.h"
#include "Inputs/Inputs.h"
#include <vector>

class CCrosshair
{
public:
  CCrosshair(const Util::Config::Node &config);
  ~CCrosshair();

  // Loads both images (fails if either is missing) and sets up GL.
  Result Init();

  // Draws the crosshairs the Crosshairs setting asks for into the given
  // viewport.
  void Update(uint32_t currentInputs, CInputs *Inputs, unsigned int xOffset, unsigned int yOffset, unsigned int xRes, unsigned int yRes);

private:
  struct Point { float x, y, z; };

  void Draw(New3D::Mat4 m, float x, float y, int player, float aspect);

  const Util::Config::Node &m_config;
  bool m_bitmap = false;
  GLuint m_textures[2] = { 0, 0 };
  float m_dpiScale = 0.0f;
  std::vector<Point> m_shape;

  GLSLShader m_shader;
  int m_locMvp = -1, m_locColour = -1, m_locTexture = -1, m_locIsBitmap = -1;
  VBO m_positions;
  VBO m_uvs;
  GLuint m_vao = 0;
};

#endif // INCLUDED_CROSSHAIR_H
