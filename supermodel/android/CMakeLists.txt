# RetroPixels for Android: Supermodel as libsupermodel.so (and SDL2 as
# libSDL2.so) for one Android ABI, with OpenGL ES 3.2. supermodel/build.sh
# android runs this once per ABI with the NDK's toolchain file; see
# docs/supermodel.md.
#
#   SM_SRC       the engine's code (default: supermodel/Src next to this)
#   SM_SDL_SRC   the pinned SDL2 source (pins.sh SDL2_SRC_*)
#   SM_VERSION   the version string save states and NVRAM carry
#
# With SM_HOST_GLES=ON it instead builds a Linux program, supermodel-gles,
# from the same sources and the system's SDL2 and Mesa's OpenGL ES, to test
# the OpenGL ES renderer on a PC (no Android code in it).
cmake_minimum_required(VERSION 3.18)
project(supermodel_android C CXX)

set(SM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../Src" CACHE PATH "the engine's code (supermodel/Src)")
set(SM_SDL_SRC "" CACHE PATH "the pinned SDL2 source")
set(SM_VERSION "rp-dev" CACHE STRING "the engine's version string")
option(SM_HOST_GLES "build supermodel-gles for Linux instead" OFF)

if(NOT EXISTS "${SM_SRC}/OSD/SDL/Main.cpp")
  message(FATAL_ERROR "SM_SRC must be supermodel/Src")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(S "${SM_SRC}")

# What the desktop build compiles (supermodel/Makefile, SOURCES), less
# GLEW, the settings window (Dear ImGui), networking (SDL_net), the legacy
# 3D engine and the debugger; plus the OpenGL ES shader translation and, on
# Android, OSD/Android.
set(SM_SOURCES
  CPU/PowerPC/PPCDisasm.cpp BlockFile.cpp Pkgs/unzip.c Pkgs/ioapi.c Model3/93C46.cpp Model3/JTAG.cpp
  Graphics/Shader.cpp Model3/Real3D.cpp
  Graphics/New3D/GLSLShader.cpp Graphics/New3D/R3DFrameBuffers.cpp Graphics/New3D/New3D.cpp
  Graphics/New3D/Mat4.cpp Graphics/New3D/Model.cpp Graphics/New3D/PolyHeader.cpp Graphics/New3D/VBO.cpp
  Graphics/New3D/Vec.cpp Graphics/New3D/R3DShader.cpp Graphics/New3D/R3DFloat.cpp
  Graphics/New3D/R3DScrollFog.cpp Graphics/New3D/TextureBank.cpp
  Graphics/FBO.cpp Graphics/Render2D.cpp Graphics/SuperAA.cpp Graphics/GLES/GLES.cpp
  Model3/TileGen.cpp Model3/Model3.cpp CPU/PowerPC/ppc.cpp
  OSD/SDL/Main.cpp OSD/SDL/Audio.cpp OSD/SDL/Thread.cpp OSD/RetroPixels.cpp
  Model3/SoundBoard.cpp Sound/SCSP.cpp Sound/SCSPDSP.cpp CPU/68K/68K.cpp CPU/68K/M68000.cpp
  Model3/DSB.cpp CPU/Z80/Z80.cpp Model3/IRQ.cpp Model3/53C810.cpp Model3/PCI.cpp Model3/RTC72421.cpp
  Model3/DriveBoard/DriveBoard.cpp Model3/DriveBoard/WheelBoard.cpp Model3/DriveBoard/JoystickBoard.cpp
  Model3/DriveBoard/SkiBoard.cpp Model3/DriveBoard/BillBoard.cpp Model3/DriveBoard/Z80CTC.cpp
  Model3/MPC10x.cpp
  Inputs/Input.cpp Inputs/Inputs.cpp Inputs/InputSource.cpp Inputs/InputSystem.cpp Inputs/InputTypes.cpp
  Inputs/MultiInputSource.cpp OSD/SDL/SDLInputSystem.cpp OSD/SDL/Crosshair.cpp OSD/Outputs.cpp
  Sound/MPEG/MpegAudio.cpp Model3/Crypto.cpp OSD/Logger.cpp
  Util/Format.cpp Util/NewConfig.cpp Util/ByteSwap.cpp Util/ConfigBuilders.cpp
  GameLoader.cpp Pkgs/tinyxml2.cpp ROMSet.cpp OSD/Unix/FileSystemPath.cpp)
if(ANDROID)
  list(APPEND SM_SOURCES OSD/Android/Android.cpp)
endif()
list(TRANSFORM SM_SOURCES PREPEND "${S}/")

# Every source folder is on the include path, as with the Makefile; the
# OpenGL ES stand-in for GLEW (Graphics/GLES/GL/glew.h) comes first.
set(SM_INCLUDES "${S}/Graphics/GLES" "${S}")
foreach(f IN LISTS SM_SOURCES)
  get_filename_component(d "${f}" DIRECTORY)
  list(APPEND SM_INCLUDES "${d}")
endforeach()
list(REMOVE_DUPLICATES SM_INCLUDES)

set(SM_DEFINES SUPERMODEL_GLES SUPERMODEL_NO_NET SUPERMODEL_NO_GUI "SUPERMODEL_VERSION=\"${SM_VERSION}\"")

if(SM_HOST_GLES)
  find_package(SDL2 REQUIRED)
  find_package(ZLIB REQUIRED)
  add_executable(supermodel-gles ${SM_SOURCES})
  target_include_directories(supermodel-gles PRIVATE ${SM_INCLUDES} ${SDL2_INCLUDE_DIRS})
  target_compile_definitions(supermodel-gles PRIVATE ${SM_DEFINES})
  target_link_libraries(supermodel-gles PRIVATE ${SDL2_LIBRARIES} GLESv2 ZLIB::ZLIB pthread m)
  return()
endif()

if(NOT ANDROID)
  message(FATAL_ERROR "use the NDK's toolchain file (or SM_HOST_GLES=ON)")
endif()
if(NOT EXISTS "${SM_SDL_SRC}/CMakeLists.txt")
  message(FATAL_ERROR "SM_SDL_SRC must be the SDL2 source")
endif()

# 16 KB pages (Android 15 devices), for SDL too; stripped (the app keeps
# them as they are)
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-z,max-page-size=16384 -s")

# SDL2 as the shared library SDLActivity loads. HIDAPI is built (SDL's Java
# code calls into it) but turned off when the program starts.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${SM_SDL_SRC}" sdl EXCLUDE_FROM_ALL)

add_library(supermodel SHARED ${SM_SOURCES})
target_include_directories(supermodel PRIVATE ${SM_INCLUDES})
target_compile_definitions(supermodel PRIVATE ${SM_DEFINES})
target_compile_options(supermodel PRIVATE -Wno-unused-parameter)
target_link_libraries(supermodel PRIVATE SDL2::SDL2 GLESv3 z log android m)
