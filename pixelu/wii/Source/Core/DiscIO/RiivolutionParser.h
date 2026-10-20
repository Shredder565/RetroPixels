// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace DiscIO
{
struct GameModDescriptorRiivolution;
}

// The data model of a Riivolution patch XML (<wiidisc version="1">) and of a Riivolution config
// XML (<riivolution version="2">), which stores the user's option choices.
namespace DiscIO::Riivolution
{
class FileDataLoader;

// The <id> element: which games a patch XML applies to. Unset fields match anything.
struct GameFilter
{
  std::optional<std::string> m_game;       // prefix of the game ID
  std::optional<std::string> m_developer;  // two-character maker code
  std::optional<int> m_disc;
  std::optional<int> m_version;
  std::optional<std::vector<std::string>> m_regions;  // one-character region codes
};

// A <patch id="..."> reference inside a choice, with the parameters it is given.
struct PatchReference
{
  std::string m_id;
  std::map<std::string, std::string> m_params;
};

// One selectable value of an option.
struct Choice
{
  std::string m_name;
  std::vector<PatchReference> m_patch_references;
};

// One user-visible setting.
struct Option
{
  std::string m_name;
  std::string m_id;
  std::vector<Choice> m_choices;

  // 1-based index into m_choices. 0 = disabled.
  u32 m_selected_choice = 0;
};

// A named group of options.
struct Section
{
  std::string m_name;
  std::vector<Option> m_options;
};

// <file>: replaces, extends or creates a file of the disc's file system.
struct File
{
  // Disc path. A leading '/' names an exact path; otherwise the first file with this name is used
  // ("main.dol" means the main executable).
  std::string m_disc;

  // Path of the replacement data on the SD card.
  std::string m_external;

  // true: the disc file ends where the patch data ends. false: the disc file never shrinks.
  bool m_resize = true;

  // Create the disc file if it does not exist (only for exact paths).
  bool m_create = false;

  // Where the patch data starts in the disc file.
  u32 m_offset = 0;

  // Where to start reading the external file.
  u32 m_fileoffset = 0;

  // How many bytes to place. 0 = everything from m_fileoffset to the end of the external file.
  u32 m_length = 0;
};

// <folder>: applies every file of an SD card folder as a <file> patch.
struct Folder
{
  // Disc folder. Empty means each file is matched by name anywhere on the disc.
  std::string m_disc;

  // SD card folder.
  std::string m_external;

  // m_resize, m_create and m_length are given to every file; see File.
  bool m_resize = true;
  bool m_create = false;

  // Also apply the subfolders of the SD card folder.
  bool m_recursive = true;

  u32 m_length = 0;
};

// <savegame>: keeps the game's save data in an SD card folder instead of the NAND.
struct Savegame
{
  // The SD card folder. Created when missing.
  std::string m_external;

  // Copy the NAND save into the folder when the folder is empty or missing.
  bool m_clone = true;
};

// <memory>: changes RAM just before the game's executable starts.
struct Memory
{
  // Target address.
  u32 m_offset = 0;

  // Bytes to write.
  std::vector<u8> m_value;

  // SD card file to take the bytes from instead of m_value.
  std::string m_valuefile;

  // If not empty, the write only happens when RAM currently holds these bytes.
  std::vector<u8> m_original;

  // Ocarina style: find m_value in the loaded code and turn the next blr into a branch to
  // m_offset.
  bool m_ocarina = false;

  // Search MEM1 for m_original (at multiples of m_align) and patch the first hit.
  bool m_search = false;

  u32 m_align = 1;
};

struct Patch
{
  std::string m_id;

  // The patch's 'root' attribute. Paths are resolved through m_file_data_loader.
  std::string m_root;

  std::shared_ptr<FileDataLoader> m_file_data_loader;

  std::vector<File> m_file_patches;
  std::vector<Folder> m_folder_patches;
  std::vector<File> m_sys_file_patches;
  std::vector<Folder> m_sys_folder_patches;
  std::vector<Savegame> m_savegame_patches;
  std::vector<Memory> m_memory_patches;

  ~Patch();
};

struct Disc
{
  // Format version from the XML. Only 1 is accepted.
  int m_version = 0;

  GameFilter m_game_filter;

  std::vector<Section> m_sections;

  // Every <patch> of the XML, selected or not.
  std::vector<Patch> m_patches;

  // Where the XML was loaded from.
  std::string m_xml_path;

  // Whether m_game_filter accepts this game.
  bool IsValidForGame(const std::string& game_id, std::optional<u16> revision,
                      std::optional<u8> disc_number) const;

  // Copies of the patches that the selected choices refer to, with {$name} placeholders replaced.
  std::vector<Patch> GeneratePatches(const std::string& game_id) const;
};

// One remembered choice in a config XML.
struct ConfigOption
{
  // The option's ID, or section name + option name for options without an ID.
  std::string m_id;

  // The chosen value (same numbering as Option::m_selected_choice).
  u32 m_default = 0;
};

struct Config
{
  // Format version. Only 2 is accepted.
  int m_version = 2;
  std::vector<ConfigOption> m_options;
};

std::optional<Disc> ParseFile(const std::string& filename);
std::optional<Disc> ParseString(std::string_view xml, std::string xml_path);
std::vector<Patch> GenerateRiivolutionPatchesFromGameModDescriptor(
    const GameModDescriptorRiivolution& descriptor, const std::string& game_id,
    std::optional<u16> revision, std::optional<u8> disc_number);
std::vector<Patch> GenerateRiivolutionPatchesFromConfig(const std::string& root_directory,
                                                        const std::string& game_id,
                                                        std::optional<u16> revision,
                                                        std::optional<u8> disc_number);
std::optional<Config> ParseConfigFile(const std::string& filename);
std::optional<Config> ParseConfigString(std::string_view xml);
std::string WriteConfigString(const Config& config);
bool WriteConfigFile(const std::string& filename, const Config& config);
void ApplyConfigDefaults(Disc* disc, const Config& config);
}  // namespace DiscIO::Riivolution
