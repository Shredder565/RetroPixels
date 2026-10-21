// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/CustomTextureData.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include "Common/IOFile.h"
#include "Common/Image.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/VideoConfig.h"

// DDS reader, written from Microsoft's public description of the DDS file format
// ("DDS" / "DDS_HEADER" / "DDS_PIXELFORMAT" / "DDS_HEADER_DXT10" reference pages).
//
// A file is the magic "DDS ", a 124-byte header, an optional 20-byte DX10 header (when the
// four-character code is "DX10"), then each array slice's mip chain, largest level first.
// Only block-compressed formats the backend can sample directly and a handful of 8-bit-per-
// channel RGB(A) layouts are accepted; anything else is left to the generic image loader.

namespace
{
using Level = VideoCommon::CustomTextureData::ArraySlice::Level;

constexpr u32 FourCC(char a, char b, char c, char d)
{
  return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

constexpr u32 MAGIC = FourCC('D', 'D', 'S', ' ');

// Word indices into the 124-byte header (31 little-endian u32s).
enum HeaderWord : std::size_t
{
  H_SIZE = 0,
  H_FLAGS = 1,
  H_HEIGHT = 2,
  H_WIDTH = 3,
  H_PITCH_OR_LINEAR_SIZE = 4,
  H_MIP_COUNT = 6,
  H_PIXEL_FORMAT = 18,  // eight words: size, flags, fourcc, bit count, R, G, B, A masks
  H_CAPS2 = 27,
  H_WORDS = 31,
};
constexpr std::size_t PF_FLAGS = 1;
constexpr std::size_t PF_FOURCC = 2;
constexpr std::size_t PF_BIT_COUNT = 3;

// Words of the DX10 extension header.
constexpr std::size_t X_DXGI_FORMAT = 0;
constexpr std::size_t X_ARRAY_SIZE = 3;
constexpr std::size_t X_WORDS = 5;

// Header flags (DDSD_*).
constexpr u32 HF_REQUIRED = 0x1 | 0x2 | 0x4 | 0x1000;  // caps, height, width, pixel format
constexpr u32 HF_PITCH = 0x8;
constexpr u32 HF_MIPMAPCOUNT = 0x20000;
constexpr u32 HF_LINEARSIZE = 0x80000;
constexpr u32 HF_DEPTH = 0x800000;

// Pixel format flags (DDPF_*).
constexpr u32 PFF_ALPHAPIXELS = 0x1;
constexpr u32 PFF_FOURCC = 0x4;
constexpr u32 PFF_RGB = 0x40;

// caps2 (DDSCAPS2_*): the cube map bit plus the six face bits.
constexpr u32 CAPS2_CUBEMAP = 0x200;
constexpr u32 CAPS2_ALL_FACES = CAPS2_CUBEMAP | 0xFC00;

// What has to happen to an uncompressed level to make it RGBA8.
enum class Fixup
{
  None,          // stored R, G, B, A
  SwapRB,        // stored B, G, R, A
  SwapRBOpaque,  // stored B, G, R, unused
  Opaque,        // stored R, G, B, unused
  ExpandBGR,     // stored B, G, R (three bytes per texel)
};

struct UncompressedLayout
{
  u32 flags;
  u32 bit_count;
  std::array<u32, 4> masks;  // R, G, B, A
  Fixup fixup;
};

constexpr std::array<UncompressedLayout, 5> UNCOMPRESSED_LAYOUTS{{
    {PFF_RGB | PFF_ALPHAPIXELS,
     32,
     {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000},
     Fixup::SwapRB},
    {PFF_RGB, 32, {0x00ff0000, 0x0000ff00, 0x000000ff, 0}, Fixup::SwapRBOpaque},
    {PFF_RGB, 32, {0x000000ff, 0x0000ff00, 0x00ff0000, 0}, Fixup::Opaque},
    {PFF_RGB, 24, {0x00ff0000, 0x0000ff00, 0x000000ff, 0}, Fixup::ExpandBGR},
    {PFF_RGB | PFF_ALPHAPIXELS, 32, {0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000}, Fixup::None},
}};

struct CompressedLayout
{
  u32 fourcc;       // legacy four-character code, or 0 when only a DXGI format names it
  u32 dxgi_format;  // DXGI_FORMAT value used in a DX10 header
  AbstractTextureFormat format;
  u32 bytes_per_block;
  bool is_s3tc;
};

constexpr std::array<CompressedLayout, 4> COMPRESSED_LAYOUTS{{
    {FourCC('D', 'X', 'T', '1'), 71, AbstractTextureFormat::DXT1, 8, true},
    {FourCC('D', 'X', 'T', '3'), 74, AbstractTextureFormat::DXT3, 16, true},
    {FourCC('D', 'X', 'T', '5'), 77, AbstractTextureFormat::DXT5, 16, true},
    {0, 98, AbstractTextureFormat::BPTC, 16, false},
}};

// Everything needed to read the image data that follows the headers.
struct DdsLayout
{
  u32 width = 0;
  u32 height = 0;
  u32 mip_count = 0;
  u32 slice_count = 0;
  AbstractTextureFormat format = AbstractTextureFormat::RGBA8;
  u32 block_extent = 1;     // texels along each side of one block (1 = uncompressed)
  u32 bytes_per_block = 4;  // bytes per block (per texel when uncompressed)
  Fixup fixup = Fixup::None;

  std::size_t data_offset = 0;
  u32 top_row_length = 0;
  std::size_t top_size = 0;
};

u32 BlocksAcross(u32 texels, u32 block_extent)
{
  return std::max((texels + block_extent - 1) / block_extent, 1u);
}

u32 FullMipChainLength(u32 width, u32 height)
{
  u32 levels = 1;
  for (u32 longest = std::max(width, height); longest > 1; longest /= 2)
    ++levels;
  return levels;
}

// 6 for a complete cube map, 1 for a plain texture, nothing for a partial cube map.
std::optional<u32> SliceCountFromCaps(u32 caps2)
{
  if ((caps2 & CAPS2_CUBEMAP) == 0)
    return 1;
  if ((caps2 & CAPS2_ALL_FACES) != CAPS2_ALL_FACES)
    return std::nullopt;
  return 6;
}

// Fills in format details for a pixel format that uses a four-character code.
bool ReadCompressedFormat(File::IOFile& file, const std::array<u32, H_WORDS>& header,
                          DdsLayout& layout, std::size_t& headers_size)
{
  const u32 fourcc = header[H_PIXEL_FORMAT + PF_FOURCC];
  u32 dxgi_format = 0;

  if (fourcc == FourCC('D', 'X', '1', '0'))
  {
    std::array<u32, X_WORDS> extension;
    if (!file.ReadBytes(extension.data(), sizeof(extension)))
      return false;
    headers_size += sizeof(extension);
    dxgi_format = extension[X_DXGI_FORMAT];
    layout.slice_count = extension[X_ARRAY_SIZE];
  }
  else
  {
    const std::optional<u32> slices = SliceCountFromCaps(header[H_CAPS2]);
    if (!slices)
      return false;
    layout.slice_count = *slices;
  }

  const auto match = std::ranges::find_if(COMPRESSED_LAYOUTS, [&](const CompressedLayout& c) {
    return (c.fourcc != 0 && c.fourcc == fourcc) || c.dxgi_format == dxgi_format;
  });
  // Other codes are left to the generic loader.
  if (match == COMPRESSED_LAYOUTS.end())
    return false;

  layout.format = match->format;
  layout.block_extent = 4;
  layout.bytes_per_block = match->bytes_per_block;

  // Only formats the backend samples natively are kept compressed; the generic loader
  // decompresses the rest.
  if (match->format == AbstractTextureFormat::BPTC && !g_backend_info.bSupportsBPTCTextures)
    return false;
  if (match->is_s3tc && !g_backend_info.bSupportsST3CTextures)
    return false;
  return true;
}

// Fills in format details for an uncompressed RGB(A) pixel format.
bool ReadUncompressedFormat(const std::array<u32, H_WORDS>& header, DdsLayout& layout)
{
  const u32* pf = &header[H_PIXEL_FORMAT];
  const auto match = std::ranges::find_if(UNCOMPRESSED_LAYOUTS, [&](const UncompressedLayout& u) {
    return pf[0] == 32 && pf[PF_FLAGS] == u.flags && pf[PF_FOURCC] == 0 &&
           pf[PF_BIT_COUNT] == u.bit_count && pf[4] == u.masks[0] && pf[5] == u.masks[1] &&
           pf[6] == u.masks[2] && pf[7] == u.masks[3];
  });
  if (match == UNCOMPRESSED_LAYOUTS.end())
    return false;

  const std::optional<u32> slices = SliceCountFromCaps(header[H_CAPS2]);
  if (!slices)
    return false;

  layout.fixup = match->fixup;
  layout.slice_count = *slices;
  layout.format = AbstractTextureFormat::RGBA8;
  layout.block_extent = 1;
  layout.bytes_per_block = pf[PF_BIT_COUNT] / 8;
  return true;
}

// Reads the headers. Returns false for anything that is not a DDS file this loader handles.
bool ReadDdsLayout(File::IOFile& file, DdsLayout& layout)
{
  // Cheap rejection first: every custom texture file is offered to this reader.
  u32 magic = 0;
  if (!file.ReadBytes(&magic, sizeof(magic)) || magic != MAGIC)
    return false;

  std::array<u32, H_WORDS> header;
  std::size_t headers_size = sizeof(header);
  if (!file.ReadBytes(header.data(), sizeof(header)) || header[H_SIZE] < sizeof(header))
    return false;

  const u32 flags = header[H_FLAGS];
  if ((flags & HF_REQUIRED) != HF_REQUIRED)
    return false;
  // Volume textures are not supported.
  if ((flags & HF_DEPTH) != 0)
    return false;

  layout.width = header[H_WIDTH];
  layout.height = header[H_HEIGHT];
  if (layout.width == 0 || layout.height == 0)
    return false;

  if ((flags & HF_MIPMAPCOUNT) == 0)
    layout.mip_count = 1;
  else if (header[H_MIP_COUNT] == 0)
    layout.mip_count = FullMipChainLength(layout.width, layout.height);
  else
    layout.mip_count = header[H_MIP_COUNT];

  const bool compressed = (header[H_PIXEL_FORMAT + PF_FLAGS] & PFF_FOURCC) != 0;
  const bool known = compressed ? ReadCompressedFormat(file, header, layout, headers_size) :
                                  ReadUncompressedFormat(header, layout);
  if (!known)
    return false;

  const u32 blocks_x = BlocksAcross(layout.width, layout.block_extent);
  const u32 blocks_y = BlocksAcross(layout.height, layout.block_extent);

  if ((flags & HF_PITCH) != 0 && (flags & HF_LINEARSIZE) != 0)
  {
    // The writer gave an explicit row pitch in bytes.
    const u32 pitch = header[H_PITCH_OR_LINEAR_SIZE];
    if (pitch < layout.bytes_per_block)
      return false;
    const u32 blocks_per_row = std::max(pitch / layout.bytes_per_block, 1u);
    layout.top_row_length = blocks_per_row * layout.block_extent;
    layout.top_size =
        std::size_t{layout.top_row_length / layout.block_extent} * layout.block_extent * blocks_y;
  }
  else
  {
    // Rows of blocks are packed without padding.
    layout.top_row_length = blocks_x * layout.block_extent;
    layout.top_size = std::size_t{blocks_x} * layout.bytes_per_block * blocks_y;
  }

  layout.data_offset = sizeof(magic) + headers_size;
  // A file that ends right after its headers has no image.
  return layout.data_offset < file.GetSize();
}

// In-place fixups for four-byte texels. Texels past the end of the buffer are ignored.
void FixFourByteTexels(Level& level, Fixup fixup)
{
  const std::size_t texels =
      std::min(std::size_t{level.row_length} * level.height, level.data.size() / 4);
  u8* p = level.data.data();
  for (std::size_t i = 0; i < texels; ++i, p += 4)
  {
    if (fixup != Fixup::Opaque)
      std::swap(p[0], p[2]);
    if (fixup != Fixup::SwapRB)
      p[3] = 0xFF;
  }
}

// Widens three-byte B, G, R texels into a new R, G, B, A buffer. Source bytes past the end of
// the buffer read as zero.
void ExpandBGRTexels(Level& level)
{
  const std::size_t texels = std::size_t{level.row_length} * level.height;
  Common::UniqueBuffer<u8> rgba(texels * 4);

  const u8* src = level.data.data();
  const std::size_t src_size = level.data.size();
  const auto source_byte = [&](std::size_t at) -> u8 { return at < src_size ? src[at] : 0; };

  u8* out = rgba.data();
  for (std::size_t i = 0; i < texels; ++i)
  {
    const std::size_t at = i * 3;
    out[i * 4 + 0] = source_byte(at + 2);
    out[i * 4 + 1] = source_byte(at + 1);
    out[i * 4 + 2] = source_byte(at + 0);
    out[i * 4 + 3] = 0xFF;
  }
  level.data = std::move(rgba);
}

bool ReadLevel(Level& level, File::IOFile& file, const std::string& filename, u32 mip_level,
               const DdsLayout& layout, u32 width, u32 height, u32 row_length, std::size_t size)
{
  // Backends require the top level of a block-compressed texture to be whole blocks.
  const u32 extent = layout.block_extent;
  if (mip_level == 0 && extent > 1 && (width % extent != 0 || height % extent != 0))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Invalid dimensions for DDS texture {}. For compressed textures of this format, "
                  "the width/height of the first mip level must be a multiple of {}.",
                  filename, extent);
    return false;
  }

  level.width = width;
  level.height = height;
  level.format = layout.format;
  level.row_length = row_length;
  level.data.reset(size);
  if (!file.ReadBytes(level.data.data(), level.data.size()))
    return false;

  switch (layout.fixup)
  {
  case Fixup::None:
    break;
  case Fixup::ExpandBGR:
    ExpandBGRTexels(level);
    break;
  default:
    FixFourByteTexels(level, layout.fixup);
    break;
  }
  return true;
}

}  // namespace

namespace VideoCommon
{
bool LoadDDSTexture(CustomTextureData* texture, const std::string& filename)
{
  File::IOFile file(filename, "rb");
  DdsLayout layout;
  if (!file.IsOpen() || !ReadDdsLayout(file, layout))
    return false;
  if (!file.Seek(static_cast<s64>(layout.data_offset), File::SeekOrigin::Begin))
    return false;

  for (u32 slice_index = 0; slice_index < layout.slice_count; ++slice_index)
  {
    auto& slice = texture->m_slices.emplace_back();

    // The top level may use the pitch from the header.
    Level top;
    if (!ReadLevel(top, file, filename, 0, layout, layout.width, layout.height,
                   layout.top_row_length, layout.top_size))
    {
      return false;
    }
    slice.m_levels.push_back(std::move(top));

    // Smaller levels are always tightly packed. A truncated chain simply ends early.
    u32 w = layout.width;
    u32 h = layout.height;
    for (u32 mip = 1; mip < layout.mip_count; ++mip)
    {
      w = std::max(w / 2, 1u);
      h = std::max(h / 2, 1u);
      const u32 blocks_x = BlocksAcross(w, layout.block_extent);
      const u32 blocks_y = BlocksAcross(h, layout.block_extent);
      const std::size_t bytes = std::size_t{blocks_x} * layout.bytes_per_block * blocks_y;

      Level level;
      if (!ReadLevel(level, file, filename, mip, layout, w, h, blocks_x * layout.block_extent,
                     bytes))
      {
        break;
      }
      slice.m_levels.push_back(std::move(level));
    }
  }

  return true;
}

bool LoadDDSTexture(CustomTextureData::ArraySlice::Level* level, const std::string& filename,
                    u32 mip_level)
{
  File::IOFile file(filename, "rb");
  DdsLayout layout;
  if (!file.IsOpen() || !ReadDdsLayout(file, layout))
    return false;

  // The headers have just been read, so the file is positioned at the top level.
  return ReadLevel(*level, file, filename, mip_level, layout, layout.width, layout.height,
                   layout.top_row_length, layout.top_size);
}

bool LoadPNGTexture(CustomTextureData::ArraySlice::Level* level, const std::string& filename)
{
  if (level == nullptr) [[unlikely]]
    return false;

  File::IOFile file;
  file.Open(filename, "rb");
  const u64 size = file.GetSize();
  Common::UniqueBuffer<u8> contents(static_cast<std::size_t>(size));
  file.ReadBytes(contents.data(), static_cast<std::size_t>(size));
  return LoadPNGTexture(level, contents);
}

bool LoadPNGTexture(CustomTextureData::ArraySlice::Level* level, std::span<const u8> buffer)
{
  if (level == nullptr) [[unlikely]]
    return false;

  if (!Common::LoadPNG(buffer, &level->data, &level->width, &level->height) || level->data.empty())
  {
    return false;
  }

  // The PNG decoder always produces tightly packed RGBA8.
  level->format = AbstractTextureFormat::RGBA8;
  level->row_length = level->width;
  return true;
}
}  // namespace VideoCommon
