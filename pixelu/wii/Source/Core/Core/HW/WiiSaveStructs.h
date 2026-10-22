// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Lazy.h"
#include "Common/Swap.h"

// On-disk records of the data.bin save format, and the storage interface
// that save copies go through.
namespace WiiSave
{
enum
{
  // File contents are stored in whole blocks of this size.
  BLOCK_SZ = 0x40,
  // A banner: fixed part of BNR_SZ bytes, then one to eight icons.
  ICON_SZ = 0x1200,
  BNR_SZ = 0x60a0,
  FULL_BNR_MIN = BNR_SZ + 1 * ICON_SZ,  // 0x72a0
  FULL_BNR_MAX = BNR_SZ + 8 * ICON_SZ,  // 0xf0a0
  // The size BkHeader declares for itself (the record is padded to 0x80).
  BK_LISTED_SZ = 0x70,
  SIG_SZ = 0x40,
  // Signature area after the files: signature, magic and two certificates.
  FULL_CERT_SZ = 0x3C0,

  BK_HDR_MAGIC = 0x426B0001,  // "Bk", version 1
  FILE_HDR_MAGIC = 0x03adf17e
};

#pragma pack(push, 1)
// The encrypted main header at offset 0.
struct Header
{
  Common::BigEndianValue<u64> tid;
  Common::BigEndianValue<u32> banner_size;
  u8 permissions;
  u8 unk1;
  // MD5 of this header with the MD5 field replaced by a fixed blanker.
  std::array<u8, 0x10> md5;
  Common::BigEndianValue<u16> unk2;
  u8 banner[FULL_BNR_MAX];
};
static_assert(sizeof(Header) == 0xf0c0, "Header has an incorrect size");

// The plain backup header that follows Header.
struct BkHeader
{
  Common::BigEndianValue<u32> size;
  Common::BigEndianValue<u32> magic;
  Common::BigEndianValue<u32> ngid;
  Common::BigEndianValue<u32> number_of_files;
  // Records plus padded contents of all entries.
  Common::BigEndianValue<u32> size_of_files;
  Common::BigEndianValue<u32> unk1;
  Common::BigEndianValue<u32> unk2;
  // size_of_files + FULL_CERT_SZ.
  Common::BigEndianValue<u32> total_size;
  std::array<u8, 64> unk3;
  Common::BigEndianValue<u64> tid;
  std::array<u8, 6> mac_address;
  std::array<u8, 0x12> padding;
};
static_assert(sizeof(BkHeader) == 0x80, "BkHeader has an incorrect size");

// One file or directory entry; a file's encrypted contents follow it.
struct FileHDR
{
  Common::BigEndianValue<u32> magic;
  Common::BigEndianValue<u32> size;
  u8 permissions;
  u8 attrib;
  u8 type;  // 1: file, 2: directory
  std::array<char, 0x40> name;
  std::array<u8, 5> padding;
  std::array<u8, 0x10> iv;
  std::array<u8, 0x20> unk;
};
static_assert(sizeof(FileHDR) == 0x80, "FileHDR has an incorrect size");
#pragma pack(pop)

// A place a save can be read from and written to.
class Storage
{
public:
  struct SaveFile
  {
    enum class Type : u8
    {
      File = 1,
      Directory = 2,
    };
    u8 mode = 0;
    u8 attributes = 0;
    Type type{};
    // Relative to the title's data directory.
    std::string path;
    // The contents, produced on first use; files only.
    Common::Lazy<std::optional<std::vector<u8>>> data;
  };

  virtual ~Storage() = default;
  virtual bool SaveExists() const = 0;
  virtual bool EraseSave() = 0;
  virtual std::optional<Header> ReadHeader() = 0;
  virtual std::optional<BkHeader> ReadBkHeader() = 0;
  virtual std::optional<std::vector<SaveFile>> ReadFiles() = 0;
  virtual bool WriteHeader(const Header& header) = 0;
  virtual bool WriteBkHeader(const BkHeader& bk_header) = 0;
  virtual bool WriteFiles(std::span<const SaveFile> files) = 0;
};
}  // namespace WiiSave
