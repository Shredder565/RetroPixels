// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include <cstdint>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoBackends/OGL/OGLVertexManager.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexShaderGen.h"

namespace OGL
{
std::unique_ptr<NativeVertexFormat>
OGLGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<GLVertexFormat>(vtx_decl);
}

namespace
{
GLenum ComponentType(ComponentFormat format)
{
  switch (format)
  {
  case ComponentFormat::UByte:
    return GL_UNSIGNED_BYTE;
  case ComponentFormat::Byte:
    return GL_BYTE;
  case ComponentFormat::UShort:
    return GL_UNSIGNED_SHORT;
  case ComponentFormat::Short:
    return GL_SHORT;
  default:
    // Float, and the unnamed values that games use as float.
    return GL_FLOAT;
  }
}

// Points one attribute at its place in the interleaved vertex. Integer attributes stay
// integers; the rest are normalised to [0, 1] / [-1, 1] (floats are unaffected).
void DescribeAttribute(GLuint location, GLsizei stride, const AttributeFormat& attr)
{
  if (!attr.enable)
    return;

  const auto* offset =
      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(static_cast<u32>(attr.offset)));
  glEnableVertexAttribArray(location);
  if (attr.integer)
    glVertexAttribIPointer(location, attr.components, ComponentType(attr.type), stride, offset);
  else
    glVertexAttribPointer(location, attr.components, ComponentType(attr.type), GL_TRUE, stride,
                          offset);
}

GLuint Location(ShaderAttrib attrib, u32 index = 0)
{
  return static_cast<GLuint>(attrib) + index;
}
}  // namespace

GLVertexFormat::GLVertexFormat(const PortableVertexDeclaration& vtx_decl)
    : NativeVertexFormat(vtx_decl)
{
  const u32 stride = static_cast<u32>(vtx_decl.stride);
  if ((stride & 3) != 0)
    PanicAlertFmt("Uneven vertex stride: {}", stride);

  const auto* vm = static_cast<const VertexManager*>(g_vertex_manager.get());

  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);

  // The element buffer binding is part of the VAO, so every VAO gets it.
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vm->GetIndexBufferHandle());
  glBindBuffer(GL_ARRAY_BUFFER, vm->GetVertexBufferHandle());

  const auto gl_stride = static_cast<GLsizei>(stride);
  DescribeAttribute(Location(ShaderAttrib::Position), gl_stride, vtx_decl.position);
  for (u32 i = 0; i < 3; ++i)
    DescribeAttribute(Location(ShaderAttrib::Normal, i), gl_stride, vtx_decl.normals[i]);
  for (u32 i = 0; i < 2; ++i)
    DescribeAttribute(Location(ShaderAttrib::Color0, i), gl_stride, vtx_decl.colors[i]);
  for (u32 i = 0; i < 8; ++i)
    DescribeAttribute(Location(ShaderAttrib::TexCoord0, i), gl_stride, vtx_decl.texcoords[i]);
  DescribeAttribute(Location(ShaderAttrib::PositionMatrix), gl_stride, vtx_decl.posmtx);

  // Creating the VAO bound it; put back whatever the rest of the backend thinks is bound.
  ProgramShaderCache::ReBindVertexFormat();
}

GLVertexFormat::~GLVertexFormat()
{
  ProgramShaderCache::InvalidateVertexFormatIfBound(m_vao);
  glDeleteVertexArrays(1, &m_vao);
}
}  // namespace OGL
