// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DX12Shader.h"

#include "Common/Assert.h"
#include "Common/StringUtil.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/DX12Context.h"

namespace DX12
{
DXShader::DXShader(ShaderStage stage, BinaryData bytecode, std::string_view name)
    : D3DCommon::Shader(stage, std::move(bytecode)), m_name(UTF8ToWString(name))
{
}

DXShader::~DXShader() = default;

std::unique_ptr<DXShader> DXShader::CreateFromSource(ShaderStage stage, std::string_view source,
                                                     VideoCommon::ShaderIncluder* shader_includer,
                                                     std::string_view name)
{
  std::optional<BinaryData> compiled =
      CompileShader(g_dx_context->GetFeatureLevel(), stage, source, shader_includer);
  if (!compiled.has_value())
    return nullptr;
  return CreateFromBytecode(stage, std::move(compiled.value()), name);
}

std::unique_ptr<DXShader> DXShader::CreateFromBytecode(ShaderStage stage, BinaryData bytecode,
                                                       std::string_view name)
{
  std::unique_ptr<DXShader> shader(new DXShader(stage, std::move(bytecode), name));
  const bool needs_pipeline = stage == ShaderStage::Compute;
  if (needs_pipeline && !shader->CreateComputePipeline())
    return nullptr;
  return shader;
}

D3D12_SHADER_BYTECODE DXShader::GetD3DByteCode() const
{
  D3D12_SHADER_BYTECODE code;
  code.pShaderBytecode = m_bytecode.data();
  code.BytecodeLength = m_bytecode.size();
  return code;
}

bool DXShader::CreateComputePipeline()
{
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = g_dx_context->GetComputeRootSignature();
  desc.CS = GetD3DByteCode();
  desc.NodeMask = 1;

  const HRESULT hr = g_dx_context->GetDevice()->CreateComputePipelineState(
      &desc, IID_PPV_ARGS(&m_compute_pipeline));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Creating compute pipeline failed: {}", DX12HRWrap(hr));

  // Name whatever came back, even alongside a failure code.
  if (m_compute_pipeline != nullptr && !m_name.empty())
    m_compute_pipeline->SetName(m_name.c_str());
  return SUCCEEDED(hr);
}

}  // namespace DX12
