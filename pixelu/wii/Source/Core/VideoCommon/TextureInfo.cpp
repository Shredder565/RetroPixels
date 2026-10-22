// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TextureInfo.h"

#include <algorithm>
#include <span>

#include <fmt/format.h>
#include <xxhash.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/SpanUtils.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureDecoder.h"

namespace
{
// Byte count of a `width` x `height` image of `format`, both already whole blocks.
u32 ImageBytes(u32 width, u32 height, TextureFormat format)
{
  return static_cast<u32>(TexDecoder_GetTextureSizeInBytes(static_cast<int>(width),
                                                           static_cast<int>(height), format));
}

// The smallest and largest palette index a paletted texture actually uses. Only that part of
// the palette contributes to the texture's name, so unused entries can differ freely.
struct IndexRange
{
  u32 lowest = 0xffff;
  u32 highest = 0;

  void Include(u32 index)
  {
    lowest = std::min(lowest, index);
    highest = std::max(highest, index);
  }
};

IndexRange UsedPaletteIndices(std::span<const u8> texels, size_t texel_bytes, size_t palette_bytes)
{
  IndexRange range;
  const std::span<const u8> used = texels.first(texel_bytes);
  if (palette_bytes == 16 * 2)
  {
    // C4: two indices per byte.
    for (const u8 byte : used)
    {
      range.Include(byte & 0xFu);
      range.Include(static_cast<u32>(byte >> 4));
    }
  }
  else if (palette_bytes == 256 * 2)
  {
    // C8: one index per byte.
    for (const u8 byte : used)
      range.Include(byte);
  }
  else if (palette_bytes == 16384 * 2)
  {
    // C14X2: one index per 16-bit texel. For compatibility with existing texture packs this
    // reproduces the historical calculation, which byte-swaps only the first byte of each
    // texel as if it were a whole 16-bit value. That leaves the index as the low six bits of
    // that byte, shifted up by eight; the second byte is ignored.
    for (size_t i = 0; i < used.size(); i += 2)
      range.Include((static_cast<u32>(used[i]) << 8) & 0x3fffu);
  }
  return range;
}
}  // namespace

TextureInfo TextureInfo::FromStage(u32 stage)
{
  const auto& unit = bpmem.tex.GetUnit(stage);

  const TextureFormat texture_format = unit.texImage0.format;
  const TLUTFormat tlut_format = unit.texTlut.tlut_format;
  const u32 width = unit.texImage0.width + 1u;
  const u32 height = unit.texImage0.height + 1u;
  // Image addresses are stored in 32-byte units, TLUT offsets in 512-byte units of TMEM.
  const u32 address = unit.texImage3.image_base * 32u;
  const std::span<const u8> tlut = TexDecoder_GetTmemSpan(unit.texTlut.tmem_offset * 512u);

  // max_lod is in sixteenths of a level; a partial level still needs that level's data.
  std::optional<u32> mip_count;
  if (unit.texMode0.mipmap_filter != MipMode::None)
    mip_count = (unit.texMode1.max_lod + 15u) / 16u;

  if (unit.texImage1.cache_manually_managed)
  {
    // Preloaded into TMEM: level 0 and the even levels sit in the even region, odd levels in
    // the odd region.
    const std::span<const u8> even =
        TexDecoder_GetTmemSpan(unit.texImage1.tmem_even * u32{TMEM_LINE_SIZE});
    const std::span<const u8> odd =
        TexDecoder_GetTmemSpan(unit.texImage2.tmem_odd * u32{TMEM_LINE_SIZE});
    return TextureInfo(stage, even, tlut, address, texture_format, tlut_format, width, height,
                       true, odd, even, mip_count);
  }

  const std::span<const u8> ram = Core::System::GetInstance().GetMemory().GetSpanForAddress(address);
  return TextureInfo(stage, ram, tlut, address, texture_format, tlut_format, width, height, false,
                     {}, {}, mip_count);
}

TextureInfo::TextureInfo(u32 stage, std::span<const u8> data, std::span<const u8> tlut_data,
                         u32 address, TextureFormat texture_format, TLUTFormat tlut_format,
                         u32 width, u32 height, bool from_tmem, std::span<const u8> tmem_odd,
                         std::span<const u8> tmem_even, std::optional<u32> mip_count)
    : m_data(data), m_tlut_data(tlut_data), m_address(address), m_from_tmem(from_tmem),
      m_tmem_even(tmem_even), m_tmem_odd(tmem_odd), m_texture_format(texture_format),
      m_tlut_format(tlut_format), m_raw_width(width), m_raw_height(height), m_stage(stage)
{
  if (IsColorIndexed(texture_format))
    m_palette_size = static_cast<u32>(TexDecoder_GetPaletteSize(texture_format));

  m_block_width = static_cast<u32>(TexDecoder_GetBlockWidthInTexels(texture_format));
  m_block_height = static_cast<u32>(TexDecoder_GetBlockHeightInTexels(texture_format));
  m_expanded_width = Common::AlignUp(width, m_block_width);
  m_expanded_height = Common::AlignUp(height, m_block_height);
  m_texture_size = ImageBytes(m_expanded_width, m_expanded_height, texture_format);

  const bool texels_readable = data.size() >= m_texture_size;
  const bool palette_readable = !m_palette_size || tlut_data.size() >= *m_palette_size;
  if (!texels_readable)
    ERROR_LOG_FMT(VIDEO, "Trying to use an invalid texture address {:#010x}", GetRawAddress());
  else if (!palette_readable)
    ERROR_LOG_FMT(VIDEO, "Trying to use an invalid TLUT address {:#010x}", GetRawAddress());
  m_data_valid = texels_readable && palette_readable;

  if (mip_count)
  {
    m_mipmaps_enabled = true;
    // Backends reject chains that would continue past a single 1x1 level (a 64x64 texture
    // has at most six levels after level 0), so cap the count at what the size allows.
    // Counted as chain lengths (level 0 included) so that the unsigned arithmetic matches
    // the earlier implementation even for out-of-range requests.
    const u32 longest_chain = static_cast<u32>(MathUtil::IntLog2(std::max(width, height))) + 1;
    const u32 requested_chain = *mip_count + 1;
    m_limited_mip_count = std::min(longest_chain, requested_chain) - 1;
  }
}

std::string TextureInfo::NameDetails::GetFullName() const
{
  return fmt::format("{}_{}{}_{}", base_name, texture_name, tlut_name, format_name);
}

TextureInfo::NameDetails TextureInfo::CalculateTextureName() const
{
  if (!IsDataValid())
    return NameDetails{};

  // For paletted textures only the span of palette entries the texels use is hashed.
  std::span<const u8> palette;
  if (m_palette_size.value_or(0) != 0)
  {
    const IndexRange range = UsedPaletteIndices(m_data, m_texture_size, *m_palette_size);
    const u32 entries = range.highest + 1 - range.lowest;
    palette = std::span<const u8>(m_tlut_data.data() + 2 * range.lowest, 2 * entries);
  }
  DEBUG_ASSERT(palette.size() <= m_palette_size.value_or(0));

  NameDetails name;
  name.base_name = fmt::format("{}{}x{}{}", format_prefix, m_raw_width, m_raw_height,
                               m_mipmaps_enabled ? "_m" : "");
  name.texture_name = fmt::format("{:016x}", XXH64(m_data.data(), m_texture_size, 0));
  if (!palette.empty())
    name.tlut_name = fmt::format("_{:016x}", XXH64(palette.data(), palette.size(), 0));
  name.format_name = fmt::to_string(static_cast<int>(m_texture_format));
  return name;
}

TextureInfo::MipLevels TextureInfo::GetMipMapLevels() const
{
  // Mip data follows level 0 directly, in RAM or in the even TMEM region; the odd region
  // starts with level 1.
  MipLevelIterator first;
  first.m_parent = this;
  first.m_from_tmem = m_from_tmem;
  first.m_data = Common::SafeSubspan(m_data, m_texture_size);
  first.m_tmem_even = Common::SafeSubspan(m_tmem_even, m_texture_size);
  first.m_tmem_odd = m_tmem_odd;
  first.CreateMipLevel();

  MipLevelIterator last;
  last.m_level_index = m_limited_mip_count;
  return MipLevels(first, last);
}

u32 TextureInfo::GetFullLevelSize() const
{
  u32 total = m_texture_size;
  for (const MipLevel& level : GetMipMapLevels())
  {
    if (level.IsDataValid())
      total += level.GetTextureSize();
  }
  return total;
}

TextureInfo::MipLevel::MipLevel(u32 level, const TextureInfo& parent, std::span<const u8>* data)
    : m_level(level)
{
  m_raw_width = std::max(parent.GetRawWidth() >> level, 1u);
  m_raw_height = std::max(parent.GetRawHeight() >> level, 1u);
  m_expanded_width = Common::AlignUp(m_raw_width, parent.GetBlockWidth());
  m_expanded_height = Common::AlignUp(m_raw_height, parent.GetBlockHeight());
  m_texture_size = ImageBytes(m_expanded_width, m_expanded_height, parent.GetTextureFormat());

  const std::span<const u8> remaining = *data;
  m_ptr = remaining.data();
  m_data_valid = remaining.size() >= m_texture_size;
  *data = Common::SafeSubspan(remaining, m_texture_size);
}

TextureInfo::MipLevelIterator& TextureInfo::MipLevelIterator::operator++()
{
  ++m_level_index;
  CreateMipLevel();
  return *this;
}

void TextureInfo::MipLevelIterator::CreateMipLevel()
{
  // Position n of the iterator is mip level n + 1.
  const u32 level = m_level_index + 1;
  std::span<const u8>* source = &m_data;
  if (m_from_tmem)
    source = (level & 1) != 0 ? &m_tmem_odd : &m_tmem_even;
  m_mip_level = MipLevel(level, *m_parent, source);
}
