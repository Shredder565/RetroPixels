# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# The null video backend: VideoCommon runs as usual but nothing is drawn, read back or shown.

add_library(videonull
  VideoBackend.h NullBackend.cpp
  NullGfx.cpp NullGfx.h
  NullTexture.cpp NullTexture.h
  NullVertexManager.cpp NullVertexManager.h
  NullBoundingBox.h
  PerfQuery.h
  TextureCache.h
)

target_link_libraries(videonull PUBLIC common videocommon)

# MSVC builds share the precompiled header target.
if(MSVC)
  target_link_libraries(videonull PRIVATE use_pch)
endif()
