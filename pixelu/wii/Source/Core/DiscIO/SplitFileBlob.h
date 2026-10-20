// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
// A plain disc image cut into pieces named name.part0.iso, name.part1.iso, ... (as written by
// tools that work around FAT32's 4 GiB limit). The pieces are read back as one image.
class SplitPlainFileReader final : public BlobReader
{
public:
  // Takes the path of the .part0.iso piece. Needs at least two non-empty pieces.
  static std::unique_ptr<SplitPlainFileReader> Create(std::string_view first_file_path);

  BlobType GetBlobType() const override { return BlobType::SPLIT_PLAIN; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_size; }
  u64 GetDataSize() const override { return m_size; }
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr) override;

private:
  struct Piece
  {
    File::DirectIOFile file;
    u64 start;  // offset of the piece's first byte in the whole image
    u64 size;
  };

  explicit SplitPlainFileReader(std::vector<Piece> pieces);

  std::vector<Piece> m_pieces;
  u64 m_size = 0;
};

}  // namespace DiscIO
