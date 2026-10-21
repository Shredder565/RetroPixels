# The OpenGL video backend.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(videoogl
  IN .
    OGLBoundingBox.cpp OGLBoundingBox.h OGLConfig.cpp OGLConfig.h OGLGfx.cpp OGLGfx.h
    OGLMain.cpp OGLNativeVertexFormat.cpp OGLPerfQuery.cpp OGLPerfQuery.h OGLPipeline.cpp
    OGLPipeline.h OGLShader.cpp OGLShader.h OGLStreamBuffer.cpp OGLStreamBuffer.h
    OGLTexture.cpp OGLTexture.h OGLVertexManager.cpp OGLVertexManager.h ProgramShaderCache.cpp
    ProgramShaderCache.h SamplerCache.cpp SamplerCache.h VideoBackend.h
)
target_link_libraries(videoogl PUBLIC common videocommon)
pixelu_use_pch(videoogl)
