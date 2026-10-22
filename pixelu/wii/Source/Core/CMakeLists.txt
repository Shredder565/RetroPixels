# PixelU-Wii's modules. Original code for RetroPixels (PixelU-Wii).
# Updater and analytics are off in PixelU-Wii, so there is no WinUpdater here.

foreach(module AudioCommon Common Core DiscIO InputCommon UICommon VideoCommon VideoBackends)
  add_subdirectory(${module})
endforeach()

if(ENABLE_CLI_TOOL)
  add_subdirectory(DolphinTool)
endif()

if(ENABLE_QT)
  add_subdirectory(DolphinQt)
endif()

if(ENABLE_NOGUI)
  add_subdirectory(DolphinNoGUI)
endif()

add_subdirectory(UpdaterCommon)
