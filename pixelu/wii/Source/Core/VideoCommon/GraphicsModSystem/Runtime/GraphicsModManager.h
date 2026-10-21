// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/HookableEvent.h"

#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/XFMemory.h"

class GraphicsModGroupConfig;

// Turns the loaded graphics mods into per-target action lists that the video code queries
// while rendering. Each Get*Actions call returns the actions registered for that target, in
// mod and feature order, or an empty list.
class GraphicsModManager
{
public:
  bool Initialize();

  const std::vector<GraphicsModAction*>& GetProjectionActions(ProjectionType projection_type) const;
  const std::vector<GraphicsModAction*>&
  GetProjectionTextureActions(ProjectionType projection_type,
                              const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>&
  GetDrawStartedActions(const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>&
  GetTextureLoadActions(const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>&
  GetTextureCreateActions(const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>& GetEFBActions(const FBInfo& efb) const;
  const std::vector<GraphicsModAction*>& GetXFBActions(const FBInfo& xfb) const;

  // Replaces all actions with the ones described by `config`.
  void Load(const GraphicsModGroupConfig& config);

private:
  using ActionList = std::vector<GraphicsModAction*>;
  template <typename Key, typename Hash = std::hash<Key>>
  using ActionMap = std::unordered_map<Key, ActionList, Hash>;

  void OnFrameEnded();
  void Clear();

  static inline const ActionList s_no_actions = {};

  // Owns every action; the maps below hold non-owning pointers into it.
  std::list<std::unique_ptr<GraphicsModAction>> m_owned;

  ActionMap<ProjectionType> m_on_projection;
  // Keyed by "<texture name>_<projection type number>".
  ActionMap<std::string> m_on_projection_with_texture;
  ActionMap<std::string> m_on_draw_started;
  ActionMap<std::string> m_on_texture_load;
  ActionMap<std::string> m_on_texture_create;
  ActionMap<FBInfo, FBInfoHasher> m_on_efb;
  ActionMap<FBInfo, FBInfoHasher> m_on_xfb;

  std::unordered_set<std::string> m_group_names;

  Common::EventHook m_frame_end_hook;
};

extern std::unique_ptr<GraphicsModManager> g_graphics_mod_manager;
