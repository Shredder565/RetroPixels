// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/OnScreenUIKeyMap.h"

class NativeVertexFormat;
class AbstractTexture;
class AbstractPipeline;
struct ImTextureData;

namespace VideoCommon
{
// Owns the Dear ImGui / ImPlot contexts and draws their output (statistics, the performance
// overlay, OSD messages, NetPlay chat, achievement indicators) with the active video backend.
class OnScreenUI
{
public:
  OnScreenUI() = default;
  ~OnScreenUI();

  // Creates the contexts, the font atlas setup and the pipeline. Needs the backend to be up,
  // since it creates GPU objects.
  bool Initialize(u32 width, u32 height, float scale);

  // The lock that guards all ImGui state. Code outside the video thread may take it to change
  // ImGui data, but must not draw: drawing belongs to the frame being built.
  std::unique_lock<std::mutex> GetImGuiLock();

  bool IsReady() { return m_ready; }

  // Starts a new ImGui frame for a back buffer of the given size (takes the lock).
  void BeginImGuiFrame(u32 width, u32 height);
  // The same, for callers that already hold the lock.
  void BeginImGuiFrameUnlocked(u32 width, u32 height);

  // Draws the finished ImGui frame into the bound framebuffer. Call with the lock held.
  void DrawImGui();

  // Rebuilds the ImGui pipeline (for example when the stereo mode changes).
  bool RecompileImGuiPipeline();

  void SetScale(float backbuffer_scale);

  // Emits this frame's windows and ends the ImGui frame.
  void Finalize();

  // Keyboard and mouse input forwarded from the UI.
  void SetKeyMap(const DolphinKeyMap& key_map);
  void SetKey(u32 key, bool is_down, const char* chars);
  void SetMousePos(float x, float y);
  void SetMousePress(u32 button_mask);

private:
  void DrawDebugText();
  void DrawChallengesAndLeaderboards();
  void UpdateImguiTexture(ImTextureData* tex);

  void CreateAtlasTexture(ImTextureData* tex);
  void PatchAtlasTexture(ImTextureData* tex);
  void ReleaseAtlasTexture(ImTextureData* tex);

  std::unique_ptr<NativeVertexFormat> m_vertex_format;
  // Textures ImGui asked for (font atlas pages); ImGui refers to them by pointer.
  std::vector<std::unique_ptr<AbstractTexture>> m_atlas_textures;
  std::unique_ptr<AbstractPipeline> m_pipeline;
  // Host key code (low 9 bits) -> ImGuiKey.
  std::map<u32, int> m_host_key_to_imgui;
  std::mutex m_lock;
  u64 m_last_frame_us = 0;

  u32 m_backbuffer_width = 1;
  u32 m_backbuffer_height = 1;
  float m_backbuffer_scale = 1.0;

#ifdef USE_RETRO_ACHIEVEMENTS
  std::map<int, std::unique_ptr<AbstractTexture>, std::less<>> m_challenge_badges;
#endif  // USE_RETRO_ACHIEVEMENTS

  bool m_ready = false;
};
}  // namespace VideoCommon
