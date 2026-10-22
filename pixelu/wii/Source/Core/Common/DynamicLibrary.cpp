// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/DynamicLibrary.h"

#include <string>
#include <string_view>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Common
{
namespace
{
void* LoadNative(const char* filename)
{
#ifdef _WIN32
  return reinterpret_cast<void*>(LoadLibraryA(filename));
#else
  return dlopen(filename, RTLD_NOW);
#endif
}

void FreeNative(void* handle)
{
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

// "-1-2" style (Windows) or ".1.2" style version suffix; empty pieces for negative numbers.
std::string VersionSuffix(char separator, int major, int minor)
{
  std::string suffix;
  if (major >= 0)
  {
    suffix += fmt::format("{}{}", separator, major);
    if (minor >= 0)
      suffix += fmt::format("{}{}", separator, minor);
  }
  return suffix;
}

[[maybe_unused]] std::string_view LibPrefix(std::string_view name)
{
  return name.starts_with("lib") ? "" : "lib";
}
}  // namespace

DynamicLibrary::DynamicLibrary() = default;

DynamicLibrary::DynamicLibrary(const char* filename)
{
  Open(filename);
}

DynamicLibrary::DynamicLibrary(void* handle) : m_handle(handle)
{
}

DynamicLibrary::~DynamicLibrary()
{
  Close();
}

DynamicLibrary& DynamicLibrary::operator=(void* handle)
{
  m_handle = handle;
  return *this;
}

std::string DynamicLibrary::GetUnprefixedFilename(const char* filename)
{
#if defined(_WIN32)
  constexpr std::string_view extension = ".dll";
#elif defined(__APPLE__)
  constexpr std::string_view extension = ".dylib";
#else
  constexpr std::string_view extension = ".so";
#endif
  return std::string(filename).append(extension);
}

std::string DynamicLibrary::GetVersionedFilename(const char* libname, int major, int minor)
{
#if defined(_WIN32)
  return fmt::format("{}{}.dll", libname, VersionSuffix('-', major, minor));
#elif defined(__APPLE__)
  return fmt::format("{}{}{}.dylib", LibPrefix(libname), libname, VersionSuffix('.', major, minor));
#else
  return fmt::format("{}{}.so{}", LibPrefix(libname), libname, VersionSuffix('.', major, minor));
#endif
}

bool DynamicLibrary::Open(const char* filename)
{
  // Load the new library before letting go of the old one, so re-opening the same library does
  // not unload and reload it.
  void* const handle = LoadNative(filename);
  Close();
  m_handle = handle;
  return IsOpen();
}

void DynamicLibrary::Close()
{
  if (m_handle == nullptr)
    return;
  FreeNative(m_handle);
  m_handle = nullptr;
}

void* DynamicLibrary::GetSymbolAddress(const char* name) const
{
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
  return dlsym(m_handle, name);
#endif
}
}  // namespace Common
