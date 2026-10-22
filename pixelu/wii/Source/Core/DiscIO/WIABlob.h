// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// WIA (from Wiimm's ISO Tools) and RVZ (its PixelU/Dolphin derivative) store a disc as a list
// of compressed "groups" (chunks). Wii partition data is kept decrypted and without its hash
// blocks, which are recomputed on reading; hashes that don't match what the disc had are kept
// as "hash exceptions". RVZ additionally stores the discs' pseudorandom padding as generator
// seeds. The reading and writing code lives in WIABlob.cpp and WIACompression.cpp.

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Swap.h"
#include "DiscIO/Blob.h"
// Not needed here; kept so that files which relied on this header for them still build.
#include "DiscIO/WiiEncryptionCache.h"

namespace DiscIO
{
// Stored in the files; don't renumber.
enum class WIARVZCompressionType : u32
{
  None = 0,
  Purge = 1,
  Bzip2 = 2,
  LZMA = 3,
  LZMA2 = 4,
  Zstd = 5,
};

// The range of compression levels to offer for a compression type ({0, -1} if levels don't
// apply). `gui` leaves out zstd's negative levels.
std::pair<int, int> GetAllowedCompressionLevels(WIARVZCompressionType compression_type, bool gui);

constexpr u32 WIA_MAGIC = 0x01414957;  // "WIA\x1" read as a little endian u32
constexpr u32 RVZ_MAGIC = 0x015A5652;  // "RVZ\x1" read as a little endian u32

class WIARVZReader;

template <bool RVZ>
class WIARVZFileReader final : public BlobReader
{
public:
  ~WIARVZFileReader() override;

  static std::unique_ptr<WIARVZFileReader> Create(File::DirectIOFile file, const std::string& path);

  BlobType GetBlobType() const override;
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override;
  bool HasFastRandomAccessInBlock() const override { return false; }
  std::string GetCompressionMethod() const override;
  std::optional<int> GetCompressionLevel() const override;

  bool Read(u64 offset, u64 size, u8* out_ptr) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) override;

private:
  explicit WIARVZFileReader(std::unique_ptr<WIARVZReader> reader);

  // All the work is done by a format-agnostic reader in WIABlob.cpp.
  std::unique_ptr<WIARVZReader> m_reader;
};

using WIAFileReader = WIARVZFileReader<false>;
using RVZFileReader = WIARVZFileReader<true>;

extern template class WIARVZFileReader<false>;
extern template class WIARVZFileReader<true>;

}  // namespace DiscIO
