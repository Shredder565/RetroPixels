// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include "UICommon/ResourcePack/ResourcePack.h"

// The user's resource packs, in priority order (first = highest). Order and installed state are
// kept in Packs.ini in the resource pack folder.
namespace ResourcePack
{
bool Init();

// Inserts the pack at `offset` (-1 = lowest priority); null if the pack is invalid.
ResourcePack* Add(const std::string& path, int offset = -1);
bool Remove(ResourcePack& pack);
void SetInstalled(const ResourcePack& pack, bool installed);
bool IsInstalled(const ResourcePack& pack);

std::vector<ResourcePack>& GetPacks();

std::vector<ResourcePack*> GetHigherPriorityPacks(const ResourcePack& pack);
std::vector<ResourcePack*> GetLowerPriorityPacks(const ResourcePack& pack);
}  // namespace ResourcePack
