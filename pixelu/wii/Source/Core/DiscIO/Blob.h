// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// A "blob" is the raw byte stream of a GameCube or Wii disc, whatever container it is stored
// in (plain ISO, one of the compressed formats, a WBFS partition, an extracted directory...).
// CreateBlobReader looks at a file and returns the reader that understands it.

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace DiscIO
{
enum class WIARVZCompressionType : u32;

// The numeric values are stored in the game list cache (GameFileCache.cpp); bump its
// CACHE_REVISION when changing this enum.
enum class BlobType
{
  PLAIN,
  DRIVE,
  DIRECTORY,
  GCZ,
  CISO,
  WBFS,
  TGC,
  WIA,
  RVZ,
  MOD_DESCRIPTOR,
  NFS,
  SPLIT_PLAIN,
};

// How GetDataSize relates to the size of the equivalent plain disc image.
enum class DataSizeType
{
  // Exactly the same size.
  Accurate,
  // Never more than the real size; reads past it yield filler data.
  LowerBound,
  // Never less than the real size (usually much more).
  UpperBound,
};

std::string GetName(BlobType blob_type, bool translate);

class BlobReader
{
public:
  virtual ~BlobReader() = default;

  virtual BlobType GetBlobType() const = 0;
  virtual std::unique_ptr<BlobReader> CopyReader() const = 0;

  virtual u64 GetRawSize() const = 0;
  virtual u64 GetDataSize() const = 0;
  virtual DataSizeType GetDataSizeType() const = 0;

  // 0 when the container has no block structure.
  virtual u64 GetBlockSize() const = 0;
  virtual bool HasFastRandomAccessInBlock() const = 0;
  virtual std::string GetCompressionMethod() const = 0;
  virtual std::optional<int> GetCompressionLevel() const = 0;

  // Not safe to call concurrently on the same reader.
  virtual bool Read(u64 offset, u64 size, u8* out_ptr) = 0;

  template <typename T>
  std::optional<T> ReadSwapped(u64 offset)
  {
    T value;
    if (!Read(offset, sizeof(T), reinterpret_cast<u8*>(&value)))
      return std::nullopt;
    return Common::FromBigEndian(value);
  }

  // Formats that keep Wii partition data decrypted can hand it out directly.
  virtual bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const
  {
    return false;
  }
  virtual bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset)
  {
    return false;
  }

  // Only the in-memory caching reader (CachedBlob.h) answers true.
  virtual bool IsCached() const { return false; }

protected:
  BlobReader() = default;
};

// Base for formats made of equally sized blocks that can only be decoded whole. It turns byte
// reads into block reads and keeps the most recently decoded blocks around.
// GetDataSize() should be a multiple of the block size.
class SectorReader : public BlobReader
{
public:
  ~SectorReader() override = 0;

  bool Read(u64 offset, u64 size, u8* out_ptr) override;

protected:
  void SetSectorSize(int block_size);
  int GetSectorSize() const { return static_cast<int>(m_block_size); }

  // Decodes one whole block into out (GetSectorSize() bytes).
  virtual bool GetBlock(u64 block_num, u8* out) = 0;

private:
  struct CachedBlock
  {
    std::vector<u8> bytes;
    u64 index = 0;
    u64 last_use = 0;
    bool valid = false;
  };

  const CachedBlock* FetchBlock(u64 block_num);

  static constexpr size_t CACHE_SIZE = 32;
  u64 m_block_size = 0;
  u64 m_use_counter = 0;
  std::array<CachedBlock, CACHE_SIZE> m_cache;
};

// Opens the file at `filename` with whichever reader recognises it.
std::unique_ptr<BlobReader> CreateBlobReader(const std::string& filename);

// Progress callback for conversions: (status text, fraction done). Returning false cancels.
using CompressCB = std::function<bool(const std::string& text, float percent)>;

bool ConvertToGCZ(BlobReader* infile, const std::string& infile_path,
                  const std::string& outfile_path, u32 sub_type, int sector_size,
                  const CompressCB& callback);
bool ConvertToPlain(BlobReader* infile, const std::string& infile_path,
                    const std::string& outfile_path, const CompressCB& callback);
bool ConvertToWIAOrRVZ(BlobReader* infile, const std::string& infile_path,
                       const std::string& outfile_path, bool rvz,
                       WIARVZCompressionType compression_type, int compression_level,
                       int chunk_size, const CompressCB& callback);

}  // namespace DiscIO
