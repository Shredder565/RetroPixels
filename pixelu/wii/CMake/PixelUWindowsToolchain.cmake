# Original code for RetroPixels (PixelU-Wii).
#
# Toolchain file of the Windows x64 build (CMakePresets.json). It names the target
# explicitly (Windows on x86_64) instead of letting CMake take the host's
# description, keeps library and header searches inside CMAKE_SYSROOT when one is
# given, and points Qt's host tools (moc, rcc, uic) at the bundled Qt.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(NOT QT_HOST_PATH)
  if(NOT DEFINED PIXELU_WII_THIRD_PARTY)
    # The default of the top-level CMakeLists.txt, which has not run yet.
    get_filename_component(PIXELU_WII_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/../dolphin/Externals" ABSOLUTE)
  endif()
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(QT_HOST_PATH "${PIXELU_WII_THIRD_PARTY}/Qt/Qt6.8.3/ARM64")
  else()
    set(QT_HOST_PATH "${PIXELU_WII_THIRD_PARTY}/Qt/Qt6.8.3/x64")
  endif()
endif()
