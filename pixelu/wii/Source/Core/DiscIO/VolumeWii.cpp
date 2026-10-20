// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/VolumeWii.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/FileSystemGCWii.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WiiSaveBanner.h"

namespace DiscIO
{
namespace
{
// Unencrypted disc header: two flags that are 0 on retail discs.
constexpr u64 DISABLE_HASHES_FLAG_OFFSET = 0x60;
constexpr u64 DISABLE_ENCRYPTION_FLAG_OFFSET = 0x61;

// The partition tables: four (count, table offset >> 2) pairs, each table holding
// (partition offset >> 2, type) pairs.
constexpr u64 PARTITION_TABLE_INFO_OFFSET = 0x40000;
constexpr u32 PARTITION_TABLE_COUNT = 4;

// Partition header field after the ticket, TMD, cert chain and H3 table fields.
constexpr u64 PARTITION_DATA_OFFSET_ADDRESS = 0x2b8;

// The IV of a block's data area is taken from its encrypted hash area.
constexpr size_t BLOCK_DATA_IV_OFFSET = 0x3d0;

// Each H0 hash covers this much of a block's data.
constexpr size_t H0_CHUNK_SIZE = 0x400;
constexpr size_t H0_CHUNKS = 31;
static_assert(H0_CHUNK_SIZE * H0_CHUNKS == VolumeWii::BLOCK_DATA_SIZE);

// Banner names in a Wii opening.bnr (IMET header).
constexpr u64 OPENING_BNR_NAMES_OFFSET = 0x5c;
}  // namespace

VolumeWii::VolumeWii(std::unique_ptr<BlobReader> reader) : m_reader(std::move(reader))
{
  ASSERT(m_reader);

  m_has_hashes = m_reader->ReadSwapped<u8>(DISABLE_HASHES_FLAG_OFFSET) == u8(0);
  m_has_encryption = m_reader->ReadSwapped<u8>(DISABLE_ENCRYPTION_FLAG_OFFSET) == u8(0);
  if (m_has_encryption && !m_has_hashes)
    ERROR_LOG_FMT(DISCIO, "Wii disc has encryption but no hashes! This probably won't work well");

  for (u32 table = 0; table < PARTITION_TABLE_COUNT; ++table)
  {
    const u64 info = PARTITION_TABLE_INFO_OFFSET + table * 8;
    const std::optional<u32> count = m_reader->ReadSwapped<u32>(info);
    const std::optional<u64> table_offset = ReadSwappedAndShifted(info + 4, PARTITION_NONE);
    if (!count || !table_offset)
      continue;

    for (u32 i = 0; i < *count; ++i)
    {
      const u64 entry = *table_offset + u64{i} * 8;
      const std::optional<u64> offset = ReadSwappedAndShifted(entry, PARTITION_NONE);
      const std::optional<u32> type = m_reader->ReadSwapped<u32>(entry + 4);
      if (!offset || !type)
        continue;

      const Partition partition(*offset);
      // The first data partition listed is the one that gets booted.
      if (*type == PARTITION_DATA && m_game_partition == PARTITION_NONE)
        m_game_partition = partition;
      AddPartition(partition, *type);
    }
  }
}

void VolumeWii::AddPartition(const Partition& partition, u32 type)
{
  // A partition listed twice keeps its first entry.
  const auto [it, inserted] = m_partitions.try_emplace(partition);
  if (!inserted)
    return;
  PartitionInfo& info = it->second;
  info.type = type;

  const u64 start = partition.offset;

  // Reads a (size, offset >> 2) pair from the partition header and then the data it points at.
  const auto read_area = [this, start](u64 size_field, u64 offset_field) -> std::optional<
                                                                              std::vector<u8>> {
    const std::optional<u32> size = m_reader->ReadSwapped<u32>(start + size_field);
    const std::optional<u64> offset = ReadSwappedAndShifted(start + offset_field, PARTITION_NONE);
    if (!size || !offset)
      return std::nullopt;
    std::vector<u8> data(*size);
    if (!m_reader->Read(start + *offset, data.size(), data.data()))
      return std::nullopt;
    return data;
  };

  info.ticket = [this, start]() -> IOS::ES::TicketReader {
    std::vector<u8> bytes(sizeof(IOS::ES::Ticket));
    if (!m_reader->Read(start + WII_PARTITION_TICKET_ADDRESS, bytes.size(), bytes.data()))
      return INVALID_TICKET;
    return IOS::ES::TicketReader{std::move(bytes)};
  };

  info.tmd = [this, start]() -> IOS::ES::TMDReader {
    const std::optional<u32> size = m_reader->ReadSwapped<u32>(start + WII_PARTITION_TMD_SIZE_ADDRESS);
    const std::optional<u64> offset =
        ReadSwappedAndShifted(start + WII_PARTITION_TMD_OFFSET_ADDRESS, PARTITION_NONE);
    if (!size || !offset)
      return INVALID_TMD;
    // ES would reject a bad size too, but checking first avoids allocating a huge buffer.
    if (!IOS::ES::IsValidTMDSize(*size))
    {
      ERROR_LOG_FMT(DISCIO, "Invalid TMD size");
      return INVALID_TMD;
    }
    std::vector<u8> bytes(*size);
    if (!m_reader->Read(start + *offset, bytes.size(), bytes.data()))
      return INVALID_TMD;
    return IOS::ES::TMDReader{std::move(bytes)};
  };

  info.cert_chain = [read_area]() -> std::vector<u8> {
    return read_area(WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS, WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS)
        .value_or(std::vector<u8>{});
  };

  info.h3_table = [this, start]() -> std::vector<u8> {
    if (!m_has_hashes)
      return {};
    const std::optional<u64> offset =
        ReadSwappedAndShifted(start + WII_PARTITION_H3_OFFSET_ADDRESS, PARTITION_NONE);
    if (!offset)
      return {};
    std::vector<u8> table(WII_PARTITION_H3_SIZE);
    if (!m_reader->Read(start + *offset, table.size(), table.data()))
      return {};
    return table;
  };

  info.key = [this, partition]() -> std::unique_ptr<Common::AES::Context> {
    const IOS::ES::TicketReader& ticket = GetTicket(partition);
    if (!ticket.IsValid())
      return nullptr;
    return Common::AES::CreateContextDecrypt(ticket.GetTitleKey().data());
  };

  info.data_offset = [this, start]() -> u64 {
    return ReadSwappedAndShifted(start + PARTITION_DATA_OFFSET_ADDRESS, PARTITION_NONE).value_or(0);
  };

  info.file_system = [this, partition]() -> std::unique_ptr<FileSystem> {
    auto file_system = std::make_unique<FileSystemGCWii>(this, partition);
    if (!file_system->IsValid())
      return nullptr;
    return file_system;
  };
}

VolumeWii::~VolumeWii() = default;

const VolumeWii::PartitionInfo* VolumeWii::FindPartition(const Partition& partition) const
{
  const auto it = m_partitions.find(partition);
  return it != m_partitions.end() ? &it->second : nullptr;
}

bool VolumeWii::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  if (partition == PARTITION_NONE)
    return m_reader->Read(offset, length, buffer);

  const PartitionInfo* info = FindPartition(partition);
  if (!info)
    return false;
  return ReadFromPartition(offset, length, buffer, partition, *info);
}

bool VolumeWii::ReadFromPartition(u64 offset, u64 length, u8* buffer, const Partition& partition,
                                  const PartitionInfo& info) const
{
  const u64 data_start = partition.offset + *info.data_offset;

  // Some formats (like RVZ) store partitions decrypted and can serve the data directly.
  if (m_has_hashes && m_has_encryption &&
      m_reader->SupportsReadWiiDecrypted(offset, length, data_start))
  {
    return m_reader->ReadWiiDecrypted(offset, length, buffer, data_start);
  }

  // Without hashes, the partition data is stored contiguously.
  if (!m_has_hashes)
    return m_reader->Read(data_start + offset, length, buffer);

  const Common::AES::Context* key = nullptr;
  std::vector<u8> encrypted_block;
  if (m_has_encryption)
  {
    key = info.key->get();
    if (!key)
      return false;
    encrypted_block.resize(BLOCK_TOTAL_SIZE);
  }

  while (length > 0)
  {
    const u64 block_offset = data_start + offset / BLOCK_DATA_SIZE * BLOCK_TOTAL_SIZE;
    const u64 offset_in_block = offset % BLOCK_DATA_SIZE;

    if (m_cached_block_offset != block_offset)
    {
      // Forget the old block first, so that a failed read cannot leave stale data behind.
      m_cached_block_offset = UINT64_MAX;
      if (m_has_encryption)
      {
        if (!m_reader->Read(block_offset, BLOCK_TOTAL_SIZE, encrypted_block.data()))
          return false;
        key->Crypt(&encrypted_block[BLOCK_DATA_IV_OFFSET], &encrypted_block[BLOCK_HEADER_SIZE],
                   m_cached_block.data(), BLOCK_DATA_SIZE);
      }
      else if (!m_reader->Read(block_offset + BLOCK_HEADER_SIZE, BLOCK_DATA_SIZE,
                               m_cached_block.data()))
      {
        return false;
      }
      m_cached_block_offset = block_offset;
    }

    const u64 chunk = std::min(length, BLOCK_DATA_SIZE - offset_in_block);
    std::memcpy(buffer, m_cached_block.data() + offset_in_block, static_cast<size_t>(chunk));
    buffer += chunk;
    offset += chunk;
    length -= chunk;
  }

  return true;
}

bool VolumeWii::HasWiiHashes() const
{
  return m_has_hashes;
}

bool VolumeWii::HasWiiEncryption() const
{
  return m_has_encryption;
}

std::vector<Partition> VolumeWii::GetPartitions() const
{
  std::vector<Partition> partitions;
  partitions.reserve(m_partitions.size());
  for (const auto& entry : m_partitions)
    partitions.push_back(entry.first);
  return partitions;
}

Partition VolumeWii::GetGamePartition() const
{
  return m_game_partition;
}

std::optional<u32> VolumeWii::GetPartitionType(const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  if (!info)
    return std::nullopt;
  return info->type;
}

std::optional<u64> VolumeWii::GetTitleID(const Partition& partition) const
{
  const IOS::ES::TicketReader& ticket = GetTicket(partition);
  if (!ticket.IsValid())
    return std::nullopt;
  return ticket.GetTitleId();
}

const IOS::ES::TicketReader& VolumeWii::GetTicket(const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  return info ? *info->ticket : INVALID_TICKET;
}

const IOS::ES::TMDReader& VolumeWii::GetTMD(const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  return info ? *info->tmd : INVALID_TMD;
}

const std::vector<u8>& VolumeWii::GetCertificateChain(const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  return info ? *info->cert_chain : INVALID_CERT_CHAIN;
}

const FileSystem* VolumeWii::GetFileSystem(const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  return info ? info->file_system->get() : nullptr;
}

u64 VolumeWii::OffsetInHashedPartitionToRawOffset(u64 offset, const Partition& partition,
                                                  u64 partition_data_offset)
{
  if (partition == PARTITION_NONE)
    return offset;

  const u64 block = offset / BLOCK_DATA_SIZE;
  const u64 offset_in_block = offset % BLOCK_DATA_SIZE;
  return partition.offset + partition_data_offset + block * BLOCK_TOTAL_SIZE + offset_in_block;
}

u64 VolumeWii::PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  if (!info)
    return offset;

  const u64 data_offset = *info->data_offset;
  if (!m_has_hashes)
    return partition.offset + data_offset + offset;
  return OffsetInHashedPartitionToRawOffset(offset, partition, data_offset);
}

std::string VolumeWii::GetGameTDBID(const Partition& partition) const
{
  return GetGameID(partition);
}

Region VolumeWii::GetRegion() const
{
  return RegionCodeToRegion(m_reader->ReadSwapped<u32>(WII_REGION_DATA_ADDRESS));
}

std::map<Language, std::string> VolumeWii::GetLongNames() const
{
  std::vector<char16_t> names(NAMES_TOTAL_CHARS);
  const u64 bytes_read =
      ReadFile(*this, GetGamePartition(), "opening.bnr", reinterpret_cast<u8*>(names.data()),
               NAMES_TOTAL_BYTES, OPENING_BNR_NAMES_OFFSET);
  // The amount read is applied as a count of characters, so a short read keeps any name field
  // that the byte count covers (the rest of such a field reads as zeros).
  const size_t usable = static_cast<size_t>(std::min<u64>(bytes_read, names.size()));
  return ReadWiiNames(std::span<const char16_t>(names.data(), usable));
}

std::vector<u32> VolumeWii::GetBanner(u32* width, u32* height) const
{
  *width = 0;
  *height = 0;

  // Wii discs have no banner image of their own; the one from the save data is used.
  const std::optional<u64> title_id = GetTitleID(GetGamePartition());
  if (!title_id)
    return {};
  return WiiSaveBanner(*title_id).GetBanner(width, height);
}

Platform VolumeWii::GetVolumeType() const
{
  return Platform::WiiDisc;
}

bool VolumeWii::IsDatelDisc() const
{
  return m_game_partition == PARTITION_NONE;
}

BlobType VolumeWii::GetBlobType() const
{
  return m_reader->GetBlobType();
}

u64 VolumeWii::GetDataSize() const
{
  return m_reader->GetDataSize();
}

DataSizeType VolumeWii::GetDataSizeType() const
{
  return m_reader->GetDataSizeType();
}

u64 VolumeWii::GetRawSize() const
{
  return m_reader->GetRawSize();
}

const BlobReader& VolumeWii::GetBlobReader() const
{
  return *m_reader;
}

std::array<u8, 20> VolumeWii::GetSyncHash() const
{
  const std::unique_ptr<Common::SHA1::Context> context = Common::SHA1::CreateContext();

  // The unencrypted disc header and the region data.
  ReadAndAddToSyncHash(context.get(), 0, 0x80, PARTITION_NONE);
  ReadAndAddToSyncHash(context.get(), WII_REGION_DATA_ADDRESS, 4, PARTITION_NONE);

  // Where the game partition's data starts on the disc: it affects emulated read timings.
  const u64 data_start = PartitionOffsetToRawOffset(0, GetGamePartition());
  context->Update(reinterpret_cast<const u8*>(&data_start), sizeof(data_start));

  AddTMDToSyncHash(context.get(), GetGamePartition());
  AddGamePartitionToSyncHash(context.get());

  return context->Finish();
}

bool VolumeWii::CheckH3TableIntegrity(const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  if (!info)
    return false;

  const std::vector<u8>& h3_table = *info->h3_table;
  if (h3_table.size() != WII_PARTITION_H3_SIZE)
    return false;

  // The TMD of a disc partition lists a single content, whose hash is that of the H3 table.
  const IOS::ES::TMDReader& tmd = *info->tmd;
  if (!tmd.IsValid())
    return false;
  const std::vector<IOS::ES::Content> contents = tmd.GetContents();
  return contents.size() == 1 && Common::SHA1::CalculateDigest(h3_table) == contents[0].sha1;
}

bool VolumeWii::CheckBlockIntegrity(u64 block_index, const u8* encrypted_data,
                                    const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  if (!info)
    return false;

  const std::vector<u8>& h3_table = *info->h3_table;
  const u64 group = block_index / BLOCKS_PER_GROUP;
  if (group * Common::SHA1::DIGEST_LEN >= h3_table.size())
    return false;

  HashBlock hashes;
  std::vector<u8> decrypted_data;
  const u8* data;
  if (m_has_encryption)
  {
    Common::AES::Context* key = info->key->get();
    if (!key)
      return false;
    DecryptBlockHashes(encrypted_data, &hashes, key);
    decrypted_data.resize(BLOCK_DATA_SIZE);
    DecryptBlockData(encrypted_data, decrypted_data.data(), key);
    data = decrypted_data.data();
  }
  else
  {
    std::memcpy(&hashes, encrypted_data, sizeof(HashBlock));
    data = encrypted_data + BLOCK_HEADER_SIZE;
  }

  for (size_t chunk = 0; chunk < H0_CHUNKS; ++chunk)
  {
    if (Common::SHA1::CalculateDigest(data + chunk * H0_CHUNK_SIZE, H0_CHUNK_SIZE) !=
        hashes.h0[chunk])
    {
      return false;
    }
  }

  if (Common::SHA1::CalculateDigest(hashes.h0) != hashes.h1[block_index % 8])
    return false;
  if (Common::SHA1::CalculateDigest(hashes.h1) != hashes.h2[block_index / 8 % 8])
    return false;

  Common::SHA1::Digest expected_h3;
  std::memcpy(expected_h3.data(), h3_table.data() + group * Common::SHA1::DIGEST_LEN,
              expected_h3.size());
  return Common::SHA1::CalculateDigest(hashes.h2) == expected_h3;
}

bool VolumeWii::CheckBlockIntegrity(u64 block_index, const Partition& partition) const
{
  const PartitionInfo* info = FindPartition(partition);
  if (!info)
    return false;

  const u64 block_offset =
      partition.offset + *info->data_offset + block_index * BLOCK_TOTAL_SIZE;
  std::vector<u8> block(BLOCK_TOTAL_SIZE);
  if (!m_reader->Read(block_offset, block.size(), block.data()))
    return false;
  return CheckBlockIntegrity(block_index, block.data(), partition);
}

bool VolumeWii::HashGroup(const std::array<u8, BLOCK_DATA_SIZE> in[BLOCKS_PER_GROUP],
                          HashBlock out[BLOCKS_PER_GROUP],
                          const std::function<bool(size_t block)>& read_function)
{
  // The H0 hashes of each block are computed in the background while the next block is read.
  std::array<std::future<void>, BLOCKS_PER_GROUP> h0_jobs;
  bool read_ok = true;
  for (size_t block = 0; block < BLOCKS_PER_GROUP; ++block)
  {
    if (read_function && !read_function(block))
    {
      read_ok = false;
      break;
    }
    h0_jobs[block] = std::async(std::launch::async, [in, out, block] {
      for (size_t chunk = 0; chunk < H0_CHUNKS; ++chunk)
      {
        out[block].h0[chunk] =
            Common::SHA1::CalculateDigest(in[block].data() + chunk * H0_CHUNK_SIZE, H0_CHUNK_SIZE);
      }
    });
  }
  for (std::future<void>& job : h0_jobs)
  {
    if (job.valid())
      job.get();
  }
  if (!read_ok)
    return false;

  // H1: one table per subgroup of 8 blocks, stored in each of the 8. H2: one table for the
  // whole group, stored in all 64 blocks. All padding is zero.
  std::array<Common::SHA1::Digest, 8> h2;
  for (size_t subgroup = 0; subgroup < 8; ++subgroup)
  {
    HashBlock* blocks = out + subgroup * 8;
    std::array<Common::SHA1::Digest, 8> h1;
    for (size_t i = 0; i < 8; ++i)
      h1[i] = Common::SHA1::CalculateDigest(blocks[i].h0);
    for (size_t i = 0; i < 8; ++i)
      blocks[i].h1 = h1;
    h2[subgroup] = Common::SHA1::CalculateDigest(h1);
  }
  for (size_t block = 0; block < BLOCKS_PER_GROUP; ++block)
  {
    out[block].h2 = h2;
    out[block].padding_0 = {};
    out[block].padding_1 = {};
    out[block].padding_2 = {};
  }

  return true;
}

bool VolumeWii::EncryptGroup(
    u64 offset, u64 partition_data_offset, u64 partition_data_decrypted_size,
    const std::array<u8, AES_KEY_SIZE>& key, BlobReader* blob,
    std::array<u8, GROUP_TOTAL_SIZE>* out,
    const std::function<void(HashBlock hash_blocks[BLOCKS_PER_GROUP])>& hash_exception_callback)
{
  std::vector<std::array<u8, BLOCK_DATA_SIZE>> data(BLOCKS_PER_GROUP);
  std::vector<HashBlock> hashes(BLOCKS_PER_GROUP);

  // Blocks that do not lie completely inside the partition's data are left as zeros.
  const auto read_block = [&](size_t block) {
    const u64 block_start = offset + block * BLOCK_DATA_SIZE;
    if (block_start + BLOCK_DATA_SIZE > partition_data_decrypted_size)
    {
      data[block].fill(0);
      return true;
    }
    return blob->ReadWiiDecrypted(block_start, BLOCK_DATA_SIZE, data[block].data(),
                                  partition_data_offset);
  };
  if (!HashGroup(data.data(), hashes.data(), read_block))
    return false;

  if (hash_exception_callback)
    hash_exception_callback(hashes.data());

  const std::unique_ptr<Common::AES::Context> aes = Common::AES::CreateContextEncrypt(key.data());

  // Each block is encrypted on its own (hash area with a zero IV, then the data with an IV taken
  // from the encrypted hash area), so the blocks are split between a few threads.
  const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency(), 1,
                                            BLOCKS_PER_GROUP);
  std::vector<std::future<void>> jobs;
  jobs.reserve(workers);
  for (size_t worker = 0; worker < workers; ++worker)
  {
    const size_t first = worker * BLOCKS_PER_GROUP / workers;
    const size_t last = (worker + 1) * BLOCKS_PER_GROUP / workers;
    jobs.push_back(std::async(std::launch::async, [&, first, last] {
      for (size_t block = first; block < last; ++block)
      {
        u8* dest = out->data() + block * BLOCK_TOTAL_SIZE;
        aes->CryptIvZero(reinterpret_cast<const u8*>(&hashes[block]), dest, BLOCK_HEADER_SIZE);
        aes->Crypt(dest + BLOCK_DATA_IV_OFFSET, data[block].data(), dest + BLOCK_HEADER_SIZE,
                   BLOCK_DATA_SIZE);
      }
    }));
  }
  for (std::future<void>& job : jobs)
    job.get();

  return true;
}

void VolumeWii::DecryptBlockHashes(const u8* in, HashBlock* out, Common::AES::Context* aes_context)
{
  aes_context->CryptIvZero(in, reinterpret_cast<u8*>(out), sizeof(HashBlock));
}

void VolumeWii::DecryptBlockData(const u8* in, u8* out, Common::AES::Context* aes_context)
{
  aes_context->Crypt(in + BLOCK_DATA_IV_OFFSET, in + BLOCK_HEADER_SIZE, out, BLOCK_DATA_SIZE);
}

}  // namespace DiscIO
