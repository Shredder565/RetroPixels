# PixelU-Wii (PixelU-Wii.exe on Windows, PixelU-Wii on Linux): the program. It boots the game the
# command line names into a window of its own (Win32, or X11 on Linux) that RetroPixels docks,
# or into no window at all with -p headless (start-up checks, tests, frame dumps).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

add_executable(pixelu-wii)
pixelu_add_sources(pixelu-wii
  IN .
    Hotkeys.cpp Hotkeys.h MainNoGUI.cpp Platform.h PlatformHeadless.cpp
)
set_target_properties(pixelu-wii PROPERTIES
  OUTPUT_NAME PixelU-Wii
  DEBUG_POSTFIX D
)

target_link_libraries(pixelu-wii PRIVATE
  core
  uicommon
  cpp-optparse
)

if(WIN32)
  # A GUI program (no console window), with its icon and manifest.
  pixelu_add_sources(pixelu-wii
    IN .
      PlatformWin32.cpp PixelU-Wii.rc PixelU-Wii.manifest
  )
  set_target_properties(pixelu-wii PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_libraries(pixelu-wii PRIVATE user32.lib gdi32.lib)
elseif(ENABLE_X11)
  # The X11 platform (-p x11, the default when DISPLAY is set); ENABLE_X11 is InputCommon's
  # option (libx11-dev).
  find_package(X11 REQUIRED)
  pixelu_add_sources(pixelu-wii
    IN .
      PlatformX11.cpp
  )
  target_link_libraries(pixelu-wii PRIVATE X11::X11)
  target_compile_definitions(pixelu-wii PRIVATE PIXELU_NOGUI_X11=1)

  # pixelu-wii-dock-check: starts the program under an X server and checks that the game is
  # drawn in its window. Xlib only; not built unless asked for (ninja pixelu-wii-dock-check).
  add_executable(pixelu-wii-dock-check EXCLUDE_FROM_ALL)
  pixelu_add_sources(pixelu-wii-dock-check
    IN LinuxCheck
      DockCheck.cpp
  )
  target_link_libraries(pixelu-wii-dock-check PRIVATE X11::X11)
endif()

pixelu_use_pch(pixelu-wii)

########################################
# The runnable folder (build/Binaries): next to the program go Sys and the licence files.

set(bin_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# Sys is PixelU-Wii's own Data/Sys plus a short list of third-party files
# (CMake/PixelUAssembleSys.cmake), assembled fresh on every build.
add_custom_command(TARGET pixelu-wii POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
          "-DTHIRD_PARTY_SYS=${PIXELU_WII_DOLPHIN_DATA}/Data/Sys"
          "-DOWN_SYS=${PROJECT_SOURCE_DIR}/Data/Sys"
          "-DOUT=${bin_dir}/Sys"
          -P "${PIXELU_WII_CMAKE_DIR}/PixelUAssembleSys.cmake"
  COMMENT "Assembling Binaries/Sys"
  VERBATIM)
pixelu_after_build_link(pixelu-wii "${PROJECT_SOURCE_DIR}/COPYING" "${bin_dir}/COPYING")
pixelu_after_build_link(pixelu-wii "${PROJECT_SOURCE_DIR}/LICENSES" "${bin_dir}/Licenses")
if(ENABLE_AUTOUPDATE)
  pixelu_after_build_link(pixelu-wii "${CMAKE_BINARY_DIR}/build_info.txt" "${bin_dir}/build_info.txt")
endif()
