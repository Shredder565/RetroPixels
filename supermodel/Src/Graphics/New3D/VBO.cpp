// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// VBO.cpp

#include "VBO.h"

void VBO::Create(GLenum target, GLenum usage, GLsizeiptr size, const void *data)
{
  glGenBuffers(1, &m_id);
  glBindBuffer(target, m_id);
  glBufferData(target, size, data, usage);
  m_target = target;
  m_capacity = int(size);
  m_used = 0;
  glBindBuffer(target, 0);
}

void VBO::BufferSubData(GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
  glBufferSubData(m_target, offset, size, data);
}

bool VBO::AppendData(GLsizeiptr size, const GLvoid *data)
{
  if (size == 0 || data == nullptr)
    return true;
  if (m_used + size >= m_capacity)
    return false;
  BufferSubData(m_used, size, data);
  m_used += int(size);
  return true;
}

void VBO::Reset()
{
  m_used = 0;
}

void VBO::Destroy()
{
  if (m_id == 0)
    return;
  glDeleteBuffers(1, &m_id);
  m_id = 0;
  m_target = 0;
  m_capacity = 0;
  m_used = 0;
}

void VBO::Bind(bool enable)
{
  glBindBuffer(m_target, enable ? m_id : 0);
}

int VBO::GetSize() const
{
  return m_used;
}

int VBO::GetCapacity() const
{
  return m_capacity;
}
