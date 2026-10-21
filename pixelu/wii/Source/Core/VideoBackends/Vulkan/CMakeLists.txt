# The Vulkan video backend. The Vulkan loader is opened at run time, so only the
# Vulkan and Vulkan Memory Allocator headers are needed, and only here.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(videovulkan
  IN .
    CommandBufferManager.cpp CommandBufferManager.h Constants.h ObjectCache.cpp ObjectCache.h
    ShaderCompiler.cpp ShaderCompiler.h StagingBuffer.cpp StagingBuffer.h StateTracker.cpp
    StateTracker.h VKBoundingBox.cpp VKBoundingBox.h VKGfx.cpp VKGfx.h VKMain.cpp
    VKPerfQuery.cpp VKPerfQuery.h VKPipeline.cpp VKPipeline.h VKShader.cpp VKShader.h
    VKStreamBuffer.cpp VKStreamBuffer.h VKSwapChain.cpp VKSwapChain.h VKTexture.cpp VKTexture.h
    VKVertexFormat.cpp VKVertexFormat.h VKVertexManager.cpp VKVertexManager.h VulkanContext.cpp
    VulkanContext.h VulkanLoader.cpp VulkanLoader.h
)

target_link_libraries(videovulkan
  PUBLIC
    common
    videocommon
  PRIVATE
    xxhash::xxhash
)
target_include_directories(videovulkan PRIVATE
  "${THIRD}/Vulkan-Headers/include"
  "${THIRD}/VulkanMemoryAllocator/include"
)

pixelu_use_pch(videovulkan)
