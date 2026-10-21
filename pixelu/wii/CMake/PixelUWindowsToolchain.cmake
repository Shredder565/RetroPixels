# Original code for RetroPixels (PixelU-Wii).
#
# Toolchain file of the Windows x64 build (CMakePresets.json). It names the target
# explicitly (Windows on x86_64) instead of letting CMake take the host's
# description, keeps library and header searches inside CMAKE_SYSROOT when one is
# given, and points Qt's host tools (moc, rcc, uic) at the Qt the build uses.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(NOT QT_HOST_PATH)
  # The defaults of the top-level CMakeLists.txt, which has not run yet. On an
  # x64 host the target's Qt is also the host's; an ARM64 host needs its own Qt
  # for the tools, given with -DQT_HOST_PATH=<its folder>.
  if(DEFINED PIXELU_WII_QT_DIR)
    set(QT_HOST_PATH "${PIXELU_WII_QT_DIR}")
  else()
    if(NOT DEFINED PIXELU_WII_THIRD_PARTY)
      get_filename_component(PIXELU_WII_THIRD_PARTY "${CMAKE_CURRENT_LIST_DIR}/../third_party" ABSOLUTE)
    endif()
    set(QT_HOST_PATH "${PIXELU_WII_THIRD_PARTY}/Qt/6.8.3/msvc2022_64")
  endif()
endif()
