// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/IniFile.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

// INI format as read and written here:
//
//   [Section]            names compare case-insensitively
//   key = value          whitespace around both trimmed, one pair of quotes around value removed
//   # comment            kept as a raw line
//   $Name / +x / *x      raw lines (codes and patches), kept verbatim even if they contain '='
//
// Any other line without a key or value is kept as a raw line of its section. Lines before the
// first section header are dropped. A section is saved with its key/value pairs if it has any,
// otherwise with its raw lines.

namespace Common
{
const std::string& IniFile::NULL_STRING = "";

void IniFile::ParseLine(std::string_view line, std::string* keyOut, std::string* valueOut)
{
  if (line.empty() || line.front() == '#')
    return;

  const std::size_t equals = line.find('=');
  if (equals == std::string_view::npos)
    return;

  *keyOut = StripWhitespace(line.substr(0, equals));
  if (valueOut)
    *valueOut = StripQuotes(StripWhitespace(line.substr(equals + 1)));
}

// ---- Section ---------------------------------------------------------------------------------

IniFile::Section::Section() = default;

IniFile::Section::Section(std::string name_) : name{std::move(name_)}
{
}

void IniFile::Section::Set(const std::string& key, std::string new_value)
{
  // An existing key keeps its original spelling and position.
  if (values.insert_or_assign(key, std::move(new_value)).second)
    keys_order.push_back(key);
}

bool IniFile::Section::Get(std::string_view key, std::string* value,
                           const std::string& default_value) const
{
  if (const auto found = values.find(key); found != values.end())
  {
    *value = found->second;
    return true;
  }
  // A default passed by the caller counts as success; the built-in placeholder does not.
  if (&default_value != &NULL_STRING)
  {
    *value = default_value;
    return true;
  }
  return false;
}

bool IniFile::Section::Exists(std::string_view key) const
{
  return values.contains(key);
}

bool IniFile::Section::Delete(std::string_view key)
{
  const auto found = values.find(key);
  if (found == values.end())
    return false;
  values.erase(found);

  const auto in_order = std::ranges::find_if(
      keys_order, [key](std::string_view k) { return CaseInsensitiveEquals(key, k); });
  if (in_order != keys_order.end())
    keys_order.erase(in_order);
  return true;
}

void IniFile::Section::SetLines(std::vector<std::string> lines)
{
  m_lines = std::move(lines);
}

bool IniFile::Section::GetLines(std::vector<std::string>* lines, const bool remove_comments) const
{
  for (const std::string& raw : m_lines)
  {
    std::string_view line = StripWhitespace(raw);
    if (remove_comments)
    {
      const std::size_t hash = line.find('#');
      if (hash == 0)
        continue;  // a whole-line comment
      if (hash != std::string_view::npos)
        line = StripWhitespace(line.substr(0, hash));
    }
    lines->emplace_back(line);
  }
  return true;
}

// ---- IniFile ---------------------------------------------------------------------------------

IniFile::IniFile() = default;

IniFile::~IniFile() = default;

IniFile::Section* IniFile::GetSection(std::string_view section_name)
{
  const auto found = std::ranges::find_if(sections, [section_name](const Section& s) {
    return CaseInsensitiveEquals(s.name, section_name);
  });
  return found != sections.end() ? &*found : nullptr;
}

const IniFile::Section* IniFile::GetSection(std::string_view section_name) const
{
  return const_cast<IniFile*>(this)->GetSection(section_name);
}

IniFile::Section* IniFile::GetOrCreateSection(std::string_view section_name)
{
  if (Section* existing = GetSection(section_name))
    return existing;
  return &sections.emplace_back(std::string(section_name));
}

bool IniFile::DeleteSection(std::string_view section_name)
{
  const auto found = std::ranges::find_if(sections, [section_name](const Section& s) {
    return CaseInsensitiveEquals(s.name, section_name);
  });
  if (found == sections.end())
    return false;
  sections.erase(found);
  return true;
}

bool IniFile::Exists(std::string_view section_name) const
{
  return GetSection(section_name) != nullptr;
}

bool IniFile::Exists(std::string_view section_name, std::string_view key) const
{
  const Section* section = GetSection(section_name);
  return section && section->Exists(key);
}

void IniFile::SetLines(std::string_view section_name, std::vector<std::string> lines)
{
  GetOrCreateSection(section_name)->SetLines(std::move(lines));
}

bool IniFile::DeleteKey(std::string_view section_name, std::string_view key)
{
  Section* section = GetSection(section_name);
  return section && section->Delete(key);
}

bool IniFile::GetKeys(std::string_view section_name, std::vector<std::string>* keys) const
{
  const Section* section = GetSection(section_name);
  if (!section)
    return false;
  *keys = section->keys_order;
  return true;
}

bool IniFile::GetLines(std::string_view section_name, std::vector<std::string>* lines,
                       const bool remove_comments) const
{
  lines->clear();
  const Section* section = GetSection(section_name);
  return section && section->GetLines(lines, remove_comments);
}

void IniFile::SortSections()
{
  sections.sort();
}

bool IniFile::Load(const std::string& filename, bool keep_current_data)
{
  if (!keep_current_data)
    sections.clear();

  std::ifstream in;
  File::OpenFStream(in, filename, std::ios::in);
  if (in.fail())
    return false;

  Section* section = nullptr;  // nothing is stored until the first header
  std::string buffer;
  for (bool first = true; std::getline(in, buffer); first = false)
  {
    std::string_view line = buffer;

    // Editors such as Notepad may start the file with a UTF-8 byte order mark.
    if (first && line.starts_with("\xEF\xBB\xBF"))
      line.remove_prefix(3);

#ifndef _WIN32
    // Windows text-mode streams already turn CRLF into LF.
    if (line.ends_with('\r'))
      line.remove_suffix(1);
#endif

    if (line.empty())
      continue;

    if (line.front() == '[')
    {
      // A '[' line without a closing ']' is ignored.
      if (const std::size_t close = line.find(']'); close != std::string_view::npos)
        section = GetOrCreateSection(line.substr(1, close - 1));
      continue;
    }

    if (!section)
      continue;

    std::string key, value;
    ParseLine(line, &key, &value);

    const bool raw_marker = line.front() == '$' || line.front() == '+' || line.front() == '*';
    if (raw_marker || (key.empty() && value.empty()))
      section->m_lines.emplace_back(line);
    else
      section->Set(key, std::move(value));
  }

  // getline stops at end of file (success) or on a read error (failure).
  return !in.bad();
}

bool IniFile::Save(const std::string& filename)
{
  // Write a temporary file and rename it over the target, so a crash can't leave half a file.
  const std::string temp_path = File::GetTempFilenameForAtomicWrite(filename);
  std::ofstream out;
  File::OpenFStream(out, temp_path, std::ios::out);
  if (out.fail())
    return false;

  for (const Section& section : sections)
  {
    const bool has_keys = !section.keys_order.empty();
    if (has_keys || !section.m_lines.empty())
      out << '[' << section.name << ']' << std::endl;

    if (has_keys)
    {
      for (const std::string& key : section.keys_order)
      {
        const auto entry = section.values.find(key);
        out << entry->first << " = " << entry->second << std::endl;
      }
    }
    else
    {
      for (const std::string& line : section.m_lines)
        out << line << std::endl;
    }
  }
  out.close();

  return File::RenameSync(temp_path, filename);
}
}  // namespace Common
