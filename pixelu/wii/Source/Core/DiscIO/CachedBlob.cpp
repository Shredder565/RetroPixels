// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/CachedBlob.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MemArena.h"

#include "DiscIO/DiscScrubber.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
namespace
{
constexpr u64 CLUSTER = DiscScrubber::CLUSTER_SIZE;

// Owns the in-memory copy and the thread that fills it, front to back. Shared by a reader and
// all its copies.
class MemoryCopy final
{
public:
  MemoryCopy(std::unique_ptr<BlobReader> source, bool skip_unused)
  {
    m_thread = std::thread([this, src = std::move(source), skip_unused]() mutable {
      Fill(std::move(src), skip_unused);
    });
  }

  ~MemoryCopy()
  {
    m_cancel.store(true, std::memory_order_relaxed);
    m_thread.join();
  }

  MemoryCopy(const MemoryCopy&) = delete;
  MemoryCopy& operator=(const MemoryCopy&) = delete;

  // Succeeds only if every byte asked for is already in memory.
  bool TryRead(u64 offset, u64 size, u8* out) const
  {
    if (size == 0)
      return true;

    // m_filled_to is published after the memory is written (and after m_base is set).
    if (offset + size > m_filled_to.load(std::memory_order_acquire))
      return false;

    // Skipped clusters were never copied. Check every cluster the read touches.
    for (u64 cluster = offset / CLUSTER; cluster <= (offset + size - 1) / CLUSTER; ++cluster)
    {
      if (m_scrubber.CanBlockBeScrubbed(cluster * CLUSTER))
      {
        WARN_LOG_FMT(DISCIO, "Disc cache: Read({}, {}) touches a cluster that wasn't cached",
                     offset, size);
        return false;
      }
    }

    std::memcpy(out, m_base + offset, size);
    return true;
  }

private:
  void Fill(std::unique_ptr<BlobReader> source, bool skip_unused)
  {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const u64 total = source->GetDataSize();

    m_base = static_cast<u8*>(m_memory.Create(total));
    if (!m_base)
    {
      ERROR_LOG_FMT(DISCIO, "Disc cache: couldn't reserve {} bytes", total);
      return;
    }

    // The scrubber is only set up here, before anything is published, so readers never see
    // it change.
    u64 to_copy = total;
    if (skip_unused)
    {
      const std::unique_ptr<Volume> volume = CreateVolume(source->CopyReader());
      if (volume && m_scrubber.SetupScrub(*volume))
      {
        for (u64 pos = 0; pos < total; pos += CLUSTER)
        {
          if (m_scrubber.CanBlockBeScrubbed(pos))
            to_copy -= std::min(CLUSTER, total - pos);
        }
      }
      else
      {
        WARN_LOG_FMT(DISCIO, "Disc cache: couldn't scrub, caching the whole disc");
      }
    }

    u64 copied = 0;
    auto next_report = started + std::chrono::seconds(1);
    for (u64 pos = 0; pos < total;)
    {
      if (m_cancel.load(std::memory_order_relaxed))
      {
        INFO_LOG_FMT(DISCIO, "Disc cache: stopped");
        return;
      }

      const u64 n = std::min(CLUSTER, total - pos);
      if (!m_scrubber.CanBlockBeScrubbed(pos))
      {
        m_memory.EnsureMemoryPagesWritable(pos, n);
        if (!source->Read(pos, n, m_base + pos))
        {
          ERROR_LOG_FMT(DISCIO, "Disc cache: Read({}, {}) failed", pos, n);
          return;
        }
        copied += n;
      }
      pos += n;
      m_filled_to.store(pos, std::memory_order_release);

      const auto now = Clock::now();
      if (now >= next_report)
      {
        INFO_LOG_FMT(DISCIO, "Disc cache: {}%", to_copy == 0 ? 100 : copied * 100 / to_copy);
        next_report = now + std::chrono::seconds(1);
      }
    }

    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    NOTICE_LOG_FMT(DISCIO, "Disc cache: done, {:.2f} of {:.2f} MiB in {:.2f} s",
                   copied / 1048576.0, total / 1048576.0, seconds);
  }

  Common::LazyMemoryRegion m_memory;
  u8* m_base = nullptr;
  DiscScrubber m_scrubber;
  std::atomic<u64> m_filled_to{0};
  std::atomic<bool> m_cancel{false};
  std::thread m_thread;  // last, so everything above exists before it starts
};

class CachedBlobReader final : public BlobReader
{
public:
  CachedBlobReader(std::shared_ptr<MemoryCopy> copy, std::unique_ptr<BlobReader> inner)
      : m_copy(std::move(copy)), m_inner(std::move(inner))
  {
  }

  std::unique_ptr<BlobReader> CopyReader() const override
  {
    return std::make_unique<CachedBlobReader>(m_copy, m_inner->CopyReader());
  }

  BlobType GetBlobType() const override { return m_inner->GetBlobType(); }
  u64 GetRawSize() const override { return GetDataSize(); }
  u64 GetDataSize() const override { return m_inner->GetDataSize(); }
  DataSizeType GetDataSizeType() const override { return m_inner->GetDataSizeType(); }

  // Reads from memory are cheap and unblocked.
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    return m_copy->TryRead(offset, size, out_ptr) || m_inner->Read(offset, size, out_ptr);
  }

  bool IsCached() const override { return true; }

private:
  const std::shared_ptr<MemoryCopy> m_copy;
  const std::unique_ptr<BlobReader> m_inner;
};

std::unique_ptr<BlobReader> Wrap(std::unique_ptr<BlobReader> reader, bool skip_unused)
{
  if (reader->IsCached())
    return reader;
  // The filling thread gets its own reader, since readers can't be shared between threads.
  auto copy = std::make_shared<MemoryCopy>(reader->CopyReader(), skip_unused);
  return std::make_unique<CachedBlobReader>(std::move(copy), std::move(reader));
}
}  // namespace

std::unique_ptr<BlobReader> CreateCachedBlobReader(std::unique_ptr<BlobReader> reader)
{
  return Wrap(std::move(reader), false);
}

std::unique_ptr<BlobReader> CreateScrubbingCachedBlobReader(std::unique_ptr<BlobReader> reader)
{
  return Wrap(std::move(reader), true);
}

}  // namespace DiscIO
