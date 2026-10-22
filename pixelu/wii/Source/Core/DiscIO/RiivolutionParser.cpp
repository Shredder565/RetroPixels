// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/RiivolutionParser.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <pugixml.hpp>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "DiscIO/GameModDescriptor.h"
#include "DiscIO/RiivolutionPatcher.h"

namespace DiscIO::Riivolution
{
namespace
{
using ParamMap = std::map<std::string, std::string>;
using Substitutions = std::vector<std::pair<std::string, std::string>>;

std::optional<std::string> ReadWholeFile(const std::string& filename)
{
  ::File::IOFile file(filename, "rb");
  if (!file)
    return std::nullopt;
  std::string contents(file.GetSize(), '\0');
  if (!file.ReadBytes(contents.data(), contents.size()))
    return std::nullopt;
  return contents;
}

// The <param name=".." value=".."/> children of a node, on top of the inherited ones.
ParamMap CollectParams(const pugi::xml_node& node, ParamMap params = {})
{
  for (const pugi::xml_node& param : node.children("param"))
    params[param.attribute("name").as_string()] = param.attribute("value").as_string();
  return params;
}

// "0x"-prefixed or plain hex digit pairs. Anything malformed gives an empty vector. (The length
// check comes before the prefix is removed, as Riivolution XMLs expect.)
std::vector<u8> HexToBytes(std::string_view text)
{
  if (text.size() % 2 != 0)
    return {};
  if (text.starts_with("0x") || text.starts_with("0X"))
    text.remove_prefix(2);

  std::vector<u8> bytes(text.size() / 2);
  for (size_t i = 0; i < bytes.size(); ++i)
  {
    if (!TryParse(std::string(text.substr(i * 2, 2)), &bytes[i], 16))
      return {};
  }
  return bytes;
}

GameFilter ReadGameFilter(const pugi::xml_node& id)
{
  GameFilter filter;
  for (const pugi::xml_attribute& attribute : id.attributes())
  {
    const std::string_view name = attribute.name();
    if (name == "game")
      filter.m_game = attribute.as_string();
    else if (name == "developer")
      filter.m_developer = attribute.as_string();
    else if (name == "disc")
      filter.m_disc = attribute.as_int(-1);
    else if (name == "version")
      filter.m_version = attribute.as_int(-1);
  }

  std::vector<std::string> regions;
  for (const pugi::xml_node& region : id.children("region"))
    regions.emplace_back(region.attribute("type").as_string());
  if (!regions.empty())
    filter.m_regions = std::move(regions);
  return filter;
}

Option ReadOption(const pugi::xml_node& node)
{
  Option option;
  option.m_id = node.attribute("id").as_string();
  option.m_name = node.attribute("name").as_string();
  option.m_selected_choice = node.attribute("default").as_uint(0);

  const ParamMap option_params = CollectParams(node);
  for (const pugi::xml_node& choice_node : node.children("choice"))
  {
    Choice& choice = option.m_choices.emplace_back();
    choice.m_name = choice_node.attribute("name").as_string();
    const ParamMap choice_params = CollectParams(choice_node, option_params);
    for (const pugi::xml_node& ref_node : choice_node.children("patch"))
    {
      choice.m_patch_references.push_back(
          {ref_node.attribute("id").as_string(), CollectParams(ref_node, choice_params)});
    }
  }
  return option;
}

std::vector<Section> ReadSections(const pugi::xml_node& options)
{
  std::vector<Section> sections;
  for (const pugi::xml_node& section_node : options.children("section"))
  {
    Section& section = sections.emplace_back();
    section.m_name = section_node.attribute("name").as_string();
    for (const pugi::xml_node& option_node : section_node.children("option"))
      section.m_options.push_back(ReadOption(option_node));
  }
  // <macros> elements are accepted but have no effect: in Dolphin's reader the copied option
  // they produce is never stored, so the result is the same as ignoring them.
  return sections;
}

void ReadFileAttributes(const pugi::xml_node& node, File* file)
{
  file->m_disc = node.attribute("disc").as_string();
  file->m_external = node.attribute("external").as_string();
  file->m_resize = node.attribute("resize").as_bool(true);
  file->m_create = node.attribute("create").as_bool(false);
  file->m_offset = node.attribute("offset").as_uint(0);
  file->m_fileoffset = node.attribute("fileoffset").as_uint(0);
  file->m_length = node.attribute("length").as_uint(0);
}

void ReadFolderAttributes(const pugi::xml_node& node, Folder* folder)
{
  folder->m_disc = node.attribute("disc").as_string();
  folder->m_external = node.attribute("external").as_string();
  folder->m_resize = node.attribute("resize").as_bool(true);
  folder->m_create = node.attribute("create").as_bool(false);
  folder->m_recursive = node.attribute("recursive").as_bool(true);
  folder->m_length = node.attribute("length").as_uint(0);
}

Memory ReadMemoryAttributes(const pugi::xml_node& node)
{
  Memory memory;
  memory.m_offset = node.attribute("offset").as_uint(0);
  memory.m_value = HexToBytes(node.attribute("value").as_string());
  memory.m_valuefile = node.attribute("valuefile").as_string();
  memory.m_original = HexToBytes(node.attribute("original").as_string());
  memory.m_ocarina = node.attribute("ocarina").as_bool(false);
  memory.m_search = node.attribute("search").as_bool(false);
  memory.m_align = node.attribute("align").as_uint(1);
  return memory;
}

Patch ReadPatch(const pugi::xml_node& node, const std::string& default_root)
{
  Patch patch;
  patch.m_id = node.attribute("id").as_string();
  patch.m_root = node.attribute("root").as_string();
  if (patch.m_root.empty())
    patch.m_root = default_root;

  for (const pugi::xml_node& child : node.children())
  {
    const std::string_view kind = child.name();
    if (kind == "file")
      ReadFileAttributes(child, &patch.m_file_patches.emplace_back());
    else if (kind == "dolphin_sys_file")
      ReadFileAttributes(child, &patch.m_sys_file_patches.emplace_back());
    else if (kind == "folder")
      ReadFolderAttributes(child, &patch.m_folder_patches.emplace_back());
    else if (kind == "dolphin_sys_folder")
      ReadFolderAttributes(child, &patch.m_sys_folder_patches.emplace_back());
    else if (kind == "savegame")
      patch.m_savegame_patches.push_back(
          {child.attribute("external").as_string(), child.attribute("clone").as_bool(true)});
    else if (kind == "memory")
      patch.m_memory_patches.push_back(ReadMemoryAttributes(child));
  }
  return patch;
}

// Replaces every placeholder from the table, scanning left to right. At each position the first
// table entry that matches wins; replaced text is not scanned again.
std::string Substitute(std::string_view text, const Substitutions& table)
{
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size())
  {
    const std::string_view rest = text.substr(pos);
    const auto hit = std::ranges::find_if(
        table, [&](const auto& entry) { return rest.starts_with(entry.first); });
    if (hit == table.end())
    {
      out += text[pos];
      ++pos;
    }
    else
    {
      out += hit->second;
      pos += hit->first.size();
    }
  }
  return out;
}

void SubstituteInPatch(Patch* patch, const Substitutions& table)
{
  // Only these fields take placeholders; the dolphin_sys_* entries are used as written.
  patch->m_root = Substitute(patch->m_root, table);
  for (File& file : patch->m_file_patches)
  {
    file.m_disc = Substitute(file.m_disc, table);
    file.m_external = Substitute(file.m_external, table);
  }
  for (Folder& folder : patch->m_folder_patches)
  {
    folder.m_disc = Substitute(folder.m_disc, table);
    folder.m_external = Substitute(folder.m_external, table);
  }
  for (Savegame& savegame : patch->m_savegame_patches)
    savegame.m_external = Substitute(savegame.m_external, table);
  for (Memory& memory : patch->m_memory_patches)
    memory.m_valuefile = Substitute(memory.m_valuefile, table);
}

const GameModDescriptorRiivolutionPatchOption*
FindDescriptorChoice(const std::vector<GameModDescriptorRiivolutionPatchOption>& choices,
                     const Section& section, const Option& option)
{
  for (const GameModDescriptorRiivolutionPatchOption& entry : choices)
  {
    if (entry.section_name != section.m_name)
      continue;
    const bool id_matches = !entry.option_id.empty() && entry.option_id == option.m_id;
    const bool name_matches = !entry.option_name.empty() && entry.option_name == option.m_name;
    if (id_matches || name_matches)
      return &entry;
  }
  return nullptr;
}

// Generates the selected patches of a parsed XML and gives each one a loader for its files.
void AppendGeneratedPatches(const Disc& disc, const std::string& game_id,
                            const std::string& sd_root, std::vector<Patch>* out)
{
  for (Patch& patch : disc.GeneratePatches(game_id))
  {
    patch.m_file_data_loader =
        std::make_shared<FileDataLoaderHostFS>(sd_root, disc.m_xml_path, patch.m_root);
    out->push_back(std::move(patch));
  }
}
}  // namespace

Patch::~Patch() = default;

std::optional<Disc> ParseFile(const std::string& filename)
{
  const std::optional<std::string> xml = ReadWholeFile(filename);
  if (!xml)
    return std::nullopt;
  return ParseString(*xml, filename);
}

std::optional<Disc> ParseString(std::string_view xml, std::string xml_path)
{
  pugi::xml_document document;
  if (!document.load_buffer(xml.data(), xml.size()))
    return std::nullopt;

  const pugi::xml_node root = document.child("wiidisc");
  if (!root)
    return std::nullopt;

  Disc disc;
  disc.m_xml_path = std::move(xml_path);
  disc.m_version = root.attribute("version").as_int(-1);
  if (disc.m_version != 1)
    return std::nullopt;

  if (const pugi::xml_node id = root.child("id"))
    disc.m_game_filter = ReadGameFilter(id);
  if (const pugi::xml_node options = root.child("options"))
    disc.m_sections = ReadSections(options);

  const std::string default_root = root.attribute("root").as_string();
  for (const pugi::xml_node& patch_node : root.children("patch"))
    disc.m_patches.push_back(ReadPatch(patch_node, default_root));

  return disc;
}

bool Disc::IsValidForGame(const std::string& game_id, std::optional<u16> revision,
                          std::optional<u8> disc_number) const
{
  if (game_id.size() != 6)
    return false;

  const std::string_view id = game_id;
  const GameFilter& filter = m_game_filter;
  if (filter.m_game && !id.starts_with(*filter.m_game))
    return false;
  if (filter.m_developer && id.substr(4, 2) != *filter.m_developer)
    return false;
  if (filter.m_disc && (disc_number ? int{*disc_number} : -1) != *filter.m_disc)
    return false;
  if (filter.m_version && (revision ? int{*revision} : -1) != *filter.m_version)
    return false;
  if (filter.m_regions && !filter.m_regions->empty() &&
      std::ranges::find(*filter.m_regions, id.substr(3, 1)) == filter.m_regions->end())
  {
    return false;
  }
  return true;
}

std::vector<Patch> Disc::GeneratePatches(const std::string& game_id) const
{
  const std::string_view id = game_id;
  const std::string base_id(id.substr(0, 3));
  const std::string region(id.substr(3, 1));
  const std::string maker(id.substr(4, 2));

  std::vector<Patch> result;
  for (const Section& section : m_sections)
  {
    for (const Option& option : section.m_options)
    {
      const u32 choice_number = option.m_selected_choice;
      if (choice_number == 0 || choice_number > option.m_choices.size())
        continue;

      for (const PatchReference& reference : option.m_choices[choice_number - 1].m_patch_references)
      {
        const auto source = std::ranges::find(m_patches, reference.m_id, &Patch::m_id);
        if (source == m_patches.end())
          continue;

        // Built-in placeholders take precedence over parameters of the same name.
        Substitutions table{
            {"{$__gameid}", base_id}, {"{$__region}", region}, {"{$__maker}", maker}};
        for (const auto& [name, value] : reference.m_params)
          table.emplace_back(fmt::format("{{${}}}", name), value);

        Patch& patch = result.emplace_back(*source);
        SubstituteInPatch(&patch, table);
      }
    }
  }
  return result;
}

std::vector<Patch> GenerateRiivolutionPatchesFromGameModDescriptor(
    const GameModDescriptorRiivolution& descriptor, const std::string& game_id,
    std::optional<u16> revision, std::optional<u8> disc_number)
{
  std::vector<Patch> result;
  for (const GameModDescriptorRiivolutionPatch& entry : descriptor.patches)
  {
    std::optional<Disc> disc = ParseFile(entry.xml);
    if (!disc || !disc->IsValidForGame(game_id, revision, disc_number))
      continue;

    for (Section& section : disc->m_sections)
    {
      for (Option& option : section.m_options)
      {
        const auto* choice = FindDescriptorChoice(entry.options, section, option);
        if (choice && choice->choice <= option.m_choices.size())
          option.m_selected_choice = choice->choice;
      }
    }
    AppendGeneratedPatches(*disc, game_id, entry.root, &result);
  }
  return result;
}

std::vector<Patch> GenerateRiivolutionPatchesFromConfig(const std::string& root_directory,
                                                        const std::string& game_id,
                                                        std::optional<u16> revision,
                                                        std::optional<u8> disc_number)
{
  // Riivolution names its config files after 4-character game IDs.
  if (game_id.size() != 4 && game_id.size() != 6)
    return {};

  const std::optional<Config> config = ParseConfigFile(
      fmt::format("{}/riivolution/config/{}.xml", root_directory, game_id.substr(0, 4)));

  std::vector<Patch> result;
  for (const std::string& xml_path : Common::DoFileSearch(root_directory + "riivolution", ".xml"))
  {
    std::optional<Disc> disc = ParseFile(xml_path);
    if (!disc || !disc->IsValidForGame(game_id, revision, disc_number))
      continue;
    if (config)
      ApplyConfigDefaults(&*disc, *config);
    AppendGeneratedPatches(*disc, game_id, root_directory, &result);
  }
  return result;
}

std::optional<Config> ParseConfigFile(const std::string& filename)
{
  const std::optional<std::string> xml = ReadWholeFile(filename);
  if (!xml)
    return std::nullopt;
  return ParseConfigString(*xml);
}

std::optional<Config> ParseConfigString(std::string_view xml)
{
  pugi::xml_document document;
  if (!document.load_buffer(xml.data(), xml.size()))
    return std::nullopt;

  const pugi::xml_node root = document.child("riivolution");
  if (!root)
    return std::nullopt;

  Config config;
  config.m_version = root.attribute("version").as_int(-1);
  if (config.m_version != 2)
    return std::nullopt;

  for (const pugi::xml_node& node : root.children("option"))
  {
    config.m_options.push_back(
        {node.attribute("id").as_string(), node.attribute("default").as_uint(0)});
  }
  return config;
}

std::string WriteConfigString(const Config& config)
{
  pugi::xml_document document;
  pugi::xml_node root = document.append_child("riivolution");
  root.append_attribute("version").set_value(config.m_version);
  for (const ConfigOption& option : config.m_options)
  {
    pugi::xml_node node = root.append_child("option");
    node.append_attribute("id").set_value(option.m_id.c_str());
    node.append_attribute("default").set_value(option.m_default);
  }

  std::ostringstream stream;
  document.print(stream, "  ");
  return stream.str();
}

bool WriteConfigFile(const std::string& filename, const Config& config)
{
  const std::string xml = WriteConfigString(config);
  if (xml.empty())
    return false;

  ::File::CreateFullPath(filename);
  ::File::IOFile file(filename, "wb");
  return file && file.WriteString(xml);
}

void ApplyConfigDefaults(Disc* disc, const Config& config)
{
  // An option is identified by its ID, or by section name + option name when it has none.
  const auto find_option = [disc](const std::string& key) -> Option* {
    for (Section& section : disc->m_sections)
    {
      for (Option& option : section.m_options)
      {
        const bool matches =
            option.m_id.empty() ? section.m_name + option.m_name == key : option.m_id == key;
        if (matches)
          return &option;
      }
    }
    return nullptr;
  };

  for (const ConfigOption& remembered : config.m_options)
  {
    if (Option* option = find_option(remembered.m_id))
      option->m_selected_choice = remembered.m_default;
  }
}
}  // namespace DiscIO::Riivolution
