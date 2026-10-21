# The Direct3D 11 video backend.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(videod3d
  IN .
    D3DBase.cpp D3DBase.h D3DBoundingBox.cpp D3DBoundingBox.h D3DGfx.cpp D3DGfx.h D3DMain.cpp
    D3DNativeVertexFormat.cpp D3DPerfQuery.cpp D3DPerfQuery.h D3DState.cpp D3DState.h
    D3DSwapChain.cpp D3DVertexManager.cpp D3DVertexManager.h DXPipeline.cpp DXPipeline.h
    DXShader.cpp DXShader.h DXTexture.cpp DXTexture.h VideoBackend.h
)
target_link_libraries(videod3d PUBLIC common videocommon videod3dcommon)
pixelu_use_pch(videod3d)
