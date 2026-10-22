// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TextureUtils.h"

#include <fmt/format.h>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractTexture.h"

namespace
{
// Per-game dump folder; the game id is read on every call since it changes between games.
std::string GameDumpFolder()
{
  return File::GetUserPath(D_DUMPTEXTURES_IDX) + SConfig::GetInstance().GetGameID();
}

void EnsureFolderExists(const std::string& folder)
{
  if (!File::IsDirectory(folder))
    File::CreateDir(folder);
}

// File name without folder or extension.
std::string DumpName(std::string basename, u32 level, bool is_arbitrary)
{
  std::string name = std::move(basename);
  if (is_arbitrary)
    name.append("_arb");
  if (level != 0)
    name.append(fmt::format("_mip{}", level));
  return name;
}

std::string DumpPath(const std::string& folder, const std::string& name)
{
  return fmt::format("{}/{}.png", folder, name);
}

void WritePNG(const AbstractTexture& texture, const std::string& path, u32 level)
{
  texture.Save(path, level, Config::Get(Config::GFX_TEXTURE_PNG_COMPRESSION_LEVEL));
}
}  // namespace

namespace VideoCommon::TextureUtils
{
void DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                 bool is_arbitrary)
{
  const std::string folder = GameDumpFolder();
  EnsureFolderExists(folder);

  const std::string path = DumpPath(folder, DumpName(std::move(basename), level, is_arbitrary));
  if (!File::Exists(path))
    WritePNG(texture, path, level);
}

void TextureDumper::DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                                bool is_arbitrary)
{
  const std::string folder = GameDumpFolder();

  // Nothing recorded yet: learn what earlier sessions already wrote.
  if (m_dumped_textures.empty())
  {
    EnsureFolderExists(folder);
    for (const std::string& existing : Common::DoFileSearch(folder, ".png", true))
    {
      std::string stem;
      SplitPath(existing, nullptr, &stem, nullptr);
      m_dumped_textures.insert(std::move(stem));
    }
    NOTICE_LOG_FMT(VIDEO, "Found {} dumped textures that will not be re-dumped.",
                   m_dumped_textures.size());
  }

  std::string name = DumpName(std::move(basename), level, is_arbitrary);
  const std::string path = DumpPath(folder, name);
  if (m_dumped_textures.insert(std::move(name)).second)
    WritePNG(texture, path, level);
}
}  // namespace VideoCommon::TextureUtils
