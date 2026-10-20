// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/CompressedBlob.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "DiscIO/MultithreadedCompressor.h"

namespace DiscIO
{
namespace
{
constexpr u64 STORED_RAW = u64(1) << 63;

// A block is only kept deflated when that saves at least this many bytes.
constexpr u32 MIN_SAVING = 10;

// Room for a deflated block a little larger than the block itself.
constexpr u32 READ_SLACK = 64;

u64 OffsetOf(u64 entry)
{
  return entry & ~STORED_RAW;
}

bool IsStoredRaw(u64 entry)
{
  return (entry & STORED_RAW) != 0;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Reading

std::unique_ptr<CompressedBlobReader> CompressedBlobReader::Create(File::DirectIOFile file,
                                                                   const std::string& filename)
{
  std::unique_ptr<CompressedBlobReader> reader(
      new CompressedBlobReader(std::move(file), filename));
  if (!reader->Open())
    return nullptr;
  return reader;
}

CompressedBlobReader::CompressedBlobReader(File::DirectIOFile file, std::string filename)
    : m_file(std::move(file)), m_file_name(std::move(filename))
{
}

CompressedBlobReader::~CompressedBlobReader() = default;

bool CompressedBlobReader::Open()
{
  if (!m_file.OffsetRead(0, Common::AsWritableU8Span(m_header)) ||
      m_header.magic_cookie != GCZ_MAGIC)
  {
    return false;
  }
  m_file_size = m_file.GetSize();

  const u64 count = m_header.num_blocks;
  const u64 tables_end =
      sizeof(CompressedBlobHeader) + count * (sizeof(u64) + sizeof(u32));
  if (tables_end > m_file_size)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: the block tables don't fit in the file");
    return false;
  }
  if (m_header.compressed_data_size > m_file_size - tables_end)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: the block data doesn't fit in the file");
    return false;
  }
  if (count == 0 || m_header.block_size == 0)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: the image has no blocks");
    return false;
  }

  m_block_offsets.resize(count);
  m_block_hashes.resize(count);
  if (!m_file.OffsetRead(sizeof(CompressedBlobHeader), Common::AsWritableU8Span(m_block_offsets)) ||
      !m_file.OffsetRead(sizeof(CompressedBlobHeader) + count * sizeof(u64),
                         Common::AsWritableU8Span(m_block_hashes)))
  {
    return false;
  }

  m_data_start = tables_end;
  m_stored_block.resize(u64(m_header.block_size) + READ_SLACK);
  SetSectorSize(m_header.block_size);

  // Every block must lie inside the data area and have a size that GetBlock will accept.
  u64 bad_blocks = 0;
  for (u64 i = 0; i < count; ++i)
  {
    const u64 begin = OffsetOf(m_block_offsets[i]);
    const u64 end =
        i + 1 < count ? OffsetOf(m_block_offsets[i + 1]) : m_header.compressed_data_size;
    const bool ok = begin <= m_header.compressed_data_size &&
                    end <= m_header.compressed_data_size && begin <= end &&
                    (IsStoredRaw(m_block_offsets[i]) ? end - begin == m_header.block_size :
                                                       end - begin <= m_stored_block.size());
    if (!ok)
      ++bad_blocks;
  }
  if (bad_blocks != 0)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: {} blocks have invalid offsets", bad_blocks);
    return false;
  }
  return true;
}

std::unique_ptr<BlobReader> CompressedBlobReader::CopyReader() const
{
  return Create(m_file, m_file_name);
}

u64 CompressedBlobReader::GetBlockCompressedSize(u64 block_num) const
{
  const u64 count = m_header.num_blocks;
  if (block_num >= count)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: no block {}", block_num);
    return 0;
  }
  const u64 end =
      block_num + 1 < count ? OffsetOf(m_block_offsets[block_num + 1]) : m_header.compressed_data_size;
  return end - OffsetOf(m_block_offsets[block_num]);
}

bool CompressedBlobReader::GetBlock(u64 block_num, u8* out_ptr)
{
  if (block_num >= m_header.num_blocks)
    return false;

  const u64 entry = m_block_offsets[block_num];
  const u64 stored_size = GetBlockCompressedSize(block_num);
  const bool raw = IsStoredRaw(entry);

  if (raw ? stored_size != m_header.block_size : stored_size > m_stored_block.size())
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: block {} has an impossible size", block_num);
    return false;
  }

  if (!m_file.OffsetRead(m_data_start + OffsetOf(entry), m_stored_block.data(), stored_size))
  {
    ERROR_LOG_FMT(DISCIO, "The disc image \"{}\" is truncated, some of the data is missing.",
                  m_file_name);
    return false;
  }

  // A hash mismatch is reported but not fatal: the data may still decode.
  const u32 hash = Common::HashAdler32(m_stored_block.data(), stored_size);
  if (hash != m_block_hashes[block_num])
  {
    ERROR_LOG_FMT(DISCIO, "The disc image \"{}\" is corrupt: block {} hashes to {:08x}, not {:08x}.",
                  m_file_name, block_num, hash, m_block_hashes[block_num]);
  }

  if (raw)
  {
    std::memcpy(out_ptr, m_stored_block.data(), m_header.block_size);
    return true;
  }

  // The block is good as long as it inflates to exactly one full block.
  z_stream stream{};
  stream.next_in = m_stored_block.data();
  stream.avail_in = static_cast<uInt>(stored_size);
  stream.next_out = out_ptr;
  stream.avail_out = m_header.block_size;
  inflateInit(&stream);
  const int status = inflate(&stream, Z_SYNC_FLUSH);
  const uInt missing = stream.avail_out;
  inflateEnd(&stream);

  if (status != Z_STREAM_END)
    ERROR_LOG_FMT(DISCIO, "GCZ: block {} didn't end cleanly", block_num);
  if (missing != 0)
  {
    ERROR_LOG_FMT(DISCIO, "GCZ: block {} inflated to the wrong size", block_num);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Writing

namespace
{
// One deflate stream per worker, reused for every block. It must stay where it is once
// initialised, hence no copying or moving.
struct DeflateState
{
  DeflateState() = default;
  DeflateState(const DeflateState&) = delete;
  DeflateState& operator=(const DeflateState&) = delete;
  ~DeflateState()
  {
    if (initialised)
      deflateEnd(&stream);
  }

  z_stream stream{};
  bool initialised = false;
};

struct BlockIn
{
  std::vector<u8> data;
  u32 index = 0;
  u64 input_end = 0;  // disc offset just past this block
};

struct BlockOut
{
  std::vector<u8> data;  // exactly what goes into the file
  u32 index = 0;
  bool deflated = false;
  u64 input_end = 0;
};
}  // namespace

bool ConvertToGCZ(BlobReader* infile, const std::string& infile_path,
                  const std::string& outfile_path, u32 sub_type, int block_size,
                  const CompressCB& callback)
{
  ASSERT(infile->GetDataSizeType() == DataSizeType::Accurate);

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

  callback(Common::GetStringT("Files opened, ready to compress."), 0);

  const u64 disc_size = infile->GetDataSize();
  const u32 block_count = static_cast<u32>((disc_size + block_size - 1) / block_size);

  std::vector<u64> block_offsets(block_count);
  std::vector<u32> block_hashes(block_count);

  // The header and the two tables are written last; leave room for them.
  const u64 tables_size =
      sizeof(CompressedBlobHeader) + u64(block_count) * (sizeof(u64) + sizeof(u32));
  outfile.Seek(static_cast<s64>(tables_size), File::SeekOrigin::Current);

  // Report progress roughly a thousand times over the whole conversion.
  const u32 report_every = std::max<u32>(1, block_count / 1000);
  u64 bytes_written = 0;

  const auto set_up = [](DeflateState* state) {
    if (deflateInit(&state->stream, 9) != Z_OK)
      return ConversionResultCode::InternalError;
    state->initialised = true;
    return ConversionResultCode::Success;
  };

  const auto compress = [&](DeflateState* state, BlockIn in) -> ConversionResult<BlockOut> {
    if (deflateReset(&state->stream) != Z_OK)
    {
      ERROR_LOG_FMT(DISCIO, "GCZ: deflateReset failed");
      return std::unexpected(ConversionResultCode::InternalError);
    }

    std::vector<u8> packed(block_size);
    state->stream.next_in = in.data.data();
    state->stream.avail_in = block_size;
    state->stream.next_out = packed.data();
    state->stream.avail_out = block_size;
    const int status = deflate(&state->stream, Z_FINISH);
    const uInt left = state->stream.avail_out;

    BlockOut out;
    out.index = in.index;
    out.input_end = in.input_end;
    out.deflated = status == Z_STREAM_END && left >= MIN_SAVING;
    if (out.deflated)
    {
      packed.resize(block_size - left);
      out.data = std::move(packed);
    }
    else
    {
      out.data = std::move(in.data);
    }

    // Each worker handles different blocks, so these writes never overlap.
    block_hashes[out.index] = Common::HashAdler32(out.data.data(), out.data.size());
    return out;
  };

  const auto output = [&](BlockOut out) {
    block_offsets[out.index] = bytes_written | (out.deflated ? 0 : STORED_RAW);
    bytes_written += out.data.size();

    if (!outfile.Write(out.data))
      return ConversionResultCode::WriteFailed;

    if (out.index % report_every == 0)
    {
      const int ratio = out.input_end == 0 ? 0 : static_cast<int>(100 * bytes_written / out.input_end);
      const std::string text = Common::FmtFormatT("{0} of {1} blocks. Compression ratio {2}%",
                                                  out.index, block_count, ratio);
      if (!callback(text, static_cast<float>(out.index) / block_count))
        return ConversionResultCode::Canceled;
    }
    return ConversionResultCode::Success;
  };

  MultithreadedCompressor<DeflateState, BlockIn, BlockOut> compressor(set_up, compress, output);

  u64 position = 0;
  for (u32 i = 0; i < block_count; ++i)
  {
    if (compressor.GetStatus() != ConversionResultCode::Success)
      break;

    // The last block is padded with zeroes up to the full block size.
    std::vector<u8> block(block_size, 0);
    const u64 to_read = std::min<u64>(block_size, disc_size - position);
    if (!infile->Read(position, to_read, block.data()))
    {
      compressor.SetError(ConversionResultCode::ReadFailed);
      break;
    }
    position += block_size;
    compressor.CompressAndWrite(BlockIn{std::move(block), i, position});
  }

  compressor.Shutdown();
  const ConversionResultCode result = compressor.GetStatus();

  if (result == ConversionResultCode::Success)
  {
    CompressedBlobHeader header{};
    header.magic_cookie = GCZ_MAGIC;
    header.sub_type = sub_type;
    header.compressed_data_size = bytes_written;
    header.disc_size = disc_size;
    header.block_size = block_size;
    header.num_blocks = block_count;

    outfile.Seek(0, File::SeekOrigin::Begin);
    outfile.Write(Common::AsU8Span(header));
    outfile.Write(Common::AsU8Span(block_offsets));
    outfile.Write(Common::AsU8Span(block_hashes));

    callback(Common::GetStringT("Done compressing disc image."), 1.0f);
  }
  else
  {
    // Don't leave a half-written image behind.
    outfile.Close();
    File::Delete(outfile_path);
  }

  if (result == ConversionResultCode::ReadFailed)
    PanicAlertFmtT("Failed to read from the input file \"{0}\".", infile_path);

  if (result == ConversionResultCode::WriteFailed)
  {
    PanicAlertFmtT("Failed to write the output file \"{0}\".\n"
                   "Check that you have enough space available on the target drive.",
                   outfile_path);
  }

  return result == ConversionResultCode::Success;
}

}  // namespace DiscIO
