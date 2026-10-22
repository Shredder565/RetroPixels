// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/HSP/HSP_DeviceNull.h"

namespace HSP
{

void CHSPDevice_Null::Read(u32, std::span<u8, TRANSFER_SIZE>)
{
}

void CHSPDevice_Null::Write(u32, std::span<const u8, TRANSFER_SIZE>)
{
}

}  // namespace HSP
