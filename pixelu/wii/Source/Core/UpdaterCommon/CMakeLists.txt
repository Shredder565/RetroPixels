# UpdaterCommon: applying a signed update manifest. PixelU-Wii ships with the
# updater off (ENABLE_AUTOUPDATE), but UICommon's update check still links it.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(updatercommon
  IN .
    UI.h UpdaterCommon.cpp UpdaterCommon.h
)

target_link_libraries(updatercommon PRIVATE
  uicommon
  MbedTLS::mbedtls
  ZLIB::ZLIB
  ed25519
  cpp-optparse
)

pixelu_use_pch(updatercommon)
