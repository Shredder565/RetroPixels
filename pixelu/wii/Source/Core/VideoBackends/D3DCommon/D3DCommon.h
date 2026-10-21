// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Pieces shared by the Direct3D 11 and Direct3D 12 backends: the runtime-loaded
// DXGI and shader compiler entry points, adapter enumeration, and the mapping
// between the renderer's texture formats and DXGI formats.

#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

#include "Common/CommonTypes.h"

struct IDXGIFactory;

enum class AbstractTextureFormat : u32;

namespace D3DCommon
{
// Opens dxgi.dll and the D3D shader compiler and resolves their entry points.
// Safe to call repeatedly; returns false (after telling the user) on failure.
bool LoadLibraries();
// Drops the entry points and closes both libraries again.
void UnloadLibraries();

// Description strings of every adapter DXGI reports, in enumeration order.
std::vector<std::string> GetAdapterNames();

// Makes a DXGI factory, with the debug flag when the runtime supports it.
// Returns null after an alert when no factory could be made.
Microsoft::WRL::ComPtr<IDXGIFactory> CreateDXGIFactory(bool debug_device);

// D3DCompile from the loaded compiler library.
extern pD3DCompile d3d_compile;

// Texture format translation. "typeless" asks for the storage format of a
// texture that is also viewed as a render target; "integer" asks for the
// unsigned-integer view used for logic ops.
DXGI_FORMAT GetDXGIFormatForAbstractFormat(AbstractTextureFormat format, bool typeless);
DXGI_FORMAT GetSRVFormatForAbstractFormat(AbstractTextureFormat format);
DXGI_FORMAT GetRTVFormatForAbstractFormat(AbstractTextureFormat format, bool integer);
DXGI_FORMAT GetDSVFormatForAbstractFormat(AbstractTextureFormat format);
AbstractTextureFormat GetAbstractFormatForDXGIFormat(DXGI_FORMAT format);

// Tags a D3D11 or D3D12 object with a readable name for the debug layer.
// Does nothing unless the validation layer is enabled.
void SetDebugObjectName(IUnknown* resource, std::string_view name);
}  // namespace D3DCommon
