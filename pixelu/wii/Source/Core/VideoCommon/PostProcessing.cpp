// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PostProcessing.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/ShaderCompileUtils.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
namespace
{
using Option = PostProcessingConfiguration::ConfigurationOption;
using OptionType = Option::OptionType;

// Used when no user shader is selected (or it fails): passes the picture through.
constexpr char PASSTHROUGH_SHADER[] = "void main() { SetOutput(Sample()); }\n";
// The built-in pass's shader file. It is not offered in the shader lists.
constexpr char BUILTIN_PASS_SHADER[] = "default_pre_post_process";
// The built-in pass may do gamma and gamut conversions and HDR, so its output is kept at a
// precision where storing gamma-space values loses nothing visible.
constexpr AbstractTextureFormat INTERMEDIATE_FORMAT = AbstractTextureFormat::RGBA16F;

std::string UserShaderDir(const std::string& sub_dir)
{
  return File::GetUserPath(D_SHADERS_IDX) + sub_dir;
}

std::string SystemShaderDir(const std::string& sub_dir)
{
  return File::GetSysDirectory() + SHADERS_DIR DIR_SEP + sub_dir;
}

// Reads <name>.glsl from the user shader folder, or the system one when the user has none.
bool ReadShaderFile(const std::string& name, const std::string& sub_dir, std::string& out_code)
{
  std::string path = UserShaderDir(sub_dir) + name + ".glsl";
  if (!File::Exists(path))
    path = SystemShaderDir(sub_dir) + name + ".glsl";

  if (File::ReadFileToString(path, out_code))
    return true;
  out_code = "";
  ERROR_LOG_FMT(VIDEO, "Post-processing shader not found: {}", path);
  return false;
}

// Shader names (file names without .glsl) found in either shader folder.
std::vector<std::string> ListShaders(const std::string& sub_dir = "")
{
  const std::string user_dir = UserShaderDir(sub_dir);
  const std::string system_dir = SystemShaderDir(sub_dir);
  const std::string_view dirs[] = {user_dir, system_dir};

  std::vector<std::string> names;
  for (const std::string& path : Common::DoFileSearch(dirs, ".glsl"))
  {
    std::string name;
    SplitPath(path, nullptr, &name, nullptr);
    if (name != BUILTIN_PASS_SHADER)
      names.push_back(std::move(name));
  }
  return names;
}

// Option section headers in a shader's [configuration] block. Anything else counts as a bool.
OptionType OptionTypeFromSection(std::string_view section)
{
  if (section == "OptionRangeFloat")
    return OptionType::Float;
  if (section == "OptionRangeInteger")
    return OptionType::Integer;
  return OptionType::Bool;
}

// The value lists a numeric key writes to.
struct ValueTargets
{
  std::vector<s32>* integers;
  std::vector<float>* floats;
};

std::optional<ValueTargets> TargetsForKey(Option& option, std::string_view key)
{
  if (key == "MinValue")
    return ValueTargets{&option.m_integer_min_values, &option.m_float_min_values};
  if (key == "MaxValue")
    return ValueTargets{&option.m_integer_max_values, &option.m_float_max_values};
  if (key == "DefaultValue")
    return ValueTargets{&option.m_integer_values, &option.m_float_values};
  if (key == "StepAmount")
    return ValueTargets{&option.m_integer_step_values, &option.m_float_step_values};
  return std::nullopt;
}

// Parses a comma separated list, keeping at most four components.
template <typename T>
void ParseComponents(const std::string& text, std::vector<T>* out)
{
  TryParseVector(text, out);
  if (out->size() > 4)
    out->resize(4);
}

// Replaces values with the list in text, unless it does not parse.
template <typename T>
void OverrideComponents(const std::string& text, std::vector<T>* values)
{
  std::vector<T> parsed;
  if (TryParseVector(text, &parsed))
    *values = std::move(parsed);
}

std::string JoinIntegers(const std::vector<s32>& values)
{
  std::string text;
  for (size_t i = 0; i < values.size(); ++i)
  {
    if (i != 0)
      text += ", ";
    text += fmt::format("{}", values[i]);
  }
  return text;
}

// Floats are written with iostream's default formatting in the C locale.
std::string JoinFloats(const std::vector<float>& values)
{
  std::ostringstream text;
  text.imbue(std::locale("C"));
  for (size_t i = 0; i < values.size(); ++i)
  {
    if (i != 0)
      text << ", ";
    text << values[i];
  }
  return text.str();
}

// Whether a pass needs the texcoord geometry shader to read and write both stereo layers in
// one draw. Other stereo layouts draw each eye separately.
bool PassUsesLayeredStereo(bool is_intermediary_buffer)
{
  switch (g_ActiveConfig.stereo_mode)
  {
  case StereoMode::QuadBuffer:
    return !g_backend_info.bUsesExplictQuadBuffering;
  case StereoMode::Anaglyph:
  case StereoMode::Passive:
    return is_intermediary_buffer;
  default:
    return false;
  }
}

// The built-in uniforms, in block order. Shaders see these names.
constexpr std::array<std::string_view, 20> BUILTIN_UNIFORMS = {
    "float4 resolution",        "float4 target_resolution",  "float4 window_resolution",
    "int2 stereo_views",        "float4 src_rect",           "int src_layer",
    "uint time",                "int graphics_api",          "int intermediary_buffer",
    "int resampling_method",    "int correct_color_space",   "int game_color_space",
    "int correct_gamma",        "float game_gamma",          "int sdr_display_gamma_sRGB",
    "float sdr_display_custom_gamma", "int linear_space_output", "int hdr_output",
    "float hdr_paper_white_nits",     "float hdr_sdr_white_nits",
};

// CPU image of the built-in uniforms (std140; bools as s32). stereo_views is declared int2 in
// the shader but takes a full vec4 slot here, which is where std140 puts src_rect anyway.
struct BuiltinBlock
{
  std::array<float, 4> source_resolution;
  std::array<float, 4> target_resolution;
  std::array<float, 4> window_resolution;
  std::array<float, 4> stereo_views;
  std::array<float, 4> src_rect;
  s32 src_layer;
  u32 time;
  s32 graphics_api;
  s32 intermediary_buffer;
  s32 resampling_method;
  s32 correct_color_space;
  s32 game_color_space;
  s32 correct_gamma;
  float game_gamma;
  s32 sdr_display_gamma_sRGB;
  float sdr_display_custom_gamma;
  s32 linear_space_output;
  s32 hdr_output;
  float hdr_paper_white_nits;
  float hdr_sdr_white_nits;
};

// Accessors offered to user shaders: return type, name, expression.
struct ShaderGetter
{
  std::string_view type;
  std::string_view name;
  std::string_view expression;
};
constexpr std::array<ShaderGetter, 9> SHADER_GETTERS = {{
    {"float2", "GetTargetResolution", "target_resolution.xy"},
    {"float2", "GetInvTargetResolution", "target_resolution.zw"},
    {"float2", "GetWindowResolution", "window_resolution.xy"},
    {"float2", "GetInvWindowResolution", "window_resolution.zw"},
    {"float2", "GetResolution", "resolution.xy"},
    {"float2", "GetInvResolution", "resolution.zw"},
    {"float2", "GetCoordinates", "v_tex0.xy"},
    {"float", "GetLayer", "v_tex0.z"},
    {"uint", "GetTime", "time"},
}};

// The shared fullscreen-triangle vertex shader, after the uniform block.
std::string VertexShaderBody()
{
  std::string body = g_backend_info.bSupportsGeometryShaders ?
                         "VARYING_LOCATION(0) out VertexData {\n  float3 v_tex0;\n};\n" :
                         "VARYING_LOCATION(0) out float3 v_tex0;\n";
  body += "#define id gl_VertexID\n";
  body += "#define opos gl_Position\n";
  body += "void main() {\n";
  // A triangle covering the target, with texcoords mapped into src_rect.
  body += "  v_tex0 = float3(float((id << 1) & 2), float(id & 2), 0.0f);\n";
  body += "  opos = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n";
  body += "  v_tex0 = float3(src_rect.xy + (src_rect.zw * v_tex0.xy), float(src_layer));\n";
  // Vulkan flips every pass; OpenGL flips all but the final one.
  if (g_backend_info.api_type == APIType::Vulkan)
    body += "  opos.y = -opos.y;\n";
  else if (g_backend_info.api_type == APIType::OpenGL)
    body += "  if (intermediary_buffer != 0)\n    opos.y = -opos.y;\n";
  body += "}\n";
  return body;
}
}  // namespace

PostProcessingConfiguration::PostProcessingConfiguration() = default;

PostProcessingConfiguration::~PostProcessingConfiguration() = default;

void PostProcessingConfiguration::LoadShader(const std::string& shader)
{
  m_current_shader = shader;
  if (shader.empty())
  {
    LoadDefaultShader();
    return;
  }

  // Stereo modes that combine the eyes in the shader have their own shader folders.
  std::string sub_dir;
  if (g_Config.stereo_mode == StereoMode::Anaglyph)
    sub_dir = ANAGLYPH_DIR DIR_SEP;
  else if (g_Config.stereo_mode == StereoMode::Passive)
    sub_dir = PASSIVE_DIR DIR_SEP;

  std::string code;
  if (!ReadShaderFile(shader, sub_dir, code))
  {
    LoadDefaultShader();
    return;
  }

  // Declared defaults first, then the user's saved values over them.
  LoadOptions(code);
  LoadOptionsConfiguration();
  m_current_shader_code = code;
  m_shader_includer =
      std::make_unique<ShaderIncluder>(UserShaderDir(sub_dir), SystemShaderDir(sub_dir));
}

void PostProcessingConfiguration::LoadDefaultShader()
{
  m_options.clear();
  m_any_options_dirty = false;
  m_current_shader = "";
  m_current_shader_code = PASSTHROUGH_SHADER;
}

// The [configuration] ... [/configuration] block holds ini-style sections, one per option:
//   [OptionRangeFloat]
//   GUIName = ...
//   OptionName = ...
//   MinValue = 0.0, 0.0
//   ...
void PostProcessingConfiguration::LoadOptions(const std::string& code)
{
  constexpr std::string_view BLOCK_OPEN = "[configuration]";
  constexpr std::string_view BLOCK_CLOSE = "[/configuration]";
  const size_t open_at = code.find(BLOCK_OPEN);
  const size_t close_at = code.find(BLOCK_CLOSE);

  m_options.clear();
  m_any_options_dirty = true;
  if (open_at == std::string::npos || close_at == std::string::npos)
    return;

  struct Section
  {
    std::string type;
    std::vector<std::pair<std::string, std::string>> entries;
  };
  std::vector<Section> sections;

  std::istringstream block(
      code.substr(open_at + BLOCK_OPEN.size(), close_at - open_at - BLOCK_OPEN.size()));
  std::string raw_line;
  while (std::getline(block, raw_line))
  {
    std::string_view line = raw_line;
#ifndef _WIN32
    // Files with CRLF line ends.
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
#endif
    if (line.empty())
      continue;

    if (line.front() == '[')
    {
      // A header without its closing bracket is ignored (entries keep going to the previous
      // section).
      const size_t close = line.find(']');
      if (close != std::string_view::npos)
        sections.push_back({std::string(line.substr(1, close - 1)), {}});
      continue;
    }

    // Entries before the first header belong to nothing.
    if (sections.empty())
      continue;
    std::string key;
    std::string value;
    Common::IniFile::ParseLine(line, &key, &value);
    if (!key.empty() || !value.empty())
      sections.back().entries.emplace_back(std::move(key), std::move(value));
  }

  for (const Section& section : sections)
  {
    Option option;
    option.m_dirty = true;
    option.m_type = OptionTypeFromSection(section.type);

    for (const auto& [key, value] : section.entries)
    {
      if (key == "GUIName")
      {
        option.m_gui_name = value;
      }
      else if (key == "OptionName")
      {
        option.m_option_name = value;
      }
      else if (key == "DependentOption")
      {
        option.m_dependent_option = value;
      }
      else if (const std::optional<ValueTargets> targets = TargetsForKey(option, key))
      {
        // A bool takes any of the numeric keys as its value.
        switch (option.m_type)
        {
        case OptionType::Bool:
          TryParse(value, &option.m_bool_value);
          break;
        case OptionType::Integer:
          ParseComponents(value, targets->integers);
          break;
        case OptionType::Float:
          ParseComponents(value, targets->floats);
          break;
        }
      }
    }

    // A later option with the same name replaces an earlier one.
    m_options[option.m_option_name] = option;
  }
}

void PostProcessingConfiguration::LoadOptionsConfiguration()
{
  Common::IniFile ini;
  ini.Load(File::GetUserPath(F_DOLPHINCONFIG_IDX));
  const std::string section_name = m_current_shader + "-options";

  // Every option is still marked dirty from LoadOptions.
  for (auto& [key, option] : m_options)
  {
    Common::IniFile::Section* const section = ini.GetOrCreateSection(section_name);
    if (option.m_type == OptionType::Bool)
    {
      section->Get(option.m_option_name, &option.m_bool_value, option.m_bool_value);
      continue;
    }

    std::string saved;
    section->Get(option.m_option_name, &saved);
    if (saved.empty())
      continue;
    if (option.m_type == OptionType::Integer)
      OverrideComponents(saved, &option.m_integer_values);
    else
      OverrideComponents(saved, &option.m_float_values);
  }
}

void PostProcessingConfiguration::SaveOptionsConfiguration()
{
  Common::IniFile ini;
  ini.Load(File::GetUserPath(F_DOLPHINCONFIG_IDX));
  const std::string section_name = m_current_shader + "-options";

  for (const auto& [key, option] : m_options)
  {
    Common::IniFile::Section* const section = ini.GetOrCreateSection(section_name);
    switch (option.m_type)
    {
    case OptionType::Bool:
      section->Set(option.m_option_name, option.m_bool_value);
      break;
    case OptionType::Integer:
      section->Set(option.m_option_name, JoinIntegers(option.m_integer_values));
      break;
    case OptionType::Float:
      section->Set(option.m_option_name, JoinFloats(option.m_float_values));
      break;
    }
  }
  ini.Save(File::GetUserPath(F_DOLPHINCONFIG_IDX));
}

void PostProcessingConfiguration::SetOptionf(const std::string& option, int index, float value)
{
  Option& target = m_options.find(option)->second;
  target.m_float_values[static_cast<size_t>(index)] = value;
  target.m_dirty = true;
  m_any_options_dirty = true;
}

void PostProcessingConfiguration::SetOptioni(const std::string& option, int index, s32 value)
{
  Option& target = m_options.find(option)->second;
  target.m_integer_values[static_cast<size_t>(index)] = value;
  target.m_dirty = true;
  m_any_options_dirty = true;
}

void PostProcessingConfiguration::SetOptionb(const std::string& option, bool value)
{
  Option& target = m_options.find(option)->second;
  target.m_bool_value = value;
  target.m_dirty = true;
  m_any_options_dirty = true;
}

PostProcessing::PostProcessing()
{
  m_timer.Start();
}

PostProcessing::~PostProcessing()
{
  m_timer.Stop();
}

std::vector<std::string> PostProcessing::GetShaderList()
{
  return ListShaders();
}

std::vector<std::string> PostProcessing::GetAnaglyphShaderList()
{
  return ListShaders(ANAGLYPH_DIR DIR_SEP);
}

std::vector<std::string> PostProcessing::GetPassiveShaderList()
{
  return ListShaders(PASSIVE_DIR DIR_SEP);
}

bool PostProcessing::Initialize(AbstractTextureFormat format)
{
  m_framebuffer_format = format;
  // The pixel shader goes first: it loads the options, which the vertex shader's copy of the
  // uniform block must also declare (some backends refuse to link mismatched blocks).
  return CompilePixelShader() && CompileVertexShader() && CompilePipeline();
}

void PostProcessing::RecompileShader()
{
  // Everything is rebuilt, needed or not.
  m_default_pipeline.reset();
  m_pipeline.reset();
  m_default_pixel_shader.reset();
  m_pixel_shader.reset();
  m_default_vertex_shader.reset();
  m_vertex_shader.reset();

  if (CompilePixelShader() && CompileVertexShader())
    CompilePipeline();
}

void PostProcessing::RecompilePipeline()
{
  m_default_pipeline.reset();
  m_pipeline.reset();
  CompilePipeline();
}

bool PostProcessing::IsColorCorrectionActive() const
{
  // Otherwise the built-in pass would change nothing worth its cost.
  const auto& correction = g_ActiveConfig.color_correction;
  return correction.bCorrectColorSpace || correction.bCorrectGamma ||
         m_framebuffer_format == AbstractTextureFormat::RGBA16F;
}

bool PostProcessing::NeedsIntermediaryBuffer() const
{
  // Only when there is a user pass to follow the built-in one.
  return !m_config.GetShader().empty();
}

void PostProcessing::BlitFromTexture(const MathUtil::Rectangle<int>& dst,
                                     const MathUtil::Rectangle<int>& src,
                                     const AbstractTexture* src_tex, int src_layer)
{
  // The pipelines are built for the target format; follow it if it changed.
  const AbstractTextureFormat target_format = g_gfx->GetCurrentFramebuffer()->GetColorFormat();
  if (target_format != m_framebuffer_format)
  {
    m_framebuffer_format = target_format;
    RecompilePipeline();
  }

  const bool all_layers = src_layer < 0;
  int layer = std::max(src_layer, 0);
  MathUtil::Rectangle<int> source_rect = src;
  const AbstractTexture* source = src_tex;

  // Sampler 0 is bilinear, sampler 1 point; both see the source.
  g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
  g_gfx->SetSamplerState(1, RenderState::GetPointSamplerState());
  g_gfx->SetTexture(0, source);
  g_gfx->SetTexture(1, source);

  // The "Default" resampling mode is plain bilinear sampling, which needs no pass of its own.
  const bool resample = g_ActiveConfig.output_resampling_mode > OutputResamplingMode::Default;
  const bool builtin_wanted = IsColorCorrectionActive() || resample;
  const bool user_pass_follows = NeedsIntermediaryBuffer();
  const MathUtil::Rectangle<int> window_rect = g_presenter->GetTargetRectangle();

  const AbstractPipeline* final_pipeline = m_pipeline.get();
  std::vector<u8>* uniforms = &m_default_uniform_staging_buffer;
  bool final_is_user_pass = false;

  // Fills and uploads the uniforms, then draws target_rc of target (the bound framebuffer).
  const auto upload_and_draw = [&](const AbstractPipeline* pipeline,
                                   const MathUtil::Rectangle<int>& target_rc,
                                   const AbstractFramebuffer* target, bool intermediary) {
    FillUniformBuffer(source_rect, source, layer, g_gfx->GetCurrentFramebuffer()->GetRect(),
                      window_rect, uniforms->data(), final_is_user_pass, intermediary);
    g_vertex_manager->UploadUtilityUniforms(uniforms->data(),
                                            static_cast<u32>(uniforms->size()));
    g_gfx->SetViewportAndScissor(g_gfx->ConvertFramebufferRectangle(target_rc, target));
    g_gfx->SetPipeline(pipeline);
    g_gfx->Draw(0, 3);
  };

  if (m_default_pipeline && builtin_wanted && user_pass_follows)
  {
    // Built-in pass into a high-precision intermediate, so resampling and gamma/gamut work keep
    // their quality and the user pass works on the corrected picture.
    AbstractFramebuffer* const restore_framebuffer = g_gfx->GetCurrentFramebuffer();

    // When the caller copies one layer per call (e.g. GL quad-buffer stereo), one layer is
    // enough.
    const u32 layers = all_layers ? source->GetLayers() : 1;
    const u32 width = static_cast<u32>(resample ? window_rect.GetWidth() : source_rect.GetWidth());
    const u32 height =
        static_cast<u32>(resample ? window_rect.GetHeight() : source_rect.GetHeight());

    const bool reusable = m_intermediary_frame_buffer && m_intermediary_color_texture &&
                          m_intermediary_color_texture->GetWidth() == width &&
                          m_intermediary_color_texture->GetHeight() == height &&
                          m_intermediary_color_texture->GetLayers() == layers;
    if (!reusable)
    {
      const TextureConfig config(width, height, 1, layers, source->GetSamples(),
                                 INTERMEDIATE_FORMAT, AbstractTextureFlag_RenderTarget,
                                 AbstractTextureType::Texture_2DArray);
      m_intermediary_color_texture =
          g_gfx->CreateTexture(config, "Intermediary post process texture");
      m_intermediary_frame_buffer =
          g_gfx->CreateFramebuffer(m_intermediary_color_texture.get(), nullptr);
    }

    g_gfx->SetFramebuffer(m_intermediary_frame_buffer.get());
    upload_and_draw(m_default_pipeline.get(), m_intermediary_color_texture->GetRect(),
                    m_intermediary_frame_buffer.get(), true);
    g_gfx->SetFramebuffer(restore_framebuffer);

    // The user pass reads the intermediate, whose layer 0 already holds the requested layer
    // (asking for a missing layer would fall back to the first one anyway).
    source_rect = m_intermediary_color_texture->GetRect();
    source = m_intermediary_color_texture.get();
    g_gfx->SetTexture(0, source);
    g_gfx->SetTexture(1, source);
    layer = 0;
    uniforms = &m_uniform_staging_buffer;
    final_is_user_pass = true;
  }
  else
  {
    // A single pass: the built-in one when it is wanted and there is no user shader (a second,
    // pass-through pass would be wasted), otherwise the user one.
    if (m_default_pipeline && builtin_wanted)
    {
      final_pipeline = m_default_pipeline.get();
    }
    else
    {
      uniforms = &m_uniform_staging_buffer;
      final_is_user_pass = true;
    }
    m_intermediary_frame_buffer.reset();
    m_intermediary_color_texture.reset();
  }

  // The user pass runs in gamma space, which current shaders are written for.
  if (final_pipeline)
  {
    upload_and_draw(final_pipeline, dst, g_gfx->GetCurrentFramebuffer(), false);
  }
}

std::string PostProcessing::GetUniformBufferHeader(bool user_post_process) const
{
  std::string block = "UBO_BINDING(std140, 1) uniform PSBlock {\n";
  for (const std::string_view uniform : BUILTIN_UNIFORMS)
    block += fmt::format("  {};\n", uniform);

  if (user_post_process)
  {
    block += "\n";
    // Each option takes a whole vec4 slot; unused components get filler names.
    u32 filler = 1;
    for (const auto& [name, option] : m_config.GetOptions())
    {
      std::string_view base;
      u32 components;
      switch (option.m_type)
      {
      case OptionType::Bool:
        base = "int";
        components = 1;
        break;
      case OptionType::Integer:
        base = "int";
        components = static_cast<u32>(option.m_integer_values.size());
        break;
      case OptionType::Float:
        base = "float";
        components = static_cast<u32>(option.m_float_values.size());
        break;
      default:
        continue;
      }

      if (components == 1)
        block += fmt::format("  {} {};\n", base, name);
      else
        block += fmt::format("  {}{} {};\n", base, components, name);
      for (u32 c = components; c < 4; ++c)
        block += fmt::format("  {} ubo_align_{}_;\n", base, filler++);
    }
  }

  block += "};\n\n";
  return block;
}

std::string PostProcessing::GetHeader(bool user_post_process) const
{
  std::string header = GetUniformBufferHeader(user_post_process);
  header += "SAMPLER_BINDING(0) uniform sampler2DArray samp0;\n";
  header += "SAMPLER_BINDING(1) uniform sampler2DArray samp1;\n";
  header += g_backend_info.bSupportsGeometryShaders ?
                "VARYING_LOCATION(0) in VertexData {\n  float3 v_tex0;\n};\n" :
                "VARYING_LOCATION(0) in float3 v_tex0;\n";
  header += "FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;\n";

  // The helper API for user shaders.
  header += "\n";
  header += "float4 Sample() { return texture(samp0, v_tex0); }\n";
  header += "float4 SampleLocation(float2 location) { return texture(samp0, float3(location, "
            "float(v_tex0.z))); }\n";
  header += "float4 SampleLayer(int layer) { return texture(samp0, float3(v_tex0.xy, "
            "float(layer))); }\n";
  header += "#define SampleOffset(offset) textureOffset(samp0, v_tex0, offset)\n";
  header += "\n";
  for (const ShaderGetter& getter : SHADER_GETTERS)
  {
    header += fmt::format("{} {}()\n{{\n  return {};\n}}\n\n", getter.type, getter.name,
                          getter.expression);
  }
  header += "void SetOutput(float4 color)\n{\n  ocol0 = color;\n}\n\n";
  header += "#define GetOption(x) (x)\n";
  header += "#define OptionEnabled(x) ((x) != 0)\n";
  header += "#define OptionDisabled(x) ((x) == 0)\n";
  header += "\n";
  return header;
}

std::string PostProcessing::GetFooter() const
{
  return {};
}

bool PostProcessing::CompileVertexShader()
{
  // One per uniform block layout: without and with the user options.
  m_default_vertex_shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Vertex, GetUniformBufferHeader(false) + VertexShaderBody(),
                                    nullptr, "Default post-processing vertex shader");
  m_vertex_shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Vertex, GetUniformBufferHeader(true) + VertexShaderBody(),
                                    nullptr, "Post-processing vertex shader");
  if (m_default_vertex_shader && m_vertex_shader)
    return true;

  PanicAlertFmt("Failed to compile post-processing vertex shader");
  m_default_vertex_shader.reset();
  m_vertex_shader.reset();
  return false;
}

size_t PostProcessing::CalculateUniformsSize(bool user_post_process) const
{
  // One vec4 per user option.
  const size_t option_slots = user_post_process ? m_config.GetOptions().size() : 0;
  return sizeof(BuiltinBlock) + option_slots * sizeof(float) * 4;
}

void PostProcessing::FillUniformBuffer(const MathUtil::Rectangle<int>& src,
                                       const AbstractTexture* src_tex, int src_layer,
                                       const MathUtil::Rectangle<int>& dst,
                                       const MathUtil::Rectangle<int>& wnd, u8* buffer,
                                       bool user_post_process, bool intermediary_buffer)
{
  // {width, height, 1/width, 1/height}
  const auto size_and_reciprocal = [](int width, int height) -> std::array<float, 4> {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    return {w, h, 1.0f / w, 1.0f / h};
  };

  const float tex_w = static_cast<float>(src_tex->GetWidth());
  const float tex_h = static_cast<float>(src_tex->GetHeight());
  const float inv_tex_w = 1.0f / tex_w;
  const float inv_tex_h = 1.0f / tex_h;
  const auto& correction = g_ActiveConfig.color_correction;
  const bool scrgb_output = m_framebuffer_format == AbstractTextureFormat::RGBA16F;

  BuiltinBlock block{};
  block.source_resolution = {tex_w, tex_h, inv_tex_w, inv_tex_h};
  block.target_resolution = size_and_reciprocal(dst.GetWidth(), dst.GetHeight());
  block.window_resolution = size_and_reciprocal(wnd.GetWidth(), wnd.GetHeight());
  block.src_rect = {static_cast<float>(src.left) * inv_tex_w,
                    static_cast<float>(src.top) * inv_tex_h,
                    static_cast<float>(src.GetWidth()) * inv_tex_w,
                    static_cast<float>(src.GetHeight()) * inv_tex_h};
  block.src_layer = static_cast<s32>(src_layer);
  block.time = static_cast<u32>(m_timer.ElapsedMs());
  block.graphics_api = static_cast<s32>(g_backend_info.api_type);
  block.intermediary_buffer = static_cast<s32>(intermediary_buffer);
  block.resampling_method = static_cast<s32>(g_ActiveConfig.output_resampling_mode);
  // Colour handling; mostly for the built-in pass, but every shader can read it.
  block.correct_color_space = correction.bCorrectColorSpace;
  block.game_color_space = static_cast<s32>(correction.game_color_space);
  block.correct_gamma = correction.bCorrectGamma;
  block.game_gamma = correction.fGameGamma;
  block.sdr_display_gamma_sRGB = correction.bSDRDisplayGammaSRGB;
  block.sdr_display_custom_gamma = correction.fSDRDisplayCustomGamma;
  // scRGB output is linear and may exceed 1.
  block.linear_space_output = scrgb_output;
  block.hdr_output = scrgb_output;
  block.hdr_paper_white_nits = correction.fHDRPaperWhiteNits;
  // 1.0 in scRGB is 80 nits.
  block.hdr_sdr_white_nits = 80.f;

  std::memcpy(buffer, &block, sizeof(block));
  buffer += sizeof(block);

  // Only the user pass's block has the option slots.
  if (!user_post_process)
    return;

  for (auto& [name, option] : m_config.GetOptions())
  {
    std::array<u32, 4> slot{};
    switch (option.m_type)
    {
    case OptionType::Bool:
      slot[0] = option.m_bool_value ? 1 : 0;
      break;
    case OptionType::Integer:
    {
      ASSERT(option.m_integer_values.size() <= 4);
      const size_t count = std::min<size_t>(option.m_integer_values.size(), 4);
      std::memcpy(slot.data(), option.m_integer_values.data(), count * sizeof(s32));
      break;
    }
    case OptionType::Float:
    {
      ASSERT(option.m_float_values.size() <= 4);
      const size_t count = std::min<size_t>(option.m_float_values.size(), 4);
      std::memcpy(slot.data(), option.m_float_values.data(), count * sizeof(float));
      break;
    }
    }
    option.m_dirty = false;

    std::memcpy(buffer, slot.data(), sizeof(slot));
    buffer += sizeof(slot);
  }
  m_config.SetDirty(false);
}

bool PostProcessing::CompilePixelShader()
{
  m_default_pixel_shader.reset();
  m_pixel_shader.reset();

  // The built-in pass is optional: without its file (or if it fails to build) it is skipped.
  std::string builtin_code;
  if (ReadShaderFile(BUILTIN_PASS_SHADER, "", builtin_code))
  {
    m_default_pixel_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Pixel, GetHeader(false) + builtin_code + GetFooter(), nullptr,
        "Default post-processing pixel shader");
    m_default_uniform_staging_buffer.resize(CalculateUniformsSize(false));
  }
  else
  {
    m_default_uniform_staging_buffer.resize(0);
  }

  m_config.LoadShader(g_ActiveConfig.sPostProcessingShader);
  m_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, GetHeader(true) + m_config.GetShaderCode() + GetFooter(),
      m_config.GetShaderIncluder(),
      fmt::format("User post-processing pixel shader: {}", m_config.GetShader()));

  if (!m_pixel_shader)
  {
    // Fall back to the pass-through shader.
    PanicAlertFmt("Failed to compile user post-processing shader {}", m_config.GetShader());
    m_config.LoadDefaultShader();
    m_pixel_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Pixel, GetHeader(true) + m_config.GetShaderCode() + GetFooter(), nullptr,
        "Default user post-processing pixel shader");
    if (!m_pixel_shader)
    {
      m_uniform_staging_buffer.resize(0);
      return false;
    }
  }

  m_uniform_staging_buffer.resize(CalculateUniformsSize(true));
  return true;
}

bool PostProcessing::CompilePipeline()
{
  // Nothing to draw into yet (and some backends reject pipelines without a target format).
  if (m_framebuffer_format == AbstractTextureFormat::Undefined)
    return true;

  const auto geometry_shader_for = [](bool is_intermediary_buffer) -> const AbstractShader* {
    if (!PassUsesLayeredStereo(is_intermediary_buffer))
      return nullptr;
    return g_shader_cache->GetTexcoordGeometryShader();
  };
  const bool to_intermediate = NeedsIntermediaryBuffer();

  AbstractPipelineConfig config = {};
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.usage = AbstractPipelineUsage::Utility;

  // Built-in pass: into the intermediate when a user pass follows. Without its shader it is
  // left out.
  config.vertex_shader = m_default_vertex_shader.get();
  config.geometry_shader = geometry_shader_for(to_intermediate);
  config.pixel_shader = m_default_pixel_shader.get();
  config.framebuffer_state = RenderState::GetColorFramebufferState(
      to_intermediate ? INTERMEDIATE_FORMAT : m_framebuffer_format);
  if (config.pixel_shader)
    m_default_pipeline = g_gfx->CreatePipeline(config);

  // User pass: always the final one.
  config.vertex_shader = m_vertex_shader.get();
  config.geometry_shader = geometry_shader_for(false);
  config.pixel_shader = m_pixel_shader.get();
  config.framebuffer_state = RenderState::GetColorFramebufferState(m_framebuffer_format);
  m_pipeline = g_gfx->CreatePipeline(config);
  return m_pipeline != nullptr;
}
}  // namespace VideoCommon
