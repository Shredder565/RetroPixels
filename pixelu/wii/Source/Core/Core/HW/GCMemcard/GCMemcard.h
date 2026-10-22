// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The GameCube memory card file system, as described in YAGCD chapter 12
// (memory card format) and the public libogc card headers.
//
// A card is a run of 8 KiB blocks:
//   block 0      header (serial, format time, size, encoding, checksums)
//   blocks 1, 2  two copies of the directory (127 entries of 64 bytes)
//   blocks 3, 4  two copies of the block allocation table (BAT)
//   blocks 5..   file data
// Each directory and BAT copy carries an update counter; the copy with the
// higher counter is the live one, and a change is written to the other copy.

#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/NandPaths.h"
#include "Common/Swap.h"
#include "Common/Timer.h"

#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Sram.h"

namespace File
{
class IOFile;
}

namespace Memcard
{
enum
{
  SLOT_A = 0,
  SLOT_B = 1,
};

enum class GCMemcardGetSaveDataRetVal
{
  SUCCESS,
  FAIL,
  NOMEMCARD,
};

enum class GCMemcardImportFileRetVal
{
  SUCCESS,
  NOMEMCARD,
  OUTOFDIRENTRIES,
  OUTOFBLOCKS,
  TITLEPRESENT,
};

enum class GCMemcardRemoveFileRetVal
{
  SUCCESS,
  NOMEMCARD,
  DELETE_FAIL,
};

enum class GCMemcardValidityIssues
{
  FAILED_TO_OPEN,
  IO_ERROR,
  INVALID_CARD_SIZE,
  INVALID_CHECKSUM,
  MISMATCHED_CARD_SIZE,
  FREE_BLOCK_MISMATCH,
  DIR_BAT_INCONSISTENT,
  DATA_IN_UNUSED_AREA,
  COUNT
};

// A set of GCMemcardValidityIssues. DATA_IN_UNUSED_AREA is the only one that
// does not make a card unusable.
class GCMemcardErrorCode
{
public:
  bool HasCriticalErrors() const;
  bool Test(GCMemcardValidityIssues code) const;
  void Set(GCMemcardValidityIssues code);
  GCMemcardErrorCode& operator|=(const GCMemcardErrorCode& other);

private:
  std::bitset<static_cast<size_t>(GCMemcardValidityIssues::COUNT)> m_errors;
};

struct GCMemcardAnimationFrameRGBA8
{
  std::vector<u32> image_data;
  u8 delay = 0;
};

constexpr u32 BLOCK_SIZE = 0x2000;

// Blocks per megabit of card capacity (16).
constexpr u32 MBIT_TO_BLOCKS = (1024 * 1024) / (BLOCK_SIZE * 8);

// Header, two directories and two BATs.
constexpr u32 MC_FST_BLOCKS = 0x05;

// Directory entries per card.
constexpr u8 DIRLEN = 0x7F;

// Length of each of the two comment strings, and of a file name.
constexpr u32 DENTRY_STRLEN = 0x20;

constexpr u32 DENTRY_SIZE = 0x40;

// BAT entries (u16 each): one per possible data block.
constexpr u16 BAT_SIZE = 0xFFB;

// Card capacities in megabits. The names give the number of usable blocks.
constexpr u16 MBIT_SIZE_MEMORY_CARD_59 = 0x04;
constexpr u16 MBIT_SIZE_MEMORY_CARD_123 = 0x08;
constexpr u16 MBIT_SIZE_MEMORY_CARD_251 = 0x10;
constexpr u16 MBIT_SIZE_MEMORY_CARD_507 = 0x20;
constexpr u16 MBIT_SIZE_MEMORY_CARD_1019 = 0x40;
constexpr u16 MBIT_SIZE_MEMORY_CARD_2043 = 0x80;

// Banner image: 96x32, CI8 or RGB5A3 (low two bits of m_banner_and_icon_flags).
constexpr u32 MEMORY_CARD_BANNER_WIDTH = 96;
constexpr u32 MEMORY_CARD_BANNER_HEIGHT = 32;
constexpr u8 MEMORY_CARD_BANNER_FORMAT_CI8 = 1;
constexpr u8 MEMORY_CARD_BANNER_FORMAT_RGB5A3 = 2;

// Icon: up to 8 frames of 32x32, format two bits per frame in m_icon_format.
constexpr u32 MEMORY_CARD_ICON_WIDTH = 32;
constexpr u32 MEMORY_CARD_ICON_HEIGHT = 32;
constexpr u32 MEMORY_CARD_ICON_ANIMATION_MAX_FRAMES = 8;
constexpr u8 MEMORY_CARD_ICON_FORMAT_CI8_SHARED_PALETTE = 1;
constexpr u8 MEMORY_CARD_ICON_FORMAT_RGB5A3 = 2;
constexpr u8 MEMORY_CARD_ICON_FORMAT_CI8_UNIQUE_PALETTE = 3;

// A CI8 palette has 256 RGB5A3 (u16) colours.
constexpr u32 MEMORY_CARD_CI8_PALETTE_ENTRIES = 256;

constexpr u32 MbitToFreeBlocks(u16 size_mb)
{
  return size_mb * MBIT_TO_BLOCKS - MC_FST_BLOCKS;
}

// One 8 KiB block; a new block is erased (all 0xFF), like flash.
struct GCMBlock
{
  GCMBlock();
  void Erase();
  std::array<u8, BLOCK_SIZE> m_block;
};
static_assert(sizeof(GCMBlock) == BLOCK_SIZE);
static_assert(std::is_trivially_copyable_v<GCMBlock>);

#pragma pack(push, 1)
// The meaningful start of the header block. GCI folders store only this and
// rebuild the rest of the header from it.
struct HeaderData
{
  // 0x00: card serial. (libogc calls the first 0x20 bytes, up to and
  // including m_dtv_status, the serial.)
  std::array<u8, 12> m_serial;

  // 0x0c: OSTime of formatting
  Common::BigEndianValue<u64> m_format_time;

  // 0x14: RTC bias from SRAM when formatted
  u32 m_sram_bias;

  // 0x18: SRAM language
  Common::BigEndianValue<u32> m_sram_language;

  // 0x1c: video DTV status at format time
  u32 m_dtv_status;

  // 0x20: slot the card was formatted in (0 = A, 1 = B)
  Common::BigEndianValue<u16> m_device_id;

  // 0x22: capacity in megabits
  Common::BigEndianValue<u16> m_size_mb;

  // 0x24: 0 = Windows-1252, 1 = Shift JIS
  Common::BigEndianValue<u16> m_encoding;
};
static_assert(std::is_trivially_copyable_v<HeaderData>);

void InitializeHeaderData(HeaderData* data, const CardFlashId& flash_id, u16 size_mbits,
                          bool shift_jis, u32 rtc_bias, u32 sram_language, u64 format_time);

bool operator==(const HeaderData& lhs, const HeaderData& rhs);

struct Header
{
  HeaderData m_data;

  // 0x26: unused, 0xFF
  std::array<u8, 468> m_unused_1;

  // 0x1fa: update counter (normally 0xFFFF; unused in practice)
  u16 m_update_counter;

  // 0x1fc: checksum and inverse checksum over 0x000-0x1fb
  u16 m_checksum;
  u16 m_checksum_inv;

  // 0x200: unused, 0xFF
  std::array<u8, 7680> m_unused_2;

  // An unformatted (all 0xFF) header.
  explicit Header();

  // A freshly formatted header.
  explicit Header(const CardFlashId& flash_id, u16 size_mbits, bool shift_jis, u32 rtc_bias,
                  u32 sram_language, u64 format_time);

  // A header rebuilt from saved HeaderData.
  explicit Header(const HeaderData& data);

  // The two serial words some games use to bind their saves to a card.
  std::pair<u32, u32> CalculateSerial() const;

  void FixChecksums();
  std::pair<u16, u16> CalculateChecksums() const;

  GCMemcardErrorCode CheckForErrors(u16 card_size_mbits) const;

  bool IsShiftJIS() const;
};
static_assert(sizeof(Header) == BLOCK_SIZE);
static_assert(std::is_trivially_copyable_v<Header>);

// One directory entry; also the 64-byte header of a .gci file.
struct DEntry
{
  DEntry();

  static constexpr std::array<u8, 4> UNINITIALIZED_GAMECODE{{0xFF, 0xFF, 0xFF, 0xFF}};

  // 0x00
  std::array<u8, 4> m_gamecode;

  // 0x04
  std::array<u8, 2> m_makercode;

  // 0x06: unused, 0xFF
  u8 m_unused_1;

  // 0x07: bits 0-1 banner format (1 = CI8, 2 = RGB5A3, 0 or 3 = none);
  // bit 2 icon animation order (0 = loop, 1 = ping-pong).
  u8 m_banner_and_icon_flags;

  // 0x08
  std::array<u8, DENTRY_STRLEN> m_filename;

  // 0x28: seconds since 2000-01-01 00:00
  Common::BigEndianValue<u32> m_modification_time;

  // 0x2c: offset of the banner/icon data in the file
  Common::BigEndianValue<u32> m_image_offset;

  // 0x30: icon format, two bits per frame (MEMORY_CARD_ICON_FORMAT_*)
  Common::BigEndianValue<u16> m_icon_format;

  // 0x32: frame duration, two bits per frame (0 = end, 1/2/3 = 4/8/12 frames)
  Common::BigEndianValue<u16> m_animation_speed;

  // 0x34: bit 2 public, bit 3 no copy, bit 4 no move
  u8 m_file_permissions;

  // 0x35
  u8 m_copy_counter;

  // 0x36: first block of the file (absolute block number)
  Common::BigEndianValue<u16> m_first_block;

  // 0x38: length in blocks
  Common::BigEndianValue<u16> m_block_count;

  // 0x3a: unused, 0xFFFF
  std::array<u8, 2> m_unused_2;

  // 0x3c: offset of the two comment strings in the file
  Common::BigEndianValue<u32> m_comments_address;
};
static_assert(sizeof(DEntry) == DENTRY_SIZE);
static_assert(std::is_trivially_copyable_v<DEntry>);

struct BlockAlloc;

struct Directory
{
  std::array<DEntry, DIRLEN> m_dir_entries;

  // 0x1fc0: unused, 0xFF
  std::array<u8, 0x3a> m_padding;

  // 0x1ffa
  Common::BigEndianValue<s16> m_update_counter;

  // 0x1ffc: checksums over 0x0000-0x1ffb
  u16 m_checksum;
  u16 m_checksum_inv;

  // An empty directory.
  Directory();

  // Stores `entry` at `index` (0-126) and updates the checksums.
  bool Replace(const DEntry& entry, size_t index);

  void FixChecksums();
  std::pair<u16, u16> CalculateChecksums() const;

  GCMemcardErrorCode CheckForErrors() const;

  GCMemcardErrorCode CheckForErrorsWithBat(const BlockAlloc& bat) const;
};
static_assert(sizeof(Directory) == BLOCK_SIZE);
static_assert(std::is_trivially_copyable_v<Directory>);

struct BlockAlloc
{
  // 0x0000: checksums over 0x0004-0x1fff
  u16 m_checksum;
  u16 m_checksum_inv;

  // 0x0004
  Common::BigEndianValue<s16> m_update_counter;

  // 0x0006
  Common::BigEndianValue<u16> m_free_blocks;

  // 0x0008
  Common::BigEndianValue<u16> m_last_allocated_block;

  // 0x000a: one entry per data block (block 5 first): 0 = free, 0xFFFF = last
  // block of a file, otherwise the next block of the file.
  std::array<Common::BigEndianValue<u16>, BAT_SIZE> m_map;

  explicit BlockAlloc(u16 size_mbits = MBIT_SIZE_MEMORY_CARD_2043);

  // Block numbers here are absolute (5 = first data block).
  u16 GetNextBlock(u16 block) const;
  u16 NextFreeBlock(u16 max_block, u16 starting_block = MC_FST_BLOCKS) const;
  bool ClearBlocks(u16 starting_block, u16 block_count);
  u16 AssignBlocksContiguous(u16 length);

  void FixChecksums();
  std::pair<u16, u16> CalculateChecksums() const;

  GCMemcardErrorCode CheckForErrors(u16 size_mbits) const;
};
static_assert(sizeof(BlockAlloc) == BLOCK_SIZE);
static_assert(std::is_trivially_copyable_v<BlockAlloc>);
#pragma pack(pop)

struct Savefile
{
  DEntry dir_entry;
  std::vector<GCMBlock> blocks;
};

// A whole raw card image, loaded into memory.
class GCMemcard
{
private:
  bool m_valid;
  std::string m_filename;

  u32 m_size_blocks;
  u16 m_size_mb;

  Header m_header_block;
  std::array<Directory, 2> m_directory_blocks;
  std::array<BlockAlloc, 2> m_bat_blocks;
  std::vector<GCMBlock> m_data_blocks;

  int m_active_directory;
  int m_active_bat;

  GCMemcard();

  const Directory& GetActiveDirectory() const;
  const BlockAlloc& GetActiveBat() const;

  void UpdateDirectory(const Directory& directory);
  void UpdateBat(const BlockAlloc& bat);

public:
  static std::optional<GCMemcard> Create(std::string filename, const CardFlashId& flash_id,
                                         u16 size_mbits, bool shift_jis, u32 rtc_bias,
                                         u32 sram_language, u64 format_time);

  static std::pair<GCMemcardErrorCode, std::optional<GCMemcard>> Open(std::string filename);

  GCMemcard(const GCMemcard&) = delete;
  GCMemcard& operator=(const GCMemcard&) = delete;
  GCMemcard(GCMemcard&&) = default;
  GCMemcard& operator=(GCMemcard&&) = default;

  bool IsValid() const { return m_valid; }
  bool IsShiftJIS() const;
  bool Save();
  bool Format(const CardFlashId& flash_id, u16 size_mbits, bool shift_jis, u32 rtc_bias,
              u32 sram_language, u64 format_time);
  static bool Format(u8* card_data, const CardFlashId& flash_id, u16 size_mbits, bool shift_jis,
                     u32 rtc_bias, u32 sram_language, u64 format_time);
  static s32 FZEROGX_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                       std::vector<GCMBlock>& FileBuffer);
  static s32 PSO_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                   std::vector<GCMBlock>& FileBuffer);

  bool FixChecksums();

  // Number of used directory entries.
  u8 GetNumFiles() const;
  // Directory index of the fileNumber-th used entry, or 0xFF.
  u8 GetFileIndex(u8 fileNumber) const;

  u16 GetFreeBlocks() const;

  // Directory index of a save with the same identity as `d`, if any.
  std::optional<u8> TitlePresent(const DEntry& d) const;

  // Per-entry accessors; index is a directory index below DIRLEN.
  bool DEntry_IsPingPong(u8 index) const;
  u16 DEntry_FirstBlock(u8 index) const;
  u16 DEntry_BlockCount(u8 index) const;

  std::optional<std::vector<u8>>
  GetSaveDataBytes(u8 save_index, size_t offset = 0,
                   size_t length = std::numeric_limits<size_t>::max()) const;

  // The two comment lines the IPL shows for a save (title, then detail),
  // converted to UTF-8.
  std::optional<std::pair<std::string, std::string>> GetSaveComments(u8 index) const;

  std::optional<DEntry> GetDEntry(u8 index) const;

  GCMemcardGetSaveDataRetVal GetSaveData(u8 index, std::vector<GCMBlock>& saveBlocks) const;

  GCMemcardImportFileRetVal ImportFile(const Savefile& savefile);

  std::optional<Savefile> ExportFile(u8 index) const;

  GCMemcardRemoveFileRetVal RemoveFile(u8 index);

  std::optional<std::vector<u32>> ReadBannerRGBA8(u8 index) const;

  std::optional<std::vector<GCMemcardAnimationFrameRGBA8>> ReadAnimRGBA8(u8 index) const;
};
}  // namespace Memcard
