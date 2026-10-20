// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"
#include "VideoCommon/CPUCull.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/VideoEvents.h"

struct CustomPixelShaderContents;
class CustomShaderCache;
class DataReader;
class GeometryShaderManager;
class NativeVertexFormat;
class PixelShaderManager;
class PointerWrap;
struct PortableVertexDeclaration;

// Depth plane of the last triangle drawn without z-freeze: z = f0 + dfdx * x + dfdy * y in
// screen space. Saved in savestates as it is.
struct Slope
{
  float dfdx;
  float dfdy;
  float f0;
  bool dirty;
};

// Element formats of the texel buffer read by the texture decoding shaders.
enum TexelBufferFormat : u32
{
  TEXEL_BUFFER_FORMAT_R8_UINT,
  TEXEL_BUFFER_FORMAT_R16_UINT,
  TEXEL_BUFFER_FORMAT_RGBA8_UINT,
  TEXEL_BUFFER_FORMAT_R32G32_UINT,
  NUM_TEXEL_BUFFER_FORMATS
};

namespace OpcodeDecoder
{
enum class Primitive : u8;
}

// Collects converted GX vertices and indices into batches and draws a batch whenever state
// that affects it changes. Backends derive from this and supply the buffers and the draw.
class VertexManagerBase
{
private:
  // Smallest host vertex: an XYZ position.
  static constexpr u32 SMALLEST_POSSIBLE_VERTEX = sizeof(float) * 3;
  // Largest host vertex: position, normal/tangent/binormal, eight 3-float texture coordinates,
  // the position matrix index and two colours.
  static constexpr u32 LARGEST_POSSIBLE_VERTEX = sizeof(float) * 45 + sizeof(u32) * 2;

  static constexpr u32 MAX_PRIMITIVES_PER_COMMAND = 65535;

  // Projection statistics for the 16:9 anamorphic widescreen heuristic.
  struct FlushStatistics
  {
    struct ProjectionCounts
    {
      size_t normal_flush_count;
      size_t anamorphic_flush_count;
      size_t other_flush_count;

      // Despite the names, these add up index counts.
      size_t normal_vertex_count;
      size_t anamorphic_vertex_count;
      size_t other_vertex_count;

      size_t GetTotalFlushCount() const
      {
        return normal_flush_count + anamorphic_flush_count + other_flush_count;
      }

      size_t GetTotalVertexCount() const
      {
        return normal_vertex_count + anamorphic_vertex_count + other_vertex_count;
      }

      MathUtil::RunningMean<float> average_ratio;
    };

    ProjectionCounts perspective;
    ProjectionCounts orthographic;
  };

public:
  static constexpr u32 MAXVBUFFERSIZE =
      MathUtil::NextPowerOf2(MAX_PRIMITIVES_PER_COMMAND * LARGEST_POSSIBLE_VERTEX);

  // Fans become triangle lists: nearly three indices per vertex.
  static constexpr u32 MAXIBUFFERSIZE = MathUtil::NextPowerOf2(MAX_PRIMITIVES_PER_COMMAND * 3);

  // Streaming buffer sizes. The texel buffer holds the largest encoded GX texture
  // (1024x1024 RGBA8, 4 MiB) with room to spare.
  static constexpr u32 VERTEX_STREAM_BUFFER_SIZE = 48 * 1024 * 1024;
  static constexpr u32 INDEX_STREAM_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 UNIFORM_STREAM_BUFFER_SIZE = 64 * 1024 * 1024;
  static constexpr u32 TEXEL_STREAM_BUFFER_SIZE = 16 * 1024 * 1024;

  VertexManagerBase();
  virtual ~VertexManagerBase();

  virtual bool Initialize();

  PrimitiveType GetCurrentPrimitiveType() const { return m_current_primitive_type; }
  void AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices);
  bool AreAllVerticesCulled(VertexLoaderBase* loader, OpcodeDecoder::Primitive primitive,
                            const u8* src, u32 count);
  // Makes room for `count` vertices of `stride` bytes and returns where to write them. With
  // cullall the vertices go to a CPU-only buffer and are never drawn.
  virtual DataReader PrepareForAdditionalData(OpcodeDecoder::Primitive primitive, u32 count,
                                              u32 stride, bool cullall);
  // After PrepareForAdditionalData(cullall = true): the vertices are to be drawn after all.
  // Returns the drawable buffer position (the caller moves the vertices there).
  DataReader DisableCullAll(u32 stride);
  // Commits `count` vertices written at the position PrepareForAdditionalData returned.
  void FlushData(u32 count, u32 stride);

  // Draws the current batch, if any.
  void Flush();
  bool HasSendableVertices() const { return !m_is_flushed && !m_cull_all; }

  void DoState(PointerWrap& p);

  FlushStatistics ResetFlushAspectRatioCount();

  // Called when the matching register groups change.
  void SetRasterizationStateChanged() { m_rasterization_state_changed = true; }
  void SetDepthStateChanged() { m_depth_state_changed = true; }
  void SetBlendingStateChanged() { m_blending_state_changed = true; }
  void InvalidatePipelineObject()
  {
    m_current_pipeline_object = nullptr;
    m_pipeline_config_changed = true;
  }
  void NotifyCustomShaderCacheOfHostChange(const ShaderHostConfig& host_config);

  // Utility draws (EFB copies, post-processing, UI) through the same buffers.
  virtual void UploadUtilityUniforms(const void* uniforms, u32 uniforms_size);
  void UploadUtilityVertices(const void* vertices, u32 vertex_stride, u32 num_vertices,
                             const u16* indices, u32 num_indices, u32* out_base_vertex,
                             u32* out_base_index);

  // Bytes per texel buffer element of a format.
  static u32 GetTexelBufferElementSize(TexelBufferFormat buffer_format);

  // Texel buffer uploads (palette conversion, GPU texture decoding). The second form also
  // uploads a palette into the same buffer.
  virtual bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                 u32* out_offset);
  virtual bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                 u32* out_offset, const void* palette_data, u32 palette_size,
                                 TexelBufferFormat palette_format, u32* out_palette_offset);

  // The active configuration changed.
  void OnConfigChange();

  // Command buffer scheduling around CPU reads of the EFB. OnDraw follows every draw,
  // OnCPUEFBAccess a CPU access, OnEFBCopyToRAM an EFB copy to RAM, OnEndFrame each frame.
  void OnDraw();
  void OnCPUEFBAccess();
  void OnEFBCopyToRAM();
  void OnEndFrame();

protected:
  // Marks the GX uniforms for upload again (after utility uniforms replaced them).
  static void InvalidateConstants();

  // Starts a new batch in the backend's buffers.
  virtual void ResetBuffer(u32 vertex_stride);
  // Hands the batch to the backend; returns where its vertices and indices start.
  virtual void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                            u32* out_base_vertex, u32* out_base_index);
  // Uploads the GX uniform blocks.
  virtual void UploadUniforms();
  // Draws the committed batch.
  virtual void DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex);

  u8* m_cur_buffer_pointer = nullptr;
  u8* m_base_buffer_pointer = nullptr;
  u8* m_end_buffer_pointer = nullptr;

  // CPU-side buffers: the default batch storage, and where culled vertices go.
  std::vector<u8> m_cpu_vertex_buffer;
  std::vector<u16> m_cpu_index_buffer;

  PrimitiveType m_current_primitive_type = PrimitiveType::Points;
  IndexGenerator m_index_generator;

private:
  // A command buffer is only kicked early for a readback when it has at least this many draws.
  static constexpr u32 MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK = 10;

  u32 GetRemainingSize() const;
  u32 GetRemainingIndices(OpcodeDecoder::Primitive primitive) const;
  BitSet32 UsedTextures() const;
  void UpdateFlushStatistics();
  void CalculateZSlope(NativeVertexFormat* format);
  void CalculateNormals(NativeVertexFormat* format);
  void DrawBatch(const BitSet32& used_textures, const std::array<SamplerState, 8>& samplers,
                 const std::vector<std::string>& texture_names,
                 const Common::SmallVector<u32, 8>& texture_units,
                 PixelShaderManager& pixel_shader_manager,
                 GeometryShaderManager& geometry_shader_manager);
  void RenderDrawCall(PixelShaderManager& pixel_shader_manager,
                      GeometryShaderManager& geometry_shader_manager,
                      std::span<u8> custom_pixel_shader_uniforms, PrimitiveType primitive_type,
                      const AbstractPipeline* current_pipeline);
  void UpdatePipelineConfig();
  void UpdatePipelineObject();
  const AbstractPipeline*
  GetCustomPipeline(const CustomPixelShaderContents& custom_pixel_shader_contents,
                    const AbstractPipeline* current_pipeline) const;

  Slope m_zslope = {};

  VideoCommon::GXPipelineUid m_current_pipeline_config;
  VideoCommon::GXUberPipelineUid m_current_uber_pipeline_config;
  const AbstractPipeline* m_current_pipeline_object = nullptr;
  bool m_pipeline_config_changed = true;
  bool m_rasterization_state_changed = true;
  bool m_depth_state_changed = true;
  bool m_blending_state_changed = true;
  bool m_cull_all = false;
  bool m_is_flushed = true;

  CPUCull m_cpu_cull;
  FlushStatistics m_flush_statistics = {};

  // CPU access tracking.
  u32 m_draw_counter = 0;
  u32 m_last_efb_copy_draw_counter = 0;
  bool m_unflushed_efb_copy = false;
  std::vector<u32> m_cpu_accesses_this_frame;
  std::vector<u32> m_scheduled_command_buffer_kicks;
  bool m_allow_background_execution = true;

  std::unique_ptr<CustomShaderCache> m_custom_shader_cache;
  u64 m_ticks_elapsed = 0;

  Common::EventHook m_frame_end_event;
  Common::EventHook m_after_present_event;
};

extern std::unique_ptr<VertexManagerBase> g_vertex_manager;
