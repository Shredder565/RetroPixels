// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DX12VertexFormat.h"

#include "Common/Assert.h"

#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderGen.h"

namespace DX12
{
namespace
{
// Element formats for the four packed component types (UByte, Byte, UShort,
// Short). A three-component vector of these is fetched as four components,
// since DXGI has no three-component 8/16-bit formats.
struct PackedFormats
{
  DXGI_FORMAT normalized[4];
  DXGI_FORMAT integer[4];
};

constexpr PackedFormats PACKED_1 = {
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
    {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT}};
constexpr PackedFormats PACKED_2 = {{DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM,
                                     DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
                                    {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT,
                                     DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT}};
constexpr PackedFormats PACKED_4 = {{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
                                     DXGI_FORMAT_R16G16B16A16_UNORM,
                                     DXGI_FORMAT_R16G16B16A16_SNORM},
                                    {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT,
                                     DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT}};

// Float and the unused float codes always read as 32-bit floats, integer
// flag or not.
constexpr DXGI_FORMAT FLOATS[4] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                                   DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};

DXGI_FORMAT ElementFormat(ComponentFormat type, int components, bool integer)
{
  ASSERT(components > 0 && components <= 4);
  const auto count = static_cast<u32>(components);
  const auto type_index = static_cast<u32>(type);
  if (type_index >= 4)
    return FLOATS[count - 1];

  const PackedFormats& packed = count == 1 ? PACKED_1 : (count == 2 ? PACKED_2 : PACKED_4);
  return integer ? packed.integer[type_index] : packed.normalized[type_index];
}
}  // namespace

DXVertexFormat::DXVertexFormat(const PortableVertexDeclaration& vtx_decl)
    : NativeVertexFormat(vtx_decl)
{
  // Elements follow the shader attribute order; each is a TEXCOORD whose
  // semantic index is the attribute number.
  AddAttribute(m_decl.position, ShaderAttrib::Position);
  for (u32 n = 0; n < 3; ++n)
    AddAttribute(m_decl.normals[n], ShaderAttrib::Normal + static_cast<int>(n));
  for (u32 n = 0; n < 2; ++n)
    AddAttribute(m_decl.colors[n], ShaderAttrib::Color0 + static_cast<int>(n));
  for (u32 n = 0; n < 8; ++n)
    AddAttribute(m_decl.texcoords[n], ShaderAttrib::TexCoord0 + static_cast<int>(n));
  AddAttribute(m_decl.posmtx, ShaderAttrib::PositionMatrix);
}

void DXVertexFormat::GetInputLayoutDesc(D3D12_INPUT_LAYOUT_DESC* desc) const
{
  desc->pInputElementDescs = m_attribute_descriptions.data();
  desc->NumElements = m_num_attributes;
}

void DXVertexFormat::AddAttribute(const AttributeFormat& format, ShaderAttrib attribute)
{
  if (!format.enable)
    return;

  ASSERT(m_num_attributes < MAX_VERTEX_ATTRIBUTES);
  D3D12_INPUT_ELEMENT_DESC& element = m_attribute_descriptions[m_num_attributes++];
  element.SemanticName = "TEXCOORD";
  element.SemanticIndex = static_cast<u32>(attribute);
  element.Format = ElementFormat(format.type, format.components, format.integer);
  element.InputSlot = 0;
  element.AlignedByteOffset = static_cast<u32>(format.offset);
  element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  element.InstanceDataStepRate = 0;
}

}  // namespace DX12
