// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/ICMP.h"

// Sending raw ICMP needs elevated privileges on the hosts we run on, so pings always fail.

int icmp_echo_req(const u32, const sockaddr_in*, const u8*, const u32)
{
  return -1;
}

int icmp_echo_rep(const u32, sockaddr_in*, const u32, const u32)
{
  return -1;
}
