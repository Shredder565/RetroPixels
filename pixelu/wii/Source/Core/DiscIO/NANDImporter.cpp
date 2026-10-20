// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/NANDImporter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "Common/Crypto/AES.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Core/IOS/ES/Formats.h"

namespace DiscIO
{
namespace
{
// BootMii backup layout: 0x40000 pages of 0x800 data bytes, each followed by 0x40 ECC bytes,
// optionally followed by the 0x400-byte OTP/SEEPROM dump.
constexpr size_t PAGE_COUNT = 0x40000;
constexpr size_t PAGE_DATA_SIZE = 0x800;
constexpr size_t PAGE_ECC_SIZE = 0x40;
constexpr size_t BACKUP_SIZE = PAGE_COUNT * (PAGE_DATA_SIZE + PAGE_ECC_SIZE);
constexpr size_t FLASH_SIZE = PAGE_COUNT * PAGE_DATA_SIZE;
constexpr size_t OTP_DUMP_SIZE = 0x400;

// Where the NAND AES key sits in the OTP/SEEPROM dump.
constexpr size_t NAND_KEY_OFFSET = 0x158;

// The 16 superblocks fill the last 4 MiB of the flash.
constexpr size_t SUPERBLOCK_AREA = 0x1fc00000;
constexpr int SUPERBLOCK_COUNT = 16;

// File data is stored in encrypted clusters of 0x4000 bytes, chained through the FAT.
constexpr size_t CLUSTER_SIZE = 0x4000;
constexpr u16 FAT_LAST_CLUSTER = 0xFFFB;
constexpr u16 NO_ENTRY = 0xffff;

std::string EntryName(const NANDImporter::NANDFSTEntry& entry)
{
  return std::string(entry.name, strnlen(entry.name, sizeof(entry.name)));
}
}  // namespace

NANDImporter::NANDImporter() : m_nand_root(File::GetUserPath(D_WIIROOT_IDX))
{
}

NANDImporter::~NANDImporter() = default;

void NANDImporter::ImportNANDBin(const std::string& path_to_bin, UpdateCallback update_callback,
                                 const std::function<std::string()>& get_otp_dump_path)
{
  m_update_callback = std::move(update_callback);

  if (!LoadBackup(path_to_bin, get_otp_dump_path) || !SelectSuperblock())
    return;
  WriteKeys();
  if (!ExtractTree())
    return;
  ExtractCertificates();

  // Final progress report; cancelling is meaningless at this point.
  std::ignore = m_update_callback(Step::Extracting, m_files_done, m_files_total);
}

bool NANDImporter::LoadBackup(const std::string& path_to_bin,
                              const std::function<std::string()>& get_otp_dump_path)
{
  File::IOFile backup(path_to_bin, "rb");
  const u64 size = backup.GetSize();
  const bool has_otp_dump = size == BACKUP_SIZE + OTP_DUMP_SIZE;
  if (!has_otp_dump && size != BACKUP_SIZE)
  {
    PanicAlertFmtT("This file does not look like a BootMii NAND backup.");
    return false;
  }

  m_flash.resize(FLASH_SIZE);
  for (size_t page = 0; page < PAGE_COUNT; ++page)
  {
    if (m_update_callback(Step::Loading, static_cast<int>(page), static_cast<int>(PAGE_COUNT)))
      return false;
    backup.ReadBytes(m_flash.data() + page * PAGE_DATA_SIZE, PAGE_DATA_SIZE);
    backup.Seek(PAGE_ECC_SIZE, File::SeekOrigin::Current);  // ECC is not needed
  }

  m_otp_dump.resize(OTP_DUMP_SIZE);
  if (has_otp_dump)
    return backup.ReadBytes(m_otp_dump.data(), m_otp_dump.size());

  const std::string otp_path = get_otp_dump_path();
  if (otp_path.empty())
    return false;
  File::IOFile otp_file(otp_path, "rb");
  return otp_file.ReadBytes(m_otp_dump.data(), m_otp_dump.size());
}

bool NANDImporter::SelectSuperblock()
{
  // Every superblock with the right magic is a candidate; the highest version is the newest.
  for (int index = 0; index < SUPERBLOCK_COUNT; ++index)
  {
    auto candidate = std::make_unique<NANDSuperblock>();
    std::memcpy(candidate.get(), m_flash.data() + SUPERBLOCK_AREA + index * sizeof(NANDSuperblock),
                sizeof(NANDSuperblock));

    if (std::string_view(candidate->magic.data(), candidate->magic.size()) != "SFFS")
    {
      ERROR_LOG_FMT(DISCIO, "Superblock #{} does not exist", index);
      continue;
    }
    INFO_LOG_FMT(DISCIO, "Superblock #{} has version {:#x}", index, candidate->version);

    if (!m_superblock || candidate->version > m_superblock->version)
      m_superblock = std::move(candidate);
  }

  if (!m_superblock)
  {
    PanicAlertFmtT("This file does not contain a valid Wii filesystem.");
    return false;
  }
  INFO_LOG_FMT(DISCIO, "Using superblock version {:#x}", m_superblock->version);
  return true;
}

void NANDImporter::WriteKeys()
{
  m_cluster_cipher = Common::AES::CreateContextDecrypt(m_otp_dump.data() + NAND_KEY_OFFSET);

  const std::string keys_path = m_nand_root + "/keys.bin";
  File::IOFile keys_file(keys_path, "wb");
  if (!keys_file.WriteBytes(m_otp_dump.data(), m_otp_dump.size()))
    PanicAlertFmtT("Unable to write to file {0}", keys_path);
}

bool NANDImporter::ExtractTree()
{
  // Each file's cluster chain has exactly one end marker, which gives the number of files.
  m_files_done = 0;
  m_files_total = static_cast<u16>(std::ranges::count(m_superblock->fat, FAT_LAST_CLUSTER));

  std::bitset<FSTEntryCount> seen;
  return ExtractSiblings(0, "", &seen);
}

bool NANDImporter::ExtractSiblings(u16 first_entry, const std::string& parent_path,
                                   std::bitset<FSTEntryCount>* seen)
{
  for (u16 number = first_entry; number != NO_ENTRY;)
  {
    if (number >= m_superblock->fst.size())
    {
      ERROR_LOG_FMT(DISCIO, "FST entry number {} out of range", number);
      return false;
    }
    if (seen->test(number))
    {
      ERROR_LOG_FMT(DISCIO, "FST entry number {} visited multiple times", number);
      return false;
    }
    seen->set(number);

    const NANDFSTEntry entry = m_superblock->fst[number];
    // Entry 0 is the root directory, which maps to the NAND root itself.
    const std::string path =
        number == 0 ? parent_path : parent_path + '/' + Common::EscapeFileName(EntryName(entry));
    INFO_LOG_FMT(DISCIO, "Entry: {} Path: {}", entry, path);

    switch (static_cast<Type>(entry.mode & 3))
    {
    case Type::File:
    {
      if (m_update_callback(Step::Extracting, m_files_done++, m_files_total))
        return false;
      const std::vector<u8> contents = ReadFileClusters(entry);
      File::IOFile out(m_nand_root + path, "wb");
      out.WriteBytes(contents.data(), contents.size());
      break;
    }
    case Type::Directory:
      File::CreateDir(m_nand_root + path);
      if (!ExtractSiblings(entry.sub, path, seen))
        return false;
      break;
    default:
      ERROR_LOG_FMT(DISCIO, "Ignoring unknown entry type for {}", entry);
      break;
    }

    number = entry.sib;
  }
  return true;
}

std::vector<u8> NANDImporter::ReadFileClusters(const NANDFSTEntry& entry) const
{
  size_t left = entry.size;
  std::vector<u8> contents;
  contents.reserve(left);

  std::vector<u8> cluster(CLUSTER_SIZE);
  for (u16 index = entry.sub; left > 0; index = m_superblock->fat[index])
  {
    if (index >= m_superblock->fat.size())
    {
      ERROR_LOG_FMT(DISCIO, "FAT block index {} out of range", index);
      return {};
    }
    m_cluster_cipher->CryptIvZero(m_flash.data() + CLUSTER_SIZE * index, cluster.data(),
                                  CLUSTER_SIZE);
    const size_t take = std::min(left, CLUSTER_SIZE);
    contents.insert(contents.end(), cluster.begin(), cluster.begin() + take);
    left -= take;
  }
  return contents;
}

bool NANDImporter::ExtractCertificates()
{
  // IOS13's boot content embeds the certificates used by the SSL module, each a DER blob preceded
  // by a big-endian u16 length.
  struct EmbeddedCertificate
  {
    std::string_view file_name;
    std::array<u8, 4> der_start;
  };
  static constexpr std::array<EmbeddedCertificate, 3> CERTIFICATES{{
      {"/clientca.pem", {{0x30, 0x82, 0x03, 0xE9}}},
      {"/clientcakey.pem", {{0x30, 0x82, 0x02, 0x5D}}},
      {"/rootca.pem", {{0x30, 0x82, 0x03, 0x7D}}},
  }};
  constexpr ptrdiff_t LENGTH_FIELD_SIZE = 2;

  const std::string content_dir = m_nand_root + "/title/00000001/0000000d/content/";

  File::IOFile tmd_file(content_dir + "title.tmd", "rb");
  std::vector<u8> tmd_data(tmd_file.GetSize());
  if (!tmd_file.ReadBytes(tmd_data.data(), tmd_data.size()))
  {
    ERROR_LOG_FMT(DISCIO, "ExtractCertificates: Could not read IOS13 TMD");
    return false;
  }

  const IOS::ES::TMDReader tmd(std::move(tmd_data));
  IOS::ES::Content boot_content;
  if (!tmd.GetContent(tmd.GetBootIndex(), &boot_content))
  {
    ERROR_LOG_FMT(DISCIO, "ExtractCertificates: Could not get content ID from TMD");
    return false;
  }

  File::IOFile app_file(content_dir + fmt::format("{:08x}.app", boot_content.id), "rb");
  std::vector<u8> app(app_file.GetSize());
  if (!app_file.ReadBytes(app.data(), app.size()))
  {
    ERROR_LOG_FMT(DISCIO, "ExtractCertificates: Could not read IOS13 contents");
    return false;
  }

  for (const EmbeddedCertificate& certificate : CERTIFICATES)
  {
    const auto found = std::ranges::search(app, certificate.der_start);
    if (found.empty())
    {
      ERROR_LOG_FMT(DISCIO, "ExtractCertificates: Could not find offset for certificate '{}'",
                    certificate.file_name);
      return false;
    }

    const ptrdiff_t offset = found.begin() - app.begin();
    if (offset < LENGTH_FIELD_SIZE)
    {
      ERROR_LOG_FMT(
          DISCIO,
          "ExtractCertificates: Invalid certificate offset {:#x}, must be between {:#x} and {:#x}",
          offset, int{LENGTH_FIELD_SIZE}, app.size());
      return false;
    }

    const u16 length = Common::swap16(&app[offset - LENGTH_FIELD_SIZE]);
    const size_t room = app.size() - static_cast<size_t>(offset);
    if (length > room)
    {
      ERROR_LOG_FMT(
          DISCIO,
          "ExtractCertificates: Invalid certificate size {:#x}, must be {:#x} bytes or smaller",
          length, room);
      return false;
    }
    INFO_LOG_FMT(DISCIO, "ExtractCertificates: '{}' offset: {:#x} size: {:#x}",
                 certificate.file_name, offset, length);

    const std::string pem_path = m_nand_root + std::string(certificate.file_name);
    File::IOFile pem_file(pem_path, "wb");
    if (!pem_file.WriteBytes(&app[offset], length))
    {
      ERROR_LOG_FMT(DISCIO, "ExtractCertificates: Unable to write to file {}", pem_path);
      return false;
    }
  }
  return true;
}
}  // namespace DiscIO
