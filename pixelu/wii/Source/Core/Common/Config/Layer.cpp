// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Config/Layer.h"

#include <utility>

namespace Config
{
ConfigLayerLoader::ConfigLayerLoader(LayerType layer) : m_layer(layer)
{
}

ConfigLayerLoader::~ConfigLayerLoader() = default;

LayerType ConfigLayerLoader::GetLayer() const
{
  return m_layer;
}

Layer::Layer(LayerType layer) : m_layer(layer)
{
}

Layer::Layer(std::unique_ptr<ConfigLayerLoader> loader)
    : m_layer(loader->GetLayer()), m_loader(std::move(loader))
{
  Load();
}

Layer::~Layer()
{
  Save();
}

bool Layer::Exists(const Location& location) const
{
  const auto entry = m_map.find(location);
  return entry != m_map.end() && entry->second.has_value();
}

bool Layer::DeleteKey(const Location& location)
{
  // Marked dirty even if nothing was there, so that a save writes the deletion out.
  m_is_dirty = true;
  const auto entry = m_map.find(location);
  if (entry == m_map.end() || !entry->second)
    return false;
  entry->second = std::nullopt;
  return true;
}

void Layer::DeleteAllKeys()
{
  m_is_dirty = true;
  for (auto& entry : m_map)
    entry.second = std::nullopt;
}

// Keys of a section sort between (system, section, "") and the first key of the next possible
// section name, which is the section name followed by '\x01'.
Section Layer::GetSection(System system, const std::string& section)
{
  const auto first = m_map.lower_bound(Location{system, section, std::string()});
  const auto last = m_map.lower_bound(Location{system, section + '\x01', std::string()});
  return Section{first, last};
}

ConstSection Layer::GetSection(System system, const std::string& section) const
{
  const auto first = m_map.lower_bound(Location{system, section, std::string()});
  const auto last = m_map.lower_bound(Location{system, section + '\x01', std::string()});
  return ConstSection{first, last};
}

void Layer::Load()
{
  if (m_loader != nullptr)
    m_loader->Load(this);
  m_is_dirty = false;
}

void Layer::Save()
{
  if (m_loader == nullptr || !m_is_dirty)
    return;
  m_loader->Save(this);
  m_is_dirty = false;
}

LayerType Layer::GetLayer() const
{
  return m_layer;
}

const LayerMap& Layer::GetLayerMap() const
{
  return m_map;
}
}  // namespace Config
