// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <dxgi1_5.h>
#include <wrl/client.h>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/TextureConfig.h"

namespace D3DCommon
{
// The DXGI swap chain both D3D backends present through. Owns the DXGI object
// and the window/stereo/HDR/fullscreen choices; the backend subclass owns the
// views or descriptors of the back buffers.
class SwapChain
{
public:
  SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory, IUnknown* d3d_device);
  virtual ~SwapChain();

  // Three back buffers.
  static const u32 SWAP_CHAIN_BUFFER_COUNT = 3;

  // Whether the configuration asks for quad-buffered stereo.
  static bool WantsStereo();

  // Whether the configuration asks for HDR output.
  static bool WantsHDR();

  IDXGISwapChain* GetDXGISwapChain() const { return m_swap_chain.Get(); }
  AbstractTextureFormat GetFormat() const
  {
    return m_hdr ? m_texture_format_hdr : m_texture_format;
  }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }

  bool GetFullscreen() const;
  void SetFullscreen(bool request);

  // Returns true when exclusive fullscreen was gained or lost since the last call.
  bool CheckForFullscreenChange();

  virtual bool Present();

  // Recreates the swap chain for a new window.
  bool ChangeSurface(void* native_handle);
  // Resizes the buffers to the window's current size.
  bool ResizeSwapChain();
  void SetStereo(bool stereo);
  void SetHDR(bool hdr);

protected:
  u32 GetSwapChainFlags() const;
  bool CreateSwapChain(bool stereo = false, bool hdr = false);
  void DestroySwapChain();

  virtual bool CreateSwapChainBuffers() = 0;
  virtual void DestroySwapChainBuffers() = 0;

  WindowSystemInfo m_wsi;
  Microsoft::WRL::ComPtr<IDXGIFactory> m_dxgi_factory;
  Microsoft::WRL::ComPtr<IDXGISwapChain> m_swap_chain;
  Microsoft::WRL::ComPtr<IUnknown> m_d3d_device;
  const AbstractTextureFormat m_texture_format = AbstractTextureFormat::RGB10_A2;
  const AbstractTextureFormat m_texture_format_hdr = AbstractTextureFormat::RGBA16F;

  u32 m_width = 1;
  u32 m_height = 1;

  bool m_stereo = false;
  bool m_hdr = false;
  bool m_allow_tearing_supported = false;
  bool m_has_fullscreen = false;
  bool m_fullscreen_request = false;

private:
  HWND Window() const;
  HRESULT CreateFlipModel(bool stereo);
  HRESULT CreateBitBltModel();
  bool TryEnableHDR();
  void LeaveFullscreen();
  bool RecreateWith(bool stereo, bool hdr);
};

}  // namespace D3DCommon
