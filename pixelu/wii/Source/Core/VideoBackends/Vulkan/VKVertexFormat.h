// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/NativeVertexFormat.h"

enum class ShaderAttrib : u32;

namespace Vulkan
{
// The Vulkan vertex input description of a vertex declaration: one binding (the vertex
// buffer) and one attribute per enabled component, at the shader's attribute locations.
class VertexFormat : public ::NativeVertexFormat
{
public:
  VertexFormat(const PortableVertexDeclaration& vtx_decl);

  // For pipeline creation; points into this object.
  const VkPipelineVertexInputStateCreateInfo& GetVertexInputStateInfo() const;

  // Fill the attribute list, then the input state that refers to it.
  void MapAttributes();
  void SetupInputState();

private:
  void AddAttribute(ShaderAttrib location, uint32_t binding, VkFormat format, uint32_t offset);

  VkVertexInputBindingDescription m_binding = {};
  std::array<VkVertexInputAttributeDescription, MAX_VERTEX_ATTRIBUTES> m_attributes = {};
  VkPipelineVertexInputStateCreateInfo m_input_state = {};
  uint32_t m_attribute_count = 0;
};
}  // namespace Vulkan
