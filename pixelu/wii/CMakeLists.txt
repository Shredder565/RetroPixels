# PixelU-Wii: PixelU's GameCube and Wii engine (PixelU\wii\PixelU-Wii.exe).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# This is the whole build: PixelU-Wii's own code in Source/, plus third-party
# libraries built from their own sources. Externals/fetch.py fetches those
# sources from each library's own upstream project into third_party/ (the list,
# with versions, commits and checksums, is Externals/sources.json); Externals/
# holds PixelU's build files for them. Qt and FFmpeg come from upstream too:
# Qt's official build (installed with aqtinstall) and FFmpeg built from its
# source (Externals/FFmpeg/build-ffmpeg.sh); see .github/workflows/pixelu-wii.yml.
# Only the game settings (Data/Sys/GameSettings, ApprovedInis.json) and the
# translations (Languages/po) still come from a checkout of Dolphin 2609a's
# repository, by default a sparse one in dolphin/.
#
# Supported: Windows x64 with MSVC (Visual Studio 2026), and Linux x86-64 with
# GCC 12+ (glibc 2.35 and newer; Clang 19+ should work too); see CMakePresets.json.

cmake_minimum_required(VERSION 3.25...4.2.1)

set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PixelUFlagsOverride.cmake")
# No C++20 modules here: skip the dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

project(pixelu-wii C CXX)

set(PIXELU_WII_CMAKE_DIR "${PROJECT_SOURCE_DIR}/CMake")
list(APPEND CMAKE_MODULE_PATH "${PIXELU_WII_CMAKE_DIR}")
include(GNUInstallDirs)
include(PixelUHelpers)

########################################
# Where things come from

set(PIXELU_WII_THIRD_PARTY "${PROJECT_SOURCE_DIR}/third_party" CACHE PATH
  "Folder with the third-party library sources (what Externals/fetch.py fetches)")
if(WIN32)
  set(qt_default "${PIXELU_WII_THIRD_PARTY}/Qt/6.8.3/msvc2022_64")
else()
  set(qt_default "${PIXELU_WII_THIRD_PARTY}/Qt/6.8.3/gcc_64")
endif()
set(PIXELU_WII_QT_DIR "${qt_default}" CACHE PATH
  "Qt 6.8.3 (the official MSVC x64 or Linux gcc_64 build, as aqtinstall installs it)")
set(PIXELU_WII_FFMPEG_DIR "${PIXELU_WII_THIRD_PARTY}/FFmpeg/x64" CACHE PATH
  "FFmpeg static libraries and headers (what Externals/FFmpeg/build-ffmpeg.sh installs)")
set(PIXELU_WII_DOLPHIN_DATA "${PROJECT_SOURCE_DIR}/dolphin" CACHE PATH
  "Folder with Dolphin 2609a's Data/Sys/GameSettings, Data/Sys/ApprovedInis.json and Languages/po")

foreach(dir "${PIXELU_WII_THIRD_PARTY}/fmt/fmt" "${PIXELU_WII_DOLPHIN_DATA}/Data/Sys")
  if(NOT EXISTS "${dir}")
    message(FATAL_ERROR "${dir} is missing. Run Externals/fetch.py and fetch Dolphin's game "
      "settings and translations (see .github/workflows/pixelu-wii.yml), or point "
      "PIXELU_WII_THIRD_PARTY / PIXELU_WII_DOLPHIN_DATA somewhere else.")
  endif()
endforeach()
# Third-party sources that are fetched but older than sources.json say so here.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Externals/fetch.py" --check
            --dest "${PIXELU_WII_THIRD_PARTY}"
    RESULT_VARIABLE fetch_check OUTPUT_VARIABLE fetch_check_out ERROR_VARIABLE fetch_check_out)
  if(NOT fetch_check EQUAL 0)
    message(WARNING "${fetch_check_out}Run Externals/fetch.py to bring third_party up to date.")
  endif()
endif()
set(THIRD "${PIXELU_WII_THIRD_PARTY}")

########################################
# Options

if(WIN32)
  set(on_windows ON)
  set(on_linux OFF)
else()
  set(on_windows OFF)
  set(on_linux ON)
endif()

option(ENABLE_QT "Build the Qt program (PixelU-Wii.exe)" ON)
option(ENABLE_NOGUI "Build dolphin-emu-nogui, the program without a user interface" ${on_linux})
option(ENABLE_CLI_TOOL "Also configure the dolphin-tool command-line program" ON)
option(ENABLE_VULKAN "Vulkan video backend" ON)
# On Linux cubeb needs its PulseAudio/ALSA build (Externals/cubeb); until then
# the Null sound backend is the only one there.
option(ENABLE_CUBEB "Cubeb sound backend and microphone input" ${on_windows})
option(ENABLE_SDL "SDL controller input" ON)
option(ENABLE_LLVM "Use LLVM, when its CMake package is found, for the debugger's host disassembly" ON)
option(ENABLE_LTO "Link-time optimisation" OFF)
option(ENCODE_FRAMEDUMPS "Frame dumps encoded with FFmpeg" ON)
option(USE_UPNP "UPnP port forwarding for netplay" ON)
option(USE_DISCORD_PRESENCE "Discord Rich Presence" ON)
option(USE_MGBA "Game Boy Advance link emulation with mGBA" ON)
option(USE_RETRO_ACHIEVEMENTS "RetroAchievements" ON)
option(RC_CLIENT_SUPPORTS_RAINTEGRATION "RetroAchievements developer tools (RAIntegration, Windows only)" ${on_windows})
option(FASTLOG "Keep every log level in the build" OFF)
# PixelU-Wii never updates itself or sends usage statistics; both stay off.
option(ENABLE_AUTOUPDATE "Automatic updates (needs an update server; keep OFF)" OFF)
option(ENABLE_ANALYTICS "Opt-in usage statistics (keep OFF)" OFF)

########################################
# Compiler and target checks

if(NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  message(FATAL_ERROR "PixelU-Wii builds for Windows and Linux only (CMAKE_SYSTEM_NAME is ${CMAKE_SYSTEM_NAME}).")
endif()
if(WIN32 AND NOT MSVC)
  message(FATAL_ERROR "On Windows PixelU-Wii builds with MSVC only.")
endif()
if(RC_CLIENT_SUPPORTS_RAINTEGRATION AND NOT WIN32)
  message(STATUS "RAIntegration is Windows only; turned off")
  set(RC_CLIENT_SUPPORTS_RAINTEGRATION OFF CACHE BOOL "" FORCE)
endif()
if(NOT (CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$"))
  message(FATAL_ERROR "PixelU-Wii builds for x86-64 only, not for '${CMAKE_SYSTEM_PROCESSOR}' "
    "with ${CMAKE_SIZEOF_VOID_P}-byte pointers.")
endif()
message(STATUS "Using ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.32)
    message(FATAL_ERROR "PixelU-Wii needs MSVC 19.32 (Visual Studio 2022 17.2) or newer.")
  endif()
  if(DEFINED MSVC_TOOLSET_VERSION AND MSVC_TOOLSET_VERSION LESS 143)
    message(FATAL_ERROR "PixelU-Wii needs the v143 platform toolset or newer.")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
    message(FATAL_ERROR "PixelU-Wii needs GCC 12 or newer (C++23).")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
  # Older Clang cannot use libstdc++'s <expected> (it needs P2493 concepts).
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    message(FATAL_ERROR "PixelU-Wii needs Clang 19 or newer (C++23 std::expected).")
  endif()
elseif(NOT MSVC)
  message(FATAL_ERROR "PixelU-Wii builds with MSVC, GCC or Clang, not ${CMAKE_CXX_COMPILER_ID}.")
endif()

set(_ARCH_64 1)
set(_M_X86_64 1)
add_definitions(-D_ARCH_64=1 -D_M_X86_64=1)

########################################
# Build settings for everything (PixelU-Wii and the third-party libraries)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT dolphin-emu)

# The program and its DLLs go to <build>/Binaries, PDB files to <build>/pdb; the
# $<1:...> keeps multi-configuration generators from adding a Release/ folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/Binaries>")
set(CMAKE_PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>")

# Installed data folder; only used when no Sys folder sits next to the program.
set(datadir "${CMAKE_INSTALL_FULL_DATADIR}/pixelu-wii" CACHE PATH "datadir")
add_definitions(-DDATA_DIR="${datadir}/")

if(CMAKE_GENERATOR MATCHES "Visual Studio")
  # MSBuild compiles one project at a time unless each one compiles in parallel.
  add_compile_options(/MP)
endif()

if(MSVC)
  set(CMAKE_C_STANDARD 99)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  add_compile_options(/EHsc)
  add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)

  add_compile_options(
    /GR-     # no RTTI (DolphinQt turns it back on for Qt)
    /W4 /WX  # all warnings, as errors
    # ...except these, which the code base does not follow:
    /wd4201  # nameless struct/union
    /wd4127  # constant conditional expression
    /wd4100  # unreferenced parameter
    /wd4200  # zero-sized array in struct
    /wd4244  # narrowing conversion
    /wd4121  # alignment sensitive to packing
    /wd4324  # structure padded for alignment
    /wd4714  # __forceinline not inlined
    /wd4351  # array members default-initialised
    /wd4245  # signed/unsigned conversion
    # ...and these extra ones turned on:
    /w15262  # unannotated switch fall-through
    /w44263  # member function hides a virtual one
    /w44265  # virtual functions without a virtual destructor
    /w44946  # reinterpret_cast between related classes
    /utf-8
    # Headers included with <> are external: no warnings from them.
    /external:anglebrackets /external:W0 /external:templates-
    /experimental:deterministic
    /Gy      # function-level linking
    /Oi      # intrinsic functions
    $<IF:$<CONFIG:Debug>,/GS,/GS->
    /Zc:inline
    /Zc:__cplusplus,enumTypes,externConstexpr,preprocessor,templateScope,throwingNew
    /volatile:iso
    /fp:precise
  )
  add_link_options(/experimental:deterministic)
  string(APPEND CMAKE_EXE_LINKER_FLAGS " /NXCOMPAT /DEBUG /OPT:REF /OPT:ICF")
else()
  # GCC and Clang (Linux). The code base was written for MSVC: no type-based
  # aliasing assumptions, signed overflow never relied on, and warnings are not
  # errors here (the Windows build keeps /WX).
  set(CMAKE_C_STANDARD 99)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)
  add_compile_options(
    -fno-strict-aliasing
    -fwrapv
    -fvisibility-inlines-hidden
    # SSE4.1 and the others are chosen at run time (CPUDetect); this is the
    # x86-64 baseline the Windows build also assumes.
    -msse2
    -Wall
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-Wno-class-memaccess>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof>
  )
  if(CMAKE_GENERATOR MATCHES "Ninja")
    add_compile_options(-fdiagnostics-color=always)
  endif()
  # Programs find their bundled libraries (lib/ next to them) without
  # LD_LIBRARY_PATH, and keep working when the folder is moved.
  set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
  set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
  add_link_options(-Wl,--as-needed -Wl,-z,noexecstack)
endif()

if(ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT lto_ok OUTPUT lto_error)
  if(lto_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(STATUS "Link-time optimisation is not available: ${lto_error}")
  endif()
endif()

if(FASTLOG)
  add_definitions(-DDEBUGFAST)
endif()
if(ENABLE_ANALYTICS)
  add_definitions(-DUSE_ANALYTICS=1)
endif()
if(ENABLE_AUTOUPDATE)
  add_definitions(-DAUTOUPDATE=1)
endif()

find_package(Threads)

find_package(OpenGL)
if(OPENGL_GL)
  include_directories(${OPENGL_INCLUDE_DIR})
endif()

if(ENCODE_FRAMEDUMPS)
  set(FFMPEG_DIR "${PIXELU_WII_FFMPEG_DIR}")
  find_package(FFmpeg COMPONENTS avcodec avformat avutil swresample swscale)
  if(FFmpeg_FOUND)
    message(STATUS "FFmpeg found: frame dumps are encoded")
    add_definitions(-DHAVE_FFMPEG)
    if(WIN32)
      # FFmpeg's static libraries call into bcrypt (random numbers in libavutil).
      set_property(TARGET FFmpeg::avutil APPEND PROPERTY INTERFACE_LINK_LIBRARIES "Bcrypt.lib")
    endif()
  else()
    message(STATUS "FFmpeg not found: no frame dump encoding")
  endif()
endif()

# PixelU-Wii's headers come before every third-party folder.
include_directories(Source/Core)

########################################
# Third-party libraries (Externals/<name>/CMakeLists.txt builds ${THIRD}/<name>)
#
# The order matters a little: definitions and include folders added here reach
# the libraries added after them.

add_subdirectory(Externals/Bochs_disasm)
add_subdirectory(Externals/cpp-optparse)
add_subdirectory(Externals/fmt EXCLUDE_FROM_ALL)
add_subdirectory(Externals/imgui)
add_subdirectory(Externals/implot)
add_subdirectory(Externals/glslang EXCLUDE_FROM_ALL)
# GLSL to HLSL for the Direct3D backends.
add_subdirectory(Externals/spirv_cross)
add_subdirectory(Externals/tinygltf)

if(ENABLE_VULKAN)
  add_definitions(-DHAS_VULKAN)
endif()
add_definitions(-DHAS_OPENGL)

add_subdirectory(Externals/pugixml EXCLUDE_FROM_ALL)
add_subdirectory(Externals/enet EXCLUDE_FROM_ALL)
add_subdirectory(Externals/xxhash EXCLUDE_FROM_ALL)
add_subdirectory(Externals/bzip2 EXCLUDE_FROM_ALL)
add_subdirectory(Externals/liblzma EXCLUDE_FROM_ALL)
add_subdirectory(Externals/zstd EXCLUDE_FROM_ALL)
add_subdirectory(Externals/zlib-ng EXCLUDE_FROM_ALL)
add_subdirectory(Externals/minizip-ng EXCLUDE_FROM_ALL)
add_subdirectory(Externals/LZO EXCLUDE_FROM_ALL)
add_subdirectory(Externals/lz4 EXCLUDE_FROM_ALL)
add_subdirectory(Externals/libspng EXCLUDE_FROM_ALL)
add_subdirectory(Externals/FreeSurround)
add_subdirectory(Externals/ed25519)
include_directories("${THIRD}/ed25519/ed25519/src")

if(ENABLE_CUBEB)
  add_subdirectory(Externals/cubeb EXCLUDE_FROM_ALL)
  add_definitions(-DHAVE_CUBEB)
endif()

add_subdirectory(Externals/libusb EXCLUDE_FROM_ALL)
add_definitions(-D__LIBUSB__)

if(ENABLE_SDL)
  add_subdirectory(Externals/SDL EXCLUDE_FROM_ALL)
  add_definitions(-DHAVE_SDL3)
endif()

add_subdirectory(Externals/SFML EXCLUDE_FROM_ALL)

if(USE_UPNP)
  add_subdirectory(Externals/miniupnpc EXCLUDE_FROM_ALL)
  add_definitions(-DUSE_UPNP)
endif()

add_subdirectory(Externals/mbedtls EXCLUDE_FROM_ALL)
add_subdirectory(Externals/curl EXCLUDE_FROM_ALL)
add_subdirectory(Externals/hidapi EXCLUDE_FROM_ALL)

if(USE_DISCORD_PRESENCE)
  add_subdirectory(Externals/discord-rpc EXCLUDE_FROM_ALL)
  include_directories("${THIRD}/discord-rpc/discord-rpc/include")
endif()

if(USE_MGBA)
  add_subdirectory(Externals/mGBA EXCLUDE_FROM_ALL)
endif()

# Header-only pieces.
include_directories("${THIRD}/wil/include" "${THIRD}/OpenAL/openal-soft/include/AL" "${THIRD}/picojson/picojson")

add_subdirectory(Externals/expr)
add_subdirectory(Externals/FatFs)
if(USE_RETRO_ACHIEVEMENTS)
  add_subdirectory(Externals/rcheevos)
endif()
add_subdirectory(Externals/watcher)
add_subdirectory(Externals/cpp-ipc)
add_definitions(-DHAVE_CPPIPC)

########################################
# Generated headers: Common/scmrev.h (the version, written on every build by the
# pixelu_wii_version target of Source/Core/Common/PixelUWiiVersion.cmake) lives in
# <build>/Source/Core.

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/Source/Core/Common")
include_directories("${PROJECT_BINARY_DIR}/Source/Core")

########################################
# PixelU-Wii

add_subdirectory(Source)
