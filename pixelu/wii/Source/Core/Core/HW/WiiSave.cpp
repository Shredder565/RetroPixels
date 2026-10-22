// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiiSave.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mbedtls/md5.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Crypto/ec.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Lazy.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/WiiUtils.h"

namespace WiiSave
{
namespace FS = IOS::HLE::FS;
using IOS::HLE::IOSC;

namespace
{
using Md5 = std::array<u8, 0x10>;
using Iv = std::array<u8, 0x10>;

// The header of a data.bin is encrypted with the SD card key and a fixed IV. Both are Nintendo's
// and RetroPixels has neither: they come from the owner's wii_keys.txt (sd_key, sd_iv).
std::optional<Iv> LoadSdIv()
{
  const std::optional<std::vector<u8>> bytes = IOSC::LoadOwnerKey("sd_iv", Iv{}.size());
  if (!bytes)
    return std::nullopt;
  Iv iv;
  std::ranges::copy(*bytes, iv.begin());
  return iv;
}

bool HasSdKey(const IOSC& iosc)
{
  Iv iv{};
  std::array<u8, 16> block{};
  return iosc.Encrypt(IOSC::HANDLE_SD_KEY, iv.data(), block.data(), block.size(), block.data(),
                      IOS::PID_ES) == IOS::HLE::IPC_SUCCESS;
}

// Import and export need both; without them they fail with this message.
bool CheckSdKeys(const IOSC& iosc)
{
  if (HasSdKey(iosc) && LoadSdIv())
    return true;
  PanicAlertFmtT("Wii save files (data.bin) are encrypted with the Wii's SD card key and IV, which "
                 "RetroPixels doesn't include.\n\nTo import or export them, add sd_key and sd_iv "
                 "from your own console to wii_keys.txt in:\n{0}",
                 File::GetUserPath(D_WIIROOT_IDX));
  return false;
}

// Fixed values of the data.bin format, as the System Menu writes it.
// Stands in for the header's own MD5 while the header is hashed. This is a published format
// constant (the value the MD5 field holds while it is computed), not key material.
constexpr Md5 MD5_BLANKER{0x0E, 0x65, 0x37, 0x81, 0x99, 0xBE, 0x45, 0x17,
                          0xAB, 0x06, 0xEC, 0x22, 0x45, 0x1A, 0x57, 0x93};
constexpr u32 NG_ID = 0x0403AC68;
// Written (big-endian) between the AP signature and the certificates.
constexpr u32 SIGNATURE_END_MAGIC = 0x2f536969;

constexpr const char* BANNER_FILE_NAME = "banner.bin";

// Puts the blanker in the header's MD5 field and returns the header's MD5.
Md5 HashWithBlanker(Header& header)
{
  header.md5 = MD5_BLANKER;
  Md5 digest;
  mbedtls_md5_ret(reinterpret_cast<const u8*>(&header), sizeof(Header), digest.data());
  return digest;
}

// data.bin stores permissions as one byte: owner in bits 4-5, group in bits
// 2-3, others in bits 0-1.
constexpr FS::Modes ModesFromByte(u8 packed)
{
  return {FS::Mode((packed >> 4) & 3), FS::Mode((packed >> 2) & 3), FS::Mode(packed & 3)};
}

constexpr u8 ByteFromModes(const FS::Modes& modes)
{
  return static_cast<u8>(u8(modes.owner) << 4 | u8(modes.group) << 2 | u8(modes.other));
}

// A save in a title's data directory on the emulated NAND.
class NandStorage final : public Storage
{
public:
  NandStorage(FS::FileSystem* fs, u64 tid)
      : m_fs{fs}, m_tid{tid}, m_data_dir{Common::GetTitleDataPath(tid)}
  {
    // The save's files belong to the title's owner and group, which the
    // data directory carries.
    if (const auto dir = m_fs->GetMetadata(IOS::PID_KERNEL, IOS::PID_KERNEL, m_data_dir))
    {
      m_uid = dir->uid;
      m_gid = dir->gid;
    }
    CollectEntries(m_data_dir);
  }

  bool SaveExists() const override
  {
    if (!m_entries.empty())
      return true;
    return HaveOwner() && m_fs->GetMetadata(*m_uid, *m_gid, BannerPath()).has_value();
  }

  bool EraseSave() override
  {
    // The banner is not one of the listed entries.
    if (m_fs->Delete(IOS::PID_KERNEL, IOS::PID_KERNEL, BannerPath()) != FS::ResultCode::Success)
      return false;

    // Deleting a top-level directory takes its contents with it.
    for (const SaveFile& entry : m_entries)
    {
      if (entry.path.contains('/'))
        continue;
      if (m_fs->Delete(IOS::PID_KERNEL, IOS::PID_KERNEL, m_data_dir + "/" + entry.path) !=
          FS::ResultCode::Success)
      {
        return false;
      }
    }

    m_entries.clear();
    m_entries_size = 0;
    return true;
  }

  std::optional<Header> ReadHeader() override
  {
    if (!HaveOwner())
      return std::nullopt;

    const std::string path = BannerPath();
    const auto banner = m_fs->OpenFile(*m_uid, *m_gid, path, FS::Mode::Read);
    if (!banner)
      return std::nullopt;

    Header header{};
    header.banner_size = banner->GetStatus()->size;
    if (header.banner_size > sizeof(header.banner))
    {
      ERROR_LOG_FMT(CORE, "NandStorage::ReadHeader: {} corrupted banner_size: {:x}", path,
                    header.banner_size);
      return std::nullopt;
    }
    header.tid = m_tid;
    header.md5 = MD5_BLANKER;

    const u8 permissions = PermissionsOf(path);
    if (permissions == 0 || !banner->Read(header.banner, header.banner_size))
      return std::nullopt;
    header.permissions = permissions;
    // Exported banners never carry the "no copy" flag.
    header.banner[7] &= ~1;

    header.md5 = HashWithBlanker(header);
    return header;
  }

  std::optional<BkHeader> ReadBkHeader() override
  {
    BkHeader bk{};
    bk.size = BK_LISTED_SZ;
    bk.magic = BK_HDR_MAGIC;
    bk.ngid = NG_ID;
    bk.number_of_files = static_cast<u32>(m_entries.size());
    bk.size_of_files = m_entries_size;
    bk.total_size = m_entries_size + FULL_CERT_SZ;
    bk.tid = m_tid;
    return bk;
  }

  std::optional<std::vector<SaveFile>> ReadFiles() override { return m_entries; }

  bool WriteHeader(const Header& header) override
  {
    if (!HaveOwner())
      return false;
    const auto banner = m_fs->CreateAndOpenFile(*m_uid, *m_gid, BannerPath(),
                                                ModesFromByte(header.permissions));
    return banner && banner->Write(header.banner, header.banner_size);
  }

  // The NAND has no place for the backup header.
  bool WriteBkHeader(const BkHeader&) override { return true; }

  bool WriteFiles(std::span<const SaveFile> files) override
  {
    if (!HaveOwner())
      return false;

    for (const SaveFile& entry : files)
    {
      const FS::Modes modes = ModesFromByte(entry.mode);
      const std::string path = m_data_dir + '/' + entry.path;

      if (entry.type == SaveFile::Type::File)
      {
        // The file is created before its contents are produced.
        const auto handle = m_fs->CreateAndOpenFile(*m_uid, *m_gid, path, modes);
        const std::optional<std::vector<u8>>& contents = *entry.data;
        if (!contents || !handle || !handle->Write(contents->data(), contents->size()))
          return false;
        continue;
      }

      if (entry.type == SaveFile::Type::Directory)
      {
        // A file in the way cannot become a directory.
        const auto existing = m_fs->GetMetadata(*m_uid, *m_gid, path);
        if (existing && existing->is_file)
          return false;
        if (m_fs->CreateDirectory(*m_uid, *m_gid, path, 0, modes) != FS::ResultCode::Success)
          return false;
      }
    }
    return true;
  }

private:
  bool HaveOwner() const { return m_uid.has_value() && m_gid.has_value(); }

  std::string BannerPath() const { return m_data_dir + "/" + BANNER_FILE_NAME; }

  u8 PermissionsOf(const std::string& path) const
  {
    const auto metadata = m_fs->GetMetadata(*m_uid, *m_gid, path);
    return metadata ? ByteFromModes(metadata->modes) : 0;
  }

  // Lists a directory depth-first, recording each entry and adding its size
  // in data.bin to m_entries_size. A failed metadata lookup ends the walk of
  // that directory.
  void CollectEntries(const std::string& dir)
  {
    if (!HaveOwner())
      return;

    const auto names = m_fs->ReadDirectory(*m_uid, *m_gid, dir);
    if (!names)
      return;

    for (const std::string& name : *names)
    {
      if (name == BANNER_FILE_NAME)
        continue;

      const std::string path = dir + '/' + name;
      const auto metadata = m_fs->GetMetadata(*m_uid, *m_gid, path);
      if (!metadata)
        return;

      SaveFile entry;
      entry.mode = ByteFromModes(metadata->modes);
      entry.attributes = 0;
      entry.type = metadata->is_file ? SaveFile::Type::File : SaveFile::Type::Directory;
      entry.path = path.substr(m_data_dir.size() + 1);
      entry.data = [this, path]() { return LoadFile(path); };
      m_entries.push_back(std::move(entry));

      m_entries_size += sizeof(FileHDR);
      if (metadata->is_file)
        m_entries_size += static_cast<u32>(Common::AlignUp(metadata->size, BLOCK_SZ));
      else
        CollectEntries(path);
    }
  }

  std::optional<std::vector<u8>> LoadFile(const std::string& path) const
  {
    const auto handle = m_fs->OpenFile(*m_uid, *m_gid, path, FS::Mode::Read);
    if (!handle)
      return std::nullopt;
    std::vector<u8> contents(handle->GetStatus()->size);
    if (!handle->Read(contents.data(), contents.size()))
      return std::nullopt;
    return contents;
  }

  FS::FileSystem* m_fs = nullptr;
  u64 m_tid = 0;
  std::string m_data_dir;
  std::optional<u32> m_uid;
  std::optional<u16> m_gid;
  std::vector<SaveFile> m_entries;
  u32 m_entries_size = 0;
};

// An encrypted data.bin file as the System Menu exports it to SD:
// header, backup header, file entries, signatures and certificates.
class DataBinStorage final : public Storage
{
public:
  DataBinStorage(IOSC* iosc, const std::string& path, const char* mode)
      : m_iosc{*iosc}, m_header_iv{LoadSdIv()}
  {
    // Without the SD key and IV nothing can be read or written, so the file is left alone
    // (opening it for writing would empty it).
    if (!m_header_iv || !HasSdKey(m_iosc))
    {
      ERROR_LOG_FMT(CORE, "WiiSave: no SD key and IV (wii_keys.txt); {} is not used", path);
      m_header_iv.reset();
      return;
    }
    File::CreateFullPath(path);
    m_file = File::IOFile{path, mode};
  }

  bool SaveExists() const override { return m_file.GetSize() > 0; }

  bool EraseSave() override { return m_file.GetSize() == 0 || m_file.Resize(0); }

  std::optional<Header> ReadHeader() override
  {
    Header header;
    if (!m_header_iv || !m_file.Seek(0, File::SeekOrigin::Begin) || !m_file.ReadArray(&header, 1))
      return std::nullopt;

    Iv iv = *m_header_iv;
    u8* const raw = reinterpret_cast<u8*>(&header);
    m_iosc.Decrypt(IOSC::HANDLE_SD_KEY, iv.data(), raw, sizeof(Header), raw, IOS::PID_ES);

    // The banner is the fixed part followed by one to eight icons.
    const u32 banner_size = header.banner_size;
    const bool size_ok = banner_size >= FULL_BNR_MIN && banner_size <= FULL_BNR_MAX &&
                         (banner_size - BNR_SZ) % ICON_SZ == 0;
    if (!size_ok)
    {
      ERROR_LOG_FMT(CONSOLE, "Not a Wii save or read failure for file header size {:x}",
                    banner_size);
      return std::nullopt;
    }

    const Md5 stored = header.md5;
    const Md5 computed = HashWithBlanker(header);
    if (stored != computed)
    {
      ERROR_LOG_FMT(CONSOLE, "MD5 mismatch\n {:016x}{:016x} != {:016x}{:016x}",
                    Common::swap64(stored.data()), Common::swap64(stored.data() + 8),
                    Common::swap64(computed.data()), Common::swap64(computed.data() + 8));
      return std::nullopt;
    }
    return header;
  }

  std::optional<BkHeader> ReadBkHeader() override
  {
    BkHeader bk;
    m_file.Seek(sizeof(Header), File::SeekOrigin::Begin);
    if (!m_file.ReadArray(&bk, 1))
      return std::nullopt;
    const bool valid = bk.size == BK_LISTED_SZ && bk.magic == BK_HDR_MAGIC &&
                       bk.size_of_files + FULL_CERT_SZ == bk.total_size;
    if (!valid)
      return std::nullopt;
    return bk;
  }

  std::optional<std::vector<SaveFile>> ReadFiles() override
  {
    const std::optional<BkHeader> bk = ReadBkHeader();
    if (!bk || !m_file.Seek(FilesOffset(), File::SeekOrigin::Begin))
      return std::nullopt;

    std::vector<SaveFile> entries;
    for (u32 n = 0; n < bk->number_of_files; ++n)
    {
      FileHDR record;
      if (!m_file.ReadArray(&record, 1) || record.magic != FILE_HDR_MAGIC)
        return std::nullopt;

      const auto type = static_cast<SaveFile::Type>(record.type);
      if (type != SaveFile::Type::File && type != SaveFile::Type::Directory)
        return std::nullopt;

      SaveFile entry;
      entry.mode = record.permissions;
      entry.attributes = record.attrib;
      entry.type = type;
      entry.path.assign(record.name.data(), strnlen(record.name.data(), record.name.size()));

      if (type == SaveFile::Type::File)
      {
        // The contents follow the record, padded to whole blocks, and are
        // only read and decrypted when asked for.
        const u32 size = record.size;
        const u32 padded_size = Common::AlignUp<u32>(size, BLOCK_SZ);
        const u64 offset = m_file.Tell();
        entry.data = [this, size, padded_size, offset, iv = record.iv]() mutable {
          return LoadContents(offset, size, padded_size, iv);
        };
        m_file.Seek(offset + padded_size, File::SeekOrigin::Begin);
      }
      entries.push_back(std::move(entry));
    }
    return entries;
  }

  bool WriteHeader(const Header& header) override
  {
    if (!m_header_iv)
      return false;
    Header encrypted;
    Iv iv = *m_header_iv;
    m_iosc.Encrypt(IOSC::HANDLE_SD_KEY, iv.data(), reinterpret_cast<const u8*>(&header),
                   sizeof(Header), reinterpret_cast<u8*>(&encrypted), IOS::PID_ES);
    return m_file.Seek(0, File::SeekOrigin::Begin) && m_file.WriteArray(&encrypted, 1);
  }

  bool WriteBkHeader(const BkHeader& bk_header) override
  {
    return m_file.Seek(sizeof(Header), File::SeekOrigin::Begin) && m_file.WriteArray(&bk_header, 1);
  }

  bool WriteFiles(std::span<const SaveFile> files) override
  {
    if (!m_file.Seek(FilesOffset(), File::SeekOrigin::Begin))
      return false;

    for (const SaveFile& entry : files)
    {
      if (!WriteEntry(entry))
        return false;
    }

    if (!AppendSignatures())
    {
      ERROR_LOG_FMT(CORE, "WiiSave::WriteFiles: Failed to write signatures");
      return false;
    }
    return true;
  }

private:
  static constexpr u64 FilesOffset() { return sizeof(Header) + sizeof(BkHeader); }

  std::optional<std::vector<u8>> LoadContents(u64 offset, u32 size, u32 padded_size, Iv& iv)
  {
    std::vector<u8> contents(padded_size);
    if (!m_file.Seek(offset, File::SeekOrigin::Begin) ||
        !m_file.ReadBytes(contents.data(), padded_size))
    {
      return std::nullopt;
    }
    m_iosc.Decrypt(IOSC::HANDLE_SD_KEY, iv.data(), contents.data(), padded_size, contents.data(),
                   IOS::PID_ES);
    contents.resize(size);
    return contents;
  }

  // One record, followed by the encrypted contents for a file. The record
  // is written with an all-zero IV, which is also the IV the contents are
  // encrypted with.
  bool WriteEntry(const SaveFile& entry)
  {
    FileHDR record{};
    record.magic = FILE_HDR_MAGIC;
    record.permissions = entry.mode;
    record.attrib = entry.attributes;
    record.type = static_cast<u8>(entry.type);
    if (entry.path.length() > record.name.size())
      return false;
    std::strncpy(record.name.data(), entry.path.data(), record.name.size());

    std::optional<std::vector<u8>> contents;
    if (entry.type == SaveFile::Type::File)
    {
      contents = *entry.data;
      if (!contents)
        return false;
      record.size = static_cast<u32>(contents->size());
    }

    if (!m_file.WriteArray(&record, 1))
      return false;
    if (!contents)
      return true;

    std::vector<u8> padded(Common::AlignUp(contents->size(), BLOCK_SZ));
    std::ranges::copy(*contents, padded.begin());
    m_iosc.Encrypt(IOSC::HANDLE_SD_KEY, record.iv.data(), padded.data(), padded.size(),
                   padded.data(), IOS::PID_ES);
    return m_file.WriteBytes(padded.data(), padded.size());
  }

  // Signs the backup header and file area as the System Menu would, then
  // appends the signature, the end magic, the console certificate and the
  // signing certificate.
  bool AppendSignatures()
  {
    const std::optional<BkHeader> bk = ReadBkHeader();
    if (!bk)
      return false;

    const u32 signed_size = bk->size_of_files + sizeof(BkHeader);
    auto signed_data = std::make_unique<u8[]>(signed_size);
    m_file.Seek(sizeof(Header), File::SeekOrigin::Begin);
    if (!m_file.ReadBytes(signed_data.get(), signed_size))
      return false;
    const Common::SHA1::Digest digest = Common::SHA1::CalculateDigest(signed_data.get(), signed_size);
    signed_data.reset();

    IOS::CertECC ap_cert;
    Common::ec::Signature ap_signature;
    m_iosc.Sign(ap_signature.data(), reinterpret_cast<u8*>(&ap_cert), Titles::SYSTEM_MENU,
                digest.data(), static_cast<u32>(digest.size()));

    if (!m_file.Seek(0, File::SeekOrigin::End))
      return false;
    const u32 end_magic = Common::swap32(SIGNATURE_END_MAGIC);
    const IOS::CertECC device_cert = m_iosc.GetDeviceCertificate();
    return m_file.WriteArray(ap_signature.data(), ap_signature.size()) &&
           m_file.WriteArray(&end_magic, 1) && m_file.WriteArray(&device_cert, 1) &&
           m_file.WriteArray(&ap_cert, 1);
  }

  IOSC& m_iosc;
  std::optional<Iv> m_header_iv;  // empty without the owner's SD key and IV
  File::IOFile m_file;
};

CopyResult Fail(CopyResult result, const char* what)
{
  ERROR_LOG_FMT(CORE, "WiiSave::Copy: Failed to {}", what);
  return result;
}

CopyResult ExportWith(IOS::HLE::Kernel& ios, u64 tid, std::string_view export_path)
{
  // The directory name is the title's four-character game code.
  const std::string path =
      fmt::format("{}/private/wii/title/{}{}{}{}/data.bin", export_path,
                  static_cast<char>(tid >> 24), static_cast<char>(tid >> 16),
                  static_cast<char>(tid >> 8), static_cast<char>(tid));
  return Copy(MakeNandStorage(ios.GetFS().get(), tid).get(),
              MakeDataBinStorage(&ios.GetIOSC(), path, "w+b").get());
}
}  // namespace

void StorageDeleter::operator()(Storage* p) const
{
  delete p;
}

StoragePointer MakeNandStorage(FS::FileSystem* fs, u64 tid)
{
  return StoragePointer{new NandStorage{fs, tid}};
}

StoragePointer MakeDataBinStorage(IOSC* iosc, const std::string& path, const char* mode)
{
  return StoragePointer{new DataBinStorage{iosc, path, mode}};
}

CopyResult Copy(Storage* source, Storage* destination)
{
  // Read everything first, so a broken source leaves the destination alone.
  const std::optional<Header> header = source->ReadHeader();
  if (!header)
    return Fail(CopyResult::CorruptedSource, "read header");
  const std::optional<BkHeader> bk_header = source->ReadBkHeader();
  if (!bk_header)
    return Fail(CopyResult::CorruptedSource, "read bk header");
  const std::optional<std::vector<Storage::SaveFile>> files = source->ReadFiles();
  if (!files)
    return Fail(CopyResult::CorruptedSource, "read files");

  if (destination->SaveExists() && !destination->EraseSave())
    return Fail(CopyResult::Error, "erase existing save");

  if (!destination->WriteHeader(*header))
    return Fail(CopyResult::Error, "write header");
  if (!destination->WriteBkHeader(*bk_header))
    return Fail(CopyResult::Error, "write bk header");
  if (!destination->WriteFiles(*files))
    return Fail(CopyResult::Error, "write files");
  return CopyResult::Success;
}

CopyResult Import(const std::string& data_bin_path, const std::function<bool()>& can_overwrite)
{
  IOS::HLE::Kernel ios;
  // The player has been told why; Cancelled keeps the caller from adding a second, misleading
  // message about a damaged NAND.
  if (!CheckSdKeys(ios.GetIOSC()))
    return CopyResult::Cancelled;
  const StoragePointer data_bin = MakeDataBinStorage(&ios.GetIOSC(), data_bin_path, "rb");
  const std::optional<Header> header = data_bin->ReadHeader();
  if (!header)
  {
    ERROR_LOG_FMT(CORE, "WiiSave::Import: Failed to read header");
    return CopyResult::CorruptedSource;
  }

  // The title must be known to the NAND before its save can go there.
  if (!WiiUtils::EnsureTMDIsImported(*ios.GetFS(), ios.GetESCore(), header->tid))
  {
    ERROR_LOG_FMT(CORE, "WiiSave::Import: Failed to find or import TMD for title {:16x}",
                  header->tid);
    return CopyResult::TitleMissing;
  }

  const StoragePointer nand = MakeNandStorage(ios.GetFS().get(), header->tid);
  if (nand->SaveExists() && !can_overwrite())
    return CopyResult::Cancelled;
  return Copy(data_bin.get(), nand.get());
}

CopyResult Export(u64 tid, std::string_view export_path)
{
  IOS::HLE::Kernel ios;
  if (!CheckSdKeys(ios.GetIOSC()))
    return CopyResult::Error;
  return ExportWith(ios, tid, export_path);
}

size_t ExportAll(std::string_view export_path)
{
  IOS::HLE::Kernel ios;
  if (!CheckSdKeys(ios.GetIOSC()))
    return 0;
  size_t exported = 0;
  for (const u64 tid : ios.GetESCore().GetInstalledTitles())
  {
    if (ExportWith(ios, tid, export_path) == CopyResult::Success)
      ++exported;
  }
  return exported;
}
}  // namespace WiiSave
