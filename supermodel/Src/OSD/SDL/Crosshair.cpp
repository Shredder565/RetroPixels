// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Crosshair.h"
#include "Supermodel.h"
#include "Graphics/New3D/New3D.h"
#include "OSD/FileSystemPath.h"
#include "SDLIncludes.h"
#include <GL/glew.h>
#include <algorithm>
#include <vector>
#ifdef SUPERMODEL_GLES
#include <cstdint>
#include <cstring>
#endif
#include "Inputs/Inputs.h"
#include "Util/Format.h"

namespace
{
  const size_t kMaxPoints = 1024;

  // Each image is drawn as a unit square, then scaled to a tenth.
  const float kBitmapScale = 0.1f;

  // Screen DPI the sizes are meant for.
  const float kReferenceDpi = 96.0f;

  const float kSquareUV[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };

  const char kVertexShader[] = R"glsl(
#version 410 core
uniform mat4 mvp;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uvIn;
out vec2 uv;
void main(void)
{
  uv = uvIn;
  gl_Position = mvp * vec4(position, 1.0);
}
)glsl";

  const char kFragmentShader[] = R"glsl(
#version 410 core
uniform vec4 colour;
uniform sampler2D CrosshairTexture;
uniform bool isBitmap;
in vec2 uv;
out vec4 result;
void main(void)
{
  result = isBitmap ? colour * texture(CrosshairTexture, uv) : colour;
}
)glsl";

  // Light gun position (as the game reads it) to 0..1 across the screen.
  float GunX(float x) { return (x - 150.0f) / 501.0f; }
  float GunY(float y) { return (y - 80.0f) / 385.0f; }

  // Analog guns give 0..255, the vertical axis upwards.
  float AnalogX(float x) { return x / 255.0f; }
  float AnalogY(float y) { return (255.0f - y) / 255.0f; }

  void Upload(GLuint texture, SDL_Surface *image)
  {
    glBindTexture(GL_TEXTURE_2D, texture);
#ifdef SUPERMODEL_GLES
    // OpenGL ES takes no BGRA uploads: the same bytes (4 per pixel, as
    // GL_BGRA reads them), with blue and red swapped.
    std::vector<uint8_t> rgba(size_t(image->w) * size_t(image->h) * 4);
    std::memcpy(rgba.data(), image->pixels, std::min(rgba.size(), size_t(image->pitch) * size_t(image->h)));
    for (size_t i = 0; i < rgba.size(); i += 4)
      std::swap(rgba[i], rgba[i + 2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->w, image->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->w, image->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, image->pixels);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
}

CCrosshair::CCrosshair(const Util::Config::Node &config)
  : m_config(config)
{
}

CCrosshair::~CCrosshair()
{
}

Result CCrosshair::Init()
{
  std::string style = Util::ToLower(m_config["CrosshairStyle"].ValueAs<std::string>());
  m_bitmap = style == "bmp";
  if (style != "bmp" && style != "vector")
    ErrorLog("Invalid crosshair style '%s', must be 'vector' or 'bmp'. Reverting to 'vector'.\n", style.c_str());

  const std::string assets = FileSystemPath::GetPath(FileSystemPath::Assets);
  SDL_Surface *images[2] = {
    SDL_LoadBMP((assets + "p1crosshair.bmp").c_str()),
    SDL_LoadBMP((assets + "p2crosshair.bmp").c_str())
  };
  if (!images[0] || !images[1])
  {
    SDL_FreeSurface(images[0]);
    SDL_FreeSurface(images[1]);
    return Result::FAIL;
  }
  glGenTextures(2, m_textures);
  Upload(m_textures[0], images[0]);
  Upload(m_textures[1], images[1]);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_FreeSurface(images[0]);
  SDL_FreeSurface(images[1]);

  // Sized for a 96 DPI screen, scaled by the screen's horizontal DPI (0 if
  // SDL can't tell).
  float diagonal = 0.0f, horizontal = 0.0f, vertical = 0.0f;
  SDL_GetDisplayDPI(0, &diagonal, &horizontal, &vertical);
  m_dpiScale = horizontal / kReferenceDpi;

  m_shape.clear();
  if (m_bitmap)
  {
    // a unit square, as two triangles matching kSquareUV
    for (const auto &uv : kSquareUV)
      m_shape.push_back({ uv[0] - 0.5f, uv[1] - 0.5f, 0.0f });
  }
  else
  {
    // four triangles round the aim, tips 0.004 from it, 0.02 long, 0.01 wide
    const float tip = 0.004f, len = 0.02f, half = 0.005f;
    const float back = tip + len;
    m_shape = {
      { 0, tip, 0 },   { half, back, 0 },   { -half, back, 0 },     // below
      { 0, -tip, 0 },  { -half, -back, 0 }, { half, -back, 0 },     // above
      { -tip, 0, 0 },  { -back, half, 0 },  { -back, -half, 0 },    // left
      { tip, 0, 0 },   { back, -half, 0 },  { back, half, 0 },      // right
    };
  }

  m_shader.LoadShaders(kVertexShader, kFragmentShader);
  m_locMvp = m_shader.GetUniformLocation("mvp");
  m_locTexture = m_shader.GetUniformLocation("CrosshairTexture");
  m_locColour = m_shader.GetUniformLocation("colour");
  m_locIsBitmap = m_shader.GetUniformLocation("isBitmap");

  m_positions.Create(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW, sizeof(Point) * kMaxPoints);
  m_uvs.Create(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW, sizeof(kSquareUV));

  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);
  m_positions.Bind(true);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point), nullptr);
  m_positions.Bind(false);
  m_uvs.Bind(true);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(kSquareUV[0]), nullptr);
  m_uvs.Bind(false);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glBindVertexArray(0);
  return Result::OKAY;
}

void CCrosshair::Draw(New3D::Mat4 m, float x, float y, int player, float aspect)
{
  const GLsizei count = GLsizei(std::min(m_shape.size(), kMaxPoints));

  m_shader.EnableShader();
  m.Translate(x, y, 0);
  if (m_bitmap)
  {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[player]);
    const float s = m_dpiScale * kBitmapScale;
    m.Scale(s, s * aspect, 0);
    glUniformMatrix4fv(m_locMvp, 1, GL_FALSE, m);
    glUniform1i(m_locTexture, 0);
    glUniform4f(m_locColour, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1i(m_locIsBitmap, 1);
    m_positions.Bind(true);
    m_positions.BufferSubData(0, count * sizeof(Point), m_shape.data());
    m_positions.Bind(false);
    m_uvs.Bind(true);
    m_uvs.BufferSubData(0, sizeof(kSquareUV), kSquareUV);
    m_uvs.Bind(false);
  }
  else
  {
    m.Scale(m_dpiScale, m_dpiScale * aspect, 0);
    glUniformMatrix4fv(m_locMvp, 1, GL_FALSE, m);
    glUniform4f(m_locColour, player == 0 ? 1.0f : 0.0f, player == 1 ? 1.0f : 0.0f, 0.0f, 1.0f);
    glUniform1i(m_locIsBitmap, 0);
    m_positions.Bind(true);
    m_positions.BufferSubData(0, count * sizeof(Point), m_shape.data());
  }

  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, count);
  glBindVertexArray(0);
  m_shader.DisableShader();
}

void CCrosshair::Update(uint32_t currentInputs, CInputs *Inputs, unsigned int xOffset, unsigned int yOffset, unsigned int xRes, unsigned int yRes)
{
  // bit 0: player 1, bit 1: player 2 (can change while running)
  const unsigned wanted = m_config["Crosshairs"].ValueAs<unsigned>() & 3;
  if (!wanted)
    return;

  glUseProgram(0);
  glViewport(xOffset, yOffset, xRes, yRes);
  glDisable(GL_DEPTH_TEST);
  if (m_bitmap)
  {
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  else
    glDisable(GL_BLEND);

  New3D::Mat4 m;
  m.Ortho(0.0, 1.0, 1.0, 0.0, -1.0f, 1.0f);   // 0..1 across, y downwards

  const uint32_t analogGun[2] = { Game::INPUT_ANALOG_GUN1, Game::INPUT_ANALOG_GUN2 };
  const uint32_t gun[2] = { Game::INPUT_GUN1, Game::INPUT_GUN2 };
  const float aspect = float(xRes) / float(yRes);

  for (int p = 0; p < 2; p++)
  {
    float x = 0.0f, y = 0.0f;
    bool offscreen = false;   // hidden while the gun points away from the screen
    if (currentInputs & analogGun[p])
    {
      x = AnalogX(float(Inputs->analogGunX[p]->value));
      y = AnalogY(float(Inputs->analogGunY[p]->value));
      offscreen = Inputs->analogTriggerLeft[p]->value || Inputs->analogTriggerRight[p]->value;
    }
    else if (currentInputs & gun[p])
    {
      x = GunX(float(Inputs->gunX[p]->value));
      y = GunY(float(Inputs->gunY[p]->value));
      offscreen = Inputs->trigger[p]->offscreenValue > 0;
    }
    if ((wanted & (1u << p)) && !offscreen)
      Draw(m, x, y, p, aspect);
  }
}
