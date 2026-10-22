// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <d3d12.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/NativeVertexFormat.h"

enum class ShaderAttrib : u32;

namespace DX12
{
// The D3D12 input layout for a portable vertex declaration. Every attribute
// comes from vertex buffer slot 0.
class DXVertexFormat : public NativeVertexFormat
{
public:
  static const u32 MAX_VERTEX_ATTRIBUTES = 16;

  DXVertexFormat(const PortableVertexDeclaration& vtx_decl);

  // Fills the layout part of a pipeline description; the elements stay owned
  // by this object.
  void GetInputLayoutDesc(D3D12_INPUT_LAYOUT_DESC* desc) const;

private:
  void AddAttribute(const AttributeFormat& format, ShaderAttrib attribute);

  std::array<D3D12_INPUT_ELEMENT_DESC, MAX_VERTEX_ATTRIBUTES> m_attribute_descriptions = {};
  u32 m_num_attributes = 0;
};
}  // namespace DX12
