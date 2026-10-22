# PixelU-Wii's own sources. Original code for RetroPixels (PixelU-Wii).

if(WIN32)
  # Windows headers: no min/max macros, wide-character APIs, the lean windows.h,
  # and no deprecation warnings for the classic C runtime functions.
  add_compile_definitions(
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _SCL_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE
    _CRT_NONSTDC_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
  )
endif()

if(MSVC)
  # One precompiled header shared by every module (see PCH/CMakeLists.txt).
  add_subdirectory(PCH)
endif()

# Some headers (LLVM's, FFmpeg's) still look for the C99 limit/constant macros.
add_compile_definitions(__STDC_LIMIT_MACROS __STDC_CONSTANT_MACROS)

add_subdirectory(Core)
