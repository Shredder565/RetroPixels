// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

namespace Vulkan
{
// Size of the ring of command buffers the GPU can be working through at once.
constexpr size_t NUM_COMMAND_BUFFERS = 8;

// Frames that may be in flight; each has its own set of descriptor pools.
constexpr size_t NUM_FRAMES_IN_FLIGHT = 2;

// Whether a staging buffer is written by the CPU (upload) or read by it (readback).
enum STAGING_BUFFER_TYPE
{
  STAGING_BUFFER_TYPE_UPLOAD,
  STAGING_BUFFER_TYPE_READBACK
};

// The descriptor set layouts created by the object cache, in creation order.
enum DESCRIPTOR_SET_LAYOUT
{
  DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS,
  DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS,
  DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS,
  DESCRIPTOR_SET_LAYOUT_UTILITY_UNIFORM_BUFFER,
  DESCRIPTOR_SET_LAYOUT_UTILITY_SAMPLERS,
  DESCRIPTOR_SET_LAYOUT_COMPUTE,
  NUM_DESCRIPTOR_SET_LAYOUTS
};

// Pipeline layouts and the descriptor sets bound with them:
//
//   STANDARD  set 0: dynamic UBOs (binding 0 PS, 1 VS, 2 custom PS, 3 GS)
//             set 1: pixel samplers (an array of 8 at binding 0, then bindings 8..15)
//             set 2: bounding-box SSBO (binding 0), only when something needs it
//   UBER      as STANDARD, plus the dynamic vertex loader's SSBO at set 2 binding 1
//   UTILITY   set 0: one dynamic UBO for every stage
//             set 1: 8 samplers (bindings 0..7) and a texel buffer (binding 8)
//   COMPUTE   set 0: UBO (0), 8 samplers (1..8), 2 texel buffers (9..10), 8 images (11..18)
enum PIPELINE_LAYOUT
{
  PIPELINE_LAYOUT_STANDARD,
  PIPELINE_LAYOUT_UBER,
  PIPELINE_LAYOUT_UTILITY,
  PIPELINE_LAYOUT_COMPUTE,
  NUM_PIPELINE_LAYOUTS
};

// Binding numbers of the GX uniform buffers inside the first descriptor set.
enum UNIFORM_BUFFER_DESCRIPTOR_SET_BINDING
{
  UBO_DESCRIPTOR_SET_BINDING_PS,
  UBO_DESCRIPTOR_SET_BINDING_VS,
  UBO_DESCRIPTOR_SET_BINDING_CUST,
  UBO_DESCRIPTOR_SET_BINDING_GS,
  NUM_UBO_DESCRIPTOR_SET_BINDINGS
};

// Upper bound on vertex attributes in one native vertex format.
constexpr u32 MAX_VERTEX_ATTRIBUTES = 16;

// Samplers visible to utility (non-GX) pixel shaders.
constexpr u32 NUM_UTILITY_PIXEL_SAMPLERS = 8;

// Texel buffers visible to compute shaders.
constexpr u32 NUM_COMPUTE_TEXEL_BUFFERS = 2;

// Size of the shared stream buffer used for texture uploads.
constexpr u32 TEXTURE_UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;

// Uploads larger than this (4 MiB, a 1024x1024 RGBA8 level) get their own temporary staging
// buffer instead of a slice of the stream buffer, so a few large HD textures cannot keep the
// shared buffer full and force waits.
constexpr u32 STAGING_TEXTURE_UPLOAD_THRESHOLD = 1024 * 1024 * 4;
}  // namespace Vulkan
