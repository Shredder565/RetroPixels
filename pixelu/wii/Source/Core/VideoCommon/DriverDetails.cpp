// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/DriverDetails.h"

#include <array>
#include <map>
#include <string_view>
#include <utility>

#include "Core/DolphinAnalytics.h"

#ifdef __APPLE__
#include "Common/CommonFuncs.h"
#endif

namespace DriverDetails
{
namespace
{
#if defined(_WIN32)
constexpr u32 HOST_OS = OS_WINDOWS;
#elif defined(ANDROID)
constexpr u32 HOST_OS = OS_ANDROID;
#elif defined(__APPLE__)
constexpr u32 HOST_OS = OS_OSX;
#elif defined(__linux__)
constexpr u32 HOST_OS = OS_LINUX;
#elif defined(__FreeBSD__)
constexpr u32 HOST_OS = OS_FREEBSD;
#elif defined(__OpenBSD__)
constexpr u32 HOST_OS = OS_OPENBSD;
#elif defined(__NetBSD__)
constexpr u32 HOST_OS = OS_NETBSD;
#elif defined(__HAIKU__)
constexpr u32 HOST_OS = OS_HAIKU;
#endif
// An entry's OS mask matches when it shares a bit with this.
constexpr u32 HOST_OS_MASK = OS_ALL | HOST_OS;

// Version bound that means "no bound".
constexpr double ANY = -1.0;

constexpr u32 GL = API_OPENGL;
constexpr u32 VK = API_VULKAN;
constexpr u32 MTL = API_METAL;

// One affected configuration: the bug applies when the API, OS, vendor, driver and family
// match and from <= version < until.
struct Affected
{
  Bug bug;
  u32 apis;
  u32 oses;
  Vendor vendor;
  Driver driver;
  Family family;
  double from;
  double until;
};

constexpr Family ANY_FAMILY = Family::UNKNOWN;

// clang-format off
constexpr Affected AFFECTED[] = {
  {BUG_BROKEN_UBO, GL, OS_ALL, VENDOR_MESA, DRIVER_NOUVEAU, ANY_FAMILY, 900, 916},
  {BUG_BROKEN_UBO, GL, OS_ALL, VENDOR_MESA, DRIVER_R600, ANY_FAMILY, 900, 913},
  {BUG_BROKEN_UBO, GL, OS_ALL, VENDOR_MESA, DRIVER_I965, ANY_FAMILY, 900, 920},

  {BUG_BROKEN_PINNED_MEMORY, GL, OS_LINUX, VENDOR_ATI, DRIVER_ATI, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_PINNED_MEMORY, GL, OS_ALL, VENDOR_MESA, DRIVER_R600, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_BUFFER_STREAM, GL, OS_ALL, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_BUFFER_STREAM, GL, OS_ALL, VENDOR_ARM, DRIVER_ARM, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_BUFFER_STREAM, GL, OS_ALL, VENDOR_IMGTEC, DRIVER_IMGTEC, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_BUFFER_STORAGE, GL, OS_LINUX, VENDOR_NVIDIA, DRIVER_NVIDIA, ANY_FAMILY, ANY, 33138.0},

  {BUG_PRIMITIVE_RESTART, GL, OS_OSX, VENDOR_INTEL, DRIVER_INTEL, Family::INTEL_SANDY, ANY, ANY},
  {BUG_PRIMITIVE_RESTART, VK, OS_WINDOWS, VENDOR_ATI, DRIVER_ATI, ANY_FAMILY, ANY, ANY},
  {BUG_PRIMITIVE_RESTART, VK, OS_ALL, VENDOR_ARM, DRIVER_ARM, ANY_FAMILY, ANY, ANY},
  {BUG_PRIMITIVE_RESTART, GL | VK, OS_ALL, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_UNSYNC_MAPPING, GL, OS_WINDOWS | OS_LINUX, VENDOR_NVIDIA, DRIVER_NVIDIA, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_UNSYNC_MAPPING, GL, OS_ALL, VENDOR_ARM, DRIVER_ARM, ANY_FAMILY, ANY, ANY},

  {BUG_INTEL_BROKEN_BUFFER_STORAGE, GL, OS_WINDOWS, VENDOR_INTEL, DRIVER_INTEL, ANY_FAMILY, 101810.3907, 101810.3960},

  {BUG_BROKEN_NEGATED_BOOLEAN, GL, OS_ALL, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_COPYIMAGE, GL, OS_ALL, VENDOR_MESA, DRIVER_ALL, ANY_FAMILY, ANY, 1064.0},

  {BUG_BROKEN_VSYNC, GL, OS_ALL, VENDOR_ARM, DRIVER_ARM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_GEOMETRY_SHADERS, GL, OS_ALL, VENDOR_MESA, DRIVER_R600, ANY_FAMILY, ANY, 1112.0},
  {BUG_BROKEN_GEOMETRY_SHADERS, GL, OS_ALL, VENDOR_MESA, DRIVER_I965, Family::INTEL_SANDY, ANY, 1120.0},

  {BUG_SLOW_GETBUFFERSUBDATA, GL, OS_ALL, VENDOR_ATI, DRIVER_ATI, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_CLIP_DISTANCE, GL, OS_ALL, VENDOR_MESA, DRIVER_I965, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_DUAL_SOURCE_BLENDING, GL, OS_WINDOWS, VENDOR_ATI, DRIVER_ATI, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_DUAL_SOURCE_BLENDING, GL, OS_OSX, VENDOR_INTEL, DRIVER_INTEL, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_DUAL_SOURCE_BLENDING, VK, OS_OSX, VENDOR_INTEL, DRIVER_PORTABILITY, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_DUAL_SOURCE_BLENDING, MTL, OS_OSX, VENDOR_INTEL, DRIVER_APPLE, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_BITWISE_OP_NEGATION, GL, OS_ALL, VENDOR_IMGTEC, DRIVER_IMGTEC, ANY_FAMILY, ANY, 108.4693462},

  {BUG_SHARED_CONTEXT_SHADER_COMPILATION, GL, OS_LINUX, VENDOR_MESA, DRIVER_I965, ANY_FAMILY, ANY, ANY},
  {BUG_SHARED_CONTEXT_SHADER_COMPILATION, GL, OS_LINUX, VENDOR_MESA, DRIVER_NOUVEAU, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_MSAA_CLEAR, VK, OS_ALL, VENDOR_NVIDIA, DRIVER_NVIDIA, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_CLEAR_LOADOP_RENDERPASS, VK, OS_ALL, VENDOR_IMGTEC, DRIVER_IMGTEC, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_D32F_CLEAR, VK, OS_ALL, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_REVERSED_DEPTH_RANGE, VK, OS_ALL, VENDOR_MESA, DRIVER_I965, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_REVERSED_DEPTH_RANGE, VK, OS_ALL, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_REVERSED_DEPTH_RANGE, VK, OS_OSX, VENDOR_ALL, DRIVER_PORTABILITY, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_REVERSED_DEPTH_RANGE, MTL, OS_OSX, VENDOR_ALL, DRIVER_APPLE, ANY_FAMILY, ANY, ANY},

  {BUG_SLOW_CACHED_READBACK_MEMORY, VK, OS_ALL, VENDOR_ARM, DRIVER_ARM, ANY_FAMILY, ANY, ANY},
  {BUG_SLOW_CACHED_READBACK_MEMORY, VK, OS_ALL, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_VECTOR_BITWISE_AND, GL | VK, OS_ALL, VENDOR_ARM, DRIVER_ARM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD, VK, OS_OSX, VENDOR_INTEL, DRIVER_PORTABILITY, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD, MTL, OS_OSX, VENDOR_INTEL, DRIVER_APPLE, ANY_FAMILY, ANY, ANY},

  {BUG_INVERTED_IS_HELPER, VK, OS_OSX, VENDOR_ATI, DRIVER_PORTABILITY, ANY_FAMILY, ANY, ANY},
  {BUG_INVERTED_IS_HELPER, MTL, OS_OSX, VENDOR_ATI, DRIVER_APPLE, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_MULTITHREADED_SHADER_PRECOMPILATION, GL | VK, OS_ANDROID, VENDOR_ALL, DRIVER_ALL, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_DISCARD_WITH_EARLY_Z, VK, OS_OSX, VENDOR_APPLE, DRIVER_PORTABILITY, ANY_FAMILY, 1100, 1400},
  {BUG_BROKEN_DISCARD_WITH_EARLY_Z, MTL, OS_OSX, VENDOR_APPLE, DRIVER_APPLE, ANY_FAMILY, 1100, 1400},

  {BUG_BROKEN_DYNAMIC_SAMPLER_INDEXING, VK, OS_OSX, VENDOR_INTEL, DRIVER_PORTABILITY, ANY_FAMILY, ANY, ANY},
  {BUG_BROKEN_DYNAMIC_SAMPLER_INDEXING, MTL, OS_OSX, VENDOR_INTEL, DRIVER_APPLE, ANY_FAMILY, ANY, ANY},

  {BUG_SLOW_OPTIMAL_IMAGE_TO_BUFFER_COPY, VK, OS_ANDROID, VENDOR_QUALCOMM, DRIVER_QUALCOMM, ANY_FAMILY, ANY, ANY},

  {BUG_BROKEN_DEPTH_CLAMP_CONTROL, VK, OS_ALL, VENDOR_ATI, DRIVER_ATI, ANY_FAMILY, ANY, ANY},
};
// clang-format on

// The device given to Init.
struct Device
{
  API api = API_OPENGL;
  Vendor vendor = VENDOR_UNKNOWN;
  Driver driver = DRIVER_UNKNOWN;
  Family family = Family::UNKNOWN;
  double version = 0.0;
  std::string name;
};
Device s_device;

// Bugs known for the device: present with true when it has them, false when overridden off.
std::map<Bug, bool> s_bug_state;

// The vendor's own driver, for when the backend could not tell which driver is running.
Driver VendorDriver(Vendor vendor, Driver fallback)
{
  switch (vendor)
  {
  case VENDOR_NVIDIA:
  case VENDOR_TEGRA:
    return DRIVER_NVIDIA;
  case VENDOR_ATI:
    return DRIVER_ATI;
  case VENDOR_INTEL:
    return DRIVER_INTEL;
  case VENDOR_IMGTEC:
    return DRIVER_IMGTEC;
  case VENDOR_VIVANTE:
    return DRIVER_VIVANTE;
  default:
    return fallback;
  }
}

bool InRange(double version, double from, double until)
{
  const bool after_start = from == ANY || from <= version;
  const bool before_end = until == ANY || version < until;
  return after_start && before_end;
}

bool Applies(const Affected& entry, API api, const Device& dev)
{
  if ((entry.apis & static_cast<u32>(api)) == 0 || (entry.oses & HOST_OS_MASK) == 0)
    return false;
  if (entry.vendor != VENDOR_ALL && entry.vendor != dev.vendor)
    return false;
  if (entry.driver != DRIVER_ALL && entry.driver != dev.driver)
    return false;
  if (entry.family != Family::UNKNOWN && entry.family != dev.family)
    return false;
  return InRange(dev.version, entry.from, entry.until);
}

// Names used in analytics reports.
const char* ApiName(API api)
{
  switch (api)
  {
  case API_OPENGL:
    return "OpenGL";
  case API_VULKAN:
    return "Vulkan";
  case API_METAL:
    return "Metal";
  }
  return "Unknown";
}

const char* DriverName(Driver driver)
{
  static constexpr std::array<const char*, DRIVER_UNKNOWN + 1> names = {
      "All",       "Nvidia",  "Nouveau",     "ATI",   "R600",   "Intel",
      "I965",      "ARM",     "Lima",        "Qualcomm", "Freedreno", "Imgtech",
      "Vivante",   "Portability", "Apple",   "Unknown"};
  const auto index = static_cast<size_t>(driver);
  return index < names.size() ? names[index] : "Unknown";
}

const char* BugName(Bug bug)
{
  static constexpr std::array<const char*, BUG_BROKEN_DEPTH_CLAMP_CONTROL + 1> names = {
      "broken-ubo",
      "broken-pinned-memory",
      "broken-buffer-stream",
      "broken-buffer-storage",
      "primitive-restart",
      "broken-unsync-mapping",
      "intel-broken-buffer-storage",
      "broken-negated-boolean",
      "broken-copyimage",
      "broken-vsync",
      "broken-geometry-shaders",
      "slow-getBufferSubData",
      "broken-clip-distance",
      "broken-dual-source-blending",
      "broken-bitwise-op-negation",
      "shared-context-shader-compilation",
      "broken-msaa-clear",
      "broken-clear-loadop-renderpass",
      "broken-d32f-clear",
      "broken-reversed-depth-range",
      "slow-cached-readback-memory",
      "broken-vector-bitwise-and",
      "broken-subgroup-ops-with-discard",
      "inverted-is-helper",
      "broken-multithreaded-shader-precompilation",
      "broken-discard-with-early-z",
      "broken-dynamic-sampler-indexing",
      "slow-optimal-image-to-buffer-copy",
      "broken-depth-clamp-control",
  };
  const auto index = static_cast<size_t>(bug);
  return index < names.size() ? names[index] : "Unknown";
}
}  // namespace

void Init(API api, Vendor vendor, Driver driver, const double version, const Family family,
          std::string name)
{
  s_device.api = api;
  s_device.vendor = vendor;
  s_device.driver = driver == DRIVER_UNKNOWN ? VendorDriver(vendor, driver) : driver;
  s_device.family = family;
  s_device.version = version;
  s_device.name = std::move(name);

#ifdef __APPLE__
  // Metal drivers ship with the OS, so the macOS version stands in for the driver version.
  if (api == API_METAL || (api == API_VULKAN && driver == DRIVER_PORTABILITY))
  {
    const Common::MacOSVersion os_version = Common::GetMacOSVersion();
    s_device.version = os_version.major * 100 + os_version.minor;
  }
#endif

  // Start over; the API or device may have changed since the last call.
  s_bug_state.clear();
  for (const Affected& entry : AFFECTED)
  {
    if (Applies(entry, api, s_device))
      s_bug_state[entry.bug] = true;
  }
}

bool HasBug(Bug bug)
{
  const auto found = s_bug_state.find(bug);
  return found != s_bug_state.end() && found->second;
}

void OverrideBug(Bug bug, bool new_value)
{
  bool& state = s_bug_state.try_emplace(bug, false).first->second;
  if (state == new_value)
    return;

  DolphinAnalytics& analytics = DolphinAnalytics::Instance();
  Common::AnalyticsReportBuilder report(analytics.BaseBuilder());
  report.AddData("type", "gpu-bug-override");
  report.AddData("bug", BugName(bug));
  report.AddData("value", new_value);
  report.AddData("gpu", s_device.name);
  report.AddData("api", ApiName(s_device.api));
  report.AddData("driver", DriverName(s_device.driver));
  report.AddData("version", std::to_string(s_device.version));
  analytics.Send(report);

  state = new_value;
}
}  // namespace DriverDetails
