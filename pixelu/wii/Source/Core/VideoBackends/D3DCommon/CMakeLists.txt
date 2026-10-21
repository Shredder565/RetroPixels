# What the Direct3D 11 and 12 backends share: DXGI swap chains and HLSL shader
# compilation (SPIR-V translated to HLSL with SPIRV-Cross).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(videod3dcommon
  IN .
    D3DCommon.cpp D3DCommon.h Shader.cpp Shader.h SwapChain.cpp SwapChain.h
)
target_link_libraries(videod3dcommon PUBLIC common videocommon spirv_cross)
pixelu_use_pch(videod3dcommon)
