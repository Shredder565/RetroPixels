// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Games.xml describes each game as named ROM regions; each region is built
// from files in the game's zip, interleaved: a file gives `chunk_size`
// bytes every `stride` bytes from its `offset`, and the region's bytes may
// then be reordered within each stride (`byte_layout`, or `byte_swap` for
// 16-bit pairs). A clone (a game with a parent) lists only what differs
// from its parent: files at the same offset replace the parent's.
//
// Finding the game in a zip: files are matched by CRC32 (or by name when
// Games.xml gives no CRC). A game is complete when every file of its
// required regions is there. Precedence: a complete clone (counting its
// parent's files) > a clone whose zip has only its own files (the parent's
// zip, next to it, supplies the rest) > a complete non-clone.

#include "GameLoader.h"
#include "OSD/Logger.h"
#include "Util/ConfigBuilders.h"
#include "Util/Format.h"
#include "Pkgs/unzip.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

namespace
{
  template <typename T>
  struct Named
  {
    const char *name;
    T value;
  };

  // Value for a name in a table; T() for a name not in it.
  template <typename T, size_t N>
  T Lookup(const Named<T> (&table)[N], const std::string &name)
  {
    for (const auto &e : table)
    {
      if (name == e.name)
        return e.value;
    }
    return T();
  }

  const Named<Game::AudioTypes> kAudio[] =
  {
    { "",                      Game::STEREO_LR },
    { "Mono",                  Game::MONO },
    { "Stereo",                Game::STEREO_LR },
    { "StereoReversed",        Game::STEREO_RL },
    { "QuadFrontRear",         Game::QUAD_1_FLR_2_RLR },
    { "QuadFrontRearReversed", Game::QUAD_1_FRL_2_RRL },
    { "QuadRearFront",         Game::QUAD_1_RLR_2_FLR },
    { "QuadRearFrontReversed", Game::QUAD_1_RRL_2_FRL },
    { "QuadMix",               Game::QUAD_1_LR_2_FR_MIX },
  };

  const Named<uint32_t> kInputs[] =
  {
    { "common",          Game::INPUT_COMMON },
    { "vehicle",         Game::INPUT_VEHICLE },
    { "joystick1",       Game::INPUT_JOYSTICK1 },
    { "joystick2",       Game::INPUT_JOYSTICK2 },
    { "fighting",        Game::INPUT_FIGHTING },
    { "vr4",             Game::INPUT_VR4 },
    { "viewchange",      Game::INPUT_VIEWCHANGE },
    { "shift4",          Game::INPUT_SHIFT4 },
    { "shiftupdown",     Game::INPUT_SHIFTUPDOWN },
    { "handbrake",       Game::INPUT_HANDBRAKE },
    { "harley",          Game::INPUT_HARLEY },
    { "gun1",            Game::INPUT_GUN1 },
    { "gun2",            Game::INPUT_GUN2 },
    { "analog_joystick", Game::INPUT_ANALOG_JOYSTICK },
    { "twin_joysticks",  Game::INPUT_TWIN_JOYSTICKS },
    { "soccer",          Game::INPUT_SOCCER },
    { "spikeout",        Game::INPUT_SPIKEOUT },
    { "analog_gun1",     Game::INPUT_ANALOG_GUN1 },
    { "analog_gun2",     Game::INPUT_ANALOG_GUN2 },
    { "ski",             Game::INPUT_SKI },
    { "magtruck",        Game::INPUT_MAGTRUCK },
    { "fishing",         Game::INPUT_FISHING },
  };

  const Named<Game::DriveBoardType> kDriveBoards[] =
  {
    { "Wheel",     Game::DRIVE_BOARD_WHEEL },
    { "Joystick",  Game::DRIVE_BOARD_JOYSTICK },
    { "Ski",       Game::DRIVE_BOARD_SKI },
    { "Billboard", Game::DRIVE_BOARD_BILLBOARD },
  };

  Game ReadGameInfo(const Util::Config::Node &node)
  {
    Game g;
    g.name = node["name"].ValueAs<std::string>();
    g.parent = node["parent"].ValueAsDefault<std::string>("");
    g.title = node["identity/title"].ValueAsDefault<std::string>("Unknown");
    g.version = node["identity/version"].ValueAsDefault<std::string>("");
    g.manufacturer = node["identity/manufacturer"].ValueAsDefault<std::string>("Unknown");
    g.year = node["identity/year"].ValueAsDefault<unsigned>(0);
    g.stepping = node["hardware/stepping"].ValueAsDefault<std::string>("");
    g.mpeg_board = node["hardware/mpeg_board"].ValueAsDefault<std::string>("");
    g.audio = Lookup(kAudio, node["hardware/audio"].ValueAsDefault<std::string>(""));
    g.encryption_key = node["hardware/encryption_key"].ValueAsDefault<uint32_t>(0);
    g.netboard_present = node["hardware/netboard"].ValueAsDefault<bool>(false);
    for (const auto &input : node["hardware/inputs"])
    {
      if (input.Key() == "input" && input["type"].Exists())
        g.inputs |= Lookup(kInputs, input["type"].ValueAs<std::string>());
    }
    g.driveboard_type = Lookup(kDriveBoards, node["hardware/drive_board"].ValueAsDefault<std::string>(std::string()));
    return g;
  }

  // The folder part of a path, with its final separator ("" if none).
  std::string FolderOf(const std::string &path)
  {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
  }

  bool IsClone(const Game &g)
  {
    return !g.parent.empty();
  }
}

//
// The opened zip file(s): the game's zip, and its parent's when needed.
// Entries are keyed by CRC32; a later entry with the same CRC replaces an
// earlier one.
//

class GameLoader::Archive
{
public:
  struct Entry
  {
    unzFile zip = nullptr;
    std::string zipName;
    std::string name;
    uint32_t size = 0;
    uint32_t crc = 0;
  };

  ~Archive()
  {
    for (unzFile z : m_zips)
      unzClose(z);
  }

  // Adds a zip file's entries. True on error.
  bool Open(const std::string &path)
  {
    unzFile z = unzOpen(path.c_str());
    if (z == nullptr)
    {
      ErrorLog("Could not open '%s'.", path.c_str());
      return true;
    }
    m_zips.push_back(z);
    m_names.push_back(path);

    int status = unzGoToFirstFile(z);
    for (; status == UNZ_OK; status = unzGoToNextFile(z))
    {
      unz_file_info info;
      char name[256];
      if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
        continue;
      Entry &e = m_byCrc[uint32_t(info.crc)];
      e.zip = z;
      e.zipName = path;
      e.name = name;
      e.size = uint32_t(info.uncompressed_size);
      e.crc = uint32_t(info.crc);
    }
    if (status != UNZ_END_OF_LIST_OF_FILE)
    {
      ErrorLog("Unable to read the contents of '%s' (code 0x%x).", path.c_str(), status);
      return true;
    }
    InfoLog("Opened %s.", path.c_str());
    return false;
  }

  // The entry for a file of Games.xml, or null.
  const Entry *Find(const RomFile &f) const
  {
    if (f.hasCrc)
    {
      auto it = m_byCrc.find(f.crc);
      return (it == m_byCrc.end()) ? nullptr : &it->second;
    }
    for (const auto &kv : m_byCrc)
    {
      if (Util::ToLower(kv.second.name) == f.name)
        return &kv.second;
    }
    return nullptr;
  }

  // Same, logging a missing file.
  const Entry *Need(const RomFile &f) const
  {
    const Entry *e = Find(f);
    if (e == nullptr)
    {
      std::string where = (m_names.size() == 1) ? m_names[0] : Util::Format("', '").Join(m_names).str();
      if (f.hasCrc)
        ErrorLog("'%s' with CRC32 0x%08x not found in '%s'.", f.name.c_str(), f.crc, where.c_str());
      else
        ErrorLog("'%s' not found in '%s'.", f.name.c_str(), where.c_str());
    }
    return e;
  }

  // Decompresses a file. True on error.
  bool Read(std::vector<uint8_t> *data, const RomFile &f) const
  {
    const Entry *e = Need(f);
    if (e == nullptr)
      return true;
    const char *name = e->name.c_str();
    const char *zipName = e->zipName.c_str();
    if (unzLocateFile(e->zip, name, 2) != UNZ_OK)   // 2: case-insensitive
    {
      ErrorLog("Unable to locate '%s' in '%s'. Is zip file corrupt?", name, zipName);
      return true;
    }
    if (unzOpenCurrentFile(e->zip) != UNZ_OK)
    {
      ErrorLog("Unable to read '%s' from '%s'. Is zip file corrupt?", name, zipName);
      return true;
    }
    data->assign(e->size, 0);
    int got = unzReadCurrentFile(e->zip, data->data(), e->size);
    if (got < 0 || uint32_t(got) != e->size)
    {
      ErrorLog("Unable to read '%s' from '%s'. Is zip file corrupt?", name, zipName);
      unzCloseCurrentFile(e->zip);
      return true;
    }
    if (unzCloseCurrentFile(e->zip) == UNZ_CRCERROR)
      ErrorLog("CRC error reading '%s' from '%s'. File may be corrupt.", name, zipName);
    return false;
  }

private:
  std::vector<unzFile> m_zips;
  std::vector<std::string> m_names;
  std::map<uint32_t, Entry> m_byCrc;
};

//
// Games.xml
//

bool GameLoader::RomFile::Is(const std::string &otherName, uint32_t otherCrc) const
{
  return hasCrc ? (otherCrc == crc) : (Util::ToLower(otherName) == name);
}

bool GameLoader::NeedsAttribute(const Util::Config::Node &node, const char *name) const
{
  if (!node[name].Empty())
    return false;
  ErrorLog("%s: <%s> tag is missing required attribute '%s'.", m_xmlFile.c_str(), node.Key().c_str(), name);
  return true;
}

GameLoader::FileRef GameLoader::NewFile(const Util::Config::Node &node) const
{
  // Check every attribute, so each missing one is reported.
  bool missing = NeedsAttribute(node, "name");
  missing |= NeedsAttribute(node, "offset");
  if (missing)
    return nullptr;
  auto f = std::make_shared<RomFile>();
  f->name = Util::ToLower(node["name"].ValueAs<std::string>());
  f->offset = node["offset"].ValueAs<uint32_t>();
  f->hasCrc = node["crc32"].Exists();
  if (f->hasCrc)
    f->crc = node["crc32"].ValueAs<uint32_t>();
  return f;
}

GameLoader::RegionRef GameLoader::NewRegion(const Util::Config::Node &node) const
{
  bool missing = NeedsAttribute(node, "name");
  missing |= NeedsAttribute(node, "stride");
  missing |= NeedsAttribute(node, "chunk_size");
  if (missing)
    return nullptr;

  const std::string name = node["name"].Value<std::string>();
  const char *xml = m_xmlFile.c_str();
  bool swapGiven = node["byte_swap"].Exists();
  if (swapGiven && node["byte_layout"].Exists())
  {
    ErrorLog("%s: '%s' region has both 'byte_swap' and 'byte_layout' attributes. Use one or the other.", xml, name.c_str());
    return nullptr;
  }

  auto r = std::make_shared<Region>();
  r->name = name;
  r->stride = node["stride"].ValueAs<uint32_t>();
  if (r->stride == 0)
  {
    ErrorLog("%s: '%s' stride length must be greater than 0.", xml, name.c_str());
    return nullptr;
  }
  r->chunk = node["chunk_size"].ValueAs<uint32_t>();
  if (r->chunk == 0)
  {
    ErrorLog("%s: '%s' chunk size must be greater than 0.", xml, name.c_str());
    return nullptr;
  }
  r->required = node["required"].ValueAsDefault<bool>(true);

  if (!swapGiven)
    r->layout = node["byte_layout"].ValueAsDefault<std::string>(std::string());
  else if (node["byte_swap"].ValueAs<bool>())
  {
    // A lone 16-bit ROM (stride and chunk 1) is swapped as 2-byte units.
    if (r->stride == 1 && r->chunk == 1)
      r->stride = r->chunk = 2;
    // Swap each pair: "1032..."
    for (uint32_t i = 0; i < r->stride; i++)
      r->layout.push_back(char('0' + (i ^ 1)));
  }
  return r;
}

void GameLoader::ReadRoms(const std::string &game, const Util::Config::Node &roms)
{
  const char *xml = m_xmlFile.c_str();
  Regions &regions = m_regions[game];

  // A region may be given more than once; its files add up.
  for (const auto &node : roms)
  {
    if (node.Key() != "region")
      continue;
    auto known = regions.find(node["name"].Value<std::string>());
    RegionRef r = (known != regions.end()) ? known->second : NewRegion(node);
    if (!r)
      continue;
    for (const auto &fileNode : node)
    {
      if (fileNode.Key() != "file")
        continue;
      if (FileRef f = NewFile(fileNode))
        r->files.push_back(f);
    }
    if (r->files.empty())
      ErrorLog("%s: No files defined in region '%s' of '%s'.", xml, r->name.c_str(), game.c_str());
    else
      regions[r->name] = r;
  }

  Patches &patches = m_patches[game];
  for (const auto &group : roms)
  {
    if (group.Key() != "patches")
      continue;
    for (const auto &p : group)
    {
      if (NeedsAttribute(p, "region") || NeedsAttribute(p, "bits") || NeedsAttribute(p, "offset") || NeedsAttribute(p, "value"))
        continue;
      std::string region = p["region"].ValueAs<std::string>();
      unsigned bits = p["bits"].ValueAs<unsigned>();
      uint32_t offset = p["offset"].ValueAs<uint32_t>();
      uint64_t value = p["value"].ValueAs<uint64_t>();
      if (bits == 8 || bits == 16 || bits == 32 || bits == 64)
        patches[region].push_back(ROM::BigEndianPatch(offset, value, bits));
      else
        ErrorLog("%s: Ignoring ROM patch in '%s' with invalid bit length. Must be 8, 16, 32, or 64!", xml, game.c_str());
    }
  }
}

bool GameLoader::ReadGames(const Util::Config::Node &xml)
{
  for (const auto &top : xml)
  {
    if (top.Key() != "games")
      continue;
    for (const auto &node : top)
    {
      if (node.Key() != "game" || node["name"].Empty())
        continue;
      const std::string name = node["name"].ValueAs<std::string>();
      if (m_regions.count(name))
      {
        ErrorLog("%s: Ignoring redefinition of game '%s'.", m_xmlFile.c_str(), name.c_str());
        continue;
      }
      m_regions[name];
      m_patches[name];
      m_games[name] = ReadGameInfo(node);
      for (const auto &roms : node)
      {
        if (roms.Key() == "roms")
          ReadRoms(name, roms);
      }
      if (m_regions[name].empty())
        ErrorLog("%s: No ROM regions defined for '%s'.", m_xmlFile.c_str(), name.c_str());
    }
  }
  if (m_regions.empty())
  {
    ErrorLog("%s: No games defined.", m_xmlFile.c_str());
    return true;
  }
  return false;
}

bool GameLoader::MergeClones()
{
  bool error = false;
  // (A parent missing from Games.xml is treated as one with no regions.)
  for (auto &kv : m_regions)
  {
    const Game &clone = m_games[kv.first];
    if (!IsClone(clone))
      continue;
    const Regions &own = kv.second;
    const Regions &parents = m_regions[clone.parent];

    Regions merged;
    for (const auto &pr : parents)
    {
      auto r = std::make_shared<Region>(*pr.second);
      merged[pr.first] = r;
      auto mine = own.find(pr.first);
      if (mine == own.end())
        continue;
      const Region &c = *mine->second;
      if (r->stride != c.stride || r->chunk != c.chunk || r->layout != c.layout)
      {
        ErrorLog("%s: Attributes of region '%s' in parent '%s' and child '%s' do not match.", m_xmlFile.c_str(), pr.first.c_str(), clone.parent.c_str(), clone.name.c_str());
        error = true;
      }
      // The clone's file at an offset replaces the parent's.
      for (const FileRef &f : c.files)
      {
        auto same = std::find_if(r->files.begin(), r->files.end(), [&](const FileRef &p) { return p->offset == f->offset; });
        if (same != r->files.end())
          *same = f;
        else
          r->files.push_back(f);
      }
    }
    for (const auto &cr : own)
      merged.insert(cr);    // regions only the clone has
    m_merged[kv.first] = merged;
  }
  return error;
}

void GameLoader::ReadDefinitions(const std::string &filename)
{
  m_xmlFile = filename;
  Util::Config::Node xml("xml");
  if (Util::Config::FromXMLFile(&xml, filename))
  {
    ErrorLog("Game and ROM set definitions could not be loaded! ROMs will not be detected.");
    return;
  }
  if (ReadGames(xml))
    return;

  // Only one level of parents.
  std::set<std::string> nested;
  for (auto &kv : m_games)
  {
    if (IsClone(kv.second) && IsClone(m_games[kv.second.parent]))
      nested.insert(kv.second.parent);
  }
  for (const auto &name : nested)
    ErrorLog("%s: Parent ROM set '%s' also has parent defined, which is not allowed.", m_xmlFile.c_str(), name.c_str());

  MergeClones();
}

GameLoader::GameLoader(const std::string &xml_file)
{
  ReadDefinitions(xml_file);
}

//
// Finding the game in a zip
//

GameLoader::Scan GameLoader::ScanArchive(const Archive &zip, const std::map<std::string, Regions> &sets) const
{
  // Files of required regions only (optional ones don't identify a game).
  std::map<std::string, FileSet> wanted, present;
  for (const auto &game : sets)
  {
    for (const auto &kv : game.second)
    {
      if (!kv.second->required)
        continue;
      for (const FileRef &f : kv.second->files)
      {
        wanted[game.first].insert(f);
        if (zip.Find(*f) != nullptr)
          present[game.first].insert(f);
      }
    }
  }

  // Clones can share files. If all that a game has here is also another
  // game's (which has more here), the files are that other game's.
  std::vector<std::string> notHere;
  for (const auto &a : present)
  {
    for (const auto &b : present)
    {
      if (a.first == b.first || a.second.size() == b.second.size())
        continue;
      size_t shared = 0;
      for (const FileRef &fa : a.second)
      {
        bool match = std::any_of(b.second.begin(), b.second.end(), [&](const FileRef &fb) { return fa->Is(fb->name, fb->crc); });
        shared += match ? 1 : 0;
      }
      if (shared == b.second.size())
        notHere.push_back(b.first);
    }
  }
  for (const auto &name : notHere)
    present.erase(name);

  Scan scan;
  auto byName = [](const FileRef &x, const FileRef &y) { return x->name < y->name; };
  for (const auto &kv : present)
  {
    std::vector<FileRef> need(wanted[kv.first].begin(), wanted[kv.first].end());
    std::vector<FileRef> have(kv.second.begin(), kv.second.end());
    std::sort(need.begin(), need.end(), byName);
    std::sort(have.begin(), have.end(), byName);
    FileSet lacking;
    std::set_difference(need.begin(), need.end(), have.begin(), have.end(), std::inserter(lacking, lacking.end()), byName);
    if (kv.second == wanted[kv.first])
      scan.complete.insert(kv.first);
    if (!lacking.empty())
      scan.missing[kv.first] = lacking;
  }
  return scan;
}

std::string GameLoader::PickGame(bool *needParent, const Archive &zip, const std::string &zipfilename) const
{
  *needParent = false;
  const char *zipName = zipfilename.c_str();
  Scan sets = ScanArchive(zip, m_regions);    // each set on its own
  Scan clones = ScanArchive(zip, m_merged);   // clones with their parents' files

  // A clone found on its own: its zip may hold just the clone's files.
  std::set<std::string> partialClones;
  for (const auto &kv : m_games)
  {
    if (IsClone(kv.second) && (sets.complete.count(kv.first) || sets.missing.count(kv.first)))
    {
      partialClones.insert(kv.first);
      sets.complete.erase(kv.first);
      sets.missing.erase(kv.first);
    }
  }
  for (const auto &name : clones.complete)
  {
    partialClones.erase(name);
    // The whole clone is here, so its parent isn't wanted (and lacks the
    // files the clone replaces).
    const std::string &parent = m_games.find(name)->second.parent;
    sets.complete.erase(parent);
    sets.missing.erase(parent);
  }

  for (const auto &kv : sets.missing)
  {
    for (const FileRef &f : kv.second)
      ErrorLog("'%s' (CRC32 0x%08x) not found in '%s' for game '%s'.", f->name.c_str(), f->crc, zipName, kv.first.c_str());
    ErrorLog("Ignoring game '%s' in '%s' because it is missing files.", kv.first.c_str(), zipName);
  }

  std::string chosen;
  if (!clones.complete.empty())
    chosen = *clones.complete.begin();
  else if (!partialClones.empty())
  {
    chosen = *partialClones.begin();
    *needParent = true;
  }
  else if (!sets.complete.empty())
    chosen = *sets.complete.begin();
  else
  {
    ErrorLog("No complete Model 3 games found in '%s'.", zipName);
    return chosen;
  }

  std::set<std::string> all(sets.complete);
  all.insert(clones.complete.begin(), clones.complete.end());
  all.insert(partialClones.begin(), partialClones.end());
  if (all.size() > 1)
    ErrorLog("Multiple games found in '%s' (%s). Loading '%s'.", zipName, Util::Format(", ").Join(all).str().c_str(), chosen.c_str());
  return chosen;
}

//
// Loading
//

namespace
{
  // Reorders the bytes of each stride: byte i comes from byte layout[i].
  // True on error.
  bool Rearrange(ROM *rom, const std::string &layout, uint32_t stride, const std::string &region)
  {
    if (layout.empty())
      return false;
    const char *name = region.c_str();
    if (layout.size() != stride)
    {
      ErrorLog("Byte layout of '%s' region does not match the stride length (%d bytes but should be %d bytes).", name, int(layout.size()), int(stride));
      return true;
    }
    if (stride > 8)
    {
      ErrorLog("Region '%s' has stride larger than 8 (%d), which is currently unsupported.", name, int(stride));
      return true;
    }
    uint8_t from[8];
    bool used[8] = {};
    for (uint32_t i = 0; i < stride; i++)
    {
      char c = layout[i];
      if (!std::isdigit(static_cast<unsigned char>(c)))
      {
        ErrorLog("Byte layout of '%s' region contains non-numeric characters. Use single-digit byte indices only.", name);
        return true;
      }
      from[i] = uint8_t(c - '0');
    }
    // Each byte 0..stride-1 exactly once.
    for (uint32_t i = 0; i < stride; i++)
    {
      if (from[i] >= stride || used[from[i]])
      {
        ErrorLog("Byte layout of '%s' region must specify all byte offsets exactly once.", name);
        return true;
      }
      used[from[i]] = true;
    }

    uint8_t *p = rom->data.get();
    uint8_t unit[8];
    for (size_t pos = 0; pos + stride <= rom->size; pos += stride)
    {
      std::memcpy(unit, p + pos, stride);
      for (uint32_t i = 0; i < stride; i++)
        p[pos + i] = unit[from[i]];
    }
    return false;
  }
}

bool GameLoader::LoadRegion(ROM *rom, const Region &region, const Archive &zip) const
{
  bool error = false;
  for (const FileRef &f : region.files)
  {
    // (Every file is read, so each problem is reported.)
    std::vector<uint8_t> data;
    error |= zip.Read(&data, *f);
    if (error)
      continue;
    uint8_t *dest = rom->data.get() + f->offset;
    if (region.chunk == region.stride)
      std::memcpy(dest, data.data(), data.size());
    else
    {
      size_t chunks = data.size() / region.chunk;
      for (size_t i = 0; i < chunks; i++)
        std::memcpy(dest + i * region.stride, data.data() + i * region.chunk, region.chunk);
    }
  }
  if (error)
    return true;
  return Rearrange(rom, region.layout, region.stride, region.name);
}

bool GameLoader::LoadRoms(ROMSet *romSet, const std::string &game, const Archive &zip) const
{
  auto info = m_games.find(game);
  if (info == m_games.end())
  {
    ErrorLog("Cannot load unknown game '%s'. Is it defined in '%s'?", game.c_str(), m_xmlFile.c_str());
    return true;
  }
  const Regions &regions = (IsClone(info->second) ? m_merged : m_regions).find(game)->second;

  InfoLog("%s:", game.c_str());
  for (const auto &kv : regions)
  {
    const Region &r = *kv.second;
    InfoLog("  %s: stride=%zu, chunk size=%zu, byte layout=%s", kv.first.c_str(), size_t(r.stride), size_t(r.chunk), r.layout.c_str());
    for (const FileRef &f : r.files)
      InfoLog("    %s, crc32=0x%08x, offset=0x%08x", f->name.c_str(), f->crc, f->offset);
  }

  bool error = false;
  for (const auto &kv : regions)
  {
    const Region &r = *kv.second;

    // Region size: the furthest end of any file's last chunk.
    bool failed = false;
    uint32_t size = 0;
    std::vector<uint32_t> ends;
    for (const FileRef &f : r.files)
    {
      const Archive::Entry *e = zip.Need(*f);
      if (e == nullptr)
      {
        failed = true;
        continue;
      }
      if (e->size % r.chunk != 0)
      {
        ErrorLog("File '%s' in '%s' is not sized in %d-byte chunks.", e->name.c_str(), e->zipName.c_str(), int(r.chunk));
        failed = true;
      }
      uint32_t chunks = e->size / r.chunk;
      ends.push_back(f->offset + r.stride * (chunks - 1) + r.chunk);
    }
    if (!failed)
    {
      size = *std::max_element(ends.begin(), ends.end());
      ROM &rom = romSet->rom_by_region[r.name];
      rom.data.reset(new uint8_t[size], std::default_delete<uint8_t[]>());
      rom.size = size;
      failed = LoadRegion(&rom, r, zip);
    }

    if (failed && !r.required)
    {
      romSet->rom_by_region.erase(r.name);
      ErrorLog("Optional ROM region '%s' in '%s' could not be loaded.", r.name.c_str(), game.c_str());
    }
    else
      error |= failed;
  }

  for (const auto &kv : m_patches.find(game)->second)
  {
    if (!regions.count(kv.first))
      ErrorLog("%s: Ignoring ROM patch for undefined region '%s' in '%s'.", m_xmlFile.c_str(), kv.first.c_str(), game.c_str());
    else if (romSet->rom_by_region.count(kv.first))
      romSet->rom_by_region[kv.first].patches = kv.second;
  }
  return error;
}

bool GameLoader::Load(Game *game, ROMSet *rom_set, const std::string &zipfilename) const
{
  *game = Game();
  Archive zip;
  if (zip.Open(zipfilename))
    return true;

  bool needParent = false;
  std::string chosen = PickGame(&needParent, zip, zipfilename);
  if (chosen.empty())
    return true;
  *game = m_games.find(chosen)->second;

  if (needParent)
  {
    std::string parentZip = FolderOf(zipfilename) + game->parent + ".zip";
    if (zip.Open(parentZip))
    {
      ErrorLog("Expected to find parent ROM set of '%s' at '%s'.", game->name.c_str(), parentZip.c_str());
      return true;
    }
  }

  bool error = LoadRoms(rom_set, game->name, zip);
  if (error)
    *game = Game();
  return error;
}
