# dolphin-emu-nogui: PixelU-Wii without a user interface (headless start-up checks,
# tests, frame dumps). Built on Linux by default (ENABLE_NOGUI).
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

pixelu_use_pch(dolphin-emu-nogui)
