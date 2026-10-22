// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/NCD/WiiNetConfig.h"

#include <string>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::Net
{
namespace
{
const std::string SETTINGS_FILE = "/shared2/sys/net/02/config.dat";

// Everyone may read and write the settings file.
constexpr FS::Modes SETTINGS_MODES{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};

bool LoadSettings(FS::FileSystem* fs, void* data, size_t size)
{
  // The handle is closed again before returning, so a reset can delete the file.
  const auto file = fs->OpenFile(PID_NCD, PID_NCD, SETTINGS_FILE, FS::Mode::Read);
  return file && file->Read(static_cast<u8*>(data), size);
}
}  // namespace

WiiNetConfig::WiiNetConfig() = default;

void WiiNetConfig::ReadConfig(FS::FileSystem* fs)
{
  if (!LoadSettings(fs, &m_data, sizeof(m_data)))
    ResetConfig(fs);
}

void WiiNetConfig::WriteConfig(FS::FileSystem* fs) const
{
  fs->CreateFullPath(PID_NCD, PID_NCD, SETTINGS_FILE, 0, SETTINGS_MODES);
  const auto file = fs->CreateAndOpenFile(PID_NCD, PID_NCD, SETTINGS_FILE, SETTINGS_MODES);
  const bool written = file && file->Write(reinterpret_cast<const u8*>(&m_data), sizeof(m_data));
  if (!written)
    ERROR_LOG_FMT(IOS_NET, "Failed to write config");
}

void WiiNetConfig::ResetConfig(FS::FileSystem* fs)
{
  fs->Delete(PID_NCD, PID_NCD, SETTINGS_FILE);

  m_data = ConfigData{};
  m_data.connType = ConfigData::IF_WIRED;
  m_data.connection[0].flags = ConnectionSettings::WIRED_IF | ConnectionSettings::IP_DHCP |
                               ConnectionSettings::DNS_DHCP |
                               ConnectionSettings::CONNECTION_SELECTED |
                               ConnectionSettings::CONNECTION_TEST_OK;
  WriteConfig(fs);
}

void WiiNetConfig::WriteToMem(Memory::MemoryManager& memory, const u32 address) const
{
  memory.CopyToEmu(address, &m_data, sizeof(m_data));
}

void WiiNetConfig::ReadFromMem(const Memory::MemoryManager& memory, const u32 address)
{
  memory.CopyFromEmu(&m_data, address, sizeof(m_data));
}
}  // namespace IOS::HLE::Net
