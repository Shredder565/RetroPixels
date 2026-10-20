// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/NFSBlob.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

namespace DiscIO
{
std::unique_ptr<NFSFileReader> NFSFileReader::Create(File::DirectIOFile first_file,
                                                     const std::string& path)
{
  // Expected layout: <title>/content/hif_000000.nfs, with the key in <title>/code/htk.bin.
  std::string directory, name, extension;
  SplitPath(path, &directory, &name, &extension);
  if (name + extension != "hif_000000.nfs")
    return nullptr;

  std::string title_dir, dir_name, dir_extension;
  SplitPath(std::string_view(directory).substr(0, directory.size() - 1), &title_dir, &dir_name,
            &dir_extension);
  if (dir_name + dir_extension != "content")
  {
    ERROR_LOG_FMT(DISCIO, "NFS: {} isn't inside a folder called 'content'", path);
    return nullptr;
  }

  Key key;
  const std::string key_path = title_dir + "code/htk.bin";
  if (!File::DirectIOFile(key_path, File::AccessMode::Read).Read(key))
  {
    ERROR_LOG_FMT(DISCIO, "NFS: couldn't read the key from {}", key_path);
    return nullptr;
  }

  NFSHeader header;
  if (!first_file.OffsetRead(0, Common::AsWritableU8Span(header)) || header.magic != NFS_MAGIC)
    return nullptr;

  // At most 61 ranges fit in the header, whatever the count says.
  const size_t range_count =
      std::min<size_t>(Common::swap32(header.lba_range_count), header.lba_ranges.size());
  std::vector<Range> ranges;
  u64 stored_blocks = 0;
  u32 disc_blocks = 0;  // 32-bit on purpose: the format's block numbers are
  for (size_t i = 0; i < range_count; ++i)
  {
    const u32 first = Common::swap32(header.lba_ranges[i].start_block);
    const u32 count = Common::swap32(header.lba_ranges[i].num_blocks);
    ranges.push_back(Range{first, count, stored_blocks});
    stored_blocks += count;
    disc_blocks = std::max<u32>(disc_blocks, first + count);
  }

  // Open as many hif_NNNNNN.nfs files as the stored data needs.
  const u64 needed_size = sizeof(NFSHeader) + stored_blocks * BLOCK_SIZE;
  const u64 file_count = (needed_size + MAX_FILE_SIZE - 1) / MAX_FILE_SIZE;

  std::vector<File::DirectIOFile> files;
  u64 raw_size = first_file.GetSize();
  files.push_back(std::move(first_file));
  for (u64 i = 1; i < file_count; ++i)
  {
    const std::string file_path = fmt::format("{}hif_{:06}.nfs", directory, i);
    File::DirectIOFile file(file_path, File::AccessMode::Read);
    if (!file.IsOpen())
    {
      ERROR_LOG_FMT(DISCIO, "NFS: couldn't open {}", file_path);
      return nullptr;
    }
    raw_size += file.GetSize();
    files.push_back(std::move(file));
  }

  if (raw_size < needed_size)
  {
    ERROR_LOG_FMT(DISCIO, "NFS: the files in {} hold {} bytes; at least {} are needed", directory,
                  raw_size, needed_size);
    return nullptr;
  }

  return std::unique_ptr<NFSFileReader>(new NFSFileReader(
      std::move(ranges), std::move(files), key, raw_size, u64(disc_blocks) * BLOCK_SIZE));
}

NFSFileReader::NFSFileReader(std::vector<Range> ranges, std::vector<File::DirectIOFile> files,
                             const Key& key, u64 raw_size, u64 data_size)
    : m_ranges(std::move(ranges)), m_files(std::move(files)), m_key(key),
      m_aes(Common::AES::CreateContextDecrypt(key.data())), m_raw_size(raw_size),
      m_data_size(data_size)
{
}

std::unique_ptr<BlobReader> NFSFileReader::CopyReader() const
{
  return std::unique_ptr<BlobReader>(
      new NFSFileReader(m_ranges, m_files, m_key, m_raw_size, m_data_size));
}

u64 NFSFileReader::GetDataSize() const
{
  return m_data_size;
}

u64 NFSFileReader::GetRawSize() const
{
  return m_raw_size;
}

u64 NFSFileReader::StoredIndexOf(u64 disc_block) const
{
  // If ranges overlap, the first one listed wins.
  for (const Range& range : m_ranges)
  {
    if (disc_block >= range.first && disc_block < u64(range.first) + range.count)
      return range.stored_before + (disc_block - range.first);
  }
  return NOT_STORED;
}

bool NFSFileReader::LoadBlock(u64 disc_block)
{
  const u64 stored = StoredIndexOf(disc_block);
  if (stored == NOT_STORED)
  {
    m_decrypted.fill(0);
  }
  else
  {
    // Header and blocks form one stream cut into files of MAX_FILE_SIZE bytes, so file k
    // starts at stream offset k * MAX_FILE_SIZE and the last block of a file runs 0x200 bytes
    // into the next one.
    u64 pos = sizeof(NFSHeader) + stored * BLOCK_SIZE;
    u64 done = 0;
    while (done < BLOCK_SIZE)
    {
      const u64 file_index = pos / MAX_FILE_SIZE;
      const u64 in_file = pos % MAX_FILE_SIZE;
      if (file_index >= m_files.size())
        return false;
      const u64 n = std::min<u64>(BLOCK_SIZE - done, MAX_FILE_SIZE - in_file);
      if (!m_files[file_index].OffsetRead(in_file, m_encrypted.data() + done, n))
        return false;
      done += n;
      pos += n;
    }

    std::array<u8, 16> iv{};
    const u64 be_index = Common::swap64(disc_block);
    std::memcpy(iv.data() + 8, &be_index, sizeof(be_index));
    m_aes->Crypt(iv.data(), m_encrypted.data(), m_decrypted.data(), BLOCK_SIZE);
  }

  // The disc is presented decrypted, so mark its header as such (byte 0x61) or the volume code
  // would try to decrypt it again.
  if (disc_block == 0)
    m_decrypted[0x61] = 1;

  return true;
}

bool NFSFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  // Reads beyond GetDataSize() are allowed and give zeroes (it's a lower bound).
  while (nbytes > 0)
  {
    const u64 block = offset / BLOCK_SIZE;
    const u64 in_block = offset % BLOCK_SIZE;
    if (block != m_loaded_block)
    {
      if (!LoadBlock(block))
        return false;
      m_loaded_block = block;
    }

    const u64 n = std::min<u64>(nbytes, BLOCK_SIZE - in_block);
    std::memcpy(out_ptr, m_decrypted.data() + in_block, n);
    offset += n;
    out_ptr += n;
    nbytes -= n;
  }
  return true;
}

}  // namespace DiscIO
