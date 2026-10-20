// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "VideoCommon/VertexLoaderBase.h"

enum class VertexComponentFormat;
enum class ComponentFormat;
enum class ColorFormat;
enum class CPArray : u8;

// The x86-64 vertex loader: generates one specialised conversion loop per vertex format.
class VertexLoaderX64 : public VertexLoaderBase, public Gen::X64CodeBlock
{
public:
  VertexLoaderX64(const TVtxDesc& vtx_desc, const VAT& vtx_att);

protected:
  int RunVertices(const u8* src, u8* dst, int count) override;

private:
  // Which z-freeze/lighting cache a converted vector also goes to.
  enum class Cache
  {
    None,
    Position,
    Normal,
    Tangent,
    Binormal,
  };

  // How one vector attribute is converted.
  struct VectorJob
  {
    VertexComponentFormat source;
    ComponentFormat format;
    int count_in;
    int count_out;
    // Power-of-two fraction to scale fixed-point values by, or 0 for none.
    u32 frac;
    Cache cache;
  };

  using Constant = std::array<u8, 16>;

  void Generate();
  Gen::OpArg AttributeData(CPArray array, VertexComponentFormat source, bool may_drop);
  void ConvertVector(Gen::OpArg data, const VectorJob& job, AttributeFormat* attr);
  void LoadVectorSSSE3(Gen::OpArg data, const VectorJob& job);
  void LoadVectorSSE2(Gen::OpArg data, const VectorJob& job);
  void StoreToCache(Cache cache);
  void ConvertColor(Gen::OpArg data, VertexComponentFormat source, ColorFormat format);
  void WriteTexMatrix(u32 index_offset, bool coordinate_present);
  const void* PoolConstant(const Constant& value);
  Gen::OpArg Global(const void* address) const;

  // Read and write positions within the current vertex while generating.
  u32 m_in = 0;
  u32 m_out = 0;
  // Branches taken when an indexed position reads an all-ones index.
  std::vector<Gen::FixupBranch> m_drop_jumps;
  // 16-byte constants used by the generated code, at the start of the code space.
  u8* m_pool = nullptr;
  u32 m_pool_used = 0;
  const u8* m_entry = nullptr;
};
