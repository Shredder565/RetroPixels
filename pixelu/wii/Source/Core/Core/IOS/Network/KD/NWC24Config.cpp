// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/NWC24Config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24
{
namespace
{
// Every multi-byte field of these files is stored big endian.
u32 ByteSwapped(u32 value)
{
  return Common::swap32(value);
}

u64 ByteSwapped(u64 value)
{
  return Common::swap64(value);
}
}  // namespace

namespace
{
constexpr const char CONFIG_PATH[] = "/" WII_WC24CONF_DIR "/nwc24msg.cfg";
constexpr const char CBK_PATH[] = "/" WII_WC24CONF_DIR "/nwc24msg.cbk";

constexpr u32 CONFIG_MAGIC = 0x57634366;  // "WcCf"
constexpr u32 CONFIG_VERSION = 8;
constexpr u32 MAX_ID_GENERATION = 0x1f;
constexpr size_t CHECKSUMMED_WORDS = 0xff;

constexpr s32 CONFIG_BROKEN = -14;
constexpr s32 CONFIG_BAD_VERSION = -27;

// Server URLs of a fresh config, in http_urls order.
constexpr std::array<const char*, 5> DEFAULT_URLS{
    "https://amw.wc24.wii.com/cgi-bin/account.cgi",
    "http://rcw.wc24.wii.com/cgi-bin/check.cgi",
    "http://mtw.wc24.wii.com/cgi-bin/receive.cgi",
    "http://mtw.wc24.wii.com/cgi-bin/delete.cgi",
    "http://mtw.wc24.wii.com/cgi-bin/send.cgi",
};

// Copies `text` up to its first NUL into a fixed field, zero-filling the rest.
void StoreText(char* field, size_t field_size, std::string_view text)
{
  const size_t end = std::min(text.find('\0'), text.size());
  const size_t length = std::min(end, field_size);
  std::memcpy(field, text.data(), length);
  std::memset(field + length, 0, field_size - length);
}

// The text in a fixed field, which need not be NUL-terminated.
std::string_view LoadText(const char* field, size_t field_size)
{
  return {field, strnlen(field, field_size)};
}

bool LoadFile(FS::FileSystem& fs, void* data, size_t size)
{
  const auto file = fs.OpenFile(PID_KD, PID_KD, CONFIG_PATH, FS::Mode::Read);
  return file && file->Read(static_cast<u8*>(data), size);
}
}  // namespace

NWC24Config::NWC24Config(std::shared_ptr<FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  ReadConfig();
}

void NWC24Config::ReadConfig()
{
  if (!LoadFile(*m_fs, &m_data, sizeof(m_data)))
  {
    ResetConfig();
    return;
  }

  // A damaged file is still used as it is.
  if (const s32 problem = CheckNwc24Config(); problem != 0)
    ERROR_LOG_FMT(IOS_WC24, "There is an error in the config for for WC24: {}", problem);
}

void NWC24Config::WriteCBK() const
{
  WriteConfigToPath(CBK_PATH);
}

void NWC24Config::WriteConfig() const
{
  WriteConfigToPath(CONFIG_PATH);
}

void NWC24Config::WriteConfigToPath(const std::string& path) const
{
  constexpr FS::Modes everyone_rw{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  m_fs->CreateFullPath(PID_KD, PID_KD, path, 0, everyone_rw);
  const auto file = m_fs->CreateAndOpenFile(PID_KD, PID_KD, path, everyone_rw);
  const bool written = file && file->Write(reinterpret_cast<const u8*>(&m_data), sizeof(m_data));
  if (!written)
    ERROR_LOG_FMT(IOS_WC24, "Failed to open or write WC24 config file at {}", path);
}

void NWC24Config::ResetConfig()
{
  m_fs->Delete(PID_KD, PID_KD, CONFIG_PATH);

  std::memset(&m_data, 0, sizeof(m_data));
  SetMagic(CONFIG_MAGIC);
  SetVersion(CONFIG_VERSION);
  SetCreationStage(NWC24CreationStage::Initial);
  SetEnableBooting(0);
  SetEmail("@wii.com");
  for (size_t i = 0; i < DEFAULT_URLS.size(); ++i)
    StoreText(m_data.http_urls[i], MAX_URL_LENGTH, DEFAULT_URLS[i]);
  SetChecksum(CalculateNwc24ConfigChecksum());

  WriteConfig();
}

u32 NWC24Config::CalculateNwc24ConfigChecksum() const
{
  std::array<u32, CHECKSUMMED_WORDS> words;
  std::memcpy(words.data(), &m_data, sizeof(words));

  u32 sum = 0;
  for (const u32 word : words)
    sum += ByteSwapped(word);
  return sum;
}

s32 NWC24Config::CheckNwc24Config() const
{
  if (Magic() != CONFIG_MAGIC)
  {
    ERROR_LOG_FMT(IOS_WC24, "Magic mismatch");
    return CONFIG_BROKEN;
  }

  const u32 expected = CalculateNwc24ConfigChecksum();
  if (Checksum() != expected)
  {
    ERROR_LOG_FMT(IOS_WC24, "Checksum mismatch expected {:X} and got {:X}", expected, Checksum());
    return CONFIG_BROKEN;
  }

  if (IdGen() > MAX_ID_GENERATION)
  {
    ERROR_LOG_FMT(IOS_WC24, "Id gen error");
    return CONFIG_BROKEN;
  }

  return Version() == CONFIG_VERSION ? 0 : CONFIG_BAD_VERSION;
}

u32 NWC24Config::Magic() const
{
  return ByteSwapped(m_data.magic);
}

void NWC24Config::SetMagic(u32 magic)
{
  m_data.magic = ByteSwapped(magic);
}

u32 NWC24Config::Version() const
{
  return ByteSwapped(m_data.version);
}

void NWC24Config::SetVersion(u32 version)
{
  m_data.version = ByteSwapped(version);
}

u32 NWC24Config::IdGen() const
{
  return ByteSwapped(m_data.id_generation);
}

void NWC24Config::SetIdGen(u32 id_generation)
{
  m_data.id_generation = ByteSwapped(id_generation);
}

void NWC24Config::IncrementIdGen()
{
  SetIdGen((IdGen() + 1) & MAX_ID_GENERATION);
}

u32 NWC24Config::Checksum() const
{
  return ByteSwapped(m_data.checksum);
}

void NWC24Config::SetChecksum(u32 checksum)
{
  m_data.checksum = ByteSwapped(checksum);
}

NWC24CreationStage NWC24Config::CreationStage() const
{
  return static_cast<NWC24CreationStage>(ByteSwapped(static_cast<u32>(m_data.creation_stage)));
}

void NWC24Config::SetCreationStage(NWC24CreationStage creation_stage)
{
  m_data.creation_stage =
      static_cast<NWC24CreationStage>(ByteSwapped(static_cast<u32>(creation_stage)));
}

u32 NWC24Config::EnableBooting() const
{
  return ByteSwapped(m_data.enable_booting);
}

void NWC24Config::SetEnableBooting(u32 enable_booting)
{
  m_data.enable_booting = ByteSwapped(enable_booting);
}

u64 NWC24Config::Id() const
{
  return ByteSwapped(m_data.nwc24_id);
}

void NWC24Config::SetId(u64 nwc24_id)
{
  m_data.nwc24_id = ByteSwapped(nwc24_id);
}

const char* NWC24Config::Email() const
{
  return m_data.email;
}

void NWC24Config::SetEmail(const char* email)
{
  // Always NUL-terminated, so at most 63 characters are kept.
  StoreText(m_data.email, MAX_EMAIL_LENGTH, email);
  m_data.email[MAX_EMAIL_LENGTH - 1] = '\0';
}

std::string_view NWC24Config::GetMlchkid() const
{
  return LoadText(m_data.mlchkid, MAX_MLCHKID_LENGTH);
}

std::string_view NWC24Config::GetPassword() const
{
  return LoadText(m_data.paswd, MAX_PASSWORD_LENGTH);
}

std::string NWC24Config::GetAccountURL() const
{
  return std::string(LoadText(m_data.http_urls[URL_ACCOUNT], MAX_URL_LENGTH));
}

std::string NWC24Config::GetCheckURL() const
{
  return std::string(LoadText(m_data.http_urls[URL_CHECK], MAX_URL_LENGTH));
}

std::string NWC24Config::GetSendURL() const
{
  return std::string(LoadText(m_data.http_urls[URL_SEND], MAX_URL_LENGTH));
}

void NWC24Config::SetMailCheckID(std::string_view mlchkid)
{
  StoreText(m_data.mlchkid, MAX_MLCHKID_LENGTH, mlchkid);
  m_data.mlchkid[MAX_MLCHKID_LENGTH - 1] = '\0';
}

void NWC24Config::SetPassword(std::string_view password)
{
  StoreText(m_data.paswd, MAX_PASSWORD_LENGTH, password);
  m_data.paswd[MAX_PASSWORD_LENGTH - 1] = '\0';
}
}  // namespace IOS::HLE::NWC24
