// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"

namespace OGL
{
// A GL buffer object that the CPU streams data into (vertices, indices, uniforms, texel
// buffers). Create() picks the upload technique that suits the driver.
//
// Use: Map(max_size) gives a CPU pointer and the byte offset in the GL buffer where the data
// will be; Unmap(used_size) commits how much was actually written. Data from earlier Maps must
// not be touched again.
class StreamBuffer
{
public:
  static std::unique_ptr<StreamBuffer> Create(u32 type, u32 size);
  virtual ~StreamBuffer();

  u32 GetGLBufferId() const { return m_buffer; }
  u32 GetSize() const { return m_size; }
  u32 GetCurrentOffset() const { return m_write_pos; }

  virtual std::pair<u8*, u32> Map(u32 size) = 0;
  virtual void Unmap(u32 used_size) = 0;

  // Map with the returned offset rounded up to a multiple of stride.
  std::pair<u8*, u32> Map(u32 size, u32 stride)
  {
    const u32 misalignment = m_write_pos % stride;
    if (misalignment != 0)
      m_write_pos += stride - misalignment;
    return Map(size);
  }

protected:
  StreamBuffer(u32 type, u32 size);

  // Ring-buffer synchronisation for the techniques that write into memory the GPU may still be
  // reading. The buffer is split into CHUNKS equal parts with one fence each.
  void FenceEveryChunk();
  void DeleteOutstandingFences();
  void WaitForSpace(u32 size);

  const u32 m_target;
  const u32 m_buffer;
  const u32 m_size;  // a power of two

  u32 m_write_pos = 0;     // where the next Map writes
  u32 m_fenced_pos = 0;    // everything before this has a fence covering it
  u32 m_reserved_end = 0;  // everything before this is known to be free for writing

private:
  static constexpr int CHUNKS = 16;

  int ChunkOf(u32 offset) const { return static_cast<int>(offset >> m_chunk_shift); }
  void FenceChunks(int first, int end);
  void WaitChunks(int first, int last);

  const u32 m_chunk_shift;
  std::array<GLsync, CHUNKS> m_fences{};
};
}  // namespace OGL
