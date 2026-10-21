// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLConfig.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/GL/GLContext.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/OGL/OGLTexture.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
bool Has(const char* name)
{
  return GLExtensions::Supports(name);
}

bool HasAny(std::initializer_list<const char*> names)
{
  return std::ranges::any_of(names, [](const char* name) { return Has(name); });
}

bool Bug(DriverDetails::Bug bug)
{
  return DriverDetails::HasBug(bug);
}

// ---------------------------------------------------------------------------------------------
// Driver identification

struct DriverId
{
  DriverDetails::Vendor vendor = DriverDetails::VENDOR_UNKNOWN;
  DriverDetails::Driver driver = DriverDetails::DRIVER_UNKNOWN;
  DriverDetails::Family family = DriverDetails::Family::UNKNOWN;
  double version = 0.0;
};

// The GL_VENDOR string decides the vendor, except that any Mesa version string means Mesa.
void IdentifyVendor(std::string_view vendor, std::string_view renderer, std::string_view version,
                    DriverId* id)
{
  using namespace DriverDetails;
  if (vendor == "NVIDIA Corporation")
    id->vendor = renderer == "NVIDIA Tegra" ? VENDOR_TEGRA : VENDOR_NVIDIA;
  else if (vendor == "ATI Technologies Inc." || vendor == "Advanced Micro Devices, Inc.")
    id->vendor = VENDOR_ATI;
  else if (version.contains("Mesa"))
    id->vendor = VENDOR_MESA;
  else if (vendor.contains("Intel"))
    id->vendor = VENDOR_INTEL;
  else if (vendor == "ARM")
    id->vendor = VENDOR_ARM;
  else if (vendor == "http://limadriver.org/")
  {
    id->vendor = VENDOR_ARM;
    id->driver = DRIVER_LIMA;  // (ReadMaliVersion replaces this with DRIVER_ARM.)
  }
  else if (vendor == "Qualcomm")
    id->vendor = VENDOR_QUALCOMM;
  else if (vendor == "Imagination Technologies")
    id->vendor = VENDOR_IMGTEC;
  else if (vendor == "Vivante Corporation")
    id->vendor = VENDOR_VIVANTE;
}

void ReadQualcommVersion(const char* version, DriverId* id)
{
  // "OpenGL ES 3.1 V@145.0 ..." -> 145.0
  id->driver = DriverDetails::DRIVER_QUALCOMM;
  double es_version = 0.0;
  std::sscanf(version, "OpenGL ES %lg V@%lg", &es_version, &id->version);
}

void ReadMaliVersion(DriverId* id)
{
  // The Mali driver does not report its release. r4p0 added pixel local storage, and anything
  // older without ES 3.0 is unsupported anyway, so the extension tells the two apart.
  id->driver = DriverDetails::DRIVER_ARM;
  id->version = Has("GL_EXT_shader_pixel_local_storage") ? 400 : 300;
}

void ReadMesaDetails(std::string_view vendor, std::string_view renderer, const char* version,
                     DriverId* id)
{
  if (vendor == "nouveau")
  {
    id->driver = DriverDetails::DRIVER_NOUVEAU;
  }
  else if (vendor == "Intel Open Source Technology Center")
  {
    id->driver = DriverDetails::DRIVER_I965;
    if (renderer.contains("Sandybridge"))
      id->family = DriverDetails::Family::INTEL_SANDY;
    else if (renderer.contains("Ivybridge"))
      id->family = DriverDetails::Family::INTEL_IVY;
  }
  else if (renderer.contains("AMD") || renderer.contains("ATI"))
  {
    id->driver = DriverDetails::DRIVER_R600;
  }

  int major = 0, minor = 0, release = 0;
  std::sscanf(version, "%*s (Core Profile) Mesa %d.%d.%d", &major, &minor, &release);
  id->version = 100 * major + 10 * minor + release;
}

void ReadIntelDetails(const char* renderer, [[maybe_unused]] const char* version, DriverId* id)
{
  int model = 0;
  std::sscanf(renderer, "Intel HD Graphics %d", &model);
  if (model == 2000 || model == 3000)
    id->family = DriverDetails::Family::INTEL_SANDY;
  else if (model == 2500 || model == 4000)
    id->family = DriverDetails::Family::INTEL_IVY;
  else
    id->family = DriverDetails::Family::UNKNOWN;

#ifdef _WIN32
  // e.g. "4.3.0 - Build 10.18.10.3907": the build number is the driver version.
  int gl_major = 0, gl_minor = 0;
  int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  std::sscanf(version, "%d.%d.0 - Build %d.%d.%d.%d", &gl_major, &gl_minor, &b0, &b1, &b2, &b3);
  const int packed = 100000000 * b0 + 1000000 * b1 + 10000 * b2 + b3;
  id->version = static_cast<double>(packed) / 10000;
#endif
}

void ReadNvidiaVersion(const char* version, DriverId* id)
{
  // "4.6.0 NVIDIA 535.98" on Linux; Windows drivers leave the number out.
  int gl_major = 0, gl_minor = 0, gl_release = 0;
  int major = 0, minor = 0;
  std::sscanf(version, "%d.%d.%d NVIDIA %d.%d", &gl_major, &gl_minor, &gl_release, &major, &minor);
  id->version = 100 * major + minor;
}

void ReadImgTecVersion(const char* version, DriverId* id)
{
  // "OpenGL ES 3.2 build 1.9@4850625": branch 1.9 becomes 109, the change number the fraction.
  // Change numbers are only comparable within one branch.
  id->driver = DriverDetails::DRIVER_IMGTEC;
  constexpr double CHANGE_SCALE = 10000000;
  double es_version = 0.0;
  int branch_major = 0, branch_minor = 0, change = 0;
  std::sscanf(version, "OpenGL ES %lg build %d.%d@%d", &es_version, &branch_major, &branch_minor,
              &change);
  id->version = 100 * branch_major + branch_minor;
  if (change < CHANGE_SCALE)
    id->version += static_cast<double>(change) / CHANGE_SCALE;
  else
    ERROR_LOG_FMT(VIDEO, "Version changeID overflow - change:{} scale:{}", change, CHANGE_SCALE);
}

// ---------------------------------------------------------------------------------------------
// Capability detection

// Desktop GL extensions the backend cannot work without. Every check runs so the user sees
// all of what is missing.
bool CheckDesktopRequirements()
{
  bool ok = true;
  if (!Has("GL_ARB_framebuffer_object"))
  {
    // Needed for blitting between framebuffers (and matches what ES 3 has).
    PanicAlertFmtT("GPU: ERROR: Need GL_ARB_framebuffer_object for multiple render targets.\n"
                   "GPU: Does your video card support OpenGL 3.0?");
    ok = false;
  }
  if (!Has("GL_ARB_vertex_array_object"))
  {
    PanicAlertFmtT("GPU: OGL ERROR: Need GL_ARB_vertex_array_object.\n"
                   "GPU: Does your video card support OpenGL 3.0?");
    ok = false;
  }
  if (!Has("GL_ARB_map_buffer_range"))
  {
    // All the streaming buffers map ranges.
    PanicAlertFmtT("GPU: OGL ERROR: Need GL_ARB_map_buffer_range.\n"
                   "GPU: Does your video card support OpenGL 3.0?");
    ok = false;
  }
  if (!Has("GL_ARB_uniform_buffer_object"))
  {
    PanicAlertFmtT("GPU: OGL ERROR: Need GL_ARB_uniform_buffer_object.\n"
                   "GPU: Does your video card support OpenGL 3.1?");
    ok = false;
  }
  else if (Bug(DriverDetails::BUG_BROKEN_UBO))
  {
    PanicAlertFmtT(
        "Buggy GPU driver detected.\n"
        "Please either install the closed-source GPU driver or update your Mesa 3D version.");
    ok = false;
  }
  if (!Has("GL_ARB_sampler_objects"))
  {
    // The sampler cache is built on sampler objects.
    PanicAlertFmtT("GPU: OGL ERROR: Need GL_ARB_sampler_objects.\n"
                   "GPU: Does your video card support OpenGL 3.3?");
    ok = false;
  }
  return ok;
}

// What the extension list says, before the version-specific adjustments.
void ReadExtensionFeatures(bool is_es, bool* program_binaries)
{
  auto& info = g_backend_info;
  auto& gl = g_ogl_config;
  const u32 version = GLExtensions::Version();
  const bool shader5 = Has("GL_ARB_gpu_shader5");

  info.bSupportsDualSourceBlend =
      HasAny({"GL_ARB_blend_func_extended", "GL_EXT_blend_func_extended"});
  info.bSupportsPrimitiveRestart = !Bug(DriverDetails::BUG_PRIMITIVE_RESTART) &&
                                   (version >= 310 || Has("GL_NV_primitive_restart"));
  info.bSupportsGSInstancing = shader5;
  info.bSupportsSSAA = shader5 && Has("GL_ARB_sample_shading");
  info.bSupportsGeometryShaders =
      version >= 320 && !Bug(DriverDetails::BUG_BROKEN_GEOMETRY_SHADERS);
  info.bSupportsPaletteConversion =
      HasAny({"GL_ARB_texture_buffer_object", "GL_OES_texture_buffer", "GL_EXT_texture_buffer"});
  info.bSupportsClipControl = Has("GL_ARB_clip_control");
  gl.bSupportsCopySubImage =
      HasAny({"GL_ARB_copy_image", "GL_NV_copy_image", "GL_EXT_copy_image", "GL_OES_copy_image"}) &&
      !Bug(DriverDetails::BUG_BROKEN_COPYIMAGE);
  // Note: asked for without the "GL_" prefix, as it always has been.
  gl.bSupportsTextureSubImage = Has("ARB_get_texture_sub_image");

  // Binding layout qualifiers on desktop come with 420pack (ES 3.1 has them built in).
  info.bSupportsBindingLayout = Has("GL_ARB_shading_language_420pack");
  // Clip distances are only useful with depth clamping.
  info.bSupportsDepthClamp = Has("GL_ARB_depth_clamp");
  // Bitfield ops and dynamic sampler indexing come with shader5 on desktop.
  info.bSupportsBitfield = shader5;
  info.bSupportsDynamicSamplerIndexing = shader5;

  gl.bIsES = is_es;
  *program_binaries = Has("GL_ARB_get_program_binary");
  gl.bSupportsGLPinnedMemory = Has("GL_AMD_pinned_memory");
  gl.bSupportsGLSync = Has("GL_ARB_sync");
  gl.bSupportsGLBaseVertex =
      HasAny({"GL_ARB_draw_elements_base_vertex", "GL_EXT_draw_elements_base_vertex",
              "GL_OES_draw_elements_base_vertex"});
  gl.bSupportsGLBufferStorage = HasAny({"GL_ARB_buffer_storage", "GL_EXT_buffer_storage"});
  gl.bSupportsMSAA = Has("GL_ARB_texture_multisample");
  gl.bSupportViewportFloat = Has("GL_ARB_viewport_array");
  gl.bSupportsDebug = HasAny({"GL_KHR_debug", "GL_ARB_debug_output"});
  gl.bSupportsTextureStorage = Has("GL_ARB_texture_storage");
  gl.SupportedMultisampleTexStorage = MultisampleTexStorageType::TexStorageNone;
  gl.bSupportsImageLoadStore = Has("GL_ARB_shader_image_load_store");
  gl.bSupportsConservativeDepth = Has("GL_ARB_conservative_depth");
  gl.bSupportsAniso = Has("GL_EXT_texture_filter_anisotropic");
  info.bSupportsComputeShaders = Has("GL_ARB_compute_shader");
  info.bSupportsST3CTextures = Has("GL_EXT_texture_compression_s3tc");
  info.bSupportsBPTCTextures = Has("GL_ARB_texture_compression_bptc");
  info.bSupportsCoarseDerivatives = Has("GL_ARB_derivative_control") || version >= 450;
  info.bSupportsTextureQueryLevels = Has("GL_ARB_texture_query_levels") || version >= 430;

  // Fragment stores (bounding box) and vertex-shader line/point expansion read SSBOs.
  if (Has("GL_ARB_shader_storage_buffer_object"))
  {
    GLint fragment_blocks = 0;
    GLint vertex_blocks = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragment_blocks);
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertex_blocks);
    info.bSupportsFragmentStoresAndAtomics = fragment_blocks >= 1;
    info.bSupportsVSLinePointExpand = vertex_blocks >= 1;
  }
  else
  {
    info.bSupportsFragmentStoresAndAtomics = false;
    info.bSupportsVSLinePointExpand = false;
  }

  if (Has("GL_EXT_shader_framebuffer_fetch"))
    gl.SupportedFramebufferFetch = EsFbFetchType::FbFetchExt;
  else if (Has("GL_ARM_shader_framebuffer_fetch"))
    gl.SupportedFramebufferFetch = EsFbFetchType::FbFetchArm;
  else
    gl.SupportedFramebufferFetch = EsFbFetchType::FbFetchNone;
  info.bSupportsFramebufferFetch = gl.SupportedFramebufferFetch != EsFbFetchType::FbFetchNone;
}

void ApplyESProfile(bool* program_binaries)
{
  auto& info = g_backend_info;
  auto& gl = g_ogl_config;

  if (Has("GL_OES_geometry_point_size"))
    gl.SupportedESPointSize = EsPointSizeType::PointSizeOes;
  else if (Has("GL_EXT_geometry_point_size"))
    gl.SupportedESPointSize = EsPointSizeType::PointSizeExt;
  else
    gl.SupportedESPointSize = EsPointSizeType::PointSizeNone;

  if (Has("VERSION_GLES_3_2"))
    gl.SupportedESTextureBuffer = EsTexbufType::TexbufCore;
  else if (Has("GL_OES_texture_buffer"))
    gl.SupportedESTextureBuffer = EsTexbufType::TexbufOes;
  else if (Has("GL_EXT_texture_buffer"))
    gl.SupportedESTextureBuffer = EsTexbufType::TexbufExt;
  else
    gl.SupportedESTextureBuffer = EsTexbufType::TexbufNone;

  // ES 3.0 has program binaries and sync objects in core.
  *program_binaries = true;
  gl.bSupportsGLSync = true;

  // No depth clamp extension to go with GL_EXT_clip_cull_distance, no logic ops, no LOD bias
  // sampler parameter.
  info.bSupportsDepthClamp = false;
  info.bSupportsLogicOp = false;
  info.bSupportsLodBiasInSampler = false;
  // glReadPixels cannot read depth on ES; glGetTextureSubImage can, where it exists.
  info.bSupportsDepthReadback = gl.bSupportsTextureSubImage;

  const bool has_point_size = gl.SupportedESPointSize != EsPointSizeType::PointSizeNone;
  const u32 version = GLExtensions::Version();
  if (version == 300)
  {
    gl.eSupportedGLSLVersion = GlslEs300;
    gl.bSupportsAEP = false;
    gl.bSupportsTextureStorage = true;
    info.bSupportsGeometryShaders = false;
    return;
  }

  // ES 3.1 and newer.
  gl.bSupportsAEP = Has("GL_ANDROID_extension_pack_es31a");
  info.bSupportsBindingLayout = true;
  gl.bSupportsImageLoadStore = true;
  info.bSupportsComputeShaders = true;
  info.bSupportsFragmentStoresAndAtomics = true;
  gl.bSupportsMSAA = true;
  gl.bSupportsTextureStorage = true;
  info.bSupportsBitfield = true;

  if (version == 310)
  {
    // Geometry shaders, sample shading and sampler indexing need the extension pack on 3.1.
    gl.eSupportedGLSLVersion = GlslEs310;
    info.bSupportsGeometryShaders = gl.bSupportsAEP;
    info.bSupportsGSInstancing = info.bSupportsGeometryShaders && has_point_size;
    info.bSupportsSSAA = gl.bSupportsAEP;
    if (Has("GL_OES_texture_storage_multisample_2d_array"))
      gl.SupportedMultisampleTexStorage = MultisampleTexStorageType::TexStorageOes;
    info.bSupportsDynamicSamplerIndexing = gl.bSupportsAEP;
    return;
  }

  gl.eSupportedGLSLVersion = GlslEs320;
  info.bSupportsGeometryShaders = true;
  info.bSupportsGSInstancing = has_point_size;
  info.bSupportsPaletteConversion = true;
  info.bSupportsSSAA = true;
  gl.bSupportsCopySubImage = true;
  gl.bSupportsGLBaseVertex = true;
  gl.bSupportsDebug = true;
  gl.SupportedMultisampleTexStorage = MultisampleTexStorageType::TexStorageCore;
  info.bSupportsDynamicSamplerIndexing = true;
  info.bSupportsSettingObjectNames = true;
}

bool ApplyDesktopProfile()
{
  auto& info = g_backend_info;
  auto& gl = g_ogl_config;
  bool ok = true;

  if (Has("GL_ARB_texture_storage_multisample"))
    gl.SupportedMultisampleTexStorage = MultisampleTexStorageType::TexStorageCore;

  const u32 version = GLExtensions::Version();
  if (version < 300)
  {
    PanicAlertFmtT("GPU: OGL ERROR: Need at least GLSL 1.30\n"
                   "GPU: Does your video card support OpenGL 3.0?\n"
                   "GPU: Your driver supports GLSL {0}",
                   reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
    ok = false;
  }
  else if (version == 300 || version == 310)
  {
    // GLSL 1.30/1.40 have no layout qualifiers (needed for early depth and image access) and
    // no geometry shaders.
    gl.eSupportedGLSLVersion = version == 300 ? Glsl130 : Glsl140;
    gl.bSupportsImageLoadStore = false;
    gl.bSupportsConservativeDepth = false;
    info.bSupportsGeometryShaders = false;
  }
  else if (version == 320)
  {
    gl.eSupportedGLSLVersion = Glsl150;
  }
  else if (version == 330)
  {
    gl.eSupportedGLSLVersion = Glsl330;
    gl.bSupportsExplicitLayoutInShader = Has("GL_ARB_explicit_attrib_location");
  }
  else if (version >= 430)
  {
    // The GLSL version follows the GL version; GL_SHADING_LANGUAGE_VERSION is not parsed.
    gl.eSupportedGLSLVersion = version >= 450 ? Glsl450 : Glsl430;
    gl.bSupportsTextureStorage = true;
    gl.SupportedMultisampleTexStorage = MultisampleTexStorageType::TexStorageCore;
    gl.bSupportsImageLoadStore = true;
    gl.bSupportsExplicitLayoutInShader = true;
    info.bSupportsSSAA = true;
    info.bSupportsSettingObjectNames = true;
    info.bSupportsComputeShaders = true;  // core in 4.3
    if (version >= 450)
      gl.bSupportsTextureSubImage = true;
  }
  else
  {
    // 4.0 - 4.2 (and any other version between 3.3 and 4.3).
    gl.eSupportedGLSLVersion = Glsl400;
    info.bSupportsSSAA = true;
    if (version == 420)
    {
      gl.bSupportsTextureStorage = true;
      gl.bSupportsImageLoadStore = true;
    }
  }

  gl.bSupportsAEP = false;

  // gl_PointSize only takes effect on desktop with GL_PROGRAM_POINT_SIZE (ES has it always).
  glEnable(GL_PROGRAM_POINT_SIZE);
  return ok;
}

// Sample counts the driver offers for multisampled array textures of the given format, largest
// first, with 1 added at the end when the driver leaves it out.
std::vector<int> QuerySampleCounts(GLenum internal_format, const char* kind, const char* kind_short)
{
  GLint count = 0;
  glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, internal_format, GL_NUM_SAMPLE_COUNTS, 1,
                        &count);
  ASSERT_MSG(VIDEO, count >= 0, "negative GL_NUM_SAMPLE_COUNTS for {} does not make sense: {}",
             kind, count);

  std::vector<int> counts;
  counts.reserve(static_cast<size_t>(std::max(count, 0)) + 1);
  if (count > 0)
  {
    counts.resize(static_cast<size_t>(count));
    static_assert(sizeof(GLint) == sizeof(int));
    glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, internal_format, GL_SAMPLES, count,
                          counts.data());
    ASSERT_MSG(VIDEO, std::ranges::is_sorted(counts | std::views::reverse),
               "GPU driver didn't return sorted {} AA modes: [{}]", kind_short,
               fmt::join(counts, ", "));
  }
  if (counts.empty() || counts.back() != 1)
    counts.push_back(1);
  return counts;
}

void DetectAAModes()
{
  auto& modes = g_backend_info.AAModes;
  modes.clear();

  if (!g_ogl_config.bSupportsMSAA)
  {
    modes = {1};
    return;
  }

  if (Has("VERSION_4_2") || Has("VERSION_GLES_3"))
  {
    // Ask per format. Both lists are taken for the EFB colour format. (The ES
    // GL_TEXTURE_2D_MULTISAMPLE_ARRAY_OES token has the same value, 0x9102.)
    const GLenum format = OGLTexture::GetGLInternalFormatForTextureFormat(
        FramebufferManager::GetEFBColorFormat(), true);
    const std::vector<int> color = QuerySampleCounts(format, "colors", "color");
    const std::vector<int> depth = QuerySampleCounts(format, "depth", "depth");

    // Counts usable for both, ascending (the lists are descending, so walk them reversed).
    modes.clear();
    modes.reserve(std::min(color.size(), depth.size()));
    std::ranges::set_intersection(color | std::views::reverse, depth | std::views::reverse,
                                  std::back_inserter(modes));
    return;
  }

  // Without the per-format query the limit is the smallest of the general limits. (The colour
  // texture limit is read for depth as well.)
  GLint color_limit = 0;
  GLint depth_limit = 0;
  GLint framebuffer_limit = 0;
  glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &color_limit);
  glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &depth_limit);
  glGetIntegerv(GL_MAX_SAMPLES, &framebuffer_limit);

  // Every power of two from the limit down to 1, listed ascending for the UI.
  u32 samples = static_cast<u32>(std::min({framebuffer_limit, color_limit, depth_limit}));
  for (; samples > 1; samples /= 2)
    modes.insert(modes.begin(), samples);
  modes.insert(modes.begin(), 1);
}

void DetectSubgroupSupport()
{
  auto& gl = g_ogl_config;
  // The subgroup paths also use gl_HelperInvocation (GLSL 4.50 / ES 3.20).
  const bool helper_invocation =
      gl.bIsES ? gl.eSupportedGLSLVersion >= GlslEs320 : gl.eSupportedGLSLVersion >= Glsl450;
  gl.bSupportsKHRShaderSubgroup = Has("GL_KHR_shader_subgroup") && helper_invocation;
  if (!gl.bSupportsKHRShaderSubgroup)
    return;

  constexpr GLint needed = GL_SUBGROUP_FEATURE_BASIC_BIT_KHR |
                           GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR |
                           GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR;
  GLint features = 0;
  glGetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &features);
  if ((features & needed) != needed)
    gl.bSupportsKHRShaderSubgroup = false;
}

void ReportMissingFeatures(bool program_binaries)
{
  const auto& info = g_backend_info;
  const auto& gl = g_ogl_config;
  const std::pair<bool, const char*> checks[] = {
      {info.bSupportsDualSourceBlend, "DualSourceBlend "},
      {info.bSupportsPrimitiveRestart, "PrimitiveRestart "},
      {info.bSupportsEarlyZ, "EarlyZ "},
      {gl.bSupportsGLPinnedMemory, "PinnedMemory "},
      {program_binaries, "ShaderCache "},
      {gl.bSupportsGLBaseVertex, "BaseVertex "},
      {gl.bSupportsGLBufferStorage, "BufferStorage "},
      {gl.bSupportsGLSync, "Sync "},
      {gl.bSupportsMSAA, "MSAA "},
      {info.bSupportsSSAA, "SSAA "},
      {info.bSupportsGSInstancing, "GSInstancing "},
      {info.bSupportsClipControl, "ClipControl "},
      {gl.bSupportsCopySubImage, "CopyImageSubData "},
      {info.bSupportsDepthClamp, "DepthClamp "},
  };
  std::string missing;
  for (const auto& [present, label] : checks)
  {
    if (!present)
      missing += label;
  }

  if (missing.empty())
    INFO_LOG_FMT(VIDEO, "All used OGL Extensions are available.");
  else
    WARN_LOG_FMT(VIDEO, "Missing OGL Extensions: {}", missing);
}
}  // namespace

void InitDriverInfo()
{
  const char* vendor = g_ogl_config.gl_vendor;
  const char* renderer = g_ogl_config.gl_renderer;
  const char* version = g_ogl_config.gl_version;

  DriverId id;
  IdentifyVendor(vendor, renderer, version, &id);

  switch (id.vendor)
  {
  case DriverDetails::VENDOR_QUALCOMM:
    ReadQualcommVersion(version, &id);
    break;
  case DriverDetails::VENDOR_ARM:
    ReadMaliVersion(&id);
    break;
  case DriverDetails::VENDOR_MESA:
    ReadMesaDetails(vendor, renderer, version, &id);
    break;
  case DriverDetails::VENDOR_INTEL:
    ReadIntelDetails(renderer, version, &id);
    break;
  case DriverDetails::VENDOR_NVIDIA:
    ReadNvidiaVersion(version, &id);
    break;
  case DriverDetails::VENDOR_IMGTEC:
    ReadImgTecVersion(version, &id);
    break;
  default:
    // Nothing else is needed for the remaining vendors.
    break;
  }

  DriverDetails::Init(DriverDetails::API_OPENGL, id.vendor, id.driver, id.version, id.family,
                      std::string(renderer));
}

bool PopulateConfig(GLContext* main_gl_context)
{
  auto& info = g_backend_info;
  auto& gl = g_ogl_config;
  const bool is_es = main_gl_context->IsGLES();

  gl.gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  gl.gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  gl.gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));

  bool ok = is_es || CheckDesktopRequirements();

  // Analytics reads the adapter name from here.
  info.AdapterName = gl.gl_renderer;

  bool program_binaries = false;
  ReadExtensionFeatures(is_es, &program_binaries);

  if (is_es)
    ApplyESProfile(&program_binaries);
  else if (!ApplyDesktopProfile())
    ok = false;

  // Layer selection in the pixel shader: every ES with geometry shaders, desktop 4.3+.
  info.bSupportsGLLayerInFS = info.bSupportsGeometryShaders && gl.eSupportedGLSLVersion >= Glsl430;
  info.bSupportsBBox = info.bSupportsFragmentStoresAndAtomics;
  // Early depth tests work through either qualifier (see the pixel shader generator).
  info.bSupportsEarlyZ = gl.bSupportsImageLoadStore || gl.bSupportsConservativeDepth;

  DetectAAModes();
  DetectSubgroupSupport();

  // GPU texture decoding needs texel buffers, compute shaders and image stores.
  info.bSupportsGPUTextureDecoding =
      info.bSupportsPaletteConversion && info.bSupportsComputeShaders && gl.bSupportsImageLoadStore;
  info.bSupportsBackgroundCompiling = !Bug(DriverDetails::BUG_SHARED_CONTEXT_SHADER_COMPILATION);

  // Program binaries are only worth it when the driver has at least one binary format.
  if (program_binaries)
  {
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    program_binaries = formats > 0;
  }
  info.bSupportsPipelineCacheData = program_binaries;

  // Nothing is drawn multisampled into the window, and blitting into a multisampled default
  // framebuffer fails, so a driver that forces MSAA on it is a fatal setup problem.
  GLint window_samples = 0;
  glGetIntegerv(GL_SAMPLES, &window_samples);
  if (window_samples > 1)
  {
    PanicAlertFmtT(
        "The graphics driver is forcibly enabling anti-aliasing for Dolphin. You need to "
        "turn this off in the graphics driver's settings in order for Dolphin to work.\n\n"
        "(MSAA with {0} samples found on default framebuffer)",
        window_samples);
    ok = false;
  }

  if (!ok)
    return false;

  g_Config.VerifyValidity();
  UpdateActiveConfig();

  OSD::AddMessage(
      fmt::format("Video Info: {}, {}, {}", gl.gl_vendor, gl.gl_renderer, gl.gl_version), 5000);
  if (!gl.bSupportsGLBufferStorage && !gl.bSupportsGLPinnedMemory)
  {
    OSD::AddMessage(fmt::format("Your OpenGL driver does not support {}_buffer_storage.",
                                is_es ? "EXT" : "ARB"),
                    60000);
    OSD::AddMessage("This device's performance may be poor.", 60000);
  }
  INFO_LOG_FMT(VIDEO, "Video Info: {}, {}, {}", gl.gl_vendor, gl.gl_renderer, gl.gl_version);

  ReportMissingFeatures(program_binaries);
  return true;
}

}  // namespace OGL
