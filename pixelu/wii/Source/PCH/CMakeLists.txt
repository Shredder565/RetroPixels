# One MSVC precompiled header for all of PixelU-Wii's modules.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# CMake's target_precompile_headers builds a PCH per target. Here every module
# shares a single pch.h compiled once by build_pch: a target opts in by linking
# use_pch (see pixelu_use_pch), which force-includes pch.h, uses the shared .pch
# file and orders the build after it.

set(pch_file "$<TARGET_FILE_DIR:build_pch>/dolphin.pch")

add_library(build_pch pch.h pch.cpp)
# pch.h pulls in fmt.
target_link_libraries(build_pch PUBLIC fmt::fmt)
# pch.cpp creates the PCH; everyone, build_pch included, names the same file.
target_compile_options(build_pch
  PRIVATE /Ycpch.h
  PUBLIC /Fp${pch_file}
)

# Object files that share a PCH cannot share one .pdb, so debug information is
# embedded in the objects (/Z7) for all targets from here on, and minimal
# rebuild (which needs a .pdb) is off.
set(embedded_debug_info "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
set_property(TARGET build_pch PROPERTY MSVC_DEBUG_INFORMATION_FORMAT "${embedded_debug_info}")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "${embedded_debug_info}" PARENT_SCOPE)
target_compile_options(build_pch PUBLIC "$<$<CONFIG:Debug,RelWithDebInfo>:/Gm->")

# Tell single-step generators that compiling pch.cpp also writes the .pch, and
# give them a target to wait for.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp" PROPERTIES OBJECT_OUTPUTS "${pch_file}")
add_custom_target(force_build_pch DEPENDS "${pch_file}")

add_library(use_pch INTERFACE)
target_link_libraries(use_pch INTERFACE build_pch)
target_compile_options(use_pch INTERFACE /Yupch.h /FIpch.h)
add_dependencies(use_pch force_build_pch)
