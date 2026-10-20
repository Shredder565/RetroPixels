// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <list>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/StringUtil.h"

namespace Common
{
// An INI document: an ordered list of sections, each holding key/value pairs (in insertion
// order, keys case-insensitive) and/or raw lines (used for codes and patches).
class IniFile
{
public:
  class Section
  {
    friend class IniFile;

  public:
    Section();
    explicit Section(std::string name_);

    void Set(const std::string& key, std::string new_value);
    // Anything else is stored in its ValueToString form.
    template <typename T>
    void Set(const std::string& key, T&& new_value)
    {
      std::string text = ValueToString(std::forward<T>(new_value));
      Set(key, std::move(text));
    }

    // Stores the value, or removes the key if the value equals the default.
    template <typename T>
    void Set(const std::string& key, T&& new_value, const std::common_type_t<T>& default_value)
    {
      if (new_value != default_value)
        Set(key, std::forward<T>(new_value));
      else
        Delete(key);
    }

    bool Exists(std::string_view key) const;
    bool Delete(std::string_view key);

    // True if the key exists, or if a default_value was passed (which is then returned).
    bool Get(std::string_view key, std::string* value,
             const std::string& default_value = NULL_STRING) const;

    // True if the key exists and parses as T; otherwise *value = default_value.
    template <typename T>
    bool Get(std::string_view key, T* value, const std::common_type_t<T>& default_value = {}) const
    {
      std::string text;
      if (Get(key, &text) && TryParse(text, value))
        return true;
      *value = default_value;
      return false;
    }

    void SetLines(std::vector<std::string> lines);
    // Appends the raw lines, whitespace-trimmed; with remove_comments, '#' comments are cut off
    // and comment-only lines skipped.
    bool GetLines(std::vector<std::string>* lines, const bool remove_comments = true) const;

    using SectionMap = std::map<std::string, std::string, CaseInsensitiveLess>;
    const SectionMap& GetValues() const { return values; }
    bool HasLines() const { return !m_lines.empty(); }
    const std::string& GetName() const { return name; }

    // Sections sort by name.
    bool operator<(const Section& other) const { return name < other.name; }

  protected:
    std::string name;

    std::vector<std::string> keys_order;
    SectionMap values;

    std::vector<std::string> m_lines;
  };

  IniFile();
  ~IniFile();

  // Reads a file. With keep_current_data, the file's sections and keys are merged into (and
  // override) what is already loaded; this layering is only meant for read-only use, as with
  // default + user game INIs.
  bool Load(const std::string& filename, bool keep_current_data = false);

  // Writes the document atomically (temporary file + rename).
  bool Save(const std::string& filename);

  bool Exists(std::string_view section_name) const;
  bool Exists(std::string_view section_name, std::string_view key) const;

  template <typename T>
  bool GetIfExists(std::string_view section_name, std::string_view key, T* value)
  {
    if (Exists(section_name, key))
      return GetOrCreateSection(section_name)->Get(key, value);
    return false;
  }

  template <typename T>
  bool GetIfExists(std::string_view section_name, std::string_view key, T* value, T default_value)
  {
    if (Exists(section_name, key))
      return GetOrCreateSection(section_name)->Get(key, value, default_value);
    *value = default_value;
    return false;
  }

  // The section's keys in insertion order.
  bool GetKeys(std::string_view section_name, std::vector<std::string>* keys) const;

  void SetLines(std::string_view section_name, std::vector<std::string> lines);
  // Replaces *lines with the section's raw lines (see Section::GetLines).
  bool GetLines(std::string_view section_name, std::vector<std::string>* lines,
                bool remove_comments = true) const;

  bool DeleteKey(std::string_view section_name, std::string_view key);
  bool DeleteSection(std::string_view section_name);

  // Orders sections by name (case-sensitive).
  void SortSections();

  Section* GetOrCreateSection(std::string_view section_name);
  const Section* GetSection(std::string_view section_name) const;
  Section* GetSection(std::string_view section_name);

  // Splits "key = value" (trimmed, quotes removed from the value). Leaves the outputs untouched
  // for empty lines, '#' comments and lines without '='. Also used outside IniFile, e.g. for
  // post-processing shader options.
  static void ParseLine(std::string_view line, std::string* keyOut, std::string* valueOut);

  const std::list<Section>& GetSections() const { return sections; }

private:
  std::list<Section> sections;

  static const std::string& NULL_STRING;
};
}  // namespace Common
