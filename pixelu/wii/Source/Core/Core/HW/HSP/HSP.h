// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <span>

#include "Common/CommonTypes.h"
#include "Core/HW/HSP/HSP_Device.h"

class PointerWrap;

namespace HSP
{
// Owns whatever is attached to the high speed port and forwards DSP block
// transfers to it.
class HSPManager
{
public:
  HSPManager();
  HSPManager(const HSPManager& other) = delete;
  HSPManager(HSPManager&& other) = delete;
  HSPManager& operator=(const HSPManager& other) = delete;
  HSPManager& operator=(HSPManager&& other) = delete;
  ~HSPManager();

  void Init();
  void Shutdown();

  void Read(u32 address, std::span<u8, TRANSFER_SIZE> data);
  void Write(u32 address, std::span<const u8, TRANSFER_SIZE> data);

  void DoState(PointerWrap& p);

  void SetDevice(std::unique_ptr<IHSPDevice> device);
  void SetDevice(HSPDeviceType device);

private:
  std::unique_ptr<IHSPDevice> m_attached;
};
}  // namespace HSP
