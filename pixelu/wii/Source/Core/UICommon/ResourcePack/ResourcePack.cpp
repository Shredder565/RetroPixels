// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/ResourcePack/ResourcePack.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <mz.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/CommonPaths.h"
#include "Common/Contains.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/MinizipUtil.h"
#include "Common/StringUtil.h"

#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/Manifest.h"

namespace ResourcePack
{
namespace
{
// Archive folder holding the textures, and where they go under the user folder.
constexpr std::string_view ARCHIVE_TEXTURE_DIR = "textures/";
constexpr char INSTALLED_TEXTURE_DIR[] = HIRES_TEXTURES_DIR DIR_SEP;

// Owns a minizip reader handle.
class ZipReader
{
public:
  ZipReader() : m_handle(mz_zip_reader_create()) {}
  ~ZipReader()
  {
    if (m_handle != nullptr)
      mz_zip_reader_delete(&m_handle);
  }
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool Created() const { return m_handle != nullptr; }
  void* Get() const { return m_handle; }

  // Information about the current entry, or null.
  mz_zip_file* CurrentEntry() const
  {
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(m_handle, &info) != MZ_OK)
      return nullptr;
    return info;
  }

private:
  void* m_handle;
};

// Whether any of `packs` lists `texture` (optionally only counting installed packs).
bool AnyPackHas(const std::vector<ResourcePack*>& packs, const std::string& texture,
                bool installed_only)
{
  return std::ranges::any_of(packs, [&](const ResourcePack* pack) {
    if (installed_only && !IsInstalled(*pack))
      return false;
    return Common::Contains(pack->GetTextures(), texture);
  });
}
}  // namespace

bool ResourcePack::Invalidate(std::string reason)
{
  m_valid = false;
  m_error = std::move(reason);
  return false;
}

ResourcePack::ResourcePack(const std::string& path) : m_path(path)
{
  ZipReader zip;
  if (!zip.Created())
  {
    Invalidate("Failed to create zip reader");
    return;
  }
  if (mz_zip_reader_open_file(zip.Get(), path.c_str()) != MZ_OK)
  {
    Invalidate("Failed to open resource pack");
    return;
  }

  // The manifest is required and must parse.
  if (mz_zip_reader_locate_entry(zip.Get(), "manifest.json", 0) != MZ_OK)
  {
    Invalidate("Resource pack is missing a manifest.");
    return;
  }
  const mz_zip_file* manifest_entry = zip.CurrentEntry();
  if (manifest_entry == nullptr)
  {
    Invalidate("Failed to access manifest.json");
    return;
  }
  std::string manifest_text(static_cast<std::size_t>(manifest_entry->uncompressed_size), '\0');
  if (!Common::ReadFileFromZip(zip.Get(), &manifest_text))
  {
    Invalidate("Failed to read manifest.json");
    return;
  }

  m_manifest = std::make_shared<Manifest>(manifest_text);
  if (!m_manifest->IsValid())
  {
    Invalidate("Manifest error: " + m_manifest->GetError());
    return;
  }

  // The logo is optional, but must be readable if present.
  if (mz_zip_reader_locate_entry(zip.Get(), "logo.png", 0) == MZ_OK)
  {
    const mz_zip_file* logo_entry = zip.CurrentEntry();
    if (logo_entry != nullptr)
      m_logo_data.resize(static_cast<std::size_t>(logo_entry->uncompressed_size));
    if (logo_entry == nullptr || !Common::ReadFileFromZip(zip.Get(), &m_logo_data))
    {
      Invalidate("Failed to read logo.png");
      return;
    }
  }

  // List the non-empty files under textures/. Unless the manifest says the pack is compressed,
  // every texture must be stored uncompressed.
  mz_zip_reader_goto_first_entry(zip.Get());
  do
  {
    const mz_zip_file* entry = zip.CurrentEntry();
    if (entry == nullptr)
      continue;

    const std::string name = entry->filename;
    if (!name.starts_with(ARCHIVE_TEXTURE_DIR) || entry->uncompressed_size == 0)
      continue;

    if (entry->compression_method != 0 && !m_manifest->IsCompressed())
    {
      Invalidate("Texture " + name + " is compressed!");
      return;
    }
    m_textures.push_back(name.substr(ARCHIVE_TEXTURE_DIR.size()));
  } while (mz_zip_reader_goto_next_entry(zip.Get()) != MZ_END_OF_LIST);
}

bool ResourcePack::IsValid() const
{
  return m_valid;
}

const std::vector<char>& ResourcePack::GetLogo() const
{
  return m_logo_data;
}

const std::string& ResourcePack::GetPath() const
{
  return m_path;
}

const std::string& ResourcePack::GetError() const
{
  return m_error;
}

const Manifest* ResourcePack::GetManifest() const
{
  return m_manifest.get();
}

const std::vector<std::string>& ResourcePack::GetTextures() const
{
  return m_textures;
}

bool ResourcePack::Install(const std::string& path)
{
  if (!IsValid())
  {
    m_error = "Invalid pack";
    return false;
  }

  ZipReader zip;
  if (!zip.Created())
    return Invalidate("Failed to create zip reader");
  if (mz_zip_reader_open_file(zip.Get(), m_path.c_str()) != MZ_OK)
    return Invalidate("Failed to open resource pack");
  if (mz_zip_reader_goto_first_entry(zip.Get()) != MZ_OK)
    return false;

  const std::string texture_root = path + INSTALLED_TEXTURE_DIR;

  do
  {
    const mz_zip_file* entry = zip.CurrentEntry();
    if (entry == nullptr)
      return false;

    const std::string entry_name = entry->filename;
    if (!entry_name.starts_with(ARCHIVE_TEXTURE_DIR))
      continue;

    // Match the entry against the pack's texture list (wildcards allowed, case ignored), and
    // install it under the listed name.
    const std::string relative = entry_name.substr(ARCHIVE_TEXTURE_DIR.size());
    const auto listed = std::ranges::find_if(m_textures, [&relative](const std::string& texture) {
      return mz_path_compare_wc(texture.c_str(), relative.c_str(), 1) == MZ_OK;
    });
    if (listed == m_textures.end())
      continue;
    const std::string texture = *listed;

    // A higher-priority pack's copy wins.
    if (AnyPackHas(GetHigherPriorityPacks(*this), texture, false))
      continue;

    const std::string target = texture_root + texture;
    std::string target_dir;
    if (!SplitPath(target, &target_dir, nullptr, nullptr))
      continue;
    if (!File::CreateFullPath(target_dir))
    {
      m_error = "Failed to create full path " + target_dir;
      return false;
    }

    const auto size = static_cast<std::size_t>(entry->uncompressed_size);
    const auto contents = std::make_unique<u8[]>(size);
    if (!Common::ReadFileFromZip(zip.Get(), contents.get(), size))
    {
      m_error = "Failed to read texture " + texture;
      return false;
    }

    File::IOFile output(target, "wb");
    if (!output)
    {
      m_error = "Failed to open " + texture;
      return false;
    }
    if (!output.WriteBytes(contents.get(), size))
    {
      m_error = "Failed to write " + texture;
      return false;
    }
  } while (mz_zip_reader_goto_next_entry(zip.Get()) == MZ_OK);

  SetInstalled(*this, true);
  return true;
}

bool ResourcePack::Uninstall(const std::string& path)
{
  if (!IsValid())
  {
    m_error = "Invalid pack";
    return false;
  }

  // Taken while this pack still counts as installed.
  const std::vector<ResourcePack*> lower = GetLowerPriorityPacks(*this);
  SetInstalled(*this, false);

  const std::string texture_root = path + INSTALLED_TEXTURE_DIR;

  for (const std::string& texture : m_textures)
  {
    // An installed higher-priority pack's copy stays.
    if (AnyPackHas(GetHigherPriorityPacks(*this), texture, true))
      continue;

    // Otherwise the first installed lower-priority pack with this texture takes over.
    const auto successor = std::ranges::find_if(lower, [&texture](ResourcePack* pack) {
      return IsInstalled(*pack) && Common::Contains(pack->GetTextures(), texture);
    });
    if (successor != lower.end())
    {
      (*successor)->Install(path);
      continue;
    }

    const std::string target = texture_root + texture;
    if (File::Exists(target) && !File::Delete(target))
    {
      m_error = "Failed to delete texture " + texture;
      return false;
    }

    // Remove the folders above the texture that are now empty, up to the texture root. Each step
    // trims the trailing separator and one more character before taking the parent folder.
    std::string folder;
    SplitPath(target, &folder, nullptr, nullptr);
    while (folder.size() > texture_root.size())
    {
      if (Common::DoFileSearch(folder).empty())
        File::DeleteDir(folder);
      SplitPath(folder.substr(0, folder.size() - 2), &folder, nullptr, nullptr);
    }
  }

  return true;
}

bool ResourcePack::operator==(const ResourcePack& pack) const
{
  return m_path == pack.GetPath();
}

}  // namespace ResourcePack
