// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/HSP/HSP.h"

#include <memory>

#include <fmt/ranges.h>

#include "Common/ChunkFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/HSP/HSP_Device.h"

namespace HSP
{
HSPManager::HSPManager() = default;
HSPManager::~HSPManager() = default;

void HSPManager::Init()
{
  SetDevice(Config::Get(Config::MAIN_HSP_DEVICE));
}

void HSPManager::Shutdown()
{
  m_attached = nullptr;
}

void HSPManager::Read(u32 address, std::span<u8, TRANSFER_SIZE> data)
{
  DEBUG_LOG_FMT(HSP, "HSP block read at 0x{:08x}", address);
  m_attached->Read(address, data);
}

void HSPManager::Write(u32 address, std::span<const u8, TRANSFER_SIZE> data)
{
  DEBUG_LOG_FMT(HSP, "HSP block write at 0x{:08x}: {:02x}", address, fmt::join(data, " "));
  m_attached->Write(address, data);
}

void HSPManager::DoState(PointerWrap& p)
{
  // The device type goes first; a state made with another device attached
  // swaps it in before the device's own data is read.
  HSPDeviceType saved = m_attached->GetDeviceType();
  p.Do(saved);
  if (saved != m_attached->GetDeviceType())
    SetDevice(saved);

  m_attached->DoState(p);
}

void HSPManager::SetDevice(std::unique_ptr<IHSPDevice> device)
{
  m_attached = std::move(device);
}

void HSPManager::SetDevice(const HSPDeviceType device)
{
  SetDevice(HSPDevice_Create(device));
}

}  // namespace HSP
