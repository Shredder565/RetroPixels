// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/IOFile.h"

#include <cstdint>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <share.h>

#include "Common/StringUtil.h"
#else
#include <unistd.h>
#endif

#include "Common/CommonFuncs.h"
#include "Common/FileUtil.h"

namespace File
{
// Every constructor starts from "no file, no error".
IOFile::IOFile(std::FILE* file) : m_file(file), m_good(true)
{
}

IOFile::IOFile() : IOFile(static_cast<std::FILE*>(nullptr))
{
}

IOFile::IOFile(const std::string& filename, const char openmode[], SharedAccess sh) : IOFile()
{
  Open(filename, openmode, sh);
}

IOFile::IOFile(IOFile&& other) noexcept : IOFile()
{
  Swap(other);
}

IOFile::~IOFile()
{
  Close();
}

void IOFile::Swap(IOFile& other) noexcept
{
  std::swap(m_good, other.m_good);
  std::swap(m_file, other.m_file);
}

// Swapping hands our previous file to `other`, which closes it when it is destroyed.
IOFile& IOFile::operator=(IOFile&& other) noexcept
{
  Swap(other);
  return *this;
}

bool IOFile::Open(const std::string& filename, const char openmode[],
                  [[maybe_unused]] SharedAccess sh)
{
  Close();

#ifdef _WIN32
  const std::wstring wide_name = UTF8ToWString(filename);
  const std::wstring wide_mode = UTF8ToWString(openmode);
  if (sh == SharedAccess::Read)
  {
    m_file = _wfsopen(wide_name.c_str(), wide_mode.c_str(), _SH_DENYWR);
  }
  else
  {
    if (_wfopen_s(&m_file, wide_name.c_str(), wide_mode.c_str()) != 0)
      m_file = nullptr;
  }
#else
  m_file = std::fopen(filename.c_str(), openmode);
#endif

  m_good = m_file != nullptr;
  return m_good;
}

bool IOFile::Close()
{
  // Closing a file that is not open counts as a failure, like every other operation.
  if (!IsOpen() || std::fclose(m_file) != 0)
    m_good = false;
  m_file = nullptr;
  return m_good;
}

void IOFile::SetHandle(std::FILE* file)
{
  Close();
  ClearError();
  m_file = file;
}

u64 IOFile::GetSize() const
{
  return IsOpen() ? File::GetSize(m_file) : 0;
}

bool IOFile::Seek(s64 offset, SeekOrigin origin)
{
  int whence;
  switch (origin)
  {
  case SeekOrigin::Begin:
    whence = SEEK_SET;
    break;
  case SeekOrigin::Current:
    whence = SEEK_CUR;
    break;
  case SeekOrigin::End:
    whence = SEEK_END;
    break;
  default:
    return false;
  }

  if (!IsOpen() || fseeko(m_file, offset, whence) != 0)
    m_good = false;
  return m_good;
}

u64 IOFile::Tell() const
{
  return IsOpen() ? static_cast<u64>(ftello(m_file)) : UINT64_MAX;
}

bool IOFile::Flush()
{
  if (!IsOpen() || std::fflush(m_file) != 0)
    m_good = false;
  return m_good;
}

bool IOFile::Resize(u64 size)
{
  if (!IsOpen())
  {
    m_good = false;
    return false;
  }
#ifdef _WIN32
  const bool ok = _chsize_s(_fileno(m_file), static_cast<__int64>(size)) == 0;
#else
  const bool ok = ftruncate(fileno(m_file), static_cast<off_t>(size)) == 0;
#endif
  if (!ok)
    m_good = false;
  return m_good;
}
}  // namespace File
