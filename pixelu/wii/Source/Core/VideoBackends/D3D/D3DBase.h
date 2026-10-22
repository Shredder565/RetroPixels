// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <fmt/format.h>
#include <vector>
#include <wrl/client.h>

#include "Common/CommonTypes.h"
#include "Common/HRWrap.h"

namespace DX11
{
using Microsoft::WRL::ComPtr;
class SwapChain;

// The process-wide D3D11 device and its immediate context.
namespace D3D
{
extern ComPtr<IDXGIFactory> dxgi_factory;
extern ComPtr<ID3D11Device> device;
extern ComPtr<ID3D11Device1> device1;  // null without D3D 11.1 (no logic ops)
extern ComPtr<ID3D11DeviceContext> context;
extern D3D_FEATURE_LEVEL feature_level;

// Loads the runtime, creates the factory and the device on the given adapter
// (falling back to the default adapter) and the state manager.
bool Create(u32 adapter_index, bool enable_debug_layer);
// Releases everything Create made, reporting leaked device references.
void Destroy();

// Sample counts usable for the EFB colour format. Without a device, a
// throwaway one is made on the adapter.
std::vector<u32> GetAAModes(u32 adapter_index);

// Whether the device can make 2D textures of this format.
bool SupportsTextureFormat(DXGI_FORMAT format);

// Whether output-merger logic ops are available (D3D 11.1 and driver support).
bool SupportsLogicOp(u32 adapter_index);

}  // namespace D3D

// fmt wrapper for an HRESULT from D3D11. HRESULT is a plain typedef, so it
// cannot have its own formatter. On DXGI_ERROR_DEVICE_REMOVED the device's
// removal reason is printed as well.
struct DX11HRWrap
{
  constexpr explicit DX11HRWrap(HRESULT hr) : m_hr(hr) {}
  const HRESULT m_hr;
};

}  // namespace DX11

template <>
struct fmt::formatter<DX11::DX11HRWrap>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const DX11::DX11HRWrap& wrapped, FormatContext& ctx) const
  {
    const bool removed = wrapped.m_hr == DXGI_ERROR_DEVICE_REMOVED && DX11::D3D::device != nullptr;
    if (!removed)
      return fmt::format_to(ctx.out(), "{}", Common::HRWrap(wrapped.m_hr));

    const HRESULT reason = DX11::D3D::device->GetDeviceRemovedReason();
    return fmt::format_to(ctx.out(), "{}\nDevice removal reason: {}", Common::HRWrap(wrapped.m_hr),
                          Common::HRWrap(reason));
  }
};
