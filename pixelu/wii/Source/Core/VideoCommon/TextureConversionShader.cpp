// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TextureConversionShader.h"

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

// Two families of shaders live here:
//  * EFB-to-RAM encoders: fragment shaders that read the EFB copy source and write texels laid out
//    exactly as the console's tiled texture formats, so the result can be read back into RAM.
//  * Texture decoders: compute shaders that turn raw tiled texture data (from a texel buffer) into
//    RGBA8 images, plus a fragment shader that applies a palette to an index texture.
// The GLSL they produce feeds shader caches and must not change, so the text is kept verbatim.

namespace TextureConversionShaderTiled
{
namespace
{
// ---------------------------------------------------------------------------------------------
// GLSL text of the compute decoders and the palette shader. This is shader source that the
// emulator outputs (and that existing shader caches are keyed on), kept exactly as before.
// ---------------------------------------------------------------------------------------------

constexpr char kComputeCommon[] = R"glsl(
#if defined(PALETTE_FORMAT_IA8) || defined(PALETTE_FORMAT_RGB565) || defined(PALETTE_FORMAT_RGB5A3)
#define HAS_PALETTE 1
#endif

UBO_BINDING(std140, 1) uniform UBO {
  uint2 u_dst_size;
  uint2 u_src_size;
  uint u_src_offset;
  uint u_src_row_stride;
  uint u_palette_offset;
};

#if defined(API_METAL)

#if defined(TEXEL_BUFFER_FORMAT_R8)
  SSBO_BINDING(0) readonly buffer Input { uint8_t s_input_buffer[]; };
  #define FETCH(offset) uint(s_input_buffer[offset])
#elif defined(TEXEL_BUFFER_FORMAT_R16)
  SSBO_BINDING(0) readonly buffer Input { uint16_t s_input_buffer[]; };
  #define FETCH(offset) uint(s_input_buffer[offset])
#elif defined(TEXEL_BUFFER_FORMAT_RGBA8)
  SSBO_BINDING(0) readonly buffer Input { u8vec4 s_input_buffer[]; };
  #define FETCH(offset) uvec4(s_input_buffer[offset])
#elif defined(TEXEL_BUFFER_FORMAT_R32G32)
  SSBO_BINDING(0) readonly buffer Input { uvec2 s_input_buffer[]; };
  #define FETCH(offset) s_input_buffer[offset]
#else
  #error No texel buffer?
#endif

#ifdef HAS_PALETTE
  SSBO_BINDING(1) readonly buffer Palette { uint16_t s_palette_buffer[]; };
  #define FETCH_PALETTE(offset) uint(s_palette_buffer[offset])
#endif

#else

TEXEL_BUFFER_BINDING(0) uniform usamplerBuffer s_input_buffer;

#if defined(TEXEL_BUFFER_FORMAT_R8) || defined(TEXEL_BUFFER_FORMAT_R16)
  #define FETCH(offset) texelFetch(s_input_buffer, int((offset) + u_src_offset)).r
#elif defined(TEXEL_BUFFER_FORMAT_RGBA8)
  #define FETCH(offset) texelFetch(s_input_buffer, int((offset) + u_src_offset))
#elif defined(TEXEL_BUFFER_FORMAT_R32G32)
  #define FETCH(offset) texelFetch(s_input_buffer, int((offset) + u_src_offset)).rg
#else
  #error No texel buffer?
#endif

#ifdef HAS_PALETTE
  TEXEL_BUFFER_BINDING(1) uniform usamplerBuffer s_palette_buffer;
  #define FETCH_PALETTE(offset) texelFetch(s_palette_buffer, int((offset) + u_palette_offset)).r
#endif

#endif // defined(API_METAL)
IMAGE_BINDING(rgba8, 0) uniform writeonly image2DArray output_image;

#define GROUP_MEMORY_BARRIER_WITH_SYNC memoryBarrierShared(); barrier();
#define GROUP_SHARED shared

#define DEFINE_MAIN(lx, ly) \
  layout(local_size_x = lx, local_size_y = ly) in; \
  void main()

uint Swap16(uint v)
{
  // Convert BE to LE.
  return ((v >> 8) | (v << 8)) & 0xFFFFu;
}

uint Convert3To8(uint v)
{
  // Swizzle bits: 00000123 -> 12312312
  return (v << 5) | (v << 2) | (v >> 1);
}
uint Convert4To8(uint v)
{
  // Swizzle bits: 00001234 -> 12341234
  return (v << 4) | v;
}
uint Convert5To8(uint v)
{
  // Swizzle bits: 00012345 -> 12345123
  return (v << 3) | (v >> 2);
}
uint Convert6To8(uint v)
{
  // Swizzle bits: 00123456 -> 12345612
  return (v << 2) | (v >> 4);
}

uint GetTiledTexelOffset(uint2 block_size, uint2 coords)
{
  uint2 block = coords / block_size;
  uint2 offset = coords % block_size;
  uint buffer_pos = 0u;
  buffer_pos += block.y * u_src_row_stride;
  buffer_pos += block.x * (block_size.x * block_size.y);
  buffer_pos += offset.y * block_size.x;
  buffer_pos += offset.x;
  return buffer_pos;
}

#if defined(HAS_PALETTE)
uint4 GetPaletteColor(uint index)
{
  // Fetch and swap BE to LE.
  uint val = Swap16(FETCH_PALETTE(index));

  uint4 color;
#if defined(PALETTE_FORMAT_IA8)
  uint a = bitfieldExtract(val, 8, 8);
  uint i = bitfieldExtract(val, 0, 8);
  color = uint4(i, i, i, a);
#elif defined(PALETTE_FORMAT_RGB565)
  color.x = Convert5To8(bitfieldExtract(val, 11, 5));
  color.y = Convert6To8(bitfieldExtract(val, 5, 6));
  color.z = Convert5To8(bitfieldExtract(val, 0, 5));
  color.a = 255u;

#elif defined(PALETTE_FORMAT_RGB5A3)
  if ((val & 0x8000u) != 0u)
  {
    color.x = Convert5To8(bitfieldExtract(val, 10, 5));
    color.y = Convert5To8(bitfieldExtract(val, 5, 5));
    color.z = Convert5To8(bitfieldExtract(val, 0, 5));
    color.a = 255u;
  }
  else
  {
    color.a = Convert3To8(bitfieldExtract(val, 12, 3));
    color.r = Convert4To8(bitfieldExtract(val, 8, 4));
    color.g = Convert4To8(bitfieldExtract(val, 4, 4));
    color.b = Convert4To8(bitfieldExtract(val, 0, 4));
  }
#else
  // Not used.
  color = uint4(0, 0, 0, 0);
#endif

  return color;
}

float4 GetPaletteColorNormalized(uint index)
{
  uint4 color = GetPaletteColor(index);
  return float4(color) / 255.0;
}
#endif // defined(HAS_PALETTE)

)glsl";

constexpr char kComputeI4[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x8 blocks, 4 bits per pixel
        // We need to do the tiling manually here because the texel size is smaller than
        // the size of the buffer elements.
        uint2 block = coords.xy / 8u;
        uint2 offset = coords.xy % 8u;
        uint buffer_pos = 0u;
        buffer_pos += block.y * u_src_row_stride;
        buffer_pos += block.x * 32u;
        buffer_pos += offset.y * 4u;
        buffer_pos += offset.x / 2u;

        // Select high nibble for odd texels, low for even.
        uint val = FETCH(buffer_pos);
        uint i;
        if ((coords.x & 1u) == 0u)
          i = Convert4To8((val >> 4));
        else
          i = Convert4To8((val & 0x0Fu));

        uint4 color = uint4(i, i, i, i);
        float4 norm_color = float4(color) / 255.0;

        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )glsl";

constexpr char kComputeIA4[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x4 blocks, 8 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(8u, 4u), coords);
        uint val = FETCH(buffer_pos);
        uint i = Convert4To8((val & 0x0Fu));
        uint a = Convert4To8((val >> 4));
        uint4 color = uint4(i, i, i, a);
        float4 norm_color = float4(color) / 255.0;

        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )glsl";

constexpr char kComputeI8[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x4 blocks, 8 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(8u, 4u), coords);
        uint i = FETCH(buffer_pos);
        uint4 color = uint4(i, i, i, i);
        float4 norm_color = float4(color) / 255.0;

        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )glsl";

constexpr char kComputeIA8[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks, 16 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint val = FETCH(buffer_pos);
        uint a = (val & 0xFFu);
        uint i = (val >> 8);
        uint4 color = uint4(i, i, i, a);
        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )glsl";

constexpr char kComputeRGB565[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint val = Swap16(FETCH(buffer_pos));

        uint4 color;
        color.x = Convert5To8(bitfieldExtract(val, 11, 5));
        color.y = Convert6To8(bitfieldExtract(val, 5, 6));
        color.z = Convert5To8(bitfieldExtract(val, 0, 5));
        color.a = 255u;

        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )glsl";

constexpr char kComputeRGB5A3[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint val = Swap16(FETCH(buffer_pos));

        uint4 color;
        if ((val & 0x8000u) != 0u)
        {
          color.x = Convert5To8(bitfieldExtract(val, 10, 5));
          color.y = Convert5To8(bitfieldExtract(val, 5, 5));
          color.z = Convert5To8(bitfieldExtract(val, 0, 5));
          color.a = 255u;
        }
        else
        {
          color.a = Convert3To8(bitfieldExtract(val, 12, 3));
          color.r = Convert4To8(bitfieldExtract(val, 8, 4));
          color.g = Convert4To8(bitfieldExtract(val, 4, 4));
          color.b = Convert4To8(bitfieldExtract(val, 0, 4));
        }

        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )glsl";

constexpr char kComputeRGBA8[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks
        // We can't use the normal calculation function, as these are packed as the AR channels
        // for the entire block, then the GB channels afterwards.
        uint2 block = coords.xy / 4u;
        uint2 offset = coords.xy % 4u;
        uint buffer_pos = 0u;

        // Our buffer has 16-bit elements, so the offsets here are half what they would be in bytes.
        buffer_pos += block.y * u_src_row_stride;
        buffer_pos += block.x * 32u;
        buffer_pos += offset.y * 4u;
        buffer_pos += offset.x;

        // The two GB channels follow after the block's AR channels.
        uint val1 = FETCH(buffer_pos +  0u);
        uint val2 = FETCH(buffer_pos + 16u);

        uint4 color;
        color.a = (val1 & 0xFFu);
        color.r = (val1 >> 8);
        color.g = (val2 & 0xFFu);
        color.b = (val2 >> 8);

        float4 norm_color = float4(color) / 255.0;
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )glsl";

constexpr char kComputeCMPR[] = R"glsl(
      // In the compute version of this decoder, we flatten the blocks to a one-dimension array.
      // Each group is subdivided into 16, and the first thread in each group fetches the DXT data.
      // All threads then calculate the possible colors for the block and write to the output image.

      #define GROUP_SIZE 64u
      #define BLOCK_SIZE_X 4u
      #define BLOCK_SIZE_Y 4u
      #define BLOCK_SIZE (BLOCK_SIZE_X * BLOCK_SIZE_Y)
      #define BLOCKS_PER_GROUP (GROUP_SIZE / BLOCK_SIZE)

      uint DXTBlend(uint v1, uint v2)
      {
        // 3/8 blend, which is close to 1/3
        return ((v1 * 3u + v2 * 5u) >> 3);
      }

      GROUP_SHARED uint2 shared_temp[BLOCKS_PER_GROUP];

      DEFINE_MAIN(GROUP_SIZE, 1)
      {
        uint local_thread_id = gl_LocalInvocationID.x;
        uint block_in_group = local_thread_id / BLOCK_SIZE;
        uint thread_in_block = local_thread_id % BLOCK_SIZE;
        uint block_index = gl_WorkGroupID.x * BLOCKS_PER_GROUP + block_in_group;

        // Annoyingly, we can't precalculate this as a uniform because the DXT block size differs
        // from the block size of the overall texture (4 vs 8). We can however use a multiply and
        // subtraction to avoid the modulo for calculating the block's X coordinate.
        uint blocks_wide = u_src_size.x / BLOCK_SIZE_X;
        uint2 block_coords;
        block_coords.y = block_index / blocks_wide;
        block_coords.x = block_index - (block_coords.y * blocks_wide);

        // Only the first thread for each block reads from the texel buffer.
        if (thread_in_block == 0u)
        {
          // Calculate tiled block coordinates.
          uint2 tile_block_coords = block_coords / 2u;
          uint2 subtile_block_coords = block_coords % 2u;
          uint buffer_pos = 0u;
          buffer_pos += tile_block_coords.y * u_src_row_stride;
          buffer_pos += tile_block_coords.x * 4u;
          buffer_pos += subtile_block_coords.y * 2u;
          buffer_pos += subtile_block_coords.x;

          // Read the entire DXT block to shared memory.
          uint2 raw_data = FETCH(buffer_pos);
          shared_temp[block_in_group] = raw_data;
        }

        // Ensure store is completed before the remaining threads in the block continue.
        GROUP_MEMORY_BARRIER_WITH_SYNC;

        // Unpack colors and swap BE to LE.
        uint2 raw_data = shared_temp[block_in_group];
        uint swapped = ((raw_data.x & 0xFF00FF00u) >> 8) | ((raw_data.x & 0x00FF00FFu) << 8);
        uint c1 = swapped & 0xFFFFu;
        uint c2 = swapped >> 16;

        // Expand 5/6 bit channels to 8-bits per channel.
        uint blue1 = Convert5To8(bitfieldExtract(c1, 0, 5));
        uint blue2 = Convert5To8(bitfieldExtract(c2, 0, 5));
        uint green1 = Convert6To8(bitfieldExtract(c1, 5, 6));
        uint green2 = Convert6To8(bitfieldExtract(c2, 5, 6));
        uint red1 = Convert5To8(bitfieldExtract(c1, 11, 5));
        uint red2 = Convert5To8(bitfieldExtract(c2, 11, 5));

        // Determine the four colors the block can use.
        // It's quicker to just precalculate all four colors rather than branching on the index.
        // NOTE: These must be masked with 0xFF. This is done at the normalization stage below.
        uint4 color0, color1, color2, color3;
        color0 = uint4(red1, green1, blue1, 255u);
        color1 = uint4(red2, green2, blue2, 255u);
        if (c1 > c2)
        {
          color2 = uint4(DXTBlend(red2, red1), DXTBlend(green2, green1), DXTBlend(blue2, blue1), 255u);
          color3 = uint4(DXTBlend(red1, red2), DXTBlend(green1, green2), DXTBlend(blue1, blue2), 255u);
        }
        else
        {
          color2 = uint4((red1 + red2) / 2u, (green1 + green2) / 2u, (blue1 + blue2) / 2u, 255u);
          color3 = uint4((red1 + red2) / 2u, (green1 + green2) / 2u, (blue1 + blue2) / 2u, 0u);
        }

        // Calculate the texel coordinates that we will write to.
        // The divides/modulo here should be turned into a shift/binary AND.
        uint local_y = thread_in_block / BLOCK_SIZE_X;
        uint local_x = thread_in_block % BLOCK_SIZE_X;
        uint global_x = block_coords.x * BLOCK_SIZE_X + local_x;
        uint global_y = block_coords.y * BLOCK_SIZE_Y + local_y;

        // Use the coordinates within the block to shift the 32-bit value containing
        // all 16 indices to a single 2-bit index.
        uint index = bitfieldExtract(raw_data.y, int((local_y * 8u) + (6u - local_x * 2u)), 2);

        // Select the un-normalized color from the precalculated color array.
        // Using a switch statement here removes the need for dynamic indexing of an array.
        uint4 color;
        switch (index)
        {
        case 0u:  color = color0;   break;
        case 1u:  color = color1;   break;
        case 2u:  color = color2;   break;
        case 3u:  color = color3;   break;
        default:  color = color0;   break;
        }

        // Normalize and write to the output image.
        float4 norm_color = float4(color & 0xFFu) / 255.0;
        imageStore(output_image, int3(int2(uint2(global_x, global_y)), 0), norm_color);
      }
      )glsl";

constexpr char kComputeC4[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x8 blocks, 4 bits per pixel
        // We need to do the tiling manually here because the texel size is smaller than
        // the size of the buffer elements.
        uint2 block = coords.xy / 8u;
        uint2 offset = coords.xy % 8u;
        uint buffer_pos = 0u;
        buffer_pos += block.y * u_src_row_stride;
        buffer_pos += block.x * 32u;
        buffer_pos += offset.y * 4u;
        buffer_pos += offset.x / 2u;

        // Select high nibble for odd texels, low for even.
        uint val = FETCH(buffer_pos);
        uint index = ((coords.x & 1u) == 0u) ? (val >> 4) : (val & 0x0Fu);
        float4 norm_color = GetPaletteColorNormalized(index);
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }

      )glsl";

constexpr char kComputeC8[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 8x4 blocks, 8 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(8u, 4u), coords);
        uint index = FETCH(buffer_pos);
        float4 norm_color = GetPaletteColorNormalized(index);
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )glsl";

constexpr char kComputeC14X2[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 coords = gl_GlobalInvocationID.xy;

        // Tiled in 4x4 blocks, 16 bits per pixel
        uint buffer_pos = GetTiledTexelOffset(uint2(4u, 4u), coords);
        uint index = Swap16(FETCH(buffer_pos)) & 0x3FFFu;
        float4 norm_color = GetPaletteColorNormalized(index);
        imageStore(output_image, int3(int2(coords), 0), norm_color);
      }
      )glsl";

constexpr char kComputeXFB[] = R"glsl(
      DEFINE_MAIN(8, 8)
      {
        uint2 uv = gl_GlobalInvocationID.xy;
        uint buffer_pos = (uv.y * u_src_row_stride) + (uv.x / 2u);
        float4 yuyv = float4(FETCH(buffer_pos));

        float y = (uv.x & 1u) != 0u ? yuyv.b : yuyv.r;

        float yComp = 1.164 * (y - 16.0);
        float uComp = yuyv.g - 128.0;
        float vComp = yuyv.a - 128.0;

        float4 rgb = float4(yComp + (1.596 * vComp),
                        yComp - (0.813 * vComp) - (0.391 * uComp),
                        yComp + (2.018 * uComp),
                        255.0);
        float4 rgba_norm = rgb / 255.0;
        imageStore(output_image, int3(int2(uv), 0), rgba_norm);
      }
      )glsl";

constexpr char kPaletteExpandFunctions[] = R"glsl(
int Convert3To8(int v)
{
  // Swizzle bits: 00000123 -> 12312312
  return (v << 5) | (v << 2) | (v >> 1);
}
int Convert4To8(int v)
{
  // Swizzle bits: 00001234 -> 12341234
  return (v << 4) | v;
}
int Convert5To8(int v)
{
  // Swizzle bits: 00012345 -> 12345123
  return (v << 3) | (v >> 2);
}
int Convert6To8(int v)
{
  // Swizzle bits: 00123456 -> 12345612
  return (v << 2) | (v >> 4);
})glsl";

constexpr char kPaletteDecodeIA8[] = R"glsl(
float4 DecodePixel(int val)
{
  int i = val & 0xFF;
  int a = val >> 8;
  return float4(i, i, i, a) / 255.0;
})glsl";

constexpr char kPaletteDecodeRGB565[] = R"glsl(
float4 DecodePixel(int val)
{
  int r, g, b, a;
  r = Convert5To8((val >> 11) & 0x1f);
  g = Convert6To8((val >> 5) & 0x3f);
  b = Convert5To8((val) & 0x1f);
  a = 0xFF;
  return float4(r, g, b, a) / 255.0;
})glsl";

constexpr char kPaletteDecodeRGB5A3[] = R"glsl(
float4 DecodePixel(int val)
{
  int r,g,b,a;
  if ((val&0x8000) > 0)
  {
    r=Convert5To8((val>>10) & 0x1f);
    g=Convert5To8((val>>5 ) & 0x1f);
    b=Convert5To8((val    ) & 0x1f);
    a=0xFF;
  }
  else
  {
    a=Convert3To8((val>>12) & 0x7);
    r=Convert4To8((val>>8 ) & 0xf);
    g=Convert4To8((val>>4 ) & 0xf);
    b=Convert4To8((val    ) & 0xf);
  }
  return float4(r, g, b, a) / 255.0;
})glsl";

// ---------------------------------------------------------------------------------------------
// GLSL text of the EFB encoders
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kEncoderUniforms = R"glsl(UBO_BINDING(std140, 1) uniform PSBlock {
  int4 position;
  float y_scale;
  float gamma_rcp;
  float2 clamp_tb;
  uint3 filter_coefficients;
};
)glsl";

constexpr std::string_view kVertexDataBlockIn = R"glsl(VARYING_LOCATION(0) in VertexData {
  float3 v_tex0;
};
)glsl";

constexpr std::string_view kVertexDataPlainIn = "VARYING_LOCATION(0) in float3 v_tex0;\n";

constexpr std::string_view kEncoderBindings = R"glsl(SAMPLER_BINDING(0) uniform sampler2DArray samp0;
FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;
)glsl";

// Reduce a sampled colour to what the EFB pixel format can actually hold. A format without
// alpha reads back as opaque.
constexpr std::string_view kEfbPrecisionHelpers = R"glsl(float4 RGBA8ToRGB8(float4 src)
{
  return float4(src.xyz, 1.0);
}
float4 RGBA8ToRGBA6(float4 src)
{
  int4 val = int4(roundEven(src * 255.0));
  val = (val & 0xfc) | (val >> 6);
  return float4(val) / 255.0;
}
float4 RGBA8ToRGB565(float4 src)
{
  int4 val = int4(roundEven(src * 255.0));
  val.r = (val.r & 0xf8) | (val.r >> 5);
  val.g = (val.g & 0xfc) | (val.g >> 6);
  val.b = (val.b & 0xf8) | (val.b >> 5);
  val.a = 255;
  return float4(val) / 255.0;
}
)glsl";

constexpr std::string_view kRawSampleOpen =
    "uint4 SampleEFB0(float2 uv, float2 pixel_size, float x_offset, float y_offset) {\n"
    "  float4 tex_sample = texture(samp0, float3(uv.x + x_offset * pixel_size.x, ";

constexpr std::string_view kFlipDepth = "  tex_sample.x = 1.0 - tex_sample.x;\n";

constexpr std::string_view kDepthAsBytes = R"glsl(  uint depth = uint(tex_sample.x * 16777216.0);
  return uint4((depth >> 16) & 255u, (depth >> 8) & 255u, depth & 255u, 255u);
}
)glsl";

constexpr std::string_view kColorAsBytes = R"glsl(  return uint4(tex_sample * 255.0);
}
)glsl";

constexpr std::string_view kFilteredSampleOpen =
    "float4 SampleEFB(float2 uv, float2 pixel_size, int x_offset)\n{\n";

constexpr std::string_view kFilterThreeRows =
    R"glsl(  uint3 combined_rows = prev_row.rgb * filter_coefficients[0] +
                        current_row.rgb * filter_coefficients[1] +
                        next_row.rgb * filter_coefficients[2];
)glsl";

constexpr std::string_view kFilterOneRow =
    "  uint3 combined_rows = current_row.rgb * filter_coefficients[1];\n";

constexpr std::string_view kFilterNormalize = R"glsl(  // Shift right by 6 to divide by 64, as filter coefficients
  // that sum to 64 result in no change in brightness
  uint4 texcol_raw = uint4(combined_rows.rgb >> 6, current_row.a);
)glsl";

constexpr std::string_view kWrapOverflow = "  texcol_raw &= 0x1ffu;\n";
constexpr std::string_view kSaturate = "  texcol_raw = min(texcol_raw, uint4(255, 255, 255, 255));\n";

constexpr std::string_view kGamma = R"glsl(  texcol_raw = uint4(round(pow(float4(texcol_raw) / 255.0,
                     float4(gamma_rcp, gamma_rcp, gamma_rcp, 1.0)) * 255.0));
)glsl";

constexpr std::string_view kToYUV = R"glsl(  // Intensity/YUV format conversion constants determined by hardware testing
  const float4 y_const = float4( 66, 129,  25,  16);
  const float4 u_const = float4(-38, -74, 112, 128);
  const float4 v_const = float4(112, -94, -18, 128);
  // Intensity/YUV format conversion
  texcol_raw.rgb = uint3(dot(y_const, float4(texcol_raw.rgb, 256)),
                         dot(u_const, float4(texcol_raw.rgb, 256)),
                         dot(v_const, float4(texcol_raw.rgb, 256)));
  // Divide by 256 and round .5 and higher up
  texcol_raw.rgb = (texcol_raw.rgb >> 8) + ((texcol_raw.rgb >> 7) & 1u);
)glsl";

constexpr std::string_view kFilteredSampleClose = R"glsl(  return float4(texcol_raw) / 255.0;
}
)glsl";

constexpr std::string_view kMainOpen = R"glsl(void main()
{
  int2 sampleUv;
  int2 uv1 = int2(gl_FragCoord.xy);
)glsl";

constexpr std::string_view kXFBEncode = R"glsl(  // Intensity/YUV format conversion constants determined by hardware testing
  const float4 y_const = float4( 66, 129,  25,  16);
  const float4 u_const = float4(-38, -74, 112, 128);
  const float4 v_const = float4(112, -94, -18, 128);
  float4 average = (color0 + color1) * 0.5;
  // TODO: check rounding
  ocol0.b = round(dot(color0,  y_const)) / 256.0;
  ocol0.g = round(dot(average, u_const)) / 256.0;
  ocol0.r = round(dot(color1,  y_const)) / 256.0;
  ocol0.a = round(dot(average, v_const)) / 256.0;
)glsl";

// ---------------------------------------------------------------------------------------------
// EFB encoder generation
// ---------------------------------------------------------------------------------------------

// ShaderCode::Write takes a format string; literal GLSL goes through "{}" so that its braces
// are copied as they are.
void Put(ShaderCode& code, std::string_view glsl)
{
  code.Write("{}", glsl);
}

void PutVertexDataInput(ShaderCode& code)
{
  Put(code, g_backend_info.bSupportsGeometryShaders ? kVertexDataBlockIn : kVertexDataPlainIn);
}

// `dest = SampleEFB(...)` for the texel `x_offset` steps to the right, keeping `channels`.
void Fetch(ShaderCode& code, std::string_view dest, int x_offset, std::string_view channels)
{
  code.Write("  {} = SampleEFB(uv0, pixel_size, {}).{};\n", dest, x_offset, channels);
}

// `dest = src` reduced from [0, 1] to an integer with `bits` bits (as a float).
void Quantize(ShaderCode& code, int bits, std::string_view src, std::string_view dest)
{
  code.Write("  {} = floor({} * 255.0 / exp2(8.0 - {}.0));\n", dest, src, bits);
}

void PutDeclarations(ShaderCode& code, std::initializer_list<std::string_view> declarations)
{
  for (const std::string_view declaration : declarations)
    code.Write("  {};\n", declaration);
}

// The raw EFB read (with format precision and depth handling), then the copy filter, gamma and
// optional YUV conversion on top of it.
void PutSamplers(ShaderCode& code, const EFBCopyParams& params, APIType api_type)
{
  Put(code, kRawSampleOpen);
  // OpenGL counts rows from the bottom of the framebuffer.
  code.Write("clamp(uv.y {} y_offset * pixel_size.y, clamp_tb.x, clamp_tb.y), 0.0));\n",
             api_type == APIType::OpenGL ? '-' : '+');

  std::string_view precision_helper;
  if (params.efb_format == PixelFormat::RGB8_Z24)
    precision_helper = "RGBA8ToRGB8";
  else if (params.efb_format == PixelFormat::RGBA6_Z24)
    precision_helper = "RGBA8ToRGBA6";
  else if (params.efb_format == PixelFormat::RGB565_Z16)
    precision_helper = "RGBA8ToRGB565";
  if (!precision_helper.empty())
    code.Write("  tex_sample = {}(tex_sample);\n", precision_helper);

  if (params.depth)
  {
    if (!g_backend_info.bSupportsReversedDepthRange)
      Put(code, kFlipDepth);
    Put(code, kDepthAsBytes);
  }
  else
  {
    Put(code, kColorAsBytes);
  }

  // The vertical copy filter (verified on hardware to apply to depth copies too) only touches
  // RGB. Without outer taps only the middle row is fetched.
  Put(code, kFilteredSampleOpen);
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> rows = {{
      {"prev", "-1.0f"},
      {"current", "0.0f"},
      {"next", "1.0f"},
  }};
  for (const auto& [row, y_offset] : rows)
  {
    if (row != "current" && !params.all_copy_filter_coefs_needed)
      continue;
    code.Write("  uint4 {}_row = SampleEFB0(uv, pixel_size, float(x_offset), {});\n", row,
               y_offset);
  }
  Put(code, params.all_copy_filter_coefs_needed ? kFilterThreeRows : kFilterOneRow);
  Put(code, kFilterNormalize);
  if (params.copy_filter_can_overflow)
    Put(code, kWrapOverflow);
  // Saturating is harmless when nothing overflowed, so it is always done.
  Put(code, kSaturate);
  if (params.apply_gamma)
    Put(code, kGamma);
  if (params.yuv)
    Put(code, kToYUV);
  Put(code, kFilteredSampleClose);
}

// Maps the output pixel to the EFB texel(s) it encodes: each output pixel packs `samples`
// horizontally adjacent texels of one block row.
void PutTexelAddressing(ShaderCode& code, const EFBCopyParams& params, APIType api_type)
{
  Put(code, kMainOpen);

  const int block_w = TexDecoder_GetEFBCopyBlockWidthInTexels(params.copy_format);
  const int block_h = TexDecoder_GetEFBCopyBlockHeightInTexels(params.copy_format);
  int per_pixel = GetEncodedSampleCount(params.copy_format);
  const int block_texels = block_w * block_h;
  const auto ilog2 = [](int value) { return MathUtil::IntLog2(static_cast<u64>(value)); };

  code.Write("  int x_block_position = (uv1.x >> {}) << {};\n", ilog2(block_texels / per_pixel),
             ilog2(block_w));
  code.Write("  int y_block_position = uv1.y << {};\n", ilog2(block_h));
  if (per_pixel == 1)
  {
    // 32-bit texels are written as two half blocks (AR, then GB), two texels per pixel each.
    code.Write("  bool first = (uv1.x & {}) == 0;\n", block_texels / 2);
    per_pixel = 2;
  }
  const int pixels_per_row = block_w / per_pixel;
  code.Write("  int offset_in_block = uv1.x & {};\n", block_texels / per_pixel - 1);
  code.Write("  int y_offset_in_block = offset_in_block >> {};\n", ilog2(pixels_per_row));
  code.Write("  int x_offset_in_block = (offset_in_block & {}) << {};\n", pixels_per_row - 1,
             ilog2(per_pixel));

  Put(code, "  sampleUv.x = x_block_position + x_offset_in_block;\n"
            "  sampleUv.y = y_block_position + y_offset_in_block;\n"
            "  float2 uv0 = float2(sampleUv);\n"
            "  uv0 += float2(0.5, 0.5);\n"
            "  uv0 *= float(position.w);\n"
            "  uv0 += float2(position.xy);\n");
  code.Write("  uv0 /= float2({}, {});\n", EFB_WIDTH, EFB_HEIGHT);
  Put(code, "  uv0 /= float2(1, y_scale);\n");
  if (api_type == APIType::OpenGL)
    Put(code, "  uv0.y = 1.0-uv0.y;\n");
  code.Write("  float2 pixel_size = float2(position.w, position.w) / float2({}, {});\n", EFB_WIDTH,
             EFB_HEIGHT);
}

// 4-bit formats: eight texels per pixel, high nibble first, bytes stored b, g, r, a.
void EncodeNibbles(ShaderCode& code, std::string_view channel)
{
  PutDeclarations(code, {"float4 color0", "float4 color1"});
  static constexpr std::array<std::string_view, 8> targets = {
      "color0.b", "color1.b", "color0.g", "color1.g", "color0.r", "color1.r", "color0.a", "color1.a"};
  for (int i = 0; i < 8; ++i)
    Fetch(code, targets[static_cast<size_t>(i)], i, channel);
  Quantize(code, 4, "color0", "color0");
  Quantize(code, 4, "color1", "color1");
  Put(code, "  ocol0 = (color0 * 16.0 + color1) / 255.0;\n");
}

// 8-bit single-channel formats: four texels per pixel.
void EncodeBytes(ShaderCode& code, std::string_view channel)
{
  static constexpr std::array<std::string_view, 4> targets = {"ocol0.b", "ocol0.g", "ocol0.r",
                                                              "ocol0.a"};
  for (int i = 0; i < 4; ++i)
    Fetch(code, targets[static_cast<size_t>(i)], i, channel);
}

// Two 4-bit channels per texel (RA4): four texels per pixel.
void EncodeNibblePairs(ShaderCode& code, std::string_view channels)
{
  PutDeclarations(code, {"float2 texSample", "float4 color0", "float4 color1"});
  static constexpr std::array<char, 4> lanes = {'b', 'g', 'r', 'a'};
  for (int i = 0; i < 4; ++i)
  {
    Fetch(code, "texSample", i, channels);
    const char lane = lanes[static_cast<size_t>(i)];
    code.Write("  color0.{0} = texSample.x;\n  color1.{0} = texSample.y;\n", lane);
  }
  Quantize(code, 4, "color0", "color0");
  Quantize(code, 4, "color1", "color1");
  Put(code, "  ocol0 = (color0 * 16.0 + color1) / 255.0;\n");
}

// Two 8-bit channels per texel: two texels per pixel.
void EncodeBytePairs(ShaderCode& code, std::string_view channels)
{
  Fetch(code, "ocol0.bg", 0, channels);
  Fetch(code, "ocol0.ra", 1, channels);
}

void EncodeRGB565(ShaderCode& code)
{
  PutDeclarations(code, {"float3 texSample0", "float3 texSample1"});
  Fetch(code, "texSample0", 0, "rgb");
  Fetch(code, "texSample1", 1, "rgb");
  for (const char c : {'R', 'G', 'B'})
  {
    const char lower = static_cast<char>(c - 'A' + 'a');
    code.Write("  float2 tex{}s = float2(texSample0.{}, texSample1.{});\n", c, lower, lower);
  }
  // Green straddles the two bytes: its top three bits join red, the low three join blue.
  Quantize(code, 6, "texGs", "float2 gInt");
  Put(code, "  float2 gUpper = floor(gInt / 8.0);\n"
            "  float2 gLower = gInt - gUpper * 8.0;\n");
  Quantize(code, 5, "texRs", "ocol0.br");
  Put(code, "  ocol0.br = ocol0.br * 8.0 + gUpper;\n");
  Quantize(code, 5, "texBs", "ocol0.ga");
  Put(code, "  ocol0.ga = ocol0.ga + gLower * 32.0;\n");
  Put(code, "  ocol0 = ocol0 / 255.0;\n");
}

void EncodeRGB5A3(ShaderCode& code)
{
  PutDeclarations(code, {"float4 texSample", "float color0", "float gUpper", "float gLower"});

  // Two texels per pixel; the first fills bytes b/g, the second r/a. Texels whose alpha is
  // above what 3 bits hold (224/255) are stored opaque as RGB555 with the top bit set,
  // the rest as ARGB3444.
  static constexpr std::array<std::pair<char, char>, 2> bytes = {{{'b', 'g'}, {'r', 'a'}}};
  for (int texel = 0; texel < 2; ++texel)
  {
    const auto [high, low] = bytes[static_cast<size_t>(texel)];
    const std::string high_byte = fmt::format("ocol0.{}", high);
    const std::string low_byte = fmt::format("ocol0.{}", low);

    Fetch(code, "texSample", texel, "rgba");
    Put(code, "if(texSample.a > 0.878f) {\n");
    Quantize(code, 5, "texSample.g", "color0");
    Put(code, "  gUpper = floor(color0 / 8.0);\n"
              "  gLower = color0 - gUpper * 8.0;\n");
    Quantize(code, 5, "texSample.r", high_byte);
    code.Write("  {0} = {0} * 4.0 + gUpper + 128.0;\n", high_byte);
    Quantize(code, 5, "texSample.b", low_byte);
    code.Write("  {0} = {0} + gLower * 32.0;\n", low_byte);
    Put(code, "} else {\n");
    Quantize(code, 4, "texSample.r", high_byte);
    Quantize(code, 4, "texSample.b", low_byte);
    Quantize(code, 3, "texSample.a", "color0");
    code.Write("{0} = {0} + color0 * 16.0;\n", high_byte);
    Quantize(code, 4, "texSample.g", "color0");
    code.Write("{0} = {0} + color0 * 16.0;\n", low_byte);
    Put(code, "}\n");
  }
  Put(code, "  ocol0 = ocol0 / 255.0;\n");
}

void EncodeRGBA8(ShaderCode& code)
{
  PutDeclarations(code, {"float4 texSample", "float4 color0", "float4 color1"});
  // color0 collects the AR half block and color1 the GB half block; `first` picks one.
  static constexpr std::array<std::pair<char, char>, 2> lanes = {{{'b', 'g'}, {'r', 'a'}}};
  for (int texel = 0; texel < 2; ++texel)
  {
    const auto [lane0, lane1] = lanes[static_cast<size_t>(texel)];
    Fetch(code, "texSample", texel, "rgba");
    code.Write("  color0.{0} = texSample.a;\n"
               "  color0.{1} = texSample.r;\n"
               "  color1.{0} = texSample.g;\n"
               "  color1.{1} = texSample.b;\n",
               lane0, lane1);
  }
  Put(code, "  ocol0 = first ? color0 : color1;\n");
}

void EncodeXFB(ShaderCode& code)
{
  Put(code, "float4 color0 = float4(0, 0, 0, 1), color1 = float4(0, 0, 0, 1);\n");
  Fetch(code, "color0.rgb", 0, "rgb");
  Fetch(code, "color1.rgb", 1, "rgb");
  Put(code, kXFBEncode);
}

bool PutEncoder(ShaderCode& code, EFBCopyFormat format)
{
  switch (format)
  {
  case EFBCopyFormat::R4:
    EncodeNibbles(code, "r");
    return true;
  case EFBCopyFormat::RA4:
    EncodeNibblePairs(code, "ar");
    return true;
  case EFBCopyFormat::A8:
    EncodeBytes(code, "a");
    return true;
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
    EncodeBytes(code, "r");
    return true;
  case EFBCopyFormat::G8:
    EncodeBytes(code, "g");
    return true;
  case EFBCopyFormat::B8:
    EncodeBytes(code, "b");
    return true;
  case EFBCopyFormat::RA8:
    EncodeBytePairs(code, "ar");
    return true;
  case EFBCopyFormat::RG8:
    EncodeBytePairs(code, "gr");
    return true;
  case EFBCopyFormat::GB8:
    EncodeBytePairs(code, "bg");
    return true;
  case EFBCopyFormat::RGB565:
    EncodeRGB565(code);
    return true;
  case EFBCopyFormat::RGB5A3:
    EncodeRGB5A3(code);
    return true;
  case EFBCopyFormat::RGBA8:
    EncodeRGBA8(code);
    return true;
  case EFBCopyFormat::XFB:
    EncodeXFB(code);
    return true;
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Decoder table
// ---------------------------------------------------------------------------------------------

struct DecoderEntry
{
  TextureFormat format;
  DecodingShaderInfo info;
};

u32 PaletteBytes(TextureFormat format)
{
  return static_cast<u32>(TexDecoder_GetPaletteSize(format));
}

// Group sizes: 8x8 threads for every tiled format; CMPR runs one thread per texel of a
// flattened list of 64-texel blocks.
const std::array<DecoderEntry, 12> s_decoders = {{
    {TextureFormat::I4, {TEXEL_BUFFER_FORMAT_R8_UINT, 0, 8, 8, false, kComputeI4}},
    {TextureFormat::IA4, {TEXEL_BUFFER_FORMAT_R8_UINT, 0, 8, 8, false, kComputeIA4}},
    {TextureFormat::I8, {TEXEL_BUFFER_FORMAT_R8_UINT, 0, 8, 8, false, kComputeI8}},
    {TextureFormat::IA8, {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false, kComputeIA8}},
    {TextureFormat::RGB565, {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false, kComputeRGB565}},
    {TextureFormat::RGB5A3, {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false, kComputeRGB5A3}},
    {TextureFormat::RGBA8, {TEXEL_BUFFER_FORMAT_R16_UINT, 0, 8, 8, false, kComputeRGBA8}},
    {TextureFormat::CMPR, {TEXEL_BUFFER_FORMAT_R32G32_UINT, 0, 64, 1, true, kComputeCMPR}},
    {TextureFormat::C4,
     {TEXEL_BUFFER_FORMAT_R8_UINT, PaletteBytes(TextureFormat::C4), 8, 8, false, kComputeC4}},
    {TextureFormat::C8,
     {TEXEL_BUFFER_FORMAT_R8_UINT, PaletteBytes(TextureFormat::C8), 8, 8, false, kComputeC8}},
    {TextureFormat::C14X2,
     {TEXEL_BUFFER_FORMAT_R16_UINT, PaletteBytes(TextureFormat::C14X2), 8, 8, false,
      kComputeC14X2}},
    {TextureFormat::XFB, {TEXEL_BUFFER_FORMAT_RGBA8_UINT, 0, 8, 8, false, kComputeXFB}},
}};

std::string_view PaletteDefine(TLUTFormat format)
{
  switch (format)
  {
  case TLUTFormat::IA8:
    return "#define PALETTE_FORMAT_IA8 1\n";
  case TLUTFormat::RGB565:
    return "#define PALETTE_FORMAT_RGB565 1\n";
  case TLUTFormat::RGB5A3:
    return "#define PALETTE_FORMAT_RGB5A3 1\n";
  default:
    return {};
  }
}

std::string_view TexelBufferDefine(TexelBufferFormat format)
{
  switch (format)
  {
  case TEXEL_BUFFER_FORMAT_R8_UINT:
    return "#define TEXEL_BUFFER_FORMAT_R8 1\n";
  case TEXEL_BUFFER_FORMAT_R16_UINT:
    return "#define TEXEL_BUFFER_FORMAT_R16 1\n";
  case TEXEL_BUFFER_FORMAT_RGBA8_UINT:
    return "#define TEXEL_BUFFER_FORMAT_RGBA8 1\n";
  case TEXEL_BUFFER_FORMAT_R32G32_UINT:
    return "#define TEXEL_BUFFER_FORMAT_R32G32 1\n";
  case NUM_TEXEL_BUFFER_FORMATS:
    ASSERT(false);
    return {};
  }
  return {};
}
}  // namespace

u16 GetEncodedSampleCount(EFBCopyFormat format)
{
  // EFB texels packed into one RGBA8 output pixel.
  switch (format)
  {
  case EFBCopyFormat::R4:
    return 8;
  case EFBCopyFormat::RA4:
  case EFBCopyFormat::A8:
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
  case EFBCopyFormat::G8:
  case EFBCopyFormat::B8:
    return 4;
  case EFBCopyFormat::RA8:
  case EFBCopyFormat::RGB565:
  case EFBCopyFormat::RGB5A3:
  case EFBCopyFormat::RG8:
  case EFBCopyFormat::GB8:
  case EFBCopyFormat::XFB:
    return 2;
  case EFBCopyFormat::RGBA8:
    return 1;
  default:
    PanicAlertFmt("Invalid EFB Copy Format {}! (GetEncodedSampleCount)", format);
    return 1;
  }
}

std::string GenerateEncodingShader(const EFBCopyParams& params, APIType api_type)
{
  ShaderCode code;
  Put(code, kEncoderUniforms);
  PutVertexDataInput(code);
  Put(code, kEncoderBindings);
  Put(code, kEfbPrecisionHelpers);
  PutSamplers(code, params, api_type);
  PutTexelAddressing(code, params, api_type);
  if (!PutEncoder(code, params.copy_format))
    PanicAlertFmt("Invalid EFB Copy Format {}! (GenerateEncodingShader)", params.copy_format);
  Put(code, "}\n");
  return code.GetBuffer();
}

const DecodingShaderInfo* GetDecodingShaderInfo(TextureFormat format)
{
  for (const DecoderEntry& entry : s_decoders)
  {
    if (entry.format == format)
      return &entry.info;
  }
  return nullptr;
}

std::pair<u32, u32> GetDispatchCount(const DecodingShaderInfo* info, u32 width, u32 height)
{
  const auto groups = [](u32 items, u32 group_size) { return (items + group_size - 1) / group_size; };
  if (info->group_flatten)
    return {groups(width * height, info->group_size_x), 1};
  return {groups(width, info->group_size_x), groups(height, info->group_size_y)};
}

// The decoders are written in the common shader dialect, so the API does not change the source.
std::string GenerateDecodingShader(TextureFormat format, std::optional<TLUTFormat> palette_format,
                                   APIType /*api_type*/)
{
  const DecodingShaderInfo* info = GetDecodingShaderInfo(format);
  if (info == nullptr)
    return {};

  std::string source;
  if (palette_format)
    source += PaletteDefine(*palette_format);
  source += TexelBufferDefine(info->buffer_format);
  source += kComputeCommon;
  source += info->shader_body;
  return source;
}

std::string GeneratePaletteConversionShader(TLUTFormat palette_format, APIType api_type)
{
  std::string source = kPaletteExpandFunctions;
  switch (palette_format)
  {
  case TLUTFormat::IA8:
    source += kPaletteDecodeIA8;
    break;
  case TLUTFormat::RGB565:
    source += kPaletteDecodeRGB565;
    break;
  case TLUTFormat::RGB5A3:
    source += kPaletteDecodeRGB5A3;
    break;
  default:
    PanicAlertFmt("Unknown format");
    break;
  }
  source += '\n';

  const bool metal = api_type == APIType::Metal;
  source += metal ? "SSBO_BINDING(0) readonly buffer Palette { uint16_t palette[]; };\n" :
                    "TEXEL_BUFFER_BINDING(0) uniform usamplerBuffer samp0;\n";
  source += "SAMPLER_BINDING(1) uniform sampler2DArray samp1;\n"
            "UBO_BINDING(std140, 1) uniform PSBlock {\n"
            "  float multiplier;\n"
            "  int texel_buffer_offset;\n"
            "};\n";
  source += g_backend_info.bSupportsGeometryShaders ?
                "VARYING_LOCATION(0) in VertexData {\n  float3 v_tex0;\n};\n" :
                "VARYING_LOCATION(0) in float3 v_tex0;\n";
  // The index texture holds index / multiplier; look the palette entry up and byte-swap it.
  source += "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n"
            "void main() {\n"
            "  float3 coords = v_tex0;\n"
            "  int src = int(round(texture(samp1, coords).r * multiplier));\n";
  source += metal ? "  src = int(palette[uint(src)]);\n" :
                    "  src = int(texelFetch(samp0, src + texel_buffer_offset).r);\n";
  source += "  src = ((src << 8) | (src >> 8)) & 0xFFFF;\n"
            "  ocol0 = DecodePixel(src);\n"
            "}\n";
  return source;
}

}  // namespace TextureConversionShaderTiled
