// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;
enum class AlphaTestOp : u32;
enum class AlphaTestResult;
enum class CompareMode : u32;
enum class DstBlendFactor : u32;
enum class EmulatedZ : u32;
enum class FogProjection : u32;
enum class FogType : u32;
enum class KonstSel : u32;
enum class RasColorChan : u32;
enum class SrcBlendFactor : u32;
enum class ZTexOp : u32;

// Key of a specialised TEV pixel shader. The struct is packed and stored byte for byte in the disk
// shader caches, so its layout must not change. Only the first num_values bytes are meaningful:
// unused TEV stages at the end are left out unless per-pixel lighting needs the lighting block.
#pragma pack(1)
struct pixel_shader_uid_data
{
  u32 num_values;
  u32 NumValues() const { return num_values; }

  u32 pad0 : 4;
  u32 useDstAlpha : 1;
  u32 no_dual_src : 1;
  AlphaTestResult Pretest : 2;
  u32 nIndirectStagesUsed : 4;  // bit per indirect stage read by an active TEV stage
  u32 genMode_numtexgens : 4;
  u32 genMode_numtevstages : 4;  // stage count minus one
  u32 genMode_numindstages : 3;
  CompareMode alpha_test_comp0 : 3;
  CompareMode alpha_test_comp1 : 3;
  AlphaTestOp alpha_test_logic : 2;
  FogProjection fog_proj : 1;

  FogType fog_fsel : 3;
  u32 fog_RangeBaseEnabled : 1;
  ZTexOp ztex_op : 2;
  u32 per_pixel_depth : 1;
  EmulatedZ ztest : 3;
  u32 bounding_box : 1;
  u32 zfreeze : 1;
  u32 numColorChans : 2;
  u32 rgba6_format : 1;
  u32 dither : 1;
  u32 uint_output : 1;
  // Blending done in the shader through framebuffer fetch:
  u32 blend_enable : 1;
  SrcBlendFactor blend_src_factor : 3;
  SrcBlendFactor blend_src_factor_alpha : 3;
  DstBlendFactor blend_dst_factor : 3;
  DstBlendFactor blend_dst_factor_alpha : 3;
  u32 blend_subtract : 1;
  u32 blend_subtract_alpha : 1;
  // Logic ops, either approximated with blending or done with framebuffer fetch:
  u32 emulate_logic_op_with_blend : 1;
  u32 logic_op_enable : 1;
  u32 logic_op_mode : 4;

  u32 texMtxInfo_n_projection : 8;  // projection (ST/STQ) bit per texgen
  // Indirect stage n reads texture coordinate bc<n> from texture map bi<n>.
  u32 tevindref_bi0 : 3;
  u32 tevindref_bc0 : 3;
  u32 tevindref_bi1 : 3;
  u32 tevindref_bc1 : 3;
  u32 tevindref_bi2 : 3;
  u32 tevindref_bc2 : 3;
  u32 tevindref_bi3 : 3;
  u32 tevindref_bc3 : 3;

  // Indices outside 0-3 are ignored (and read back as 0).
  void SetTevindrefValues(int index, u32 texcoord, u32 texmap)
  {
    switch (index)
    {
    case 0:
      tevindref_bc0 = texcoord;
      tevindref_bi0 = texmap;
      break;
    case 1:
      tevindref_bc1 = texcoord;
      tevindref_bi1 = texmap;
      break;
    case 2:
      tevindref_bc2 = texcoord;
      tevindref_bi2 = texmap;
      break;
    case 3:
      tevindref_bc3 = texcoord;
      tevindref_bi3 = texmap;
      break;
    default:
      break;
    }
  }

  u32 GetTevindirefCoord(int index) const
  {
    switch (index)
    {
    case 0:
      return tevindref_bc0;
    case 1:
      return tevindref_bc1;
    case 2:
      return tevindref_bc2;
    case 3:
      return tevindref_bc3;
    default:
      return 0;
    }
  }

  u32 GetTevindirefMap(int index) const
  {
    switch (index)
    {
    case 0:
      return tevindref_bi0;
    case 1:
      return tevindref_bi1;
    case 2:
      return tevindref_bi2;
    case 3:
      return tevindref_bi3;
    default:
      return 0;
    }
  }

  // One entry per TEV stage.
  struct
  {
    u32 cc : 24;  // colour combiner register
    u32 ac : 24;  // alpha combiner register without rswap/tswap (decoded into the swaps below)

    u32 tevorders_texmap : 3;
    u32 tevorders_texcoord : 3;
    u32 tevorders_enable : 1;
    RasColorChan tevorders_colorchan : 3;
    u32 pad1 : 7;

    u32 tevind : 21;
    ColorChannel ras_swap_r : 2;
    ColorChannel ras_swap_g : 2;
    ColorChannel ras_swap_b : 2;
    ColorChannel ras_swap_a : 2;
    u32 pad2 : 2;

    ColorChannel tex_swap_r : 2;
    ColorChannel tex_swap_g : 2;
    ColorChannel tex_swap_b : 2;
    ColorChannel tex_swap_a : 2;
    KonstSel tevksel_kc : 5;
    KonstSel tevksel_ka : 5;
    u32 pad3 : 14;
  } stagehash[16];

  LightingUidData lighting;  // only filled in with per-pixel lighting
};
#pragma pack()

using PixelShaderUid = ShaderUid<pixel_shader_uid_data>;

// Graphics-mod replacement for process_fragment() and its uniform block.
struct CustomPixelContents
{
  std::string_view shader = "";
  std::string_view uniforms = "";
};

ShaderCode GeneratePixelShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                   const pixel_shader_uid_data* uid_data,
                                   CustomPixelContents custom_contents);
// Helpers, uniforms and texture sampling shared with the pixel ubershader.
void WritePixelShaderCommonHeader(ShaderCode& out, APIType api_type,
                                  const ShaderHostConfig& host_config, bool bounding_box);
// The body of dolphin_process_emulated_fragment(): lighting, indirect lookups and TEV stages.
void WriteFragmentBody(APIType api_type, const ShaderHostConfig& host_config,
                       const pixel_shader_uid_data* uid_data, ShaderCode& out);
// Drops key bits that the given backend cannot use, so equivalent shaders share one key.
void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid);
PixelShaderUid GetPixelShaderUid();
