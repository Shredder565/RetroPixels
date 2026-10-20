// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace Memory
{
class MemoryManager;
}

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}

namespace Net
{
#pragma pack(push, 1)
// One of the three connection slots of the system network settings (2332 bytes each).
struct ConnectionSettings final
{
  // Proxy part, stored twice in every slot.
  struct ProxySettings final
  {
    u8 use_proxy;
    u8 use_proxy_userandpass;
    u8 reserved_0[2];
    u8 proxy_name[255];
    u8 reserved_1;
    u16 proxy_port;
    u8 proxy_username[32];
    u8 reserved_2;
    u8 proxy_password[32];
  };

  // Bits of `flags`.
  enum
  {
    WIRED_IF = 1,             // set: wired, clear: wireless
    DNS_DHCP = 2,             // DNS servers from DHCP
    IP_DHCP = 4,              // address from DHCP
    USE_PROXY = 16,
    CONNECTION_TEST_OK = 32,  // the settings passed the connection test
    CONNECTION_SELECTED = 128,
  };

  // Values of `encryption`.
  enum
  {
    OPEN = 0,
    WEP64 = 1,
    WEP128 = 2,
    WPA_TKIP = 4,
    WPA2_AES = 5,
    WPA_AES = 6,
  };

  // Link states reported by NCD.
  enum Status
  {
    LINK_BUSY = 1,
    LINK_NONE = 2,
    LINK_WIRED = 3,
    LINK_WIFI_DOWN = 4,
    LINK_WIFI_UP = 5,
  };

  // Bits of the WiiConnect24 permission byte.
  enum
  {
    PERM_NONE = 0,
    PERM_SEND_MAIL = 1,
    PERM_RECV_MAIL = 2,
    PERM_DOWNLOAD = 4,
    PERM_ALL = PERM_SEND_MAIL | PERM_RECV_MAIL | PERM_DOWNLOAD,
  };

  // Used by both kinds of connection.
  u8 flags;
  u8 reserved_0[3];
  u8 ip[4];
  u8 netmask[4];
  u8 gateway[4];
  u8 dns1[4];
  u8 dns2[4];
  u8 reserved_1[2];
  u16 mtu;
  u8 reserved_2[8];
  ProxySettings proxy_settings;
  u8 reserved_3;
  ProxySettings proxy_settings_copy;
  u8 reserved_4[1297];

  // Only used by wireless connections.
  u8 ssid[32];
  u8 reserved_5;
  u8 ssid_length;
  u8 reserved_6[3];
  u8 encryption;
  u8 reserved_7[3];
  u8 key_length;  // 0 for WEP
  u8 unknown;     // seen as 0 or 1 depending on how the key was entered
  u8 reserved_8;
  u8 key[64];     // WEP keys are stored four times over
  u8 reserved_9[236];
};
#pragma pack(pop)

static_assert(sizeof(ConnectionSettings::ProxySettings) == 327);
static_assert(offsetof(ConnectionSettings::ProxySettings, proxy_port) == 260);
static_assert(offsetof(ConnectionSettings::ProxySettings, proxy_password) == 295);
static_assert(offsetof(ConnectionSettings, mtu) == 26);
static_assert(offsetof(ConnectionSettings, proxy_settings_copy) == 364);
static_assert(offsetof(ConnectionSettings, ssid) == 1988);
static_assert(offsetof(ConnectionSettings, encryption) == 2025);
static_assert(offsetof(ConnectionSettings, key) == 2032);
static_assert(sizeof(ConnectionSettings) == 2332);

// The system network settings (/shared2/sys/net/02/config.dat), as used by /dev/net/ncd/manage.
class WiiNetConfig final
{
public:
  WiiNetConfig();

  // Loads the file; a missing or short file is replaced by the default settings.
  void ReadConfig(FS::FileSystem* fs);
  void WriteConfig(FS::FileSystem* fs) const;
  // Deletes the file and writes the defaults: one wired DHCP connection, tested and selected.
  void ResetConfig(FS::FileSystem* fs);

  void WriteToMem(Memory::MemoryManager& memory, u32 address) const;
  void ReadFromMem(const Memory::MemoryManager& memory, u32 address);

private:
#pragma pack(push, 1)
  struct ConfigData final
  {
    // Values of connType.
    enum
    {
      IF_NONE,
      IF_WIFI,
      IF_WIRED,
    };

    u32 version;
    u8 connType;
    u8 linkTimeout;
    u8 nwc24Permission;
    u8 padding;
    ConnectionSettings connection[3];
  };
#pragma pack(pop)
  static_assert(sizeof(ConfigData) == 7004);

  ConfigData m_data{};
};
}  // namespace Net
}  // namespace IOS::HLE
