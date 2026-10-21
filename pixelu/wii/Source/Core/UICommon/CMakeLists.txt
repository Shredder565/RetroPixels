# UICommon: what every front end shares (command line, game list cache and
# covers, resource packs, Discord presence, screen saver control, user folders).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(uicommon
  IN .
    AutoUpdate.cpp AutoUpdate.h CommandLineParse.cpp CommandLineParse.h DiscordPresence.cpp
    DiscordPresence.h GameFile.cpp GameFile.h GameFileCache.cpp GameFileCache.h
    NetPlayIndex.cpp NetPlayIndex.h UICommon.cpp UICommon.h
  IN ResourcePack
    Manager.cpp Manager.h Manifest.cpp Manifest.h ResourcePack.cpp ResourcePack.h
)

target_link_libraries(uicommon
  PUBLIC
    common
    core
    cpp-optparse
    MINIZIP::minizip-ng
    pugixml
  PRIVATE
    fmt::fmt
    bdisasm
)

if(TARGET LibUSB::LibUSB)
  target_link_libraries(uicommon PRIVATE LibUSB::LibUSB)
endif()

if(USE_DISCORD_PRESENCE)
  target_compile_definitions(uicommon PRIVATE USE_DISCORD_PRESENCE)
  target_link_libraries(uicommon PRIVATE discord-rpc)
endif()

pixelu_use_pch(uicommon)
