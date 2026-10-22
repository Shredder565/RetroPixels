# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# The software renderer: a CPU implementation of the GX pipeline (transform, clipping,
# rasterization, TEV, EFB and EFB copies) that presents through a small OpenGL window.

set(VIDEOSOFTWARE_PIPELINE_SOURCES
  NativeVertexFormat.h
  SWVertexLoader.cpp SWVertexLoader.h
  TransformUnit.cpp TransformUnit.h
  SetupUnit.cpp SetupUnit.h
  Clipper.cpp Clipper.h
  Rasterizer.cpp Rasterizer.h
  Tev.cpp Tev.h
  TextureSampler.cpp TextureSampler.h
)

set(VIDEOSOFTWARE_EFB_SOURCES
  SWEfbInterface.cpp SWEfbInterface.h
  SWBoundingBox.cpp SWBoundingBox.h
  EfbCopy.cpp EfbCopy.h
  TextureEncoder.cpp TextureEncoder.h
  CopyRegion.h
)

set(VIDEOSOFTWARE_DEVICE_SOURCES
  SWmain.cpp VideoBackend.h
  SWGfx.cpp SWGfx.h
  SWTexture.cpp SWTexture.h
  SWOGLWindow.cpp SWOGLWindow.h
)

add_library(videosoftware
  ${VIDEOSOFTWARE_PIPELINE_SOURCES}
  ${VIDEOSOFTWARE_EFB_SOURCES}
  ${VIDEOSOFTWARE_DEVICE_SOURCES}
)

target_link_libraries(videosoftware PUBLIC common videocommon)

# MSVC builds share the precompiled header target.
if(MSVC)
  target_link_libraries(videosoftware PRIVATE use_pch)
endif()
