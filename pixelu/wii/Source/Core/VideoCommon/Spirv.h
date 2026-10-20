// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include <glslang/Public/ShaderLang.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/VideoCommon.h"

// GLSL to SPIR-V compilation through glslang, shared by the Vulkan, Metal and D3D backends.
// Each function returns std::nullopt (after reporting the error and dumping the shader) when the
// source does not compile. shader_includer may be null, in which case #include is rejected.
namespace SPIRV
{
using CodeType = u32;
using CodeVector = std::vector<CodeType>;

std::optional<CodeVector> CompileVertexShader(std::string_view source_code, APIType api_type,
                                              glslang::EShTargetLanguageVersion language_version,
                                              glslang::TShader::Includer* shader_includer);

std::optional<CodeVector> CompileGeometryShader(std::string_view source_code, APIType api_type,
                                                glslang::EShTargetLanguageVersion language_version,
                                                glslang::TShader::Includer* shader_includer);

std::optional<CodeVector> CompileFragmentShader(std::string_view source_code, APIType api_type,
                                                glslang::EShTargetLanguageVersion language_version,
                                                glslang::TShader::Includer* shader_includer);

std::optional<CodeVector> CompileComputeShader(std::string_view source_code, APIType api_type,
                                               glslang::EShTargetLanguageVersion language_version,
                                               glslang::TShader::Includer* shader_includer);
}  // namespace SPIRV
