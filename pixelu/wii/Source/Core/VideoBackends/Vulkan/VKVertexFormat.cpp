// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKVertexFormat.h"

#include <algorithm>

#include "Common/Assert.h"

#include "VideoBackends/Vulkan/ObjectCache.h"

#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderGen.h"

namespace Vulkan
{
namespace
{
// [component type][integer?][component count - 1]. Floats (and the invalid float encodings
// 5..7, clamped to row 4) are read as floats even when marked integer.
constexpr VkFormat ATTRIBUTE_FORMATS[5][2][4] = {
    // UByte
    {{VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
     {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT}},
    // Byte
    {{VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
     {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT}},
    // UShort
    {{VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM,
      VK_FORMAT_R16G16B16A16_UNORM},
     {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT,
      VK_FORMAT_R16G16B16A16_UINT}},
    // Short
    {{VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM,
      VK_FORMAT_R16G16B16A16_SNORM},
     {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT,
      VK_FORMAT_R16G16B16A16_SINT}},
    // Float
    {{VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_R32G32B32A32_SFLOAT},
     {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_R32G32B32A32_SFLOAT}},
};

VkFormat AttributeFormat(const ::AttributeFormat& attr)
{
  ASSERT(attr.components > 0 && attr.components <= 4);
  const u32 row = std::min(static_cast<u32>(attr.type), 4u);
  return ATTRIBUTE_FORMATS[row][attr.integer ? 1 : 0][attr.components - 1];
}
}  // namespace

VertexFormat::VertexFormat(const PortableVertexDeclaration& vtx_decl) : NativeVertexFormat(vtx_decl)
{
  MapAttributes();
  SetupInputState();
}

const VkPipelineVertexInputStateCreateInfo& VertexFormat::GetVertexInputStateInfo() const
{
  return m_input_state;
}

void VertexFormat::MapAttributes()
{
  m_attribute_count = 0;
  // All attributes come from binding 0, in shader location order of the declaration.
  const auto add = [this](ShaderAttrib location, const ::AttributeFormat& attr) {
    if (attr.enable)
      AddAttribute(location, 0, AttributeFormat(attr), static_cast<uint32_t>(attr.offset));
  };

  add(ShaderAttrib::Position, m_decl.position);
  for (size_t n = 0; n < 3; ++n)
    add(ShaderAttrib::Normal + static_cast<int>(n), m_decl.normals[n]);
  for (size_t n = 0; n < 2; ++n)
    add(ShaderAttrib::Color0 + static_cast<int>(n), m_decl.colors[n]);
  for (size_t n = 0; n < 8; ++n)
    add(ShaderAttrib::TexCoord0 + static_cast<int>(n), m_decl.texcoords[n]);
  add(ShaderAttrib::PositionMatrix, m_decl.posmtx);
}

void VertexFormat::SetupInputState()
{
  m_binding = {0, static_cast<uint32_t>(m_decl.stride), VK_VERTEX_INPUT_RATE_VERTEX};

  m_input_state = {};
  m_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  m_input_state.vertexBindingDescriptionCount = 1;
  m_input_state.pVertexBindingDescriptions = &m_binding;
  m_input_state.vertexAttributeDescriptionCount = m_attribute_count;
  m_input_state.pVertexAttributeDescriptions = m_attributes.data();
}

void VertexFormat::AddAttribute(ShaderAttrib location, uint32_t binding, VkFormat format,
                                uint32_t offset)
{
  ASSERT(m_attribute_count < MAX_VERTEX_ATTRIBUTES);
  m_attributes[m_attribute_count++] = {static_cast<uint32_t>(location), binding, format, offset};
}
}  // namespace Vulkan
