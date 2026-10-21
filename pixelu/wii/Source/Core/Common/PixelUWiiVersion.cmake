# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# PixelU-Wii's version header, Common/scmrev.h, made from scmrev.h.in.
#
# PixelU-Wii carries RetroPixels' version (appVersion in RetroPixels' main.go,
# found by walking up from the build tree) and RetroPixels' git commit and
# branch. Nothing here reads the Dolphin submodule's tags.
#
# Included from Common/CMakeLists.txt, this file adds the target
# pixelu_wii_version, which runs this same file as a script on every build so
# the commit in the header is always the one being built. Run as a script
# (cmake -P) it writes the header.
#
# Cache settings (all optional):
#   PIXELU_WII_VERSION   use this version instead of the one in main.go
#   PIXELU_WII_ROOT      the RetroPixels checkout (the folder with main.go)

# Servers that know only Dolphin releases see this version (in the HTTP user
# agent and the RetroAchievements integration): it is what PixelU-Wii builds
# sent until now, and those servers check it against Dolphin's release numbers.
set(PIXELU_WII_COMPAT_VERSION_DEFAULT "2609a-pixelu")

function(pixelu_wii_find_root start out)
  set(dir "${start}")
  while(TRUE)
    if(EXISTS "${dir}/main.go")
      file(STRINGS "${dir}/main.go" hit REGEX "appVersion[ \t]*=[ \t]*\"")
      if(hit)
        set(${out} "${dir}" PARENT_SCOPE)
        return()
      endif()
    endif()
    get_filename_component(parent "${dir}" DIRECTORY)
    if(parent STREQUAL dir)
      set(${out} "" PARENT_SCOPE)
      return()
    endif()
    set(dir "${parent}")
  endwhile()
endfunction()

function(pixelu_wii_read_version root out)
  set(${out} "" PARENT_SCOPE)
  if(root AND EXISTS "${root}/main.go")
    file(STRINGS "${root}/main.go" hit REGEX "appVersion[ \t]*=[ \t]*\"[^\"]+\"")
    if(hit)
      list(GET hit 0 first)
      string(REGEX REPLACE ".*appVersion[ \t]*=[ \t]*\"([^\"]+)\".*" "\\1" ver "${first}")
      set(${out} "${ver}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

if(NOT CMAKE_SCRIPT_MODE_FILE)
  # ---- configure time: work out the inputs and add the target ----
  if(NOT PIXELU_WII_ROOT)
    pixelu_wii_find_root("${CMAKE_SOURCE_DIR}" found_root)
    set(PIXELU_WII_ROOT "${found_root}" CACHE PATH "RetroPixels checkout (the folder with main.go)")
  endif()
  set(PIXELU_WII_VERSION "" CACHE STRING "PixelU-Wii version; empty = RetroPixels' appVersion")
  if(PIXELU_WII_VERSION)
    set(pixelu_wii_ver "${PIXELU_WII_VERSION}")
  else()
    pixelu_wii_read_version("${PIXELU_WII_ROOT}" pixelu_wii_ver)
  endif()
  if(NOT pixelu_wii_ver)
    message(FATAL_ERROR "PixelU-Wii: no version. Set PIXELU_WII_ROOT to the RetroPixels checkout "
                        "(the folder with main.go) or PIXELU_WII_VERSION.")
  endif()
  message(STATUS "PixelU-Wii version ${pixelu_wii_ver} (from ${PIXELU_WII_ROOT})")

  find_package(Git QUIET)
  set(pixelu_wii_header "${PROJECT_BINARY_DIR}/Source/Core/Common/scmrev.h")
  # The header has to exist before the first build for dependency scanning.
  if(NOT EXISTS "${pixelu_wii_header}")
    file(WRITE "${pixelu_wii_header}" "")
  endif()
  add_custom_target(pixelu_wii_version
    COMMAND "${CMAKE_COMMAND}"
            "-DPIXELU_WII_ROOT=${PIXELU_WII_ROOT}"
            "-DPIXELU_WII_VER=${pixelu_wii_ver}"
            "-DPIXELU_WII_COMPAT=${PIXELU_WII_COMPAT_VERSION_DEFAULT}"
            "-DPIXELU_WII_GIT=${GIT_EXECUTABLE}"
            "-DPIXELU_WII_TEMPLATE=${CMAKE_CURRENT_LIST_DIR}/scmrev.h.in"
            "-DPIXELU_WII_OUT=${pixelu_wii_header}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "PixelU-Wii version header"
    VERBATIM)
  return()
endif()

# ---- script mode: write the header ----
set(PIXELU_WII_COMMIT "")
set(PIXELU_WII_BRANCH "")
if(PIXELU_WII_GIT AND PIXELU_WII_ROOT)
  execute_process(COMMAND "${PIXELU_WII_GIT}" -C "${PIXELU_WII_ROOT}" rev-parse HEAD
                  OUTPUT_VARIABLE PIXELU_WII_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${PIXELU_WII_GIT}" -C "${PIXELU_WII_ROOT}" rev-parse --abbrev-ref HEAD
                  OUTPUT_VARIABLE PIXELU_WII_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
# A detached checkout (as in CI) has no branch name; CI says which one it is.
if(PIXELU_WII_BRANCH STREQUAL "" OR PIXELU_WII_BRANCH STREQUAL "HEAD")
  if(DEFINED ENV{GITHUB_REF_NAME})
    set(PIXELU_WII_BRANCH "$ENV{GITHUB_REF_NAME}")
  else()
    set(PIXELU_WII_BRANCH "unknown")
  endif()
endif()
# Without git the version stands in for the commit (shader caches and netplay
# then match on the version alone).
if(NOT PIXELU_WII_COMMIT)
  set(PIXELU_WII_COMMIT "${PIXELU_WII_VER}")
endif()

set(tmp "${PIXELU_WII_OUT}.tmp")
configure_file("${PIXELU_WII_TEMPLATE}" "${tmp}" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${tmp}" "${PIXELU_WII_OUT}")
file(REMOVE "${tmp}")
