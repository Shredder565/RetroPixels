// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/LinearDiskCache.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/GXPipelineTypes.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/TextureConverterShaderGen.h"
#include "VideoCommon/UberShaderPixel.h"
#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoEvents.h"

class NativeVertexFormat;
enum class AbstractTextureFormat : u32;
enum class APIType;
enum class TextureFormat;
enum class TLUTFormat;

namespace VideoCommon
{
// Owns every shader and pipeline the video core asks the backend for: the GX pipelines (both
// specialised and ubershader), their stages, the on-disk caches of those, the per-game list of
// pipeline UIDs that have been seen, and the fixed utility shaders and pipelines.
class ShaderCache final
{
public:
  ShaderCache();
  ~ShaderCache();

  // Builds the utility shaders and pipelines and the async compiler. Returns false on failure.
  bool Initialize();
  void Shutdown();

  // Loads the disk caches and starts compiling everything known to be needed.
  void InitializeShaderCache();

  // Takes effect at the next Reload.
  void SetHostConfig(const ShaderHostConfig& host_config) { m_host_config.bits = host_config.bits; }

  // Throws away every shader and pipeline and builds them again.
  void Reload();

  // Hands finished background compiles over to the caches.
  void RetrieveAsyncShaders();

  // GX pipelines, compiled on the spot when missing.
  const AbstractPipeline* GetPipelineForUid(const GXPipelineUid& uid);
  const AbstractPipeline* GetUberPipelineForUid(const GXUberPipelineUid& uid);

  // Never blocks: an empty result means the pipeline is (now) being compiled in the background.
  std::optional<const AbstractPipeline*> GetPipelineForUidAsync(const GXPipelineUid& uid);

  // Utility shaders.
  const AbstractShader* GetScreenQuadVertexShader() const { return m_screen_quad_vs.get(); }
  const AbstractShader* GetTextureCopyVertexShader() const { return m_texture_copy_vs.get(); }
  const AbstractShader* GetEFBCopyVertexShader() const { return m_efb_copy_vs.get(); }
  const AbstractShader* GetTexcoordGeometryShader() const { return m_texcoord_gs.get(); }
  const AbstractShader* GetTextureCopyPixelShader() const { return m_texture_copy_ps.get(); }
  const AbstractShader* GetColorGeometryShader() const { return m_color_gs.get(); }
  const AbstractShader* GetColorPixelShader() const { return m_color_ps.get(); }

  // EFB copy pipelines.
  const AbstractPipeline*
  GetEFBCopyToVRAMPipeline(const TextureConversionShaderGen::TCShaderUid& uid);
  const AbstractPipeline* GetEFBCopyToRAMPipeline(const EFBCopyParams& uid);

  // Plain RGBA8 copies (the stereo one also writes every layer).
  const AbstractPipeline* GetRGBA8CopyPipeline() const { return m_rgba8_copy.get(); }
  const AbstractPipeline* GetRGBA8StereoCopyPipeline() const { return m_rgba8_stereo_copy.get(); }

  // Palette lookup on the GPU, one per TLUT format.
  const AbstractPipeline* GetPaletteConversionPipeline(TLUTFormat format);

  // Reads a texture of one format as another.
  const AbstractPipeline* GetTextureReinterpretPipeline(TextureFormat from_format,
                                                        TextureFormat to_format);

  // Compute shaders that decode guest textures.
  const AbstractShader* GetTextureDecodingShader(TextureFormat format,
                                                 std::optional<TLUTFormat> palette_format);

private:
  static constexpr size_t NUM_PALETTE_CONVERSION_SHADERS = 3;

  // Background compile jobs (defined in the .cpp).
  template <typename Uid>
  class StageJob;
  template <typename Uid>
  class PipelineJob;

  // Lower values are compiled first. Pipelines needed right now beat the ubershader set, which
  // beats whatever the UID list says might be needed later.
  enum : u32
  {
    COMPILE_PRIORITY_ONDEMAND_PIPELINE = 100,
    COMPILE_PRIORITY_UBERSHADER_PIPELINE = 200,
    COMPILE_PRIORITY_SHADERCACHE_PIPELINE = 300
  };

  template <typename Uid>
  struct ShaderModuleCache
  {
    struct Shader
    {
      std::unique_ptr<AbstractShader> shader;
      bool pending = false;
    };
    std::map<Uid, Shader> shader_map;
    Common::LinearDiskCache<Uid, u8> disk_cache;
  };

  // A pipeline slot: the object (null until compiled, or when compiling failed) and whether a
  // background compile is outstanding.
  using PipelineSlot = std::pair<std::unique_ptr<AbstractPipeline>, bool>;

  void WaitForAsyncCompiler();
  void LoadCaches();
  void ClearCaches();
  void OpenPipelineUIDList();
  void ClosePipelineUIDList();
  void QueueUnbuiltPipelines();
  void QueueUberShaderPipelines();
  // Queues every unbuilt pipeline, optionally waits for them, then drops to the runtime
  // compiler thread count.
  void FinishPrecompile();
  bool BuildUtilityShaders();

  // Stage compilation.
  std::unique_ptr<AbstractShader> CompileVertexShader(const VertexShaderUid& uid) const;
  std::unique_ptr<AbstractShader>
  CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const;
  std::unique_ptr<AbstractShader> CompilePixelShader(const PixelShaderUid& uid) const;
  std::unique_ptr<AbstractShader>
  CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const;
  const AbstractShader* InsertVertexShader(const VertexShaderUid& uid,
                                           std::unique_ptr<AbstractShader> shader);
  const AbstractShader* InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                               std::unique_ptr<AbstractShader> shader);
  const AbstractShader* InsertPixelShader(const PixelShaderUid& uid,
                                          std::unique_ptr<AbstractShader> shader);
  const AbstractShader* InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                              std::unique_ptr<AbstractShader> shader);
  const AbstractShader* CreateGeometryShader(const GeometryShaderUid& uid);
  bool NeedsGeometryShader(const GeometryShaderUid& uid) const;
  bool UseGeometryShaderForEFBCopies() const;

  template <typename Uid>
  const AbstractShader* StoreShader(ShaderModuleCache<Uid>& cache, const Uid& uid,
                                    std::unique_ptr<AbstractShader> shader, ShaderStage stage);

  // Pipeline configuration and storage.
  AbstractPipelineConfig
  GetGXPipelineConfig(const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
                      const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
                      const RasterizationState& rasterization_state, const DepthState& depth_state,
                      const BlendingState& blending_state, AbstractPipelineUsage usage);
  std::optional<AbstractPipelineConfig> GetGXPipelineConfig(const GXPipelineUid& uid);
  std::optional<AbstractPipelineConfig> GetGXPipelineConfig(const GXUberPipelineUid& uid);
  // The geometry stage a pipeline needs: null when it needs none, nullopt when it failed.
  std::optional<const AbstractShader*> GeometryStageFor(const GeometryShaderUid& uid);
  const AbstractPipeline* InsertGXPipeline(const GXPipelineUid& uid,
                                           std::unique_ptr<AbstractPipeline> pipeline);
  const AbstractPipeline* InsertGXUberPipeline(const GXUberPipelineUid& uid,
                                               std::unique_ptr<AbstractPipeline> pipeline);
  void AddSerializedGXPipelineUID(const SerializedGXPipelineUid& uid);
  void AppendGXPipelineUID(const GXPipelineUid& uid);

  // Background compilation.
  void QueueVertexShaderCompile(const VertexShaderUid& uid, u32 priority);
  void QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid, u32 priority);
  void QueuePixelShaderCompile(const PixelShaderUid& uid, u32 priority);
  void QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid, u32 priority);
  void QueuePipelineCompile(const GXPipelineUid& uid, u32 priority);
  void QueueUberPipelineCompile(const GXUberPipelineUid& uid, u32 priority);
  // Whether both shader stages of a pipeline are built; queues the ones never requested.
  bool PipelineStagesReady(const GXPipelineUid& uid, u32 priority);
  bool PipelineStagesReady(const GXUberPipelineUid& uid, u32 priority);

  // Disk caches.
  template <ShaderStage stage, typename K, typename T>
  void LoadShaderCache(T& cache, APIType api_type, const char* type, bool include_gameid);
  template <typename T>
  void ClearShaderCache(T& cache);
  template <typename KeyType, typename DiskKeyType, typename T>
  void LoadPipelineCache(T& cache, Common::LinearDiskCache<DiskKeyType, u8>& disk_cache,
                         APIType api_type, const char* type, bool include_gameid);
  template <typename T, typename Y>
  void ClearPipelineCache(T& cache, Y& disk_cache);

  APIType m_api_type;
  ShaderHostConfig m_host_config = {};
  std::unique_ptr<AsyncShaderCompiler> m_async_shader_compiler;

  // Utility shaders.
  std::unique_ptr<AbstractShader> m_screen_quad_vs;
  std::unique_ptr<AbstractShader> m_texture_copy_vs;
  std::unique_ptr<AbstractShader> m_efb_copy_vs;
  std::unique_ptr<AbstractShader> m_texcoord_gs;
  std::unique_ptr<AbstractShader> m_color_gs;
  std::unique_ptr<AbstractShader> m_texture_copy_ps;
  std::unique_ptr<AbstractShader> m_color_ps;

  // GX shader stages.
  ShaderModuleCache<VertexShaderUid> m_vs_cache;
  ShaderModuleCache<GeometryShaderUid> m_gs_cache;
  ShaderModuleCache<PixelShaderUid> m_ps_cache;
  ShaderModuleCache<UberShader::VertexShaderUid> m_uber_vs_cache;
  ShaderModuleCache<UberShader::PixelShaderUid> m_uber_ps_cache;

  // GX pipelines, the per-game UID list file, and the backend pipeline caches.
  std::map<GXPipelineUid, PipelineSlot> m_gx_pipeline_cache;
  std::map<GXUberPipelineUid, PipelineSlot> m_gx_uber_pipeline_cache;
  File::IOFile m_uid_list_file;
  Common::LinearDiskCache<SerializedGXPipelineUid, u8> m_gx_pipeline_disk_cache;
  Common::LinearDiskCache<SerializedGXUberPipelineUid, u8> m_gx_uber_pipeline_disk_cache;

  // EFB copies.
  std::map<TextureConversionShaderGen::TCShaderUid, std::unique_ptr<AbstractPipeline>>
      m_efb_copy_to_vram_pipelines;
  std::map<EFBCopyParams, std::unique_ptr<AbstractPipeline>> m_efb_copy_to_ram_pipelines;

  std::unique_ptr<AbstractPipeline> m_rgba8_copy;
  std::unique_ptr<AbstractPipeline> m_rgba8_stereo_copy;

  std::array<std::unique_ptr<AbstractPipeline>, NUM_PALETTE_CONVERSION_SHADERS>
      m_palette_conversion_pipelines;

  std::map<std::pair<TextureFormat, TextureFormat>, std::unique_ptr<AbstractPipeline>>
      m_texture_reinterpret_pipelines;

  // Keyed by (texture format, palette format or IA8).
  std::map<std::pair<u32, u32>, std::unique_ptr<AbstractShader>> m_texture_decoding_shaders;

  Common::EventHook m_frame_end_hook;
};

}  // namespace VideoCommon

extern std::unique_ptr<VideoCommon::ShaderCache> g_shader_cache;
