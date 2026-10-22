// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

namespace Config
{
class ConfigLayerLoader;
}

namespace NetPlay
{
struct NetSettings;
}

namespace ConfigLoaders
{
std::unique_ptr<Config::ConfigLayerLoader>
GenerateNetPlayConfigLoader(const NetPlay::NetSettings& settings);
}
