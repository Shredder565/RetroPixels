// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"

#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModAsset.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

std::unique_ptr<GraphicsModManager> g_graphics_mod_manager;

namespace
{
// Forwards to the wrapped action only while the mod it came from is enabled. The mod's
// settings are captured when the action is created.
class ModGatedAction final : public GraphicsModAction
{
public:
  ModGatedAction(std::unique_ptr<GraphicsModAction> inner, GraphicsModConfig mod)
      : m_inner(std::move(inner)), m_mod(std::move(mod))
  {
  }

  void OnDrawStarted(GraphicsModActionData::DrawStarted* data) override
  {
    if (m_mod.m_enabled)
      m_inner->OnDrawStarted(data);
  }
  void BeforeEFB(GraphicsModActionData::PreEFB* data) override
  {
    if (m_mod.m_enabled)
      m_inner->BeforeEFB(data);
  }
  void AfterEFB(GraphicsModActionData::PostEFB* data) override
  {
    if (m_mod.m_enabled)
      m_inner->AfterEFB(data);
  }
  void OnProjection(GraphicsModActionData::Projection* data) override
  {
    if (m_mod.m_enabled)
      m_inner->OnProjection(data);
  }
  void OnProjectionAndTexture(GraphicsModActionData::Projection* data) override
  {
    if (m_mod.m_enabled)
      m_inner->OnProjectionAndTexture(data);
  }
  void OnTextureLoad(GraphicsModActionData::TextureLoad* data) override
  {
    if (m_mod.m_enabled)
      m_inner->OnTextureLoad(data);
  }
  void OnTextureCreate(GraphicsModActionData::TextureCreate* data) override
  {
    if (m_mod.m_enabled)
      m_inner->OnTextureCreate(data);
  }
  void OnFrameEnd() override
  {
    if (m_mod.m_enabled)
      m_inner->OnFrameEnd();
  }

private:
  std::unique_ptr<GraphicsModAction> m_inner;
  GraphicsModConfig m_mod;
};

std::string ProjectionTextureKey(const std::string& texture, ProjectionType type)
{
  return fmt::format("{}_{}", texture, static_cast<int>(type));
}

FBInfo DescribeFramebuffer(const FBTarget& target)
{
  FBInfo info;
  info.m_width = target.m_width;
  info.m_height = target.m_height;
  info.m_texture_format = target.m_texture_format;
  return info;
}

template <typename Map, typename Key>
const std::vector<GraphicsModAction*>& ActionsFor(const Map& map, const Key& key,
                                                  const std::vector<GraphicsModAction*>& none)
{
  const auto it = map.find(key);
  return it != map.end() ? it->second : none;
}

// Points the asset's relative file names at the mod's own directory.
VideoCommon::Assets::AssetMap ResolveAssetFiles(const GraphicsModAssetConfig& asset,
                                                const GraphicsModConfig& mod,
                                                const std::string& mod_directory)
{
  VideoCommon::Assets::AssetMap files = asset.m_map;
  for (auto& entry : files)
  {
    std::filesystem::path& path = entry.second;
    if (!path.is_absolute())
    {
      path = std::filesystem::path{mod_directory} / path;
      continue;
    }
    WARN_LOG_FMT(VIDEO,
                 "Specified graphics mod asset '{}' for mod '{}' has an absolute path, you "
                 "shouldn't release this to users.",
                 asset.m_asset_id, mod.m_title);
  }
  return files;
}
}  // namespace

bool GraphicsModManager::Initialize()
{
  if (!g_ActiveConfig.bGraphicMods)
    return true;

  // Keep the change count from an earlier session so that the freshly loaded config is not
  // mistaken for a user change.
  auto& mod_config = g_ActiveConfig.graphics_mod_config;
  const u32 previous_changes = mod_config ? mod_config->GetChangeCount() : 0;
  mod_config = GraphicsModGroupConfig(SConfig::GetInstance().GetGameID());
  mod_config->Load();
  mod_config->SetChangeCount(previous_changes);
  g_graphics_mod_manager->Load(*mod_config);

  m_frame_end_hook =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { OnFrameEnded(); });
  return true;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetProjectionActions(ProjectionType projection_type) const
{
  return ActionsFor(m_on_projection, projection_type, s_no_actions);
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetProjectionTextureActions(ProjectionType projection_type,
                                                const std::string& texture_name) const
{
  return ActionsFor(m_on_projection_with_texture,
                    ProjectionTextureKey(texture_name, projection_type), s_no_actions);
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetDrawStartedActions(const std::string& texture_name) const
{
  return ActionsFor(m_on_draw_started, texture_name, s_no_actions);
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetTextureLoadActions(const std::string& texture_name) const
{
  return ActionsFor(m_on_texture_load, texture_name, s_no_actions);
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetTextureCreateActions(const std::string& texture_name) const
{
  return ActionsFor(m_on_texture_create, texture_name, s_no_actions);
}

const std::vector<GraphicsModAction*>& GraphicsModManager::GetEFBActions(const FBInfo& efb) const
{
  return ActionsFor(m_on_efb, efb, s_no_actions);
}

const std::vector<GraphicsModAction*>& GraphicsModManager::GetXFBActions(const FBInfo& xfb) const
{
  // XFB queries are answered from the EFB table (XFB targets are recorded but never matched).
  return ActionsFor(m_on_efb, xfb, s_no_actions);
}

void GraphicsModManager::Load(const GraphicsModGroupConfig& config)
{
  Clear();

  const std::vector<GraphicsModConfig>& mods = config.GetMods();

  auto library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
  library->Watch(File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR);
  library->Watch(File::GetUserPath(D_GRAPHICSMOD_IDX));

  // Pass 1: collect each group's targets under its plain name (shared between mods) and under
  // "<mod title>.<group name>" (private to the mod), and register the mods' assets.
  std::map<std::string, std::vector<GraphicsTargetConfig>> targets_by_group;
  for (const GraphicsModConfig& mod : mods)
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
    {
      if (!m_group_names.insert(group.m_name).second)
      {
        WARN_LOG_FMT(
            VIDEO,
            "Specified graphics mod group '{}' for mod '{}' is already specified by another mod.",
            group.m_name, mod.m_title);
      }

      const std::string private_name = fmt::format("{}.{}", mod.m_title, group.m_name);
      for (const GraphicsTargetConfig& target : group.m_targets)
      {
        targets_by_group[group.m_name].push_back(target);
        targets_by_group[private_name].push_back(target);
      }
    }

    std::string mod_directory;
    SplitPath(mod.GetAbsolutePath(), &mod_directory, nullptr, nullptr);
    for (const GraphicsModAssetConfig& asset : mod.m_assets)
      library->SetAssetIDMapData(asset.m_asset_id, ResolveAssetFiles(asset, mod, mod_directory));
  }

  const auto register_target = [this](const GraphicsTargetConfig& target,
                                      GraphicsModAction* action) {
    if (const auto* draw = std::get_if<DrawStartedTextureTarget>(&target))
      m_on_draw_started[draw->m_texture_info_string].push_back(action);
    else if (const auto* load = std::get_if<LoadTextureTarget>(&target))
      m_on_texture_load[load->m_texture_info_string].push_back(action);
    else if (const auto* create = std::get_if<CreateTextureTarget>(&target))
      m_on_texture_create[create->m_texture_info_string].push_back(action);
    else if (const auto* efb = std::get_if<EFBTarget>(&target))
      m_on_efb[DescribeFramebuffer(*efb)].push_back(action);
    else if (const auto* xfb = std::get_if<XFBTarget>(&target))
      m_on_xfb[DescribeFramebuffer(*xfb)].push_back(action);
    else if (const auto* projection = std::get_if<ProjectionTarget>(&target))
    {
      if (projection->m_texture_info_string)
      {
        m_on_projection_with_texture[ProjectionTextureKey(*projection->m_texture_info_string,
                                                          projection->m_projection_type)]
            .push_back(action);
      }
      else
      {
        m_on_projection[projection->m_projection_type].push_back(action);
      }
    }
  };

  // Pass 2: one action per feature, attached to every target of the feature's group. A mod's
  // own group wins over a same-named group from another mod.
  for (const GraphicsModConfig& mod : mods)
  {
    for (const GraphicsModFeatureConfig& feature : mod.m_features)
    {
      auto group = targets_by_group.find(fmt::format("{}.{}", mod.m_title, feature.m_group));
      if (group == targets_by_group.end())
        group = targets_by_group.find(feature.m_group);
      if (group == targets_by_group.end())
      {
        WARN_LOG_FMT(VIDEO, "Specified graphics mod group '{}' was not found for mod '{}'",
                     feature.m_group, mod.m_title);
        continue;
      }

      std::unique_ptr<GraphicsModAction> inner =
          GraphicsModActionFactory::Create(feature.m_action, feature.m_action_data, library);
      if (inner == nullptr)
      {
        WARN_LOG_FMT(VIDEO, "Failed to create action '{}' for group '{}'.", feature.m_action,
                     feature.m_group);
        continue;
      }

      GraphicsModAction* const action =
          m_owned.emplace_back(std::make_unique<ModGatedAction>(std::move(inner), mod)).get();
      for (const GraphicsTargetConfig& target : group->second)
        register_target(target, action);
    }
  }
}

void GraphicsModManager::OnFrameEnded()
{
  for (const auto& action : m_owned)
    action->OnFrameEnd();
}

void GraphicsModManager::Clear()
{
  m_owned.clear();
  m_group_names.clear();
  m_on_projection.clear();
  m_on_projection_with_texture.clear();
  m_on_draw_started.clear();
  m_on_texture_load.clear();
  m_on_texture_create.clear();
  m_on_efb.clear();
  m_on_xfb.clear();
}
