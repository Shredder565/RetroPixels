// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Random.h"

#include <algorithm>
#include <cstdint>

#include "Common/Assert.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace Common::Random
{
// Bytes come straight from the operating system's cryptographically secure generator
// (BCryptGenRandom on Windows, getrandom() elsewhere), which is thread-safe and needs no seeding.
void Generate(void* buffer, std::size_t size)
{
  auto* out = static_cast<std::uint8_t*>(buffer);

#ifdef _WIN32
  // BCryptGenRandom takes a ULONG length, so feed large requests in pieces.
  constexpr std::size_t MAX_CHUNK = 0x10000000;
  while (size != 0)
  {
    const std::size_t chunk = std::min(size, MAX_CHUNK);
    const NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(chunk),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ASSERT_MSG(COMMON, BCRYPT_SUCCESS(status), "BCryptGenRandom failed: {:#x}",
               static_cast<std::uint32_t>(status));
    out += chunk;
    size -= chunk;
  }
#else
  while (size != 0)
  {
    const ssize_t got = getrandom(out, size, 0);
    if (got < 0)
    {
      if (errno == EINTR)
        continue;
      ASSERT_MSG(COMMON, false, "getrandom failed: errno {}", errno);
      return;
    }
    out += got;
    size -= static_cast<std::size_t>(got);
  }
#endif
}
}  // namespace Common::Random
