# dolphin-emu-nogui: PixelU-Wii without a user interface (headless start-up checks,
# tests, frame dumps, or one plain X11 window with -p x11). Built on Linux by default
# (ENABLE_NOGUI).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

add_executable(dolphin-emu-nogui)
pixelu_add_sources(dolphin-emu-nogui
  IN .
    MainNoGUI.cpp Platform.h PlatformHeadless.cpp
)
set_target_properties(dolphin-emu-nogui PROPERTIES DEBUG_POSTFIX D)

target_link_libraries(dolphin-emu-nogui PRIVATE
  core
  uicommon
  cpp-optparse
)

# The X11 platform (-p x11); ENABLE_X11 is InputCommon's option (libx11-dev).
if(NOT WIN32 AND ENABLE_X11)
  find_package(X11 REQUIRED)
  pixelu_add_sources(dolphin-emu-nogui
    IN .
      PlatformX11.cpp
  )
  target_link_libraries(dolphin-emu-nogui PRIVATE X11::X11)
  target_compile_definitions(dolphin-emu-nogui PRIVATE PIXELU_NOGUI_X11=1)
endif()

pixelu_use_pch(dolphin-emu-nogui)
