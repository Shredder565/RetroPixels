// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/ShaderResource.h"

#include <array>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/StringUtil.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Assets/CustomAssetCache.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PipelineUtils.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
namespace
{
// Custom samplers are bound after the eight samplers the emulated GX pipeline uses.
constexpr std::size_t FIRST_CUSTOM_SAMPLER = 8;

// ---------------------------------------------------------------------------------------------
// Post processing wrapper
//
// The utility pipeline only has one constant buffer, so the standard post processing values and
// the asset's own uniforms share a block. Each scalar is padded out to a full vec4 slot (std140).

struct PaddedScalar
{
  std::string_view type;
  std::string_view name;
};
constexpr std::array<std::string_view, 4> VEC4_UNIFORMS = {"source_resolution", "target_resolution",
                                                           "window_resolution", "source_region"};
constexpr std::array<PaddedScalar, 4> SCALAR_UNIFORMS = {{
    {"int", "source_layer"},
    {"uint", "time"},
    {"int", "graphics_api"},
    {"uint", "efb_scale"},
}};

void AppendUniformBlock(std::string& out, std::string_view block, std::string_view custom)
{
  auto sink = std::back_inserter(out);
  fmt::format_to(sink, "UBO_BINDING(std140, 1) uniform {} {{\n", block);
  for (const std::string_view name : VEC4_UNIFORMS)
    fmt::format_to(sink, "\tvec4 {};\n", name);
  for (const PaddedScalar& scalar : SCALAR_UNIFORMS)
  {
    fmt::format_to(sink, "\t{} {};\n", scalar.type, scalar.name);
    for (int pad = 1; pad <= 3; ++pad)
      fmt::format_to(sink, "\t{} {}_pad{};\n", scalar.type, scalar.name, pad);
  }
  out += custom;
  out += "};\n";
}

// A struct whose members are given one per line, followed by a blank line.
void AppendStruct(std::string& out, std::string_view name, std::string_view members)
{
  out += "struct ";
  out += name;
  out += "\n{\n";
  out += members;
  out += "};\n\n";
}

// `fallback` is the stage's default entry point, used when the asset brings no code.
void AppendCustomCode(std::string& out, std::string_view custom, std::string_view fallback)
{
  if (custom.empty())
  {
    out += fallback;
    return;
  }
  out += custom;
  out += '\n';
}

constexpr std::string_view VERTEX_MEMBERS = "\tvec4 position;\n"
                                            "\tvec3 texture_coord_0;\n";

constexpr std::string_view VS_EMULATED =
    "void dolphin_process_emulated_vertex(in DolphinVertexInput vertex_input, out "
    "DolphinVertexOutput vertex_output)\n"
    "{\n"
    "\tvertex_output.position = vertex_input.position;\n"
    "\tvertex_output.texture_coord_0 = vertex_input.texture_coord_0;\n"
    "}\n";

constexpr std::string_view VS_DEFAULT_ENTRY =
    "void process_vertex(in DolphinVertexInput vertex_input, out DolphinVertexOutput "
    "vertex_output)\n"
    "{\n"
    "\tdolphin_process_emulated_vertex(vertex_input, vertex_output);\n"
    "}\n";

// Builds a triangle covering the whole target from gl_VertexID alone. The closing brace is
// appended by the caller after the optional Vulkan flip.
constexpr std::string_view VS_MAIN =
    "void main()\n"
    "{\n"
    "\tDolphinVertexInput vertex_input;\n"
    "\tvec3 vert = vec3(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2), 0.0f);\n"
    "\tvertex_input.position = vec4(vert.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), "
    "0.0f, 1.0f);\n"
    "\tvertex_input.texture_coord_0 = vec3(source_region.xy + (source_region.zw * vert.xy), "
    "0.0f);\n"
    "\tDolphinVertexOutput vertex_output;\n"
    "\tprocess_vertex(vertex_input, vertex_output);\n"
    "\tgl_Position = vertex_output.position;\n"
    "\tv_tex0 = vertex_output.texture_coord_0;\n";

constexpr std::array<std::string_view, 5> ABSENT_ATTRIBUTES = {"COLOR_0", "COLOR_1", "NORMAL",
                                                               "BINORMAL", "TANGENT"};

std::string PostProcessVertexSource(const CustomVertexContents& custom)
{
  std::string out;
  // OpenGL requires the block to be declared identically in both stages.
  AppendUniformBlock(out, "PSBlock", custom.uniforms);

  // The same attribute defines custom draw shaders get, so one source can serve both.
  for (const std::string_view attribute : ABSENT_ATTRIBUTES)
    fmt::format_to(std::back_inserter(out), "#define HAS_{} 0\n", attribute);
  for (u32 coord = 0; coord < 8; ++coord)
  {
    fmt::format_to(std::back_inserter(out), "#define HAS_TEXTURE_COORD_{} {}\n", coord,
                   coord == 0 ? 1 : 0);
  }

  AppendStruct(out, "DolphinVertexInput", VERTEX_MEMBERS);
  AppendStruct(out, "DolphinVertexOutput", VERTEX_MEMBERS);
  out += VS_EMULATED;
  AppendCustomCode(out, custom.shader, VS_DEFAULT_ENTRY);

  if (g_backend_info.bSupportsGeometryShaders)
    out += "VARYING_LOCATION(0) out VertexData {\n\tvec3 v_tex0;\n};\n";
  else
    out += "VARYING_LOCATION(0) out vec3 v_tex0;\n";

  out += VS_MAIN;
  // Vulkan's clip space has Y pointing down.
  if (g_backend_info.api_type == APIType::Vulkan)
    out += "\tgl_Position.y = -gl_Position.y;\n";
  out += "}\n";
  return out;
}

constexpr std::string_view PS_EMULATED =
    "void dolphin_process_emulated_fragment(in DolphinFragmentInput frag_input, out "
    "DolphinFragmentOutput frag_output)\n"
    "{\n"
    "\tfrag_output.main = texture(samp0, frag_input.tex0);\n"
    "}\n";

constexpr std::string_view PS_DEFAULT_ENTRY =
    "void process_fragment(in DolphinFragmentInput frag_input, out DolphinFragmentOutput "
    "frag_output)\n"
    "{\n"
    "\tdolphin_process_emulated_fragment(frag_input, frag_output);\n"
    "}\n";

constexpr std::string_view PS_MAIN = "void main()\n"
                                     "{\n"
                                     "\tDolphinFragmentInput frag_input;\n"
                                     "\tfrag_input.tex0 = v_tex0;\n"
                                     "\tDolphinFragmentOutput frag_output;\n"
                                     "\tprocess_fragment(frag_input, frag_output);\n"
                                     "\tocol0 = frag_output.main;\n"
                                     "}\n";

std::string PostProcessPixelSource(const CustomPixelContents& custom)
{
  std::string out;
  AppendUniformBlock(out, "PSBlock", custom.uniforms);
  out += "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n";

  if (g_backend_info.bSupportsGeometryShaders)
    out += "VARYING_LOCATION(0) in VertexData {\n\tvec3 v_tex0;\n};\n";
  else
    out += "VARYING_LOCATION(0) in float3 v_tex0;\n";

  out += "FRAGMENT_OUTPUT_LOCATION(0) out vec4 ocol0;\n";
  // The input struct has a trailing blank line inside it.
  AppendStruct(out, "DolphinFragmentInput", "\tvec3 tex0;\n\n");
  AppendStruct(out, "DolphinFragmentOutput", "\tvec4 main;\n");
  out += PS_EMULATED;
  AppendCustomCode(out, custom.shader, PS_DEFAULT_ENTRY);
  out += PS_MAIN;
  return out;
}

// ---------------------------------------------------------------------------------------------
// Asset code, shared by both stages

std::string_view SamplerTypeName(AbstractTextureType type)
{
  switch (type)
  {
  case AbstractTextureType::Texture_2D:
    return "sampler2D";
  case AbstractTextureType::Texture_2DArray:
    return "sampler2DArray";
  case AbstractTextureType::Texture_CubeMap:
    return "samplerCube";
  }
  return {};
}

// The asset's code for one stage, preceded by the material's defines and the sampler
// declarations. HAS_<name> is defined for each sampler the material binds a texture to.
std::string CustomStageBody(std::string_view defines, const RasterSurfaceShaderData& asset,
                            const std::string& stage_source)
{
  std::string out(defines);
  auto sink = std::back_inserter(out);
  for (std::size_t index = 0; index < asset.samplers.size(); ++index)
  {
    const auto& sampler = asset.samplers[index];
    fmt::format_to(sink, "SAMPLER_BINDING({}) uniform {} samp_{};\n", FIRST_CUSTOM_SAMPLER + index,
                   SamplerTypeName(sampler.type), sampler.name);
    fmt::format_to(sink, "#if HAS_SAMPLER_{} == 1\n#define HAS_{} 1\n#endif\n\n", index,
                   sampler.name);
  }
  out += '\n';
  out += ReplaceAll(stage_source, "\r\n", "\n");
  return out;
}

// Declarations for the asset's uniform properties.
std::string CustomUniforms(const RasterSurfaceShaderData& asset)
{
  ShaderCode code;
  for (const ShaderProperty& property : asset.uniform_properties)
    ShaderProperty::WriteAsShaderCode(code, property);
  std::string out = code.GetBuffer();
  if (!asset.uniform_properties.empty())
    out += "\n\n";
  return out;
}

// ---------------------------------------------------------------------------------------------
// Stage creation. A null uid selects the post processing wrapper.

std::unique_ptr<AbstractShader> BuildGeometryShader(GeometryShaderUid* uid, APIType api,
                                                    const ShaderHostConfig& host)
{
  if (uid == nullptr)
  {
    std::string passthrough = FramebufferShaderGen::GeneratePassthroughGeometryShader(1, 0);
    return g_gfx->CreateShaderFromSource(ShaderStage::Geometry, std::move(passthrough), nullptr,
                                         "Custom Post Processing Geometry Shader");
  }
  const ShaderCode code = GenerateGeometryShaderCode(api, host, uid->GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Geometry, code.GetBuffer(), nullptr,
                                       fmt::format("Geometry shader: {}", *uid->GetUidData()));
}

std::unique_ptr<AbstractShader> BuildPixelShader(PixelShaderUid* uid, std::string_view defines,
                                                 APIType api, const ShaderHostConfig& host,
                                                 const RasterSurfaceShaderData& asset)
{
  const std::string body = CustomStageBody(defines, asset, asset.pixel_source);
  const std::string uniforms = CustomUniforms(asset);
  const CustomPixelContents contents{.shader = body, .uniforms = uniforms};

  std::string source;
  if (uid != nullptr)
    source = GeneratePixelShaderCode(api, host, uid->GetUidData(), contents).GetBuffer();
  else
    source = PostProcessPixelSource(contents);
  return g_gfx->CreateShaderFromSource(ShaderStage::Pixel, std::move(source),
                                       asset.shader_includer.get(), "Custom Pixel Shader");
}

std::unique_ptr<AbstractShader> BuildVertexShader(VertexShaderUid* uid, std::string_view defines,
                                                  APIType api, const ShaderHostConfig& host,
                                                  const RasterSurfaceShaderData& asset)
{
  const std::string body = CustomStageBody(defines, asset, asset.vertex_source);
  const std::string uniforms = CustomUniforms(asset);
  const CustomVertexContents contents{.shader = body, .uniforms = uniforms};

  std::string source;
  if (uid != nullptr)
    source = GenerateVertexShaderCode(api, host, uid->GetUidData(), contents).GetBuffer();
  else
    source = PostProcessVertexSource(contents);
  return g_gfx->CreateShaderFromSource(ShaderStage::Vertex, std::move(source), nullptr,
                                       "Custom Vertex Shader");
}

// A shader compiler job that runs a callable on the worker thread; nothing to retrieve.
template <typename Fn>
class CallbackJob final : public AsyncShaderCompiler::WorkItem
{
public:
  explicit CallbackJob(Fn fn) : m_fn(std::move(fn)) {}
  bool Compile() override { return m_fn(); }
  void Retrieve() override {}

private:
  Fn m_fn;
};
}  // namespace

ShaderResource::ShaderResource(Resource::ResourceContext resource_context,
                               std::optional<GXPipelineUid> pipeline_uid,
                               std::string preprocessor_setting,
                               const ShaderHostConfig& shader_host_config)
    : Resource(std::move(resource_context)), m_host_config{.bits = shader_host_config.bits},
      m_uid(std::move(pipeline_uid)), m_defines(std::move(preprocessor_setting))
{
  const ResourceContext& ctx = m_resource_context;
  m_asset = ctx.asset_cache->CreateAsset<RasterSurfaceShaderAsset>(ctx.primary_asset_id,
                                                                   ctx.asset_library, this);
}

void ShaderResource::SetHostConfig(const ShaderHostConfig& host_config)
{
  m_host_config.bits = host_config.bits;
}

void ShaderResource::MarkAsActive()
{
  m_resource_context.asset_cache->MarkAssetActive(m_asset);
}

void ShaderResource::MarkAsPending()
{
  m_resource_context.asset_cache->MarkAssetPending(m_asset);
}

AbstractShader* ShaderResource::Data::GetVertexShader() const
{
  return m_vs.get();
}

AbstractShader* ShaderResource::Data::GetPixelShader() const
{
  return m_ps.get();
}

AbstractShader* ShaderResource::Data::GetGeometryShader() const
{
  return m_gs.get();
}

bool ShaderResource::Data::IsCompiled() const
{
  const bool gs_ok = m_gs != nullptr || !m_gs_required;
  return m_vs != nullptr && m_ps != nullptr && gs_ok;
}

AbstractTextureType ShaderResource::Data::GetTextureType(std::size_t index)
{
  if (m_source == nullptr || index >= m_source->samplers.size()) [[unlikely]]
    return AbstractTextureType::Texture_2D;
  return m_source->samplers[index].type;
}

void ShaderResource::ResetData()
{
  m_staging = std::make_shared<Data>();
  m_compile_queued = false;
}

Resource::TaskComplete ShaderResource::CollectPrimaryData()
{
  std::shared_ptr<RasterSurfaceShaderData> source = m_asset->GetData();
  if (source == nullptr) [[unlikely]]
    return TaskComplete::No;
  m_staging->m_source = std::move(source);
  return TaskComplete::Yes;
}

void ShaderResource::CompileStages(Data& data, GXPipelineUid* uid, const ShaderHostConfig& host,
                                   std::string_view defines)
{
  const APIType api = g_backend_info.api_type;
  const RasterSurfaceShaderData& asset = *data.m_source;

  if (uid == nullptr)
  {
    // A full screen pass only needs a geometry stage to expand stereo layers.
    data.m_gs_required = host.backend_geometry_shaders && host.stereo;
    if (data.m_gs_required)
      data.m_gs = BuildGeometryShader(nullptr, api, host);
    data.m_ps = BuildPixelShader(nullptr, defines, api, host, asset);
    data.m_vs = BuildVertexShader(nullptr, defines, api, host, asset);
    return;
  }

  GXPipelineUid patched = ApplyDriverBugs(*uid);
  ClearUnusedPixelShaderUidBits(api, host, &patched.ps_uid);
  data.m_gs_required =
      host.backend_geometry_shaders && !patched.gs_uid.GetUidData()->IsPassthrough();
  if (data.m_gs_required)
    data.m_gs = BuildGeometryShader(&patched.gs_uid, api, host);
  data.m_ps = BuildPixelShader(&patched.ps_uid, defines, api, host, asset);
  data.m_vs = BuildVertexShader(&patched.vs_uid, defines, api, host, asset);
}

Resource::TaskComplete ShaderResource::ProcessData()
{
  if (!m_compile_queued)
  {
    // The compile reads m_defines through a view, and the uid in place; both live as long as
    // this resource.
    auto compile = [data = m_staging, uid = m_uid ? &*m_uid : nullptr, bits = m_host_config.bits,
                    defines = std::string_view(m_defines)] {
      CompileStages(*data, uid, ShaderHostConfig{.bits = bits}, defines);
      data->m_compile_done = true;
      return true;
    };
    AsyncShaderCompiler& compiler = *m_resource_context.shader_compiler;
    // Ordering between resources is already decided by the resource system.
    compiler.QueueWorkItem(compiler.CreateWorkItem<CallbackJob<decltype(compile)>>(compile), 0);
    m_compile_queued = true;
  }

  if (!m_staging->m_compile_done)
    return TaskComplete::No;
  if (!m_staging->IsCompiled())
    return TaskComplete::Error;

  m_live.swap(m_staging);
  return TaskComplete::Yes;
}
}  // namespace VideoCommon
