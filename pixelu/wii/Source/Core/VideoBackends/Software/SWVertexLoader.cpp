// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SWVertexLoader.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "Core/System.h"

#include "VideoBackends/Software/NativeVertexFormat.h"
#include "VideoBackends/Software/Rasterizer.h"
#include "VideoBackends/Software/TransformUnit.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace
{
// Converts one stored component. Integers converted to float are normalised by their type's
// maximum value.
template <typename Out, typename In>
Out ConvertComponent(In raw)
{
  Out value = static_cast<Out>(raw);
  if constexpr (std::is_floating_point_v<Out> && std::is_integral_v<In>)
    value *= static_cast<Out>(1.0 / std::numeric_limits<In>::max());
  return value;
}

template <typename Out>
Out ReadComponent(DataReader& src, ComponentFormat type)
{
  switch (type)
  {
  case ComponentFormat::UByte:
    return ConvertComponent<Out>(src.Read<u8, false>());
  case ComponentFormat::Byte:
    return ConvertComponent<Out>(src.Read<s8, false>());
  case ComponentFormat::UShort:
    return ConvertComponent<Out>(src.Read<u16, false>());
  case ComponentFormat::Short:
    return ConvertComponent<Out>(src.Read<s16, false>());
  default:  // float, including the undefined float encodings 5-7
    return ConvertComponent<Out>(src.Read<float, false>());
  }
}

// Reads up to `count` components of an attribute, starting at component `first`, into dst
// (backwards when `reversed`). Components the attribute doesn't have read as 0, except a
// missing fourth component, which reads as 1. A disabled attribute leaves dst alone.
template <typename Out>
void ReadAttribute(Out* dst, DataReader vertex, const AttributeFormat& format, int first, int count,
                   bool reversed)
{
  if (!format.enable)
    return;

  vertex.Skip(static_cast<size_t>(format.offset));
  vertex.Skip(static_cast<size_t>(first) * GetElementSize(format.type));

  const int present = std::min(format.components - first, count);
  int i = 0;
  for (; i < present; ++i)
  {
    dst[reversed ? count - 1 - i : i] = ReadComponent<Out>(vertex, format.type);
    ASSERT_MSG(VIDEO, !format.integer || (format.type < ComponentFormat::Float),
               "only non-float values are allowed to be streamed as integer");
  }
  for (; i < count; ++i)
    dst[reversed ? count - 1 - i : i] = static_cast<Out>(i == 3 ? 1 : 0);
}

// The missing-colour value is RGBA with red in the top byte; vertex colours are stored A, B, G, R.
void SetMissingColor(std::array<u8, 4>& color)
{
  const u32 value = g_ActiveConfig.iMissingColorValue;
  for (size_t i = 0; i < 4; ++i)
    color[i] = static_cast<u8>(value >> (8 * i));
}

void ReadColors(InputVertexData& out, const DataReader& vertex,
                const PortableVertexDeclaration& vdec)
{
  // Colours fill the channels in order: with only one colour attribute (either of them), it goes
  // to channel 0. Missing channels get the configured placeholder colour.
  const AttributeFormat* present[2] = {nullptr, nullptr};
  size_t used = 0;
  for (const AttributeFormat& format : vdec.colors)
  {
    if (format.enable)
      present[used++] = &format;
  }
  for (size_t chan = 0; chan < 2; ++chan)
  {
    if (present[chan])
      ReadAttribute<u8>(out.color[chan].data(), vertex, *present[chan], 0, 4, true);
    else
      SetMissingColor(out.color[chan]);
  }
}

void LoadCached(Common::Vec3& dst, const float4& cached)
{
  dst.x = cached[0];
  dst.y = cached[1];
  dst.z = cached[2];
}
}  // namespace

SWVertexLoader::SWVertexLoader() = default;

SWVertexLoader::~SWVertexLoader() = default;

DataReader SWVertexLoader::PrepareForAdditionalData(OpcodeDecoder::Primitive primitive, u32 count,
                                                    u32 stride, bool)
{
  // Never cull everything here: culled batches still have to reach the rasterizer so that
  // z-freeze sees their depth slope.
  return VertexManagerBase::PrepareForAdditionalData(primitive, count, stride, false);
}

void SWVertexLoader::DrawCurrentBatch(u32, u32, u32)
{
  using OpcodeDecoder::Primitive;
  Primitive primitive = Primitive::GX_DRAW_QUADS;
  switch (m_current_primitive_type)
  {
  case PrimitiveType::Points:
    primitive = Primitive::GX_DRAW_POINTS;
    break;
  case PrimitiveType::Lines:
    primitive = Primitive::GX_DRAW_LINES;
    break;
  case PrimitiveType::Triangles:
    primitive = Primitive::GX_DRAW_TRIANGLES;
    break;
  case PrimitiveType::TriangleStrip:
    primitive = Primitive::GX_DRAW_TRIANGLE_STRIP;
    break;
  }

  // The base class's flush is bypassed here, so the bounding box is flushed by hand.
  if (g_bounding_box->IsEnabled())
    g_bounding_box->Flush();

  m_setup_unit.Init(primitive);
  Rasterizer::SetTevKonstColors();

  const PortableVertexDeclaration& vdec =
      VertexLoaderManager::GetCurrentVertexFormat()->GetVertexDeclaration();
  const u32 index_count = m_index_generator.GetIndexLen();
  for (u32 i = 0; i < index_count; ++i)
  {
    m_vertex = InputVertexData{};
    SetFormat();
    ParseVertex(vdec, m_cpu_index_buffer[i]);

    OutputVertexData* out = m_setup_unit.GetVertex();
    TransformUnit::TransformPosition(&m_vertex, out);
    TransformUnit::TransformNormal(&m_vertex, out);
    TransformUnit::TransformColor(&m_vertex, out);
    TransformUnit::TransformTexCoord(&m_vertex, out);

    m_setup_unit.SetupVertex();
    INCSTAT(g_stats.this_frame.num_vertices_loaded);
  }

  INCSTAT(g_stats.this_frame.num_drawn_objects);
}

void SWVertexLoader::SetFormat()
{
  const auto& a = xfmem.MatrixIndexA;
  const auto& b = xfmem.MatrixIndexB;
  m_vertex.posMtx = static_cast<u8>(a.PosNormalMtxIdx);
  m_vertex.texMtx = {static_cast<u8>(a.Tex0MtxIdx), static_cast<u8>(a.Tex1MtxIdx),
                     static_cast<u8>(a.Tex2MtxIdx), static_cast<u8>(a.Tex3MtxIdx),
                     static_cast<u8>(b.Tex4MtxIdx), static_cast<u8>(b.Tex5MtxIdx),
                     static_cast<u8>(b.Tex6MtxIdx), static_cast<u8>(b.Tex7MtxIdx)};
}

void SWVertexLoader::ParseVertex(const PortableVertexDeclaration& vdec, int index)
{
  DataReader vertex(m_cpu_vertex_buffer.data(),
                    m_cpu_vertex_buffer.data() + m_cpu_vertex_buffer.size());
  vertex.Skip(static_cast<size_t>(index) * static_cast<size_t>(vdec.stride));

  ReadAttribute<float>(&m_vertex.position.x, vertex, vdec.position, 0, 3, false);

  // Normal, tangent and binormal; absent ones keep the last values the game loaded into XF.
  for (size_t i = 0; i < m_vertex.normal.size(); ++i)
    ReadAttribute<float>(&m_vertex.normal[i].x, vertex, vdec.normals[i], 0, 3, false);
  const auto& vs_constants = Core::System::GetInstance().GetVertexShaderManager().constants;
  if (!vdec.normals[0].enable)
    LoadCached(m_vertex.normal[0], vs_constants.cached_normal);
  if (!vdec.normals[1].enable)
    LoadCached(m_vertex.normal[1], vs_constants.cached_tangent);
  if (!vdec.normals[2].enable)
    LoadCached(m_vertex.normal[2], vs_constants.cached_binormal);

  ReadColors(m_vertex, vertex, vdec);

  for (size_t i = 0; i < m_vertex.texCoords.size(); ++i)
  {
    const AttributeFormat& format = vdec.texcoords[i];
    ReadAttribute<float>(&m_vertex.texCoords[i].x, vertex, format, 0, 2, false);
    // A third component carries the texture matrix index.
    if (format.components >= 3)
      ReadAttribute<u8>(&m_vertex.texMtx[i], vertex, format, 2, 1, false);
  }

  ReadAttribute<u8>(&m_vertex.posMtx, vertex, vdec.posmtx, 0, 1, false);
}
