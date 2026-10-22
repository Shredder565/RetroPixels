// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Spirv.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <glslang/SPIRV/GlslangToSpv.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Version.h"

#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// Every shader is compiled as desktop GLSL 4.50 (core profile).
constexpr int GLSL_VERSION = 450;

// glslang always prints a notice that 4.50 support is incomplete; info logs no longer than that
// notice (or the equally fixed program log text) are not worth a warning.
constexpr std::size_t SHADER_LOG_NOISE_LENGTH = 108;
constexpr std::size_t PROGRAM_LOG_NOISE_LENGTH = 25;

// glslang needs process-wide setup once; it is torn down at exit.
bool EnsureGlslangInitialized()
{
  static bool ready = false;
  if (ready)
    return true;

  if (!glslang::InitializeProcess())
  {
    PanicAlertFmt("Failed to initialize glslang shader compiler");
    return false;
  }
  std::atexit([] { glslang::FinalizeProcess(); });
  ready = true;
  return true;
}

// Implementation limits glslang validates against. They are generous, roughly a desktop GL 4.5
// part; anything left out is zero.
TBuiltInResource MakeResourceLimits()
{
  TBuiltInResource r{};

  // Legacy and general limits.
  r.maxLights = 32;
  r.maxClipPlanes = 6;
  r.maxTextureUnits = r.maxTextureCoords = 32;
  r.maxVertexAttribs = 64;
  r.maxVertexUniformComponents = r.maxFragmentUniformComponents = 4096;
  r.maxVaryingFloats = 64;
  r.maxVertexTextureImageUnits = r.maxTextureImageUnits = 32;
  r.maxCombinedTextureImageUnits = 80;
  r.maxDrawBuffers = 32;
  r.maxVertexUniformVectors = 128;
  r.maxVaryingVectors = 8;
  r.maxFragmentUniformVectors = r.maxVertexOutputVectors = 16;
  r.maxFragmentInputVectors = 15;
  r.minProgramTexelOffset = -8;
  r.maxProgramTexelOffset = 7;
  r.maxClipDistances = r.maxCullDistances = r.maxCombinedClipAndCullDistances = 8;
  r.maxVaryingComponents = 60;
  r.maxViewports = 16;
  r.maxSamples = 4;
  r.maxDualSourceDrawBuffersEXT = 1;

  // Compute.
  r.maxComputeWorkGroupCountX = r.maxComputeWorkGroupCountY = r.maxComputeWorkGroupCountZ = 65535;
  r.maxComputeWorkGroupSizeX = r.maxComputeWorkGroupSizeY = 1024;
  r.maxComputeWorkGroupSizeZ = 64;
  r.maxComputeUniformComponents = 1024;
  r.maxComputeTextureImageUnits = 16;
  r.maxComputeImageUniforms = r.maxComputeAtomicCounters = 8;
  r.maxComputeAtomicCounterBuffers = 1;

  // Stage interfaces.
  r.maxVertexOutputComponents = r.maxGeometryInputComponents = 64;
  r.maxGeometryOutputComponents = r.maxFragmentInputComponents = 128;

  // Images (only the fragment stage gets any).
  r.maxImageUnits = 8;
  r.maxCombinedImageUnitsAndFragmentOutputs = r.maxCombinedShaderOutputResources = 8;
  r.maxFragmentImageUniforms = r.maxCombinedImageUniforms = 8;

  // Geometry.
  r.maxGeometryTextureImageUnits = 16;
  r.maxGeometryOutputVertices = 256;
  r.maxGeometryTotalOutputComponents = r.maxGeometryUniformComponents = 1024;
  r.maxGeometryVaryingComponents = 64;

  // Tessellation.
  r.maxTessControlInputComponents = r.maxTessControlOutputComponents = 128;
  r.maxTessEvaluationInputComponents = r.maxTessEvaluationOutputComponents = 128;
  r.maxTessControlTextureImageUnits = r.maxTessEvaluationTextureImageUnits = 16;
  r.maxTessControlUniformComponents = r.maxTessEvaluationUniformComponents = 1024;
  r.maxTessControlTotalOutputComponents = 4096;
  r.maxTessPatchComponents = 120;
  r.maxPatchVertices = 32;
  r.maxTessGenLevel = 64;

  // Atomic counters (fragment stage only).
  r.maxFragmentAtomicCounters = r.maxCombinedAtomicCounters = 8;
  r.maxAtomicCounterBindings = 1;
  r.maxFragmentAtomicCounterBuffers = r.maxCombinedAtomicCounterBuffers = 1;
  r.maxAtomicCounterBufferSize = 16384;

  // Transform feedback.
  r.maxTransformFeedbackBuffers = 4;
  r.maxTransformFeedbackInterleavedComponents = 64;

  // Mesh and task shaders, NV and EXT flavours.
  r.maxMeshOutputVerticesNV = 256;
  r.maxMeshOutputPrimitivesNV = 512;
  r.maxMeshWorkGroupSizeX_NV = r.maxTaskWorkGroupSizeX_NV = 32;
  r.maxMeshWorkGroupSizeY_NV = r.maxMeshWorkGroupSizeZ_NV = 1;
  r.maxTaskWorkGroupSizeY_NV = r.maxTaskWorkGroupSizeZ_NV = 1;
  r.maxMeshViewCountNV = 4;
  r.maxMeshOutputVerticesEXT = r.maxMeshOutputPrimitivesEXT = 256;
  r.maxMeshWorkGroupSizeX_EXT = r.maxMeshWorkGroupSizeY_EXT = r.maxMeshWorkGroupSizeZ_EXT = 128;
  r.maxTaskWorkGroupSizeX_EXT = r.maxTaskWorkGroupSizeY_EXT = r.maxTaskWorkGroupSizeZ_EXT = 128;
  r.maxMeshViewCountEXT = 4;

  // No restrictions on loops or indexing.
  TLimits& l = r.limits;
  l.nonInductiveForLoops = l.whileLoops = l.doWhileLoops = true;
  l.generalUniformIndexing = l.generalAttributeMatrixVectorIndexing = true;
  l.generalVaryingIndexing = l.generalSamplerIndexing = l.generalVariableIndexing = true;
  l.generalConstantMatrixVectorIndexing = true;
  return r;
}

const TBuiltInResource& ResourceLimits()
{
  static const TBuiltInResource limits = MakeResourceLimits();
  return limits;
}

// One compilation: parse, link (needed even for a single stage) and convert to SPIR-V.
class SpirvCompile
{
public:
  SpirvCompile(EShLanguage stage, const char* stage_name, std::string_view source)
      : m_stage(stage), m_stage_name(stage_name), m_source(source),
        m_source_ptr(source.data()), m_source_length(static_cast<int>(source.size())),
        m_shader(std::make_unique<glslang::TShader>(stage))
  {
  }

  std::optional<SPIRV::CodeVector> Run(APIType api_type,
                                       glslang::EShTargetLanguageVersion language_version,
                                       glslang::TShader::Includer* includer);

private:
  void ReportFailure(const char* what);
  void LogMessages(const spv::SpvBuildLogger& logger) const;

  EShLanguage m_stage;
  const char* m_stage_name;
  std::string_view m_source;
  const char* m_source_ptr;
  int m_source_length;
  std::unique_ptr<glslang::TShader> m_shader;
  std::unique_ptr<glslang::TProgram> m_program;
};

std::optional<SPIRV::CodeVector>
SpirvCompile::Run(APIType api_type, glslang::EShTargetLanguageVersion language_version,
                  glslang::TShader::Includer* includer)
{
  // Vulkan and Metal (through MoltenVK/SPIRV-Cross) want Vulkan semantics; the other users only
  // need SPIR-V rules.
  int message_bits = EShMsgDefault | EShMsgSpvRules;
  if (api_type == APIType::Vulkan || api_type == APIType::Metal)
    message_bits |= EShMsgVulkanRules;
  const auto messages = static_cast<EShMessages>(message_bits);

  m_shader->setEnvTarget(glslang::EShTargetSpv, language_version);
  m_shader->setStringsWithLengths(&m_source_ptr, &m_source_length, 1);

  glslang::TShader::ForbidIncluder no_includes;
  glslang::TShader::Includer& active_includer = includer ? *includer : no_includes;
  if (!m_shader->parse(&ResourceLimits(), GLSL_VERSION, ECoreProfile, false, true, messages,
                       active_includer))
  {
    ReportFailure("Failed to parse shader");
    return std::nullopt;
  }

  m_program = std::make_unique<glslang::TProgram>();
  m_program->addShader(m_shader.get());
  if (!m_program->link(messages))
  {
    ReportFailure("Failed to link program");
    return std::nullopt;
  }

  glslang::TIntermediate* const intermediate = m_program->getIntermediate(m_stage);
  if (intermediate == nullptr)
  {
    ReportFailure("Failed to generate SPIR-V");
    return std::nullopt;
  }

  glslang::SpvOptions options;
  if (g_ActiveConfig.bEnableValidationLayer)
  {
    // Debug builds of the shader: unoptimised, validated, with the source embedded so that
    // graphics debuggers can show it.
    m_shader->setSourceFile(m_stage_name);
    m_shader->addSourceText(m_source_ptr, static_cast<std::size_t>(m_source_length));
    options.generateDebugInfo = true;
    options.disableOptimizer = true;
    options.optimizeSize = false;
    options.disassemble = false;
    options.validate = true;
  }
  else
  {
    options.disableOptimizer = false;
    options.stripDebugInfo = true;
  }

  SPIRV::CodeVector code;
  spv::SpvBuildLogger logger;
  glslang::GlslangToSpv(*intermediate, code, &logger, &options);
  LogMessages(logger);
  return code;
}

void SpirvCompile::LogMessages(const spv::SpvBuildLogger& logger) const
{
  const char* const shader_log = m_shader->getInfoLog();
  const char* const shader_debug_log = m_shader->getInfoDebugLog();
  const char* const program_log = m_program->getInfoLog();
  const char* const program_debug_log = m_program->getInfoDebugLog();

  if (std::strlen(shader_log) > SHADER_LOG_NOISE_LENGTH)
    WARN_LOG_FMT(VIDEO, "Shader info log: {}", shader_log);
  if (shader_debug_log[0] != '\0')
    WARN_LOG_FMT(VIDEO, "Shader debug info log: {}", shader_debug_log);
  if (std::strlen(program_log) > PROGRAM_LOG_NOISE_LENGTH)
    WARN_LOG_FMT(VIDEO, "Program info log: {}", program_log);
  if (program_debug_log[0] != '\0')
    WARN_LOG_FMT(VIDEO, "Program debug info log: {}", program_debug_log);

  const std::string conversion_messages = logger.getAllMessages();
  if (!conversion_messages.empty())
    WARN_LOG_FMT(VIDEO, "SPIR-V conversion messages: {}", conversion_messages);
}

// Writes the source and the compiler logs to a numbered bad-shader file and alerts the user.
void SpirvCompile::ReportFailure(const char* what)
{
  static int s_dump_index = 0;
  const std::string path = VideoBackendBase::BadShaderFilename(m_stage_name, s_dump_index++);

  std::ofstream file;
  File::OpenFStream(file, path, std::ios_base::out);
  if (file.good())
  {
    file << m_source << std::endl << what << std::endl;
    file << "Shader Info Log:" << std::endl
         << m_shader->getInfoLog() << std::endl
         << m_shader->getInfoDebugLog() << std::endl;
    if (m_program)
    {
      file << "Program Info Log:" << std::endl
           << m_program->getInfoLog() << std::endl
           << m_program->getInfoDebugLog() << std::endl;
    }
  }
  file << "\n";
  file << "Dolphin Version: " + Common::GetScmRevStr() + "\n";
  file << "Video Backend: " + g_video_backend->GetDisplayName();
  file.close();

  PanicAlertFmt("{} (written to {})\nDebug info:\n{}", what, path, m_shader->getInfoLog());
}

std::optional<SPIRV::CodeVector> Compile(EShLanguage stage, const char* stage_name,
                                         std::string_view source, APIType api_type,
                                         glslang::EShTargetLanguageVersion language_version,
                                         glslang::TShader::Includer* includer)
{
  if (!EnsureGlslangInitialized())
    return std::nullopt;
  SpirvCompile compile(stage, stage_name, source);
  return compile.Run(api_type, language_version, includer);
}
}  // namespace

namespace SPIRV
{
std::optional<CodeVector> CompileVertexShader(std::string_view source_code, APIType api_type,
                                              glslang::EShTargetLanguageVersion language_version,
                                              glslang::TShader::Includer* shader_includer)
{
  return Compile(EShLangVertex, "vs", source_code, api_type, language_version, shader_includer);
}

std::optional<CodeVector> CompileGeometryShader(std::string_view source_code, APIType api_type,
                                                glslang::EShTargetLanguageVersion language_version,
                                                glslang::TShader::Includer* shader_includer)
{
  return Compile(EShLangGeometry, "gs", source_code, api_type, language_version, shader_includer);
}

std::optional<CodeVector> CompileFragmentShader(std::string_view source_code, APIType api_type,
                                                glslang::EShTargetLanguageVersion language_version,
                                                glslang::TShader::Includer* shader_includer)
{
  return Compile(EShLangFragment, "ps", source_code, api_type, language_version, shader_includer);
}

std::optional<CodeVector> CompileComputeShader(std::string_view source_code, APIType api_type,
                                               glslang::EShTargetLanguageVersion language_version,
                                               glslang::TShader::Includer* shader_includer)
{
  return Compile(EShLangCompute, "cs", source_code, api_type, language_version, shader_includer);
}
}  // namespace SPIRV
