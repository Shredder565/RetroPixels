// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoEvents.h"

class NativeVertexFormat;
class PointerWrap;

// EFB pixel format changes that need the colour buffer converted.
enum class EFBReinterpretType
{
  RGB8ToRGB565 = 0,
  RGB8ToRGBA6 = 1,
  RGBA6ToRGB8 = 2,
  RGBA6ToRGB565 = 3,
  RGB565ToRGB8 = 4,
  RGB565ToRGBA6 = 5
};
constexpr u32 NUM_EFB_REINTERPRET_TYPES = 6;
template <>
struct fmt::formatter<EFBReinterpretType> : EnumFormatter<EFBReinterpretType::RGB565ToRGBA6>
{
  static constexpr array_type names = {"RGB8 to RGB565", "RGB8 to RGBA6",  "RGBA6 to RGB8",
                                       "RGB6 to RGB565", "RGB565 to RGB8", "RGB565 to RGBA6"};
  constexpr formatter() : EnumFormatter(names) {}
};

// True when the half-open ranges [aLower, aUpper) and [bLower, bUpper) intersect.
inline bool AddressRangesOverlap(u32 aLower, u32 aUpper, u32 bLower, u32 bUpper)
{
  return aLower < bUpper && bLower < aUpper;
}

// Owns the host copy of the embedded framebuffer (colour and depth at internal resolution, with
// MSAA and stereo layers as configured) and everything that works on it directly: resolves,
// pixel format conversion, clears, savestates, and the CPU peek/poke path with its readback
// cache.
class FramebufferManager final
{
public:
  FramebufferManager();
  ~FramebufferManager();

  // Formats and configs of the EFB textures; usable before the EFB exists.
  static AbstractTextureFormat GetEFBColorFormat();
  static AbstractTextureFormat GetEFBDepthFormat();
  static AbstractTextureFormat GetEFBDepthCopyFormat();
  static TextureConfig GetEFBColorTextureConfig(u32 width, u32 height);
  static TextureConfig GetEFBDepthTextureConfig(u32 width, u32 height);

  AbstractTexture* GetEFBColorTexture() const { return m_color.get(); }
  AbstractTexture* GetEFBDepthTexture() const { return m_depth.get(); }
  AbstractFramebuffer* GetEFBFramebuffer() const { return m_framebuffer.get(); }
  u32 GetEFBWidth() const { return m_color->GetWidth(); }
  u32 GetEFBHeight() const { return m_color->GetHeight(); }
  u32 GetEFBLayers() const { return m_color->GetLayers(); }
  u32 GetEFBSamples() const { return m_color->GetSamples(); }
  bool IsEFBMultisampled() const { return m_color->IsMultisampled(); }
  bool IsEFBStereo() const { return m_color->GetLayers() > 1; }
  FramebufferState GetEFBFramebufferState() const;

  // Native EFB rectangle -> internal resolution rectangle.
  MathUtil::Rectangle<int> ConvertEFBRectangle(const MathUtil::Rectangle<int>& rc) const;

  unsigned int GetEFBScale() const;

  // Native EFB coordinate -> internal resolution coordinate.
  int EFBToScaledX(int x) const;
  int EFBToScaledY(int y) const;

  // The same in floating point, from the actual EFB texture size.
  float EFBToScaledXf(float x) const;
  float EFBToScaledYf(float y) const;

  bool Initialize(int efb_scale);

  // Rebuilds the EFB at a new scale (or after MSAA/stereo/HDR changes).
  void RecreateEFBFramebuffer(int efb_scale);

  // Rebuilds the pipelines (after host shader config or MSAA changes).
  void RecompileShaders();

  void BindEFBFramebuffer();

  // Single-sampled versions of the EFB for reading. The region bounds the work done.
  AbstractTexture* ResolveEFBColorTexture(const MathUtil::Rectangle<int>& region);
  AbstractTexture* ResolveEFBDepthTexture(const MathUtil::Rectangle<int>& region,
                                          bool force_r32f = false);

  // Converts the colour buffer for a pixel format change. Must be called outside a render pass;
  // the EFB framebuffer object changes, so rebind afterwards.
  bool ReinterpretPixelData(EFBReinterpretType convtype);
  PixelFormat GetPrevPixelFormat() const { return m_last_pixel_format; }
  void StorePixelFormat(PixelFormat new_format) { m_last_pixel_format = new_format; }

  // Clears a native EFB rectangle.
  void ClearEFB(const MathUtil::Rectangle<int>& rc, bool clear_color, bool clear_alpha,
                bool clear_z, u32 color, u32 z, PixelFormat pixel_format);

  AbstractPipeline* GetClearPipeline(bool clear_color, bool clear_alpha, bool clear_z) const;

  // CPU reads at native resolution. Blocks while the needed part of the cache is refilled.
  u32 PeekEFBColor(u32 x, u32 y);
  float PeekEFBDepth(u32 x, u32 y);
  void SetEFBCacheTileSize(u32 size);
  void InvalidatePeekCache(bool forced = true);
  void RefreshPeekCache();
  void FlagPeekCacheAsOutOfDate();
  void EndOfFrame();

  // CPU writes. Queued and drawn in batches; never blocks.
  void PokeEFBColor(u32 x, u32 y, u32 color);
  void PokeEFBDepth(u32 x, u32 y, float depth);
  void FlushEFBPokes();

  void DoState(PointerWrap& p);

private:
  // A poke vertex: clip-space position (w holds the point size with large points) and RGBA8.
  struct PokeVertex
  {
    float position[4];
    u32 color;
  };
  static_assert(std::is_standard_layout_v<PokeVertex>);

  struct PeekTile
  {
    bool valid;
    // Bit 0 = read this frame; shifted left at the end of each frame.
    u8 reads;
  };

  // Native-resolution copy of the EFB colour or depth, read by the CPU. Either one tile for the
  // whole EFB or a grid of tiles, left to right then top to bottom.
  struct PeekCache
  {
    // Native-resolution render target for downscaling or depth conversion (only when needed).
    std::unique_ptr<AbstractTexture> texture;
    std::unique_ptr<AbstractFramebuffer> framebuffer;
    // Full EFB-sized staging texture.
    std::unique_ptr<AbstractStagingTexture> staging;
    std::unique_ptr<AbstractPipeline> copy_pipeline;
    std::vector<PeekTile> tiles;
    bool stale;            // the EFB was drawn to since the tiles were read
    bool any_valid;        // at least one tile has been read since the last invalidation
    bool wants_refill;     // invalidated tiles that were being read should be read again
    bool pending_readback; // an async copy has been issued but not waited for
  };

  bool CreateEFB(int efb_scale);
  void DestroyEFB();
  std::tuple<u32, u32> ComputeEFBSize(int efb_scale);

  bool BuildConversionPipelines();
  void DropConversionPipelines();
  bool BuildCopyPipelines();
  void DropCopyPipelines();
  bool CreatePeekCaches();
  void DestroyPeekCaches();
  bool BuildClearPipelines();
  void DropClearPipelines();
  bool BuildPokePipelines();
  void DropPokePipelines();

  PeekCache& Cache(bool depth) { return depth ? m_peek_depth : m_peek_color; }
  bool Tiled() const { return m_tile_size > 0; }
  // Whether depth peeks have to go through a colour render target first.
  bool DepthPeeksNeedIntermediate() const;
  u32 TileIndexAt(u32 x, u32 y) const;
  MathUtil::Rectangle<int> TileRect(u32 tile) const;
  void FillTile(bool depth, u32 tile, bool async);
  void InvalidateCache(PeekCache& cache, bool forced);
  template <typename T>
  T ReadCached(bool depth, u32 x, u32 y);
  template <typename T>
  void UpdateCached(bool depth, u32 x, u32 y, const T& value);

  void QueuePoke(std::vector<PokeVertex>& queue, u32 x, u32 y, float z, u32 color);
  void DrawPokes(std::vector<PokeVertex>& queue, const AbstractPipeline* pipeline);

  void SaveEFB(PointerWrap& p);
  void LoadEFB(PointerWrap& p);

  float m_scale = 1.0f;
  PixelFormat m_last_pixel_format;

  // The EFB, and the second colour buffer that pixel format conversions draw into.
  std::unique_ptr<AbstractTexture> m_color;
  std::unique_ptr<AbstractTexture> m_color_spare;
  std::unique_ptr<AbstractTexture> m_depth;
  std::unique_ptr<AbstractFramebuffer> m_framebuffer;
  std::unique_ptr<AbstractFramebuffer> m_framebuffer_spare;

  // Single-sampled (and, for depth, R32F) copies for reading the EFB.
  std::unique_ptr<AbstractTexture> m_color_resolved;
  std::unique_ptr<AbstractFramebuffer> m_color_resolve_target;
  std::unique_ptr<AbstractPipeline> m_color_resolve_pipeline;
  std::unique_ptr<AbstractTexture> m_depth_resolved;
  std::unique_ptr<AbstractFramebuffer> m_depth_resolve_target;
  std::unique_ptr<AbstractPipeline> m_depth_resolve_pipeline;

  // Draws savestate EFB contents back.
  std::unique_ptr<AbstractPipeline> m_restore_pipeline;

  std::array<std::unique_ptr<AbstractPipeline>, NUM_EFB_REINTERPRET_TYPES> m_conversion_pipelines;

  // Peek cache tile edge in native pixels (0 = one tile), and tiles per row.
  u32 m_tile_size = 0;
  u32 m_tiles_per_row = 1;
  PeekCache m_peek_color = {};
  PeekCache m_peek_depth = {};

  // Clear pipelines, indexed by colour write << 2 | alpha write << 1 | depth write.
  std::array<std::unique_ptr<AbstractPipeline>, 8> m_clear_pipelines;

  std::unique_ptr<NativeVertexFormat> m_poke_format;
  std::unique_ptr<AbstractPipeline> m_color_poke_pipeline;
  std::unique_ptr<AbstractPipeline> m_depth_poke_pipeline;
  std::vector<PokeVertex> m_color_pokes;
  std::vector<PokeVertex> m_depth_pokes;

  Common::EventHook m_frame_end_hook;
};

extern std::unique_ptr<FramebufferManager> g_framebuffer_manager;
