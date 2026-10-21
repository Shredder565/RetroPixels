// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <list>
#include <map>
#include <memory>
#include <optional>

#include "Common/HookableEvent.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/GXPipelineTypes.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/UberShaderPixel.h"

// The custom shader pieces a draw asks for; part of every cache key below.
struct CustomShaderInstance
{
  CustomPixelShaderContents pixel_contents;

  bool operator==(const CustomShaderInstance& other) const = default;
};

// Builds pixel shaders and pipelines for draws that use custom (graphics mod) shaders, in the
// background. Specialized and uber pipelines each have their own compiler and caches.
class CustomShaderCache
{
public:
  CustomShaderCache();
  ~CustomShaderCache();
  CustomShaderCache(const CustomShaderCache&) = delete;
  CustomShaderCache(CustomShaderCache&&) = delete;
  CustomShaderCache& operator=(const CustomShaderCache&) = delete;
  CustomShaderCache& operator=(CustomShaderCache&&) = delete;

  // Takes effect for shaders generated from now on; call Reload to rebuild existing ones.
  void SetHostConfig(const ShaderHostConfig& host_config) { m_host_config.bits = host_config.bits; }

  // Collects finished compiles (done automatically at the end of every frame).
  void RetrieveAsyncShaders();

  // Waits for outstanding compiles, then drops every cached shader and pipeline.
  void Reload();

  // A pipeline (null if creating it failed), or nothing while it is still being built. The
  // first request for a key starts building it.
  std::optional<const AbstractPipeline*>
  GetPipelineAsync(const VideoCommon::GXPipelineUid& uid,
                   const CustomShaderInstance& custom_shaders,
                   const AbstractPipelineConfig& pipeline_config);
  std::optional<const AbstractPipeline*>
  GetPipelineAsync(const VideoCommon::GXUberPipelineUid& uid,
                   const CustomShaderInstance& custom_shaders,
                   const AbstractPipelineConfig& pipeline_config);

private:
  // Results keyed by (uid, custom shader instance). Entries never move once added.
  template <typename Uid, typename Value>
  class InstanceCache
  {
  public:
    struct Slot
    {
      std::unique_ptr<Value> value;
      bool pending = true;
    };

    const Slot* Find(const Uid& uid, const CustomShaderInstance& instance) const
    {
      const auto bucket = m_by_uid.find(uid);
      if (bucket == m_by_uid.end())
        return nullptr;
      for (const auto& entry : bucket->second)
      {
        if (entry.first == instance)
          return &entry.second;
      }
      return nullptr;
    }

    Slot* Add(const Uid& uid, const CustomShaderInstance& instance)
    {
      auto& bucket = m_by_uid[uid];
      return &bucket.emplace(bucket.begin(), instance, Slot{})->second;
    }

  private:
    std::map<Uid, std::list<std::pair<CustomShaderInstance, Slot>>> m_by_uid;
  };

  // One kind of pipeline (specialized or uber): its compiler and its two caches.
  template <typename PipelineUid, typename ShaderUid>
  struct Lane
  {
    std::unique_ptr<VideoCommon::AsyncShaderCompiler> compiler;
    InstanceCache<ShaderUid, AbstractShader> shaders;
    InstanceCache<PipelineUid, AbstractPipeline> pipelines;
  };

  struct SpecializedTraits;
  struct UberTraits;
  template <typename Traits>
  class PipelineJob;
  template <typename Traits>
  class ShaderJob;

  template <typename Traits>
  std::optional<const AbstractPipeline*> Lookup(const typename Traits::PipelineUid& uid,
                                                const CustomShaderInstance& custom_shaders,
                                                const AbstractPipelineConfig& pipeline_config);
  template <typename Traits>
  void QueueShader(const typename Traits::ShaderUid& uid,
                   const CustomShaderInstance& custom_shaders);

  APIType m_api_type = APIType::Nothing;
  ShaderHostConfig m_host_config = {};

  Lane<VideoCommon::GXPipelineUid, PixelShaderUid> m_specialized;
  Lane<VideoCommon::GXUberPipelineUid, UberShader::PixelShaderUid> m_uber;

  Common::EventHook m_frame_end_hook;
};
