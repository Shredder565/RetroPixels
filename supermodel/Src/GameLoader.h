// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Reads Config/Games.xml (each game's identity, hardware and ROM layout)
// and loads a game's ROM regions from a zip file, using the parent set's
// zip next to it for a clone whose own zip holds only its changed files.

#ifndef INCLUDED_GAMELOADER_H
#define INCLUDED_GAMELOADER_H

#include "Util/NewConfig.h"
#include "Game.h"
#include "ROMSet.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class GameLoader
{
public:
  explicit GameLoader(const std::string &xml_file);

  // Finds the game in the zip file and loads its ROMs. True on error.
  bool Load(Game *game, ROMSet *rom_set, const std::string &zipfilename) const;

  const std::map<std::string, Game> &GetGames() const
  {
    return m_games;
  }

private:
  // <file name= offset= crc32=>
  struct RomFile
  {
    std::string name;     // lower case
    uint32_t offset = 0;
    uint32_t crc = 0;
    bool hasCrc = false;

    // Whether a file with this name and CRC is this one (by CRC when we
    // know ours, else by name).
    bool Is(const std::string &otherName, uint32_t otherCrc) const;
  };
  using FileRef = std::shared_ptr<RomFile>;
  using FileSet = std::set<FileRef>;

  // <region name= stride= chunk_size= byte_layout|byte_swap= required=>
  struct Region
  {
    std::string name;
    uint32_t stride = 0;        // bytes between a file's chunks in the region
    uint32_t chunk = 0;         // bytes a file contributes per stride
    std::string layout;         // byte order within a stride, e.g. "1032"
    bool required = true;
    std::vector<FileRef> files;
  };
  using RegionRef = std::shared_ptr<Region>;
  using Regions = std::map<std::string, RegionRef>;
  using Patches = std::map<std::string, std::vector<ROM::BigEndianPatch>>;

  class Archive;  // the opened zip file(s)

  struct Scan     // what one zip holds of each game
  {
    std::set<std::string> complete;
    std::map<std::string, FileSet> missing;
  };

  std::string m_xmlFile;
  std::map<std::string, Game> m_games;
  std::map<std::string, Regions> m_regions;   // as Games.xml defines them
  std::map<std::string, Regions> m_merged;    // clones, with the parent's regions
  std::map<std::string, Patches> m_patches;   // per game (clones don't inherit)

  void ReadDefinitions(const std::string &filename);
  bool ReadGames(const Util::Config::Node &xml);
  bool NeedsAttribute(const Util::Config::Node &node, const char *name) const;
  RegionRef NewRegion(const Util::Config::Node &node) const;
  FileRef NewFile(const Util::Config::Node &node) const;
  void ReadRoms(const std::string &game, const Util::Config::Node &roms);
  bool MergeClones();

  Scan ScanArchive(const Archive &zip, const std::map<std::string, Regions> &sets) const;
  std::string PickGame(bool *needParent, const Archive &zip, const std::string &zipfilename) const;
  bool LoadRegion(ROM *rom, const Region &region, const Archive &zip) const;
  bool LoadRoms(ROMSet *romSet, const std::string &game, const Archive &zip) const;
};

#endif  // INCLUDED_GAMELOADER_H
