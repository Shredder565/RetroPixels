// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/OnScreenDisplay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <list>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Timer.h"

#include "Core/Config/MainSettings.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/TextureConfig.h"

namespace OSD
{
namespace
{
// Layout, in unscaled pixels: distance of the first message from the window's corner, and the
// gap between two messages.
constexpr float CORNER_OFFSET = 10.0f;
constexpr float MESSAGE_GAP = 4.0f;

// A message fades out over the last second of its life.
constexpr float FADE_OUT_MS = 1000.0f;
// A message that never got drawn (no frames were presented) is still thrown away once it is this
// many milliseconds past its end.
constexpr s64 UNSHOWN_GRACE_MS = 5000;

constexpr ImGuiWindowFlags MESSAGE_WINDOW_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

using IconLevel = VideoCommon::CustomTextureData::ArraySlice::Level;

struct Notice
{
  Notice(std::string text_, u32 lifetime_ms_, u32 argb_, const IconLevel* icon_)
      : text(std::move(text_)), lifetime_ms(lifetime_ms_), argb(argb_), icon(icon_)
  {
    age.Start();
  }

  s64 MillisecondsLeft() const
  {
    return static_cast<s64>(lifetime_ms) - static_cast<s64>(age.ElapsedMs());
  }

  std::string text;
  u32 lifetime_ms;
  u32 argb;
  // Not owned. Cleared when no texture could be made from it.
  const IconLevel* icon;
  // Created on the video thread the first time the notice is drawn.
  std::unique_ptr<AbstractTexture> icon_texture;
  Common::Timer age;
  bool shown = false;
  // Set when a newer notice of the same type arrives. Only DrawMessages removes notices, since
  // the icon texture has to be released on the video thread.
  bool superseded = false;
};

constexpr std::size_t TYPE_COUNT = static_cast<std::size_t>(MessageType::Typeless) + 1;

// One queue per message type, drawn in type order, oldest first within a type.
struct Board
{
  std::mutex lock;
  std::array<std::list<Notice>, TYPE_COUNT> queues;
};

Board& GetBoard()
{
  static Board board;
  return board;
}

std::atomic<int> s_covered_left{0};
std::atomic<int> s_covered_top{0};

ImVec4 ToImGuiColor(u32 argb)
{
  const auto channel = [argb](int shift) {
    return static_cast<float>((argb >> shift) & 0xFFu) / 255.0f;
  };
  return ImVec4(channel(16), channel(8), channel(0), channel(24));
}

void UploadIcon(Notice& notice)
{
  const u32 w = notice.icon->width;
  const u32 h = notice.icon->height;
  const TextureConfig config(w, h, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                             AbstractTextureType::Texture_2DArray);
  notice.icon_texture = g_gfx->CreateTexture(config);
  if (!notice.icon_texture)
  {
    // Drop the icon so we don't retry every frame.
    notice.icon = nullptr;
    return;
  }
  notice.icon_texture->Load(0, w, h, w, notice.icon->data.data(), sizeof(u32) * w * h);
}

// Emits one message window at `corner` and returns how far the next one goes down.
float EmitNotice(int slot, Notice& notice, const ImVec2& corner, int ms_left)
{
  // ImGui identifies windows by name, so number them by their position in this frame's list.
  const std::string window_name = fmt::format("osd_{}", slot);

  ImGui::SetNextWindowPos(corner);
  // A zero size makes the window fit this text instead of the size of a previous message.
  ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));

  // Fade over the last part of the message's life, but show it fully the first time.
  const float fade_span =
      std::max(std::min(FADE_OUT_MS, static_cast<float>(notice.lifetime_ms)), 1.0f);
  const float opacity =
      notice.shown ? std::clamp(static_cast<float>(ms_left) / fade_span, 0.0f, 1.0f) : 1.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);

  float advance = 0.0f;
  if (ImGui::Begin(window_name.c_str(), nullptr, MESSAGE_WINDOW_FLAGS))
  {
    if (notice.icon != nullptr)
    {
      if (!notice.icon_texture)
        UploadIcon(notice);

      if (notice.icon_texture)
      {
        ImGui::Image(*notice.icon_texture, ImVec2(static_cast<float>(notice.icon->width),
                                                  static_cast<float>(notice.icon->height)));
        ImGui::SameLine();
      }
    }

    // Through "%s" so a '%' in the text is printed as is.
    if (!notice.text.empty())
      ImGui::TextColored(ToImGuiColor(notice.argb), "%s", notice.text.c_str());

    advance = ImGui::GetWindowSize().y + MESSAGE_GAP * ImGui::GetIO().DisplayFramebufferScale.y;
  }
  ImGui::End();
  ImGui::PopStyleVar();

  notice.shown = true;
  return advance;
}

bool IsFinished(const Notice& notice, s64 ms_left)
{
  if (ms_left > 0)
    return false;
  // A message is drawn at least once (even with a zero duration), unless it is long overdue.
  return notice.shown || -ms_left >= UNSHOWN_GRACE_MS;
}
}  // namespace

void AddTypedMessage(MessageType type, std::string message, u32 ms, u32 argb,
                     const VideoCommon::CustomTextureData::ArraySlice::Level* icon)
{
  Board& board = GetBoard();
  std::lock_guard guard(board.lock);
  auto& queue = board.queues[static_cast<std::size_t>(type)];
  for (Notice& older : queue)
    older.superseded = true;
  queue.emplace_back(std::move(message), ms, argb, icon);
}

void AddMessage(std::string message, u32 ms, u32 argb,
                const VideoCommon::CustomTextureData::ArraySlice::Level* icon)
{
  Board& board = GetBoard();
  std::lock_guard guard(board.lock);
  board.queues[static_cast<std::size_t>(MessageType::Typeless)].emplace_back(std::move(message), ms,
                                                                             argb, icon);
}

void DrawMessages()
{
  const bool visible = Config::Get(Config::MAIN_OSD_MESSAGES);
  const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
  const float left = CORNER_OFFSET * scale.x + static_cast<float>(s_covered_left.load());
  float top = CORNER_OFFSET * scale.y + static_cast<float>(s_covered_top.load());
  int slot = 0;

  Board& board = GetBoard();
  std::lock_guard guard(board.lock);
  for (auto& queue : board.queues)
  {
    auto it = queue.begin();
    while (it != queue.end())
    {
      Notice& notice = *it;
      if (notice.superseded)
      {
        it = queue.erase(it);
        continue;
      }

      const s64 ms_left = notice.MillisecondsLeft();
      if (IsFinished(notice, ms_left))
      {
        it = queue.erase(it);
        continue;
      }
      ++it;

      if (visible)
        top += EmitNotice(slot++, notice, ImVec2(left, top), static_cast<int>(ms_left));
    }
  }
}

void ClearMessages()
{
  Board& board = GetBoard();
  std::lock_guard guard(board.lock);
  // Release newest-first, last type first, so icon textures are freed in the same order as
  // before (callers may log or count backend releases).
  for (auto queue = board.queues.rbegin(); queue != board.queues.rend(); ++queue)
  {
    while (!queue->empty())
      queue->pop_back();
  }
}

void SetObscuredPixelsLeft(int width)
{
  s_covered_left = width;
}

void SetObscuredPixelsTop(int height)
{
  s_covered_top = height;
}
}  // namespace OSD
