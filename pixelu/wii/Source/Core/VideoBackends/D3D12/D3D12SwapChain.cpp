// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/D3D12SwapChain.h"

#include "Common/Assert.h"

#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Texture.h"

namespace DX12
{
SwapChain::SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory,
                     ID3D12CommandQueue* d3d_command_queue)
    : D3DCommon::SwapChain(wsi, dxgi_factory, d3d_command_queue)
{
}

SwapChain::~SwapChain() = default;

std::unique_ptr<SwapChain> SwapChain::Create(const WindowSystemInfo& wsi)
{
  // D3D12 swap chains are created against the queue, not the device.
  auto chain = std::make_unique<SwapChain>(wsi, g_dx_context->GetDXGIFactory(),
                                           g_dx_context->GetCommandQueue());
  if (!chain->CreateSwapChain(WantsStereo(), WantsHDR()))
    return nullptr;
  return chain;
}

bool SwapChain::CreateSwapChainBuffers()
{
  for (u32 index = 0; index < SWAP_CHAIN_BUFFER_COUNT; ++index)
  {
    ComPtr<ID3D12Resource> back_buffer;
    const HRESULT hr = m_swap_chain->GetBuffer(index, IID_PPV_ARGS(&back_buffer));
    ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to get swap chain buffer {}: {}", index,
               DX12HRWrap(hr));
    if (FAILED(hr))
      return false;

    BufferResources entry;
    entry.texture = DXTexture::CreateAdopted(back_buffer.Get());
    ASSERT_MSG(VIDEO, entry.texture != nullptr, "Failed to create swap chain buffer texture");
    if (entry.texture == nullptr)
      return false;

    entry.framebuffer = DXFramebuffer::Create(entry.texture.get(), nullptr, {});
    ASSERT_MSG(VIDEO, entry.framebuffer != nullptr,
               "Failed to create swap chain buffer framebuffer");
    if (entry.framebuffer == nullptr)
      return false;

    m_buffers.push_back(std::move(entry));
  }

  m_current_buffer = 0;
  return true;
}

void SwapChain::DestroySwapChainBuffers()
{
  // DXGI only resizes once every buffer reference is gone, so the views and
  // resources are released now instead of after the GPU catches up.
  for (BufferResources& entry : m_buffers)
  {
    entry.framebuffer.reset();
    entry.texture->DestroyResource();
    entry.texture.reset();
  }
  m_buffers.clear();
}

bool SwapChain::Present()
{
  if (!D3DCommon::SwapChain::Present())
    return false;

  // Flip model hands out the buffers in order.
  m_current_buffer = (m_current_buffer + 1) % SWAP_CHAIN_BUFFER_COUNT;
  return true;
}
}  // namespace DX12
