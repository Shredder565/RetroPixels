// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/ScrubbedBlob.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "DiscIO/Blob.h"
#include "DiscIO/DiscScrubber.h"
#include "DiscIO/VolumeDisc.h"

namespace DiscIO
{
ScrubbedBlob::ScrubbedBlob(std::unique_ptr<BlobReader> inner, DiscScrubber scrubber)
    : m_inner(std::move(inner)), m_scrubber(std::move(scrubber))
{
}

std::unique_ptr<ScrubbedBlob> ScrubbedBlob::Create(const std::string& path)
{
  // The scrubber needs a parsed volume; the reads go through a separate plain blob reader.
  const std::unique_ptr<VolumeDisc> disc = CreateDisc(path);
  if (!disc)
    return nullptr;

  DiscScrubber scrubber;
  if (!scrubber.SetupScrub(*disc))
    return nullptr;

  std::unique_ptr<BlobReader> inner = CreateBlobReader(path);
  if (!inner)
    return nullptr;

  return std::unique_ptr<ScrubbedBlob>(new ScrubbedBlob(std::move(inner), std::move(scrubber)));
}

std::unique_ptr<BlobReader> ScrubbedBlob::CopyReader() const
{
  return std::unique_ptr<BlobReader>(new ScrubbedBlob(m_inner->CopyReader(), m_scrubber));
}

bool ScrubbedBlob::Read(u64 offset, u64 size, u8* out_ptr)
{
  // One cluster (or the part of one that's wanted) at a time.
  constexpr u64 CLUSTER = DiscScrubber::CLUSTER_SIZE;
  while (size > 0)
  {
    const u64 n = std::min(CLUSTER - offset % CLUSTER, size);
    if (m_scrubber.CanBlockBeScrubbed(offset))
      std::memset(out_ptr, 0, n);
    else if (!m_inner->Read(offset, n, out_ptr))
      return false;

    offset += n;
    out_ptr += n;
    size -= n;
  }
  return true;
}

}  // namespace DiscIO
