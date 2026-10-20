// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include "DiscIO/Blob.h"
#include "DiscIO/DiscScrubber.h"

namespace DiscIO
{
// Reads a disc image as it would look with every unused cluster (as found by DiscScrubber)
// replaced by zeroes. Everything else is passed through to the underlying reader.
class ScrubbedBlob final : public BlobReader
{
public:
  // Null if the image can't be opened or its file systems can't be walked.
  static std::unique_ptr<ScrubbedBlob> Create(const std::string& path);

  BlobType GetBlobType() const override { return m_inner->GetBlobType(); }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_inner->GetRawSize(); }
  u64 GetDataSize() const override { return m_inner->GetDataSize(); }
  DataSizeType GetDataSizeType() const override { return m_inner->GetDataSizeType(); }

  u64 GetBlockSize() const override { return m_inner->GetBlockSize(); }
  bool HasFastRandomAccessInBlock() const override
  {
    return m_inner->HasFastRandomAccessInBlock();
  }
  std::string GetCompressionMethod() const override { return m_inner->GetCompressionMethod(); }
  std::optional<int> GetCompressionLevel() const override
  {
    return m_inner->GetCompressionLevel();
  }

  bool Read(u64 offset, u64 size, u8* out_ptr) override;

private:
  ScrubbedBlob(std::unique_ptr<BlobReader> inner, DiscScrubber scrubber);

  std::unique_ptr<BlobReader> m_inner;
  DiscScrubber m_scrubber;
};

}  // namespace DiscIO
