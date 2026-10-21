// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
class ShaderIncluder;
}

// GLSL to SPIR-V for the Vulkan backend. Each function puts the backend's GLSL prelude
// (binding macros, HLSL-style type names, subgroup helpers and driver workarounds) in front
// of the source and hands it to glslang. Returns nothing when the source does not compile.
namespace Vulkan::ShaderCompiler
{
using SPIRVCodeType = u32;
using SPIRVCodeVector = std::vector<SPIRVCodeType>;

std::optional<SPIRVCodeVector> CompileVertexShader(std::string_view source_code,
                                                   VideoCommon::ShaderIncluder* shader_includer);
std::optional<SPIRVCodeVector> CompileGeometryShader(std::string_view source_code,
                                                     VideoCommon::ShaderIncluder* shader_includer);
std::optional<SPIRVCodeVector> CompileFragmentShader(std::string_view source_code,
                                                     VideoCommon::ShaderIncluder* shader_includer);
std::optional<SPIRVCodeVector> CompileComputeShader(std::string_view source_code,
                                                    VideoCommon::ShaderIncluder* shader_includer);
}  // namespace Vulkan::ShaderCompiler
