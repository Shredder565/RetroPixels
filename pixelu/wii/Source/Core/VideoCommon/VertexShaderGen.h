// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"

#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;
enum class TexInputForm : u8;
enum class TexGenType : u8;
enum class SourceRow : u8;
enum class TexSize : u8;
enum class VSExpand : u32;

// Vertex attribute slots shared by every backend (input locations / D3D TEXCOORD indices).
// Slot 7 is unused.
enum class ShaderAttrib : u32
{
  Position = 0,
  PositionMatrix = 1,
  Normal = 2,
  Tangent = 3,
  Binormal = 4,
  Color0 = 5,
  Color1 = 6,

  TexCoord0 = 8,
  TexCoord1 = 9,
  TexCoord2 = 10,
  TexCoord3 = 11,
  TexCoord4 = 12,
  TexCoord5 = 13,
  TexCoord6 = 14,
  TexCoord7 = 15
};

template <>
struct fmt::formatter<ShaderAttrib> : EnumFormatter<ShaderAttrib::TexCoord7>
{
  static constexpr array_type names = {
      "Position",    "Position Matrix", "Normal",      "Tangent",     "Binormal",    "Color 0",
      "Color 1",     nullptr,           "Tex Coord 0", "Tex Coord 1", "Tex Coord 2", "Tex Coord 3",
      "Tex Coord 4", "Tex Coord 5",     "Tex Coord 6", "Tex Coord 7"};
  constexpr formatter() : EnumFormatter(names) {}
};

// Steps from Color0 or TexCoord0 to the n-th colour or texture coordinate slot.
constexpr ShaderAttrib operator+(ShaderAttrib attrib, int offset)
{
  return static_cast<ShaderAttrib>(static_cast<u8>(attrib) + offset);
}

// Specialised vertex shader key, 28 bytes. The layout is part of the disk shader caches.
struct alignas(4) vertex_shader_uid_data
{
  u32 NumValues() const { return sizeof(vertex_shader_uid_data); }

  u32 components : 14;             // VB_HAS_* bits of the vertex format that affect the shader
  u32 position_has_3_elems : 1;    // only used with VS line/point expansion
  u32 dualTexTrans_enabled : 1;    // post-transform matrices (only with a regular texgen)
  u32 numTexGens : 4;              // 0-8
  u32 numColorChans : 2;           // colour channels passed on to TEV
  VSExpand vs_expand : 2;          // point/line expansion in the vertex shader

  u32 pad : 8;

  // Per-texgen state packed into 16 bits; is_regular_texgen selects the union member.
  struct
  {
    u8 texcoord_elem_count : 2;  // components of the matching vertex texcoord (0 = absent)
    TexInputForm inputform : 1;
    SourceRow sourcerow : 4;
    bool is_regular_texgen : 1;
    union
    {
      struct
      {
        u8 postmtx_index : 6;
        u8 postmtx_normalize : 1;
        TexSize projection : 1;
      } regular;
      struct
      {
        TexGenType texgentype : 2;
        u8 emboss_sourceshift : 3;  // emboss only
        u8 emboss_lightshift : 3;   // emboss only
      } other;
    };
  } texGenInfo[8];

  static_assert(sizeof(texGenInfo[0]) == 2, "texGenInfo should be 2 bytes per texgen");

  LightingUidData lighting;
};

static_assert(offsetof(vertex_shader_uid_data, lighting) % alignof(LightingUidData) == 0);
static_assert(offsetof(vertex_shader_uid_data, texGenInfo) % alignof(u32) == 0);
static_assert(sizeof(vertex_shader_uid_data) == 28, "vertex_shader_uid_data should be 28 bytes");

using VertexShaderUid = ShaderUid<vertex_shader_uid_data>;

// Graphics-mod replacement for process_vertex() and its uniform block.
struct CustomVertexContents
{
  std::string_view shader = "";
  std::string_view uniforms = "";
};

// Builds the key from the current XF state and vertex format.
VertexShaderUid GetVertexShaderUid();
ShaderCode GenerateVertexShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                    const vertex_shader_uid_data* uid_data,
                                    CustomVertexContents custom_contents);
// The body of dolphin_process_emulated_vertex(): transform, lighting and texgen.
void WriteVertexBody(APIType api_type, const ShaderHostConfig& host_config,
                     const vertex_shader_uid_data* uid_data, ShaderCode& out);
