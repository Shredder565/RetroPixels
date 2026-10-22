// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <filesystem>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/MathUtil.h"

#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Assets/CustomAsset.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/TextureInfo.h"
#include "VideoCommon/TextureUtils.h"
#include "VideoCommon/VideoEvents.h"

class AbstractFramebuffer;
class AbstractStagingTexture;
class PointerWrap;
struct SamplerState;
struct VideoConfig;

namespace VideoCommon
{
class CustomTextureData;
class GameTextureAsset;
class MaterialResource;
}  // namespace VideoCommon

// Name prefixes for dumped (and graphics-mod matched) EFB and XFB copies.
constexpr std::string_view EFB_DUMP_PREFIX = "efb1";
constexpr std::string_view XFB_DUMP_PREFIX = "xfb1";

// frameCount value of an entry (or pooled texture) that was used during the current frame.
static constexpr int FRAMECOUNT_INVALID = 0;

// A texture format plus the palette format. The palette format only takes part in comparisons
// when the texture format is a colour-indexed one.
struct TextureAndTLUTFormat
{
  TextureAndTLUTFormat(TextureFormat texfmt_ = TextureFormat::I4,
                       TLUTFormat tlutfmt_ = TLUTFormat::IA8)
      : texfmt(texfmt_), tlutfmt(tlutfmt_)
  {
  }

  bool operator==(const TextureAndTLUTFormat& other) const
  {
    const bool same_texture_format = texfmt == other.texfmt;
    if (!IsColorIndexed(texfmt))
      return same_texture_format;
    return same_texture_format && tlutfmt == other.tlutfmt;
  }

  TextureFormat texfmt;
  TLUTFormat tlutfmt;
};

// Everything that selects an EFB-to-RAM encoding pipeline.
struct EFBCopyParams
{
  EFBCopyParams(PixelFormat efb_format_, EFBCopyFormat copy_format_, bool depth_, bool yuv_,
                bool all_copy_filter_coefs_needed_, bool copy_filter_can_overflow_,
                bool apply_gamma_)
      : efb_format(efb_format_), copy_format(copy_format_), depth(depth_), yuv(yuv_),
        all_copy_filter_coefs_needed(all_copy_filter_coefs_needed_),
        copy_filter_can_overflow(copy_filter_can_overflow_), apply_gamma(apply_gamma_)
  {
  }

  PixelFormat efb_format;
  EFBCopyFormat copy_format;
  bool depth;
  bool yuv;
  bool all_copy_filter_coefs_needed;
  bool copy_filter_can_overflow;
  bool apply_gamma;

private:
  auto SortKey() const
  {
    return std::tie(efb_format, copy_format, depth, yuv, all_copy_filter_coefs_needed,
                    copy_filter_can_overflow, apply_gamma);
  }

public:
  // Ordered field by field, in declaration order.
  bool operator<(const EFBCopyParams& rhs) const { return SortKey() < rhs.SortKey(); }
};

template <>
struct fmt::formatter<EFBCopyParams>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const EFBCopyParams& params, FormatContext& ctx) const
  {
    // XFB is outside the range the EFBCopyFormat formatter names.
    const std::string copy_format = params.copy_format == EFBCopyFormat::XFB ?
                                        std::string("XFB") :
                                        fmt::to_string(params.copy_format);
    return fmt::format_to(ctx.out(),
                          "format: {}, copy format: {}, depth: {}, yuv: {}, apply_gamma: {}, "
                          "all_copy_filter_coefs_needed: {}, copy_filter_can_overflow: {}",
                          params.efb_format, copy_format, params.depth, params.yuv,
                          params.apply_gamma, params.all_copy_filter_coefs_needed,
                          params.copy_filter_can_overflow);
  }
};

// One texture known to the cache: a decoded RAM/TMEM texture, an EFB copy or an XFB copy.
struct TCacheEntry
{
  // --- the host texture and where it came from
  std::unique_ptr<AbstractTexture> texture;
  std::unique_ptr<AbstractFramebuffer> framebuffer;
  u32 addr = 0;
  u32 size_in_bytes = 0;
  u64 base_hash = 0;
  u64 hash = 0;  // base_hash, mixed with the palette hash for paletted textures
  TextureAndTLUTFormat format;
  u32 memory_stride = 0;
  bool is_efb_copy = false;
  bool is_custom_tex = false;
  // Set while an EFB copy may still need to be applied as a partial update.
  bool may_have_overlapping_textures = true;
  // The mip levels hold their own artwork rather than downscaled copies of level 0.
  bool has_arbitrary_mips = false;
  bool should_force_safe_hashing = false;  // XFB copies hash every byte
  bool is_xfb_copy = false;
  bool is_xfb_container = false;
  u64 id = 0;
  u32 content_semaphore = 0;  // number of outstanding content locks

  // Removed from m_textures_by_address; other holders may still keep the object alive.
  bool invalidated = false;

  // An XFB copy this one was stitched from has been replaced.
  bool reference_changed = false;

  // Size as the console sees it (the host texture may be scaled).
  u32 native_width = 0;
  u32 native_height = 0;
  u32 native_levels = 0;

  // Frame of last use, for evicting entries idle for TEXTURE_KILL_THRESHOLD frames.
  int frameCount = FRAMECOUNT_INVALID;

  // This entry's slot in m_textures_by_hash (or that map's end()), for O(1) removal.
  std::multimap<u64, std::shared_ptr<TCacheEntry>>::iterator textures_by_hash_iter;

  // Two-way links between EFB copies and the textures they were applied to (partial updates
  // and XFB stitching), so the same update is not applied twice.
  std::unordered_set<TCacheEntry*> references;

  // EFB-to-RAM copy whose readback has not been written to guest memory yet.
  std::unique_ptr<AbstractStagingTexture> pending_efb_copy;
  u32 pending_efb_copy_width = 0;
  u32 pending_efb_copy_height = 0;

  std::string texture_info_name = "";

  VideoCommon::CustomAsset::TimeType last_load_time;
  std::shared_ptr<HiresTexture> hires_texture;

  explicit TCacheEntry(std::unique_ptr<AbstractTexture> tex,
                       std::unique_ptr<AbstractFramebuffer> fb);

  ~TCacheEntry();

  void SetGeneralParameters(u32 _addr, u32 _size, TextureAndTLUTFormat _format,
                            bool force_safe_hashing)
  {
    format = _format;
    addr = _addr;
    size_in_bytes = _size;
    should_force_safe_hashing = force_safe_hashing;
  }

  // Native dimensions; the row pitch defaults to the width until a copy sets its own.
  void SetDimensions(unsigned int _native_width, unsigned int _native_height,
                     unsigned int _native_levels)
  {
    memory_stride = _native_width;
    native_width = _native_width;
    native_height = _native_height;
    native_levels = _native_levels;
  }

  void SetHashes(u64 _base_hash, u64 _hash)
  {
    hash = _hash;
    base_hash = _base_hash;
  }

  // While locked, the cache must not overwrite the texture's contents in place.
  void AcquireContentLock() { ++content_semaphore; }
  void ReleaseContentLock() { --content_semaphore; }
  bool IsLocked() const { return content_semaphore != 0; }

  // Records that other_entry was built (partly) from this one, and the reverse.
  void CreateReference(TCacheEntry* other_entry)
  {
    references.emplace(other_entry);
    other_entry->references.emplace(this);
  }

  void SetXfbCopy(u32 stride);
  void SetEfbCopy(u32 stride);
  void SetNotCopy();

  bool OverlapsMemoryRange(u32 range_address, u32 range_size) const;

  bool IsEfbCopy() const { return is_efb_copy; }
  bool IsCopy() const { return is_efb_copy || is_xfb_copy; }
  u32 NumBlocksX() const;
  u32 NumBlocksY() const;
  u32 BytesPerRow() const;

  u64 CalculateHash() const;

  int HashSampleSize() const;
  u32 GetWidth() const { return texture->GetConfig().width; }
  u32 GetHeight() const { return texture->GetConfig().height; }
  u32 GetNumLevels() const { return texture->GetConfig().levels; }
  u32 GetNumLayers() const { return texture->GetConfig().layers; }
  AbstractTextureFormat GetFormat() const { return texture->GetConfig().format; }
  void DoState(PointerWrap& p);
};

using RcTcacheEntry = std::shared_ptr<TCacheEntry>;

class TextureCacheBase
{
public:
  // Hashes and sizes GetTexture works out before creating an entry.
  struct TextureCreationInfo
  {
    u64 base_hash;
    u64 full_hash;
    u32 bytes_per_block;
    u32 palette_size;
  };

  // A host texture waiting in the reuse pool.
  struct TexPoolEntry
  {
    std::unique_ptr<AbstractTexture> texture;
    std::unique_ptr<AbstractFramebuffer> framebuffer;
    int frameCount = FRAMECOUNT_INVALID;

    TexPoolEntry(std::unique_ptr<AbstractTexture> tex, std::unique_ptr<AbstractFramebuffer> fb);
  };

  TextureCacheBase();
  virtual ~TextureCacheBase();

  // Texture for a texture unit (null if it can't be had), and the XFB at an address.
  TCacheEntry* Load(u32 stage);
  RcTcacheEntry GetTexture(const int textureCacheSafetyColorSampleSize,
                           const TextureInfo& texture_info);
  RcTcacheEntry GetXFBTexture(u32 address, u32 width, u32 height, u32 stride,
                              MathUtil::Rectangle<int>* display_rect);

  bool Initialize();
  void Shutdown();

  void OnConfigChanged(const VideoConfig& config);

  // Evicts entries (and pooled textures) that have gone unused for too many frames.
  void Cleanup(int _frameCount);

  void Invalidate();
  void ReleaseToPool(TCacheEntry* entry);

  virtual void BindTextures(BitSet32 used_textures, const std::array<SamplerState, 8>& samplers);
  void CopyRenderTargetToTexture(u32 dstAddr, EFBCopyFormat dstFormat, u32 width, u32 height,
                                 u32 dstStride, bool is_depth_copy,
                                 const MathUtil::Rectangle<int>& srcRect, bool isIntensity,
                                 bool scaleByHalf, float y_scale, float gamma, bool clamp_top,
                                 bool clamp_bottom,
                                 const CopyFilterCoefficients::Values& filter_coefficients);

  void ScaleTextureCacheEntryTo(RcTcacheEntry& entry, u32 new_width, u32 new_height);

  // Writes every deferred EFB copy to guest RAM.
  void FlushEFBCopies();

  // Drops bindings whose texture can no longer be assumed to be in TMEM.
  void FlushStaleBinds();

  // Save-state helpers for a single host texture.
  void SerializeTexture(AbstractTexture* tex, const TextureConfig& config, PointerWrap& p);
  std::optional<TexPoolEntry> DeserializeTexture(PointerWrap& p);

  void DoState(PointerWrap& p);

  static bool AllCopyFilterCoefsNeeded(const std::array<u32, 3>& coefficients);
  static bool CopyFilterCanOverflow(const std::array<u32, 3>& coefficients);

  // Sampler state for a texture unit, with the user's filtering overrides applied.
  static SamplerState GetSamplerState(u32 index, float custom_tex_scale, bool custom_tex,
                                      bool has_arbitrary_mips);

  static void RenderPaletteEntry(u32 texel_buffer_offset, const RcTcacheEntry& entry,
                                 AbstractTexture* texture, TLUTFormat tlutfmt);
  static void RenderReinterpretEntry(const RcTcacheEntry& entry, AbstractTexture* texture,
                                     TextureFormat old_format, TextureFormat new_format);

protected:
  virtual void CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
                       u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
                       const MathUtil::Rectangle<int>& src_rect, bool scale_by_half,
                       bool linear_filter, float y_scale, float gamma, bool clamp_top,
                       bool clamp_bottom, const std::array<u32, 3>& filter_coefficients);
  virtual void CopyEFBToCacheEntry(RcTcacheEntry& entry, bool is_depth_copy,
                                   const MathUtil::Rectangle<int>& src_rect, bool scale_by_half,
                                   bool linear_filter, EFBCopyFormat dst_format, bool is_intensity,
                                   float gamma, bool clamp_top, bool clamp_bottom,
                                   const std::array<u32, 3>& filter_coefficients);

  // Decodes one level on the GPU into entry's texture. Returns false when the format or the
  // backend cannot do it, in which case the caller decodes on the CPU instead. width/height
  // are the texel size, aligned_* the block-aligned size, row_stride the bytes per block row.
  bool DecodeTextureOnGPU(RcTcacheEntry& entry, u32 dst_level, const u8* data, u32 data_size,
                          TextureFormat format, u32 width, u32 height, u32 aligned_width,
                          u32 aligned_height, u32 row_stride, const u8* palette,
                          TLUTFormat palette_format);

  alignas(16) u8* m_temp = nullptr;
  size_t m_temp_size = 0;

private:
  using TexAddrCache = std::multimap<u32, RcTcacheEntry>;
  using TexHashCache = std::multimap<u64, RcTcacheEntry>;

  using TexPool = std::unordered_multimap<TextureConfig, TexPoolEntry>;

  // --- Lookup and creation ---
  TCacheEntry* LoadImpl(u32 stage, bool force_reload);
  // True when the custom texture behind an entry was reloaded since the entry was made.
  static bool DidLinkedAssetsChange(const TCacheEntry& entry);
  RcTcacheEntry CreateTextureEntry(const TextureCreationInfo& creation_info,
                                   const TextureInfo& texture_info, int safety_color_sample_size,
                                   VideoCommon::CustomTextureData* custom_texture_data,
                                   bool custom_arbitrary_mipmaps, bool skip_texture_dump);
  RcTcacheEntry DoPartialTextureUpdates(RcTcacheEntry& entry_to_update, const u8* palette,
                                        TLUTFormat tlutfmt);
  RcTcacheEntry GetXFBFromCache(u32 address, u32 width, u32 height, u32 stride);
  void StitchXFBCopy(RcTcacheEntry& entry_to_update);

  // --- Entries made by a GPU pass from another entry ---
  std::pair<RcTcacheEntry, u32> CreatePaletteEntryWithOffset(const RcTcacheEntry& entry,
                                                             const u8* palette);
  RcTcacheEntry CreateReinterpretEntry(const RcTcacheEntry& existing_entry,
                                       TextureFormat new_format);
  void ApplyMaterialToCacheEntry(const VideoCommon::MaterialResource& material, TCacheEntry* entry);

  // --- The address map ---
  TexAddrCache::iterator GetTexCacheIter(TCacheEntry* entry);
  // Entries that may overlap [addr, addr + size_in_bytes). Only start addresses are indexed,
  // so the range is widened by the largest possible texture and can contain non-overlapping
  // entries; callers check OverlapsMemoryRange.
  std::pair<TexAddrCache::iterator, TexAddrCache::iterator>
  FindOverlappingTextures(u32 addr, u32 size_in_bytes);
  // Removes an entry from the cache, returning its texture to the pool where possible.
  TexAddrCache::iterator InvalidateTexture(TexAddrCache::iterator t_iter,
                                           bool discard_pending_efb_copy = false);

  // --- Host textures ---
  RcTcacheEntry AllocateCacheEntry(const TextureConfig& config);
  std::optional<TexPoolEntry> AllocateTexture(const TextureConfig& config);
  TexPool::iterator FindMatchingTextureFromPool(const TextureConfig& config);
  bool CreateUtilityTextures();
  void CheckTempSize(size_t required_size);

  // --- EFB copies to RAM ---
  // The seven copy filter taps folded into weights for the rows above, at and below.
  static std::array<u32, 3>
  GetRAMCopyFilterCoefficients(const CopyFilterCoefficients::Values& coefficients);
  static std::array<u32, 3>
  GetVRAMCopyFilterCoefficients(const CopyFilterCoefficients::Values& coefficients);
  // Staging textures sized for the largest EFB copy, reused between copies.
  std::unique_ptr<AbstractStagingTexture> GetEFBCopyStagingTexture();
  void ReleaseEFBCopyStagingTexture(std::unique_ptr<AbstractStagingTexture> tex);
  // Moves a finished EFB-to-RAM readback into guest memory.
  void WriteEFBCopyToRAM(u8* dst_ptr, u32 width, u32 height, u32 stride,
                         std::unique_ptr<AbstractStagingTexture> staging_texture);
  void FlushEFBCopy(TCacheEntry* entry);
  // Fill patterns for copies that skip the RAM write.
  void UninitializeEFBMemory(u8* dst, u32 stride, u32 bytes_per_row, u32 num_blocks_y);
  void UninitializeXFBMemory(u8* dst, u32 stride, u32 bytes_per_row, u32 num_blocks_y);

  // --- Save states and config ---
  void DoSaveState(PointerWrap& p);
  void DoLoadState(PointerWrap& p);
  bool CheckReadbackTexture(u32 width, u32 height, AbstractTextureFormat format);
  void SetBackupConfig(const VideoConfig& config);

  // The authoritative set of cached entries, keyed by guest address. Invalidated entries can
  // live on elsewhere (bindings, pending copies) after leaving this map.
  TexAddrCache m_textures_by_address;

  // Entries that were hashed in full, keyed by hash, for reuse at other addresses. Every entry
  // here is also in m_textures_by_address.
  TexHashCache m_textures_by_hash;

  // Entries bound to the eight texture units; may include invalidated entries.
  std::array<RcTcacheEntry, 8> m_bound_textures{};

  TexPool m_texture_pool;
  u64 m_last_entry_id = 0;

  // The config values the cache depends on, as of the last OnConfigChanged.
  struct BackupConfig
  {
    int color_samples;
    bool texfmt_overlay;
    bool texfmt_overlay_center;
    bool hires_textures;
    bool cache_hires_textures;
    bool copy_cache_enable;
    bool stereo_3d;
    bool efb_mono_depth;
    bool gpu_texture_decoding;
    bool disable_vram_copies;
    bool arbitrary_mipmap_detection;
    bool graphics_mods;
    u32 graphics_mod_change_count;
  };
  BackupConfig m_backup_config = {};

  // Render target the EFB-to-RAM encoders draw into.
  std::unique_ptr<AbstractTexture> m_efb_encoding_texture;
  std::unique_ptr<AbstractFramebuffer> m_efb_encoding_framebuffer;

  // Compute target for GPU texture decoding.
  std::unique_ptr<AbstractTexture> m_decoding_texture;

  // Readback textures free for deferred EFB copies.
  std::vector<std::unique_ptr<AbstractStagingTexture>> m_efb_copy_staging_texture_pool;

  // Deferred EFB copies in issue order, which is also the order they must reach guest RAM
  // (later copies overwrite earlier ones). May include invalidated entries.
  std::vector<RcTcacheEntry> m_pending_efb_copies;

  // Readback texture kept between save-state serialisations.
  std::unique_ptr<AbstractStagingTexture> m_readback_texture;

  void OnFrameEnd();

  Common::EventHook m_frame_event =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { OnFrameEnd(); });

  VideoCommon::TextureUtils::TextureDumper m_texture_dumper;
};

extern std::unique_ptr<TextureCacheBase> g_texture_cache;
