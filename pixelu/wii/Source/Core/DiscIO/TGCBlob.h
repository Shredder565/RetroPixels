// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
// TGC: a GameCube disc embedded in another disc's file system (used for demo discs). It has its
// own header in front of the disc data, the DOL and FST live at other offsets, and the FST's file
// offsets are relative to a "virtual" file area. The reader presents the embedded disc as a
// normal GameCube disc.
static constexpr u32 TGC_MAGIC = 0xA2380FAE;

// All fields are big endian on disk.
struct TGCHeader
{
  u32 magic;
  u32 unknown_1;
  u32 tgc_header_size;
  u32 disc_header_area_size;

  u32 fst_real_offset;
  u32 fst_size;
  u32 fst_max_size;
  u32 dol_real_offset;

  u32 dol_size;
  u32 file_area_real_offset;
  u32 unknown_2;
  u32 unknown_3;

  u32 unknown_4;
  u32 file_area_virtual_offset;
};

class TGCFileReader final : public BlobReader
{
public:
  static std::unique_ptr<TGCFileReader> Create(File::DirectIOFile file);

  BlobType GetBlobType() const override { return BlobType::TGC; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_file_size; }
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr) override;

private:
  explicit TGCFileReader(File::DirectIOFile file);

  File::DirectIOFile m_file;
  u64 m_file_size = 0;

  // Header values in host byte order
  u32 m_header_size = 0;
  u32 m_disc_dol_offset = 0;  // where the DOL appears in the presented disc
  u32 m_disc_fst_offset = 0;  // where the FST appears in the presented disc

  // The FST with its file offsets rewritten for the presented disc
  std::vector<u8> m_fst;
};

}  // namespace DiscIO
