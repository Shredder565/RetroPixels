// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/OpcodeDecoding.h"

// Software pre-pass that transforms the positions of a batch to clip space and decides whether
// every triangle in it would be rejected (back-face culled or entirely off one side of the
// view), so the draw can be skipped without involving the host GPU.
class CPUCull
{
public:
  ~CPUCull();
  // Selects the fastest kernels the host CPU supports.
  void Init();
  bool AreAllVerticesCulled(VertexLoaderBase* loader, OpcodeDecoder::Primitive primitive,
                            const u8* src, u32 count);

  struct alignas(16) TransformedVertex
  {
    float x, y, z, w;
  };

  using TransformFunction = void (*)(void*, const void*, u32, int);
  using CullFunction = bool (*)(const CPUCull::TransformedVertex*, int);

private:
  // Number of triangle primitive kinds (quads, quads_2, triangles, strip, fan) and cull modes.
  static constexpr size_t TRIANGLE_PRIMITIVE_KINDS = 5;
  static constexpr size_t CULL_MODES = 4;

  struct AlignedFree
  {
    void operator()(TransformedVertex* ptr) const;
  };

  void EnsureCapacity(u32 count);

  std::unique_ptr<TransformedVertex[], AlignedFree> m_scratch{};
  u32 m_scratch_capacity = 0;
  // Indexed by (position has z) * 2 + (vertices carry their own matrix index).
  std::array<TransformFunction, 4> m_transforms{};
  std::array<std::array<CullFunction, CULL_MODES>, TRIANGLE_PRIMITIVE_KINDS> m_culls{};
};
