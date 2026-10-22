// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

namespace IOS::HLE
{
// Returns the parameters of an SDP ServiceAttributeResponse for the Wii Remote's service record
// `service_handle` (0x10000: HID, 0x10001: PnP information), starting at the attribute list
// offset given by the request's continuation state: the PDU parameter length, the
// AttributeListByteCount, a fragment of the attribute list and the next continuation state.
// Returns nullptr (and leaves `size` alone) when there is no such fragment.
const u8* GetAttribPacket(u32 service_handle, u32 continuation, u32& size);
}  // namespace IOS::HLE
