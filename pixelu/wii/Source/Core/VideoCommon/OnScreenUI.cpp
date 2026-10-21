// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/OnScreenUI.h"

#include "Common/CommonPaths.h"
#include "Common/EnumMap.h"
#include "Common/FileUtil.h"
#include "Common/Profiler.h"
#include "Common/Timer.h"

#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/NetPlayChatUI.h"
#include "VideoCommon/NetPlayGolfUI.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <mutex>
#include <span>
#include <string>

#include <imgui.h>
#include <implot.h>

namespace VideoCommon
{
namespace
{
// Flags shared by the small, fixed, non-interactive windows (achievement badges, leaderboards).
constexpr ImGuiWindowFlags OVERLAY_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

// ImGui key for each DolphinKey, in DolphinKey order.
constexpr std::array<ImGuiKey, static_cast<std::size_t>(DolphinKey::Z) + 1> IMGUI_KEY_FOR = {
    ImGuiKey_Tab,       ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow,
    ImGuiKey_DownArrow, ImGuiKey_PageUp,    ImGuiKey_PageDown,   ImGuiKey_Home,
    ImGuiKey_End,       ImGuiKey_Insert,    ImGuiKey_Delete,     ImGuiKey_Backspace,
    ImGuiKey_Space,     ImGuiKey_Enter,     ImGuiKey_Escape,     ImGuiKey_KeypadEnter,
    ImGuiKey_A,         ImGuiKey_C,         ImGuiKey_V,          ImGuiKey_X,
    ImGuiKey_Y,         ImGuiKey_Z,
};

// The layout of ImDrawVert as the backend sees it: float2 position, float2 UV, RGBA8 colour.
PortableVertexDeclaration ImGuiVertexLayout()
{
  PortableVertexDeclaration decl = {};
  decl.position = {ComponentFormat::Float, 2, offsetof(ImDrawVert, pos), true, false};
  decl.texcoords[0] = {ComponentFormat::Float, 2, offsetof(ImDrawVert, uv), true, false};
  decl.colors[0] = {ComponentFormat::UByte, 4, offsetof(ImDrawVert, col), true, false};
  decl.stride = sizeof(ImDrawVert);
  return decl;
}

// The user's OSD_Font.ttf if there is one, else the one shipped in Sys/Resources, else "".
std::string FindOsdFont()
{
  std::string path = File::GetUserPath(D_LOAD_IDX) + "OSD_Font.ttf";
  if (File::Exists(path))
    return path;
  path = File::GetSysDirectory() + DIR_SEP + RESOURCES_DIR + DIR_SEP + "OSD_Font.ttf";
  if (File::Exists(path))
    return path;
  return {};
}

// A 2D-array RGBA8 texture description, as used for every texture the UI creates.
TextureConfig UiTextureConfig(u32 width, u32 height)
{
  return TextureConfig(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                       AbstractTextureType::Texture_2DArray);
}

void LoadRgba(AbstractTexture& texture, u32 width, u32 height, const u8* pixels)
{
  texture.Load(0, width, height, width, pixels, sizeof(u32) * width * height);
}

MathUtil::Rectangle<int> ToFramebufferRect(int left, int top, int right, int bottom)
{
  return g_gfx->ConvertFramebufferRectangle(MathUtil::Rectangle<int>(left, top, right, bottom),
                                            g_gfx->GetCurrentFramebuffer());
}
}  // namespace

bool OnScreenUI::Initialize(u32 width, u32 height, float scale)
{
  std::unique_lock guard(m_lock);

  if (!IMGUI_CHECKVERSION())
  {
    PanicAlertFmt("ImGui version check failed");
    return false;
  }
  if (!ImGui::CreateContext())
  {
    PanicAlertFmt("Creating ImGui context failed");
    return false;
  }
  if (!ImPlot::CreateContext())
  {
    PanicAlertFmt("Creating ImPlot context failed");
    return false;
  }

  // Window positions are not persisted.
  ImGui::GetIO().IniFilename = nullptr;
  SetScale(scale);

  m_vertex_format = g_gfx->CreateNativeVertexFormat(ImGuiVertexLayout());
  if (!m_vertex_format)
  {
    PanicAlertFmt("Failed to create ImGui vertex format");
    return false;
  }

  ImGuiIO& io = ImGui::GetIO();
  m_atlas_textures.clear();

  if (const std::string font = FindOsdFont(); !font.empty())
  {
    io.Fonts->Clear();
    io.Fonts->AddFontFromFileTTF(font.c_str());
  }

  // We create and update the atlas textures ourselves (see UpdateImguiTexture) and honour
  // ImDrawCmd::VtxOffset.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

  if (!RecompileImGuiPipeline())
    return false;

  m_last_frame_us = Common::Timer::NowUs();
  m_ready = true;
  BeginImGuiFrameUnlocked(width, height);
  return true;
}

OnScreenUI::~OnScreenUI()
{
  std::unique_lock guard(m_lock);

  ImGui::EndFrame();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
  m_atlas_textures.clear();
}

bool OnScreenUI::RecompileImGuiPipeline()
{
  const AbstractTextureFormat target_format = g_presenter->GetBackbufferFormat();
  // Without a back buffer (no window) nothing is drawn, and some backends refuse pipelines
  // without a colour target.
  if (target_format == AbstractTextureFormat::Undefined)
    return true;

  // A floating-point back buffer is linear, so the pixel shader converts the sRGB UI colours.
  const bool linear_output = target_format == AbstractTextureFormat::RGBA16F;

  std::unique_ptr<AbstractShader> vs = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, FramebufferShaderGen::GenerateImGuiVertexShader(), nullptr,
      "ImGui vertex shader");
  std::unique_ptr<AbstractShader> ps = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateImGuiPixelShader(linear_output), nullptr,
      "ImGui pixel shader");
  if (!vs || !ps)
  {
    PanicAlertFmt("Failed to compile ImGui shaders");
    return false;
  }

  // With stereo, a pass-through geometry shader puts the UI on both layers.
  std::unique_ptr<AbstractShader> gs;
  if (g_gfx->UseGeometryShaderForUI())
  {
    gs = g_gfx->CreateShaderFromSource(
        ShaderStage::Geometry, FramebufferShaderGen::GeneratePassthroughGeometryShader(1, 1),
        nullptr, "ImGui passthrough geometry shader");
    if (!gs)
    {
      PanicAlertFmt("Failed to compile ImGui geometry shader");
      return false;
    }
  }

  AbstractPipelineConfig config = {};
  config.vertex_format = m_vertex_format.get();
  config.vertex_shader = vs.get();
  config.geometry_shader = gs.get();
  config.pixel_shader = ps.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();

  // Straight alpha blending for colour; the destination alpha is kept.
  BlendingState blend = RenderState::GetNoBlendingBlendState();
  blend.blend_enable = true;
  blend.src_factor = SrcBlendFactor::SrcAlpha;
  blend.dst_factor = DstBlendFactor::InvSrcAlpha;
  blend.src_factor_alpha = SrcBlendFactor::Zero;
  blend.dst_factor_alpha = DstBlendFactor::One;
  config.blending_state = blend;

  config.framebuffer_state.color_texture_format = target_format;
  config.framebuffer_state.depth_texture_format = AbstractTextureFormat::Undefined;
  config.framebuffer_state.samples = 1;
  config.framebuffer_state.per_sample_shading = false;
  config.usage = AbstractPipelineUsage::Utility;

  m_pipeline = g_gfx->CreatePipeline(config);
  if (!m_pipeline)
  {
    PanicAlertFmt("Failed to create imgui pipeline");
    return false;
  }
  return true;
}

void OnScreenUI::BeginImGuiFrame(u32 width, u32 height)
{
  std::unique_lock guard(m_lock);
  BeginImGuiFrameUnlocked(width, height);
}

void OnScreenUI::BeginImGuiFrameUnlocked(u32 width, u32 height)
{
  m_backbuffer_width = width;
  m_backbuffer_height = height;

  const u64 now_us = Common::Timer::NowUs();
  const u64 elapsed_us = now_us - m_last_frame_us;
  m_last_frame_us = now_us;

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
  io.DeltaTime = static_cast<float>(static_cast<double>(elapsed_us) / 1000000.0);

  ImGui::NewFrame();
}

void OnScreenUI::DrawImGui()
{
  ImDrawData* const frame = ImGui::GetDrawData();
  if (frame == nullptr)
    return;

  const float width = static_cast<float>(m_backbuffer_width);
  const float height = static_cast<float>(m_backbuffer_height);
  g_gfx->SetViewport(0.0f, 0.0f, width, height, 0.0f, 1.0f);

  // The vertex shader maps pixel positions to clip space with this scale.
  struct UiConstants
  {
    float pixel_to_clip[2];
    float pad[2];
  };
  UiConstants constants = {};
  constants.pixel_to_clip[0] = 1.0f / width * 2.0f;
  constants.pixel_to_clip[1] = 1.0f / height * 2.0f;

  g_gfx->SetPipeline(m_pipeline.get());
  g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
  g_vertex_manager->UploadUtilityUniforms(&constants, sizeof(constants));

  for (const ImDrawList* list :
       std::span(frame->CmdLists.Data, static_cast<std::size_t>(frame->CmdLists.Size)))
  {
    // An empty list ends the drawing for this frame (the scissor reset below is skipped too).
    if (list->VtxBuffer.empty() || list->IdxBuffer.empty())
      return;

    u32 first_vertex = 0;
    u32 first_index = 0;
    g_vertex_manager->UploadUtilityVertices(
        list->VtxBuffer.Data, sizeof(ImDrawVert), static_cast<u32>(list->VtxBuffer.Size),
        list->IdxBuffer.Data, static_cast<u32>(list->IdxBuffer.Size), &first_vertex, &first_index);

    for (const ImDrawCmd& cmd : list->CmdBuffer)
    {
      if (cmd.UserCallback != nullptr)
      {
        cmd.UserCallback(list, &cmd);
        continue;
      }

      const ImVec4& clip = cmd.ClipRect;
      g_gfx->SetScissorRect(ToFramebufferRect(static_cast<int>(clip.x), static_cast<int>(clip.y),
                                              static_cast<int>(clip.z), static_cast<int>(clip.w)));
      g_gfx->SetTexture(0, reinterpret_cast<const AbstractTexture*>(cmd.GetTexID()));
      g_gfx->DrawIndexed(first_index, cmd.ElemCount, first_vertex);
      first_index += cmd.ElemCount;
    }
  }

  // Leave the scissor covering the whole back buffer: capture tools that copy the back buffer
  // with a scissor-respecting blit (OBS's GL capture, for one) would otherwise only get the area
  // of the last ImGui draw.
  g_gfx->SetScissorRect(ToFramebufferRect(0, 0, static_cast<int>(m_backbuffer_width),
                                          static_cast<int>(m_backbuffer_height)));
}

void OnScreenUI::DrawDebugText()
{
  if (Config::Get(Config::MAIN_MOVIE_SHOW_OSD))
  {
    const float font = ImGui::GetFontSize();
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    // Right-aligned, below the frame rate counters.
    ImGui::SetNextWindowPos(
        ImVec2(display.x - font * m_backbuffer_scale, 80.f * m_backbuffer_scale),
        ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(5.0f * font * m_backbuffer_scale, 2.1f * font * m_backbuffer_scale), display);
    if (ImGui::Begin("Movie", nullptr, ImGuiWindowFlags_NoFocusOnAppearing))
    {
      auto& movie = Core::System::GetInstance().GetMovie();
      if (movie.IsPlayingInput())
      {
        ImGui::Text("Frame: %" PRIu64 " / %" PRIu64, movie.GetCurrentFrame(),
                    movie.GetTotalFrames());
        ImGui::Text("Input: %" PRIu64 " / %" PRIu64, movie.GetCurrentInputCount(),
                    movie.GetTotalInputCount());
      }
      else if (Config::Get(Config::MAIN_SHOW_FRAME_COUNT))
      {
        ImGui::Text("Frame: %" PRIu64, movie.GetCurrentFrame());
        if (movie.IsRecordingInput())
          ImGui::Text("Input: %" PRIu64, movie.GetCurrentInputCount());
      }

      if (Config::Get(Config::MAIN_SHOW_LAG))
        ImGui::Text("Lag: %" PRIu64 "\n", movie.GetCurrentLagCount());
      if (Config::Get(Config::MAIN_MOVIE_SHOW_INPUT_DISPLAY))
        ImGui::TextUnformatted(movie.GetInputDisplay().c_str());
      if (Config::Get(Config::MAIN_MOVIE_SHOW_RTC))
        ImGui::TextUnformatted(movie.GetRTCDisplay().c_str());
      if (Config::Get(Config::MAIN_MOVIE_SHOW_RERECORD))
        ImGui::TextUnformatted(movie.GetRerecords().c_str());
    }
    ImGui::End();
  }

  if (g_ActiveConfig.bOverlayStats)
    g_stats.Display();

  if (Config::Get(Config::GFX_SHOW_NETPLAY_MESSAGES) && g_netplay_chat_ui)
    g_netplay_chat_ui->Display();

  if (Config::Get(Config::NETPLAY_GOLF_MODE_OVERLAY) && g_netplay_golf_ui)
    g_netplay_golf_ui->Display();

  if (g_ActiveConfig.bOverlayProjStats)
    g_stats.DisplayProj();

  if (g_ActiveConfig.bOverlayScissorStats)
    g_stats.DisplayScissor();

  if (const std::string profile = Common::Profiler::ToString(); !profile.empty())
    ImGui::TextUnformatted(profile.c_str());
}

void OnScreenUI::DrawChallengesAndLeaderboards()
{
  if (!Config::Get(Config::MAIN_OSD_MESSAGES))
    return;
#ifdef USE_RETRO_ACHIEVEMENTS
  auto& achievements = AchievementManager::GetInstance();
  std::lock_guard achievements_guard{achievements.GetLock()};

  const bool show_badges = Config::Get(Config::RA_CHALLENGE_INDICATORS_ENABLED);
  const bool changed = achievements.AreChallengesUpdated();
  const auto& active = achievements.GetActiveChallenges();

  if (changed)
    achievements.ResetChallengesUpdated();

  if (!show_badges)
  {
    if (!m_challenge_badges.empty())
      m_challenge_badges.clear();
  }
  else if (changed || m_challenge_badges.size() != active.size())
  {
    // Rebuild every badge texture from the current challenge list.
    m_challenge_badges.clear();
    for (const auto& id : active)
    {
      const auto& badge = achievements.GetAchievementBadge(id, false);
      auto placed = m_challenge_badges.insert_or_assign(
          static_cast<int>(id), g_gfx->CreateTexture(UiTextureConfig(badge.width, badge.height)));
      LoadRgba(*placed.first->second, badge.width, badge.height, badge.data.data());
    }
  }

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  float leaderboard_bottom = display.y;
  if (show_badges && !m_challenge_badges.empty())
  {
    // Badges are sized relative to a 1024-pixel-high window, in the bottom-right corner.
    const float badge_scale = static_cast<float>(display.y / 1024.0);
    ImGui::SetNextWindowSize(ImVec2(0, 0));
    ImGui::SetNextWindowPos(ImVec2(display.x, display.y), 0, ImVec2(1, 1));
    if (ImGui::Begin("Challenges", nullptr, OVERLAY_FLAGS))
    {
      for (const auto& entry : m_challenge_badges)
      {
        AbstractTexture& badge = *entry.second;
        ImGui::Image(badge, ImVec2(static_cast<float>(badge.GetWidth()) * badge_scale,
                                   static_cast<float>(badge.GetHeight()) * badge_scale));
        ImGui::SameLine();
      }
    }
    leaderboard_bottom -= ImGui::GetWindowHeight();
    ImGui::End();
  }

  const auto& leaderboards = achievements.GetActiveLeaderboards();
  if (Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED) && !leaderboards.empty())
  {
    // Stacked above the badges.
    ImGui::SetNextWindowPos(ImVec2(display.x, leaderboard_bottom), 0, ImVec2(1.0, 1.0));
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Leaderboards", nullptr, OVERLAY_FLAGS))
    {
      for (const auto& line : leaderboards)
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::End();
  }
#endif  // USE_RETRO_ACHIEVEMENTS
}

void OnScreenUI::Finalize()
{
  auto guard = GetImGuiLock();

  Core::System::GetInstance().GetPerfMetrics().DrawImGuiStats(m_backbuffer_scale);
  DrawDebugText();
  OSD::DrawMessages();
  DrawChallengesAndLeaderboards();
  ImGui::Render();

  // Pick up a changed OSD font size for the next frame.
  ImGuiStyle& style = ImGui::GetStyle();
  const int font_size = Config::Get(Config::MAIN_OSD_FONT_SIZE);
  if (static_cast<float>(font_size) != style.FontSizeBase)
    style.FontSizeBase = static_cast<float>(font_size);

  // Serve ImGui's texture requests (font atlas creation, partial updates, destruction).
  ImDrawData* const frame = ImGui::GetDrawData();
  if (frame->Textures == nullptr)
    return;
  for (ImTextureData* tex : *frame->Textures)
  {
    if (tex->Status != ImTextureStatus_OK)
      UpdateImguiTexture(tex);
  }
}

void OnScreenUI::UpdateImguiTexture(ImTextureData* tex)
{
  switch (tex->Status)
  {
  case ImTextureStatus_WantCreate:
    CreateAtlasTexture(tex);
    break;
  case ImTextureStatus_WantUpdates:
    PatchAtlasTexture(tex);
    break;
  case ImTextureStatus_WantDestroy:
    // Wait until ImGui has stopped using it for a frame.
    if (tex->UnusedFrames > 0)
      ReleaseAtlasTexture(tex);
    break;
  default:
    break;
  }
}

void OnScreenUI::CreateAtlasTexture(ImTextureData* tex)
{
  IM_ASSERT(tex->TexID == ImTextureID_Invalid);
  IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

  const u32 width = static_cast<u32>(tex->Width);
  const u32 height = static_cast<u32>(tex->Height);
  std::unique_ptr<AbstractTexture> texture =
      g_gfx->CreateTexture(UiTextureConfig(width, height), "ImGui font texture");
  if (!texture)
  {
    PanicAlertFmt("Failed to create ImGui texture");
    return;
  }

  LoadRgba(*texture, width, height, static_cast<const u8*>(tex->Pixels));

  tex->SetTexID(static_cast<ImTextureID>(*texture));
  m_atlas_textures.push_back(std::move(texture));
  tex->SetStatus(ImTextureStatus_OK);
}

void OnScreenUI::PatchAtlasTexture(ImTextureData* tex)
{
  AbstractTexture* const target = reinterpret_cast<AbstractTexture*>(tex->GetTexID());
  if (target == nullptr || tex->TexID == ImTextureID_Invalid)
  {
    PanicAlertFmt("ImGui texture not created before update");
    return;
  }

  for (const ImTextureRect& dirty : tex->Updates)
  {
    const int x = static_cast<int>(dirty.x);
    const int y = static_cast<int>(dirty.y);
    const int w = static_cast<int>(dirty.w);
    const int h = static_cast<int>(dirty.h);

    // Copy the dirty rectangle row by row into an upload texture, then onto the atlas.
    std::unique_ptr<AbstractStagingTexture> upload = g_gfx->CreateStagingTexture(
        StagingTextureType::Upload, UiTextureConfig(static_cast<u32>(w), static_cast<u32>(h)));

    const int row_bytes = w * tex->BytesPerPixel;
    for (int row = 0; row < h; ++row)
    {
      upload->WriteTexels(MathUtil::Rectangle<int>{0, row, w, row + 1},
                          tex->GetPixelsAt(x, y + row), static_cast<u32>(row_bytes));
    }

    upload->CopyToTexture(MathUtil::Rectangle<int>{0, 0, w, h}, target,
                          MathUtil::Rectangle<int>{x, y, x + w, y + h}, 0, 0);
  }

  tex->SetStatus(ImTextureStatus_OK);
}

void OnScreenUI::ReleaseAtlasTexture(ImTextureData* tex)
{
  const AbstractTexture* const target = reinterpret_cast<AbstractTexture*>(tex->GetTexID());
  tex->SetTexID(ImTextureID_Invalid);

  const auto owned = std::ranges::find_if(
      m_atlas_textures, [target](const auto& texture) { return texture.get() == target; });
  m_atlas_textures.erase(owned);

  tex->Status = ImTextureStatus_Destroyed;
}

std::unique_lock<std::mutex> OnScreenUI::GetImGuiLock()
{
  return std::unique_lock<std::mutex>(m_lock);
}

void OnScreenUI::SetScale(float backbuffer_scale)
{
  ImGuiIO& io = ImGui::GetIO();
  io.DisplayFramebufferScale.x = backbuffer_scale;
  io.DisplayFramebufferScale.y = backbuffer_scale;

  // ScaleAllSizes multiplies the current sizes, so start again from the default style each time.
  ImGuiStyle& style = ImGui::GetStyle();
  style = {};
  style.FontScaleMain = backbuffer_scale;
  style.WindowRounding = 7.0f;
  style.ScaleAllSizes(backbuffer_scale);

  m_backbuffer_scale = backbuffer_scale;
}

void OnScreenUI::SetKeyMap(const DolphinKeyMap& key_map)
{
  auto guard = GetImGuiLock();
  if (ImGui::GetCurrentContext() == nullptr)
    return;

  m_host_key_to_imgui.clear();
  for (std::size_t index = 0; index < IMGUI_KEY_FOR.size(); ++index)
  {
    const DolphinKey key = static_cast<DolphinKey>(index);
    const u32 host_code = static_cast<u32>(key_map[key] & 0x1FF);
    m_host_key_to_imgui[host_code] = static_cast<int>(IMGUI_KEY_FOR[index]);
  }
}

void OnScreenUI::SetKey(u32 key, bool is_down, const char* chars)
{
  auto guard = GetImGuiLock();
  ImGuiIO& io = ImGui::GetIO();

  const auto mapped = m_host_key_to_imgui.find(key);
  if (mapped != m_host_key_to_imgui.end())
    io.AddKeyEvent(static_cast<ImGuiKey>(mapped->second), is_down);

  if (chars != nullptr)
    io.AddInputCharactersUTF8(chars);
}

void OnScreenUI::SetMousePos(float x, float y)
{
  auto guard = GetImGuiLock();
  ImGui::GetIO().AddMousePosEvent(x, y);
}

void OnScreenUI::SetMousePress(u32 button_mask)
{
  auto guard = GetImGuiLock();
  ImGuiIO& io = ImGui::GetIO();

  const int button_count = static_cast<int>(std::size(io.MouseDown));
  for (int button = 0; button < button_count; ++button)
    io.AddMouseButtonEvent(button, ((button_mask >> button) & 1u) != 0);
}
}  // namespace VideoCommon
