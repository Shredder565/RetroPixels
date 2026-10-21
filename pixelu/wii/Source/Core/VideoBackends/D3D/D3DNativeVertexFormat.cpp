// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include <array>

#include "Common/Assert.h"
#include "Common/EnumMap.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoCommon/NativeVertexFormat.h"

namespace DX11
{
std::mutex s_input_layout_lock;

std::unique_ptr<NativeVertexFormat>
Gfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<D3DVertexFormat>(vtx_decl);
}

namespace
{
// DXGI formats for one component count. Index 0-3 follows ComponentFormat
// (UByte, Byte, UShort, Short); every float-like type uses the 32-bit float.
struct ComponentFormats
{
  std::array<DXGI_FORMAT, 4> normalized;
  std::array<DXGI_FORMAT, 4> integer;
  DXGI_FORMAT floating;
};

// By component count minus one. Three-component vectors exist only as floats.
constexpr std::array<ComponentFormats, 4> VERTEX_FORMATS = {{
    {{DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
     {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT},
     DXGI_FORMAT_R32_FLOAT},
    {{DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R16G16_UNORM,
      DXGI_FORMAT_R16G16_SNORM},
     {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R16G16_UINT,
      DXGI_FORMAT_R16G16_SINT},
     DXGI_FORMAT_R32G32_FLOAT},
    {{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN},
     {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN},
     DXGI_FORMAT_R32G32B32_FLOAT},
    {{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R16G16B16A16_UNORM,
      DXGI_FORMAT_R16G16B16A16_SNORM},
     {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R16G16B16A16_UINT,
      DXGI_FORMAT_R16G16B16A16_SINT},
     DXGI_FORMAT_R32G32B32A32_FLOAT},
}};

DXGI_FORMAT AttributeFormatFor(ComponentFormat type, int components, bool integer)
{
  const ComponentFormats& row = VERTEX_FORMATS[static_cast<size_t>(components - 1)];
  const auto type_index = static_cast<u32>(type);

  DXGI_FORMAT format;
  if (type_index < 4)
    format = integer ? row.integer[type_index] : row.normalized[type_index];
  else
    format = integer ? DXGI_FORMAT_UNKNOWN : row.floating;  // no integer floats

  if (format == DXGI_FORMAT_UNKNOWN)
    PanicAlertFmt("VarToD3D: Invalid type/size combo {}, {}, {}", type, components, integer);
  return format;
}
}  // namespace

D3DVertexFormat::D3DVertexFormat(const PortableVertexDeclaration& vtx_decl)
    : NativeVertexFormat(vtx_decl)
{
  // Every attribute is a TEXCOORD with the shader attribute as its index.
  AddAttribute(vtx_decl.position, ShaderAttrib::Position);
  for (u32 n = 0; n < 3; ++n)
    AddAttribute(vtx_decl.normals[n], ShaderAttrib::Normal + static_cast<int>(n));
  for (u32 n = 0; n < 2; ++n)
    AddAttribute(vtx_decl.colors[n], ShaderAttrib::Color0 + static_cast<int>(n));
  for (u32 n = 0; n < 8; ++n)
    AddAttribute(vtx_decl.texcoords[n], ShaderAttrib::TexCoord0 + static_cast<int>(n));
  AddAttribute(vtx_decl.posmtx, ShaderAttrib::PositionMatrix);
}

D3DVertexFormat::~D3DVertexFormat()
{
  if (ID3D11InputLayout* const layout = m_layout.load())
    layout->Release();
}

ID3D11InputLayout* D3DVertexFormat::GetInputLayout(const void* vs_bytecode, size_t vs_bytecode_size)
{
  // The layout is checked only against the shader's input signature, which is
  // the same for every vertex shader, so one layout serves all of them.
  if (ID3D11InputLayout* const cached = m_layout.load())
    return cached;

  ID3D11InputLayout* created = nullptr;
  const HRESULT result = D3D::device->CreateInputLayout(m_elems.data(), m_num_elems, vs_bytecode,
                                                        vs_bytecode_size, &created);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create input layout: {}", DX11HRWrap(result));

  // Pipelines can be compiled on several threads at once. The first layout
  // stored wins; a loser drops its own and returns the winner's.
  ID3D11InputLayout* stored = nullptr;
  if (m_layout.compare_exchange_strong(stored, created))
    return created;

  if (created)
    created->Release();
  return stored;
}

void D3DVertexFormat::AddAttribute(const AttributeFormat& format, ShaderAttrib semantic_index)
{
  if (!format.enable)
    return;

  D3D11_INPUT_ELEMENT_DESC& element = m_elems[m_num_elems++];
  element.SemanticName = "TEXCOORD";
  element.SemanticIndex = static_cast<u32>(semantic_index);
  element.AlignedByteOffset = static_cast<UINT>(format.offset);
  element.Format = AttributeFormatFor(format.type, format.components, format.integer);
  element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
}

}  // namespace DX11
