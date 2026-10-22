// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Declared here so that every backend shader class sees the includer type without its own
// forward declaration.
namespace VideoCommon
{
class ShaderIncluder;
}

enum class ShaderStage
{
  Vertex,
  Geometry,
  Pixel,
  Compute
};

// A compiled shader object owned by a backend.
class AbstractShader
{
public:
  explicit AbstractShader(ShaderStage stage) : m_stage(stage) {}
  virtual ~AbstractShader() = default;

  ShaderStage GetStage() const { return m_stage; }

  // Backend-specific compiled form (SPIR-V, DXBC, ...), from which the shader can be recreated
  // without its source. Empty when the backend has no such form.
  using BinaryData = std::vector<u8>;
  virtual BinaryData GetBinary() const { return {}; }

protected:
  ShaderStage m_stage;
};
