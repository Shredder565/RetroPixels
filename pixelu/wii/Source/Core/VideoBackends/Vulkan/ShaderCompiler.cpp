// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/ShaderCompiler.h"

#include <initializer_list>
#include <span>
#include <string>

#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/ShaderCompileUtils.h"
#include "VideoCommon/Spirv.h"

namespace Vulkan::ShaderCompiler
{
namespace
{
// Lays the lines out the way the preludes have always been sent to glslang: a leading newline,
// then every non-empty line indented by two spaces. The SPIR-V (and so the driver's pipeline
// cache) depends on these exact bytes.
void AppendBlock(std::string* out, std::span<const std::string_view> lines)
{
  for (std::string_view line : lines)
  {
    if (!line.empty())
      out->append("  ").append(line);
    out->push_back('\n');
  }
}

// HLSL spellings used by the shared shader generators.
constexpr std::string_view HLSL_ALIASES[] = {
    "// hlsl to glsl function translation",
    "#define API_VULKAN 1",
    "#define float2 vec2",
    "#define float3 vec3",
    "#define float4 vec4",
    "#define uint2 uvec2",
    "#define uint3 uvec3",
    "#define uint4 uvec4",
    "#define int2 ivec2",
    "#define int3 ivec3",
    "#define int4 ivec4",
    "#define frac fract",
    "#define lerp mix",
};

// Graphics stages. UBO bindings are shifted down by one because the generators number them
// from 1 (binding 0 is reserved for plain uniforms in the OpenGL backend).
std::string BuildGraphicsPrelude()
{
  std::string text = "\n";
  AppendBlock(&text, std::initializer_list<std::string_view>{
                         "// Target GLSL 4.5.",
                         "#version 450 core",
                         "",
                         "#extension GL_ARB_shading_language_include : enable",
                         "",
                         "#define ATTRIBUTE_LOCATION(x) layout(location = x)",
                         "#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)",
                         "#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, "
                         "index = y)",
                         "#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - "
                         "1))",
                         "#define SAMPLER_BINDING(x) layout(set = 1, binding = x)",
                         "#define TEXEL_BUFFER_BINDING(x) layout(set = 1, binding = (x + 8))",
                         "#define SSBO_BINDING(x) layout(std430, set = 2, binding = x)",
                         "#define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, "
                         "input_attachment_index = z)",
                         "#define VARYING_LOCATION(x) layout(location = x)",
                         "#define FORCE_EARLY_Z layout(early_fragment_tests) in",
                         "",
                         "// Metal framebuffer fetch helpers.",
                         "#define FB_FETCH_VALUE subpassLoad(in_ocol0)",
                         "",
                     });
  AppendBlock(&text, HLSL_ALIASES);
  AppendBlock(&text, std::initializer_list<std::string_view>{
                         "",
                         "// These were changed in Vulkan",
                         "#define gl_VertexID gl_VertexIndex",
                         "#define gl_InstanceID gl_InstanceIndex",
                     });
  return text;
}

// Compute: everything lives in descriptor set 0 (see the compute set layout).
std::string BuildComputePrelude()
{
  std::string text = "\n";
  AppendBlock(&text, std::initializer_list<std::string_view>{
                         "// Target GLSL 4.5.",
                         "#version 450 core",
                         "// All resources are packed into one descriptor set for compute.",
                         "#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - "
                         "1))",
                         "#define SAMPLER_BINDING(x) layout(set = 0, binding = (1 + x))",
                         "#define TEXEL_BUFFER_BINDING(x) layout(set = 0, binding = (9 + x))",
                         "#define IMAGE_BINDING(format, x) layout(format, set = 0, binding = (11 "
                         "+ x))",
                         "",
                     });
  AppendBlock(&text, HLSL_ALIASES);
  return text;
}

std::string BuildSubgroupPrelude()
{
  std::string text = "\n";
  AppendBlock(&text, std::initializer_list<std::string_view>{
                         "#extension GL_KHR_shader_subgroup_basic : enable",
                         "#extension GL_KHR_shader_subgroup_arithmetic : enable",
                         "#extension GL_KHR_shader_subgroup_ballot : enable",
                         "#extension GL_KHR_shader_subgroup_shuffle : enable",
                         "",
                         "#define SUPPORTS_SUBGROUP_REDUCTION 1",
                         "#define IS_HELPER_INVOCATION gl_HelperInvocation",
                         "#define IS_FIRST_ACTIVE_INVOCATION (subgroupElect())",
                         "#define SUBGROUP_MIN(value) value = subgroupMin(value)",
                         "#define SUBGROUP_MAX(value) value = subgroupMax(value)",
                     });
  return text;
}

const std::string& GraphicsPrelude()
{
  static const std::string text = BuildGraphicsPrelude();
  return text;
}

const std::string& ComputePrelude()
{
  static const std::string text = BuildComputePrelude();
  return text;
}

const std::string& SubgroupPrelude()
{
  static const std::string text = BuildSubgroupPrelude();
  return text;
}

std::string WithPrelude(std::string_view prelude, std::string_view body)
{
  const bool subgroups = g_vulkan_context->SupportsShaderSubgroupOperations();
  const std::string& subgroup_text = SubgroupPrelude();

  std::string code;
  code.reserve(prelude.size() + subgroup_text.size() + body.size());
  code.append(prelude);
  if (subgroups)
    code.append(subgroup_text);
  if (DriverDetails::HasBug(DriverDetails::BUG_INVERTED_IS_HELPER))
  {
    code.append(
        "#define gl_HelperInvocation !gl_HelperInvocation // Work around broken AMD Metal driver\n");
  }
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD))
    code.append("#define BROKEN_SUBGROUP_WITH_DISCARD 1\n");
  code.append(body);
  return code;
}

// Subgroup operations need SPIR-V 1.3 (Vulkan 1.1).
glslang::EShTargetLanguageVersion TargetSpirv()
{
  return g_vulkan_context->SupportsShaderSubgroupOperations() ? glslang::EShTargetSpv_1_3 :
                                                                glslang::EShTargetSpv_1_0;
}
}  // namespace

std::optional<SPIRVCodeVector> CompileVertexShader(std::string_view source_code,
                                                   VideoCommon::ShaderIncluder* shader_includer)
{
  return SPIRV::CompileVertexShader(WithPrelude(GraphicsPrelude(), source_code), APIType::Vulkan,
                                    TargetSpirv(), shader_includer);
}

std::optional<SPIRVCodeVector> CompileGeometryShader(std::string_view source_code,
                                                     VideoCommon::ShaderIncluder* shader_includer)
{
  return SPIRV::CompileGeometryShader(WithPrelude(GraphicsPrelude(), source_code),
                                      APIType::Vulkan, TargetSpirv(), shader_includer);
}

std::optional<SPIRVCodeVector> CompileFragmentShader(std::string_view source_code,
                                                     VideoCommon::ShaderIncluder* shader_includer)
{
  return SPIRV::CompileFragmentShader(WithPrelude(GraphicsPrelude(), source_code),
                                      APIType::Vulkan, TargetSpirv(), shader_includer);
}

std::optional<SPIRVCodeVector> CompileComputeShader(std::string_view source_code,
                                                    VideoCommon::ShaderIncluder* shader_includer)
{
  return SPIRV::CompileComputeShader(WithPrelude(ComputePrelude(), source_code), APIType::Vulkan,
                                     TargetSpirv(), shader_includer);
}
}  // namespace Vulkan::ShaderCompiler
