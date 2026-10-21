// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Common/Buffer.h"
#include "Common/SmallVector.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/Assets/MaterialAsset.h"
#include "VideoCommon/Assets/TextureSamplerValue.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/GXPipelineTypes.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/Resources/Resource.h"
#include "VideoCommon/Resources/ShaderResource.h"
#include "VideoCommon/Resources/TextureAndSamplerResource.h"

namespace VideoCommon
{
// A material asset turned into a ready pipeline: its shader resource, the textures it samples,
// its packed uniform values and, optionally, a chained material drawn after it.
class MaterialResource final : public Resource
{
public:
  // An EFB post processing material.
  explicit MaterialResource(Resource::ResourceContext resource_context);
  // A material replacing the draws that use `pipeline_uid`.
  MaterialResource(Resource::ResourceContext resource_context, GXPipelineUid pipeline_uid);

  struct TextureLikeReference
  {
    SamplerState sampler;
    u32 sampler_index;
    TextureSamplerValue::SamplerOrigin sampler_origin;
    std::string_view texture_hash;
    AbstractTexture* texture;
  };

  class Data
  {
  public:
    AbstractPipeline* GetPipeline() const { return m_pipeline.get(); }
    std::span<const u8> GetUniforms() const { return m_uniforms; }
    std::span<const TextureLikeReference> GetTextures() const { return m_bindings; }
    MaterialResource* GetNextMaterial() const { return m_next; }

  private:
    friend class MaterialResource;
    std::unique_ptr<AbstractPipeline> m_pipeline = nullptr;
    Common::UniqueBuffer<u8> m_uniforms;
    std::shared_ptr<MaterialData> m_material = nullptr;
    ShaderResource* m_shader = nullptr;

    // One entry per texture slot of the material; a slot without an asset stays empty.
    using TextureSource = std::variant<std::shared_ptr<TextureAndSamplerResource::Data>>;
    Common::SmallVector<Resource*, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_texture_resources;
    Common::SmallVector<TextureSource, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_texture_sources;
    Common::SmallVector<TextureLikeReference, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_bindings;

    MaterialResource* m_next = nullptr;
    std::size_t m_shader_key;
    std::string m_defines;
    std::atomic_bool m_build_done;
  };

  const std::shared_ptr<Data>& GetData() const { return m_live; }
  void MarkAsActive() override;
  void MarkAsPending() override;

private:
  void ResetData() override;
  Resource::TaskComplete CollectPrimaryData() override;
  Resource::TaskComplete CollectDependencyData() override;
  Resource::TaskComplete ProcessData() override;

  // Fills the binding slots and the HAS_SAMPLER_n defines from the material's texture list.
  static void PrepareBindings(Data& data);
  // Hash of the pipeline (if any) and the defines; picks the matching shader resource.
  static void ComputeShaderKey(Data& data, const GXPipelineUid* uid);
  // Fills in AbstractTexture pointers and samplers once all textures are ready.
  void ResolveBindings(Data& data, ShaderResource::Data& shader) const;
  // Runs on the shader compiler thread.
  static bool BuildPipeline(Data& data, const ShaderResource::Data& shader,
                            const GXPipelineUid* uid, FramebufferState framebuffer);
  static void PackUniforms(Data& data);

  std::shared_ptr<Data> m_live;
  std::shared_ptr<Data> m_staging;
  // Whether m_staging has already been handed to the shader compiler.
  bool m_build_queued = false;

  // Owned by the asset cache.
  MaterialAsset* m_asset = nullptr;

  // Set for custom draw materials, empty for EFB post processing ones. Its vertex format points
  // at m_vertex_format, a copy owned here.
  std::optional<GXPipelineUid> m_uid;
  std::unique_ptr<NativeVertexFormat> m_vertex_format;
};
}  // namespace VideoCommon
