// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/JitRegister.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef USE_VTUNE
#include <jitprofiling.h>
#pragma comment(lib, "jitprofiling.lib")
#endif

namespace Common::JitRegister
{
namespace
{
// perf's map file format: one "<start address hex> <size hex> <symbol name>" line per symbol.
File::IOFile s_perf_map;
bool s_enabled = false;
}  // namespace

void Init(const std::string& perf_dir)
{
#ifdef USE_VTUNE
  s_enabled = true;
#endif

  const bool want_perf_map = !perf_dir.empty() || std::getenv("PERF_BUILDID_DIR") != nullptr;
  if (!want_perf_map)
    return;

  const std::string path =
      fmt::format("{}/perf-{}.map", perf_dir.empty() ? std::string("/tmp") : perf_dir, getpid());
  if (!s_perf_map.Open(path, "w"))
    return;

  // Unbuffered, so that the entries written so far survive a crash.
  std::setvbuf(s_perf_map.GetHandle(), nullptr, _IONBF, 0);
  s_enabled = true;
}

void Shutdown()
{
#ifdef USE_VTUNE
  iJIT_NotifyEvent(iJVM_EVENT_TYPE_SHUTDOWN, nullptr);
#endif

  if (s_perf_map.IsOpen())
    s_perf_map.Close();
  s_enabled = false;
}

bool IsEnabled()
{
  return s_enabled;
}

void Register(const void* base_address, u32 code_size, const std::string& symbol_name)
{
#ifdef USE_VTUNE
  iJIT_Method_Load method{};
  method.method_id = iJIT_GetNewMethodID();
  method.method_load_address = const_cast<void*>(base_address);
  method.method_size = code_size;
  method.method_name = const_cast<char*>(symbol_name.c_str());
  iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, &method);
#endif

  // Stop at the first write error.
  if (!s_perf_map)
    return;

  const std::string line =
      fmt::format("{} {:x} {}\n", fmt::ptr(base_address), code_size, symbol_name);
  s_perf_map.WriteBytes(line.data(), line.size());
}
}  // namespace Common::JitRegister
