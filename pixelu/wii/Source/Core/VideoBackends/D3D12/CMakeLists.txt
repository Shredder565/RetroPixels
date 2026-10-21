# The Direct3D 12 video backend.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(videod3d12
  IN .
    D3D12BoundingBox.cpp D3D12BoundingBox.h D3D12Gfx.cpp D3D12Gfx.h D3D12PerfQuery.cpp
    D3D12PerfQuery.h D3D12StreamBuffer.cpp D3D12StreamBuffer.h D3D12SwapChain.cpp
    D3D12SwapChain.h D3D12VertexManager.cpp D3D12VertexManager.h DescriptorAllocator.cpp
    DescriptorAllocator.h DescriptorHeapManager.cpp DescriptorHeapManager.h DX12Context.cpp
    DX12Context.h DX12Pipeline.cpp DX12Pipeline.h DX12Shader.cpp DX12Shader.h DX12Texture.cpp
    DX12Texture.h DX12VertexFormat.cpp DX12VertexFormat.h VideoBackend.cpp VideoBackend.h
)
target_link_libraries(videod3d12 PUBLIC common videocommon videod3dcommon)
pixelu_use_pch(videod3d12)
