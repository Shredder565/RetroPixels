# Video backends. Original code for RetroPixels (PixelU-Wii).
# OpenGL, null, software and the Direct3D 11/12 backends are always built;
# Vulkan when ENABLE_VULKAN is on.

foreach(backend OGL Null Software D3DCommon D3D D3D12)
  add_subdirectory(${backend})
endforeach()

if(ENABLE_VULKAN)
  add_subdirectory(Vulkan)
endif()
