// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

// Tells profilers about JIT-generated code: Linux perf through a /tmp/perf-<pid>.map symbol file
// and, in builds with USE_VTUNE, Intel VTune through its JIT profiling API.
namespace Common::JitRegister
{
// Starts writing the perf map into perf_dir (or /tmp when perf_dir is empty but the
// PERF_BUILDID_DIR environment variable is set).
void Init(const std::string& perf_dir);
void Shutdown();
void Register(const void* base_address, u32 code_size, const std::string& symbol_name);
bool IsEnabled();

// The name is only formatted when somebody is listening.
template <typename... Args>
inline void Register(const void* base_address, u32 code_size, fmt::format_string<Args...> format,
                     Args&&... args)
{
  if (IsEnabled())
    Register(base_address, code_size, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Register(const void* start, const void* end, fmt::format_string<Args...> format,
                     Args&&... args)
{
  if (!IsEnabled())
    return;
  const auto size = static_cast<u32>(static_cast<const u8*>(end) - static_cast<const u8*>(start));
  Register(start, size, fmt::format(format, std::forward<Args>(args)...));
}
}  // namespace Common::JitRegister
