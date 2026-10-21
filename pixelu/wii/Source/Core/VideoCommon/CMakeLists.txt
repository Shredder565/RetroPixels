# VideoCommon: the GPU emulation shared by all video backends (command
# processing, vertex loading, texture decoding and caching, shader generation,
# EFB copies, post-processing, on-screen display, graphics mods, frame dumps).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(videocommon
  IN .
    AbstractFramebuffer.cpp AbstractFramebuffer.h AbstractGfx.cpp AbstractGfx.h
    AbstractShader.h AbstractStagingTexture.cpp AbstractStagingTexture.h AbstractTexture.cpp
    AbstractTexture.h AsyncRequests.cpp AsyncRequests.h AsyncShaderCompiler.cpp
    AsyncShaderCompiler.h BoundingBox.cpp BoundingBox.h BPFunctions.cpp BPFunctions.h
    BPMemory.cpp BPMemory.h BPStructs.cpp BPStructs.h CommandProcessor.cpp CommandProcessor.h
    ConstantManager.h Constants.h CPMemory.cpp CPMemory.h CPUCull.cpp CPUCull.h CPUCullImpl.h
    DriverDetails.cpp DriverDetails.h EFBInterface.cpp EFBInterface.h Fifo.cpp Fifo.h
    FramebufferManager.cpp FramebufferManager.h FramebufferShaderGen.cpp FramebufferShaderGen.h
    FrameDumper.cpp FrameDumper.h FrameDumpFFMpeg.h FreeLookCamera.cpp FreeLookCamera.h
    GeometryShaderGen.cpp GeometryShaderGen.h GeometryShaderManager.cpp GeometryShaderManager.h
    HiresTextures.cpp HiresTextures.h IndexGenerator.cpp IndexGenerator.h LightingShaderGen.cpp
    LightingShaderGen.h LookUpTables.h NativeVertexFormat.h NetPlayChatUI.cpp NetPlayChatUI.h
    NetPlayGolfUI.cpp NetPlayGolfUI.h OnScreenDisplay.cpp OnScreenDisplay.h OnScreenUI.cpp
    OnScreenUI.h OnScreenUIKeyMap.h OpcodeDecoding.cpp OpcodeDecoding.h PerformanceMetrics.cpp
    PerformanceMetrics.h PerformanceTracker.cpp PerformanceTracker.h PerfQueryBase.cpp
    PerfQueryBase.h PipelineUtils.cpp PipelineUtils.h PixelEngine.cpp PixelEngine.h
    PixelShaderGen.cpp PixelShaderGen.h PixelShaderManager.cpp PixelShaderManager.h
    PostProcessing.cpp PostProcessing.h Present.cpp Present.h RenderState.cpp RenderState.h
    ShaderCache.cpp ShaderCache.h ShaderCompileUtils.cpp ShaderCompileUtils.h
    ShaderGenCommon.cpp ShaderGenCommon.h Spirv.cpp Spirv.h Statistics.cpp Statistics.h
    TextureCacheBase.cpp TextureCacheBase.h TextureConfig.cpp TextureConfig.h
    TextureConversionShader.cpp TextureConversionShader.h TextureConverterShaderGen.cpp
    TextureConverterShaderGen.h TextureDecoder.h TextureDecoder_Common.cpp
    TextureDecoder_Util.h TextureDecoder_x64.cpp TextureInfo.cpp TextureInfo.h TextureUtils.cpp
    TextureUtils.h TMEM.cpp TMEM.h UberShaderCommon.cpp UberShaderCommon.h UberShaderPixel.cpp
    UberShaderPixel.h UberShaderVertex.cpp UberShaderVertex.h VertexLoader.cpp VertexLoader.h
    VertexLoader_Color.cpp VertexLoader_Color.h VertexLoader_Normal.cpp VertexLoader_Normal.h
    VertexLoader_Position.cpp VertexLoader_Position.h VertexLoader_TextCoord.cpp
    VertexLoader_TextCoord.h VertexLoaderBase.cpp VertexLoaderBase.h VertexLoaderManager.cpp
    VertexLoaderManager.h VertexLoaderUtils.h VertexLoaderX64.cpp VertexLoaderX64.h
    VertexManagerBase.cpp VertexManagerBase.h VertexShaderGen.cpp VertexShaderGen.h
    VertexShaderManager.cpp VertexShaderManager.h VideoBackendBase.cpp VideoBackendBase.h
    VideoCommon.h VideoConfig.cpp VideoConfig.h VideoEvents.h VideoState.cpp VideoState.h
    Widescreen.cpp Widescreen.h XFMemory.cpp XFMemory.h XFStateManager.cpp XFStateManager.h
    XFStructs.cpp XFStructs.h
  IN Assets
    AssetListener.h CustomAsset.cpp CustomAsset.h CustomAssetCache.cpp CustomAssetCache.h
    CustomAssetLibrary.h CustomAssetLoader.cpp CustomAssetLoader.h CustomTextureData.cpp
    CustomTextureData.h DirectFilesystemAssetLibrary.cpp DirectFilesystemAssetLibrary.h
    MaterialAsset.cpp MaterialAsset.h MeshAsset.cpp MeshAsset.h ShaderAsset.cpp ShaderAsset.h
    TextureAsset.cpp TextureAsset.h TextureAssetUtils.cpp TextureAssetUtils.h
    TextureSamplerValue.cpp TextureSamplerValue.h Types.h WatchableFilesystemAssetLibrary.h
  IN GraphicsModSystem
    Constants.h
  IN GraphicsModSystem/Config
    GraphicsMod.cpp GraphicsMod.h GraphicsModAsset.cpp GraphicsModAsset.h
    GraphicsModFeature.cpp GraphicsModFeature.h GraphicsModGroup.cpp GraphicsModGroup.h
    GraphicsTarget.cpp GraphicsTarget.h GraphicsTargetGroup.cpp GraphicsTargetGroup.h
  IN GraphicsModSystem/Runtime
    CustomPipeline.cpp CustomPipeline.h CustomShaderCache.cpp CustomShaderCache.h FBInfo.cpp
    FBInfo.h GraphicsModAction.h GraphicsModActionData.h GraphicsModActionFactory.cpp
    GraphicsModActionFactory.h GraphicsModManager.cpp GraphicsModManager.h
  IN GraphicsModSystem/Runtime/Actions
    CustomPipelineAction.cpp CustomPipelineAction.h MoveAction.cpp MoveAction.h PrintAction.cpp
    PrintAction.h ScaleAction.cpp ScaleAction.h SkipAction.cpp SkipAction.h
  IN Resources
    CustomResourceManager.cpp CustomResourceManager.h InvalidTextures.cpp InvalidTextures.h
    MaterialResource.cpp MaterialResource.h Resource.cpp Resource.h ShaderResource.cpp
    ShaderResource.h TextureAndSamplerResource.cpp TextureAndSamplerResource.h
    TextureDataResource.cpp TextureDataResource.h TexturePool.cpp TexturePool.h
)

target_link_libraries(videocommon
  PUBLIC
    core
    glslang::glslang
  PRIVATE
    fmt::fmt
    spng::spng
    xxhash::xxhash
    imgui
    implot
    glslang::SPIRV
    tinygltf
)

if(FFmpeg_FOUND)
  # Frame dumps encoded to video.
  pixelu_add_sources(videocommon
    IN .
      FrameDumpFFMpeg.cpp
  )
  target_link_libraries(videocommon PRIVATE
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swresample
    FFmpeg::swscale
  )
endif()

pixelu_use_pch(videocommon)
