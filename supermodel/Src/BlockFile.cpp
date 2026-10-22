// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// See BlockFile.h for the file layout. When writing, the size field of the
// current block is filled in when the block is finished (by the next
// NewBlock(), Close() or the destructor).

#include "BlockFile.h"

#include <cstring>
#include <vector>

namespace
{
  const uint32_t kHeaderWords = 3 * sizeof(uint32_t);

  bool ReadWord(std::FILE *f, uint32_t *out)
  {
    return std::fread(out, sizeof(*out), 1, f) == 1;
  }
}

CBlockFile::CBlockFile(void)
{
}

CBlockFile::~CBlockFile(void)
{
  Close();
}

void CBlockFile::Put(const void *data, size_t size)
{
  if (m_file == nullptr || size == 0)
    return;
  size_t written = std::fwrite(data, 1, size, m_file);
  m_blockSize += uint32_t(written);
}

void CBlockFile::StartBlock(const std::string &name, const std::string &comment)
{
  if (m_file == nullptr)
    return;
  m_blockStart = std::ftell(m_file);
  m_blockSize = 0;
  uint32_t header[3] = { 0, uint32_t(name.size() + 1), uint32_t(comment.size() + 1) };
  Put(header, sizeof(header));
  Put(name.c_str(), name.size() + 1);
  Put(comment.c_str(), comment.size() + 1);
}

void CBlockFile::FinishBlock(void)
{
  if (m_file == nullptr || m_mode != Mode::Writing)
    return;
  long end = std::ftell(m_file);
  std::fseek(m_file, m_blockStart, SEEK_SET);
  std::fwrite(&m_blockSize, sizeof(m_blockSize), 1, m_file);
  std::fseek(m_file, end, SEEK_SET);
}

unsigned CBlockFile::Read(void *data, uint32_t numBytes)
{
  if (m_mode != Mode::Reading || m_file == nullptr)
    return 0;
  return unsigned(std::fread(data, 1, numBytes, m_file));
}

unsigned CBlockFile::Read(bool *value)
{
  uint8_t byte = 0;
  unsigned got = Read(&byte, 1);
  if (got != 0)
    *value = (byte != 0);
  return got;
}

unsigned CBlockFile::Read(bool &value)
{
  return Read(&value);
}

void CBlockFile::Write(const void *data, uint32_t numBytes)
{
  if (m_mode == Mode::Writing)
    Put(data, numBytes);
}

void CBlockFile::Write(bool value)
{
  uint8_t byte = value ? 1 : 0;
  Write(&byte, 1);
}

void CBlockFile::Write(const std::string &str)
{
  if (m_mode == Mode::Writing)
    Put(str.c_str(), str.size() + 1);
}

void CBlockFile::NewBlock(const std::string &name, const std::string &comment)
{
  if (m_mode != Mode::Writing)
    return;
  FinishBlock();
  StartBlock(name, comment);
}

Result CBlockFile::FindBlock(const std::string &name)
{
  if (m_mode != Mode::Reading || m_file == nullptr)
    return Result::FAIL;

  long pos = 0;
  std::vector<char> nameBuf;
  std::fseek(m_file, 0, SEEK_SET);
  while (pos < m_length)
  {
    uint32_t total, nameLen, commentLen;
    if (!ReadWord(m_file, &total) || !ReadWord(m_file, &nameLen) || !ReadWord(m_file, &commentLen))
      break;

    // The stored name ends at its first zero byte (or at nameLen bytes).
    // (A corrupt length can't make us read more than the file holds.)
    long left = m_length - (pos + long(kHeaderWords));
    size_t want = nameLen;
    if (left < 0)
      want = 0;
    else if (want > size_t(left))
      want = size_t(left);
    nameBuf.assign(want, 0);
    size_t got = want ? std::fread(nameBuf.data(), 1, want, m_file) : 0;
    size_t end = 0;
    while (end < got && nameBuf[end] != 0)
      end++;
    if (name.size() == end && std::memcmp(name.data(), nameBuf.data(), end) == 0)
    {
      std::fseek(m_file, pos + long(kHeaderWords) + long(nameLen) + long(commentLen), SEEK_SET);
      m_blockStart = pos;
      return Result::OKAY;
    }

    if (total == 0)   // a zero-sized block would never advance
      break;
    // On to the next block (a failed search leaves the file there).
    pos += long(total);
    std::fseek(m_file, pos, SEEK_SET);
  }
  return Result::FAIL;
}

Result CBlockFile::Create(const std::string &file, const std::string &headerName, const std::string &comment)
{
  Close();
  m_file = std::fopen(file.c_str(), "wb");
  if (m_file == nullptr)
    return Result::FAIL;
  m_mode = Mode::Writing;
  StartBlock(headerName, comment);
  return Result::OKAY;
}

Result CBlockFile::Load(const std::string &file)
{
  Close();
  m_file = std::fopen(file.c_str(), "rb");
  if (m_file == nullptr)
    return Result::FAIL;
  m_mode = Mode::Reading;
  std::fseek(m_file, 0, SEEK_END);
  m_length = std::ftell(m_file);
  std::fseek(m_file, 0, SEEK_SET);
  return Result::OKAY;
}

void CBlockFile::Close(void)
{
  if (m_file != nullptr)
  {
    FinishBlock();
    std::fclose(m_file);
  }
  m_file = nullptr;
  m_mode = Mode::Closed;
}
