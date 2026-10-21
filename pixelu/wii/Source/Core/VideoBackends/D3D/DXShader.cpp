// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/DXShader.h"

#include "Common/Assert.h"

#include "VideoBackends/D3D/D3DBase.h"

namespace DX11
{
namespace
{
// Makes the stage's shader object from bytecode; Create is the device method
// for that stage.
template <typename Interface, typename CreateFn>
std::unique_ptr<DXShader> MakeStage(ShaderStage stage, AbstractShader::BinaryData& bytecode,
                                    std::string_view name, const char* stage_name, CreateFn create)
{
  ComPtr<Interface> object;
  const HRESULT result =
      (D3D::device.Get()->*create)(bytecode.data(), bytecode.size(), nullptr, &object);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create {} shader: {}", stage_name,
             DX11HRWrap(result));
  if (FAILED(result))
    return nullptr;
  return std::make_unique<DXShader>(stage, std::move(bytecode), object.Get(), name);
}

template <typename Interface>
Interface* ObjectAs(const ComPtr<ID3D11DeviceChild>& object, ShaderStage actual,
                    ShaderStage expected)
{
  DEBUG_ASSERT(actual == expected);
  return static_cast<Interface*>(object.Get());
}
}  // namespace

DXShader::DXShader(ShaderStage stage, BinaryData bytecode, ID3D11DeviceChild* shader,
                   std::string_view name)
    : D3DCommon::Shader(stage, std::move(bytecode)), m_shader(shader), m_name(name)
{
  if (m_name.empty())
    return;
  m_shader->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(m_name.size()),
                           m_name.data());
}

DXShader::~DXShader() = default;

// Each getter is only meant for a shader of its own stage.
ID3D11VertexShader* DXShader::GetD3DVertexShader() const
{
  return ObjectAs<ID3D11VertexShader>(m_shader, m_stage, ShaderStage::Vertex);
}

ID3D11GeometryShader* DXShader::GetD3DGeometryShader() const
{
  return ObjectAs<ID3D11GeometryShader>(m_shader, m_stage, ShaderStage::Geometry);
}

ID3D11PixelShader* DXShader::GetD3DPixelShader() const
{
  return ObjectAs<ID3D11PixelShader>(m_shader, m_stage, ShaderStage::Pixel);
}

ID3D11ComputeShader* DXShader::GetD3DComputeShader() const
{
  return ObjectAs<ID3D11ComputeShader>(m_shader, m_stage, ShaderStage::Compute);
}

std::unique_ptr<DXShader> DXShader::CreateFromBytecode(ShaderStage stage, BinaryData bytecode,
                                                       std::string_view name)
{
  switch (stage)
  {
  case ShaderStage::Vertex:
    return MakeStage<ID3D11VertexShader>(stage, bytecode, name, "vertex",
                                         &ID3D11Device::CreateVertexShader);
  case ShaderStage::Geometry:
    return MakeStage<ID3D11GeometryShader>(stage, bytecode, name, "geometry",
                                           &ID3D11Device::CreateGeometryShader);
  case ShaderStage::Pixel:
    return MakeStage<ID3D11PixelShader>(stage, bytecode, name, "pixel",
                                        &ID3D11Device::CreatePixelShader);
  case ShaderStage::Compute:
    return MakeStage<ID3D11ComputeShader>(stage, bytecode, name, "compute",
                                          &ID3D11Device::CreateComputeShader);
  default:
    return nullptr;
  }
}
}  // namespace DX11
