// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/SplitFileBlob.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace DiscIO
{
SplitPlainFileReader::SplitPlainFileReader(std::vector<Piece> pieces) : m_pieces(std::move(pieces))
{
  if (!m_pieces.empty())
    m_size = m_pieces.back().start + m_pieces.back().size;
}

std::unique_ptr<SplitPlainFileReader> SplitPlainFileReader::Create(std::string_view first_file_path)
{
  constexpr std::string_view FIRST_SUFFIX = ".part0.iso";
  if (!first_file_path.ends_with(FIRST_SUFFIX))
    return nullptr;
  const std::string_view stem =
      first_file_path.substr(0, first_file_path.size() - FIRST_SUFFIX.size());

  std::vector<Piece> pieces;
  u64 start = 0;
  for (size_t n = 0;; ++n)
  {
    File::DirectIOFile file(fmt::format("{}.part{}.iso", stem, n), File::AccessMode::Read);
    if (!file.IsOpen())
      break;  // the first missing number ends the set
    const u64 size = file.GetSize();
    if (size == 0)
      return nullptr;
    pieces.push_back(Piece{std::move(file), start, size});
    start += size;
  }

  if (pieces.size() < 2)
    return nullptr;
  return std::unique_ptr<SplitPlainFileReader>(new SplitPlainFileReader(std::move(pieces)));
}

std::unique_ptr<BlobReader> SplitPlainFileReader::CopyReader() const
{
  std::vector<Piece> copy;
  copy.reserve(m_pieces.size());
  for (const Piece& piece : m_pieces)
    copy.push_back(Piece{piece.file, piece.start, piece.size});
  return std::unique_ptr<SplitPlainFileReader>(new SplitPlainFileReader(std::move(copy)));
}

bool SplitPlainFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  if (offset >= m_size)
    return false;

  // Pieces are sorted by start; find the one holding `offset`, then walk forward.
  auto it = std::upper_bound(m_pieces.begin(), m_pieces.end(), offset,
                             [](u64 value, const Piece& piece) { return value < piece.start; });
  --it;

  while (true)
  {
    const u64 in_piece = offset - it->start;
    const u64 n = std::min(it->size - in_piece, nbytes);
    if (!it->file.OffsetRead(in_piece, out_ptr, n))
      return false;
    nbytes -= n;
    if (nbytes == 0)
      return true;
    offset += n;
    out_ptr += n;
    if (++it == m_pieces.end())
      return false;  // ran off the end of the image
  }
}

}  // namespace DiscIO
