// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoader.h"

#include <cmath>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexLoaderUtils.h"
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"

namespace
{
// The position/normal matrix index byte (6 bits used), written as a 32-bit integer.
void ReadPositionMatrix(VertexLoaderCursor& cursor, const VertexLoaderStep&)
{
  const u32 index = *cursor.src++ & 0x3fu;
  if (cursor.remaining < 3)
    VertexLoaderManager::position_matrix_index_cache[static_cast<size_t>(cursor.remaining)] = index;
  VertexData::Emit(cursor.dst, index);
}

// A texture matrix index byte. It is kept until its texture coordinate is written.
void ReadTexMatrix(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  cursor.texmtx[step.slot] = static_cast<u8>(*cursor.src++ & 0x3fu);
}

// With a texture matrix index, a texture coordinate becomes three floats: s, t and the index.
// These fill in after the components that were read (none, s, or s and t).
template <u32 Zeros>
void WriteTexMatrix(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  for (u32 i = 0; i < Zeros; i++)
    VertexData::Emit(cursor.dst, 0.0f);
  VertexData::Emit(cursor.dst, static_cast<float>(cursor.texmtx[step.slot]));
}

float FracScale(u32 frac_bits)
{
  return std::ldexp(1.0f, -static_cast<int>(frac_bits));
}

// Normals have fixed fractions: 6 bits for s8, 7 for u8, 14 for s16 and 15 for u16.
float NormalScale(ComponentFormat format)
{
  switch (format)
  {
  case ComponentFormat::UByte:
    return FracScale(7);
  case ComponentFormat::Byte:
    return FracScale(6);
  case ComponentFormat::UShort:
    return FracScale(15);
  case ComponentFormat::Short:
    return FracScale(14);
  default:
    return 1.0f;
  }
}

void SetAttribute(AttributeFormat& attr, ComponentFormat type, int components, int offset,
                  bool integer)
{
  attr.type = type;
  attr.components = components;
  attr.offset = offset;
  attr.enable = true;
  attr.integer = integer;
}
}  // namespace

VertexLoader::VertexLoader(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
    : VertexLoaderBase(vtx_desc, vtx_attr)
{
  BuildSteps();
}

void VertexLoader::AddStep(VertexLoaderStep::Fn fn, float scale, CPArray array, u8 slot)
{
  m_steps.push_back(VertexLoaderStep{fn, scale, array, slot});
}

void VertexLoader::BuildSteps()
{
  const auto& desc = m_VtxDesc;
  const auto& attr = m_VtxAttr;
  PortableVertexDeclaration& decl = m_native_vtx_decl;
  int offset = 0;

  if (desc.low.PosMatIdx)
  {
    AddStep(ReadPositionMatrix);
    SetAttribute(decl.posmtx, ComponentFormat::UByte, 4, offset, true);
    offset += 4;
  }

  for (u8 slot = 0; slot < 8; slot++)
  {
    if (desc.low.TexMatIdx[slot])
      AddStep(ReadTexMatrix, 1.0f, CPArray::Position, slot);
  }

  const int position_components = attr.g0.PosElements == CoordComponentCount::XYZ ? 3 : 2;
  AddStep(VertexLoader_Position::GetReader(desc.low.Position, attr.g0.PosFormat,
                                           attr.g0.PosElements),
          FracScale(attr.g0.PosFrac), CPArray::Position);
  SetAttribute(decl.position, ComponentFormat::Float, position_components, offset, false);
  offset += 4 * position_components;
  m_position_indexed = IsIndexed(desc.low.Position);

  if (desc.low.Normal != VertexComponentFormat::NotPresent)
  {
    AddStep(VertexLoader_Normal::GetReader(desc.low.Normal, attr.g0.NormalFormat,
                                           attr.g0.NormalElements, attr.g0.NormalIndex3),
            NormalScale(attr.g0.NormalFormat), CPArray::Normal);
    const size_t vectors = attr.g0.NormalElements == NormalComponentCount::NTB ? 3 : 1;
    for (size_t v = 0; v < vectors; v++)
    {
      SetAttribute(decl.normals[v], ComponentFormat::Float, 3, offset, false);
      offset += 12;
    }
  }

  for (u8 i = 0; i < 2; i++)
  {
    AttributeFormat& color = decl.colors[i];
    color.type = ComponentFormat::UByte;
    color.components = 4;
    color.integer = false;

    const VertexComponentFormat source = desc.low.Color[i];
    const VertexLoaderStep::Fn reader = VertexLoader_Color::GetReader(source, attr.GetColorFormat(i));
    if (reader != nullptr)
      AddStep(reader, 1.0f, CPArray::Color0 + i);
    else
      ASSERT(source == VertexComponentFormat::NotPresent);

    if (source != VertexComponentFormat::NotPresent)
    {
      color.offset = offset;
      color.enable = true;
      offset += 4;
    }
  }

  offset = AppendTexCoords(offset);

  m_native_vtx_decl.stride = offset;
}

int VertexLoader::AppendTexCoords(int offset)
{
  const auto& desc = m_VtxDesc;
  const auto& attr = m_VtxAttr;

  // Slots after the last one that has a coordinate or a matrix index are left out entirely, but
  // the first absent slot still gets its offset and type written.
  int last_used = -1;
  for (u8 i = 0; i < 8; i++)
  {
    if (desc.high.TexCoord[i] != VertexComponentFormat::NotPresent || desc.low.TexMatIdx[i])
      last_used = i;
  }

  for (u8 i = 0; i < 8; i++)
  {
    AttributeFormat& tex = m_native_vtx_decl.texcoords[i];
    tex.offset = offset;
    tex.type = ComponentFormat::Float;
    tex.integer = false;

    const VertexComponentFormat source = desc.high.TexCoord[i];
    const bool present = source != VertexComponentFormat::NotPresent;
    const ComponentFormat format = attr.GetTexFormat(i);
    const bool two = attr.GetTexElements(i) == TexComponentCount::ST;

    if (present)
    {
      ASSERT_MSG(VIDEO, format <= ComponentFormat::Float,
                 "Invalid texture coordinates format!\n(format = {})", format);
      AddStep(VertexLoader_TextCoord::GetReader(source, format, attr.GetTexElements(i)),
              FracScale(attr.GetTexFrac(i)), CPArray::TexCoord0 + i);
    }

    if (desc.low.TexMatIdx[i])
    {
      tex.enable = true;
      tex.components = 3;
      offset += 12;
      AddStep(!present ? WriteTexMatrix<2> : (two ? WriteTexMatrix<0> : WriteTexMatrix<1>), 1.0f,
              CPArray::Position, i);
    }
    else if (present)
    {
      tex.enable = true;
      tex.components = two ? 2 : 1;
      offset += two ? 8 : 4;
    }

    if (!present && i >= last_used)
      break;
  }
  return offset;
}

int VertexLoader::RunVertices(const u8* src, u8* dst, int count)
{
  m_numLoadedVertices += count;

  VertexLoaderCursor cursor{};
  cursor.src = src;
  cursor.dst = dst;

  const ptrdiff_t stride = m_native_vtx_decl.stride;
  int dropped = 0;
  for (int left = count - 1; left >= 0; left--)
  {
    cursor.remaining = left;
    cursor.drop_vertex = false;
    for (const VertexLoaderStep& step : m_steps)
      step.run(cursor, step);

    // A dropped vertex was converted like any other; the next one overwrites it.
    if (m_position_indexed && cursor.drop_vertex)
    {
      cursor.dst -= stride;
      dropped++;
    }
  }
  return count - dropped;
}
