// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TextureCacheBase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/MemoryUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/Assets/TextureAssetUtils.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/TextureConverterShaderGen.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

std::unique_ptr<TextureCacheBase> g_texture_cache;

namespace
{
// Frames an entry may go unused before Cleanup drops it. Sonic the Fighters (in Sonic Gems
// Collection) cycles a 64-frame animation, so this must not be any shorter.
constexpr int kEntryIdleFrames = 64;

// Frames a pooled host texture may go unused before it is freed.
constexpr int kPoolIdleFrames = 3;

// Running numbers that keep the file names of dumped copies apart.
int s_xfb_dump_serial = 0;
int s_efb_dump_serial = 0;

auto& GuestMemory()
{
  return Core::System::GetInstance().GetMemory();
}

// Bytes per block of a copy in guest memory: RGBA8 blocks span two cache lines, the rest one.
constexpr u32 CopyBlockBytes(TextureFormat format)
{
  return format == TextureFormat::RGBA8 ? 64u : 32u;
}

// With full hashing (no sampling) equal hashes mean equal data, so an entry may be shared with
// other addresses holding the same texture.
bool IsFullyHashed(int sample_count, u32 texture_bytes, u32 palette_bytes)
{
  if (sample_count == 0)
    return true;
  return std::max(texture_bytes, palette_bytes) <= static_cast<u32>(sample_count) * 8;
}

bool MipmapsDisabled()
{
#ifdef __APPLE__
  return g_ActiveConfig.bNoMipmapping;
#else
  return false;
#endif
}

// EFB coordinates to internal-resolution coordinates, for the unsigned values the cache keeps.
u32 ScaledX(u32 value)
{
  return static_cast<u32>(g_framebuffer_manager->EFBToScaledX(static_cast<int>(value)));
}
u32 ScaledY(u32 value)
{
  return static_cast<u32>(g_framebuffer_manager->EFBToScaledY(static_cast<int>(value)));
}

// A rectangle given by its corner and size in texels.
MathUtil::Rectangle<int> TexelRect(u32 x, u32 y, u32 w, u32 h)
{
  return MathUtil::Rectangle<int>(static_cast<int>(x), static_cast<int>(y), static_cast<int>(x + w),
                                  static_cast<int>(y + h));
}

// Copy of `config` as a single-level render target, for entries produced by a GPU pass.
TextureConfig SingleLevelTarget(TextureConfig config)
{
  config.levels = 1;
  config.flags |= AbstractTextureFlag_RenderTarget;
  return config;
}

// One full-target triangle into `target`, sampling `source` (bound at `source_slot`) through a
// point sampler in slot 1. Used by the palette and reinterpretation passes.
void RunConversionPass(const RcTcacheEntry& target, const AbstractPipeline* pass, u32 source_slot,
                       AbstractTexture* source)
{
  g_gfx->BeginUtilityDrawing();
  g_gfx->SetAndDiscardFramebuffer(target->framebuffer.get());
  g_gfx->SetViewportAndScissor(target->texture->GetRect());
  g_gfx->SetPipeline(pass);
  g_gfx->SetTexture(source_slot, source);
  g_gfx->SetSamplerState(1, RenderState::GetPointSamplerState());
  g_gfx->Draw(0, 3);
  g_gfx->EndUtilityDrawing();
  target->texture->FinishedRendering();
}

// Gives a new EFB or XFB copy its graphics-mod name and, if `dump` is set, writes it out as a
// numbered PNG.
void NameAndMaybeDumpCopy(TCacheEntry& copy, std::string_view prefix, const std::string& size_tag,
                          int& serial, bool dump)
{
  if (g_ActiveConfig.bGraphicMods)
    copy.texture_info_name = fmt::format("{}_{}", prefix, size_tag);
  if (!dump)
    return;
  const std::string path = fmt::format("{}{}_n{:06}_{}.png", File::GetUserPath(D_DUMPTEXTURES_IDX),
                                       prefix, serial++, size_tag);
  copy.texture->Save(path, 0);
}

// What graphics mods match an EFB/XFB copy against.
FBInfo DescribeCopy(u32 width, u32 height, TextureFormat format)
{
  FBInfo info;
  info.m_width = width;
  info.m_height = height;
  info.m_texture_format = format;
  return info;
}

// The displayed part of an XFB entry, in host texels.
MathUtil::Rectangle<int> XfbDisplayRect(const TCacheEntry& xfb, u32 width, u32 height)
{
  const u32 shown_w = width * xfb.GetWidth() / xfb.native_width;
  const u32 shown_h = height * xfb.GetHeight() / xfb.native_height;
  return MathUtil::Rectangle<int>(0, 0, static_cast<int>(shown_w), static_cast<int>(shown_h));
}

// Source rectangle of an EFB copy in host framebuffer coordinates.
MathUtil::Rectangle<int> HostCopyRect(const MathUtil::Rectangle<int>& efb_rect)
{
  const MathUtil::Rectangle<int> scaled = g_framebuffer_manager->ConvertEFBRectangle(efb_rect);
  return g_gfx->ConvertFramebufferRectangle(scaled, g_framebuffer_manager->GetEFBFramebuffer());
}

AbstractTexture* ResolveCopySource(bool depth, const MathUtil::Rectangle<int>& host_rect)
{
  if (depth)
    return g_framebuffer_manager->ResolveEFBDepthTexture(host_rect);
  return g_framebuffer_manager->ResolveEFBColorTexture(host_rect);
}

// First and last readable rows of a copy, as normalised texel centres. Without the clamp bits the
// hardware reads past the EFB edge and gets whatever is on the bus; repeating the edge row stands
// in for that.
std::pair<float, float> CopyClampRows(const MathUtil::Rectangle<int>& host_rect, bool clamp_top,
                                      bool clamp_bottom, u32 efb_height, float rcp_height)
{
  const u32 top_row = clamp_top ? static_cast<u32>(host_rect.top) : 0;
  const u32 bottom_row = (clamp_bottom ? static_cast<u32>(host_rect.bottom) : efb_height) - 1;
  return {(static_cast<float>(top_row) + .5f) * rcp_height,
          (static_cast<float>(bottom_row) + .5f) * rcp_height};
}

SamplerState CopySampler(bool linear)
{
  return linear ? RenderState::GetLinearSamplerState() : RenderState::GetPointSamplerState();
}

// Width, height and row pitch of one mip level as stored in a save state.
struct LevelExtent
{
  u32 width;
  u32 height;
  u32 stride;

  static LevelExtent Of(const TextureConfig& config, u32 level)
  {
    const u32 w = std::max(config.width >> level, 1u);
    const u32 h = std::max(config.height >> level, 1u);
    return {w, h, AbstractTexture::CalculateStrideForFormat(config.format, w)};
  }
  u32 Bytes() const { return stride * height; }
};

// Detects mip chains whose smaller levels are not plain reductions of the larger ones (games use
// such chains for distance effects such as fog). Each level is compared with a box-filtered
// version of the level above it.
class MipChainChecker
{
public:
  void AddLevel(u32 width, u32 height, u32 row_length, const u8* texels)
  {
    m_levels.push_back({width, height, row_length, texels});
  }

  // `scratch` is the room for two reduced levels that CreateTextureEntry leaves after the
  // decoded chain.
  bool HasArbitraryMipmaps(u8* scratch) const
  {
    if (m_levels.size() < 2)
      return false;
    if (!g_ActiveConfig.bArbitraryMipmapDetection)
      return false;

    // Mean per-channel difference, in percent, above which the chain counts as hand-made. The
    // default sits just under the least obvious real case seen so far, the background clouds of
    // the Mario Galaxy observatory lobby.
    const auto limit = g_ActiveConfig.fArbitraryMipmapDetectionThreshold;

    u8* previous = scratch;
    u8* reduced = scratch + m_levels[1].row_length * m_levels[1].height * 4;
    float difference_total = 0.f;
    for (size_t i = 0; i + 1 < m_levels.size(); ++i)
    {
      const Level& upper = m_levels[i];
      const Level& lower = m_levels[i + 1];

      // The squared differences are summed in a u64; keep that from overflowing.
      u64 level_pixel_count = upper.width;
      level_pixel_count *= upper.height;
      ASSERT(level_pixel_count < (std::numeric_limits<u64>::max() / (255 * 255 * 4)));

      // The first step reduces the decoded base level, later ones the previous reduction. A box
      // filter is not what the artists used, but it is a lot closer than unrelated content.
      BoxReduce(i == 0 ? upper.texels : previous, upper, reduced, lower);
      difference_total += lower.DifferenceFrom(reduced);
      std::swap(previous, reduced);
    }
    return difference_total / static_cast<float>(m_levels.size() - 1) > limit;
  }

private:
  struct Level
  {
    u32 width;
    u32 height;
    u32 row_length;  // in texels
    const u8* texels;

    // Root mean square difference against `other` (laid out like this level), as a percentage of
    // the 0-255 range.
    float DifferenceFrom(const u8* other) const
    {
      // A pixel adds at most 4 * 255^2 < 2^18, so the u64 holds more than 2^46 pixels.
      u64 squared_total = 0;
      const u8* row_a = texels;
      const u8* row_b = other;
      for (u32 y = 0; y < height; ++y, row_a += row_length, row_b += row_length)
      {
        for (u32 x = 0; x < width; ++x)
        {
          const u8* a = row_a + x * 4;
          const u8* b = row_b + x * 4;
          int pixel = 0;
          for (int ch = 0; ch < 4; ++ch)
          {
            const int delta = static_cast<int>(a[ch]) - static_cast<int>(b[ch]);
            pixel += delta * delta;
          }
          squared_total += static_cast<u64>(pixel);
        }
      }
      const float mean = static_cast<float>(squared_total) / static_cast<float>(width * height * 4);
      return std::sqrt(mean) / 2.56f;
    }
  };

  // Writes the 2x2 box-filtered `src` (shaped like `src_level`) to `dst` (shaped like
  // `dst_level`), rounding to nearest.
  static void BoxReduce(const u8* src, const Level& src_level, u8* dst, const Level& dst_level)
  {
    const u32 src_pitch = src_level.row_length * 4;
    for (u32 y = 0; y < dst_level.height; ++y)
    {
      for (u32 x = 0; x < dst_level.width; ++x)
      {
        const u8* top_left = src + (2 * x + 2 * y * src_level.row_length) * 4;
        const u8* top_right = top_left + 4;
        const u8* bottom_left = top_left + src_pitch;
        const u8* bottom_right = bottom_left + 4;
        u8* out = dst + (x + y * dst_level.row_length) * 4;
        for (int ch = 0; ch < 4; ++ch)
        {
          const u32 sum = u32{top_left[ch]} + top_right[ch] + bottom_left[ch] + bottom_right[ch];
          out[ch] = static_cast<u8>((sum + 2) / 4);
        }
      }
    }
  }

  std::vector<Level> m_levels;
};
}  // namespace

// ---------------------------------------------------------------------------------------------
// TCacheEntry

TCacheEntry::TCacheEntry(std::unique_ptr<AbstractTexture> tex,
                         std::unique_ptr<AbstractFramebuffer> fb)
    : texture(std::move(tex)), framebuffer(std::move(fb))
{
}

TCacheEntry::~TCacheEntry()
{
  // Remove the back links other entries hold to this one.
  for (TCacheEntry* linked : references)
    linked->references.erase(this);

  ASSERT_MSG(VIDEO, g_texture_cache, "Texture cache destroyed before TCacheEntry was destroyed");
  // The host texture goes back to the pool.
  g_texture_cache->ReleaseToPool(this);
}

bool TCacheEntry::OverlapsMemoryRange(u32 range_address, u32 range_size) const
{
  return addr < range_address + range_size && range_address < addr + size_in_bytes;
}

void TCacheEntry::DoState(PointerWrap& p)
{
  const auto visit = [&p](auto&... field) { (p.Do(field), ...); };
  visit(addr, size_in_bytes, base_hash, hash, format, memory_stride, is_efb_copy, is_custom_tex,
        may_have_overlapping_textures, invalidated, has_arbitrary_mips, should_force_safe_hashing,
        is_xfb_copy, is_xfb_container, id, reference_changed, native_width, native_height,
        native_levels, frameCount);
}

u32 TCacheEntry::NumBlocksX() const
{
  const u32 block_w = static_cast<u32>(TexDecoder_GetBlockWidthInTexels(format.texfmt));
  return Common::AlignUp(native_width, block_w) / block_w;
}

u32 TCacheEntry::NumBlocksY() const
{
  const u32 block_h = static_cast<u32>(TexDecoder_GetBlockHeightInTexels(format.texfmt));
  return Common::AlignUp(native_height, block_h) / block_h;
}

u32 TCacheEntry::BytesPerRow() const
{
  return CopyBlockBytes(format.texfmt) * NumBlocksX();
}

void TCacheEntry::SetXfbCopy(u32 stride)
{
  is_xfb_copy = true;
  is_efb_copy = false;
  is_xfb_container = false;
  memory_stride = stride;
  ASSERT_MSG(VIDEO, memory_stride >= BytesPerRow(), "Memory stride is too small");
  size_in_bytes = NumBlocksY() * memory_stride;
}

void TCacheEntry::SetEfbCopy(u32 stride)
{
  is_efb_copy = true;
  is_xfb_copy = false;
  is_xfb_container = false;
  memory_stride = stride;
  ASSERT_MSG(VIDEO, memory_stride >= BytesPerRow(), "Memory stride is too small");
  size_in_bytes = NumBlocksY() * memory_stride;
}

void TCacheEntry::SetNotCopy()
{
  is_xfb_container = false;
  is_xfb_copy = false;
  is_efb_copy = false;
}

int TCacheEntry::HashSampleSize() const
{
  // XFB copies always hash every byte.
  return should_force_safe_hashing ? 0 : g_ActiveConfig.iSafeTextureCache_ColorSamples;
}

u64 TCacheEntry::CalculateHash() const
{
  const u32 row_bytes = BytesPerRow();
  const u32 samples = static_cast<u32>(HashSampleSize());

  // Textures preloaded into TMEM are hashed from guest RAM all the same, which is not right.
  const u8* cursor = GuestMemory().GetPointerForRange(addr, size_in_bytes);
  if (memory_stride == row_bytes)
    return Common::GetHash64(cursor, size_in_bytes, samples);

  // Strided copy: hash the rows one by one, skipping the gaps, and fold the row hashes together.
  // The prime multiplier keeps identical rows from cancelling out.
  const u32 rows = NumBlocksY();
  // At least four samples per row, so sampling doesn't end up looking only at the left edge.
  const u32 samples_per_row = samples == 0 ? 0 : std::max(samples / rows, 4u);
  u64 folded = size_in_bytes;
  for (u32 row = 0; row < rows; ++row, cursor += memory_stride)
    folded = (folded * 397) ^ Common::GetHash64(cursor, row_bytes, samples_per_row);
  return folded;
}

TextureCacheBase::TexPoolEntry::TexPoolEntry(std::unique_ptr<AbstractTexture> tex,
                                             std::unique_ptr<AbstractFramebuffer> fb)
    : texture(std::move(tex)), framebuffer(std::move(fb))
{
}

// ---------------------------------------------------------------------------------------------
// Lifetime and configuration

TextureCacheBase::TextureCacheBase()
{
  SetBackupConfig(g_ActiveConfig);

  // Scratch space for CPU decoding: one 2048x2048 RGBA8 level to begin with, grown on demand.
  m_temp_size = 2048 * 2048 * 4;
  m_temp = static_cast<u8*>(Common::AllocateAlignedMemory(m_temp_size, 16));

  TexDecoder_SetTexFmtOverlayOptions(m_backup_config.texfmt_overlay,
                                     m_backup_config.texfmt_overlay_center);
  TMEM::InvalidateAll();
}

TextureCacheBase::~TextureCacheBase()
{
  Common::FreeAlignedMemory(m_temp);
  m_temp = nullptr;
}

bool TextureCacheBase::Initialize()
{
  if (CreateUtilityTextures())
    return true;
  PanicAlertFmt("Failed to create utility textures.");
  return false;
}

void TextureCacheBase::Shutdown()
{
  // Drop the queued copies so Invalidate doesn't write them out.
  m_pending_efb_copies.clear();
  HiresTexture::Shutdown();
  // Host textures have to go before the GPU context does.
  Invalidate();
}

void TextureCacheBase::CheckTempSize(size_t required_size)
{
  if (m_temp_size >= required_size)
    return;
  Common::FreeAlignedMemory(m_temp);
  m_temp_size = required_size;
  m_temp = static_cast<u8*>(Common::AllocateAlignedMemory(m_temp_size, 16));
}

void TextureCacheBase::Invalidate()
{
  FlushEFBCopies();
  TMEM::InvalidateAll();

  m_bound_textures.fill({});
  m_textures_by_hash.clear();
  m_textures_by_address.clear();
  m_texture_pool.clear();
}

static u32 ModChangeCount(const VideoConfig& config)
{
  return config.graphics_mod_config ? config.graphics_mod_config->GetChangeCount() : 0;
}

void TextureCacheBase::OnConfigChanged(const VideoConfig& config)
{
  const BackupConfig& had = m_backup_config;

  if (std::tie(config.bHiresTextures, config.bCacheHiresTextures) !=
      std::tie(had.hires_textures, had.cache_hires_textures))
  {
    HiresTexture::Update();
  }

  // These settings change what a cache entry would contain, so everything is rebuilt. (Overkill
  // for some of them.)
  const u32 mod_changes = ModChangeCount(config);
  const bool contents_changed =
      std::tie(config.iSafeTextureCache_ColorSamples, config.bTexFmtOverlayEnable,
               config.bTexFmtOverlayCenter, config.bHiresTextures,
               config.bEnableGPUTextureDecoding, config.bDisableCopyToVRAM,
               config.bArbitraryMipmapDetection, config.bGraphicMods, mod_changes) !=
      std::tie(had.color_samples, had.texfmt_overlay, had.texfmt_overlay_center,
               had.hires_textures, had.gpu_texture_decoding, had.disable_vram_copies,
               had.arbitrary_mipmap_detection, had.graphics_mods, had.graphics_mod_change_count);
  if (contents_changed)
  {
    Invalidate();
    TexDecoder_SetTexFmtOverlayOptions(config.bTexFmtOverlayEnable, config.bTexFmtOverlayCenter);
  }

  SetBackupConfig(config);
}

void TextureCacheBase::SetBackupConfig(const VideoConfig& config)
{
  m_backup_config = BackupConfig{
      .color_samples = config.iSafeTextureCache_ColorSamples,
      .texfmt_overlay = config.bTexFmtOverlayEnable,
      .texfmt_overlay_center = config.bTexFmtOverlayCenter,
      .hires_textures = config.bHiresTextures,
      .cache_hires_textures = config.bCacheHiresTextures,
      .copy_cache_enable = false,
      .stereo_3d = config.stereo_mode != StereoMode::Off,
      .efb_mono_depth = config.bStereoEFBMonoDepth,
      .gpu_texture_decoding = config.bEnableGPUTextureDecoding,
      .disable_vram_copies = config.bDisableCopyToVRAM,
      .arbitrary_mipmap_detection = config.bArbitraryMipmapDetection,
      .graphics_mods = config.bGraphicMods,
      .graphics_mod_change_count = ModChangeCount(config),
  };
}

void TextureCacheBase::Cleanup(int frame)
{
  for (auto it = m_textures_by_address.begin(); it != m_textures_by_address.end();)
  {
    TCacheEntry& entry = *it->second;
    if (entry.frameCount == FRAMECOUNT_INVALID)
    {
      // Used this frame: start its idle clock now.
      entry.frameCount = frame;
      ++it;
      continue;
    }
    if (frame <= entry.frameCount + kEntryIdleFrames)
    {
      ++it;
      continue;
    }

    // Idle for too long. Plain textures can be decoded again from RAM, so they go. A copy can't
    // be rebuilt, so it goes only once the RAM under it has changed, and since hashing costs time
    // that is checked just once every kEntryIdleFrames frames.
    bool drop = true;
    if (entry.IsCopy())
      drop = (frame - entry.frameCount) % kEntryIdleFrames == 1 && entry.hash != entry.CalculateHash();
    it = drop ? InvalidateTexture(it) : std::next(it);
  }

  for (auto it = m_texture_pool.begin(); it != m_texture_pool.end();)
  {
    int& last_used = it->second.frameCount;
    if (last_used == FRAMECOUNT_INVALID)
      last_used = frame;
    if (frame > last_used + kPoolIdleFrames)
      it = m_texture_pool.erase(it);
    else
      ++it;
  }
}

void TextureCacheBase::OnFrameEnd()
{
  // Write queued copies out every frame; a game running without waiting for vblank would
  // otherwise pile them up without bound.
  FlushEFBCopies();
  Cleanup(g_presenter->FrameCount());
}

bool TextureCacheBase::DidLinkedAssetsChange(const TCacheEntry& entry)
{
  if (!entry.hires_texture)
    return false;
  return entry.hires_texture->LoadTexture()->GetLoadTime() > entry.last_load_time;
}

// ---------------------------------------------------------------------------------------------
// Conversions on the GPU

std::pair<RcTcacheEntry, u32>
TextureCacheBase::CreatePaletteEntryWithOffset(const RcTcacheEntry& entry, const u8* palette)
{
  DEBUG_ASSERT(g_backend_info.bSupportsPaletteConversion);

  RcTcacheEntry converted = AllocateCacheEntry(SingleLevelTarget(entry->texture->GetConfig()));
  if (!converted)
    return {nullptr, 0};

  const TCacheEntry& source = *entry;
  converted->SetGeneralParameters(source.addr, source.size_in_bytes, source.format,
                                  source.should_force_safe_hashing);
  converted->SetDimensions(source.native_width, source.native_height, 1);
  converted->SetHashes(source.base_hash, source.hash);
  converted->frameCount = FRAMECOUNT_INVALID;
  converted->should_force_safe_hashing = false;
  converted->SetNotCopy();
  converted->may_have_overlapping_textures = source.may_have_overlapping_textures;

  // 16 or 256 palette entries of two bytes each.
  const u32 palette_bytes = source.format.texfmt == TextureFormat::I4 ? 32 : 512;
  u32 offset = 0;
  if (!g_vertex_manager->UploadTexelBuffer(palette, palette_bytes,
                                           TexelBufferFormat::TEXEL_BUFFER_FORMAT_R16_UINT, &offset))
  {
    ERROR_LOG_FMT(VIDEO, "Texel buffer upload of {} bytes failed", palette_bytes);
  }

  m_textures_by_address.emplace(converted->addr, converted);
  return {converted, offset};
}

void TextureCacheBase::RenderPaletteEntry(u32 texel_buffer_offset, const RcTcacheEntry& entry,
                                          AbstractTexture* texture, TLUTFormat tlutfmt)
{
  const AbstractPipeline* lookup = g_shader_cache->GetPaletteConversionPipeline(tlutfmt);
  if (lookup == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to get conversion pipeline for format {}", tlutfmt);
    return;
  }

  // Constant block of the palette conversion shader.
  struct PaletteConstants
  {
    float index_scale;
    u32 palette_offset;
    std::array<u32, 2> unused;
  };
  static_assert(sizeof(PaletteConstants) == 16);
  PaletteConstants constants = {};
  // The source holds indices as normalised colours; scale them back to 4 or 8 bits.
  constants.index_scale = entry->format.texfmt == TextureFormat::I4 ? 15.0f : 255.0f;
  constants.palette_offset = texel_buffer_offset;
  g_vertex_manager->UploadUtilityUniforms(&constants, sizeof(constants));

  RunConversionPass(entry, lookup, 1, texture);
}

RcTcacheEntry TextureCacheBase::CreateReinterpretEntry(const RcTcacheEntry& existing_entry,
                                                       TextureFormat new_format)
{
  RcTcacheEntry result = AllocateCacheEntry(SingleLevelTarget(existing_entry->texture->GetConfig()));
  if (!result)
    return {};

  const TCacheEntry& source = *existing_entry;
  result->SetGeneralParameters(source.addr, source.size_in_bytes, new_format,
                               source.should_force_safe_hashing);
  result->SetDimensions(source.native_width, source.native_height, 1);
  result->SetHashes(source.base_hash, source.hash);
  result->frameCount = source.frameCount;
  result->SetNotCopy();
  result->is_efb_copy = source.is_efb_copy;
  result->may_have_overlapping_textures = source.may_have_overlapping_textures;

  m_textures_by_address.emplace(result->addr, result);
  return result;
}

void TextureCacheBase::RenderReinterpretEntry(const RcTcacheEntry& entry, AbstractTexture* texture,
                                              TextureFormat old_format, TextureFormat new_format)
{
  const AbstractPipeline* converter =
      g_shader_cache->GetTextureReinterpretPipeline(old_format, new_format);
  if (converter == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to obtain texture reinterpreting pipeline from format {} to {}",
                  old_format, new_format);
    return;
  }
  RunConversionPass(entry, converter, 0, texture);
}

void TextureCacheBase::ScaleTextureCacheEntryTo(RcTcacheEntry& entry, u32 new_width, u32 new_height)
{
  if (entry->GetWidth() == new_width && entry->GetHeight() == new_height)
    return;

  const u32 limit = g_backend_info.MaxTextureSize;
  if (new_width > limit || new_height > limit)
  {
    ERROR_LOG_FMT(VIDEO, "Texture too big, width = {}, height = {}", new_width, new_height);
    return;
  }

  const TextureConfig scaled_config(new_width, new_height, 1, entry->GetNumLayers(), 1,
                                    AbstractTextureFormat::RGBA8, AbstractTextureFlag_RenderTarget,
                                    AbstractTextureType::Texture_2DArray);
  std::optional<TexPoolEntry> replacement = AllocateTexture(scaled_config);
  if (!replacement)
  {
    ERROR_LOG_FMT(VIDEO, "Scaling failed due to texture allocation failure");
    return;
  }

  // Both rectangles cover their whole texture, so no coordinate conversion is needed.
  AbstractTexture* const old_texture = entry->texture.get();
  g_gfx->ScaleTexture(replacement->framebuffer.get(), replacement->texture->GetConfig().GetRect(),
                      old_texture, old_texture->GetConfig().GetRect());
  entry->texture.swap(replacement->texture);
  entry->framebuffer.swap(replacement->framebuffer);

  // `replacement` now holds the previous texture; keep it for reuse.
  const TextureConfig old_config = replacement->texture->GetConfig();
  m_texture_pool.emplace(old_config, TexPoolEntry(std::move(replacement->texture),
                                                  std::move(replacement->framebuffer)));
}

// ---------------------------------------------------------------------------------------------
// Save states

bool TextureCacheBase::CheckReadbackTexture(u32 width, u32 height, AbstractTextureFormat format)
{
  if (m_readback_texture)
  {
    const TextureConfig& have = m_readback_texture->GetConfig();
    if (have.width >= width && have.height >= height && have.format == format)
      return true;
  }

  const TextureConfig wanted(std::max(width, 128u), std::max(height, 128u), 1, 1, 1, format, 0,
                             AbstractTextureType::Texture_2DArray);
  m_readback_texture.reset();
  m_readback_texture = g_gfx->CreateStagingTexture(StagingTextureType::Readback, wanted);
  return m_readback_texture != nullptr;
}

void TextureCacheBase::SerializeTexture(AbstractTexture* tex, const TextureConfig& config,
                                        PointerWrap& p)
{
  // A measuring pass needs only the sizes, so it skips the readback.
  const bool measuring = p.IsMeasureMode();
  p.Do(config);

  if (!measuring && !CheckReadbackTexture(config.width, config.height, config.format))
  {
    PanicAlertFmt("Failed to create staging texture for serialization");
    return;
  }

  u32 total_bytes = 0;
  for (u32 layer = 0; layer < config.layers; ++layer)
  {
    for (u32 level = 0; level < config.levels; ++level)
      total_bytes += LevelExtent::Of(config, level).Bytes();
  }

  // Reserve the bytes inside the state buffer and read straight into them; no bounce buffer.
  u8* out = p.DoExternal(total_bytes);
  if (measuring)
    return;
  if (p.IsMeasureMode())
  {
    // DoExternal switched to measuring because the buffer is too small.
    ERROR_LOG_FMT(VIDEO, "Couldn't acquire {} bytes for serializing texture.", total_bytes);
    return;
  }

  for (u32 layer = 0; layer < config.layers; ++layer)
  {
    for (u32 level = 0; level < config.levels; ++level)
    {
      const MathUtil::Rectangle<int> rect = tex->GetConfig().GetMipRect(level);
      m_readback_texture->CopyFromTexture(tex, rect, layer, level, rect);
      const LevelExtent extent = LevelExtent::Of(config, level);
      m_readback_texture->ReadTexels(rect, out, extent.stride);
      out += extent.Bytes();
    }
  }
}

std::optional<TextureCacheBase::TexPoolEntry> TextureCacheBase::DeserializeTexture(PointerWrap& p)
{
  TextureConfig config;
  p.Do(config);

  // The texel data follows as one block of stored_bytes bytes.
  u32 stored_bytes = 0;
  const u8* stored = p.DoExternal(stored_bytes);
  if (!p.IsReadMode() || stored_bytes == 0)
    return std::nullopt;

  std::optional<TexPoolEntry> restored = AllocateTexture(config);
  if (!restored)
  {
    PanicAlertFmt("Failed to create texture for deserialization");
    return std::nullopt;
  }

  size_t consumed = 0;
  for (u32 layer = 0; layer < config.layers; ++layer)
  {
    for (u32 level = 0; level < config.levels; ++level)
    {
      const LevelExtent extent = LevelExtent::Of(config, level);
      const size_t bytes = size_t{extent.stride} * extent.height;
      if (consumed + bytes > stored_bytes)
      {
        ERROR_LOG_FMT(VIDEO, "Insufficient texture data for layer {} level {}", layer, level);
        return restored;
      }
      restored->texture->Load(level, extent.width, extent.height, extent.width, stored + consumed,
                              bytes);
      consumed += bytes;
    }
  }
  return restored;
}

void TextureCacheBase::DoState(PointerWrap& p)
{
  // Queued copies reach RAM before saving or loading either way.
  FlushEFBCopies();

  p.Do(m_last_entry_id);

  const bool saving = p.IsWriteMode() || p.IsMeasureMode();
  if (saving)
    DoSaveState(p);
  else
    DoLoadState(p);
}

void TextureCacheBase::DoSaveState(PointerWrap& p)
{
  FlushStaleBinds();

  // Plain textures are decoded from RAM again after loading, so storing them would only bloat the
  // state (and its decompression time). Copies can't be rebuilt from RAM, nor can invalidated
  // entries, whose RAM has since changed.
  const auto worth_saving = [](const RcTcacheEntry& e) { return e->IsCopy() || e->invalidated; };

  // Entries get ids in the order they are first met while walking the maps below. The vector
  // keeps that order for writing; the map finds an entry's id.
  std::map<const TCacheEntry*, u32> ids;
  std::vector<TCacheEntry*> save_order;
  const auto id_of = [&ids, &save_order](const RcTcacheEntry& e) -> u32 {
    const auto [slot, added] = ids.try_emplace(e.get(), static_cast<u32>(ids.size()));
    if (added)
      save_order.push_back(e.get());
    return slot->second;
  };

  std::vector<std::pair<u32, u32>> by_address;
  std::vector<std::pair<u64, u32>> by_hash;
  std::vector<std::pair<u32, u32>> bindings;
  if (Config::Get(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE))
  {
    for (const auto& [address, e] : m_textures_by_address)
    {
      if (worth_saving(e))
        by_address.emplace_back(address, id_of(e));
    }
    for (const auto& [hash_value, e] : m_textures_by_hash)
    {
      if (worth_saving(e))
        by_hash.emplace_back(hash_value, id_of(e));
    }
    for (u32 unit = 0; unit < m_bound_textures.size(); ++unit)
    {
      const RcTcacheEntry& e = m_bound_textures[unit];
      if (e && worth_saving(e))
        bindings.emplace_back(unit, id_of(e));
    }
  }

  u32 entry_count = static_cast<u32>(save_order.size());
  p.Do(entry_count);
  for (TCacheEntry* e : save_order)
  {
    SerializeTexture(e->texture.get(), e->texture->GetConfig(), p);
    e->DoState(p);
  }
  p.DoMarker("TextureCacheEntries");

  // Links can form cycles, so they are stored after every entry exists. Each linked pair is
  // written once, in whichever order it is met first.
  std::set<std::pair<u32, u32>> links;
  for (const auto& [e, own_id] : ids)
  {
    for (const TCacheEntry* other : e->references)
    {
      const auto found = ids.find(other);
      if (found == ids.end())
        continue;
      if (!links.contains({found->second, own_id}))
        links.insert({own_id, found->second});
    }
  }

  const auto write_pairs = [&p](const auto& pairs) {
    u32 count = static_cast<u32>(pairs.size());
    p.Do(count);
    for (auto [first, second] : pairs)
    {
      p.Do(first);
      p.Do(second);
    }
  };
  write_pairs(links);
  write_pairs(by_address);
  write_pairs(by_hash);
  write_pairs(bindings);

  // The readback texture may live in host-mapped GPU memory; don't hold on to it.
  m_readback_texture.reset();
}

void TextureCacheBase::DoLoadState(PointerWrap& p)
{
  // Entries restored so far, by saved id. A missing id gives a null entry.
  std::map<u32, RcTcacheEntry> restored;
  RcTcacheEntry none;
  const auto lookup = [&restored, &none](u32 saved_id) -> RcTcacheEntry& {
    const auto it = restored.find(saved_id);
    return it != restored.end() ? it->second : none;
  };

  // Only a real load replaces the cache. Other modes parse the data and keep nothing, so every
  // lookup misses and nothing below is inserted.
  const bool committing = p.IsReadMode();
  if (committing)
    Invalidate();

  u32 count = 0;
  p.Do(count);
  for (u32 index = 0; index < count; ++index)
  {
    // The entry is built even without a usable texture, to read past its fields.
    std::optional<TexPoolEntry> host = DeserializeTexture(p);
    auto e = host ? std::make_shared<TCacheEntry>(std::move(host->texture),
                                                  std::move(host->framebuffer)) :
                    std::make_shared<TCacheEntry>(nullptr, nullptr);
    e->textures_by_hash_iter = m_textures_by_hash.end();
    e->DoState(p);
    if (e->texture && committing)
      restored.emplace(index, e);
  }
  p.DoMarker("TextureCacheEntries");

  p.Do(count);
  for (u32 i = 0; i < count; ++i)
  {
    u32 first = 0;
    u32 second = 0;
    p.Do(first);
    p.Do(second);
    RcTcacheEntry a = lookup(first);
    RcTcacheEntry b = lookup(second);
    if (a && b)
      a->CreateReference(b.get());
  }

  p.Do(count);
  for (u32 i = 0; i < count; ++i)
  {
    u32 address = 0;
    u32 saved_id = 0;
    p.Do(address);
    p.Do(saved_id);
    if (RcTcacheEntry& e = lookup(saved_id))
      m_textures_by_address.emplace(address, e);
  }

  p.Do(count);
  for (u32 i = 0; i < count; ++i)
  {
    u64 hash_value = 0;
    u32 saved_id = 0;
    p.Do(hash_value);
    p.Do(saved_id);
    if (RcTcacheEntry& e = lookup(saved_id))
      e->textures_by_hash_iter = m_textures_by_hash.emplace(hash_value, e);
  }

  for (RcTcacheEntry& binding : m_bound_textures)
    binding.reset();

  p.Do(count);
  for (u32 i = 0; i < count; ++i)
  {
    u32 unit = 0;
    u32 saved_id = 0;
    p.Do(unit);
    p.Do(saved_id);
    if (RcTcacheEntry& e = lookup(saved_id))
      m_bound_textures[unit] = e;
  }
}

// ---------------------------------------------------------------------------------------------
// Partial updates

RcTcacheEntry TextureCacheBase::DoPartialTextureUpdates(RcTcacheEntry& entry_to_update,
                                                        const u8* palette, TLUTFormat tlutfmt)
{
  TCacheEntry& target = *entry_to_update;

  // The flag is set on new entries and wherever a copy lands on part of one. Once the copies
  // have been applied there is nothing to do until the next copy.
  if (!target.may_have_overlapping_textures)
    return entry_to_update;
  target.may_have_overlapping_textures = false;

  const TextureFormat wanted = target.format.texfmt;
  const bool paletted = IsColorIndexed(wanted);

  // Copies themselves are not patched; no game is known to need it.
  if (target.IsCopy())
    return entry_to_update;

  if (target.IsLocked())
  {
    // Would need the entry and its texture cloned first.
    PanicAlertFmt("TextureCache: PartialTextureUpdates of locked textures is not implemented");
    return {};
  }

  const u32 block_w = static_cast<u32>(TexDecoder_GetBlockWidthInTexels(wanted));
  const u32 block_h = static_cast<u32>(TexDecoder_GetBlockHeightInTexels(wanted));
  const u32 block_bytes = block_w * block_h * static_cast<u32>(TexDecoder_GetTexelSizeInNibbles(wanted)) / 2;
  const u32 blocks_per_row = (target.native_width + block_w - 1) / block_w;
  const u32 row_pitch = blocks_per_row * block_bytes;

  auto [cursor, stop] = FindOverlappingTextures(target.addr, target.size_in_bytes);
  while (cursor != stop)
  {
    // The map slot itself: a converted version of the copy replaces it below.
    RcTcacheEntry& source = cursor->second;
    const bool candidate = source != entry_to_update && source->IsCopy() &&
                           !source->references.contains(&target) &&
                           source->OverlapsMemoryRange(target.addr, target.size_in_bytes) &&
                           source->memory_stride == row_pitch;
    if (!candidate)
    {
      ++cursor;
      continue;
    }

    if (source->hash != source->CalculateHash())
    {
      // The RAM under the copy has changed, so the copy is no use to anything any more.
      cursor = InvalidateTexture(cursor);
      continue;
    }

    if (!IsCompatibleTextureFormat(wanted, source->format.texfmt))
    {
      // Skip copies that can't be made to match on the GPU either.
      if (!CanReinterpretTextureOnGPU(wanted, source->format.texfmt))
      {
        ++cursor;
        continue;
      }
      RcTcacheEntry reinterpreted = CreateReinterpretEntry(source, wanted);
      RenderReinterpretEntry(reinterpreted, source->texture.get(), source->format.texfmt,
                             reinterpreted->format.texfmt);
      if (reinterpreted)
        source = reinterpreted;
    }

    if (paletted)
    {
      auto [converted, offset] = CreatePaletteEntryWithOffset(source, palette);
      RenderPaletteEntry(offset, converted, source->texture.get(), tlutfmt);
      if (!converted)
      {
        ++cursor;
        continue;
      }
      // Link the copy to the texture so this update isn't applied twice, and count the copy as
      // used this frame.
      source->CreateReference(&target);
      source->frameCount = FRAMECOUNT_INVALID;
      source = converted;
    }

    // Texel position of the copy inside the texture. Ordinary textures have no stride, so a
    // copy that starts before the texture is cut off at the top only.
    u32 src_x = 0;
    u32 src_y = 0;
    u32 dst_x = 0;
    u32 dst_y = 0;
    if (source->addr >= target.addr)
    {
      const u32 blocks_in = (source->addr - target.addr) / block_bytes;
      dst_x = blocks_in % blocks_per_row * block_w;
      dst_y = blocks_in / blocks_per_row * block_h;
    }
    else
    {
      const u32 blocks_before = (target.addr - source->addr) / block_bytes;
      const u32 column = (0u - blocks_before) % blocks_per_row;
      dst_x = column * block_w;
      src_y = (blocks_before + column) / blocks_per_row * block_h;
    }
    u32 span_w = std::min(source->native_width - src_x, target.native_width - dst_x);
    u32 span_h = std::min(source->native_height - src_y, target.native_height - dst_y);

    const bool either_scaled =
        target.native_width != target.GetWidth() || target.native_height != target.GetHeight() ||
        source->native_width != source->GetWidth() || source->native_height != source->GetHeight();
    if (either_scaled)
    {
      // Bring both to the current internal resolution and work in scaled texels.
      ScaleTextureCacheEntryTo(entry_to_update, ScaledX(target.native_width),
                               ScaledY(target.native_height));
      ScaleTextureCacheEntryTo(source, ScaledX(source->native_width),
                               ScaledY(source->native_height));
      src_x = ScaledX(src_x);
      src_y = ScaledY(src_y);
      dst_x = ScaledX(dst_x);
      dst_y = ScaledY(dst_y);
      span_w = ScaledX(span_w);
      span_h = ScaledY(span_h);
    }

    // The backends don't clamp, and coordinates outside a texture can reset the GPU, so a copy
    // that doesn't fit is skipped.
    const bool fits = src_x + span_w <= source->GetWidth() && src_y + span_h <= source->GetHeight() &&
                      dst_x + span_w <= target.GetWidth() && dst_y + span_h <= target.GetHeight();
    if (!fits)
    {
      ++cursor;
      continue;
    }

    const MathUtil::Rectangle<int> from = TexelRect(src_x, src_y, span_w, span_h);
    const MathUtil::Rectangle<int> to = TexelRect(dst_x, dst_y, span_w, span_h);
    // A stereo copy and a mono texture (or the reverse) only share the layers both have.
    const u32 shared_layers = std::min(source->GetNumLayers(), target.GetNumLayers());
    for (u32 layer = 0; layer < shared_layers; ++layer)
      target.texture->CopyRectangleFromTexture(source->texture.get(), from, layer, 0, to, layer, 0);

    if (paletted)
    {
      // The converted texture was made for this update only. (Converting while copying would
      // save a pass, but this path is rare.)
      cursor = InvalidateTexture(cursor);
      continue;
    }

    source->CreateReference(&target);
    source->frameCount = FRAMECOUNT_INVALID;
    ++cursor;
  }

  return entry_to_update;
}

// ---------------------------------------------------------------------------------------------
// Sampling and binding

SamplerState TextureCacheBase::GetSamplerState(u32 index, float custom_tex_scale, bool custom_tex,
                                               bool has_arbitrary_mips)
{
  const TexMode0& game_mode = bpmem.tex.GetUnit(index).texMode0;
  const bool game_uses_mips = game_mode.mipmap_filter != MipMode::None;

  SamplerState state = {};
  state.Generate(bpmem, index);

  switch (g_ActiveConfig.texture_filtering_mode)
  {
  case TextureFilteringMode::Nearest:
    state.tm0.min_filter = FilterMode::Near;
    state.tm0.mag_filter = FilterMode::Near;
    state.tm0.mipmap_filter = FilterMode::Near;
    break;
  case TextureFilteringMode::Linear:
    state.tm0.min_filter = FilterMode::Linear;
    state.tm0.mag_filter = FilterMode::Linear;
    state.tm0.mipmap_filter = game_uses_mips ? FilterMode::Linear : FilterMode::Near;
    break;
  default:
    break;
  }

  // Custom textures may bring more levels than the game asked for.
  if (custom_tex)
    state.tm1.max_lod = 255;

  // Textures the game point-samples both ways are usually 2D UI elements, which anisotropic
  // filtering would smear off their pixel grid, so the enhancement leaves them alone.
  const bool point_sampled =
      game_mode.min_filter == FilterMode::Near && game_mode.mag_filter == FilterMode::Near;
  if (g_ActiveConfig.iMaxAnisotropy != AnisotropicFilteringMode::Default && !point_sampled)
    state.tm0.anisotropic_filtering = static_cast<u32>(std::to_underlying(g_ActiveConfig.iMaxAnisotropy));

  if (state.tm0.anisotropic_filtering != 0)
  {
    // Drivers agree on anisotropy only on top of bilinear or trilinear filtering
    // (EXT_texture_filter_anisotropic), so force one of those.
    state.tm0.min_filter = FilterMode::Linear;
    state.tm0.mag_filter = FilterMode::Linear;
    if (game_uses_mips)
      state.tm0.mipmap_filter = FilterMode::Linear;
  }

  if (has_arbitrary_mips && game_uses_mips)
  {
    // Hand-made levels (fog and similar tricks) have to switch in at the native distance at any
    // internal resolution, so bias the LOD by the scale factor, net of any custom upscale.
    const float ratio = static_cast<float>(g_framebuffer_manager->GetEFBScale()) / custom_tex_scale;
    const s32 bias_shift = static_cast<s32>(std::log2(ratio) * 256.f);
    state.tm0.lod_bias = std::clamp<s32>(state.tm0.lod_bias + bias_shift, -32768, 32767);
    // Anisotropy pushes levels further out again, so it is off for these.
    state.tm0.anisotropic_filtering = 0;
  }

  return state;
}

void TextureCacheBase::BindTextures(BitSet32 used_textures,
                                    const std::array<SamplerState, 8>& samplers)
{
  auto& shader_constants = Core::System::GetInstance().GetPixelShaderManager();
  for (u32 unit = 0; unit < m_bound_textures.size(); ++unit)
  {
    const TCacheEntry* bound = m_bound_textures[unit].get();
    if (!used_textures[unit] || bound == nullptr)
      continue;

    g_gfx->SetTexture(unit, bound->texture.get());
    shader_constants.SetTexDims(static_cast<int>(unit), bound->native_width, bound->native_height);

    const SamplerState& sampler = samplers[unit];
    g_gfx->SetSamplerState(unit, sampler);
    shader_constants.SetSamplerState(static_cast<int>(unit), sampler.tm0.hex, sampler.tm1.hex);
  }

  TMEM::FinalizeBinds(used_textures);
}

void TextureCacheBase::FlushStaleBinds()
{
  for (u32 unit = 0; unit < m_bound_textures.size(); ++unit)
  {
    if (!TMEM::IsCached(unit))
      m_bound_textures[unit].reset();
  }
}

// ---------------------------------------------------------------------------------------------
// Texture lookup

TCacheEntry* TextureCacheBase::Load(u32 stage)
{
  TCacheEntry* entry = LoadImpl(stage, false);
  if (entry == nullptr)
    return nullptr;
  if (!DidLinkedAssetsChange(*entry))
    return entry;

  // The custom texture behind the entry was reloaded; build the entry again.
  InvalidateTexture(GetTexCacheIter(entry));
  return LoadImpl(stage, true);
}

TCacheEntry* TextureCacheBase::LoadImpl(u32 stage, bool force_reload)
{
  // While the stage's registers are unchanged, last time's texture is still the right one.
  if (!force_reload && TMEM::IsValid(stage) && m_bound_textures[stage])
  {
    TCacheEntry* bound = m_bound_textures[stage].get();

    // If TMEM would still hold it, use it without hashing, even if RAM has been overwritten since
    // (the entry may even be invalidated): the hardware samples its cached copy too. Spyro: A
    // Hero's Tail relies on that in its bloom effect.
    if (TMEM::IsCached(stage))
      return bound;

    // Otherwise only if the RAM behind it is unchanged. (TMEM-preloaded textures are not hashed
    // correctly by this.)
    if (!bound->invalidated && bound->base_hash == bound->CalculateHash())
      return bound;
  }

  const TextureInfo info = TextureInfo::FromStage(stage);
  RcTcacheEntry entry = GetTexture(g_ActiveConfig.iSafeTextureCache_ColorSamples, info);
  if (!entry)
    return nullptr;

  entry->frameCount = FRAMECOUNT_INVALID;
  if (entry->texture_info_name.empty() && g_ActiveConfig.bGraphicMods)
  {
    entry->texture_info_name = info.CalculateTextureName().GetFullName();

    GraphicsModActionData::TextureLoad load_event{entry->texture_info_name};
    for (const auto& action : g_graphics_mod_manager->GetTextureLoadActions(entry->texture_info_name))
      action->OnTextureLoad(&load_event);
  }

  const u32 unit = info.GetStage();
  m_bound_textures[unit] = entry;

  // Record the residency, so later loads can tell whether TMEM still holds this texture (and
  // invalidated entries stay tracked until replaced).
  TMEM::Bind(unit, static_cast<int>(entry->NumBlocksX()), static_cast<int>(entry->NumBlocksY()),
             entry->GetNumLevels() > 1,
             entry->format == TextureFormat::RGBA8);
  return entry.get();
}

RcTcacheEntry TextureCacheBase::GetTexture(const int textureCacheSafetyColorSampleSize,
                                           const TextureInfo& texture_info)
{
  const int samples = textureCacheSafetyColorSampleSize;
  if (!texture_info.IsDataValid())
    return {};

  const TextureFormat texfmt = texture_info.GetTextureFormat();
  const TLUTFormat tlutfmt = texture_info.GetTlutFormat();
  const TextureAndTLUTFormat full_format(texfmt, tlutfmt);
  const std::optional<u32> palette = texture_info.GetPaletteSize();

  // A palette in a format the hardware doesn't have.
  if (palette && !IsValidTLUTFormat(tlutfmt))
    return {};

  const u32 bytes_per_block = texture_info.GetBlockWidth() * texture_info.GetBlockHeight() *
                              static_cast<u32>(TexDecoder_GetTexelSizeInNibbles(texfmt)) / 2;

  // (Lookup is by address, but a TMEM texture has no meaningful one, so it can alias a normal
  // texture's entry.)

  // A FIFO recording tracks which memory textures read, separately from the hashing below.
  if (OpcodeDecoder::g_record_fifo_data && !texture_info.IsFromTmem())
  {
    Core::System::GetInstance().GetFifoRecorder().UseMemory(texture_info.GetRawAddress(),
                                                            texture_info.GetFullLevelSize(),
                                                            MemoryUpdate::Type::TextureMap);
  }

  // The hash names the entry and also feeds dump and custom texture file names. (For RGBA8
  // preloaded in TMEM it covers too much of the low bank and none of the high one.)
  const u64 base_hash =
      Common::GetHash64(texture_info.GetData(), texture_info.GetTextureSize(), static_cast<u32>(samples));
  const u32 palette_size = palette.value_or(0);
  const u64 full_hash =
      palette ? base_hash ^ Common::GetHash64(texture_info.GetTlutAddress(), *palette, static_cast<u32>(samples)) :
                base_hash;

  // Look for a usable entry at this address.
  //
  // One entry per address is normally enough: a texture that changed rarely comes back, and
  // reusing an old entry (with its pooled host texture) beats piling up new ones. Some games use
  // one address for several textures at the same time, though: Metroid Prime stacks fonts in one
  // texture and picks characters through the palette, and Castlevania 3 reads the same data as
  // 1024x1024 and as 1024x256. So the least recently used entry is recycled only if it wasn't
  // used this frame; otherwise another entry is added. A copy made to this address must always be
  // used, or making it was wasted effort.
  const auto [first, last] = m_textures_by_address.equal_range(texture_info.GetRawAddress());
  TexAddrCache::iterator recycle = first;
  int recycle_frame = std::numeric_limits<int>::max();
  TexAddrCache::iterator copy_to_convert = m_textures_by_address.end();
  TexAddrCache::iterator copy_to_reinterpret = m_textures_by_address.end();

  for (auto it = first; it != last;)
  {
    RcTcacheEntry& slot = it->second;

    // (Rogue Squadron 3 and Twin Snakes sample old XFB copies as ordinary textures, which would
    // need converting here; that is not done yet.)

    // Strided copies and partly overwritten copies are never sampled directly.
    const bool whole_efb_copy =
        slot->IsEfbCopy() && slot->native_width == texture_info.GetRawWidth() &&
        slot->native_height == texture_info.GetRawHeight() &&
        slot->memory_stride == slot->BytesPerRow() && !slot->may_have_overlapping_textures;
    if (whole_efb_copy)
    {
      // A copy's formats mean something different from texture formats, so it gets its own
      // rules. Its hash covers the texels only, and a paletted read needs palette conversion.
      const bool hash_ok =
          base_hash == slot->hash && (!palette || g_backend_info.bSupportsPaletteConversion);
      if (!hash_ok && !IsPlayingBackFifologWithBrokenEFBCopies)
      {
        // A copy that is no use now practically never becomes useful later.
        it = InvalidateTexture(it);
        continue;
      }

      if (!IsCompatibleTextureFormat(slot->format.texfmt, texfmt))
      {
        // Same bits per texel but another layout (Spider-Man: Shattered Dimensions copies as B8
        // and samples as IA4) is fixed on the GPU after the loop, in case a converted entry
        // already exists. Anything else falls back to the RAM copy.
        if (CanReinterpretTextureOnGPU(slot->format.texfmt, texfmt))
          copy_to_reinterpret = it;
        ++it;
        continue;
      }

      // A copy that fits as is beats one that needs converting.
      copy_to_convert = m_textures_by_address.end();
      if (!palette || !g_backend_info.bSupportsPaletteConversion)
        return slot;

      // Paletted: the palette is applied after the loop.
      copy_to_convert = it;
    }
    else if (!slot->IsEfbCopy() && slot->hash == full_hash && slot->format == full_format &&
             slot->native_levels >= texture_info.GetLevelCount() &&
             slot->native_width == texture_info.GetRawWidth() &&
             slot->native_height == texture_info.GetRawHeight())
    {
      // Exact match; bring in any copies made over it since.
      slot = DoPartialTextureUpdates(slot, texture_info.GetTlutAddress(), tlutfmt);
      if (slot)
      {
        slot->texture->FinishedRendering();
        return slot;
      }
    }

    // Track the least recently used plain texture for recycling. Copies stay (they may feed
    // partial updates, or an XFB may still be shown or read as a texture). Paletted textures
    // with the same texel data count as one texture, which matters for games that cycle palettes
    // (Sonic the Fighters again).
    const bool same_texels = palette && slot->base_hash == base_hash;
    if (slot->frameCount != FRAMECOUNT_INVALID && slot->frameCount < recycle_frame &&
        !slot->IsCopy() && !same_texels)
    {
      recycle_frame = slot->frameCount;
      recycle = it;
    }
    ++it;
  }

  if (copy_to_reinterpret != m_textures_by_address.end())
  {
    const RcTcacheEntry& copy = copy_to_reinterpret->second;
    RcTcacheEntry result = CreateReinterpretEntry(copy, texfmt);
    RenderReinterpretEntry(result, copy->texture.get(), copy->format.texfmt, result->format.texfmt);

    // A reinterpreted copy can go through the palette as well.
    if (copy_to_reinterpret == copy_to_convert && result)
    {
      const RcTcacheEntry reinterpreted = result;
      u32 offset = 0;
      std::tie(result, offset) =
          CreatePaletteEntryWithOffset(reinterpreted, texture_info.GetTlutAddress());
      RenderPaletteEntry(offset, result, reinterpreted->texture.get(), tlutfmt);
    }

    if (result)
      return result;
  }

  if (copy_to_convert != m_textures_by_address.end())
  {
    const RcTcacheEntry& copy = copy_to_convert->second;
    auto [converted, offset] = CreatePaletteEntryWithOffset(copy, texture_info.GetTlutAddress());
    RenderPaletteEntry(offset, converted, copy->texture.get(), tlutfmt);
    if (converted)
      return converted;
  }

  // A fully hashed texture can be shared with any address that holds the same data. Tales of
  // Symphonia's menus use over 500 small textures of which only about 70 differ.
  if (IsFullyHashed(samples, texture_info.GetTextureSize(), palette_size))
  {
    const auto [hash_first, hash_last] = m_textures_by_hash.equal_range(full_hash);
    for (auto it = hash_first; it != hash_last; ++it)
    {
      RcTcacheEntry& slot = it->second;
      // Everything except the address has to match.
      const bool matches = slot->format == full_format &&
                           slot->native_levels >= texture_info.GetLevelCount() &&
                           slot->native_width == texture_info.GetRawWidth() &&
                           slot->native_height == texture_info.GetRawHeight();
      if (!matches)
        continue;
      slot = DoPartialTextureUpdates(slot, texture_info.GetTlutAddress(), tlutfmt);
      if (slot)
      {
        slot->texture->FinishedRendering();
        return slot;
      }
    }
  }

  // Unless every entry here was used this frame, the stalest one makes way (its host texture
  // goes to the pool, ready for the new entry).
  if (recycle_frame != std::numeric_limits<int>::max())
    InvalidateTexture(recycle);

  std::shared_ptr<HiresTexture> hires;
  bool hires_arbitrary_mips = false;
  bool skip_dump = false;
  std::shared_ptr<VideoCommon::CustomTextureData> custom_data = nullptr;
  VideoCommon::CustomAsset::TimeType load_time = {};
  if (g_ActiveConfig.bHiresTextures)
  {
    hires = HiresTexture::Search(texture_info);
    if (hires)
    {
      hires_arbitrary_mips = hires->HasArbitraryMipmaps();
      const auto resource = hires->LoadTexture();
      load_time = resource->GetLoadTime();
      custom_data = resource->GetData();
      if (custom_data &&
          !VideoCommon::ValidateTextureData(hires->GetId(), *custom_data,
                                            texture_info.GetRawWidth(), texture_info.GetRawHeight()))
      {
        custom_data = nullptr;
        load_time = {};
      }
      // A replaced texture isn't dumped.
      skip_dump = true;
    }
  }

  std::string mod_name;
  if (g_ActiveConfig.bGraphicMods)
  {
    mod_name = texture_info.CalculateTextureName().GetFullName();
    GraphicsModActionData::TextureCreate create_event{
        mod_name, texture_info.GetRawWidth(), texture_info.GetRawHeight(), nullptr, nullptr};
    for (const auto& action : g_graphics_mod_manager->GetTextureCreateActions(mod_name))
      action->OnTextureCreate(&create_event);
  }

  RcTcacheEntry created = CreateTextureEntry(
      TextureCreationInfo{base_hash, full_hash, bytes_per_block, palette_size}, texture_info,
      samples, custom_data.get(), hires_arbitrary_mips, skip_dump);
  created->hires_texture = std::move(hires);
  created->last_load_time = load_time;
  created->texture_info_name = std::move(mod_name);
  return created;
}

// Custom texture data is taken to have a single slice, since each texture becomes one layer of a
// texture array; validation after loading makes sure of that.
RcTcacheEntry TextureCacheBase::CreateTextureEntry(
    const TextureCreationInfo& creation_info, const TextureInfo& texture_info,
    const int safety_color_sample_size, VideoCommon::CustomTextureData* custom_texture_data,
    const bool custom_arbitrary_mipmaps, bool skip_texture_dump)
{
  const bool single_level = MipmapsDisabled();

  RcTcacheEntry entry;
  if (custom_texture_data != nullptr)
  {
    const auto& custom_levels = custom_texture_data->m_slices[0].m_levels;
    const u32 level_count = single_level ? 1 : static_cast<u32>(custom_levels.size());
    const auto& top = custom_levels[0];
    entry = AllocateCacheEntry(TextureConfig(top.width, top.height, level_count, 1, 1, top.format,
                                             0, AbstractTextureType::Texture_2DArray));
    if (!entry) [[unlikely]]
      return entry;

    const u32 upload_count = std::min(level_count, static_cast<u32>(custom_levels.size()));
    for (u32 level = 0; level < upload_count; ++level)
    {
      const auto& src = custom_levels[level];
      entry->texture->Load(level, src.width, src.height, src.row_length, src.data.data(),
                           src.data.size());
    }

    entry->has_arbitrary_mips = custom_arbitrary_mipmaps;
    entry->is_custom_tex = true;
  }
  else
  {
    const u32 level_count = single_level ? 1 : texture_info.GetLevelCount();
    const u32 width = texture_info.GetRawWidth();
    const u32 height = texture_info.GetRawHeight();
    const u32 padded_w = texture_info.GetExpandedWidth();
    const u32 padded_h = texture_info.GetExpandedHeight();

    entry = AllocateCacheEntry(TextureConfig(width, height, level_count, 1, 1,
                                             AbstractTextureFormat::RGBA8, 0,
                                             AbstractTextureType::Texture_2DArray));
    if (!entry) [[unlikely]]
      return entry;

    const TextureFormat texfmt = texture_info.GetTextureFormat();
    const bool rgba8_in_tmem = texture_info.IsFromTmem() && texfmt == TextureFormat::RGBA8;
    // RGBA8 preloaded in TMEM keeps its two halves in different banks. Joining them means a copy,
    // and with a copy anyway the CPU might as well decode, so that case stays on the CPU.
    const bool gpu_decode = g_ActiveConfig.UseGPUTextureDecoding() && !rgba8_in_tmem;
    const u32 block_w = texture_info.GetBlockWidth();
    const auto decoded_on_gpu = [&](u32 level, const u8* src, u32 src_size, u32 w, u32 h, u32 pw,
                                    u32 ph) {
      return gpu_decode &&
             DecodeTextureOnGPU(entry, level, src, src_size, texfmt, w, h, pw, ph,
                                creation_info.bytes_per_block * (pw / block_w),
                                texture_info.GetTlutAddress(), texture_info.GetTlutFormat());
    };

    MipChainChecker mip_checker;
    // Next free byte of m_temp; only CPU decoding writes there.
    u8* write_pos = nullptr;

    if (!decoded_on_gpu(0, texture_info.GetData(), texture_info.GetTextureSize(), width, height,
                        padded_w, padded_h))
    {
      const size_t base_bytes = size_t{padded_w} * sizeof(u32) * padded_h;
      // All levels go in one buffer, followed by room for the two reductions the mip check makes
      // (a quarter and a sixteenth of the base level).
      size_t needed = base_bytes;
      size_t level_bytes = base_bytes;
      for (u32 level = 1; level < texture_info.GetLevelCount(); ++level)
      {
        level_bytes /= 4;
        needed += level_bytes;
      }
      needed += base_bytes * 5 / 16;
      CheckTempSize(needed);

      write_pos = m_temp;
      if (rgba8_in_tmem)
      {
        TexDecoder_DecodeRGBA8FromTmem(write_pos, texture_info.GetData(),
                                       texture_info.GetTmemOddAddress(), static_cast<int>(padded_w),
                                       static_cast<int>(padded_h));
      }
      else
      {
        TexDecoder_Decode(write_pos, texture_info.GetData(), static_cast<int>(padded_w),
                          static_cast<int>(padded_h), texfmt,
                          texture_info.GetTlutAddress(), texture_info.GetTlutFormat());
      }
      entry->texture->Load(0, width, height, padded_w, write_pos, base_bytes);
      mip_checker.AddLevel(width, height, padded_w, write_pos);
      write_pos += base_bytes;
    }

    for (const auto& mip : texture_info.GetMipMapLevels())
    {
      if (single_level)
        break;
      if (!mip.IsDataValid())
      {
        ERROR_LOG_FMT(VIDEO, "Trying to use an invalid mipmap address {:#010x}",
                      texture_info.GetRawAddress());
        continue;
      }

      const u32 mip_w = mip.GetRawWidth();
      const u32 mip_h = mip.GetRawHeight();
      const u32 mip_pw = mip.GetExpandedWidth();
      const u32 mip_ph = mip.GetExpandedHeight();
      if (decoded_on_gpu(mip.GetLevel(), mip.GetData(), mip.GetTextureSize(), mip_w, mip_h, mip_pw,
                         mip_ph))
      {
        continue;
      }

      // m_temp was sized for every level above.
      const u32 mip_bytes = static_cast<u32>(mip_pw * sizeof(u32) * mip_ph);
      TexDecoder_Decode(write_pos, mip.GetData(), static_cast<int>(mip_pw), static_cast<int>(mip_ph),
                        texfmt,
                        texture_info.GetTlutAddress(), texture_info.GetTlutFormat());
      entry->texture->Load(mip.GetLevel(), mip_w, mip_h, mip_pw, write_pos, mip_bytes);
      mip_checker.AddLevel(mip_w, mip_h, mip_pw, write_pos);
      write_pos += mip_bytes;
    }

    entry->has_arbitrary_mips = mip_checker.HasArbitraryMipmaps(write_pos);

    if (g_ActiveConfig.bDumpTextures && !skip_texture_dump && level_count > 0)
    {
      const std::string dump_name = texture_info.CalculateTextureName().GetFullName();
      // The base level and the smaller ones have separate switches.
      const u32 first_level = g_ActiveConfig.bDumpBaseTextures ? 0 : 1;
      const u32 end_level = g_ActiveConfig.bDumpMipmapTextures ? level_count : 1;
      for (u32 level = first_level; level < end_level; ++level)
        m_texture_dumper.DumpTexture(*entry->texture, dump_name, level, entry->has_arbitrary_mips);
    }
  }

  const auto slot = m_textures_by_address.emplace(texture_info.GetRawAddress(), entry);
  if (IsFullyHashed(safety_color_sample_size, texture_info.GetTextureSize(),
                    creation_info.palette_size))
  {
    entry->textures_by_hash_iter = m_textures_by_hash.emplace(creation_info.full_hash, entry);
  }

  entry->SetGeneralParameters(
      texture_info.GetRawAddress(), texture_info.GetTextureSize(),
      TextureAndTLUTFormat(texture_info.GetTextureFormat(), texture_info.GetTlutFormat()), false);
  entry->SetDimensions(texture_info.GetRawWidth(), texture_info.GetRawHeight(),
                       texture_info.GetLevelCount());
  entry->SetHashes(creation_info.base_hash, creation_info.full_hash);
  entry->memory_stride = entry->BytesPerRow();
  entry->SetNotCopy();

  INCSTAT(g_stats.num_textures_uploaded);
  SETSTAT(g_stats.num_textures_alive, static_cast<int>(m_textures_by_address.size()));

  entry = DoPartialTextureUpdates(slot->second, texture_info.GetTlutAddress(),
                                  texture_info.GetTlutFormat());
  // Needed after a partial update or a GPU decode.
  entry->texture->FinishedRendering();
  return entry;
}

// ---------------------------------------------------------------------------------------------
// XFB

RcTcacheEntry TextureCacheBase::GetXFBTexture(u32 address, u32 width, u32 height, u32 stride,
                                              MathUtil::Rectangle<int>* display_rect)
{
  // XFBs are linear rather than tiled, so the size is simply rows times stride.
  const u32 total_size = height * stride;
  const u8* src_data = GuestMemory().GetPointerForRange(address, total_size);
  if (src_data == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Trying to load XFB texture from invalid address {:#010x}", address);
    return {};
  }

  // A VRAM version that is still current can be used as is (a stitched one is refreshed first).
  RcTcacheEntry xfb = GetXFBFromCache(address, width, height, stride);
  if (xfb && !xfb->IsLocked())
  {
    if (xfb->is_xfb_container)
    {
      StitchXFBCopy(xfb);
      xfb->texture->FinishedRendering();
    }
    *display_rect = XfbDisplayRect(*xfb, width, height);
    return xfb;
  }

  // Otherwise decode guest RAM into a new texture.
  xfb = AllocateCacheEntry(TextureConfig(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8,
                                         AbstractTextureFlag_RenderTarget,
                                         AbstractTextureType::Texture_2DArray));
  TCacheEntry& fresh = *xfb;
  fresh.SetGeneralParameters(address, total_size,
                             TextureAndTLUTFormat(TextureFormat::XFB, TLUTFormat::IA8), true);
  fresh.SetDimensions(width, height, 1);
  fresh.SetXfbCopy(stride);
  const u64 ram_hash = fresh.CalculateHash();
  fresh.SetHashes(ram_hash, ram_hash);
  fresh.is_xfb_container = true;
  fresh.is_custom_tex = false;
  fresh.may_have_overlapping_textures = false;
  fresh.frameCount = FRAMECOUNT_INVALID;

  const bool on_gpu =
      g_ActiveConfig.UseGPUTextureDecoding() &&
      DecodeTextureOnGPU(xfb, 0, src_data, total_size, fresh.format.texfmt, width, height, width,
                         height, stride, s_tex_mem.data(), fresh.format.tlutfmt);
  if (!on_gpu)
  {
    const u32 decoded_bytes = static_cast<u32>(width * height * sizeof(u32));
    CheckTempSize(decoded_bytes);
    TexDecoder_DecodeXFB(m_temp, src_data, width, height, stride);
    fresh.texture->Load(0, width, height, width, m_temp, decoded_bytes);
  }

  // Paste in the VRAM copies that cover parts of it.
  StitchXFBCopy(xfb);
  fresh.texture->FinishedRendering();

  // Cache it, so the next frame can use it again.
  m_textures_by_address.emplace(fresh.addr, xfb);
  SETSTAT(g_stats.num_textures_alive, static_cast<int>(m_textures_by_address.size()));
  INCSTAT(g_stats.num_textures_uploaded);

  if (g_ActiveConfig.bDumpXFBTarget || g_ActiveConfig.bGraphicMods)
  {
    NameAndMaybeDumpCopy(fresh, XFB_DUMP_PREFIX, fmt::format("{}x{}", width, height),
                         s_xfb_dump_serial, g_ActiveConfig.bDumpXFBTarget);
  }

  *display_rect = XfbDisplayRect(fresh, width, height);
  return xfb;
}

RcTcacheEntry TextureCacheBase::GetXFBFromCache(u32 address, u32 width, u32 height, u32 stride)
{
  const auto [first, last] = m_textures_by_address.equal_range(address);
  for (auto it = first; it != last;)
  {
    const TCacheEntry& candidate = *it->second;
    // Only the stride must match exactly; a larger copy can be shown in part.
    const bool fits = candidate.is_xfb_copy && candidate.memory_stride == stride &&
                      candidate.native_width >= width && candidate.native_height >= height &&
                      !candidate.may_have_overlapping_textures;
    if (!fits)
    {
      ++it;
      continue;
    }
    if (candidate.hash == candidate.CalculateHash() && !candidate.reference_changed)
      return it->second;

    // Its RAM has changed, or a copy it was stitched from has: rebuild rather than reuse.
    it = InvalidateTexture(it);
  }
  return {};
}

void TextureCacheBase::StitchXFBCopy(RcTcacheEntry& stitched_entry)
{
  TCacheEntry& dest = *stitched_entry;

  // Copies can overlap one another (Rogue Leader does it), so they are applied in the order they
  // were made, just as the later write wins in guest RAM. (Tracking partial invalidation of XFB
  // copies would save some of this copying.)
  std::vector<TCacheEntry*> pieces;
  bool upscale = false;

  auto [cursor, stop] = FindOverlappingTextures(dest.addr, dest.size_in_bytes);
  while (cursor != stop)
  {
    // Strides must match the VI request, so interlaced modes find no pieces. That is fine while
    // progressive output is forced; without it XFB2RAM should be on. Interlaced stitching would
    // need a shader that takes every other line.
    const RcTcacheEntry& piece = cursor->second;
    if (piece == stitched_entry || !piece->IsCopy() ||
        !piece->OverlapsMemoryRange(dest.addr, dest.size_in_bytes) ||
        piece->memory_stride != dest.memory_stride)
    {
      ++cursor;
      continue;
    }
    if (piece->hash != piece->CalculateHash())
    {
      // The RAM under the copy has changed, so it is of no further use.
      cursor = InvalidateTexture(cursor);
      continue;
    }
    // Heights can't be compared because of Y scaling.
    if (piece->native_width != piece->GetWidth())
      upscale = true;
    pieces.emplace_back(piece.get());
    ++cursor;
  }

  if (pieces.empty())
    return;

  std::ranges::sort(pieces, {}, &TCacheEntry::id);

  // Go to internal resolution only when an upscaled piece would otherwise lose detail.
  if (upscale)
  {
    ScaleTextureCacheEntryTo(stitched_entry, ScaledX(dest.native_width),
                             ScaledY(dest.native_height));
  }

  const FramebufferManager& fbm = *g_framebuffer_manager;
  for (TCacheEntry* piece : pieces)
  {
    // XFB texels are two bytes; the address gap becomes a texel offset into whichever surface
    // starts later.
    int src_x = 0;
    int src_y = 0;
    int dst_x = 0;
    int dst_y = 0;
    if (piece->addr >= dest.addr)
    {
      const u32 offset = (piece->addr - dest.addr) / 2;
      dst_x = static_cast<int>(offset % dest.native_width);
      dst_y = static_cast<int>(offset / dest.native_width);
    }
    else
    {
      const u32 offset = (dest.addr - piece->addr) / 2;
      src_x = static_cast<int>(offset % piece->native_width);
      src_y = static_cast<int>(offset / piece->native_width);
    }

    const int span_w = static_cast<int>(std::min(piece->native_width - static_cast<u32>(src_x),
                                                 dest.native_width - static_cast<u32>(dst_x)));
    const int span_h = static_cast<int>(std::min(piece->native_height - static_cast<u32>(src_y),
                                                 dest.native_height - static_cast<u32>(dst_y)));
    int src_w = span_w;
    int src_h = span_h;
    int dst_w = span_w;
    int dst_h = span_h;

    // Convert each side to the resolution its texture is at.
    if (piece->native_width != piece->GetWidth())
    {
      src_x = fbm.EFBToScaledX(src_x);
      src_y = fbm.EFBToScaledY(src_y);
      src_w = fbm.EFBToScaledX(src_w);
      src_h = fbm.EFBToScaledY(src_h);
    }
    if (upscale)
    {
      dst_x = fbm.EFBToScaledX(dst_x);
      dst_y = fbm.EFBToScaledY(dst_y);
      dst_w = fbm.EFBToScaledX(dst_w);
      dst_h = fbm.EFBToScaledY(dst_h);
    }

    // As with partial updates, never hand the backend coordinates outside a texture.
    const bool fits = static_cast<u32>(src_x + src_w) <= piece->GetWidth() &&
                      static_cast<u32>(src_y + src_h) <= piece->GetHeight() &&
                      static_cast<u32>(dst_x + dst_w) <= dest.GetWidth() &&
                      static_cast<u32>(dst_y + dst_h) <= dest.GetHeight();
    if (!fits)
      continue;

    const MathUtil::Rectangle<int> from(src_x, src_y, src_x + src_w, src_y + src_h);
    const MathUtil::Rectangle<int> to(dst_x, dst_y, dst_x + dst_w, dst_y + dst_h);
    if (from.GetWidth() != to.GetWidth() || from.GetHeight() != to.GetHeight())
    {
      // One side is not at internal resolution: scale while copying.
      g_gfx->ScaleTexture(dest.framebuffer.get(), to, piece->texture.get(), from);
    }
    else
    {
      const u32 shared_layers = std::min(piece->GetNumLayers(), dest.GetNumLayers());
      for (u32 layer = 0; layer < shared_layers; ++layer)
        dest.texture->CopyRectangleFromTexture(piece->texture.get(), from, layer, 0, to, layer, 0);
    }

    // Link the two so the piece isn't applied again, and count it as used this frame.
    piece->CreateReference(&dest);
    piece->frameCount = FRAMECOUNT_INVALID;
  }
}

// ---------------------------------------------------------------------------------------------
// EFB copies

std::array<u32, 3>
TextureCacheBase::GetRAMCopyFilterCoefficients(const CopyFilterCoefficients::Values& coefficients)
{
  // The seven taps fold into three rows: taps 0-1 are the line above, 2-4 the line itself and
  // 5-6 the line below. Precomputing the sums keeps the backends simpler.
  const auto tap = [&coefficients](size_t i) { return static_cast<u32>(coefficients[i]); };
  return {tap(0) + tap(1), tap(2) + tap(3) + tap(4), tap(5) + tap(6)};
}

std::array<u32, 3>
TextureCacheBase::GetVRAMCopyFilterCoefficients(const CopyFilterCoefficients::Values& coefficients)
{
  std::array<u32, 3> rows = GetRAMCopyFilterCoefficients(coefficients);
  // "Disable copy filter" affects only the VRAM copy, so games sensitive to the RAM copy of the
  // XFB are left alone. Some games scale brightness through the coefficients, so the total weight
  // is kept, all on the centre row.
  if (g_ActiveConfig.bDisableCopyFilter)
    rows = {0, rows[0] + rows[1] + rows[2], 0};
  return rows;
}

bool TextureCacheBase::AllCopyFilterCoefsNeeded(const std::array<u32, 3>& coefficients)
{
  // Rows with zero weight need not be sampled at all.
  return coefficients[0] != 0 || coefficients[2] != 0;
}

bool TextureCacheBase::CopyFilterCanOverflow(const std::array<u32, 3>& coefficients)
{
  // The weights normally add up to at most 64. Above that the result is clamped to 0-255, but
  // from 128 on the sum reaches 512 and wraps around to 0, which the shader has to emulate.
  const u32 total = coefficients[0] + coefficients[1] + coefficients[2];
  return total >= 128;
}

void TextureCacheBase::CopyRenderTargetToTexture(
    u32 dstAddr, EFBCopyFormat dstFormat, u32 width, u32 height, u32 dstStride, bool is_depth_copy,
    const MathUtil::Rectangle<int>& srcRect, bool isIntensity, bool scaleByHalf, float y_scale,
    float gamma, bool clamp_top, bool clamp_bottom,
    const CopyFilterCoefficients::Values& filter_coefficients)
{
  // A copy can be written to guest RAM, encoded by shaders and decoded again when a texture reads
  // it, which keeps CPU reads and edits of the copy working. It can also become a VRAM texture,
  // which skips the round trip and can stay at internal resolution. With both, RAM is what
  // counts: the VRAM texture is used only while the RAM it covers still has the hash recorded
  // here. Every copy stalls the CPU until the GPU catches up.
  const bool is_xfb_copy = !is_depth_copy && !isIntensity && dstFormat == EFBCopyFormat::XFB;
  bool to_vram = g_backend_info.bSupportsCopyToVram && !g_ActiveConfig.bDisableCopyToVRAM;
  const bool skip_ram =
      is_xfb_copy ? g_ActiveConfig.bSkipXFBCopyToRam : g_ActiveConfig.bSkipEFBCopyToRam;
  // Without a VRAM texture, RAM is the only place the copy can go.
  const bool to_ram = !skip_ram || !to_vram;

  // tex_w/tex_h is the size in guest memory; the host texture can be bigger (internal
  // resolution) and XFB copies keep the EFB height for the virtual XFB blit.
  const u32 divisor = scaleByHalf ? 2 : 1;
  const u32 tex_w = width / divisor;
  const u32 tex_h = height / divisor;
  u32 scaled_tex_w = ScaledX(width) / divisor;
  u32 scaled_tex_h = ScaledY(height) / divisor;
  if (!is_xfb_copy && !g_ActiveConfig.bCopyEFBScaled)
  {
    scaled_tex_w = tex_w;
    scaled_tex_h = tex_h;
  }

  // Layout in guest memory, in blocks of the base format.
  const TextureFormat baseFormat = TexDecoder_GetEFBCopyBaseFormat(dstFormat);
  const u32 block_h = static_cast<u32>(TexDecoder_GetBlockHeightInTexels(baseFormat));
  const u32 block_w = static_cast<u32>(TexDecoder_GetBlockWidthInTexels(baseFormat));
  const u32 num_blocks_y = Common::AlignUp(tex_h, block_h) / block_h;
  const u32 num_blocks_x = Common::AlignUp(tex_w, block_w) / block_w;
  const u32 bytes_per_row = num_blocks_x * CopyBlockBytes(baseFormat);
  const u32 covered_range = num_blocks_y * dstStride;

  u8* const dst = GuestMemory().GetPointerForRange(dstAddr, covered_range);
  if (dst == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Trying to copy from EFB to invalid address {:#010x}", dstAddr);
    return;
  }

  if (g_ActiveConfig.bGraphicMods)
  {
    const FBInfo fb_info = DescribeCopy(tex_w, tex_h, baseFormat);
    if (is_xfb_copy)
    {
      for (const auto& action : g_graphics_mod_manager->GetXFBActions(fb_info))
        action->BeforeXFB();
    }
    else
    {
      bool skip_copy = false;
      GraphicsModActionData::PreEFB pre{tex_w, tex_h, &skip_copy, &scaled_tex_w, &scaled_tex_h};
      for (const auto& action : g_graphics_mod_manager->GetEFBActions(fb_info))
        action->BeforeEFB(&pre);
      if (skip_copy)
      {
        if (to_ram)
          UninitializeEFBMemory(dst, dstStride, bytes_per_row, num_blocks_y);
        return;
      }
    }
  }

  if (dstStride < bytes_per_row)
  {
    // The RAM copy comes out scrambled, as on hardware; this is most likely a game bug or a
    // CPU/bounding box emulation problem. A VRAM copy would come out unscrambled, which is wrong,
    // so it is skipped and textures read the scrambled RAM data (or black without RAM copies).
    ERROR_LOG_FMT(VIDEO, "Memory stride too small ({} < {})", dstStride, bytes_per_row);
    to_vram = false;
  }

  // Box filtering, halving and bringing a higher internal resolution down to 1x all sample
  // linearly. (Only exact at 2x; other factors would need a wider filter.)
  const bool linear_filter =
      !is_depth_copy &&
      (scaleByHalf || g_framebuffer_manager->GetEFBScale() != 1 || y_scale > 1.0f);

  RcTcacheEntry entry;
  if (to_vram)
  {
    entry = AllocateCacheEntry(TextureConfig(
        scaled_tex_w, scaled_tex_h, 1, g_framebuffer_manager->GetEFBLayers(), 1,
        AbstractTextureFormat::RGBA8, AbstractTextureFlag_RenderTarget,
        AbstractTextureType::Texture_2DArray));
    if (entry)
    {
      TCacheEntry& copy = *entry;
      copy.SetGeneralParameters(dstAddr, 0, baseFormat, is_xfb_copy);
      copy.SetDimensions(tex_w, tex_h, 1);
      copy.frameCount = FRAMECOUNT_INVALID;
      if (is_xfb_copy)
      {
        copy.should_force_safe_hashing = true;
        copy.SetXfbCopy(dstStride);
      }
      else
      {
        copy.SetEfbCopy(dstStride);
      }
      copy.may_have_overlapping_textures = false;
      copy.is_custom_tex = false;

      CopyEFBToCacheEntry(entry, is_depth_copy, srcRect, scaleByHalf, linear_filter, dstFormat,
                          isIntensity, gamma, clamp_top, clamp_bottom,
                          GetVRAMCopyFilterCoefficients(filter_coefficients));

      if (g_ActiveConfig.bGraphicMods && !is_xfb_copy)
      {
        const FBInfo fb_info = DescribeCopy(tex_w, tex_h, baseFormat);
        GraphicsModActionData::PostEFB post;
        for (const auto& action : g_graphics_mod_manager->GetEFBActions(fb_info))
        {
          action->AfterEFB(&post);
          if (post.material)
            ApplyMaterialToCacheEntry(*post.material, entry.get());
        }
      }

      // Name the copy for graphics mods and dump it when asked to. An XFB copy with only EFB
      // dumping enabled is dumped as an EFB copy.
      if (is_xfb_copy && (g_ActiveConfig.bDumpXFBTarget || g_ActiveConfig.bGraphicMods))
      {
        NameAndMaybeDumpCopy(copy, XFB_DUMP_PREFIX, fmt::format("{}x{}", tex_w, tex_h),
                             s_xfb_dump_serial, g_ActiveConfig.bDumpXFBTarget);
      }
      else if (g_ActiveConfig.bDumpEFBTarget || g_ActiveConfig.bGraphicMods)
      {
        NameAndMaybeDumpCopy(copy, EFB_DUMP_PREFIX,
                             fmt::format("{}x{}_{}", tex_w, tex_h, static_cast<int>(baseFormat)),
                             s_efb_dump_serial, g_ActiveConfig.bDumpEFBTarget);
      }
    }
  }

  if (to_ram)
  {
    const std::array<u32, 3> ram_taps = GetRAMCopyFilterCoefficients(filter_coefficients);
    const PixelFormat src_format = bpmem.zcontrol.pixel_format;
    const EFBCopyParams params(src_format, dstFormat, is_depth_copy, isIntensity,
                               AllCopyFilterCoefsNeeded(ram_taps), CopyFilterCanOverflow(ram_taps),
                               gamma != 1.0);

    std::unique_ptr<AbstractStagingTexture> staging = GetEFBCopyStagingTexture();
    if (staging)
    {
      CopyEFB(staging.get(), params, tex_w, bytes_per_row, num_blocks_y, dstStride, srcRect,
              scaleByHalf, linear_filter, y_scale, gamma, clamp_top, clamp_bottom, ram_taps);

      // The encoder writes one 32-bit texel per four bytes of a row.
      const u32 encoded_width = bytes_per_row / 4;
      // Without a VRAM copy there is nothing to carry the hash until later, so write now.
      if (!to_vram || !g_ActiveConfig.bDeferEFBCopies)
      {
        WriteEFBCopyToRAM(dst, encoded_width, num_blocks_y, dstStride, std::move(staging));
      }
      else
      {
        entry->pending_efb_copy = std::move(staging);
        entry->pending_efb_copy_width = encoded_width;
        entry->pending_efb_copy_height = num_blocks_y;
        m_pending_efb_copies.push_back(entry);
      }
    }
  }
  else if (is_xfb_copy)
  {
    UninitializeXFBMemory(dst, dstStride, bytes_per_row, num_blocks_y);
  }
  else
  {
    UninitializeEFBMemory(dst, dstStride, bytes_per_row, num_blocks_y);
  }

  // Entries under the copy: those fully covered, or with another stride, are dropped; partly
  // covered ones with the same stride are marked for a partial update. (Overlapping strided copies
  // are not detected perfectly.)
  const bool strided = dstStride != bytes_per_row;
  auto [cursor, stop] = FindOverlappingTextures(dstAddr, covered_range);
  while (cursor != stop)
  {
    TCacheEntry& other = *cursor->second;

    // Anything stitched from an XFB copy at this address has to be rebuilt.
    if (other.addr == dstAddr && other.is_xfb_copy)
    {
      for (TCacheEntry* linked : other.references)
        linked->reference_changed = true;
    }

    if (!other.OverlapsMemoryRange(dstAddr, covered_range))
    {
      ++cursor;
      continue;
    }

    const u32 overlap = std::min(other.addr + other.size_in_bytes, dstAddr + covered_range) -
                        std::max(other.addr, dstAddr);
    const bool fully_covered = other.size_in_bytes == overlap;
    const bool replaced = !to_vram || other.memory_stride != dstStride ||
                          (fully_covered && (!strided || other.addr == dstAddr));
    if (replaced)
    {
      // A queued copy entirely under this one can be thrown away rather than written later.
      cursor = InvalidateTexture(cursor, true);
      continue;
    }

    // XFB containers keep the flag clear; otherwise they could never be reused and their
    // textures would leak for several frames.
    if (!other.is_xfb_container)
      other.may_have_overlapping_textures = true;

    // Overlapping XFB copies (Rogue Squadron 2, Texas Hold'em on WiiWare) change the RAM under an
    // earlier copy. That doesn't matter for XFB2Tex, but XFB2RAM compares hashes, so take the
    // earlier copy's hash again now.
    if (other.is_xfb_copy && to_ram)
      other.hash = other.CalculateHash();

    // Equal hashes no longer prove equal contents once part of a texture was overwritten.
    if (other.textures_by_hash_iter != m_textures_by_hash.end())
    {
      m_textures_by_hash.erase(other.textures_by_hash_iter);
      other.textures_by_hash_iter = m_textures_by_hash.end();
    }
    ++cursor;
  }

  if (OpcodeDecoder::g_record_fifo_data)
  {
    // For a FIFO log, the copied rows are GPU output rather than data the game uploaded.
    for (u32 row = 0, address = dstAddr; row < num_blocks_y; ++row, address += dstStride)
    {
      Core::System::GetInstance().GetFifoRecorder().UseMemory(address, bytes_per_row,
                                                              MemoryUpdate::Type::TextureMap, true);
    }
  }

  // Hash even when the RAM write is deferred, so that a draw sampling the copy before the flush
  // sees the same hash.
  if (entry)
  {
    const u64 ram_hash = entry->CalculateHash();
    entry->SetHashes(ram_hash, ram_hash);
    m_textures_by_address.emplace(dstAddr, std::move(entry));
  }
}

void TextureCacheBase::FlushEFBCopies()
{
  if (m_pending_efb_copies.empty())
    return;

  for (const RcTcacheEntry& pending : m_pending_efb_copies)
    FlushEFBCopy(pending.get());
  m_pending_efb_copies.clear();
}

void TextureCacheBase::WriteEFBCopyToRAM(u8* dst_ptr, u32 width, u32 height, u32 stride,
                                         std::unique_ptr<AbstractStagingTexture> staging_texture)
{
  const MathUtil::Rectangle<int> rows(0, 0, static_cast<int>(width), static_cast<int>(height));
  staging_texture->ReadTexels(rows, dst_ptr, stride);
  ReleaseEFBCopyStagingTexture(std::move(staging_texture));
}

void TextureCacheBase::FlushEFBCopy(TCacheEntry* entry)
{
  const u32 covered_range = entry->pending_efb_copy_height * entry->memory_stride;

  u8* const dst = GuestMemory().GetPointerForRange(entry->addr, covered_range);
  WriteEFBCopyToRAM(dst, entry->pending_efb_copy_width, entry->pending_efb_copy_height,
                    entry->memory_stride, std::move(entry->pending_efb_copy));

  // An invalidated copy (see InvalidateTexture) only had to reach RAM; its last owner lets go of
  // it soon.
  if (entry->invalidated)
    return;

  // RAM now holds the copy, so hash it again. Later guest writes are caught before the game can
  // sample it.
  const u64 ram_hash = entry->CalculateHash();
  entry->SetHashes(ram_hash, ram_hash);

  if (!entry->is_xfb_copy)
    return;

  // Overlapping XFB copies want a fresh hash too (see CopyRenderTargetToTexture). Note that the
  // result is stored on this entry rather than the overlapping one; that long-standing behaviour
  // is kept.
  auto [cursor, stop] = FindOverlappingTextures(entry->addr, covered_range);
  for (; cursor != stop; ++cursor)
  {
    const TCacheEntry& other = *cursor->second;
    if (other.may_have_overlapping_textures && other.is_xfb_copy &&
        other.OverlapsMemoryRange(entry->addr, covered_range))
    {
      const u64 other_hash = other.CalculateHash();
      entry->SetHashes(other_hash, other_hash);
    }
  }
}

std::unique_ptr<AbstractStagingTexture> TextureCacheBase::GetEFBCopyStagingTexture()
{
  // The most recently returned texture is reused first.
  if (!m_efb_copy_staging_texture_pool.empty())
  {
    std::unique_ptr<AbstractStagingTexture> reused =
        std::move(m_efb_copy_staging_texture_pool.back());
    m_efb_copy_staging_texture_pool.pop_back();
    return reused;
  }

  std::unique_ptr<AbstractStagingTexture> created = g_gfx->CreateStagingTexture(
      StagingTextureType::Readback, m_efb_encoding_texture->GetConfig());
  if (!created)
    WARN_LOG_FMT(VIDEO, "Failed to create EFB copy staging texture");
  return created;
}

void TextureCacheBase::ReleaseEFBCopyStagingTexture(std::unique_ptr<AbstractStagingTexture> tex)
{
  m_efb_copy_staging_texture_pool.push_back(std::move(tex));
}

void TextureCacheBase::UninitializeEFBMemory(u8* dst, u32 stride, u32 bytes_per_row,
                                             u32 num_blocks_y)
{
  // Most games never read a copy back, so it lives on in VRAM only and the RAM is zeroed; the
  // known contents let the hash check notice later CPU writes.
  for (u32 row = 0; row < num_blocks_y; ++row)
    std::memset(dst + size_t{row} * stride, 0, bytes_per_row);
}

void TextureCacheBase::UninitializeXFBMemory(u8* dst, u32 stride, u32 bytes_per_row,
                                             u32 num_blocks_y)
{
  // XFB memory without a RAM copy is filled with Y=1, U=V=254 (fuchsia) rather than the zeroes
  // (dark green in YUV) used for EFB copies. The pattern was meant for keying partial XFBs (Mario
  // Strikers, Chicken Little); that work was dropped, but it still tells the two cases apart.
  static constexpr std::array<u8, 16> pattern = {1, 254, 1, 254, 1, 254, 1, 254,
                                                 1, 254, 1, 254, 1, 254, 1, 254};
  for (u32 row = 0; row < num_blocks_y; ++row)
  {
    u8* out = dst + size_t{row} * stride;
    u32 left = bytes_per_row;
    for (; left >= pattern.size(); left -= 16, out += 16)
      std::memcpy(out, pattern.data(), pattern.size());
    std::memcpy(out, pattern.data(), left);
  }
}

// ---------------------------------------------------------------------------------------------
// Allocation and the address map

RcTcacheEntry TextureCacheBase::AllocateCacheEntry(const TextureConfig& config)
{
  std::optional<TexPoolEntry> host = AllocateTexture(config);
  if (!host)
    return {};

  auto created =
      std::make_shared<TCacheEntry>(std::move(host->texture), std::move(host->framebuffer));
  created->textures_by_hash_iter = m_textures_by_hash.end();
  created->id = m_last_entry_id++;
  return created;
}

std::optional<TextureCacheBase::TexPoolEntry>
TextureCacheBase::AllocateTexture(const TextureConfig& config)
{
  if (const auto pooled = FindMatchingTextureFromPool(config); pooled != m_texture_pool.end())
  {
    std::optional<TexPoolEntry> reused{std::move(pooled->second)};
    m_texture_pool.erase(pooled);
    return reused;
  }

  std::unique_ptr<AbstractTexture> host_texture = g_gfx->CreateTexture(config);
  if (host_texture == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Failed to allocate a {}x{}x{} texture", config.width, config.height,
                 config.layers);
    return std::nullopt;
  }

  // Render targets come with a framebuffer.
  const bool render_target = config.IsRenderTarget();
  std::unique_ptr<AbstractFramebuffer> host_fb =
      render_target ? g_gfx->CreateFramebuffer(host_texture.get(), nullptr) : nullptr;
  if (render_target && host_fb == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Failed to allocate a {}x{}x{} framebuffer", config.width,
                 config.height, config.layers);
    return std::nullopt;
  }

  INCSTAT(g_stats.num_textures_created);
  return TexPoolEntry(std::move(host_texture), std::move(host_fb));
}

TextureCacheBase::TexPool::iterator
TextureCacheBase::FindMatchingTextureFromPool(const TextureConfig& config)
{
  // Skip textures released this frame: filling one again with other data in the same frame can
  // make the driver keep two copies of it. Render targets are fine, since a separate pass fills
  // them (and the static textures that remain don't change much anyway).
  const auto [first, last] = m_texture_pool.equal_range(config);
  const auto usable = std::find_if(first, last, [](const TexPool::value_type& item) {
    return item.first.IsRenderTarget() || item.second.frameCount != FRAMECOUNT_INVALID;
  });
  return usable == last ? m_texture_pool.end() : usable;
}

TextureCacheBase::TexAddrCache::iterator TextureCacheBase::GetTexCacheIter(TCacheEntry* entry)
{
  const auto [first, last] = m_textures_by_address.equal_range(entry->addr);
  const auto match = std::find_if(
      first, last, [entry](const TexAddrCache::value_type& item) { return item.second.get() == entry; });
  return match == last ? m_textures_by_address.end() : match;
}

std::pair<TextureCacheBase::TexAddrCache::iterator, TextureCacheBase::TexAddrCache::iterator>
TextureCacheBase::FindOverlappingTextures(u32 addr, u32 size_in_bytes)
{
  // The map knows only where entries start. No texture exceeds 1024x1024 texels of 8 nibbles, so
  // anything starting further below `addr` than that can't reach it; candidates still need an
  // exact overlap test.
  constexpr u32 kLargestTexture = 1024 * 1024 * 4;
  const u32 low = addr > kLargestTexture ? addr - kLargestTexture : 0;
  return {m_textures_by_address.lower_bound(low),
          m_textures_by_address.upper_bound(addr + size_in_bytes)};
}

TextureCacheBase::TexAddrCache::iterator
TextureCacheBase::InvalidateTexture(TexAddrCache::iterator iter, bool discard_pending_efb_copy)
{
  if (iter == m_textures_by_address.end())
    return iter;

  TCacheEntry& entry = *iter->second;
  if (entry.textures_by_hash_iter != m_textures_by_hash.end())
  {
    m_textures_by_hash.erase(entry.textures_by_hash_iter);
    entry.textures_by_hash_iter = m_textures_by_hash.end();
  }

  // A queued copy is not written out here. Bloom-style effects copy, draw, copy again to the
  // same place and so on, so every copy invalidates the one before it, and flushing would undo the
  // batching. The copy stays queued but leaves the cache, so its VRAM texture is not used again;
  // the queued RAM writes land in order and give the same final result.
  if (entry.pending_efb_copy)
  {
    if (discard_pending_efb_copy)
    {
      // The new copy covers this one completely, so its RAM write would be overwritten anyway
      // (Xenoblade's sunset makes 35 copies a frame, 25 of them to one address).
      ReleaseEFBCopyStagingTexture(std::move(entry.pending_efb_copy));
      const auto queued = std::ranges::find(m_pending_efb_copies, iter->second);
      if (queued != m_pending_efb_copies.end())
        m_pending_efb_copies.erase(queued);
    }
    else if (!entry.IsLocked())
    {
      // The texels are already in the staging texture, so the host texture can go back to the
      // pool now, which saves a little VRAM.
      ReleaseToPool(&entry);
    }
  }
  entry.invalidated = true;

  return m_textures_by_address.erase(iter);
}

void TextureCacheBase::ReleaseToPool(TCacheEntry* entry)
{
  if (!entry->texture)
    return;
  const TextureConfig config = entry->texture->GetConfig();
  m_texture_pool.emplace(config,
                         TexPoolEntry(std::move(entry->texture), std::move(entry->framebuffer)));
}

bool TextureCacheBase::CreateUtilityTextures()
{
  // Render target the EFB-to-RAM encoders draw into.
  constexpr TextureConfig encoder_target(EFB_WIDTH * 4, 1024, 1, 1, 1,
                                         AbstractTextureFormat::BGRA8,
                                         AbstractTextureFlag_RenderTarget,
                                         AbstractTextureType::Texture_2DArray);
  m_efb_encoding_texture = g_gfx->CreateTexture(encoder_target, "EFB encoding texture");
  if (!m_efb_encoding_texture)
    return false;
  m_efb_encoding_framebuffer = g_gfx->CreateFramebuffer(m_efb_encoding_texture.get(), nullptr);
  if (!m_efb_encoding_framebuffer)
    return false;

  if (!g_backend_info.bSupportsGPUTextureDecoding)
    return true;

  // Compute output for GPU texture decoding; decoded levels are copied out of it.
  constexpr TextureConfig decoder_target(1024, 1024, 1, 1, 1, AbstractTextureFormat::RGBA8,
                                         AbstractTextureFlag_ComputeImage,
                                         AbstractTextureType::Texture_2DArray);
  m_decoding_texture = g_gfx->CreateTexture(decoder_target, "GPU texture decoding texture");
  return m_decoding_texture != nullptr;
}

// ---------------------------------------------------------------------------------------------
// GPU work

void TextureCacheBase::CopyEFBToCacheEntry(RcTcacheEntry& entry, bool is_depth_copy,
                                           const MathUtil::Rectangle<int>& src_rect,
                                           bool scale_by_half, bool linear_filter,
                                           EFBCopyFormat dst_format, bool is_intensity, float gamma,
                                           bool clamp_top, bool clamp_bottom,
                                           const std::array<u32, 3>& filter_coefficients)
{
  // Pending EFB pokes are part of what gets copied.
  g_framebuffer_manager->FlushEFBPokes();

  // Null if the shader failed to compile.
  const AbstractPipeline* to_vram = g_shader_cache->GetEFBCopyToVRAMPipeline(
      TextureConversionShaderGen::GetShaderUid(dst_format, is_depth_copy, is_intensity,
                                               scale_by_half, 1.0f / gamma, filter_coefficients));
  if (to_vram == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Skipping EFB copy to VRAM due to missing pipeline.");
    return;
  }

  const MathUtil::Rectangle<int> host_rect = HostCopyRect(src_rect);
  AbstractTexture* const source = ResolveCopySource(is_depth_copy, host_rect);

  g_gfx->BeginUtilityDrawing();
  source->FinishedRendering();

  const float rcp_w = 1.0f / static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  const u32 efb_height = g_framebuffer_manager->GetEFBHeight();
  const float rcp_h = 1.0f / static_cast<float>(efb_height);
  const auto [first_row, last_row] =
      CopyClampRows(host_rect, clamp_top, clamp_bottom, efb_height, rcp_h);

  // Constant block of the EFB-to-texture shaders.
  struct VramCopyConstants
  {
    std::array<float, 4> source;  // left, top, width, height; normalised
    std::array<u32, 3> taps;
    float gamma_rcp;
    float clamp_top;
    float clamp_bottom;
    float pixel_height;
    u32 unused;
  };
  static_assert(sizeof(VramCopyConstants) == 48);
  const VramCopyConstants constants = {
      .source = {host_rect.left * rcp_w, host_rect.top * rcp_h, host_rect.GetWidth() * rcp_w,
                 host_rect.GetHeight() * rcp_h},
      .taps = filter_coefficients,
      .gamma_rcp = 1.0f / gamma,
      .clamp_top = first_row,
      .clamp_bottom = last_row,
      // Row step of the vertical filter: one host row for scaled copies, else one native row.
      .pixel_height = g_ActiveConfig.bCopyEFBScaled ? rcp_h : 1.0f / EFB_HEIGHT,
      .unused = 0,
  };
  g_vertex_manager->UploadUtilityUniforms(&constants, sizeof(constants));

  g_gfx->SetAndDiscardFramebuffer(entry->framebuffer.get());
  g_gfx->SetViewportAndScissor(entry->framebuffer->GetRect());
  g_gfx->SetPipeline(to_vram);
  g_gfx->SetTexture(0, source);
  g_gfx->SetSamplerState(0, CopySampler(linear_filter));
  g_gfx->Draw(0, 3);
  g_gfx->EndUtilityDrawing();
  entry->texture->FinishedRendering();
}

void TextureCacheBase::CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params,
                               u32 native_width, u32 bytes_per_row, u32 num_blocks_y,
                               u32 /*memory_stride*/, const MathUtil::Rectangle<int>& src_rect,
                               bool scale_by_half, bool linear_filter, float y_scale, float gamma,
                               bool clamp_top, bool clamp_bottom,
                               const std::array<u32, 3>& filter_coefficients)
{
  // Pending EFB pokes are part of what gets copied.
  g_framebuffer_manager->FlushEFBPokes();

  const AbstractPipeline* encoder = g_shader_cache->GetEFBCopyToRAMPipeline(params);
  if (encoder == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Skipping EFB copy to VRAM due to missing pipeline.");
    return;
  }

  const MathUtil::Rectangle<int> host_rect = HostCopyRect(src_rect);
  AbstractTexture* const source = ResolveCopySource(params.depth, host_rect);

  g_gfx->BeginUtilityDrawing();
  source->FinishedRendering();

  const u32 efb_height = g_framebuffer_manager->GetEFBHeight();
  const float rcp_h = 1.0f / static_cast<float>(efb_height);
  const auto [first_row, last_row] =
      CopyClampRows(host_rect, clamp_top, clamp_bottom, efb_height, rcp_h);

  // Constant block of the EFB-to-RAM encoders.
  struct EncoderConstants
  {
    std::array<s32, 4> position;  // EFB left, top, native width, sample step
    std::array<float, 4> shape;   // y scale, 1/gamma, first row, last row
    std::array<u32, 4> taps;      // three filter rows, then padding
  };
  static_assert(sizeof(EncoderConstants) == 48);
  const EncoderConstants constants = {
      .position = {src_rect.left, src_rect.top, static_cast<s32>(native_width),
                   scale_by_half ? 2 : 1},
      .shape = {y_scale, 1.0f / gamma, first_row, last_row},
      .taps = {filter_coefficients[0], filter_coefficients[1], filter_coefficients[2], 0},
  };
  g_vertex_manager->UploadUtilityUniforms(&constants, sizeof(constants));

  // The encoders work from the fragment position and the result is read back, so they draw from
  // the target's origin: one 32-bit texel per four bytes of a guest row, one row per block row.
  const MathUtil::Rectangle<int> out_rect(0, 0, static_cast<int>(bytes_per_row / 4),
                                          static_cast<int>(num_blocks_y));

  g_gfx->SetAndDiscardFramebuffer(m_efb_encoding_framebuffer.get());
  g_gfx->SetViewportAndScissor(out_rect);
  g_gfx->SetPipeline(encoder);
  g_gfx->SetTexture(0, source);
  g_gfx->SetSamplerState(0, CopySampler(linear_filter));
  g_gfx->Draw(0, 3);
  dst->CopyFromTexture(m_efb_encoding_texture.get(), out_rect, 0, 0, out_rect);
  g_gfx->EndUtilityDrawing();

  // Gives the vertex manager the chance to flush once enough draws have built up.
  g_vertex_manager->OnEFBCopyToRAM();
}

bool TextureCacheBase::DecodeTextureOnGPU(RcTcacheEntry& entry, u32 dst_level, const u8* data,
                                          u32 data_size, TextureFormat format, u32 width,
                                          u32 height, u32 aligned_width, u32 aligned_height,
                                          u32 row_stride, const u8* palette,
                                          TLUTFormat palette_format)
{
  const auto* info = TextureConversionShaderTiled::GetDecodingShaderInfo(format);
  if (info == nullptr)
    return false;

  const bool paletted = info->palette_size != 0;
  const AbstractShader* decoder = g_shader_cache->GetTextureDecodingShader(
      format, paletted ? std::make_optional(palette_format) : std::nullopt);
  if (decoder == nullptr)
    return false;

  // Texels (and palette) go into the texel stream buffer, which the shader indexes in elements
  // of the buffer format.
  const u32 element_bytes = VertexManagerBase::GetTexelBufferElementSize(info->buffer_format);
  u32 src_offset = 0;
  u32 palette_offset = 0;
  const bool uploaded =
      paletted ? g_vertex_manager->UploadTexelBuffer(data, data_size, info->buffer_format,
                                                     &src_offset, palette, info->palette_size,
                                                     TEXEL_BUFFER_FORMAT_R16_UINT, &palette_offset) :
                 g_vertex_manager->UploadTexelBuffer(data, data_size, info->buffer_format,
                                                     &src_offset);
  if (!uploaded)
    return false;

  // Constant block of the decoding shaders.
  struct DecoderConstants
  {
    std::array<u32, 2> dst_size;
    std::array<u32, 2> src_size;
    u32 src_offset;
    u32 src_row_stride;  // in buffer elements
    u32 palette_offset;
    u32 unused;
  };
  const DecoderConstants constants = {{width, height},
                                      {aligned_width, aligned_height},
                                      src_offset,
                                      row_stride / element_bytes,
                                      palette_offset,
                                      0};
  g_vertex_manager->UploadUtilityUniforms(&constants, sizeof(constants));
  g_gfx->SetComputeImageTexture(0, m_decoding_texture.get(), false, true);

  const auto [groups_x, groups_y] =
      TextureConversionShaderTiled::GetDispatchCount(info, aligned_width, aligned_height);
  g_gfx->DispatchComputeShader(decoder, info->group_size_x, info->group_size_y, 1, groups_x,
                               groups_y, 1);

  // The shader writes a shared target, then the level is copied into place; that saves making a
  // compute view of every layer.
  const MathUtil::Rectangle<int> level_rect = entry->texture->GetConfig().GetMipRect(dst_level);
  entry->texture->CopyRectangleFromTexture(m_decoding_texture.get(), level_rect, 0, 0, level_rect,
                                           0, dst_level);
  entry->texture->FinishedRendering();
  return true;
}

void TextureCacheBase::ApplyMaterialToCacheEntry(const VideoCommon::MaterialResource& material,
                                                 TCacheEntry* entry)
{
  const auto material_data = material.GetData();
  if (!material_data) [[unlikely]]
    return;

  // Render into a scratch entry: a texture can't be sampled while it is the render target.
  auto scratch = AllocateCacheEntry(entry->texture->GetConfig());
  scratch->SetGeneralParameters(entry->addr, entry->size_in_bytes, entry->format,
                                entry->should_force_safe_hashing);
  scratch->SetDimensions(entry->native_width, entry->native_height, 1);
  scratch->SetEfbCopy(entry->memory_stride);
  scratch->may_have_overlapping_textures = false;
  scratch->frameCount = FRAMECOUNT_INVALID;

  g_gfx->BeginUtilityDrawing();
  entry->texture->FinishedRendering();

  const auto custom_uniforms = material_data->GetUniforms();

  // Built-in constants placed ahead of the material's own (the post-processing block has the
  // same layout). Scalars are padded to 16 bytes.
  struct BuiltinConstants
  {
    std::array<float, 4> source_resolution;
    std::array<float, 4> target_resolution;
    std::array<float, 4> window_resolution;
    std::array<float, 4> source_rectangle;
    std::array<s32, 4> source_layer;
    std::array<u32, 4> time;
    std::array<s32, 4> graphics_api;
    std::array<u32, 4> efb_scale;
  };
  static_assert(sizeof(BuiltinConstants) == 128);

  const float src_w = static_cast<float>(entry->texture->GetWidth());
  const float src_h = static_cast<float>(entry->texture->GetHeight());
  const auto window = g_presenter->GetTargetRectangle();
  const float window_w = static_cast<float>(window.GetWidth());
  const float window_h = static_cast<float>(window.GetHeight());

  BuiltinConstants builtins = {};
  builtins.source_resolution = {src_w, src_h, 1.0f / src_w, 1.0f / src_h};
  // The material writes back into the texture it reads, so source and target agree.
  builtins.target_resolution = builtins.source_resolution;
  builtins.window_resolution = {window_w, window_h, 1.0f / window_w, 1.0f / window_h};
  builtins.source_rectangle = {0, 0, 1, 1};
  builtins.graphics_api[0] = static_cast<s32>(g_backend_info.api_type);
  builtins.efb_scale[0] = g_framebuffer_manager->GetEFBScale();

  Common::UniqueBuffer<u8> upload(custom_uniforms.size() + sizeof(builtins));
  std::memcpy(upload.data(), &builtins, sizeof(builtins));
  std::memcpy(upload.data() + sizeof(builtins), custom_uniforms.data(), custom_uniforms.size());
  g_vertex_manager->UploadUtilityUniforms(upload.data(), static_cast<u32>(upload.size()));

  // Draw into the scratch entry.
  g_gfx->SetAndDiscardFramebuffer(scratch->framebuffer.get());
  g_gfx->SetViewportAndScissor(scratch->framebuffer->GetRect());
  g_gfx->SetPipeline(material_data->GetPipeline());
  g_gfx->SetTexture(0, entry->texture.get());
  g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
  for (const auto& extra : material_data->GetTextures())
  {
    g_gfx->SetTexture(extra.sampler_index, extra.texture);
    g_gfx->SetSamplerState(extra.sampler_index, extra.sampler);
  }
  g_gfx->Draw(0, 3);
  g_gfx->EndUtilityDrawing();
  scratch->texture->FinishedRendering();

  // The result becomes the entry's texture; the previous one goes back to the pool.
  std::swap(entry->texture, scratch->texture);
  std::swap(entry->framebuffer, scratch->framebuffer);
  ReleaseToPool(scratch.get());

  if (const auto* next = material_data->GetNextMaterial())
    ApplyMaterialToCacheEntry(*next, entry);
}
