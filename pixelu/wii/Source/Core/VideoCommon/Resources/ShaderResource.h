// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "VideoCommon/Resources/Resource.h"

#include "VideoCommon/Assets/ShaderAsset.h"
#include "VideoCommon/GXPipelineTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

namespace VideoCommon
{
// Compiles the vertex, pixel and (when needed) geometry shaders for a raster surface shader
// asset. With a pipeline UID the custom code is wrapped in the emulated GX shaders for that
// draw; without one it is wrapped in a full screen post processing pass.
class ShaderResource final : public Resource
{
public:
  ShaderResource(Resource::ResourceContext resource_context,
                 std::optional<GXPipelineUid> pipeline_uid, std::string preprocessor_setting,
                 const ShaderHostConfig& shader_host_config);

  class Data
  {
  public:
    AbstractShader* GetVertexShader() const;
    AbstractShader* GetPixelShader() const;
    AbstractShader* GetGeometryShader() const;

    // All required stages were created.
    bool IsCompiled() const;
    // The texture type the shader declares for sampler `index` (2D when unknown).
    AbstractTextureType GetTextureType(std::size_t index);

  private:
    friend class ShaderResource;
    std::unique_ptr<AbstractShader> m_vs;
    std::unique_ptr<AbstractShader> m_ps;
    std::unique_ptr<AbstractShader> m_gs;
    std::shared_ptr<RasterSurfaceShaderData> m_source;
    bool m_gs_required = false;
    std::atomic_bool m_compile_done;
  };

  // Changes the shader host config. Shaders should be reloaded afterwards.
  void SetHostConfig(const ShaderHostConfig& host_config);
  const std::shared_ptr<Data>& GetData() const { return m_live; }

  void MarkAsActive() override;
  void MarkAsPending() override;

private:
  void ResetData() override;
  Resource::TaskComplete CollectPrimaryData() override;
  TaskComplete ProcessData() override;

  // Runs on the shader compiler thread. A null uid builds the post processing variant.
  static void CompileStages(Data& data, GXPipelineUid* uid, const ShaderHostConfig& host,
                            std::string_view defines);

  // Owned by the asset cache.
  RasterSurfaceShaderAsset* m_asset = nullptr;

  std::shared_ptr<Data> m_live;
  std::shared_ptr<Data> m_staging;

  // Whether m_staging has already been handed to the shader compiler.
  bool m_compile_queued = false;

  ShaderHostConfig m_host_config;

  // Set for custom draw shaders, empty for EFB post processing shaders.
  std::optional<GXPipelineUid> m_uid;
  std::string m_defines;
};
}  // namespace VideoCommon
