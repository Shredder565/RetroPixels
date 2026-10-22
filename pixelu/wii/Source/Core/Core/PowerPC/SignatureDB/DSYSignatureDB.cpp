// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/SignatureDB/DSYSignatureDB.h"

#include <array>
#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace
{
struct DSYRecord
{
  u32 checksum;
  u32 size;
  std::array<char, 128> name;
};
static_assert(sizeof(DSYRecord) == 136);

void Store(HashSignatureDB::FuncDB& db, const DSYRecord& record)
{
  // The name field is NUL-padded; the last byte is always treated as a NUL.
  const char* const name = record.name.data();
  size_t length = 0;
  while (length < record.name.size() - 1 && name[length] != '\0')
    ++length;

  HashSignatureDB::DBFunc& func = db[record.checksum];
  func = {};
  func.name.assign(name, length);
  func.size = record.size;
}
}  // namespace

bool DSYSignatureDB::Load(const std::string& file_path)
{
  File::IOFile file(file_path, "rb");
  if (!file)
    return false;

  u32 count = 0;
  file.ReadArray(&count, 1);
  for (u32 i = 0; i < count; ++i)
  {
    DSYRecord record{};
    size_t complete = 0;
    file.ReadArray(&record, 1, &complete);
    Store(m_database, record);
    if (complete != 1)
    {
      // Truncated file: every record still owed reads as all zeroes.
      if (i + 1 < count)
        Store(m_database, DSYRecord{});
      break;
    }
  }
  return true;
}

bool DSYSignatureDB::Save(const std::string& file_path) const
{
  File::IOFile file(file_path, "wb");
  if (!file)
  {
    ERROR_LOG_FMT(SYMBOLS, "Database save failed");
    return false;
  }

  const u32 count = static_cast<u32>(m_database.size());
  file.WriteArray(&count, 1);
  for (const auto& [checksum, func] : m_database)
  {
    DSYRecord record{};
    record.checksum = checksum;
    record.size = func.size;
    // At most 127 characters (up to any embedded NUL), always NUL-terminated.
    const char* const name = func.name.c_str();
    for (size_t i = 0; i < record.name.size() - 1 && name[i] != '\0'; ++i)
      record.name[i] = name[i];
    file.WriteArray(&record, 1);
  }

  INFO_LOG_FMT(SYMBOLS, "Database save successful");
  return true;
}
