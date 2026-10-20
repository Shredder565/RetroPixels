// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/FileBlob.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"

namespace DiscIO
{
PlainFileReader::PlainFileReader(File::DirectIOFile file)
    : m_file(std::move(file)), m_size(m_file.GetSize())
{
}

std::unique_ptr<PlainFileReader> PlainFileReader::Create(File::DirectIOFile file)
{
  if (!file.IsOpen())
    return nullptr;
  return std::unique_ptr<PlainFileReader>(new PlainFileReader(std::move(file)));
}

std::unique_ptr<BlobReader> PlainFileReader::CopyReader() const
{
  return Create(m_file);
}

bool PlainFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  return m_file.OffsetRead(offset, out_ptr, nbytes);
}

bool ConvertToPlain(BlobReader* infile, const std::string& infile_path,
                    const std::string& outfile_path, const CompressCB& callback)
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

  // Copy in pieces of at least 512 KiB, rounded to whole blocks of the source format.
  constexpr u64 MIN_PIECE = 0x80000;
  u64 piece = infile->GetBlockSize();
  if (piece == 0)
    piece = MIN_PIECE;
  while (piece < MIN_PIECE)
    piece <<= 1;

  const u64 total = infile->GetDataSize();
  const u64 pieces = (total + piece - 1) / piece;
  const u64 report_every = std::max<int>(1, static_cast<int>(pieces / 100));

  std::vector<u8> buffer(piece);
  bool ok = true;
  for (u64 i = 0; i < pieces; ++i)
  {
    if (i % report_every == 0 &&
        !callback(Common::GetStringT("Unpacking"), static_cast<float>(i) / pieces))
    {
      ok = false;
      break;
    }

    const u64 position = i * piece;
    const u64 length = std::min(piece, total - position);
    if (!infile->Read(position, length, buffer.data()))
    {
      PanicAlertFmtT("Failed to read from the input file \"{0}\".", infile_path);
      ok = false;
      break;
    }
    if (!outfile.Write(buffer.data(), length))
    {
      PanicAlertFmtT("Failed to write the output file \"{0}\".\n"
                     "Check that you have enough space available on the target drive.",
                     outfile_path);
      ok = false;
      break;
    }
  }

  if (!ok)
  {
    // Don't leave a partial image behind.
    outfile.Close();
    File::Delete(outfile_path);
  }
  return ok;
}

}  // namespace DiscIO
