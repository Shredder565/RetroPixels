// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3DCommon/SwapChain.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/VideoConfig.h"

namespace
{
// DXGI 1.5 can report whether presents may tear (needed for an unlocked frame
// rate with flip-model swap chains).
bool FactorySupportsTearing(IDXGIFactory2* factory)
{
  Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
    return false;

  UINT supported = 0;
  const HRESULT result = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                       &supported, sizeof(supported));
  return SUCCEEDED(result) && supported != 0;
}

bool IsFullscreen(IDXGISwapChain* swap_chain)
{
  BOOL fullscreen = FALSE;
  if (FAILED(swap_chain->GetFullscreenState(&fullscreen, nullptr)))
    return false;
  return fullscreen != FALSE;
}
}  // namespace

namespace D3DCommon
{
SwapChain::SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory, IUnknown* d3d_device)
    : m_wsi(wsi), m_dxgi_factory(dxgi_factory), m_d3d_device(d3d_device)
{
}

SwapChain::~SwapChain()
{
  LeaveFullscreen();
}

bool SwapChain::WantsStereo()
{
  return g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer;
}

bool SwapChain::WantsHDR()
{
  return g_ActiveConfig.bHDR;
}

u32 SwapChain::GetSwapChainFlags() const
{
  // Flip-model chains need this flag to present without waiting for vblank.
  if (!m_allow_tearing_supported)
    return 0;
  return DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
}

HWND SwapChain::Window() const
{
  return static_cast<HWND>(m_wsi.render_surface);
}

void SwapChain::LeaveFullscreen()
{
  // DXGI refuses to release a chain that is still in exclusive fullscreen.
  if (m_swap_chain && IsFullscreen(m_swap_chain.Get()))
    m_swap_chain->SetFullscreenState(FALSE, nullptr);
}

HRESULT SwapChain::CreateFlipModel(bool stereo)
{
  // Needs DXGI 1.2 (Windows 8).
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
  HRESULT result = m_dxgi_factory.As(&factory2);
  if (FAILED(result))
    return result;

  m_allow_tearing_supported = FactorySupportsTearing(factory2.Get());

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = m_width;
  desc.Height = m_height;
  desc.Format = GetDXGIFormatForAbstractFormat(m_texture_format, false);
  desc.Stereo = stereo;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.Flags = GetSwapChainFlags();

  // Windows 8 has no FLIP_DISCARD; FLIP_SEQUENTIAL is the second choice.
  constexpr std::array<DXGI_SWAP_EFFECT, 2> effects = {DXGI_SWAP_EFFECT_FLIP_DISCARD,
                                                       DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
  Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
  for (const DXGI_SWAP_EFFECT effect : effects)
  {
    desc.SwapEffect = effect;
    result = factory2->CreateSwapChainForHwnd(m_d3d_device.Get(), Window(), &desc, nullptr, nullptr,
                                              &created);
    if (SUCCEEDED(result))
      break;
  }

  m_swap_chain = created;
  return result;
}

HRESULT SwapChain::CreateBitBltModel()
{
  // Windows 7 path. D3D12 never gets here in practice: a system without
  // DXGI 1.2 has no D3D12 either.
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferDesc.Width = m_width;
  desc.BufferDesc.Height = m_height;
  desc.BufferDesc.Format = GetDXGIFormatForAbstractFormat(m_texture_format, false);
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
  desc.OutputWindow = Window();
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  desc.Flags = 0;

  m_allow_tearing_supported = false;
  return m_dxgi_factory->CreateSwapChain(m_d3d_device.Get(), &desc, &m_swap_chain);
}

bool SwapChain::TryEnableHDR()
{
  // HDR is switched on only after creation, since there is no way to ask in
  // advance whether the FP16 format and colour space will be accepted.
  Microsoft::WRL::ComPtr<IDXGISwapChain4> chain4;
  if (FAILED(m_swap_chain.As(&chain4)))
    return false;

  // PQ/BT.2020 is the space to probe; asking about scRGB directly always
  // reports no support. The probe works whether or not the display is in
  // HDR mode right now.
  UINT support = 0;
  const HRESULT probe =
      chain4->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
  if (FAILED(probe) || (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0)
    return false;

  if (FAILED(chain4->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, 0, 0,
                                   GetDXGIFormatForAbstractFormat(m_texture_format_hdr, false),
                                   GetSwapChainFlags())))
  {
    return false;
  }

  return SUCCEEDED(chain4->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
}

bool SwapChain::CreateSwapChain(bool stereo, bool hdr)
{
  RECT client;
  if (GetClientRect(Window(), &client))
  {
    m_width = static_cast<u32>(client.right - client.left);
    m_height = static_cast<u32>(client.bottom - client.top);
  }

  m_stereo = false;
  m_hdr = false;

  HRESULT result = CreateFlipModel(stereo);
  if (FAILED(result))
  {
    // The BitBlt model has no FP16 back buffers.
    hdr = false;
    result = CreateBitBltModel();
  }

  if (FAILED(result))
  {
    PanicAlertFmt("Failed to create swap chain: {}", Common::HRWrap(result));
    return false;
  }

  // Fullscreen is managed by the frontend, not by DXGI's Alt+Enter handling.
  result = m_dxgi_factory->MakeWindowAssociation(Window(), DXGI_MWA_NO_WINDOW_CHANGES |
                                                               DXGI_MWA_NO_ALT_ENTER);
  if (FAILED(result))
    WARN_LOG_FMT(VIDEO, "MakeWindowAssociation() failed: {}", Common::HRWrap(result));

  m_stereo = stereo;
  if (hdr && TryEnableHDR())
    m_hdr = true;

  if (CreateSwapChainBuffers())
    return true;

  PanicAlertFmt("Failed to create swap chain buffers");
  DestroySwapChainBuffers();
  m_swap_chain.Reset();
  return false;
}

void SwapChain::DestroySwapChain()
{
  DestroySwapChainBuffers();
  LeaveFullscreen();
  m_swap_chain.Reset();
}

bool SwapChain::ResizeSwapChain()
{
  DestroySwapChainBuffers();

  // Zero width and height take the window's client size.
  const HRESULT resized = m_swap_chain->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, 0, 0,
                                                      DXGI_FORMAT_UNKNOWN, GetSwapChainFlags());
  if (FAILED(resized))
    WARN_LOG_FMT(VIDEO, "ResizeBuffers() failed: {}", Common::HRWrap(resized));

  // Re-apply the colour space; a failure here is not fatal.
  Microsoft::WRL::ComPtr<IDXGISwapChain4> chain4;
  if (SUCCEEDED(m_swap_chain.As(&chain4)))
  {
    chain4->SetColorSpace1(m_hdr ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 :
                                   DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  }

  DXGI_SWAP_CHAIN_DESC current;
  if (SUCCEEDED(m_swap_chain->GetDesc(&current)))
  {
    m_width = current.BufferDesc.Width;
    m_height = current.BufferDesc.Height;
  }

  return CreateSwapChainBuffers();
}

bool SwapChain::RecreateWith(bool stereo, bool hdr)
{
  DestroySwapChain();
  return CreateSwapChain(stereo, hdr);
}

void SwapChain::SetStereo(bool stereo)
{
  if (stereo == m_stereo)
    return;

  // Keep the HDR state that actually took effect, rather than re-requesting it.
  if (!RecreateWith(stereo, m_hdr))
  {
    PanicAlertFmt("Failed to switch swap chain stereo mode");
    CreateSwapChain(false, false);
  }
}

void SwapChain::SetHDR(bool hdr)
{
  if (hdr == m_hdr)
    return;

  // Same rule for stereo: only what took effect is carried over. (Switching the
  // format through ResizeBuffers alone would be cheaper, but is not attempted.)
  if (!RecreateWith(m_stereo, hdr))
  {
    PanicAlertFmt("Failed to switch swap chain SDR/HDR mode");
    CreateSwapChain(false, false);
  }
}

bool SwapChain::GetFullscreen() const
{
  return IsFullscreen(m_swap_chain.Get());
}

void SwapChain::SetFullscreen(bool request)
{
  m_swap_chain->SetFullscreenState(request, nullptr);
}

bool SwapChain::CheckForFullscreenChange()
{
  // A pending request is tried first; if DXGI takes it, that is the change.
  if (m_fullscreen_request != m_has_fullscreen &&
      SUCCEEDED(m_swap_chain->SetFullscreenState(m_fullscreen_request, nullptr)))
  {
    m_has_fullscreen = m_fullscreen_request;
    return true;
  }

  // Otherwise see whether the system moved us in or out of fullscreen.
  const bool now_fullscreen = IsFullscreen(m_swap_chain.Get());
  if (now_fullscreen == m_has_fullscreen)
    return false;

  m_has_fullscreen = now_fullscreen;
  m_fullscreen_request = now_fullscreen;
  return true;
}

bool SwapChain::Present()
{
  // With a sync interval of 0 the tearing flag should always be passed when the
  // system has it, except in exclusive fullscreen, where DXGI rejects it.
  const bool vsync = g_ActiveConfig.bVSyncActive;
  const UINT flags =
      (m_allow_tearing_supported && !vsync && !m_has_fullscreen) ? DXGI_PRESENT_ALLOW_TEARING : 0;

  const HRESULT result = m_swap_chain->Present(static_cast<UINT>(vsync), flags);
  if (SUCCEEDED(result))
    return true;

  WARN_LOG_FMT(VIDEO, "Swap chain present failed: {}", Common::HRWrap(result));
  return false;
}

bool SwapChain::ChangeSurface(void* native_handle)
{
  DestroySwapChain();
  m_wsi.render_surface = native_handle;
  // Carry over only the stereo and HDR modes that were in effect.
  return CreateSwapChain(m_stereo, m_hdr);
}

}  // namespace D3DCommon
