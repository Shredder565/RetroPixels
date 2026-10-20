// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCMemcard/GCMemcard.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/ColorUtil.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/HW/GCMemcard/GCMemcardUtils.h"

namespace Memcard
{
namespace
{
constexpr u64 BYTES_PER_MBIT = 1024 * 1024 / 8;

constexpr std::array<u16, 6> CARD_SIZES_MBIT{
    MBIT_SIZE_MEMORY_CARD_59,  MBIT_SIZE_MEMORY_CARD_123,  MBIT_SIZE_MEMORY_CARD_251,
    MBIT_SIZE_MEMORY_CARD_507, MBIT_SIZE_MEMORY_CARD_1019, MBIT_SIZE_MEMORY_CARD_2043,
};

// The card's checksum pair over a big-endian u16 area: the plain sum of the
// words and the sum of their complements. 0xFFFF is never stored (it would
// look like erased flash); it becomes 0. Results are returned in the order
// and byte order they are stored in.
std::pair<u16, u16> SumWords(const u8* data, size_t size)
{
  ASSERT(size % 2 == 0);
  u16 sum = 0;
  u16 sum_of_complements = 0;
  for (size_t i = 0; i < size; i += 2)
  {
    const u16 word = static_cast<u16>((data[i] << 8) | data[i + 1]);
    sum += word;
    sum_of_complements += static_cast<u16>(~word);
  }
  if (sum == 0xFFFF)
    sum = 0;
  if (sum_of_complements == 0xFFFF)
    sum_of_complements = 0;
  return {Common::swap16(sum), Common::swap16(sum_of_complements)};
}

template <typename Block>
const u8* BytesOf(const Block& block)
{
  static_assert(std::is_trivially_copyable_v<Block>);
  return reinterpret_cast<const u8*>(&block);
}

bool IsAllErased(std::span<const u8> bytes)
{
  return std::ranges::all_of(bytes, [](u8 b) { return b == 0xFF; });
}

// File names are compared as C strings within their 32-byte field.
bool FilenameIs(const DEntry& entry, std::string_view name)
{
  const auto& field = entry.m_filename;
  if (name.size() >= field.size())
    return false;
  return std::memcmp(field.data(), name.data(), name.size()) == 0 && field[name.size()] == 0;
}

void PutBE16(u8* where, u16 value)
{
  where[0] = static_cast<u8>(value >> 8);
  where[1] = static_cast<u8>(value);
}

void PutBE32(u8* where, u32 value)
{
  for (int i = 0; i < 4; ++i)
    where[i] = static_cast<u8>(value >> (24 - 8 * i));
}

void PutNative32(u8* where, u32 value)
{
  std::memcpy(where, &value, sizeof(value));
}
}  // namespace

// ---- GCMemcardErrorCode ---------------------------------------------------------------------

bool GCMemcardErrorCode::HasCriticalErrors() const
{
  GCMemcardErrorCode harmless;
  harmless.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);
  return (m_errors & ~harmless.m_errors).any();
}

bool GCMemcardErrorCode::Test(GCMemcardValidityIssues code) const
{
  return m_errors.test(static_cast<size_t>(code));
}

void GCMemcardErrorCode::Set(GCMemcardValidityIssues code)
{
  m_errors.set(static_cast<size_t>(code));
}

GCMemcardErrorCode& GCMemcardErrorCode::operator|=(const GCMemcardErrorCode& other)
{
  m_errors |= other.m_errors;
  return *this;
}

// ---- GCMBlock / DEntry / HeaderData ---------------------------------------------------------

GCMBlock::GCMBlock()
{
  Erase();
}

void GCMBlock::Erase()
{
  m_block.fill(0xFF);
}

DEntry::DEntry()
{
  std::memset(static_cast<void*>(this), 0xFF, sizeof(*this));
}

// The formatting procedure of the GameCube SDK. The serial is derived from the
// flash ID with the SDK's LCG seeded by the format time; changing any of this
// would make cards unreadable to games that check the serial.
void InitializeHeaderData(HeaderData* data, const CardFlashId& flash_id, u16 size_mbits,
                          bool shift_jis, u32 rtc_bias, u32 sram_language, u64 format_time)
{
  const auto lcg = [](u64 x) -> u64 { return (x * 0x41C64E6DULL + 0x3039ULL) >> 16; };

  u64 state = format_time;
  for (size_t i = 0; i < data->m_serial.size(); ++i)
  {
    state = lcg(state);
    data->m_serial[i] = static_cast<u8>(flash_id[i] + state);
    state = lcg(state) & 0x7FFF;
  }

  data->m_format_time = format_time;
  data->m_sram_bias = rtc_bias;
  data->m_sram_language = sram_language;
  // 0 is accepted in both slots.
  data->m_dtv_status = 0;
  data->m_device_id = 0;
  data->m_size_mb = size_mbits;
  data->m_encoding = shift_jis ? 1 : 0;
}

bool operator==(const HeaderData& lhs, const HeaderData& rhs)
{
  return std::memcmp(&lhs, &rhs, sizeof(HeaderData)) == 0;
}

// ---- Header ---------------------------------------------------------------------------------

Header::Header()
{
  std::memset(static_cast<void*>(this), 0xFF, sizeof(*this));
}

Header::Header(const CardFlashId& flash_id, u16 size_mbits, bool shift_jis, u32 rtc_bias,
               u32 sram_language, u64 format_time)
    : Header()
{
  InitializeHeaderData(&m_data, flash_id, size_mbits, shift_jis, rtc_bias, sram_language,
                       format_time);
  FixChecksums();
}

Header::Header(const HeaderData& data) : Header()
{
  m_data = data;
  FixChecksums();
}

// XOR of the eight 32-bit words at the start of the header, in two interleaved
// lanes, read in host byte order.
std::pair<u32, u32> Header::CalculateSerial() const
{
  const u8* bytes = BytesOf(*this);
  u32 lane[2] = {0, 0};
  for (size_t word = 0; word < 8; ++word)
  {
    u32 value;
    std::memcpy(&value, bytes + word * 4, sizeof(value));
    lane[word & 1] ^= value;
  }
  return {lane[0], lane[1]};
}

void Header::FixChecksums()
{
  const auto [sum, inverse] = CalculateChecksums();
  m_checksum = sum;
  m_checksum_inv = inverse;
}

std::pair<u16, u16> Header::CalculateChecksums() const
{
  return SumWords(BytesOf(*this), offsetof(Header, m_checksum));
}

GCMemcardErrorCode Header::CheckForErrors(u16 card_size_mbits) const
{
  GCMemcardErrorCode result;

  if (m_data.m_size_mb != card_size_mbits)
    result.Set(GCMemcardValidityIssues::MISMATCHED_CARD_SIZE);

  if (!IsAllErased(m_unused_1) || !IsAllErased(m_unused_2))
    result.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);

  if (CalculateChecksums() != std::pair{m_checksum, m_checksum_inv})
    result.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  return result;
}

bool Header::IsShiftJIS() const
{
  return m_data.m_encoding != 0;
}

// ---- Directory ------------------------------------------------------------------------------

Directory::Directory()
{
  std::memset(static_cast<void*>(this), 0xFF, sizeof(*this));
  m_update_counter = 0;
  FixChecksums();
}

bool Directory::Replace(const DEntry& entry, size_t index)
{
  if (index >= m_dir_entries.size())
    return false;
  m_dir_entries[index] = entry;
  FixChecksums();
  return true;
}

void Directory::FixChecksums()
{
  const auto [sum, inverse] = CalculateChecksums();
  m_checksum = sum;
  m_checksum_inv = inverse;
}

std::pair<u16, u16> Directory::CalculateChecksums() const
{
  return SumWords(BytesOf(*this), offsetof(Directory, m_checksum));
}

GCMemcardErrorCode Directory::CheckForErrors() const
{
  GCMemcardErrorCode result;
  if (CalculateChecksums() != std::pair{m_checksum, m_checksum_inv})
    result.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);
  if (!IsAllErased(m_padding))
    result.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);
  return result;
}

// Every file's chain in the BAT must end (0xFFFF) after exactly the number of
// blocks its directory entry claims.
GCMemcardErrorCode Directory::CheckForErrorsWithBat(const BlockAlloc& bat) const
{
  GCMemcardErrorCode result;

  const auto chain_matches = [&bat](const DEntry& entry) {
    u16 block = entry.m_first_block;
    for (u32 left = entry.m_block_count; left != 0; --left)
    {
      const u16 next = bat.GetNextBlock(block);
      if (next == 0)
        return false;
      if (next == 0xFFFF)
        return left == 1;
      block = next;
    }
    return false;
  };

  for (const DEntry& entry : m_dir_entries)
  {
    if (entry.m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
      continue;
    if (!chain_matches(entry))
    {
      result.Set(GCMemcardValidityIssues::DIR_BAT_INCONSISTENT);
      break;
    }
  }
  return result;
}

// ---- BlockAlloc -----------------------------------------------------------------------------

BlockAlloc::BlockAlloc(u16 size_mbits)
{
  std::memset(static_cast<void*>(this), 0, sizeof(*this));
  m_free_blocks = static_cast<u16>(MbitToFreeBlocks(size_mbits));
  m_last_allocated_block = MC_FST_BLOCKS - 1;
  FixChecksums();
}

u16 BlockAlloc::GetNextBlock(u16 block) const
{
  // Blocks above 4091 always read as unallocated.
  if (block < MC_FST_BLOCKS || block > 4091)
    return 0;
  return m_map[block - MC_FST_BLOCKS];
}

u16 BlockAlloc::NextFreeBlock(u16 max_block, u16 starting_block) const
{
  if (m_free_blocks == 0)
    return 0xFFFF;

  constexpr u16 lowest = MC_FST_BLOCKS;
  constexpr u16 highest = BAT_SIZE + MC_FST_BLOCKS;
  const u16 begin = std::clamp<u16>(starting_block, lowest, highest);
  const u16 end = std::clamp<u16>(max_block, lowest, highest);

  const auto is_free = [this](u16 block) { return m_map[block - MC_FST_BLOCKS] == 0; };
  // Search from the starting block up, then wrap around to the beginning.
  for (u16 block = begin; block < end; ++block)
  {
    if (is_free(block))
      return block;
  }
  for (u16 block = lowest; block < begin; ++block)
  {
    if (is_free(block))
      return block;
  }
  return 0xFFFF;
}

bool BlockAlloc::ClearBlocks(u16 starting_block, u16 block_count)
{
  std::vector<u16> chain;
  u16 block = starting_block;
  while (block != 0 && block != 0xFFFF)
  {
    // A chain can't be longer than the BAT; anything longer loops.
    if (chain.size() > BAT_SIZE)
      return false;
    chain.push_back(block);
    block = GetNextBlock(block);
  }

  // The chain must end properly and have the expected length.
  if (block == 0 || chain.size() != block_count)
    return false;

  for (const u16 b : chain)
    m_map[b - MC_FST_BLOCKS] = 0;
  m_free_blocks = m_free_blocks + block_count;
  return true;
}

void BlockAlloc::FixChecksums()
{
  const auto [sum, inverse] = CalculateChecksums();
  m_checksum = sum;
  m_checksum_inv = inverse;
}

// Allocates `length` blocks directly after the last allocated one, as one
// chain. Used when building a virtual card, where free space is never
// fragmented.
u16 BlockAlloc::AssignBlocksContiguous(u16 length)
{
  if (length > m_free_blocks)
    return 0xFFFF;

  const u16 first = m_last_allocated_block + 1;
  const u16 last = length > 1 ? static_cast<u16>(first + length - 1) : first;
  for (u16 block = first; block != last; ++block)
    m_map[block - MC_FST_BLOCKS] = block + 1;
  m_map[last - MC_FST_BLOCKS] = 0xFFFF;

  m_last_allocated_block = last;
  m_free_blocks = m_free_blocks - length;
  FixChecksums();
  return first;
}

std::pair<u16, u16> BlockAlloc::CalculateChecksums() const
{
  constexpr size_t start = offsetof(BlockAlloc, m_update_counter);
  return SumWords(BytesOf(*this) + start, sizeof(BlockAlloc) - start);
}

GCMemcardErrorCode BlockAlloc::CheckForErrors(u16 size_mbits) const
{
  GCMemcardErrorCode result;

  if (CalculateChecksums() != std::pair{m_checksum, m_checksum_inv})
    result.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  // The BAT can describe at most 256 Mbit.
  if (size_mbits == 0 || size_mbits > 256)
  {
    result.Set(GCMemcardValidityIssues::INVALID_CARD_SIZE);
    return result;
  }

  const size_t data_blocks = size_t{size_mbits} * MBIT_TO_BLOCKS - MC_FST_BLOCKS;
  ASSERT(data_blocks <= m_map.size());

  const auto is_used = [](const Common::BigEndianValue<u16>& v) { return v != 0; };
  const auto used =
      static_cast<size_t>(std::count_if(m_map.begin(), m_map.begin() + data_blocks, is_used));
  if (static_cast<u16>(data_blocks - used) != m_free_blocks)
    result.Set(GCMemcardValidityIssues::FREE_BLOCK_MISMATCH);

  // Entries for blocks the card doesn't have must be clear.
  if (std::any_of(m_map.begin() + data_blocks, m_map.end(), is_used))
    result.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);

  return result;
}

// ---- GCMemcard ------------------------------------------------------------------------------

GCMemcard::GCMemcard()
    : m_valid(false), m_size_blocks(0), m_size_mb(0), m_active_directory(0), m_active_bat(0)
{
}

std::optional<GCMemcard> GCMemcard::Create(std::string filename, const CardFlashId& flash_id,
                                           u16 size_mbits, bool shift_jis, u32 rtc_bias,
                                           u32 sram_language, u64 format_time)
{
  GCMemcard card;
  card.m_filename = std::move(filename);
  // Format() also writes the new card to m_filename.
  if (!card.Format(flash_id, size_mbits, shift_jis, rtc_bias, sram_language, format_time))
    return std::nullopt;
  return card;
}

std::pair<GCMemcardErrorCode, std::optional<GCMemcard>> GCMemcard::Open(std::string filename)
{
  GCMemcardErrorCode errors;
  const auto fail = [&errors](std::optional<GCMemcardValidityIssues> issue = {}) {
    if (issue)
      errors.Set(*issue);
    return std::pair<GCMemcardErrorCode, std::optional<GCMemcard>>{errors, std::nullopt};
  };

  File::IOFile file(filename, "rb");
  if (!file.IsOpen())
    return fail(GCMemcardValidityIssues::FAILED_TO_OPEN);

  // The file must be exactly one of the card sizes.
  const u64 file_size = file.GetSize();
  const bool whole_mbits = file_size % BYTES_PER_MBIT == 0;
  const u64 mbits = whole_mbits ? file_size / BYTES_PER_MBIT : 0;
  if (std::ranges::find(CARD_SIZES_MBIT, mbits) == CARD_SIZES_MBIT.end())
    return fail(GCMemcardValidityIssues::INVALID_CARD_SIZE);

  const u16 size_mb = static_cast<u16>(mbits);
  const u16 size_blocks = size_mb * MBIT_TO_BLOCKS;

  GCMemcard card;
  file.Seek(0, File::SeekOrigin::Begin);
  const bool read_ok = file.ReadBytes(&card.m_header_block, BLOCK_SIZE) &&
                       file.ReadBytes(&card.m_directory_blocks[0], BLOCK_SIZE) &&
                       file.ReadBytes(&card.m_directory_blocks[1], BLOCK_SIZE) &&
                       file.ReadBytes(&card.m_bat_blocks[0], BLOCK_SIZE) &&
                       file.ReadBytes(&card.m_bat_blocks[1], BLOCK_SIZE);
  if (!read_ok)
    return fail(GCMemcardValidityIssues::IO_ERROR);

  card.m_data_blocks.resize(size_blocks - MC_FST_BLOCKS);
  for (GCMBlock& block : card.m_data_blocks)
  {
    if (!file.ReadBytes(block.m_block.data(), BLOCK_SIZE))
      return fail(GCMemcardValidityIssues::IO_ERROR);
  }
  file.Close();

  card.m_filename = std::move(filename);
  card.m_size_blocks = size_blocks;
  card.m_size_mb = size_mb;

  const GCMemcardErrorCode header_errors = card.m_header_block.CheckForErrors(size_mb);
  errors |= header_errors;
  if (header_errors.HasCriticalErrors())
    return fail();

  // How the IPL treats the four system blocks: one damaged directory or BAT
  // copy is repaired from its twin (with the update counter bumped); two or
  // more damaged copies, in any combination, make the whole card corrupt.
  // Damage here means a problem inside the block itself (checksum, or the
  // BAT's free count). Consistency between the chosen directory and BAT is
  // checked afterwards and never triggers a repair.
  std::array<GCMemcardErrorCode, 2> dir_errors{card.m_directory_blocks[0].CheckForErrors(),
                                               card.m_directory_blocks[1].CheckForErrors()};
  std::array<GCMemcardErrorCode, 2> bat_errors{card.m_bat_blocks[0].CheckForErrors(size_mb),
                                               card.m_bat_blocks[1].CheckForErrors(size_mb)};

  const int damaged = dir_errors[0].HasCriticalErrors() + dir_errors[1].HasCriticalErrors() +
                      bat_errors[0].HasCriticalErrors() + bat_errors[1].HasCriticalErrors();

  const auto merge_all = [&] {
    for (const auto& e : dir_errors)
      errors |= e;
    for (const auto& e : bat_errors)
      errors |= e;
  };

  if (damaged > 1)
  {
    merge_all();
    return fail();
  }

  if (damaged == 1)
  {
    bool repaired = false;
    for (size_t bad = 0; bad < 2 && !repaired; ++bad)
    {
      if (!dir_errors[bad].HasCriticalErrors())
        continue;
      Directory& target = card.m_directory_blocks[bad];
      target = card.m_directory_blocks[1 - bad];
      target.m_update_counter = target.m_update_counter + 1;
      target.FixChecksums();
      dir_errors[bad] = target.CheckForErrors();
      repaired = true;
    }
    for (size_t bad = 0; bad < 2 && !repaired; ++bad)
    {
      if (!bat_errors[bad].HasCriticalErrors())
        continue;
      BlockAlloc& target = card.m_bat_blocks[bad];
      target = card.m_bat_blocks[1 - bad];
      target.m_update_counter = target.m_update_counter + 1;
      target.FixChecksums();
      bat_errors[bad] = target.CheckForErrors(size_mb);
      repaired = true;
    }
    ASSERT(repaired);
  }

  merge_all();

  // The newer copy (by signed update counter, ties going to block 0) is live.
  // Like the IPL, this does not handle the counter wrapping around.
  card.m_active_directory =
      card.m_directory_blocks[0].m_update_counter >= card.m_directory_blocks[1].m_update_counter ?
          0 :
          1;
  card.m_active_bat =
      card.m_bat_blocks[0].m_update_counter >= card.m_bat_blocks[1].m_update_counter ? 0 : 1;

  errors |= card.GetActiveDirectory().CheckForErrorsWithBat(card.GetActiveBat());
  if (errors.HasCriticalErrors())
    return fail();

  card.m_valid = true;
  return {errors, std::move(card)};
}

const Directory& GCMemcard::GetActiveDirectory() const
{
  return m_directory_blocks[static_cast<size_t>(m_active_directory)];
}

const BlockAlloc& GCMemcard::GetActiveBat() const
{
  return m_bat_blocks[static_cast<size_t>(m_active_bat)];
}

// Changes go to the inactive copy, which then becomes the active one.
void GCMemcard::UpdateDirectory(const Directory& directory)
{
  m_active_directory ^= 1;
  m_directory_blocks[static_cast<size_t>(m_active_directory)] = directory;
}

void GCMemcard::UpdateBat(const BlockAlloc& bat)
{
  m_active_bat ^= 1;
  m_bat_blocks[static_cast<size_t>(m_active_bat)] = bat;
}

bool GCMemcard::IsShiftJIS() const
{
  return m_header_block.IsShiftJIS();
}

bool GCMemcard::Save()
{
  File::IOFile out(m_filename, "wb");
  out.Seek(0, File::SeekOrigin::Begin);

  out.WriteBytes(&m_header_block, BLOCK_SIZE);
  for (const Directory& dir : m_directory_blocks)
    out.WriteBytes(&dir, BLOCK_SIZE);
  for (const BlockAlloc& bat : m_bat_blocks)
    out.WriteBytes(&bat, BLOCK_SIZE);

  const size_t data_blocks = m_size_blocks - MC_FST_BLOCKS;
  for (size_t i = 0; i < data_blocks; ++i)
    out.WriteBytes(m_data_blocks[i].m_block.data(), BLOCK_SIZE);

  return out.Close();
}

bool GCMemcard::FixChecksums()
{
  if (!m_valid)
    return false;

  m_header_block.FixChecksums();
  for (Directory& dir : m_directory_blocks)
    dir.FixChecksums();
  for (BlockAlloc& bat : m_bat_blocks)
    bat.FixChecksums();
  return true;
}

u8 GCMemcard::GetNumFiles() const
{
  if (!m_valid)
    return 0;

  const auto& entries = GetActiveDirectory().m_dir_entries;
  return static_cast<u8>(std::ranges::count_if(entries, [](const DEntry& e) {
    return e.m_gamecode != DEntry::UNINITIALIZED_GAMECODE;
  }));
}

u8 GCMemcard::GetFileIndex(u8 fileNumber) const
{
  if (!m_valid)
    return 0xFF;

  u8 seen = 0;
  const auto& entries = GetActiveDirectory().m_dir_entries;
  for (u8 index = 0; index < DIRLEN; ++index)
  {
    if (entries[index].m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
      continue;
    if (seen == fileNumber)
      return index;
    ++seen;
  }
  return 0xFF;
}

u16 GCMemcard::GetFreeBlocks() const
{
  return m_valid ? u16{GetActiveBat().m_free_blocks} : u16{0};
}

std::optional<u8> GCMemcard::TitlePresent(const DEntry& d) const
{
  if (!m_valid)
    return std::nullopt;

  const auto& entries = GetActiveDirectory().m_dir_entries;
  for (u8 index = 0; index < DIRLEN; ++index)
  {
    if (HasSameIdentity(entries[index], d))
      return index;
  }
  return std::nullopt;
}

bool GCMemcard::DEntry_IsPingPong(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return false;
  return (GetActiveDirectory().m_dir_entries[index].m_banner_and_icon_flags & 0x04) != 0;
}

u16 GCMemcard::DEntry_FirstBlock(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;
  const u16 block = GetActiveDirectory().m_dir_entries[index].m_first_block;
  return block > static_cast<u16>(m_size_blocks) ? u16{0xFFFF} : block;
}

u16 GCMemcard::DEntry_BlockCount(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;
  const u16 count = GetActiveDirectory().m_dir_entries[index].m_block_count;
  return count > static_cast<u16>(m_size_blocks) ? u16{0xFFFF} : count;
}

std::optional<std::vector<u8>> GCMemcard::GetSaveDataBytes(u8 save_index, size_t offset,
                                                           size_t length) const
{
  if (!m_valid || save_index >= DIRLEN)
    return std::nullopt;

  const DEntry& entry = GetActiveDirectory().m_dir_entries[save_index];
  const BlockAlloc& bat = GetActiveBat();
  const size_t end_block = MC_FST_BLOCKS + m_data_blocks.size();
  const auto in_card = [end_block](u16 block) {
    return block >= MC_FST_BLOCKS && block < end_block;
  };

  const u16 block_count = entry.m_block_count;
  if (block_count == 0xFFFF || !in_card(entry.m_first_block))
    return std::nullopt;

  const size_t file_size = size_t{block_count} * BLOCK_SIZE;
  if (offset >= file_size)
    return std::nullopt;

  size_t remaining = std::min(length, file_size - offset);
  std::vector<u8> out;
  out.reserve(remaining);

  // Walk to the block holding `offset`.
  u16 block = entry.m_first_block;
  for (size_t skip = offset / BLOCK_SIZE; skip != 0; --skip)
  {
    block = bat.GetNextBlock(block);
    if (!in_card(block))
      return std::nullopt;
  }

  size_t in_block = offset % BLOCK_SIZE;
  for (;;)
  {
    const auto& data = m_data_blocks[block - MC_FST_BLOCKS].m_block;
    const size_t take = std::min(remaining, BLOCK_SIZE - in_block);
    out.insert(out.end(), data.begin() + in_block, data.begin() + in_block + take);
    remaining -= take;
    if (remaining == 0)
      return out;

    in_block = 0;
    block = bat.GetNextBlock(block);
    if (!in_card(block))
      return std::nullopt;
  }
}

std::optional<std::pair<std::string, std::string>> GCMemcard::GetSaveComments(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return std::nullopt;

  const u32 address = GetActiveDirectory().m_dir_entries[index].m_comments_address;
  if (address == 0xFFFFFFFF)
    return std::nullopt;

  const auto raw = GetSaveDataBytes(index, address, DENTRY_STRLEN * 2);
  if (!raw || raw->size() != DENTRY_STRLEN * 2)
    return std::nullopt;

  const bool sjis = IsShiftJIS();
  const auto decode = [sjis](const u8* text) {
    const std::string_view encoded(reinterpret_cast<const char*>(text), DENTRY_STRLEN);
    std::string utf8 = sjis ? SHIFTJISToUTF8(encoded) : CP1252ToUTF8(encoded);
    if (const size_t nul = utf8.find('\0'); nul != std::string::npos)
      utf8.resize(nul);
    return utf8;
  };

  return std::pair{decode(raw->data()), decode(raw->data() + DENTRY_STRLEN)};
}

std::optional<DEntry> GCMemcard::GetDEntry(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return std::nullopt;
  return GetActiveDirectory().m_dir_entries[index];
}

GCMemcardGetSaveDataRetVal GCMemcard::GetSaveData(u8 index, std::vector<GCMBlock>& Blocks) const
{
  if (!m_valid)
    return GCMemcardGetSaveDataRetVal::NOMEMCARD;

  u16 block = DEntry_FirstBlock(index);
  const u16 count = DEntry_BlockCount(index);
  if (block == 0xFFFF || count == 0xFFFF)
    return GCMemcardGetSaveDataRetVal::FAIL;

  for (u16 i = 0; i < count; ++i)
  {
    // Also refuses chains that leave the card (possible on a card whose BAT
    // has data in its unused area).
    if (block < MC_FST_BLOCKS || block == 0xFFFF ||
        static_cast<size_t>(block - MC_FST_BLOCKS) >= m_data_blocks.size())
    {
      return GCMemcardGetSaveDataRetVal::FAIL;
    }
    Blocks.push_back(m_data_blocks[block - MC_FST_BLOCKS]);
    block = GetActiveBat().GetNextBlock(block);
  }
  return GCMemcardGetSaveDataRetVal::SUCCESS;
}

GCMemcardImportFileRetVal GCMemcard::ImportFile(const Savefile& savefile)
{
  if (!m_valid)
    return GCMemcardImportFileRetVal::NOMEMCARD;

  const DEntry& entry = savefile.dir_entry;
  const u16 block_count = entry.m_block_count;

  if (GetNumFiles() >= DIRLEN)
    return GCMemcardImportFileRetVal::OUTOFDIRENTRIES;
  if (GetActiveBat().m_free_blocks < block_count)
    return GCMemcardImportFileRetVal::OUTOFBLOCKS;
  if (TitlePresent(entry))
    return GCMemcardImportFileRetVal::TITLEPRESENT;

  u16 block = GetActiveBat().NextFreeBlock(static_cast<u16>(m_size_blocks),
                                                  GetActiveBat().m_last_allocated_block);
  if (block == 0xFFFF)
    return GCMemcardImportFileRetVal::OUTOFBLOCKS;

  // New directory: the entry goes into the first free slot.
  Directory dir = GetActiveDirectory();
  const auto slot = std::ranges::find_if(dir.m_dir_entries, [](const DEntry& e) {
    return e.m_gamecode == DEntry::UNINITIALIZED_GAMECODE;
  });
  if (slot != dir.m_dir_entries.end())
  {
    *slot = entry;
    slot->m_first_block = block;
    slot->m_copy_counter = slot->m_copy_counter + 1;
  }
  dir.m_update_counter = dir.m_update_counter + 1;
  UpdateDirectory(dir);

  // Games that bind their saves to a card need them re-signed for this one.
  std::vector<GCMBlock> data = savefile.blocks;
  data.resize(std::max<size_t>(data.size(), block_count));
  FZEROGX_MakeSaveGameValid(m_header_block, entry, data);
  PSO_MakeSaveGameValid(m_header_block, entry, data);

  // New BAT: lay the file into free blocks in ascending order.
  BlockAlloc bat = GetActiveBat();
  for (u16 i = 0; i < block_count; ++i)
  {
    if (block == 0xFFFF)
    {
      PanicAlertFmt("Fatal Error");
      return GCMemcardImportFileRetVal::OUTOFBLOCKS;
    }
    m_data_blocks[block - MC_FST_BLOCKS] = data[i];
    const bool last = i + 1 == block_count;
    const u16 next = last ? u16{0xFFFF} : bat.NextFreeBlock(static_cast<u16>(m_size_blocks),
                                                                 static_cast<u16>(block + 1));
    bat.m_map[block - MC_FST_BLOCKS] = next;
    bat.m_last_allocated_block = block;
    block = next;
  }
  bat.m_free_blocks = bat.m_free_blocks - block_count;
  bat.m_update_counter = bat.m_update_counter + 1;
  UpdateBat(bat);

  FixChecksums();
  return GCMemcardImportFileRetVal::SUCCESS;
}

std::optional<Savefile> GCMemcard::ExportFile(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return std::nullopt;

  Savefile file;
  file.dir_entry = GetActiveDirectory().m_dir_entries[index];
  if (file.dir_entry.m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
    return std::nullopt;
  if (GetSaveData(index, file.blocks) != GCMemcardGetSaveDataRetVal::SUCCESS)
    return std::nullopt;
  return file;
}

GCMemcardRemoveFileRetVal GCMemcard::RemoveFile(u8 index)
{
  if (!m_valid)
    return GCMemcardRemoveFileRetVal::NOMEMCARD;
  if (index >= DIRLEN)
    return GCMemcardRemoveFileRetVal::DELETE_FAIL;

  const DEntry& entry = GetActiveDirectory().m_dir_entries[index];

  BlockAlloc bat = GetActiveBat();
  if (!bat.ClearBlocks(entry.m_first_block, entry.m_block_count))
    return GCMemcardRemoveFileRetVal::DELETE_FAIL;
  bat.m_update_counter = bat.m_update_counter + 1;
  UpdateBat(bat);

  // (The IPL sometimes leaves a hidden "Broken File000" entry behind when it
  // deletes; that isn't reproduced.)
  Directory dir = GetActiveDirectory();
  dir.m_dir_entries[index] = DEntry();
  dir.m_update_counter = dir.m_update_counter + 1;
  UpdateDirectory(dir);

  FixChecksums();
  return GCMemcardRemoveFileRetVal::SUCCESS;
}

std::optional<std::vector<u32>> GCMemcard::ReadBannerRGBA8(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return std::nullopt;

  const DEntry& entry = GetActiveDirectory().m_dir_entries[index];
  const u32 offset = entry.m_image_offset;
  if (offset == 0xFFFFFFFF)
    return std::nullopt;

  const u8 format = entry.m_banner_and_icon_flags & 0x03;
  constexpr u32 PIXELS = MEMORY_CARD_BANNER_WIDTH * MEMORY_CARD_BANNER_HEIGHT;
  constexpr u32 PALETTE_BYTES = MEMORY_CARD_CI8_PALETTE_ENTRIES * 2;

  size_t bytes;
  if (format == MEMORY_CARD_BANNER_FORMAT_CI8)
    bytes = PIXELS + PALETTE_BYTES;
  else if (format == MEMORY_CARD_BANNER_FORMAT_RGB5A3)
    bytes = PIXELS * 2;
  else
    return std::nullopt;

  const auto raw = GetSaveDataBytes(index, offset, bytes);
  if (!raw || raw->size() != bytes)
    return std::nullopt;

  std::vector<u32> rgba(PIXELS);
  if (format == MEMORY_CARD_BANNER_FORMAT_CI8)
  {
    std::array<u16, MEMORY_CARD_CI8_PALETTE_ENTRIES> palette;
    std::memcpy(palette.data(), raw->data() + PIXELS, PALETTE_BYTES);
    Common::DecodeCI8Image(rgba.data(), raw->data(), palette.data(), MEMORY_CARD_BANNER_WIDTH,
                           MEMORY_CARD_BANNER_HEIGHT);
  }
  else
  {
    std::vector<u16> pixels(PIXELS);
    std::memcpy(pixels.data(), raw->data(), PIXELS * 2);
    Common::Decode5A3Image(rgba.data(), pixels.data(), MEMORY_CARD_BANNER_WIDTH,
                           MEMORY_CARD_BANNER_HEIGHT);
  }
  return rgba;
}

// The icon follows the banner in the image data. Frames are listed until the
// first zero speed; each has its own format, and CI8 frames either carry their
// own palette or share one stored after the last frame. A frame with format 0
// shows the next frame that has image data (or nothing if none does), which
// is what the IPL displays for such saves.
std::optional<std::vector<GCMemcardAnimationFrameRGBA8>> GCMemcard::ReadAnimRGBA8(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return std::nullopt;

  const DEntry& entry = GetActiveDirectory().m_dir_entries[index];
  u32 offset = entry.m_image_offset;
  if (offset == 0xFFFFFFFF)
    return std::nullopt;

  constexpr u32 BANNER_PIXELS = MEMORY_CARD_BANNER_WIDTH * MEMORY_CARD_BANNER_HEIGHT;
  constexpr u32 FRAME_PIXELS = MEMORY_CARD_ICON_WIDTH * MEMORY_CARD_ICON_HEIGHT;
  constexpr u32 PALETTE_BYTES = MEMORY_CARD_CI8_PALETTE_ENTRIES * 2;

  switch (entry.m_banner_and_icon_flags & 0x03)
  {
  case MEMORY_CARD_BANNER_FORMAT_CI8:
    offset += BANNER_PIXELS + PALETTE_BYTES;
    break;
  case MEMORY_CARD_BANNER_FORMAT_RGB5A3:
    offset += BANNER_PIXELS * 2;
    break;
  default:
    break;
  }

  const u16 formats = entry.m_icon_format;
  const u16 speeds = entry.m_animation_speed;
  const auto format_of = [formats](u32 frame) { return static_cast<u32>((formats >> (2 * frame)) & 3); };
  const auto speed_of = [speeds](u32 frame) { return static_cast<u8>((speeds >> (2 * frame)) & 3); };

  if (format_of(0) == 0)
    return std::nullopt;

  struct FrameInfo
  {
    u32 format;
    u8 delay;
    u32 data_offset;
  };
  std::vector<FrameInfo> frames;
  u32 total = 0;
  bool shared_palette = false;
  for (u32 f = 0; f < MEMORY_CARD_ICON_ANIMATION_MAX_FRAMES && speed_of(f) != 0; ++f)
  {
    frames.push_back({format_of(f), speed_of(f), total});
    switch (format_of(f))
    {
    case MEMORY_CARD_ICON_FORMAT_CI8_SHARED_PALETTE:
      total += FRAME_PIXELS;
      shared_palette = true;
      break;
    case MEMORY_CARD_ICON_FORMAT_RGB5A3:
      total += FRAME_PIXELS * 2;
      break;
    case MEMORY_CARD_ICON_FORMAT_CI8_UNIQUE_PALETTE:
      total += FRAME_PIXELS + PALETTE_BYTES;
      break;
    default:
      break;
    }
  }
  if (frames.empty())
    return std::nullopt;

  const u32 shared_palette_offset = total;
  if (shared_palette)
    total += PALETTE_BYTES;

  const auto raw = GetSaveDataBytes(index, offset, total);
  if (!raw || raw->size() != total)
    return std::nullopt;

  std::array<u16, MEMORY_CARD_CI8_PALETTE_ENTRIES> common_palette{};
  if (shared_palette)
    std::memcpy(common_palette.data(), raw->data() + shared_palette_offset, PALETTE_BYTES);

  const auto decode_into = [&](u32* out, const FrameInfo& src) {
    const u8* pixels = raw->data() + src.data_offset;
    switch (src.format)
    {
    case MEMORY_CARD_ICON_FORMAT_CI8_SHARED_PALETTE:
      Common::DecodeCI8Image(out, pixels, common_palette.data(), MEMORY_CARD_ICON_WIDTH,
                             MEMORY_CARD_ICON_HEIGHT);
      return true;
    case MEMORY_CARD_ICON_FORMAT_RGB5A3:
    {
      std::vector<u16> texels(FRAME_PIXELS);
      std::memcpy(texels.data(), pixels, FRAME_PIXELS * 2);
      Common::Decode5A3Image(out, texels.data(), MEMORY_CARD_ICON_WIDTH, MEMORY_CARD_ICON_HEIGHT);
      return true;
    }
    case MEMORY_CARD_ICON_FORMAT_CI8_UNIQUE_PALETTE:
    {
      std::array<u16, MEMORY_CARD_CI8_PALETTE_ENTRIES> palette;
      std::memcpy(palette.data(), pixels + FRAME_PIXELS, PALETTE_BYTES);
      Common::DecodeCI8Image(out, pixels, palette.data(), MEMORY_CARD_ICON_WIDTH,
                             MEMORY_CARD_ICON_HEIGHT);
      return true;
    }
    default:
      return false;
    }
  };

  std::vector<GCMemcardAnimationFrameRGBA8> result(frames.size());
  for (size_t i = 0; i < frames.size(); ++i)
  {
    result[i].delay = frames[i].delay;
    // Starts fully transparent, which is what a frame with nothing to show
    // ends up as.
    result[i].image_data.assign(FRAME_PIXELS, 0);
    for (size_t j = i; j < frames.size(); ++j)
    {
      if (decode_into(result[i].image_data.data(), frames[j]))
        break;
    }
  }
  return result;
}

bool GCMemcard::Format(u8* card_data, const CardFlashId& flash_id, u16 size_mbits, bool shift_jis,
                       u32 rtc_bias, u32 sram_language, u64 format_time)
{
  if (card_data == nullptr)
    return false;

  const Header header(flash_id, size_mbits, shift_jis, rtc_bias, sram_language, format_time);
  const Directory directory;
  const BlockAlloc bat(size_mbits);

  std::memcpy(card_data, &header, BLOCK_SIZE);
  for (u32 copy = 0; copy < 2; ++copy)
  {
    std::memcpy(card_data + BLOCK_SIZE * (1 + copy), &directory, BLOCK_SIZE);
    std::memcpy(card_data + BLOCK_SIZE * (3 + copy), &bat, BLOCK_SIZE);
  }
  return true;
}

bool GCMemcard::Format(const CardFlashId& flash_id, u16 size_mbits, bool shift_jis, u32 rtc_bias,
                       u32 sram_language, u64 format_time)
{
  m_header_block = Header(flash_id, size_mbits, shift_jis, rtc_bias, sram_language, format_time);
  m_directory_blocks.fill(Directory());
  m_bat_blocks.fill(BlockAlloc(size_mbits));

  m_size_mb = size_mbits;
  m_size_blocks = u32{size_mbits} * MBIT_TO_BLOCKS;
  m_data_blocks.assign(m_size_blocks - MC_FST_BLOCKS, GCMBlock());

  m_active_directory = 0;
  m_active_bat = 0;
  m_valid = true;

  return Save();
}

// F-Zero GX's system file stores the card serial and is protected by a
// CRC-16 (reflected polynomial 0x8408, initial value 0xFFFF, stored inverted)
// over everything after the checksum itself.
s32 GCMemcard::FZEROGX_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                         std::vector<GCMBlock>& FileBuffer)
{
  if (!FilenameIs(direntry, "f_zero.dat") || FileBuffer.size() != 4)
    return 0;

  const auto [serial1, serial2] = cardheader.CalculateSerial();
  const u32 s1 = Common::swap32(serial1);
  const u32 s2 = Common::swap32(serial2);
  PutBE16(&FileBuffer[1].m_block[0x0066], static_cast<u16>(s1 >> 16));
  PutBE16(&FileBuffer[3].m_block[0x1580], static_cast<u16>(s2 >> 16));
  PutBE16(&FileBuffer[1].m_block[0x0060], static_cast<u16>(s1));
  PutBE16(&FileBuffer[1].m_block[0x0200], static_cast<u16>(s2));

  u16 crc = 0xFFFF;
  for (u32 pos = 2; pos < 4 * BLOCK_SIZE; ++pos)
  {
    crc ^= FileBuffer[pos / BLOCK_SIZE].m_block[pos % BLOCK_SIZE];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 1) ? static_cast<u16>((crc >> 1) ^ 0x8408) : static_cast<u16>(crc >> 1);
  }
  PutBE16(&FileBuffer[0].m_block[0], static_cast<u16>(~crc));
  return 1;
}

// Phantasy Star Online's system file also embeds the card serial, protected by
// a CRC-32 (reflected polynomial 0xEDB88320) with PSO's own initial value.
// PSO Episode III's file is 0x10 bytes longer.
s32 GCMemcard::PSO_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                     std::vector<GCMBlock>& FileBuffer)
{
  u32 extra = 0;
  if (FilenameIs(direntry, "PSO3_SYSTEM"))
    extra = 0x10;
  else if (!FilenameIs(direntry, "PSO_SYSTEM"))
    return 0;
  if (FileBuffer.size() < 2)
    return 0;

  u8* data = FileBuffer[1].m_block.data();
  const auto [serial1, serial2] = cardheader.CalculateSerial();
  PutNative32(data + 0x0158, serial1);
  PutNative32(data + 0x015C, serial2);

  u32 crc = 0xDEBB20E3;
  for (u32 pos = 0x004C; pos < 0x0164 + extra; ++pos)
  {
    crc ^= data[pos];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
  }
  PutBE32(data + 0x0048, ~crc);
  return 1;
}
}  // namespace Memcard
