// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TextureBank.h
//
// One 2048x1024 sheet of Real3D texture RAM as an unsigned 16-bit integer
// texture. The sheet's mipmaps live inside texture RAM itself (level n at a
// fixed corner of the sheet); here each one is copied into the matching
// OpenGL mip level so shaders can fetch texels directly. The shaders decode
// the texel formats.

#ifndef INCLUDED_NEW3D_TEXTUREBANK_H
#define INCLUDED_NEW3D_TEXTUREBANK_H

#include <GL/glew.h>
#include <cstdint>

namespace New3D {

class TextureBank
{
public:
  TextureBank();
  ~TextureBank();

  void AttachMemory(const uint16_t *sheet);   // first texel of this sheet in texture RAM (2048 texels per row)
  void Bind();
  // Copies a rectangle given in texture RAM coordinates (x, y within the
  // sheet) into mip level 'level'.
  void UploadTextures(int level, int x, int y, int width, int height);
  int GetNumberOfLevels() const;

private:
  const uint16_t *m_sheet = nullptr;
  GLuint m_texture = 0;
  int m_levels = 0;
};

} // New3D

#endif  // INCLUDED_NEW3D_TEXTUREBANK_H
