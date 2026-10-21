// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3DCommon/Shader.h"

#include <array>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <glslang/SPIRV/disassemble.h>
#include <wrl/client.h>
#include "spirv_hlsl.hpp"

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"

#include "VideoCommon/ShaderCompileUtils.h"
#include "VideoCommon/Spirv.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// ---------------------------------------------------------------------------
// GLSL preambles
//
// The renderer's shaders are written against a small set of binding macros.
// For D3D the uniform-buffer bindings are shifted down by one: the GL backend
// keeps binding 0 for loose uniforms, D3D does not, so the same shader text
// ends up on register b0 here. Storage buffers sit two slots up.
// ---------------------------------------------------------------------------

constexpr std::array<std::string_view, 9> GRAPHICS_BINDING_MACROS = {
    "#define ATTRIBUTE_LOCATION(x) layout(location = x)",
    "#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)",
    "#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)",
    "#define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))",
    "#define SAMPLER_BINDING(x) layout(binding = x)",
    "#define TEXEL_BUFFER_BINDING(x) layout(binding = x)",
    "#define SSBO_BINDING(x) layout(binding = (x + 2))",
    "#define VARYING_LOCATION(x) layout(location = x)",
    "#define FORCE_EARLY_Z layout(early_fragment_tests) in",
};

constexpr std::array<std::string_view, 4> COMPUTE_BINDING_MACROS = {
    "#define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))",
    "#define SAMPLER_BINDING(x) layout(binding = x)",
    "#define TEXEL_BUFFER_BINDING(x) layout(binding = x)",
    "#define IMAGE_BINDING(format, x) layout(format, binding = x)",
};

// HLSL spellings the shared shader code uses, mapped onto GLSL.
constexpr std::array<std::pair<std::string_view, std::string_view>, 11> HLSL_NAME_ALIASES = {{
    {"float2", "vec2"},
    {"float3", "vec3"},
    {"float4", "vec4"},
    {"uint2", "uvec2"},
    {"uint3", "uvec3"},
    {"uint4", "uvec4"},
    {"int2", "ivec2"},
    {"int3", "ivec3"},
    {"int4", "ivec4"},
    {"frac", "fract"},
    {"lerp", "mix"},
}};

// Builds a preamble: starts with a newline, and every non-empty line is
// indented by two spaces.
class PreambleWriter
{
public:
  PreambleWriter() : m_text("\n") {}

  void Line(std::string_view line)
  {
    if (!line.empty())
      m_text.append("  ").append(line);
    m_text.push_back('\n');
  }
  void Blank() { m_text.push_back('\n'); }

  template <size_t N>
  void Lines(const std::array<std::string_view, N>& lines)
  {
    for (std::string_view line : lines)
      Line(line);
  }

  void AliasBlock()
  {
    Line("// hlsl to glsl function translation");
    for (const auto& [hlsl, glsl] : HLSL_NAME_ALIASES)
      Line(fmt::format("#define {} {}", hlsl, glsl));
  }

  std::string Take() { return std::move(m_text); }

private:
  std::string m_text;
};

const std::string& GraphicsPreamble()
{
  static const std::string text = [] {
    PreambleWriter out;
    out.Line("// Target GLSL 4.5.");
    out.Line("#version 450 core");
    out.Blank();
    out.Line("#extension GL_ARB_shading_language_include : enable");
    out.Blank();
    out.Lines(GRAPHICS_BINDING_MACROS);
    out.Blank();
    out.AliasBlock();
    out.Blank();
    out.Line("#define API_D3D 1");
    return out.Take();
  }();
  return text;
}

const std::string& ComputePreamble()
{
  static const std::string text = [] {
    PreambleWriter out;
    out.Line("// Target GLSL 4.5.");
    out.Line("#version 450 core");
    out.Line("// All resources are packed into one descriptor set for compute.");
    out.Lines(COMPUTE_BINDING_MACROS);
    out.Blank();
    out.AliasBlock();
    out.Blank();
    out.Line("#define API_D3D 1");
    return out.Take();
  }();
  return text;
}

// Shader-model generation for a feature level: 4.0, 4.1, or 5.0 for 11_0 and up.
enum class ModelGeneration
{
  SM40,
  SM41,
  SM50,
};

ModelGeneration GenerationFor(D3D_FEATURE_LEVEL level)
{
  if (level == D3D_FEATURE_LEVEL_10_0)
    return ModelGeneration::SM40;
  if (level == D3D_FEATURE_LEVEL_10_1)
    return ModelGeneration::SM41;
  return ModelGeneration::SM50;
}

// fxc profile name, or "" when the stage cannot be compiled at that level
// (compute needs shader model 5).
const char* ProfileFor(D3D_FEATURE_LEVEL level, ShaderStage stage)
{
  const auto gen = static_cast<size_t>(GenerationFor(level));
  switch (stage)
  {
  case ShaderStage::Vertex:
    return std::array{"vs_4_0", "vs_4_1", "vs_5_0"}[gen];
  case ShaderStage::Geometry:
    return std::array{"gs_4_0", "gs_4_1", "gs_5_0"}[gen];
  case ShaderStage::Pixel:
    return std::array{"ps_4_0", "ps_4_1", "ps_5_0"}[gen];
  case ShaderStage::Compute:
    return std::array{"", "", "cs_5_0"}[gen];
  default:
    return "";
  }
}

// GLSL to SPIR-V for the stages that go through glslang. Geometry shaders
// arrive as HLSL already, so there is nothing to produce for them.
std::optional<SPIRV::CodeVector> ToSpirv(ShaderStage stage, std::string_view source,
                                         glslang::TShader::Includer* includer)
{
  constexpr auto spv_version = glslang::EShTargetSpv_1_0;
  switch (stage)
  {
  case ShaderStage::Vertex:
    return SPIRV::CompileVertexShader(fmt::format("{}{}", GraphicsPreamble(), source),
                                      APIType::D3D, spv_version, includer);
  case ShaderStage::Pixel:
    return SPIRV::CompileFragmentShader(fmt::format("{}{}", GraphicsPreamble(), source),
                                        APIType::D3D, spv_version, includer);
  case ShaderStage::Compute:
    return SPIRV::CompileComputeShader(fmt::format("{}{}", ComputePreamble(), source),
                                       APIType::D3D, spv_version, includer);
  case ShaderStage::Geometry:
  default:
    return std::nullopt;
  }
}

std::string SpirvToHlsl(SPIRV::CodeVector spirv, D3D_FEATURE_LEVEL level)
{
  static constexpr std::array<uint32_t, 3> MODEL_NUMBERS = {40, 41, 50};

  spirv_cross::CompilerHLSL::Options options;
  options.shader_model = MODEL_NUMBERS[static_cast<size_t>(GenerationFor(level))];

  spirv_cross::CompilerHLSL cross(std::move(spirv));
  cross.set_hlsl_options(options);
  return cross.compile();
}

std::optional<std::string> SourceToHlsl(D3D_FEATURE_LEVEL level, ShaderStage stage,
                                        std::string_view source,
                                        VideoCommon::ShaderIncluder* includer)
{
  if (stage == ShaderStage::Geometry)
    return std::string(source);

  auto spirv = ToSpirv(stage, source, includer);
  if (!spirv)
    return std::nullopt;
  return SpirvToHlsl(std::move(*spirv), level);
}

std::string_view BlobText(ID3DBlob* blob)
{
  return static_cast<const char*>(blob->GetBufferPointer());
}

// Writes the bad-shader report for a failed fxc run and returns its path.
std::string WriteFailureReport(const char* profile, const std::string& hlsl, ID3DBlob* errors,
                               ShaderStage stage, std::string_view source,
                               VideoCommon::ShaderIncluder* includer)
{
  static int s_failure_count = 0;
  std::string path = VideoBackendBase::BadShaderFilename(profile, s_failure_count++);

  std::ofstream report;
  File::OpenFStream(report, path, std::ios_base::out);
  report.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
  report << "\n";
  report.write(static_cast<const char*>(errors->GetBufferPointer()),
               static_cast<std::streamsize>(errors->GetBufferSize()));
  report << "\n";
  report << "Dolphin Version: " + Common::GetScmRevStr() + "\n";
  report << "Video Backend: " + g_video_backend->GetDisplayName();

  // For GLSL input, also record what fxc was fed from: the source and its SPIR-V.
  if (auto spirv = ToSpirv(stage, source, includer))
  {
    report << "\nOriginal Source: \n";
    report << source << std::endl;
    report << "SPIRV: \n";
    spv::Disassemble(report, *spirv);
  }
  report.close();
  return path;
}
}  // namespace

namespace D3DCommon
{
Shader::Shader(ShaderStage stage, BinaryData bytecode)
    : AbstractShader(stage), m_bytecode(std::move(bytecode))
{
}

Shader::~Shader() = default;

AbstractShader::BinaryData Shader::GetBinary() const
{
  return m_bytecode;
}

std::optional<Shader::BinaryData>
Shader::CompileShader(D3D_FEATURE_LEVEL feature_level, ShaderStage stage, std::string_view source,
                      VideoCommon::ShaderIncluder* shader_includer)
{
  const std::optional<std::string> hlsl =
      SourceToHlsl(feature_level, stage, source, shader_includer);
  if (!hlsl)
    return std::nullopt;

  static constexpr D3D_SHADER_MACRO DEFINES[] = {{"API_D3D", "1"}, {nullptr, nullptr}};
  const UINT compile_flags = g_ActiveConfig.bEnableValidationLayer ?
                                 (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION) :
                                 (D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_SKIP_VALIDATION);
  const char* const profile = ProfileFor(feature_level, stage);

  Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
  Microsoft::WRL::ComPtr<ID3DBlob> messages;
  const HRESULT result = d3d_compile(hlsl->data(), hlsl->size(), nullptr, DEFINES, nullptr, "main",
                                     profile, compile_flags, 0, &bytecode, &messages);
  if (FAILED(result))
  {
    const std::string report =
        WriteFailureReport(profile, *hlsl, messages.Get(), stage, source, shader_includer);
    PanicAlertFmt("Failed to compile {}: {}\nDebug info ({}):\n{}", report, Common::HRWrap(result),
                  profile, BlobText(messages.Get()));
    return std::nullopt;
  }

  if (messages && messages->GetBufferSize() > 0)
  {
    WARN_LOG_FMT(VIDEO, "{} compilation succeeded with warnings:\n{}", profile,
                 BlobText(messages.Get()));
  }

  return CreateByteCode(bytecode->GetBufferPointer(), bytecode->GetBufferSize());
}

AbstractShader::BinaryData Shader::CreateByteCode(const void* data, size_t length)
{
  const u8* const first = static_cast<const u8*>(data);
  return BinaryData(first, first + length);
}

}  // namespace D3DCommon
