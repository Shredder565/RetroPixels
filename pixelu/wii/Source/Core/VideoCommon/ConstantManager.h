// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// Uniform-block mirrors of the PSBlock, VSBlock and GSBlock declarations written by the shader
// generators. std140 packs everything in 16-byte rows, so members are 4-component vectors or
// scalars that fill a row together (the comments name the component each scalar lands in).
// The layouts are uploaded verbatim and are part of save states; never reorder them.
using float4 = std::array<float, 4>;
using uint4 = std::array<u32, 4>;
using int4 = std::array<s32, 4>;

enum class DstBlendFactor : u32;
enum class SrcBlendFactor : u32;
enum class ZTexOp : u32;
enum class LogicOp : u32;

struct alignas(16) PixelShaderConstants
{
  std::array<int4, 4> colors;   // TEV colour registers
  std::array<int4, 4> kcolors;  // TEV konstant registers
  int4 alpha;                   // alpha test refs (.xy), destination alpha (.w)
  std::array<uint4, 8> texdims;
  std::array<int4, 2> zbias;
  std::array<int4, 2> indtexscale;
  std::array<int4, 6> indtexmtx;
  int4 fogcolor;
  int4 fogi;
  float4 fogf;
  std::array<float4, 3> fogrange;
  float4 zslope;
  std::array<float, 2> efbscale;  // .xy

  // Everything below is read by the ubershaders only (pack2 also by the specialised shaders).
  u32 genmode;                  // .z
  u32 alphaTest;                // .w
  u32 fogParam3;                // .x
  u32 fogRangeBase;             // .y
  u32 dstalpha;                 // .z
  ZTexOp ztex_op;               // .w
  u32 late_ztest;               // .x, 0 or 1
  u32 rgba6_format;             // .y, 0 or 1
  u32 dither;                   // .z, 0 or 1
  u32 bounding_box;             // .w, 0 or 1
  std::array<uint4, 16> pack1;  // per TEV stage: colour/alpha combiner, tevind, indirect ref
  std::array<uint4, 8> pack2;   // tevorder, tevksel, sampler words tm0 and tm1
  std::array<int4, 32> konst;   // every konstant selection, pre-swizzled
  // Blend state for blending in the shader (framebuffer fetch).
  u32 blend_enable;
  SrcBlendFactor blend_src_factor;
  SrcBlendFactor blend_src_factor_alpha;
  DstBlendFactor blend_dst_factor;
  DstBlendFactor blend_dst_factor_alpha;
  u32 blend_subtract;
  u32 blend_subtract_alpha;
  // Logic op emulated in the shader (framebuffer fetch).
  u32 logic_op_enable;  // 0 or 1
  LogicOp logic_op_mode;
  // Milliseconds, for graphics-mod shaders.
  u32 time_ms;
};

struct alignas(16) VertexShaderConstants
{
  u32 components;           // .x
  u32 xfmem_dualTexInfo;    // .y
  u32 xfmem_numColorChans;  // .z
  u32 missing_color_hex;    // .w, only compared on the CPU to notice config changes
  float4 missing_color_value;

  std::array<float4, 6> posnormalmatrix;
  std::array<float4, 4> projection;
  std::array<int4, 4> materials;
  struct Light
  {
    int4 color;
    float4 cosatt;
    float4 distatt;
    float4 pos;
    float4 dir;
  };
  std::array<Light, 8> lights;
  std::array<float4, 24> texmatrices;
  std::array<float4, 64> transformmatrices;
  std::array<float4, 32> normalmatrices;
  std::array<float4, 64> posttransformmatrices;
  float4 pixelcentercorrection;
  std::array<float, 2> viewport;  // .xy
  std::array<float, 2> pad2;      // .zw

  // Per texgen: .x texMtxInfo, .y postMtxInfo. Rows 0 and 1 also hold colour (.z) and alpha (.w)
  // channel control.
  std::array<uint4, 8> xfmem_pack1;

  float4 cached_normal;
  float4 cached_tangent;
  float4 cached_binormal;
  // Vertex buffer layout for the ubershader's manual vertex fetch.
  u32 vertex_stride;
  std::array<u32, 3> vertex_offset_normals;
  u32 vertex_offset_position;
  u32 vertex_offset_posmtx;
  std::array<u32, 2> vertex_offset_colors;
  std::array<u32, 8> vertex_offset_texcoords;
};

enum class VSExpand : u32
{
  None = 0,
  Point,
  Line,
};

struct alignas(16) GeometryShaderConstants
{
  float4 stereoparams;
  float4 lineptparams;
  int4 texoffset;
  VSExpand vs_expand;  // point/line expansion done by the vertex ubershader
  u32 pad[3];
};
