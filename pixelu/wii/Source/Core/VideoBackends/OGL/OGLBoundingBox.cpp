// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLBoundingBox.h"

#include <array>
#include <cstring>

#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoCommon/DriverDetails.h"

namespace OGL
{
namespace
{
constexpr GLintptr ByteOffset(u32 index)
{
  return static_cast<GLintptr>(sizeof(BBoxType) * index);
}

constexpr GLsizeiptr ByteSize(size_t count)
{
  return static_cast<GLsizeiptr>(sizeof(BBoxType) * count);
}

// Reading with glGetBufferSubData is far faster than mapping on NVIDIA (whose drivers this
// path is chosen for); mapping is the faster choice elsewhere, AMD in particular.
bool ReadWithGetBufferSubData()
{
  return !DriverDetails::HasBug(DriverDetails::BUG_SLOW_GETBUFFERSUBDATA) &&
         !static_cast<OGLGfx*>(g_gfx.get())->IsGLES() && g_ogl_config.bSupportsImageLoadStore;
}
}  // namespace

OGLBoundingBox::~OGLBoundingBox()
{
  if (m_ssbo != 0)
    glDeleteBuffers(1, &m_ssbo);
}

bool OGLBoundingBox::Initialize()
{
  constexpr std::array<BBoxType, NUM_BBOX_VALUES> zeroes{};
  glGenBuffers(1, &m_ssbo);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, ByteSize(zeroes.size()), zeroes.data(), GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_ssbo);
  return true;
}

std::vector<BBoxType> OGLBoundingBox::Read(u32 index, u32 length)
{
  std::vector<BBoxType> result(length);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);

  if (ReadWithGetBufferSubData())
  {
    // Without the barrier NVIDIA can return values from before the latest shader writes; the
    // buffer is evidently not coherent with this kind of read there.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, ByteOffset(index), ByteSize(length),
                       result.data());
  }
  else
  {
    // The whole buffer is mapped and the wanted part copied out. On a failed map the values
    // stay zero.
    const void* mapped =
        glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, ByteSize(NUM_BBOX_VALUES), GL_MAP_READ_BIT);
    if (mapped != nullptr)
    {
      std::memcpy(result.data(), static_cast<const u8*>(mapped) + ByteOffset(index),
                  static_cast<size_t>(ByteSize(length)));
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
  }

  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  return result;
}

void OGLBoundingBox::Write(u32 index, std::span<const BBoxType> values)
{
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, ByteOffset(index), ByteSize(values.size()),
                  values.data());
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

}  // namespace OGL
