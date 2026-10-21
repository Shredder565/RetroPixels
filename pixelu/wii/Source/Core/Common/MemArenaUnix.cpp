// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The memory arena on Linux (and other systems with POSIX shared memory).
//
// The backing store is an anonymous memory file: memfd_create() where the kernel has it, else a
// POSIX shared-memory object that is unlinked as soon as it is open, so in both cases nothing is
// left behind in the file system and the memory goes away with the last mapping and descriptor.
// Views are MAP_SHARED mappings of that file, so every view of the same offset sees the same
// bytes, which is how mirrors of the emulated memory work.
//
// The fastmem region is one PROT_NONE reservation. Pieces of the file are mapped over parts of it
// with MAP_FIXED; to unmap a piece, a fresh PROT_NONE reservation is mapped over it again, so the
// address range never goes back to the system while the region is held (another thread's mmap
// could otherwise land inside it).

#include "Common/MemArena.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace Common
{
namespace
{
// A reservation: address space with no access and no memory charged to it.
constexpr int RESERVE_PROT = PROT_NONE;
constexpr int RESERVE_FLAGS = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

int ProtectionFor(bool writeable)
{
  return writeable ? (PROT_READ | PROT_WRITE) : PROT_READ;
}

std::string LastError()
{
  return std::strerror(errno);
}

// Opens a new, empty memory file named after `name`. Returns -1 on failure.
int OpenMemoryFile(const std::string& name)
{
#if defined(__linux__) && defined(MFD_CLOEXEC)
  const int memfd = memfd_create(name.c_str(), MFD_CLOEXEC);
  if (memfd >= 0)
    return memfd;
  // ENOSYS: a kernel older than 3.17, or a sandbox that filters the call. Fall back to shm_open.
  WARN_LOG_FMT(MEMMAP, "memfd_create failed ({}), using shm_open instead", LastError());
#endif

  // shm_open wants a single leading slash and no other.
  const std::string shm_name = "/" + name;
  const int fd =
      shm_open(shm_name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0)
  {
    ERROR_LOG_FMT(MEMMAP, "shm_open({}) failed: {}", shm_name, LastError());
    return -1;
  }
  // Only the descriptor is needed from here on.
  shm_unlink(shm_name.c_str());
  return fd;
}
}  // namespace

MemArena::MemArena() = default;

MemArena::~MemArena()
{
  ReleaseMemoryRegion();
  ReleaseSHMSegment();
}

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  ReleaseSHMSegment();

  const std::string name = fmt::format("{}.{}", base_name, getpid());
  const int fd = OpenMemoryFile(name);
  if (fd < 0)
  {
    PanicAlertFmt("Could not create the emulated memory ({} bytes).", size);
    return;
  }

  if (ftruncate(fd, static_cast<off_t>(size)) != 0)
  {
    PanicAlertFmt("Could not size the emulated memory to {} bytes: {}", size, LastError());
    close(fd);
    return;
  }

  m_shm_fd = fd;
  m_shm_size = size;
}

void MemArena::ReleaseSHMSegment()
{
  if (m_shm_fd >= 0)
    close(m_shm_fd);
  m_shm_fd = -1;
  m_shm_size = 0;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  if (m_shm_fd < 0)
    return nullptr;

  void* const view =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, static_cast<off_t>(offset));
  if (view == MAP_FAILED)
  {
    ERROR_LOG_FMT(MEMMAP, "Mapping a view of {:#x} bytes at offset {:#x} failed: {}", size, offset,
                  LastError());
    return nullptr;
  }
  return view;
}

void MemArena::ReleaseView(void* view, size_t size)
{
  if (view && munmap(view, size) != 0)
    ERROR_LOG_FMT(MEMMAP, "Unmapping the view at {} failed: {}", view, LastError());
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  if (m_reserved_region)
  {
    ERROR_LOG_FMT(MEMMAP, "The arena already holds a reserved region");
    return nullptr;
  }

  void* const region = mmap(nullptr, memory_size, RESERVE_PROT, RESERVE_FLAGS, -1, 0);
  if (region == MAP_FAILED)
  {
    ERROR_LOG_FMT(MEMMAP, "Reserving {:#x} bytes of address space failed: {}", memory_size,
                  LastError());
    return nullptr;
  }

  m_reserved_region = region;
  m_reserved_size = memory_size;
  return static_cast<u8*>(region);
}

void MemArena::ReleaseMemoryRegion()
{
  if (!m_reserved_region)
    return;

  if (munmap(m_reserved_region, m_reserved_size) != 0)
    ERROR_LOG_FMT(MEMMAP, "Releasing the reserved region failed: {}", LastError());
  m_reserved_region = nullptr;
  m_reserved_size = 0;
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  if (m_shm_fd < 0 || !base)
    return nullptr;

  void* const mapped = mmap(base, size, ProtectionFor(writeable), MAP_SHARED | MAP_FIXED, m_shm_fd,
                            static_cast<off_t>(offset));
  if (mapped == MAP_FAILED)
  {
    ERROR_LOG_FMT(MEMMAP, "Mapping {:#x} bytes at offset {:#x} to {} failed: {}", size, offset,
                  base, LastError());
    return nullptr;
  }
  // MAP_FIXED either maps at `base` or fails.
  ASSERT(mapped == base);
  return mapped;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  if (mprotect(view, size, ProtectionFor(writeable)) != 0)
  {
    ERROR_LOG_FMT(MEMMAP, "Changing the protection of {:#x} bytes at {} failed: {}", size, view,
                  LastError());
    return false;
  }
  return true;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
  // Replaces the file mapping with a reservation again (see the top of the file).
  void* const result = mmap(view, size, RESERVE_PROT, RESERVE_FLAGS | MAP_FIXED, -1, 0);
  if (result == MAP_FAILED)
    ERROR_LOG_FMT(MEMMAP, "Unmapping {:#x} bytes at {} failed: {}", size, view, LastError());
}

size_t MemArena::GetPageSize() const
{
  static const size_t page_size = [] {
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<size_t>(value) : size_t{4096};
  }();
  return page_size;
}

// The kernel hands out zero pages for an anonymous private mapping and only backs the pages that
// are written, which is exactly what LazyMemoryRegion promises; nothing has to be done per block.

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);
  if (size == 0)
    return nullptr;

  void* const memory = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (memory == MAP_FAILED)
  {
    ERROR_LOG_FMT(MEMMAP, "Reserving a lazy region of {:#x} bytes failed: {}", size, LastError());
    return nullptr;
  }

  m_memory = memory;
  m_size = size;
  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);
  // For a private anonymous mapping MADV_DONTNEED drops the pages; the next read of each sees
  // zeros again.
  if (madvise(m_memory, m_size, MADV_DONTNEED) == 0)
    return;

  // Should not happen; mapping a fresh region over the old one has the same effect.
  void* const fresh = mmap(m_memory, m_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  if (fresh == MAP_FAILED)
    std::memset(m_memory, 0, m_size);
}

void LazyMemoryRegion::Release()
{
  if (m_memory)
    munmap(m_memory, m_size);
  m_memory = nullptr;
  m_size = 0;
}

}  // namespace Common
