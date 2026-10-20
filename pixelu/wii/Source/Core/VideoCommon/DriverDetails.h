// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

#undef OS  // some third-party headers (curl) define this

// Known host driver bugs, and which drivers have them. A backend calls Init once it knows its
// device; the rest of the video code asks HasBug before using a feature that is broken somewhere.
namespace DriverDetails
{
// Graphics APIs, as bit flags. Separate from VideoConfig's APIType because one entry can cover
// several APIs.
enum API
{
  API_OPENGL = (1 << 0),
  API_VULKAN = (1 << 1),
  API_METAL = (1 << 2),
};

// Host operating systems, as bit flags. OS_ALL is set for every OS.
enum OS
{
  OS_ALL = (1 << 0),
  OS_WINDOWS = (1 << 1),
  OS_LINUX = (1 << 2),
  OS_OSX = (1 << 3),
  OS_ANDROID = (1 << 4),
  OS_FREEBSD = (1 << 5),
  OS_OPENBSD = (1 << 6),
  OS_NETBSD = (1 << 7),
  OS_HAIKU = (1 << 8),
};

// GPU vendors. Tegra is listed apart from desktop Nvidia.
enum Vendor
{
  VENDOR_ALL = 0,
  VENDOR_NVIDIA,
  VENDOR_ATI,
  VENDOR_INTEL,
  VENDOR_ARM,
  VENDOR_QUALCOMM,
  VENDOR_IMGTEC,
  VENDOR_TEGRA,
  VENDOR_VIVANTE,
  VENDOR_MESA,
  VENDOR_APPLE,
  VENDOR_UNKNOWN
};

// Driver stacks. Unknown means "assume the vendor's own driver".
enum Driver
{
  DRIVER_ALL = 0,
  DRIVER_NVIDIA,       // Nvidia's driver (desktop and mobile)
  DRIVER_NOUVEAU,      // Mesa nouveau
  DRIVER_ATI,          // AMD's driver
  DRIVER_R600,         // Mesa radeon
  DRIVER_INTEL,        // Intel's driver
  DRIVER_I965,         // Mesa Intel
  DRIVER_ARM,          // ARM's Mali driver
  DRIVER_LIMA,         // Mesa Mali
  DRIVER_QUALCOMM,     // Qualcomm's Adreno driver
  DRIVER_FREEDRENO,    // Mesa Adreno
  DRIVER_IMGTEC,       // Imagination's PowerVR driver
  DRIVER_VIVANTE,      // Vivante's driver
  DRIVER_PORTABILITY,  // Vulkan layered on Metal (macOS)
  DRIVER_APPLE,        // Apple's Metal driver
  DRIVER_UNKNOWN
};

enum class Family
{
  UNKNOWN,
  INTEL_SANDY,
  INTEL_IVY,
};

// Each value names one workaround the video code applies when the driver has the problem.
enum Bug
{
  // Uniform buffer ranges ignore their offset or truncate it (old Mesa).
  BUG_BROKEN_UBO,
  // Pinned-memory index buffers render wrongly together with base_vertex (AMD, Linux).
  BUG_BROKEN_PINNED_MEMORY,
  // Buffer updates through glBufferSubData/glMapBufferRange stall (some mobile drivers).
  BUG_BROKEN_BUFFER_STREAM,
  // ARB_buffer_storage is broken for vertex array buffers (old Nvidia, Linux).
  BUG_BROKEN_BUFFER_STORAGE,
  // Primitive restart does not work.
  BUG_PRIMITIVE_RESTART,
  // Unsynchronised buffer mapping is slow or ignored, so another streaming path is used.
  BUG_BROKEN_UNSYNC_MAPPING,
  // buffer_storage is broken for element array buffers (one range of Intel Windows drivers).
  BUG_INTEL_BROKEN_BUFFER_STORAGE,
  // "!b" on a bool miscompiles to a bitwise not, so shaders compare with false instead.
  BUG_BROKEN_NEGATED_BOOLEAN,
  // glCopyImageSubData honours the scissor test (old Mesa).
  BUG_BROKEN_COPYIMAGE,
  // Turning vsync off stops the display from updating, so vsync stays on.
  BUG_BROKEN_VSYNC,
  // Geometry shaders draw lines wrongly.
  BUG_BROKEN_GEOMETRY_SHADERS,
  // glGetBufferSubData is slow for bounding box readback; a mapped read is used instead.
  BUG_SLOW_GETBUFFERSUBDATA,
  // Writing gl_ClipDistance in both vertex and geometry stages breaks the geometry shader.
  BUG_BROKEN_CLIP_DISTANCE,
  // Dual-source blending outputs crash or misbehave; separate shaders are built with and
  // without them.
  BUG_BROKEN_DUAL_SOURCE_BLENDING,
  // Negating the operand of a shift or bitwise op fails to compile; a temporary is used.
  BUG_BROKEN_BITWISE_OP_NEGATION,
  // Shaders compiled on a shared context are recompiled or hang, so no async compiling.
  BUG_SHARED_CONTEXT_SHADER_COMPILATION,
  // Partial clears of multisampled targets can hang the GPU; clears are drawn instead.
  BUG_BROKEN_MSAA_CLEAR,
  // Render passes with a clear load op are not compatible with the others.
  BUG_BROKEN_CLEAR_LOADOP_RENDERPASS,
  // Clearing a D32F target has no effect, so D24_S8 is used for the EFB depth buffer.
  BUG_BROKEN_D32F_CLEAR,
  // Viewports with min depth above max depth are mishandled.
  BUG_BROKEN_REVERSED_DEPTH_RANGE,
  // Cached host memory is slow for readback; coherent memory is chosen instead.
  BUG_SLOW_CACHED_READBACK_MEMORY,
  // "&" between two integer vectors returns the constant operand; done per component instead.
  BUG_BROKEN_VECTOR_BITWISE_AND,
  // Subgroup max operations return garbage in shaders that can discard.
  BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD,
  // gl_HelperInvocation has the opposite value.
  BUG_INVERTED_IS_HELPER,
  // Compiling shaders on several threads at once can crash.
  BUG_BROKEN_MULTITHREADED_SHADER_PRECOMPILATION,
  // Discarded fragments come out corrupted when early depth testing is on.
  BUG_BROKEN_DISCARD_WITH_EARLY_Z,
  // Indexing samplers dynamically hangs the GPU.
  BUG_BROKEN_DYNAMIC_SAMPLER_INDEXING,
  // Copying an optimally tiled image to a buffer goes through a slow staging image.
  BUG_SLOW_OPTIMAL_IMAGE_TO_BUFFER_COPY,
  // VK_EXT_depth_clamp_control writes wrong depth values.
  BUG_BROKEN_DEPTH_CLAMP_CONTROL
};

// Records the device in use and works out which bugs apply to it.
void Init(API api, Vendor vendor, Driver driver, const double version, const Family family,
          std::string name);

// True when the device given to Init has the bug (or it was overridden on).
bool HasBug(Bug bug);

// Forces a bug on or off for the current device.
void OverrideBug(Bug bug, bool new_value);
}  // namespace DriverDetails
