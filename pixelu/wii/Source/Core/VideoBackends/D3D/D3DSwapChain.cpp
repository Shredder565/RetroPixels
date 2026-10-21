// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DSwapChain.h"

#include "Common/Assert.h"

#include "VideoBackends/D3D/DXTexture.h"

namespace DX11
{
SwapChain::SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory,
                     ID3D11Device* d3d_device)
    : D3DCommon::SwapChain(wsi, dxgi_factory, d3d_device)
{
}

SwapChain::~SwapChain() = default;

std::unique_ptr<SwapChain> SwapChain::Create(const WindowSystemInfo& wsi)
{
  auto chain = std::make_unique<SwapChain>(wsi, D3D::dxgi_factory.Get(), D3D::device.Get());
  if (!chain->CreateSwapChain(WantsStereo(), WantsHDR()))
    return nullptr;
  return chain;
}

bool SwapChain::CreateSwapChainBuffers()
{
  ComPtr<ID3D11Texture2D> buffer;
  const HRESULT result = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to get swap chain buffer: {}", DX11HRWrap(result));
  if (FAILED(result))
    return false;

  m_texture = DXTexture::CreateAdopted(std::move(buffer));
  if (!m_texture)
    return false;

  m_framebuffer = DXFramebuffer::Create(m_texture.get(), nullptr, {});
  return m_framebuffer != nullptr;
}

void SwapChain::DestroySwapChainBuffers()
{
  // The framebuffer's view refers to the texture, so it goes first.
  m_framebuffer.reset();
  m_texture.reset();
}
}  // namespace DX11
