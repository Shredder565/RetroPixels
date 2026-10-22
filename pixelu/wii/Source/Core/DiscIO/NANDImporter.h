// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <bitset>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Swap.h"

namespace DiscIO
{
// Unpacks a BootMii NAND backup (raw flash pages with ECC data, optionally followed by the
// OTP/SEEPROM key dump) into the emulated NAND folder.
class NANDImporter final
{
public:
  NANDImporter();
  ~NANDImporter();

  enum class Step
  {
    Loading,
    Extracting,
  };
  // Progress report. Returning true cancels the import.
  using UpdateCallback = std::function<bool(Step step, int cur, int max)>;

  // Writes the backup's file system to the configured NAND root. When the backup has no key dump
  // appended, get_otp_dump_path is asked for the path of a separate keys.bin.
  void ImportNANDBin(const std::string& path_to_bin, UpdateCallback update_callback,
                     const std::function<std::string()>& get_otp_dump_path);

  // Writes the three SSL certificates found in IOS13's boot content to the NAND root.
  bool ExtractCertificates();

  enum class Type
  {
    File = 1,
    Directory = 2,
  };

#pragma pack(push, 1)
  // An SFFS file system entry, as stored in a superblock.
  struct NANDFSTEntry
  {
    char name[12];  // not NUL-terminated when 12 characters long
    u8 mode;        // low two bits: Type
    u8 attr;
    Common::BigEndianValue<u16> sub;  // first cluster (file) or first child (directory)
    Common::BigEndianValue<u16> sib;  // next entry in the same directory, 0xffff = none
    Common::BigEndianValue<u32> size;
    Common::BigEndianValue<u32> uid;
    Common::BigEndianValue<u16> gid;
    Common::BigEndianValue<u32> x3;
  };
  static_assert(sizeof(NANDFSTEntry) == 0x20, "Wrong size");

  static constexpr u16 FSTEntryCount = 0x17FF;
  // One of the 16 copies of the file system metadata at the end of the NAND.
  struct NANDSuperblock
  {
    std::array<char, 4> magic;  // "SFFS"
    Common::BigEndianValue<u32> version;
    Common::BigEndianValue<u32> unknown;
    std::array<Common::BigEndianValue<u16>, 0x8000> fat;  // next cluster of each cluster
    std::array<NANDFSTEntry, FSTEntryCount> fst;
    std::array<u8, 0x14> pad;
  };
  static_assert(sizeof(NANDSuperblock) == 0x40000, "Wrong size");
#pragma pack(pop)

private:
  bool LoadBackup(const std::string& path_to_bin,
                  const std::function<std::string()>& get_otp_dump_path);
  bool SelectSuperblock();
  void WriteKeys();
  bool ExtractTree();
  bool ExtractSiblings(u16 first_entry, const std::string& parent_path,
                       std::bitset<FSTEntryCount>* seen);
  std::vector<u8> ReadFileClusters(const NANDFSTEntry& entry) const;

  std::string m_nand_root;
  std::vector<u8> m_flash;      // the data pages, ECC removed
  std::vector<u8> m_otp_dump;   // keys.bin contents
  std::unique_ptr<Common::AES::Context> m_cluster_cipher;
  std::unique_ptr<NANDSuperblock> m_superblock;
  UpdateCallback m_update_callback;
  u16 m_files_done = 0;
  u16 m_files_total = 0;
};
}  // namespace DiscIO

template <>
struct fmt::formatter<DiscIO::NANDImporter::NANDFSTEntry>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const DiscIO::NANDImporter::NANDFSTEntry& entry, FormatContext& ctx) const
  {
    const std::string_view name(entry.name, strnlen(entry.name, sizeof(entry.name)));
    return fmt::format_to(
        ctx.out(), "{:12.12} {:#010b} {:#04x} {:#06x} {:#06x} {:#010x} {:#010x} {:#06x} {:#010x}",
        name, entry.mode, entry.attr, entry.sub, entry.sib, entry.size, entry.uid, entry.gid,
        entry.x3);
  }
};
