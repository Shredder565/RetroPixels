// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstring>

#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/UberShaderPixel.h"
#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/VertexShaderGen.h"

class NativeVertexFormat;

namespace VideoCommon
{
// Version of the pipeline UID layout below (and of the shader UIDs and render states inside
// it). The on-disk UID caches carry it; bumping it discards every cached UID.
constexpr u32 GX_PIPELINE_UID_VERSION = 9;

// Key of a GX pipeline in the shader cache. Compared bytewise because this lookup happens on
// every draw; the constructors zero the whole object so padding compares equal too.
struct GXPipelineUid
{
  const NativeVertexFormat* vertex_format;
  VertexShaderUid vs_uid;
  GeometryShaderUid gs_uid;
  PixelShaderUid ps_uid;
  RasterizationState rasterization_state;
  DepthState depth_state;
  BlendingState blending_state;

  GXPipelineUid() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
  bool operator<(const GXPipelineUid& rhs) const
  {
    return std::memcmp(this, &rhs, sizeof(*this)) < 0;
  }
  bool operator==(const GXPipelineUid& rhs) const
  {
    return std::memcmp(this, &rhs, sizeof(*this)) == 0;
  }
};

// The same for ubershader pipelines.
struct GXUberPipelineUid
{
  const NativeVertexFormat* vertex_format;
  UberShader::VertexShaderUid vs_uid;
  GeometryShaderUid gs_uid;
  UberShader::PixelShaderUid ps_uid;
  RasterizationState rasterization_state;
  DepthState depth_state;
  BlendingState blending_state;

  GXUberPipelineUid() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
  bool operator<(const GXUberPipelineUid& rhs) const
  {
    return std::memcmp(this, &rhs, sizeof(*this)) < 0;
  }
  bool operator==(const GXUberPipelineUid& rhs) const
  {
    return std::memcmp(this, &rhs, sizeof(*this)) == 0;
  }
};

// On-disk forms of the UIDs above: the vertex format pointer is replaced by its portable
// declaration and the render states by their raw bits. Packed, so the file layout does not
// depend on the compiler.
#pragma pack(push, 1)
struct SerializedGXPipelineUid
{
  PortableVertexDeclaration vertex_decl{};
  VertexShaderUid vs_uid;
  GeometryShaderUid gs_uid;
  PixelShaderUid ps_uid;
  u32 rasterization_state_bits = 0;
  u32 depth_state_bits = 0;
  u32 blending_state_bits = 0;
};
struct SerializedGXUberPipelineUid
{
  PortableVertexDeclaration vertex_decl{};
  UberShader::VertexShaderUid vs_uid;
  GeometryShaderUid gs_uid;
  UberShader::PixelShaderUid ps_uid;
  u32 rasterization_state_bits = 0;
  u32 depth_state_bits = 0;
  u32 blending_state_bits = 0;
};
#pragma pack(pop)

}  // namespace VideoCommon
