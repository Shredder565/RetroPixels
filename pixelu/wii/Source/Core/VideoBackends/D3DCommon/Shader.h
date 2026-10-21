// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string_view>

#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/AbstractShader.h"

namespace D3DCommon
{
// A compiled DXBC shader. The D3D11 and D3D12 backends derive from this and
// add their own API objects; the bytecode itself is the shader's binary.
class Shader : public AbstractShader
{
public:
  ~Shader() override;

  const BinaryData& GetByteCode() const { return m_bytecode; }

  BinaryData GetBinary() const override;

  // Turns renderer GLSL into DXBC for the given feature level: GLSL goes
  // through SPIR-V and SPIRV-Cross to HLSL, except geometry shaders, whose
  // source is already HLSL. Returns nothing (after an alert) on failure.
  static std::optional<BinaryData> CompileShader(D3D_FEATURE_LEVEL feature_level, ShaderStage stage,
                                                 std::string_view source,
                                                 VideoCommon::ShaderIncluder* shader_includer);

  // Copies a bytecode blob into a BinaryData.
  static BinaryData CreateByteCode(const void* data, size_t length);

protected:
  Shader(ShaderStage stage, BinaryData bytecode);

  BinaryData m_bytecode;
};

}  // namespace D3DCommon
