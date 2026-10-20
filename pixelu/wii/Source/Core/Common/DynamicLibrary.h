// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <string>

namespace Common
{
// Owns a handle to a loaded shared library (DLL / .so) and looks up symbols in it. The library
// is unloaded when the object is destroyed or closed.
class DynamicLibrary final
{
public:
  DynamicLibrary();
  // Loads `filename`; check IsOpen() for success.
  DynamicLibrary(const char* filename);
  // Takes ownership of an already loaded library handle.
  DynamicLibrary(void* handle);
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(DynamicLibrary&&) = delete;

  // Takes ownership of an already loaded library handle (without closing the current one).
  DynamicLibrary& operator=(void*);

  // "name" -> "name.dll" / "name.so" / "name.dylib".
  static std::string GetUnprefixedFilename(const char* filename);

  // The platform's file name for a library, with optional version numbers (-1 = none):
  //   Windows: name-MAJOR-MINOR.dll
  //   Linux:   libname.so.MAJOR.MINOR
  //   macOS:   libname.MAJOR.MINOR.dylib
  // "lib" is not prefixed twice.
  static std::string GetVersionedFilename(const char* libname, int major = -1, int minor = -1);

  bool IsOpen() const { return m_handle != nullptr; }

  // Loads `filename`, replacing whatever was loaded before. Returns IsOpen().
  bool Open(const char* filename);

  // Unloads the library; symbols obtained from it become invalid.
  void Close();

  // The address of symbol `name`, or nullptr.
  void* GetSymbolAddress(const char* name) const;

  // Looks up `name` and stores it, cast to T, in *ptr. Returns whether it was found.
  template <typename T>
  bool GetSymbol(const char* name, T* ptr) const
  {
    *ptr = reinterpret_cast<T>(GetSymbolAddress(name));
    return *ptr != nullptr;
  }

private:
  void* m_handle = nullptr;  // HMODULE or dlopen() handle
};

}  // namespace Common
