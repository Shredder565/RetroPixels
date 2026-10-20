// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/WIABlob.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <zstd.h>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/LaggedFibonacciGenerator.h"
#include "DiscIO/MultithreadedCompressor.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WIACompression.h"
#include "DiscIO/WiiEncryptionCache.h"

namespace DiscIO
{
std::pair<int, int> GetAllowedCompressionLevels(WIARVZCompressionType compression_type, bool gui)
{
  switch (compression_type)
  {
  case WIARVZCompressionType::Bzip2:
  case WIARVZCompressionType::LZMA:
  case WIARVZCompressionType::LZMA2:
    return {1, 9};
  case WIARVZCompressionType::Zstd:
    // zstd's negative levels go down to a huge magnitude, which is useless in a slider. (Level
    // 0 means "zstd's default", so the GUI starts at 1.)
    return {gui ? 1 : ZSTD_minCLevel(), ZSTD_maxCLevel()};
  default:
    return {0, -1};
  }
}

namespace
{
using Type = WIARVZCompressionType;
using TitleKey = std::array<u8, 16>;

constexpr u64 SECTOR = VolumeWii::BLOCK_TOTAL_SIZE;         // 0x8000, raw Wii block
constexpr u64 SECTOR_DATA = VolumeWii::BLOCK_DATA_SIZE;     // 0x7C00, its payload
constexpr u64 SECTOR_HASHES = VolumeWii::BLOCK_HEADER_SIZE;  // 0x400
constexpr u64 WII_GROUP = VolumeWii::GROUP_TOTAL_SIZE;       // 2 MiB of raw blocks
constexpr u64 WII_GROUP_DATA = VolumeWii::GROUP_DATA_SIZE;   // their payload
constexpr u64 BLOCKS_PER_WII_GROUP = VolumeWii::BLOCKS_PER_GROUP;
constexpr size_t DIGEST = Common::SHA1::DIGEST_LEN;

// Format versions (major.minor.patch.beta, one byte each)
constexpr u32 WIA_VERSION = 0x01000000;
// Version 0.9 of wit could perhaps read our files too, but it was never released in a form
// that could be tested, so claim 1.0.
constexpr u32 WIA_OLDEST_READER = 0x01000000;
constexpr u32 WIA_OLDEST_READABLE = 0x00080000;
constexpr u32 RVZ_VERSION = 0x01000000;
constexpr u32 RVZ_OLDEST_READER = 0x00030000;
constexpr u32 RVZ_OLDEST_READABLE = 0x00030000;

// ---------------------------------------------------------------------------------------------
// On-disk structures (all integers big endian)

#pragma pack(push, 1)
struct FileHead  // "header 1"
{
  u32 magic;
  u32 version;
  u32 version_compatible;
  u32 disc_head_size;
  Common::SHA1::Digest disc_head_hash;
  u64 iso_file_size;
  u64 wia_file_size;
  Common::SHA1::Digest file_head_hash;  // of everything above
};
static_assert(sizeof(FileHead) == 0x48);

struct DiscHead  // "header 2"
{
  u32 disc_type;  // 0 unknown, 1 GameCube, 2 Wii
  u32 compression_type;
  s32 compression_level;
  u32 chunk_size;
  std::array<u8, 0x80> disc_header;  // the disc's first 0x80 bytes
  u32 partition_count;
  u32 partition_entry_size;
  u64 partitions_offset;
  Common::SHA1::Digest partitions_hash;
  u32 raw_data_count;
  u64 raw_data_offset;
  u32 raw_data_stored_size;
  u32 group_count;
  u64 groups_offset;
  u32 groups_stored_size;
  u8 compressor_data_size;
  u8 compressor_data[7];
};
static_assert(sizeof(DiscHead) == 0xDC);
constexpr size_t DISC_HEAD_MIN_SIZE = sizeof(DiscHead) - sizeof(DiscHead::compressor_data);
constexpr size_t DISC_HEADER_COPY = sizeof(DiscHead::disc_header);

// One partition: [0] covers the partition data up to the end of its file system tables, [1] the
// rest (so that the file system can be read without decoding everything).
struct PartitionPart
{
  u32 first_sector;
  u32 sector_count;
  u32 first_group;
  u32 group_count;
};
struct PartitionRecord
{
  TitleKey key;
  std::array<PartitionPart, 2> parts;
};
static_assert(sizeof(PartitionRecord) == 0x30);

struct RawDataRecord
{
  u64 offset;
  u64 size;
  u32 first_group;
  u32 group_count;
};
static_assert(sizeof(RawDataRecord) == 0x18);

struct HashException
{
  u16 offset;  // within the 64 hash blocks of a Wii group
  Common::SHA1::Digest hash;
};
static_assert(sizeof(HashException) == 0x16);
#pragma pack(pop)

// A group entry in host byte order. On disk: u32 offset / 4, u32 size, and for RVZ also
// u32 packed size; RVZ uses bit 31 of the size for "compressed".
struct Group
{
  u32 offset_div4 = 0;
  u32 size_field = 0;
  u32 packed_size = 0;
};

size_t GroupRecordSize(bool rvz)
{
  return rvz ? 12 : 8;
}

// The biggest possible exception list for one Wii group: one exception per 20 bytes of each
// hash block, plus the count.
constexpr size_t MAX_EXCEPTION_LIST_BYTES =
    (SECTOR_HASHES + DIGEST - 1) / DIGEST * BLOCKS_PER_WII_GROUP * sizeof(HashException) +
    sizeof(u16);

std::string VersionString(u32 v)
{
  const u32 beta = v & 0xFF;
  std::string s = fmt::format("{}.{:02x}.{:02x}", v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF);
  if (beta != 0 && beta != 0xFF)
    s += fmt::format(".beta{}", beta);
  return s;
}

template <typename T>
void Append(std::vector<u8>* out, const T& value)
{
  const u8* p = reinterpret_cast<const u8*>(&value);
  out->insert(out->end(), p, p + sizeof(T));
}

void Append(std::vector<u8>* out, const u8* begin, const u8* end)
{
  out->insert(out->end(), begin, end);
}

bool AllBytesAre(const u8* begin, const u8* end, u8 value)
{
  return std::all_of(begin, end, [value](u8 b) { return b == value; });
}

bool AllBytesSame(const u8* begin, const u8* end)
{
  return AllBytesAre(begin, end, *begin);
}

u64 AlignUp(u64 x, u64 a)
{
  return (x + a - 1) / a * a;
}

// =============================================================================================
// Reading

// What lies at a given part of the disc: a partition part or a raw data record.
struct Extent
{
  u32 index;
  bool is_partition;
  u8 part;
};

// The data of one group, decoded on demand: only as much of the stored data is loaded and
// decoded as reads have needed so far.
class GroupReader
{
public:
  GroupReader() = default;
  GroupReader(File::DirectIOFile* file, u64 file_offset, u64 stored_size, u64 decoded_size,
              u32 exception_lists, bool lists_compressed, u32 packed_size, u64 data_offset,
              std::unique_ptr<Decoder> decoder)
      : m_file(file), m_file_offset(file_offset), m_lists_left(exception_lists),
        m_lists_compressed(lists_compressed), m_packed_size(packed_size),
        m_data_offset(data_offset), m_decoder(std::move(decoder))
  {
    // Compressed exception lists come out of the decoder ahead of the data, so leave room.
    m_list_room = m_lists_compressed ? MAX_EXCEPTION_LIST_BYTES * m_lists_left : 0;
    m_in.resize(stored_size);
    m_out.resize(decoded_size + m_list_room);
  }

  bool Read(u64 offset, u64 size, u8* out)
  {
    if (!m_decoder || !m_file || offset + size > m_out.size() - m_list_room)
      return false;

    while (offset + size > DataAvailable())
    {
      if (!LoadMore(offset + size))
        return false;
    }

    std::memcpy(out, m_out.data() + m_out_list_bytes + offset, size);
    return true;
  }

  // The exceptions of list `list_index`, with `offset_adjust` added to each offset. Only valid
  // once some data has been read (so that all lists have been parsed).
  void GetExceptions(std::vector<HashException>* out, u64 list_index, u16 offset_adjust) const
  {
    ASSERT(m_lists_left == 0);
    const u8* lists = m_lists_compressed ? m_out.data() : m_in.data();
    const u8* p = lists;
    for (u64 i = 0; i < list_index; ++i)
      p += sizeof(u16) + Common::swap16(p) * sizeof(HashException);

    const u16 count = Common::swap16(p);
    p += sizeof(u16);
    for (u16 i = 0; i < count; ++i, p += sizeof(HashException))
    {
      HashException& e = out->emplace_back();
      std::memcpy(&e, p, sizeof(e));
      e.offset = Common::swap16(static_cast<u16>(Common::swap16(e.offset) + offset_adjust));
    }
    ASSERT(p <= lists + (m_lists_compressed ? m_out_list_bytes : m_in_list_bytes));
  }

private:
  // Decoded data bytes (not counting exception lists) available so far.
  size_t DataAvailable() const { return m_lists_left == 0 ? m_out_pos - m_out_list_bytes : 0; }

  bool LoadMore(u64 wanted_end)
  {
    // How much stored data to load next: all of it when the read goes to the very end,
    // otherwise about what the read still needs (compression rarely expands data much), with
    // the file position rounded to a sector boundary.
    u64 count;
    if (wanted_end == m_out.size())
    {
      count = m_in.size() - m_in_loaded;
    }
    else
    {
      count = wanted_end - DataAvailable() + 0x100;
      count = AlignUp(count + m_file_offset, SECTOR) - m_file_offset;
      count = std::min<u64>(count, m_in.size() - m_in_loaded);
    }
    if (count == 0)
      return false;  // the stored data ended early, or decodes to less than it should

    if (!m_file->OffsetRead(m_file_offset, m_in.data() + m_in_loaded, count))
      return false;
    m_file_offset += count;
    m_in_loaded += count;

    if (m_lists_left > 0 && !m_lists_compressed)
    {
      // Uncompressed lists sit at the start of the stored data, padded to 4 bytes.
      if (!ParseLists(m_in.data(), m_in.size(), m_in_loaded, &m_in_list_bytes, true))
        return false;
      m_in_pos = m_in_list_bytes;
    }

    if (m_lists_left == 0 || m_lists_compressed)
    {
      if (!Decode())
        return false;
    }

    if (m_lists_left > 0 && m_lists_compressed)
    {
      if (!ParseLists(m_out.data(), m_list_room, m_out_pos, &m_out_list_bytes, false))
        return false;
      if (m_lists_left == 0 && m_packed_size != 0)
      {
        if (!Decode())
          return false;
      }
    }

    if (m_lists_left == 0)
    {
      const size_t expected = m_out.size() - m_list_room + m_out_list_bytes;
      if (m_out_pos > expected)
        return false;  // decodes to more than it should
      // (zstd can produce all output before reading the end of its frame, hence the check
      // that all input has been given before calling this an error.)
      if (m_out_pos == expected && !m_decoder->Finished() && m_in_loaded == m_in.size())
        return false;  // decodes to more than it should
      if (m_decoder->Finished() && m_in_pos != m_in.size())
        return false;  // stored data is longer than its stream
    }
    return true;
  }

  bool Decode()
  {
    if (m_packed_size != 0 && m_lists_left == 0)
    {
      // From here on the decoded bytes are RVZ packed; hand what has been decoded after the
      // lists to the unpacker and let it write the real data.
      std::vector<u8> decoded(m_out.begin() + m_out_list_bytes, m_out.begin() + m_out_pos);
      m_out_pos = m_out_list_bytes;
      m_decoder = MakeRVZUnpacker(std::move(m_decoder), std::move(decoded), m_data_offset,
                                  m_packed_size);
      m_packed_size = 0;
    }

    DecodeInput in{m_in.data(), m_in_loaded, m_in.size(), m_in_pos};
    DecodeOutput out{m_out.data(), m_out.size(), m_out_pos};
    const bool ok = m_decoder->Step(in, out);
    m_in_pos = in.pos;
    m_out_pos = out.pos;
    return ok;
  }

  // Walks over complete exception lists in data[0, filled), starting at *used.
  bool ParseLists(const u8* data, size_t room, size_t filled, size_t* used, bool pad_last)
  {
    while (m_lists_left > 0)
    {
      if (*used + sizeof(u16) > room)
      {
        ERROR_LOG_FMT(DISCIO, "WIA/RVZ: more hash exceptions than possible");
        return false;
      }
      if (*used + sizeof(u16) > filled)
        return true;

      size_t list_bytes = sizeof(u16) + Common::swap16(data + *used) * sizeof(HashException);
      if (pad_last && m_lists_left == 1)
        list_bytes = AlignUp(*used + list_bytes, 4) - *used;

      if (*used + list_bytes > room)
      {
        ERROR_LOG_FMT(DISCIO, "WIA/RVZ: more hash exceptions than possible");
        return false;
      }
      if (*used + list_bytes > filled)
        return true;

      *used += list_bytes;
      --m_lists_left;
    }
    return true;
  }

  File::DirectIOFile* m_file = nullptr;
  u64 m_file_offset = 0;

  std::vector<u8> m_in;
  size_t m_in_loaded = 0;
  size_t m_in_pos = 0;
  size_t m_in_list_bytes = 0;

  std::vector<u8> m_out;
  size_t m_out_pos = 0;
  size_t m_list_room = 0;
  size_t m_out_list_bytes = 0;

  u32 m_lists_left = 0;
  bool m_lists_compressed = false;
  u32 m_packed_size = 0;
  u64 m_data_offset = 0;
  std::unique_ptr<Decoder> m_decoder;
};
}  // namespace

class WIARVZReader final : public BlobReader
{
public:
  static std::unique_ptr<WIARVZReader> Open(File::DirectIOFile file, const std::string& path,
                                            bool rvz)
  {
    std::unique_ptr<WIARVZReader> reader(new WIARVZReader(std::move(file), path, rvz));
    if (!reader->Load())
      return nullptr;
    return reader;
  }

  BlobType GetBlobType() const override { return m_rvz ? BlobType::RVZ : BlobType::WIA; }
  std::unique_ptr<BlobReader> CopyReader() const override { return Open(m_file, m_path, m_rvz); }
  u64 GetRawSize() const override { return Common::swap64(m_file_head.wia_file_size); }
  u64 GetDataSize() const override { return Common::swap64(m_file_head.iso_file_size); }
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }
  u64 GetBlockSize() const override { return ChunkSize(); }
  bool HasFastRandomAccessInBlock() const override { return false; }

  std::string GetCompressionMethod() const override
  {
    switch (m_type)
    {
    case Type::Purge:
      return "Purge";
    case Type::Bzip2:
      return "bzip2";
    case Type::LZMA:
      return "LZMA";
    case Type::LZMA2:
      return "LZMA2";
    case Type::Zstd:
      return "Zstandard";
    default:
      return {};
    }
  }

  std::optional<int> GetCompressionLevel() const override
  {
    return static_cast<int>(Common::swap32(static_cast<u32>(m_disc_head.compression_level)));
  }

  bool Read(u64 offset, u64 size, u8* out) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out, u64 partition_data_offset) override;

private:
  WIARVZReader(File::DirectIOFile file, std::string path, bool rvz)
      : m_rvz(rvz), m_file(std::move(file)), m_path(std::move(path)), m_encryption(this)
  {
  }

  bool Load();
  bool RegionsOverlap() const;
  u32 ChunkSize() const { return Common::swap32(m_disc_head.chunk_size); }

  const PartitionRecord* FindPartition(u64 partition_data_offset, u32* first_sector) const;
  bool ReadPartition(const PartitionRecord& partition, u64* offset, u64* size, u8** out);
  bool ReadGroups(u64* offset, u64* size, u8** out, u64 chunk_size, u32 sector_size,
                  u64 data_offset, u64 data_size, u32 first_group, u32 group_count,
                  u32 exception_lists);
  GroupReader& GetGroupReader(u64 file_offset, u64 stored_size, u64 decoded_size, Type type,
                              u32 exception_lists = 0, u32 packed_size = 0, u64 data_offset = 0);

  template <typename T>
  bool LoadTable(u64 file_offset, u32 stored_size, u64 count, std::vector<T>* table)
  {
    table->resize(count);
    GroupReader& reader = GetGroupReader(file_offset, stored_size, u64(count) * sizeof(T), m_type);
    return reader.Read(0, u64(count) * sizeof(T), reinterpret_cast<u8*>(table->data()));
  }

  const bool m_rvz;
  File::DirectIOFile m_file;
  std::string m_path;
  Type m_type = Type::None;

  FileHead m_file_head{};
  DiscHead m_disc_head{};
  std::vector<PartitionRecord> m_partitions;
  std::vector<RawDataRecord> m_raw_data;
  std::vector<Group> m_groups;
  std::map<u64, Extent> m_regions;  // keyed by end offset on the disc

  // Only the most recently used group is kept.
  GroupReader m_group_reader;
  u64 m_group_reader_offset = std::numeric_limits<u64>::max();

  WiiEncryptionCache m_encryption;

  // While a Wii group is being re-encrypted, the hash exceptions of the groups its data was
  // read from are gathered here.
  bool m_collect_exceptions = false;
  std::vector<HashException> m_exceptions;
  u64 m_exceptions_last_group = std::numeric_limits<u64>::max();
};

bool WIARVZReader::Load()
{
  if (!m_file.OffsetRead(0, Common::AsWritableU8Span(m_file_head)))
    return false;
  if (m_file_head.magic != (m_rvz ? RVZ_MAGIC : WIA_MAGIC))
    return false;

  // The file says which versions can read it; we say which versions we can read.
  const u32 version = Common::swap32(m_file_head.version);
  const u32 needs_reader = Common::swap32(m_file_head.version_compatible);
  if ((m_rvz ? RVZ_VERSION : WIA_VERSION) < needs_reader ||
      (m_rvz ? RVZ_OLDEST_READABLE : WIA_OLDEST_READABLE) > version)
  {
    ERROR_LOG_FMT(DISCIO, "Unsupported version {} in {}", VersionString(version), m_path);
    return false;
  }

  if (Common::SHA1::CalculateDigest(reinterpret_cast<const u8*>(&m_file_head),
                                    offsetof(FileHead, file_head_hash)) !=
      m_file_head.file_head_hash)
  {
    return false;
  }

  if (Common::swap64(m_file_head.wia_file_size) != m_file.GetSize())
  {
    ERROR_LOG_FMT(DISCIO, "File size is incorrect for {}", m_path);
    return false;
  }

  const u32 disc_head_size = Common::swap32(m_file_head.disc_head_size);
  if (disc_head_size < DISC_HEAD_MIN_SIZE)
    return false;
  std::vector<u8> disc_head(disc_head_size);
  if (!m_file.OffsetRead(sizeof(FileHead), disc_head))
    return false;
  if (Common::SHA1::CalculateDigest(disc_head) != m_file_head.disc_head_hash)
    return false;
  std::memcpy(&m_disc_head, disc_head.data(), std::min(disc_head.size(), sizeof(DiscHead)));

  if (m_disc_head.compressor_data_size > sizeof(DiscHead::compressor_data) ||
      disc_head_size < DISC_HEAD_MIN_SIZE + m_disc_head.compressor_data_size)
  {
    return false;
  }

  // Chunks are whole Wii groups, or (RVZ only) a power of two of at least one sector.
  const u32 chunk = ChunkSize();
  const bool small_chunk_ok = m_rvz && chunk >= SECTOR && (chunk & (chunk - 1)) == 0;
  if (chunk == 0 || (!small_chunk_ok && chunk % WII_GROUP != 0))
    return false;

  const u32 type = Common::swap32(m_disc_head.compression_type);
  m_type = static_cast<Type>(type);
  if (m_type > (m_rvz ? Type::Zstd : Type::LZMA2) || (m_rvz && m_type == Type::Purge))
  {
    ERROR_LOG_FMT(DISCIO, "Unsupported compression type {} in {}", type, m_path);
    return false;
  }

  // Partition records (stored uncompressed). Records may be longer or shorter than ours.
  const size_t partition_count = Common::swap32(m_disc_head.partition_count);
  const size_t record_size = Common::swap32(m_disc_head.partition_entry_size);
  std::vector<u8> records(record_size * partition_count);
  if (!m_file.OffsetRead(Common::swap64(m_disc_head.partitions_offset), records))
    return false;
  if (Common::SHA1::CalculateDigest(records) != m_disc_head.partitions_hash)
    return false;

  m_partitions.assign(partition_count, PartitionRecord{});
  for (size_t i = 0; i < partition_count; ++i)
  {
    std::memcpy(&m_partitions[i], records.data() + i * record_size,
                std::min(record_size, sizeof(PartitionRecord)));
  }

  for (size_t i = 0; i < m_partitions.size(); ++i)
  {
    const auto& parts = m_partitions[i].parts;
    size_t used_parts = 0;
    for (size_t j = 0; j < parts.size(); ++j)
    {
      const u32 sectors = Common::swap32(parts[j].sector_count);
      if (sectors == 0)
        continue;
      ++used_parts;
      const u32 end_sector = Common::swap32(parts[j].first_sector) + sectors;
      m_regions.emplace(end_sector * SECTOR,
                        Extent{static_cast<u32>(i), true, static_cast<u8>(j)});
    }
    if (used_parts > 1 &&
        Common::swap32(parts[0].first_sector) > Common::swap32(parts[1].first_sector))
    {
      return false;
    }
  }

  // Raw data and group tables (stored with the file's compression).
  if (!LoadTable(Common::swap64(m_disc_head.raw_data_offset),
                 Common::swap32(m_disc_head.raw_data_stored_size),
                 Common::swap32(m_disc_head.raw_data_count), &m_raw_data))
  {
    return false;
  }
  for (size_t i = 0; i < m_raw_data.size(); ++i)
  {
    const u64 size = Common::swap64(m_raw_data[i].size);
    if (size != 0)
    {
      m_regions.emplace(Common::swap64(m_raw_data[i].offset) + size,
                        Extent{static_cast<u32>(i), false, 0});
    }
  }

  const u32 group_count = Common::swap32(m_disc_head.group_count);
  std::vector<u8> group_records;
  if (!LoadTable(Common::swap64(m_disc_head.groups_offset),
                 Common::swap32(m_disc_head.groups_stored_size),
                 u64(group_count) * GroupRecordSize(m_rvz), &group_records))
  {
    return false;
  }
  m_groups.resize(group_count);
  for (u32 i = 0; i < group_count; ++i)
  {
    const u8* p = group_records.data() + i * GroupRecordSize(m_rvz);
    m_groups[i].offset_div4 = Common::swap32(p);
    m_groups[i].size_field = Common::swap32(p + 4);
    if (m_rvz)
      m_groups[i].packed_size = Common::swap32(p + 8);
  }

  return !RegionsOverlap();
}

bool WIARVZReader::RegionsOverlap() const
{
  // Every region's start must lead (via the end-offset map) back to that same region.
  for (size_t i = 0; i < m_partitions.size(); ++i)
  {
    const auto& parts = m_partitions[i].parts;
    for (size_t j = 0; j < parts.size(); ++j)
    {
      if (Common::swap32(parts[j].sector_count) == 0)
        continue;
      const auto it = m_regions.upper_bound(Common::swap32(parts[j].first_sector) * SECTOR);
      if (it == m_regions.end() || !it->second.is_partition || it->second.index != i ||
          it->second.part != j)
      {
        return true;
      }
    }
  }

  for (size_t i = 0; i < m_raw_data.size(); ++i)
  {
    if (Common::swap64(m_raw_data[i].size) == 0)
      continue;
    const auto it = m_regions.upper_bound(Common::swap64(m_raw_data[i].offset));
    if (it == m_regions.end() || it->second.is_partition || it->second.index != i)
      return true;
  }
  return false;
}

GroupReader& WIARVZReader::GetGroupReader(u64 file_offset, u64 stored_size, u64 decoded_size,
                                          Type type, u32 exception_lists, u32 packed_size,
                                          u64 data_offset)
{
  if (file_offset == m_group_reader_offset)
    return m_group_reader;

  std::unique_ptr<Decoder> decoder =
      MakeDecoder(type, m_disc_head.compressor_data, m_disc_head.compressor_data_size,
                  packed_size == 0 ? decoded_size : packed_size);
  m_group_reader = GroupReader(&m_file, file_offset, stored_size, decoded_size, exception_lists,
                               type > Type::Purge, packed_size, data_offset, std::move(decoder));
  m_group_reader_offset = file_offset;
  return m_group_reader;
}

bool WIARVZReader::ReadGroups(u64* offset, u64* size, u8** out, u64 chunk_size, u32 sector_size,
                              u64 data_offset, u64 data_size, u32 first_group, u32 group_count,
                              u32 exception_lists)
{
  if (data_offset + data_size <= *offset)
    return true;  // nothing of this region is wanted
  if (*offset < data_offset)
    return false;

  // The first group starts at the beginning of the region's first sector.
  const u64 lead = data_offset % sector_size;
  data_offset -= lead;
  data_size += lead;

  for (u64 i = (*offset - data_offset) / chunk_size; i < group_count && *size > 0; ++i)
  {
    const u64 group_number = first_group + i;
    if (group_number >= m_groups.size())
      return false;
    const Group& group = m_groups[group_number];

    const u64 group_start = i * chunk_size;
    const u64 in_group = *offset - group_start - data_offset;
    // The last group of a region may be shorter (and the shorter length then sticks).
    chunk_size = std::min(chunk_size, data_size - group_start);
    const u64 n = std::min(chunk_size - in_group, *size);

    Type type = m_type;
    u32 stored_size = group.size_field;
    if (m_rvz)
    {
      if ((stored_size & 0x80000000) == 0)
        type = Type::None;
      stored_size &= 0x7FFFFFFF;
    }

    if (stored_size == 0)
    {
      std::memset(*out, 0, n);  // an empty group means all zeroes
    }
    else
    {
      GroupReader& reader =
          GetGroupReader(u64(group.offset_div4) << 2, stored_size, chunk_size, type,
                         exception_lists, m_rvz ? group.packed_size : 0, group_start);
      if (!reader.Read(in_group, n, *out))
      {
        m_group_reader_offset = std::numeric_limits<u64>::max();
        return false;
      }

      if (m_collect_exceptions && m_exceptions_last_group != group_number)
      {
        // Hash exception offsets are relative to the Wii group; a group (chunk) smaller than
        // a Wii group sits partway into one.
        const u16 adjust = static_cast<u16>(group_start % WII_GROUP_DATA / SECTOR_DATA *
                                            SECTOR_HASHES);
        reader.GetExceptions(&m_exceptions, in_group / WII_GROUP_DATA, adjust);
        m_exceptions_last_group = group_number;
      }
    }

    *offset += n;
    *size -= n;
    *out += n;
  }
  return true;
}

bool WIARVZReader::ReadPartition(const PartitionRecord& partition, u64* offset, u64* size,
                                 u8** out)
{
  const u32 first_sector = Common::swap32(partition.parts[0].first_sector);
  const u64 data_start = first_sector * SECTOR;
  const u32 second_count = Common::swap32(partition.parts[1].sector_count);
  const u32 total_sectors =
      second_count != 0 ?
          Common::swap32(partition.parts[1].first_sector) - first_sector + second_count :
          Common::swap32(partition.parts[0].sector_count);

  for (const PartitionPart& part : partition.parts)
  {
    if (*size == 0)
      return true;

    const u64 part_start = Common::swap32(part.first_sector) * SECTOR;
    const u64 part_size = Common::swap32(part.sector_count) * SECTOR;
    if (part_size == 0 || part_start + part_size <= *offset)
      continue;
    if (*offset < part_start)
      return false;

    const u64 n = std::min(part_size - (*offset - part_start), *size);

    // Rebuild the encrypted blocks. Re-encrypting a Wii group reads its decrypted data
    // through ReadWiiDecrypted, which gathers the group's hash exceptions as it goes; they
    // are applied to the recomputed hashes before encryption.
    m_collect_exceptions = true;
    m_exceptions.clear();
    m_exceptions_last_group = std::numeric_limits<u64>::max();
    bool bad_exception = false;
    const bool ok = m_encryption.EncryptGroups(
        *offset - data_start, n, *out, data_start, total_sectors * SECTOR_DATA, partition.key,
        [this, &bad_exception](VolumeWii::HashBlock hash_blocks[VolumeWii::BLOCKS_PER_GROUP],
                               u64) {
          for (const HashException& e : m_exceptions)
          {
            const u16 at = Common::swap16(e.offset);
            const size_t block = at / SECTOR_HASHES;
            const size_t in_block = at % SECTOR_HASHES;
            if (block >= BLOCKS_PER_WII_GROUP || in_block + DIGEST > SECTOR_HASHES)
            {
              bad_exception = true;
              break;
            }
            std::memcpy(reinterpret_cast<u8*>(&hash_blocks[block]) + in_block, e.hash.data(),
                        DIGEST);
          }
          // Start afresh for the next Wii group.
          m_exceptions.clear();
          m_exceptions_last_group = std::numeric_limits<u64>::max();
        });
    m_collect_exceptions = false;
    if (!ok || bad_exception)
      return false;

    *offset += n;
    *size -= n;
    *out += n;
  }
  return true;
}

bool WIARVZReader::Read(u64 offset, u64 size, u8* out)
{
  if (offset + size > GetDataSize())
    return false;

  // The first bytes of the disc are kept in the header.
  if (offset < DISC_HEADER_COPY)
  {
    const u64 n = std::min(DISC_HEADER_COPY - offset, size);
    std::memcpy(out, m_disc_head.disc_header.data() + offset, n);
    offset += n;
    size -= n;
    out += n;
  }

  while (size > 0)
  {
    const auto it = m_regions.upper_bound(offset);
    if (it == m_regions.end())
      return false;

    const Extent& region = it->second;
    if (region.is_partition)
    {
      if (!ReadPartition(m_partitions[region.index], &offset, &size, &out))
        return false;
    }
    else
    {
      const RawDataRecord& raw = m_raw_data[region.index];
      if (!ReadGroups(&offset, &size, &out, ChunkSize(), SECTOR, Common::swap64(raw.offset),
                      Common::swap64(raw.size), Common::swap32(raw.first_group),
                      Common::swap32(raw.group_count), 0))
      {
        return false;
      }
    }
  }
  return true;
}

const PartitionRecord* WIARVZReader::FindPartition(u64 partition_data_offset,
                                                   u32* first_sector) const
{
  const auto it = m_regions.upper_bound(partition_data_offset);
  if (it == m_regions.end() || !it->second.is_partition)
    return nullptr;

  const PartitionRecord& partition = m_partitions[it->second.index];
  *first_sector = Common::swap32(partition.parts[0].first_sector);
  if (partition_data_offset != *first_sector * SECTOR)
    return nullptr;
  return &partition;
}

bool WIARVZReader::SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const
{
  u32 first_sector;
  const PartitionRecord* partition = FindPartition(partition_data_offset, &first_sector);
  if (!partition)
    return false;

  for (const PartitionPart& part : partition->parts)
  {
    const u32 end_sector =
        Common::swap32(part.first_sector) - first_sector + Common::swap32(part.sector_count);
    if (offset + size <= end_sector * SECTOR_DATA)
      return true;
  }
  return false;
}

bool WIARVZReader::ReadWiiDecrypted(u64 offset, u64 size, u8* out, u64 partition_data_offset)
{
  u32 first_sector;
  const PartitionRecord* partition = FindPartition(partition_data_offset, &first_sector);
  if (!partition)
    return false;

  // Partition data is stored without hash blocks, so a group holds less of it.
  const u64 chunk_size = ChunkSize() * SECTOR_DATA / SECTOR;
  const u32 lists_per_group = std::max<u32>(1, static_cast<u32>(chunk_size / WII_GROUP_DATA));

  for (const PartitionPart& part : partition->parts)
  {
    if (size == 0)
      return true;
    const u64 part_start = (Common::swap32(part.first_sector) - first_sector) * SECTOR_DATA;
    const u64 part_size = Common::swap32(part.sector_count) * SECTOR_DATA;
    if (!ReadGroups(&offset, &size, &out, chunk_size, SECTOR_DATA, part_start, part_size,
                    Common::swap32(part.first_group), Common::swap32(part.group_count),
                    lists_per_group))
    {
      return false;
    }
  }
  return size == 0;
}

// =============================================================================================
// Writing

namespace
{
// Groups that store identical data are written once. Only groups whose data is a single
// repeated byte are considered.
struct ReuseKey
{
  TitleKey key;  // zero for raw data
  u64 size;
  bool encrypted;  // the comparison was made on encrypted data
  u8 value;
  // For encrypted data, `value` is the first byte of the whole Wii group being processed
  // (which is what earlier versions wrote into files); `fill` is the chunk's own byte, so that
  // chunks with different contents never share storage.
  u8 fill;

  auto Tie() const { return std::tie(key, size, encrypted, value, fill); }
  bool operator<(const ReuseKey& other) const { return Tie() < other.Tie(); }
};

// A planned stretch of the disc: a raw data record or one part of a partition.
struct Planned
{
  bool is_partition;
  u32 index;
  u8 part;
};

struct Plan
{
  std::vector<PartitionRecord> partitions;
  std::vector<RawDataRecord> raw_data;
  std::vector<Planned> order;  // in disc order
  std::vector<const FileSystem*> file_systems;  // per partition
  u32 group_count = 0;
};

// One stored group as produced by a worker.
struct GroupOut
{
  std::vector<u8> exception_lists;
  std::vector<u8> data;
  std::optional<ReuseKey> reuse;
  std::optional<Group> reused;  // set when an identical group has already been written
  size_t packed_size = 0;       // RVZ
  bool compressed = false;      // RVZ
};

struct Job
{
  std::vector<u8> data;
  const Planned* where = nullptr;
  u64 data_offset = 0;  // within the partition's decrypted data, or on the disc
  u64 bytes_read = 0;   // progress after this job
  size_t first_group = 0;
};

struct JobResult
{
  std::vector<GroupOut> groups;
  u64 bytes_read = 0;
  size_t first_group = 0;
};

struct WorkerState
{
  std::unique_ptr<Encoder> encoder;
  std::vector<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>> plain{BLOCKS_PER_WII_GROUP};
  std::vector<VolumeWii::HashBlock> hashes{BLOCKS_PER_WII_GROUP};
};

class ReuseTable
{
public:
  // Fills in `out.reused` if an identical group has been written.
  bool Lookup(GroupOut* out)
  {
    if (out->reused)
      return true;
    if (!out->reuse)
      return false;
    std::lock_guard lock(m_mutex);
    const auto it = m_written.find(*out->reuse);
    if (it == m_written.end())
      return false;
    out->reused = it->second;
    return true;
  }

  void Add(const ReuseKey& key, const Group& group)
  {
    std::lock_guard lock(m_mutex);
    m_written.emplace(key, group);
  }

private:
  std::mutex m_mutex;
  std::map<ReuseKey, Group> m_written;
};

// ---------------------------------------------------------------------------------------------
// RVZ packing: describe the junk (generator output) in a stretch of data, then store each
// chunk as data records and seed records.

using Seed = std::array<u32, LaggedFibonacciGenerator::SEED_SIZE>;
constexpr size_t SEED_BYTES = sizeof(Seed);

void RVZPack(const u8* in, GroupOut* out, u64 bytes_per_chunk, size_t chunks, u64 total_size,
             u64 data_offset, bool multipart, bool allow_junk_reuse, bool compression,
             const FileSystem* file_system)
{
  struct Junk
  {
    size_t start;
    Seed seed;
  };
  std::map<size_t, Junk> junk;  // keyed by end offset

  // Find junk. On discs it starts at the end of a file (after up to 32 zero bytes) and the
  // generator restarts at every sector, so look at most one sector ahead each time.
  size_t pos = 0;
  while (pos < total_size)
  {
    size_t zeroes = 0;
    while (pos + zeroes < total_size && in[pos + zeroes] == 0)
      ++zeroes;
    // Without a compressor, long runs of zeroes (scrubbed junk?) are cheaper as a zero seed.
    if (!compression && zeroes > SEED_BYTES)
      junk.emplace(pos + zeroes, Junk{pos, {}});
    pos += zeroes;
    data_offset += zeroes;

    const size_t span =
        std::min<u64>(AlignUp(data_offset + 1, SECTOR) - data_offset, total_size - pos);
    Seed seed;
    const size_t found = LaggedFibonacciGenerator::GetSeed(in + pos, span, data_offset % SECTOR,
                                                           seed.data());
    if (found > 0)
      junk.emplace(pos + found, Junk{pos, seed});

    if (file_system)
    {
      // If a file starts after the junk within this sector, carry on from the file's end.
      const std::unique_ptr<FileInfo> file = file_system->FindFileInfo(data_offset + found);
      if (file)
      {
        const u64 file_end = file->GetOffset() + file->GetSize();
        if (file_end < data_offset + span)
        {
          pos += file_end - data_offset;
          data_offset = file_end;
          continue;
        }
      }
    }

    pos += span;
    data_offset += span;
  }

  for (size_t i = 0; i < chunks; ++i)
  {
    GroupOut& group = out[i];
    if (group.reused)
      continue;

    u64 at = i * bytes_per_chunk;
    const u64 end = std::min(at + bytes_per_chunk, total_size);

    // A seed is only valid at one position within a sector, so a group that may be reused
    // elsewhere can't use seeds unless reuse is limited to matching positions.
    const bool use_seeds = allow_junk_reuse || !group.reuse;
    // A chunk without junk is stored unpacked, unless it is built from several Wii groups.
    bool may_store_plain = !multipart;

    while (at < end)
    {
      u64 junk_start = end;
      u64 junk_end = end;
      const Seed* seed = nullptr;
      if (use_seeds && end - at > SEED_BYTES)
      {
        const auto it = junk.upper_bound(at + SEED_BYTES);
        if (it != junk.end() && it->second.start + SEED_BYTES < end)
        {
          junk_start = std::max<u64>(at, it->second.start);
          junk_end = std::min<u64>(end, it->first);
          seed = &it->second.seed;
        }
      }

      if (may_store_plain)
      {
        if (junk_start == end)
        {
          Append(&group.data, in + at, in + end);
          break;
        }
        may_store_plain = false;
      }

      if (junk_start > at)
      {
        const u64 n = junk_start - at;
        Append(&group.data, Common::swap32(static_cast<u32>(n)));
        Append(&group.data, in + at, in + junk_start);
        at = junk_start;
        group.packed_size += sizeof(u32) + n;
      }
      if (junk_end > at)
      {
        const u64 n = junk_end - at;
        Append(&group.data, Common::swap32(static_cast<u32>(n) | 0x80000000));
        Append(&group.data, *seed);
        at = junk_end;
        group.packed_size += sizeof(u32) + SEED_BYTES;
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------

class Writer
{
public:
  Writer(BlobReader* infile, const VolumeDisc* volume, File::DirectIOFile* outfile, bool rvz,
         Type type, int level, int chunk_size, CompressCB callback)
      : m_in(infile), m_volume(volume), m_out(outfile), m_rvz(rvz), m_type(type),
        m_level(level), m_chunk(chunk_size), m_callback(std::move(callback)),
        m_iso_size(infile->GetDataSize()),
        m_chunks_per_wii_group(std::max<u64>(1, WII_GROUP / chunk_size)),
        m_wii_groups_per_chunk(std::max<u64>(1, chunk_size / WII_GROUP)),
        m_lists_compressed(type > Type::Purge)
  {
  }

  ConversionResultCode Run();

private:
  ConversionResultCode MakePlan();
  void AddRaw(u64 offset, u64 size);
  PartitionPart AddPartitionPart(u64 offset, u64 size, u8 part);

  ConversionResult<JobResult> Process(WorkerState* state, Job job);
  void ProcessPartition(WorkerState* state, const Job& job, std::vector<GroupOut>* groups,
                        const FileSystem* file_system);
  ConversionResultCode Store(WorkerState* state, GroupOut* group);
  ConversionResultCode Write(JobResult result);

  bool WriteTable(const u8* data, size_t size, u64* offset_out);
  bool PadTo4();

  BlobReader* const m_in;
  const VolumeDisc* const m_volume;
  File::DirectIOFile* const m_out;
  const bool m_rvz;
  const Type m_type;
  const int m_level;
  const int m_chunk;
  const CompressCB m_callback;
  const u64 m_iso_size;
  const u64 m_chunks_per_wii_group;
  const u64 m_wii_groups_per_chunk;
  const bool m_lists_compressed;

  Plan m_plan;
  std::vector<Group> m_groups;
  ReuseTable m_reuse;
  u64 m_written = 0;
  u64 m_header_room = 0;
};

void Writer::AddRaw(u64 offset, u64 size)
{
  // The first 0x80 bytes are kept in the header instead.
  const u64 skip = offset < DISC_HEADER_COPY ? std::min(DISC_HEADER_COPY - offset, size) : 0;
  offset += skip;
  size -= skip;
  if (size == 0)
    return;

  const u32 groups = static_cast<u32>(AlignUp(size, m_chunk) / m_chunk);
  m_plan.order.push_back(Planned{false, static_cast<u32>(m_plan.raw_data.size()), 0});
  m_plan.raw_data.push_back(RawDataRecord{Common::swap64(offset), Common::swap64(size),
                                          Common::swap32(m_plan.group_count),
                                          Common::swap32(groups)});
  m_plan.group_count += groups;
}

PartitionPart Writer::AddPartitionPart(u64 offset, u64 size, u8 part)
{
  const u64 whole_sectors = size / SECTOR * SECTOR;
  const u32 groups = static_cast<u32>(AlignUp(whole_sectors, m_chunk) / m_chunk);
  const PartitionPart result{Common::swap32(static_cast<u32>(offset / SECTOR)),
                             Common::swap32(static_cast<u32>(size / SECTOR)),
                             Common::swap32(m_plan.group_count), Common::swap32(groups)};
  m_plan.order.push_back(Planned{true, static_cast<u32>(m_plan.partitions.size()), part});
  m_plan.group_count += groups;
  return result;
}

ConversionResultCode Writer::MakePlan()
{
  // Partitions are only stored as partitions (decrypted, without hashes) when the disc has
  // normal encrypted and hashed partitions; anything else is stored as raw data.
  std::vector<Partition> partitions;
  if (m_volume && m_volume->HasWiiHashes() && m_volume->HasWiiEncryption())
    partitions = m_volume->GetPartitions();
  std::ranges::sort(partitions, {}, &Partition::offset);

  u64 covered_to = 0;
  for (const Partition& partition : partitions)
  {
    if (partition.offset < covered_to)
    {
      WARN_LOG_FMT(DISCIO, "Overlapping partitions at {:x}", partition.offset);
      continue;
    }
    // A partition header starts with a ticket's signature type. Anything else is probably a
    // partition that a scrubbing tool blanked without removing it from the table.
    if (m_volume->ReadSwapped<u32>(partition.offset, PARTITION_NONE) != 0x10001U)
    {
      WARN_LOG_FMT(DISCIO, "Invalid partition at {:x}", partition.offset);
      continue;
    }

    const std::optional<u64> data_offset =
        m_volume->ReadSwappedAndShifted(partition.offset + 0x2b8, PARTITION_NONE);
    const std::optional<u64> data_size =
        m_volume->ReadSwappedAndShifted(partition.offset + 0x2bc, PARTITION_NONE);
    if (!data_offset || !data_size)
      return ConversionResultCode::ReadFailed;

    const u64 data_start = partition.offset + *data_offset;
    const u64 data_end = data_start + *data_size;
    if (data_start % SECTOR != 0)
    {
      WARN_LOG_FMT(DISCIO, "Misaligned partition at {:x}", partition.offset);
      continue;
    }
    if (*data_size < SECTOR)
    {
      WARN_LOG_FMT(DISCIO, "Very small partition at {:x}", partition.offset);
      continue;
    }
    if (data_end > m_iso_size)
      WARN_LOG_FMT(DISCIO, "Too large partition at {:x}", partition.offset);

    const std::optional<u64> fst_offset = GetFSTOffset(*m_volume, partition);
    const std::optional<u64> fst_size = GetFSTSize(*m_volume, partition);
    if (!fst_offset || !fst_size)
      return ConversionResultCode::ReadFailed;

    const IOS::ES::TicketReader& ticket = m_volume->GetTicket(partition);
    if (!ticket.IsValid())
      return ConversionResultCode::ReadFailed;

    AddRaw(covered_to, partition.offset - covered_to);
    AddRaw(partition.offset, *data_offset);  // the partition header

    // Split after the file system tables (rounded to a Wii group) so they decode quickly.
    const u64 fst_end = m_volume->PartitionOffsetToRawOffset(*fst_offset + *fst_size, partition);
    const u64 split = std::min(data_end, AlignUp(fst_end - data_start, WII_GROUP) + data_start);

    PartitionRecord record;
    record.key = ticket.GetTitleKey();
    record.parts[0] = AddPartitionPart(data_start, split - data_start, 0);
    record.parts[1] = AddPartitionPart(split, data_end - split, 1);

    // Partial sectors at the end aren't part of the partition record.
    covered_to = (Common::swap32(record.parts[1].first_sector) +
                  Common::swap32(record.parts[1].sector_count)) *
                 SECTOR;

    m_plan.partitions.push_back(record);
    m_plan.file_systems.push_back(m_volume->GetFileSystem(partition));
  }

  AddRaw(covered_to, m_iso_size - covered_to);
  return ConversionResultCode::Success;
}

ConversionResult<JobResult> Writer::Process(WorkerState* state, Job job)
{
  std::vector<GroupOut> groups;
  const Planned& where = *job.where;

  if (!where.is_partition)
  {
    const FileSystem* file_system =
        m_volume ? m_volume->GetFileSystem(PARTITION_NONE) : nullptr;
    GroupOut& group = groups.emplace_back();
    if (AllBytesSame(job.data.data(), job.data.data() + job.data.size()))
    {
      const u8 value = job.data.front();
      group.reuse = ReuseKey{TitleKey{}, job.data.size(), false, value, value};
    }

    if (m_rvz)
    {
      RVZPack(job.data.data(), &group, job.data.size(), 1, job.data.size(), job.data_offset,
              false, true, m_type != Type::None, file_system);
    }
    else
    {
      group.data = std::move(job.data);
    }
  }
  else
  {
    ProcessPartition(state, job, &groups, m_plan.file_systems[where.index]);
  }

  for (GroupOut& group : groups)
  {
    const ConversionResultCode result = Store(state, &group);
    if (result != ConversionResultCode::Success)
      return std::unexpected(result);
  }
  return JobResult{std::move(groups), job.bytes_read, job.first_group};
}

void Writer::ProcessPartition(WorkerState* state, const Job& job, std::vector<GroupOut>* groups,
                              const FileSystem* file_system)
{
  const PartitionRecord& partition = m_plan.partitions[job.where->index];
  const auto aes = Common::AES::CreateContextDecrypt(partition.key.data());

  const u8* const data = job.data.data();
  const u64 data_size = job.data.size();
  ASSERT(data_size % SECTOR == 0);
  const u64 wii_groups = AlignUp(data_size, WII_GROUP) / WII_GROUP;
  const u64 blocks = data_size / SECTOR;

  // Chunks (stored groups) of this job, in blocks.
  const u64 blocks_per_chunk = m_chunks_per_wii_group == 1 ?
                                   m_wii_groups_per_chunk * BLOCKS_PER_WII_GROUP :
                                   BLOCKS_PER_WII_GROUP / m_chunks_per_wii_group;
  const u64 chunks = AlignUp(blocks, blocks_per_chunk) / blocks_per_chunk;
  const u64 raw_per_chunk = blocks_per_chunk * SECTOR;
  const u64 plain_per_chunk = blocks_per_chunk * SECTOR_DATA;

  const auto reuse_key = [&](u8 value, u8 fill, bool encrypted, u64 first_block) {
    const u64 size = std::min(blocks - first_block, blocks_per_chunk) * SECTOR_DATA;
    return ReuseKey{partition.key, size, encrypted, value, fill};
  };

  // Chunks whose encrypted data is one repeated byte (unlikely to be real data) can be reused.
  for (u64 c = 0; c < chunks; ++c)
  {
    GroupOut& group = groups->emplace_back();
    const u8* begin = data + c * raw_per_chunk;
    const u8* end = std::min(data + data_size, begin + raw_per_chunk);
    if (AllBytesSame(begin, end))
      group.reuse = reuse_key(data[0], *begin, true, c * blocks_per_chunk);

    // (Only saves work: Write() makes the final decision.)
    if (!m_reuse.Lookup(&group) && group.reuse)
    {
      for (u64 k = 0; k < c; ++k)
      {
        if ((*groups)[k].reuse && !((*groups)[k].reuse < *group.reuse) &&
            !(*group.reuse < *(*groups)[k].reuse))
        {
          group.reused = (*groups)[k].reused;
          break;
        }
      }
    }
  }

  if (std::ranges::all_of(*groups, [](const GroupOut& g) { return g.reused.has_value(); }))
    return;

  const u64 list_count = m_chunks_per_wii_group == 1 ? m_wii_groups_per_chunk : chunks;
  std::vector<std::vector<HashException>> lists(list_count);

  for (u64 w = 0; w < wii_groups; ++w)
  {
    const u8* const raw_group = data + w * WII_GROUP;
    const u64 blocks_here = std::min<u64>(BLOCKS_PER_WII_GROUP, blocks - w * BLOCKS_PER_WII_GROUP);

    for (u64 b = 0; b < BLOCKS_PER_WII_GROUP; ++b)
    {
      if (b < blocks_here)
        VolumeWii::DecryptBlockData(raw_group + b * SECTOR, state->plain[b].data(), aes.get());
      else
        state->plain[b].fill(0);
    }
    VolumeWii::HashGroup(state->plain.data(), state->hashes.data());

    // Hash exceptions: wherever the disc's hashes differ from the recomputed ones. A chunk
    // smaller than a Wii group that may be reused also keeps all its hashes, since in another
    // Wii group the recomputed hashes would differ.
    for (u64 b = 0; b < blocks_here; ++b)
    {
      const u64 c = b / blocks_per_chunk;
      if ((*groups)[c].reused)
        continue;
      std::vector<HashException>& list = lists[m_chunks_per_wii_group == 1 ? w : c];
      const bool keep_all = m_chunks_per_wii_group != 1 && (*groups)[c].reuse.has_value();
      const u64 block_base = b % blocks_per_chunk * SECTOR_HASHES;

      VolumeWii::HashBlock stored;
      VolumeWii::DecryptBlockHashes(raw_group + b * SECTOR, &stored, aes.get());
      const u8* const want = reinterpret_cast<const u8*>(&stored);
      const u8* const have = reinterpret_cast<const u8*>(&state->hashes[b]);

      // Compare in 20-byte steps per field; a field that isn't a multiple of 20 bytes gets a
      // last step aligned to its end.
      using HB = VolumeWii::HashBlock;
      static constexpr std::array<std::pair<size_t, size_t>, 6> FIELDS = {{
          {offsetof(HB, h0), sizeof(HB::h0)},
          {offsetof(HB, padding_0), sizeof(HB::padding_0)},
          {offsetof(HB, h1), sizeof(HB::h1)},
          {offsetof(HB, padding_1), sizeof(HB::padding_1)},
          {offsetof(HB, h2), sizeof(HB::h2)},
          {offsetof(HB, padding_2), sizeof(HB::padding_2)},
      }};
      for (const auto& [field, field_size] : FIELDS)
      {
        for (size_t step = 0; step < field_size; step += DIGEST)
        {
          const size_t at = field + std::min(step, field_size - DIGEST);
          if (keep_all || std::memcmp(want + at, have + at, DIGEST) != 0)
          {
            HashException& e = list.emplace_back();
            e.offset = Common::swap16(static_cast<u16>(block_base + at));
            std::memcpy(e.hash.data(), want + at, DIGEST);
          }
        }
      }
    }

    const u8* const plain = state->plain[0].data();
    if (m_rvz)
    {
      // Seeds depend on the position within a sector, so chunks smaller than a Wii group
      // (which may be reused at another position) can't share seeds through reuse.
      RVZPack(plain, groups->data(), std::min(plain_per_chunk, WII_GROUP_DATA), chunks,
              blocks_here * SECTOR_DATA, job.data_offset + w * WII_GROUP_DATA, wii_groups > 1,
              m_chunks_per_wii_group == 1, m_type != Type::None, file_system);
      continue;
    }

    for (u64 c = 0; c < chunks; ++c)
    {
      GroupOut& group = (*groups)[c];
      const u8* const chunk_plain = plain + c * plain_per_chunk;
      if (group.reused)
        continue;

      const u64 chunk_total =
          std::min(plain_per_chunk, (blocks - c * blocks_per_chunk) * SECTOR_DATA);
      if (w == 0)
        group.data.resize(chunk_total);
      const u64 here = std::min(chunk_total, WII_GROUP_DATA);
      const u64 write_at = w * WII_GROUP_DATA;
      // Only the part that belongs to the chunk (the last Wii group may be partial).
      std::memcpy(group.data.data() + write_at, chunk_plain,
                  std::min<u64>(here, group.data.size() - write_at));

      // Decrypted data that is one repeated byte can be reused too (if it ends up without
      // hash exceptions, checked below).
      const bool same = AllBytesSame(chunk_plain, chunk_plain + here);
      if (w == 0 && !group.reuse)
      {
        if (same)
          group.reuse = reuse_key(*chunk_plain, *chunk_plain, false, c * blocks_per_chunk);
      }
      else if (group.reuse && !group.reuse->encrypted &&
               (!same || group.reuse->value != *chunk_plain))
      {
        group.reuse.reset();
      }
    }
  }

  for (u64 l = 0; l < lists.size(); ++l)
  {
    GroupOut& group = (*groups)[m_chunks_per_wii_group == 1 ? 0 : l];
    if (group.reused)
      continue;
    Append(&group.exception_lists, Common::swap16(static_cast<u16>(lists[l].size())));
    for (const HashException& e : lists[l])
      Append(&group.exception_lists, e);
  }

  for (GroupOut& group : *groups)
  {
    if (group.reuse && !group.reuse->encrypted &&
        !AllBytesAre(group.exception_lists.data(),
                     group.exception_lists.data() + group.exception_lists.size(), 0))
    {
      group.reuse.reset();
    }
  }
}

ConversionResultCode Writer::Store(WorkerState* state, GroupOut* group)
{
  if (m_reuse.Lookup(group))
    return ConversionResultCode::Success;

  // An empty group reads as zeroes.
  if (group->reuse && !group->reuse->encrypted && group->reuse->value == 0)
  {
    group->exception_lists.clear();
    group->data.clear();
    group->packed_size = 0;
    group->compressed = false;
    return ConversionResultCode::Success;
  }

  const auto pad_lists = [group] {
    while (group->exception_lists.size() % 4 != 0)
      group->exception_lists.push_back(0);
  };

  Encoder* const encoder = state->encoder.get();
  if (encoder && !encoder->Begin(group->exception_lists.size() + group->data.size()))
    return ConversionResultCode::InternalError;

  if (!group->exception_lists.empty())
  {
    if (m_lists_compressed && encoder)
    {
      if (!encoder->Feed(group->exception_lists.data(), group->exception_lists.size()))
        return ConversionResultCode::InternalError;
    }
    else
    {
      if (!m_lists_compressed)
        pad_lists();
      if (encoder &&
          !encoder->HashOnly(group->exception_lists.data(), group->exception_lists.size()))
      {
        return ConversionResultCode::InternalError;
      }
    }
  }

  if (encoder && (!encoder->Feed(group->data.data(), group->data.size()) || !encoder->Finish()))
    return ConversionResultCode::InternalError;

  bool use_encoded = encoder != nullptr;
  if (m_rvz)
  {
    // RVZ stores a group uncompressed when compression doesn't make it smaller.
    const size_t plain_size =
        group->data.size() + (m_lists_compressed ? AlignUp(group->exception_lists.size(), 4) : 0);
    use_encoded = encoder && encoder->Size() < plain_size;
    group->compressed = use_encoded;
    if (!use_encoded)
      pad_lists();
  }

  if (use_encoded)
  {
    group->data.assign(encoder->Data(), encoder->Data() + encoder->Size());
    if (m_lists_compressed)
      group->exception_lists.clear();  // they're inside the compressed data
  }
  return ConversionResultCode::Success;
}

bool Writer::PadTo4()
{
  static constexpr std::array<u8, 3> ZEROES{};
  const u64 n = AlignUp(m_written, 4) - m_written;
  if (n == 0)
    return true;
  m_written += n;
  return m_out->Write(ZEROES.data(), n);
}

ConversionResultCode Writer::Write(JobResult result)
{
  Group* entry = &m_groups[result.first_group];
  for (GroupOut& group : result.groups)
  {
    if (m_reuse.Lookup(&group))
    {
      *entry++ = *group.reused;
      continue;
    }

    if (m_written >> 2 > std::numeric_limits<u32>::max())
      return ConversionResultCode::InternalError;
    ASSERT(m_written % 4 == 0);

    entry->offset_div4 = static_cast<u32>(m_written >> 2);
    const u32 size = static_cast<u32>(group.exception_lists.size() + group.data.size());
    if (m_rvz)
    {
      entry->size_field = (size & 0x7FFFFFFF) | (group.compressed ? 0x80000000 : 0);
      entry->packed_size = static_cast<u32>(group.packed_size);
    }
    else
    {
      entry->size_field = size;
    }

    if (!m_out->Write(group.exception_lists) || !m_out->Write(group.data))
      return ConversionResultCode::WriteFailed;
    m_written += group.exception_lists.size() + group.data.size();

    if (group.reuse)
      m_reuse.Add(*group.reuse, *entry);

    if (!PadTo4())
      return ConversionResultCode::WriteFailed;
    ++entry;
  }

  // Progress
  const size_t groups_done = result.first_group + result.groups.size();
  const int ratio =
      result.bytes_read == 0 ? 0 : static_cast<int>(100 * m_written / result.bytes_read);
  const std::string text = Common::FmtFormatT("{0} of {1} blocks. Compression ratio {2}%",
                                              groups_done, m_plan.group_count, ratio);
  if (!m_callback(text, static_cast<float>(result.bytes_read) / m_iso_size))
    return ConversionResultCode::Canceled;
  return ConversionResultCode::Success;
}

bool Writer::WriteTable(const u8* data, size_t size, u64* offset_out)
{
  // Tables go into the room reserved at the start of the file, or else at its end.
  if (m_written <= m_header_room && m_written + size > m_header_room)
  {
    WARN_LOG_FMT(DISCIO, "Headers did not fit in the allocated space. Writing to end of file "
                         "instead");
    if (!m_out->Seek(0, File::SeekOrigin::End))
      return false;
    m_written = m_out->Tell();
  }

  *offset_out = m_written;
  if (!m_out->Write(data, size))
    return false;
  m_written += size;
  return PadTo4();
}

ConversionResultCode Writer::Run()
{
  ASSERT(m_in->GetDataSizeType() == DataSizeType::Accurate);
  ASSERT(m_chunk > 0);

  if (const ConversionResultCode result = MakePlan(); result != ConversionResultCode::Success)
    return result;
  m_groups.resize(m_plan.group_count);

  const size_t group_record_size = GroupRecordSize(m_rvz);
  const size_t partitions_bytes = m_plan.partitions.size() * sizeof(PartitionRecord);
  const size_t raw_data_bytes = m_plan.raw_data.size() * sizeof(RawDataRecord);
  const size_t group_bytes = m_groups.size() * group_record_size;

  // Room for the headers and tables at the start of the file, estimated before the tables are
  // known: 0x100 bytes of slack for compression overhead, and RVZ group tables assumed to
  // compress to at most 9/16 of their size.
  {
    u64 room = sizeof(FileHead) + sizeof(DiscHead) + partitions_bytes + raw_data_bytes + 0x100;
    if (m_rvz && m_type > Type::Purge)
      room += u64(group_bytes) * 9 / 16;
    else
      room += group_bytes;
    m_header_room = AlignUp(room, SECTOR);
  }

  std::vector<u8> buffer(m_header_room);
  m_out->Write(buffer);
  m_written = m_header_room;

  DiscHead disc_head{};
  if (!m_in->Read(0, disc_head.disc_header.size(), disc_head.disc_header.data()))
    return ConversionResultCode::ReadFailed;

  MultithreadedCompressor<WorkerState, Job, JobResult> workers(
      [this](WorkerState* state) {
        state->encoder = MakeEncoder(m_type, m_level, nullptr, nullptr);
        return ConversionResultCode::Success;
      },
      [this](WorkerState* state, Job job) { return Process(state, std::move(job)); },
      [this](JobResult result) { return Write(std::move(result)); });

  u64 bytes_read = 0;
  size_t groups_queued = 0;
  for (const Planned& where : m_plan.order)
  {
    u32 first_group, group_count;
    u64 offset, size, data_offset;
    if (where.is_partition)
    {
      const PartitionRecord& partition = m_plan.partitions[where.index];
      const PartitionPart& part = partition.parts[where.part];
      first_group = Common::swap32(part.first_group);
      group_count = Common::swap32(part.group_count);
      const u32 first_sector = Common::swap32(part.first_sector);
      offset = first_sector * SECTOR;
      size = Common::swap32(part.sector_count) * SECTOR;
      const u32 sector_in_partition = first_sector - Common::swap32(partition.parts[0].first_sector);
      data_offset = sector_in_partition * SECTOR_DATA;
    }
    else
    {
      const RawDataRecord& raw = m_plan.raw_data[where.index];
      first_group = Common::swap32(raw.first_group);
      group_count = Common::swap32(raw.group_count);
      offset = Common::swap64(raw.offset);
      size = Common::swap64(raw.size);
      // Raw groups start on sector boundaries.
      const u64 lead = offset % SECTOR;
      offset -= lead;
      size += lead;
      data_offset = offset;
    }

    ASSERT(groups_queued == first_group);
    ASSERT(bytes_read == offset);

    const u32 end_group = first_group + group_count;
    while (groups_queued < end_group)
    {
      if (const ConversionResultCode status = workers.GetStatus();
          status != ConversionResultCode::Success)
      {
        return status;
      }

      // Partition data is processed a whole Wii group at a time (the hashes need it).
      u64 n = m_chunk;
      if (where.is_partition)
        n = std::max<u64>(n, WII_GROUP);
      n = std::min<u64>(n, offset + size - bytes_read);

      buffer.resize(n);
      if (!m_in->Read(bytes_read, n, buffer.data()))
        return ConversionResultCode::ReadFailed;
      bytes_read += n;

      workers.CompressAndWrite(Job{buffer, &where, data_offset, bytes_read, groups_queued});

      offset += n;
      size -= n;
      data_offset += where.is_partition ? n / SECTOR * SECTOR_DATA : n;
      groups_queued += AlignUp(n, m_chunk) / m_chunk;
    }
    ASSERT(size == 0);
  }
  ASSERT(groups_queued == m_plan.group_count);
  ASSERT(bytes_read == m_iso_size);

  workers.Shutdown();
  if (const ConversionResultCode status = workers.GetStatus();
      status != ConversionResultCode::Success)
  {
    return status;
  }

  // The tables. The raw data and group tables use the file's compression; for LZMA, setting
  // up the encoder also yields the parameters the header must record.
  const std::unique_ptr<Encoder> encoder =
      MakeEncoder(m_type, m_level, disc_head.compressor_data, &disc_head.compressor_data_size);
  const auto encode = [&encoder](const u8* data, size_t size) -> std::optional<std::vector<u8>> {
    if (!encoder)
      return std::vector<u8>(data, data + size);
    if (!encoder->Begin(size) || !encoder->Feed(data, size) || !encoder->Finish())
      return std::nullopt;
    return std::vector<u8>(encoder->Data(), encoder->Data() + encoder->Size());
  };

  std::vector<u8> group_records;
  group_records.reserve(group_bytes);
  for (const Group& g : m_groups)
  {
    Append(&group_records, Common::swap32(g.offset_div4));
    Append(&group_records, Common::swap32(g.size_field));
    if (m_rvz)
      Append(&group_records, Common::swap32(g.packed_size));
  }

  const std::optional<std::vector<u8>> raw_data_table =
      encode(reinterpret_cast<const u8*>(m_plan.raw_data.data()), raw_data_bytes);
  if (!raw_data_table)
    return ConversionResultCode::InternalError;
  const std::optional<std::vector<u8>> group_table = encode(group_records.data(), group_bytes);
  if (!group_table)
    return ConversionResultCode::InternalError;

  m_written = sizeof(FileHead) + sizeof(DiscHead);
  if (!m_out->Seek(m_written, File::SeekOrigin::Begin))
    return ConversionResultCode::WriteFailed;

  u64 partitions_at, raw_data_at, groups_at;
  if (!WriteTable(reinterpret_cast<const u8*>(m_plan.partitions.data()), partitions_bytes,
                  &partitions_at) ||
      !WriteTable(raw_data_table->data(), raw_data_table->size(), &raw_data_at) ||
      !WriteTable(group_table->data(), group_table->size(), &groups_at))
  {
    return ConversionResultCode::WriteFailed;
  }

  u32 disc_type = 0;
  if (m_volume && m_volume->GetVolumeType() == Platform::GameCubeDisc)
    disc_type = 1;
  else if (m_volume && m_volume->GetVolumeType() == Platform::WiiDisc)
    disc_type = 2;

  disc_head.disc_type = Common::swap32(disc_type);
  disc_head.compression_type = Common::swap32(static_cast<u32>(m_type));
  disc_head.compression_level = static_cast<s32>(Common::swap32(static_cast<u32>(m_level)));
  disc_head.chunk_size = Common::swap32(static_cast<u32>(m_chunk));
  disc_head.partition_count = Common::swap32(static_cast<u32>(m_plan.partitions.size()));
  disc_head.partition_entry_size = Common::swap32(static_cast<u32>(sizeof(PartitionRecord)));
  disc_head.partitions_offset = Common::swap64(partitions_at);
  disc_head.partitions_hash = Common::SHA1::CalculateDigest(
      reinterpret_cast<const u8*>(m_plan.partitions.data()), partitions_bytes);
  disc_head.raw_data_count = Common::swap32(static_cast<u32>(m_plan.raw_data.size()));
  disc_head.raw_data_offset = Common::swap64(raw_data_at);
  disc_head.raw_data_stored_size = Common::swap32(static_cast<u32>(raw_data_table->size()));
  disc_head.group_count = Common::swap32(static_cast<u32>(m_groups.size()));
  disc_head.groups_offset = Common::swap64(groups_at);
  disc_head.groups_stored_size = Common::swap32(static_cast<u32>(group_table->size()));

  FileHead file_head{};
  file_head.magic = m_rvz ? RVZ_MAGIC : WIA_MAGIC;
  file_head.version = Common::swap32(m_rvz ? RVZ_VERSION : WIA_VERSION);
  file_head.version_compatible = Common::swap32(m_rvz ? RVZ_OLDEST_READER : WIA_OLDEST_READER);
  file_head.disc_head_size = Common::swap32(static_cast<u32>(sizeof(DiscHead)));
  file_head.disc_head_hash =
      Common::SHA1::CalculateDigest(reinterpret_cast<const u8*>(&disc_head), sizeof(disc_head));
  file_head.iso_file_size = Common::swap64(m_in->GetDataSize());
  file_head.wia_file_size = Common::swap64(m_out->GetSize());
  file_head.file_head_hash = Common::SHA1::CalculateDigest(
      reinterpret_cast<const u8*>(&file_head), offsetof(FileHead, file_head_hash));

  if (!m_out->Seek(0, File::SeekOrigin::Begin) || !m_out->Write(Common::AsU8Span(file_head)) ||
      !m_out->Write(Common::AsU8Span(disc_head)))
  {
    return ConversionResultCode::WriteFailed;
  }
  return ConversionResultCode::Success;
}
}  // namespace

// =============================================================================================

template <bool RVZ>
WIARVZFileReader<RVZ>::WIARVZFileReader(std::unique_ptr<WIARVZReader> reader)
    : m_reader(std::move(reader))
{
}

template <bool RVZ>
WIARVZFileReader<RVZ>::~WIARVZFileReader() = default;

template <bool RVZ>
std::unique_ptr<WIARVZFileReader<RVZ>> WIARVZFileReader<RVZ>::Create(File::DirectIOFile file,
                                                                     const std::string& path)
{
  std::unique_ptr<WIARVZReader> reader = WIARVZReader::Open(std::move(file), path, RVZ);
  if (!reader)
    return nullptr;
  return std::unique_ptr<WIARVZFileReader>(new WIARVZFileReader(std::move(reader)));
}

template <bool RVZ>
BlobType WIARVZFileReader<RVZ>::GetBlobType() const
{
  return m_reader->GetBlobType();
}

template <bool RVZ>
std::unique_ptr<BlobReader> WIARVZFileReader<RVZ>::CopyReader() const
{
  std::unique_ptr<BlobReader> copy = m_reader->CopyReader();
  if (!copy)
    return nullptr;
  return std::unique_ptr<BlobReader>(new WIARVZFileReader(
      std::unique_ptr<WIARVZReader>(static_cast<WIARVZReader*>(copy.release()))));
}

template <bool RVZ>
u64 WIARVZFileReader<RVZ>::GetRawSize() const
{
  return m_reader->GetRawSize();
}

template <bool RVZ>
u64 WIARVZFileReader<RVZ>::GetDataSize() const
{
  return m_reader->GetDataSize();
}

template <bool RVZ>
u64 WIARVZFileReader<RVZ>::GetBlockSize() const
{
  return m_reader->GetBlockSize();
}

template <bool RVZ>
std::string WIARVZFileReader<RVZ>::GetCompressionMethod() const
{
  return m_reader->GetCompressionMethod();
}

template <bool RVZ>
std::optional<int> WIARVZFileReader<RVZ>::GetCompressionLevel() const
{
  return m_reader->GetCompressionLevel();
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::Read(u64 offset, u64 size, u8* out_ptr)
{
  return m_reader->Read(offset, size, out_ptr);
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::SupportsReadWiiDecrypted(u64 offset, u64 size,
                                                     u64 partition_data_offset) const
{
  return m_reader->SupportsReadWiiDecrypted(offset, size, partition_data_offset);
}

template <bool RVZ>
bool WIARVZFileReader<RVZ>::ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr,
                                             u64 partition_data_offset)
{
  return m_reader->ReadWiiDecrypted(offset, size, out_ptr, partition_data_offset);
}

template class WIARVZFileReader<false>;
template class WIARVZFileReader<true>;

bool ConvertToWIAOrRVZ(BlobReader* infile, const std::string& infile_path,
                       const std::string& outfile_path, bool rvz,
                       WIARVZCompressionType compression_type, int compression_level,
                       int chunk_size, const CompressCB& callback)
{
  File::DirectIOFile outfile(outfile_path, File::AccessMode::Write);
  if (!outfile.IsOpen())
  {
    PanicAlertFmtT(
        "Failed to open the output file \"{0}\".\n"
        "Check that you have permissions to write the target folder and that the media can "
        "be written.",
        outfile_path);
    return false;
  }

  const std::unique_ptr<VolumeDisc> infile_volume = CreateDisc(infile_path);
  ConversionResultCode result;
  {
    Writer writer(infile, infile_volume.get(), &outfile, rvz, compression_type, compression_level,
                  chunk_size, callback);
    result = writer.Run();
  }

  if (result == ConversionResultCode::ReadFailed)
    PanicAlertFmtT("Failed to read from the input file \"{0}\".", infile_path);

  if (result == ConversionResultCode::WriteFailed)
  {
    PanicAlertFmtT("Failed to write the output file \"{0}\".\n"
                   "Check that you have enough space available on the target drive.",
                   outfile_path);
  }

  if (result != ConversionResultCode::Success)
  {
    // Don't leave a half-written file behind.
    outfile.Close();
    File::Delete(outfile_path);
  }

  return result == ConversionResultCode::Success;
}

}  // namespace DiscIO
