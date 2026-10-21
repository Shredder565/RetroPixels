// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLStreamBuffer.h"

#include <algorithm>

#include "Common/Align.h"
#include "Common/MathUtil.h"
#include "Common/MemoryUtil.h"

#include "VideoBackends/OGL/OGLConfig.h"

#include "VideoCommon/DriverDetails.h"

namespace OGL
{
namespace
{
GLuint NewBufferName()
{
  GLuint name = 0;
  glGenBuffers(1, &name);
  return name;
}

constexpr GLbitfield UNSYNCHRONIZED_WRITE =
    GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
}  // namespace

StreamBuffer::StreamBuffer(u32 type, u32 size)
    : m_target(type), m_buffer(NewBufferName()), m_size(MathUtil::NextPowerOf2(size)),
      m_chunk_shift(static_cast<u32>(MathUtil::IntLog2(MathUtil::NextPowerOf2(size) / CHUNKS)))
{
}

StreamBuffer::~StreamBuffer()
{
  glDeleteBuffers(1, &m_buffer);
}

// ---------------------------------------------------------------------------------------------
// Fence ring
//
// Each chunk of the buffer has a fence that signals once the GPU has consumed what was written
// there. Before the CPU writes into a chunk again it waits on (and deletes) that chunk's fence.
// Fences are inserted lazily: at the next Map, every chunk written since the previous Map gets
// a new fence (the GPU commands using the data were issued in between).
//
//   m_fenced_pos   - chunks before this have their fence inserted
//   m_reserved_end - chunks up to this have been waited on and may be written
//   m_write_pos    - the next write

void StreamBuffer::FenceChunks(int first, int end)
{
  for (int chunk = first; chunk < end; ++chunk)
    m_fences[static_cast<size_t>(chunk)] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void StreamBuffer::WaitChunks(int first, int last)
{
  for (int chunk = first; chunk <= last; ++chunk)
  {
    glClientWaitSync(m_fences[static_cast<size_t>(chunk)], GL_SYNC_FLUSH_COMMANDS_BIT,
                     GL_TIMEOUT_IGNORED);
    glDeleteSync(m_fences[static_cast<size_t>(chunk)]);
  }
}

void StreamBuffer::FenceEveryChunk()
{
  FenceChunks(0, CHUNKS);
}

void StreamBuffer::DeleteOutstandingFences()
{
  // The fences still alive are the ones not yet waited on: after the reserved chunk up to the
  // end, and from the start up to the chunk being written.
  for (int chunk = ChunkOf(m_reserved_end) + 1; chunk < CHUNKS; ++chunk)
    glDeleteSync(m_fences[static_cast<size_t>(chunk)]);
  for (int chunk = 0; chunk < ChunkOf(m_write_pos); ++chunk)
    glDeleteSync(m_fences[static_cast<size_t>(chunk)]);
}

void StreamBuffer::WaitForSpace(u32 size)
{
  // Fence what was written since the last map.
  FenceChunks(ChunkOf(m_fenced_pos), ChunkOf(m_write_pos));
  m_fenced_pos = m_write_pos;

  // Wait for the chunks this write reaches that have not been waited on yet.
  const u32 end = m_write_pos + size;
  WaitChunks(ChunkOf(m_reserved_end) + 1, std::min(ChunkOf(end), CHUNKS - 1));

  // A large earlier reservation that was only partly used stays reserved, so its fences are
  // not waited on twice.
  m_reserved_end = std::max(m_reserved_end, end);

  if (end < m_size)
    return;

  // No room before the end: fence the unused tail, start over at offset 0 and wait for the
  // chunks the write needs there.
  FenceChunks(ChunkOf(m_fenced_pos), CHUNKS);
  m_write_pos = 0;
  m_fenced_pos = 0;
  WaitChunks(0, std::min(ChunkOf(size), CHUNKS - 1));
  m_reserved_end = size;
}

namespace
{
// ---------------------------------------------------------------------------------------------
// Upload techniques

// Map unsynchronised ranges one after another; at the end of the buffer, orphan it with a fresh
// glBufferData. Needs no sync objects.
class OrphaningStream final : public StreamBuffer
{
public:
  OrphaningStream(u32 type, u32 size) : StreamBuffer(type, size)
  {
    glBindBuffer(m_target, m_buffer);
    glBufferData(m_target, m_size, nullptr, GL_STREAM_DRAW);
  }

  std::pair<u8*, u32> Map(u32 size) override
  {
    if (m_write_pos + size >= m_size)
    {
      glBufferData(m_target, m_size, nullptr, GL_STREAM_DRAW);
      m_write_pos = 0;
    }
    auto* ptr =
        static_cast<u8*>(glMapBufferRange(m_target, m_write_pos, size, UNSYNCHRONIZED_WRITE));
    return {ptr, m_write_pos};
  }

  void Unmap(u32 used_size) override
  {
    glFlushMappedBufferRange(m_target, 0, used_size);
    glUnmapBuffer(m_target);
    m_write_pos += used_size;
  }
};

// Unsynchronised mapping into a fixed ring, with fences keeping the CPU behind the GPU.
class FencedMappingStream final : public StreamBuffer
{
public:
  FencedMappingStream(u32 type, u32 size) : StreamBuffer(type, size)
  {
    FenceEveryChunk();
    glBindBuffer(m_target, m_buffer);
    glBufferData(m_target, m_size, nullptr, GL_STREAM_DRAW);
  }

  ~FencedMappingStream() override { DeleteOutstandingFences(); }

  std::pair<u8*, u32> Map(u32 size) override
  {
    WaitForSpace(size);
    auto* ptr =
        static_cast<u8*>(glMapBufferRange(m_target, m_write_pos, size, UNSYNCHRONIZED_WRITE));
    return {ptr, m_write_pos};
  }

  void Unmap(u32 used_size) override
  {
    glFlushMappedBufferRange(m_target, 0, used_size);
    glUnmapBuffer(m_target);
    m_write_pos += used_size;
  }
};

// ARB/EXT_buffer_storage: the ring is mapped once, persistently and coherently, so Map and
// Unmap make no GL calls except for the fences.
class PersistentStream final : public StreamBuffer
{
public:
  PersistentStream(u32 type, u32 size) : StreamBuffer(type, size)
  {
    FenceEveryChunk();
    glBindBuffer(m_target, m_buffer);
    constexpr GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(m_target, m_size, nullptr, flags);
    m_base = static_cast<u8*>(glMapBufferRange(m_target, 0, m_size, flags));
  }

  ~PersistentStream() override
  {
    DeleteOutstandingFences();
    glUnmapBuffer(m_target);
    glBindBuffer(m_target, 0);
  }

  std::pair<u8*, u32> Map(u32 size) override
  {
    WaitForSpace(size);
    return {m_base + m_write_pos, m_write_pos};
  }

  void Unmap(u32 used_size) override { m_write_pos += used_size; }

private:
  u8* m_base = nullptr;
};

// AMD_pinned_memory: the GL buffer is backed by our own page-aligned allocation, so writes go
// straight to memory the GPU reads. Synchronised with the same fence ring.
class PinnedStream final : public StreamBuffer
{
public:
  PinnedStream(u32 type, u32 size) : StreamBuffer(type, size)
  {
    FenceEveryChunk();
    const size_t bytes = Common::AlignUp(m_size, PAGE);
    m_memory = static_cast<u8*>(Common::AllocateAlignedMemory(bytes, PAGE));
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, m_buffer);
    glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, static_cast<GLsizeiptr>(bytes), m_memory,
                 GL_STREAM_COPY);
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 0);
    glBindBuffer(m_target, m_buffer);
  }

  ~PinnedStream() override
  {
    DeleteOutstandingFences();
    glBindBuffer(m_target, 0);
    // The GPU may still read the memory until everything queued has finished.
    glFinish();
    Common::FreeAlignedMemory(m_memory);
  }

  std::pair<u8*, u32> Map(u32 size) override
  {
    WaitForSpace(size);
    return {m_memory + m_write_pos, m_write_pos};
  }

  void Unmap(u32 used_size) override { m_write_pos += used_size; }

private:
  static constexpr u32 PAGE = 4096;
  u8* m_memory = nullptr;
};

// Everything is written to a CPU copy and uploaded to offset 0 with glBufferSubData (one extra
// copy in the driver). For drivers where the mapping paths are unusable.
class SubDataStream final : public StreamBuffer
{
public:
  SubDataStream(u32 type, u32 size) : StreamBuffer(type, size), m_staging(new u8[m_size])
  {
    glBindBuffer(m_target, m_buffer);
    // Sized as requested, not rounded up.
    glBufferData(m_target, size, nullptr, GL_STATIC_DRAW);
  }

  std::pair<u8*, u32> Map(u32) override { return {m_staging.get(), 0}; }
  void Unmap(u32 used_size) override { glBufferSubData(m_target, 0, used_size, m_staging.get()); }

private:
  std::unique_ptr<u8[]> m_staging;
};

// Like SubDataStream but re-specifies the whole buffer with glBufferData on every upload, for
// drivers that stall in glBufferSubData. The slowest option.
class RespecifyStream final : public StreamBuffer
{
public:
  RespecifyStream(u32 type, u32 size) : StreamBuffer(type, size), m_staging(new u8[m_size])
  {
    glBindBuffer(m_target, m_buffer);
  }

  std::pair<u8*, u32> Map(u32) override { return {m_staging.get(), 0}; }
  void Unmap(u32 used_size) override
  {
    glBufferData(m_target, used_size, m_staging.get(), GL_STREAM_DRAW);
  }

private:
  std::unique_ptr<u8[]> m_staging;
};

std::unique_ptr<StreamBuffer> CopyingStream(u32 type, u32 size)
{
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BUFFER_STREAM))
    return std::make_unique<RespecifyStream>(type, size);
  return std::make_unique<SubDataStream>(type, size);
}
}  // namespace

std::unique_ptr<StreamBuffer> StreamBuffer::Create(u32 type, u32 size)
{
  // Without base vertex every draw reads from offset 0, so only the copying techniques (which
  // always upload to 0) work.
  if (!g_ogl_config.bSupportsGLBaseVertex)
    return CopyingStream(type, size);

  // Without sync objects nothing can tell when the GPU is done with a range, so orphan.
  if (!g_ogl_config.bSupportsGLSync)
    return std::make_unique<OrphaningStream>(type, size);

  // Pinned memory beats buffer storage on AMD.
  if (g_ogl_config.bSupportsGLPinnedMemory &&
      !DriverDetails::HasBug(DriverDetails::BUG_BROKEN_PINNED_MEMORY))
  {
    return std::make_unique<PinnedStream>(type, size);
  }

  const bool storage_broken_here =
      (type == GL_ARRAY_BUFFER &&
       DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BUFFER_STORAGE)) ||
      (type == GL_ELEMENT_ARRAY_BUFFER &&
       DriverDetails::HasBug(DriverDetails::BUG_INTEL_BROKEN_BUFFER_STORAGE));
  if (g_ogl_config.bSupportsGLBufferStorage && !storage_broken_here)
    return std::make_unique<PersistentStream>(type, size);

  // Drivers with slow unsynchronised mapping (NVIDIA) use the copying techniques instead.
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_UNSYNC_MAPPING))
    return CopyingStream(type, size);

  return std::make_unique<FencedMappingStream>(type, size);
}
}  // namespace OGL
