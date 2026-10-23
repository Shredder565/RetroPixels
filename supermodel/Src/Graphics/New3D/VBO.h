// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// VBO.h
//
// A fixed-capacity OpenGL buffer object that can be filled from the start
// with AppendData() or written anywhere with BufferSubData().

#ifndef INCLUDED_NEW3D_VBO_H
#define INCLUDED_NEW3D_VBO_H

#include <GL/glew.h>

class VBO
{
public:
  VBO() = default;

  void Create(GLenum target, GLenum usage, GLsizeiptr size, const void *data = nullptr);
  void BufferSubData(GLintptr offset, GLsizeiptr size, const GLvoid *data);   // buffer must be bound
  bool AppendData(GLsizeiptr size, const GLvoid *data);                      // false if it would not fit
  void Reset();       // appends start from the beginning again (contents kept)
  void Destroy();
  void Bind(bool enable);
  int GetSize() const;
  int GetCapacity() const;

private:
  GLuint m_id = 0;
  GLenum m_target = 0;
  int m_capacity = 0;
  int m_used = 0;
};

#endif  // INCLUDED_NEW3D_VBO_H
