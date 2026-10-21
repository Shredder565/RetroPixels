// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "Common/MsgHandler.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

namespace DX12
{
using Microsoft::WRL::ComPtr;

// Records a whole-resource state transition.
static void ResourceBarrier(ID3D12GraphicsCommandList* cmdlist, ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES from_state, D3D12_RESOURCE_STATES to_state)
{
  D3D12_RESOURCE_BARRIER transition;
  transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  transition.Transition.pResource = resource;
  transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  transition.Transition.StateBefore = from_state;
  transition.Transition.StateAfter = to_state;
  cmdlist->ResourceBarrier(1, &transition);
}
}  // namespace DX12
