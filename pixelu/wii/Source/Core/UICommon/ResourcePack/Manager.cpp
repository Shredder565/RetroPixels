// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/ResourcePack/Manager.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"

namespace ResourcePack
{
namespace
{
std::vector<ResourcePack> s_packs;
std::string s_ini_path;

// Packs.ini sections: [Order] maps manifest ID to priority index, [Installed] lists installed
// packs.
constexpr char ORDER_SECTION[] = "Order";
constexpr char INSTALLED_SECTION[] = "Installed";

// Loads Packs.ini (missing is fine) and remembers its path for the matching save.
Common::IniFile LoadPackIni()
{
  s_ini_path = File::GetUserPath(D_RESOURCEPACK_IDX) + "/Packs.ini";
  Common::IniFile ini;
  ini.Load(s_ini_path);
  return ini;
}

const std::string& IdOf(const ResourcePack& pack)
{
  return pack.GetManifest()->GetID();
}

// Position of `pack` in the list, or the list size if it is not there.
std::size_t IndexOf(const ResourcePack& pack)
{
  return static_cast<std::size_t>(std::ranges::find(s_packs, pack) - s_packs.begin());
}
}  // namespace

bool Init()
{
  s_packs.clear();
  const std::vector<std::string> archives =
      Common::DoFileSearch(File::GetUserPath(D_RESOURCEPACK_IDX), ".zip");

  Common::IniFile ini = LoadPackIni();
  Common::IniFile::Section* order = ini.GetOrCreateSection(ORDER_SECTION);

  // Packs are added sorted by manifest ID (an unreadable pack sorts by its path).
  struct Candidate
  {
    std::size_t archive;
    std::string sort_key;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(archives.size());
  for (std::size_t i = 0; i < archives.size(); ++i)
  {
    const ResourcePack probe(archives[i]);
    candidates.push_back({i, probe.IsValid() ? IdOf(probe) : archives[i]});
  }
  std::ranges::sort(candidates, {}, &Candidate::sort_key);

  bool all_added = true;
  for (std::size_t rank = 0; rank < candidates.size(); ++rank)
  {
    const ResourcePack* added = Add(archives[candidates[rank].archive]);
    if (added == nullptr)
    {
      all_added = false;
      continue;
    }
    order->Set(IdOf(*added), static_cast<u64>(rank));
  }

  // Note that this writes the copy loaded above, replacing what the Add calls saved.
  ini.Save(s_ini_path);
  return all_added;
}

std::vector<ResourcePack>& GetPacks()
{
  return s_packs;
}

std::vector<ResourcePack*> GetHigherPriorityPacks(const ResourcePack& pack)
{
  std::vector<ResourcePack*> result;
  const std::size_t stop = IndexOf(pack);
  for (std::size_t i = 0; i < stop; ++i)
  {
    if (IsInstalled(s_packs[i]))
      result.push_back(&s_packs[i]);
  }
  return result;
}

std::vector<ResourcePack*> GetLowerPriorityPacks(const ResourcePack& pack)
{
  // Long-standing behaviour kept: whether the packs are listed depends on `pack` itself being
  // installed, not on each lower pack. A pack that is not in the list has no lower packs.
  std::vector<ResourcePack*> result;
  const std::size_t start = IndexOf(pack) + 1;
  if (start >= s_packs.size() || !IsInstalled(pack))
    return result;

  for (std::size_t i = start; i < s_packs.size(); ++i)
    result.push_back(&s_packs[i]);
  return result;
}

ResourcePack* Add(const std::string& path, int offset)
{
  if (offset == -1)
    offset = static_cast<int>(s_packs.size());

  ResourcePack pack(path);
  if (!pack.IsValid())
    return nullptr;

  // Record the new pack's slot and shift everything after it down by one.
  Common::IniFile ini = LoadPackIni();
  Common::IniFile::Section* order = ini.GetOrCreateSection(ORDER_SECTION);
  order->Set(IdOf(pack), offset);
  for (int i = offset; i < static_cast<int>(s_packs.size()); ++i)
    order->Set(IdOf(s_packs[static_cast<std::size_t>(i)]), i + 1);
  ini.Save(s_ini_path);

  return &*s_packs.insert(s_packs.begin() + offset, std::move(pack));
}

bool Remove(ResourcePack& pack)
{
  if (!pack.Uninstall(File::GetUserPath(D_USER_IDX)))
    return false;

  const std::size_t index = IndexOf(pack);
  if (index == s_packs.size())
    return false;

  // Drop the pack's slot and shift everything after it up by one.
  Common::IniFile ini = LoadPackIni();
  Common::IniFile::Section* order = ini.GetOrCreateSection(ORDER_SECTION);
  order->Delete(IdOf(pack));
  for (std::size_t i = index + 1; i < s_packs.size(); ++i)
    order->Set(IdOf(s_packs[i]), static_cast<int>(i) - 1);
  ini.Save(s_ini_path);

  s_packs.erase(s_packs.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

void SetInstalled(const ResourcePack& pack, bool installed)
{
  Common::IniFile ini = LoadPackIni();
  Common::IniFile::Section* section = ini.GetOrCreateSection(INSTALLED_SECTION);
  if (installed)
    section->Set(IdOf(pack), true);
  else
    section->Delete(IdOf(pack));
  ini.Save(s_ini_path);
}

bool IsInstalled(const ResourcePack& pack)
{
  Common::IniFile ini = LoadPackIni();
  bool installed = false;
  ini.GetOrCreateSection(INSTALLED_SECTION)->Get(IdOf(pack), &installed, false);
  return installed;
}

}  // namespace ResourcePack
