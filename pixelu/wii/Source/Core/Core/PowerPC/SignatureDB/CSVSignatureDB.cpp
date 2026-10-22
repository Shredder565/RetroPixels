// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/SignatureDB/CSVSignatureDB.h"

#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace
{
// Line layout: "<checksum hex> <size hex><anything>\t<name>\t<field>\t<field>".
// With two trailing fields they are the object location and object name;
// with one it is the object name.
bool ParseLine(const std::string& line, u32* checksum, HashSignatureDB::DBFunc* func)
{
  std::istringstream in(line);
  u32 size = 0;
  in >> std::hex >> *checksum >> size;
  if (!in)
    return false;

  std::string ignored;
  if (!std::getline(in, ignored, '\t'))
    return false;

  std::string name, second, third;
  if (std::getline(in, name, '\t') && std::getline(in, second, '\t'))
    std::getline(in, third);

  func->name = std::move(name);
  func->size = size;
  if (third.empty())
  {
    func->object_name = std::move(second);
  }
  else
  {
    func->object_location = std::move(second);
    func->object_name = std::move(third);
  }
  return true;
}
}  // namespace

bool CSVSignatureDB::Load(const std::string& file_path)
{
  std::ifstream file;
  File::OpenFStream(file, file_path, std::ios_base::in);
  if (!file)
    return false;

  std::string line;
  for (size_t line_number = 1; std::getline(file, line); ++line_number)
  {
    u32 checksum = 0;
    DBFunc func;
    if (ParseLine(line, &checksum, &func))
      m_database[checksum] = std::move(func);
    else
      WARN_LOG_FMT(SYMBOLS, "CSV database failed to parse line {}", line_number);
  }
  return true;
}

bool CSVSignatureDB::Save(const std::string& file_path) const
{
  File::IOFile file(file_path, "w");
  if (!file)
  {
    ERROR_LOG_FMT(SYMBOLS, "CSV database save failed");
    return false;
  }

  for (const auto& [checksum, func] : m_database)
  {
    file.WriteString(fmt::format("{:08x}\t{:08x}\t{}\t{}\t{}\n", checksum, func.size, func.name,
                                 func.object_location, func.object_name));
  }

  INFO_LOG_FMT(SYMBOLS, "CSV database save successful");
  return true;
}
