// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <d3d12.h>
#include <memory>

#include "VideoCommon/AbstractPipeline.h"

namespace DX12
{
// A graphics pipeline state object plus what has to be bound alongside it.
// The pipeline state reference is owned; the root signature belongs to the
// context.
class DXPipeline final : public AbstractPipeline
{
public:
  DXPipeline(const AbstractPipelineConfig& config, ID3D12PipelineState* pipeline,
             ID3D12RootSignature* root_signature, AbstractPipelineUsage usage,
             D3D12_PRIMITIVE_TOPOLOGY primitive_topology, bool use_integer_rtv);
  ~DXPipeline() override;

  // cache_data, when given, is a blob from GetCacheData() of an earlier run.
  static std::unique_ptr<DXPipeline> Create(const AbstractPipelineConfig& config,
                                            const void* cache_data, size_t cache_data_size);

  ID3D12PipelineState* GetPipeline() const { return m_pipeline; }
  ID3D12RootSignature* GetRootSignature() const { return m_root_signature; }
  AbstractPipelineUsage GetUsage() const { return m_usage; }
  D3D12_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const { return m_primitive_topology; }

  // Logic ops write through the integer view of the render target.
  bool UseIntegerRTV() const { return m_use_integer_rtv; }

  CacheData GetCacheData() const override;

private:
  ID3D12PipelineState* m_pipeline;
  ID3D12RootSignature* m_root_signature;
  AbstractPipelineUsage m_usage;
  D3D12_PRIMITIVE_TOPOLOGY m_primitive_topology;
  bool m_use_integer_rtv;
};

}  // namespace DX12
