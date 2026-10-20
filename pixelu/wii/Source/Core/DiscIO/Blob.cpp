// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/Blob.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "Common/MsgHandler.h"

#include "DiscIO/CISOBlob.h"
#include "DiscIO/CompressedBlob.h"
#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/FileBlob.h"
#include "DiscIO/NFSBlob.h"
#include "DiscIO/SplitFileBlob.h"
#include "DiscIO/TGCBlob.h"
#include "DiscIO/WIABlob.h"
#include "DiscIO/WbfsBlob.h"

namespace DiscIO
{
std::string GetName(BlobType blob_type, bool translate)
{
  // Container names that are acronyms stay untranslated; the descriptive ones go through the
  // translation catalogue when asked to.
  const auto text = [translate](const char* s) {
    return translate ? Common::GetStringT(s) : std::string(s);
  };

  switch (blob_type)
  {
  case BlobType::PLAIN:
    return "ISO";
  case BlobType::GCZ:
    return "GCZ";
  case BlobType::CISO:
    return "CISO";
  case BlobType::WBFS:
    return "WBFS";
  case BlobType::TGC:
    return "TGC";
  case BlobType::WIA:
    return "WIA";
  case BlobType::RVZ:
    return "RVZ";
  case BlobType::NFS:
    return "NFS";
  case BlobType::DIRECTORY:
    return text("Directory");
  case BlobType::MOD_DESCRIPTOR:
    return text("Mod");
  case BlobType::SPLIT_PLAIN:
    return text("Multi-part ISO");
  case BlobType::DRIVE:
  default:
    return {};
  }
}

// ---------------------------------------------------------------------------------------------
// SectorReader

SectorReader::~SectorReader() = default;

void SectorReader::SetSectorSize(int block_size)
{
  m_block_size = static_cast<u64>(std::max(block_size, 0));
  for (CachedBlock& entry : m_cache)
  {
    entry.bytes.assign(m_block_size, 0);
    entry.valid = false;
    entry.last_use = 0;
  }
}

const SectorReader::CachedBlock* SectorReader::FetchBlock(u64 block_num)
{
  ++m_use_counter;

  // Hit?
  for (CachedBlock& entry : m_cache)
  {
    if (entry.valid && entry.index == block_num)
    {
      entry.last_use = m_use_counter;
      return &entry;
    }
  }

  // Miss: decode into the least recently used slot (empty slots have last_use 0).
  CachedBlock* victim = &m_cache[0];
  for (CachedBlock& entry : m_cache)
  {
    if (!entry.valid)
    {
      victim = &entry;
      break;
    }
    if (entry.last_use < victim->last_use)
      victim = &entry;
  }

  victim->valid = false;
  if (!GetBlock(block_num, victim->bytes.data()))
    return nullptr;

  victim->index = block_num;
  victim->last_use = m_use_counter;
  victim->valid = true;
  return victim;
}

bool SectorReader::Read(u64 offset, u64 size, u8* out_ptr)
{
  if (offset + size > GetDataSize())
    return false;

  while (size > 0)
  {
    const u64 block_num = offset / m_block_size;
    const u64 offset_in_block = offset % m_block_size;
    const u64 chunk = std::min(m_block_size - offset_in_block, size);

    const CachedBlock* block = FetchBlock(block_num);
    if (!block)
      return false;

    std::memcpy(out_ptr, block->bytes.data() + offset_in_block, chunk);
    out_ptr += chunk;
    offset += chunk;
    size -= chunk;
  }

  return true;
}

// ---------------------------------------------------------------------------------------------

std::unique_ptr<BlobReader> CreateBlobReader(const std::string& filename)
{
  File::DirectIOFile file(filename, File::AccessMode::Read);

  u32 magic;
  if (!file.Read(Common::AsWritableU8Span(magic)))
    return nullptr;

  // Every container format starts with a four-byte signature. Files without a known signature
  // are an extracted disc (if the path points at its sys/main.dol), the first part of a split
  // image, or otherwise taken to be a plain image; if that guess is wrong the volume code will
  // notice when the disc header doesn't make sense.
  switch (magic)
  {
  case GCZ_MAGIC:
    return CompressedBlobReader::Create(std::move(file), filename);
  case CISO_MAGIC:
    return CISOFileReader::Create(std::move(file));
  case WBFS_MAGIC:
    return WbfsFileReader::Create(std::move(file), filename);
  case TGC_MAGIC:
    return TGCFileReader::Create(std::move(file));
  case WIA_MAGIC:
    return WIAFileReader::Create(std::move(file), filename);
  case RVZ_MAGIC:
    return RVZFileReader::Create(std::move(file), filename);
  case NFS_MAGIC:
    return NFSFileReader::Create(std::move(file), filename);
  default:
    break;
  }

  if (std::unique_ptr<DirectoryBlobReader> extracted = DirectoryBlobReader::Create(filename))
    return extracted;

  if (std::unique_ptr<SplitPlainFileReader> split = SplitPlainFileReader::Create(filename))
    return split;

  return PlainFileReader::Create(std::move(file));
}

}  // namespace DiscIO
