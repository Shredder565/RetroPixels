// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TextureBank.cpp

#include "TextureBank.h"

namespace New3D {

namespace
{
  // Where mip level n starts inside the sheet.
  constexpr int kLevelX[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
  constexpr int kLevelY[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023, 1023 };
}

TextureBank::TextureBank()
{
  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);

  // Full mip chain down to 1x1 (12 levels for 2048x1024). Levels 0-10 hold
  // the sheet's mipmaps.
  int w = 2048, h = 1024, level = 0;
  for (;;)
  {
    glTexImage2D(GL_TEXTURE_2D, level, GL_R16UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, nullptr);
    if (w == 1 && h == 1)
      break;
    w = (w > 1) ? w / 2 : 1;
    h = (h > 1) ? h / 2 : 1;
    level++;
  }
  m_levels = level;   // levels below the last (1x1) one
}

TextureBank::~TextureBank()
{
  if (m_texture)
    glDeleteTextures(1, &m_texture);
  m_texture = 0;
}

void TextureBank::AttachMemory(const uint16_t *sheet)
{
  m_sheet = sheet;
}

void TextureBank::Bind()
{
  glBindTexture(GL_TEXTURE_2D, m_texture);
}

void TextureBank::UploadTextures(int level, int x, int y, int width, int height)
{
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
  const int toX = x - kLevelX[level];
  const int toY = y - kLevelY[level];
  // One row at a time: rows that fall outside the level are rejected by GL
  // on their own without losing the others.
  for (int row = 0; row < height; row++)
    glTexSubImage2D(GL_TEXTURE_2D, level, toX, toY + row, width, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, m_sheet + size_t(y + row) * 2048 + x);
}

int TextureBank::GetNumberOfLevels() const
{
  return m_levels;
}

} // New3D
