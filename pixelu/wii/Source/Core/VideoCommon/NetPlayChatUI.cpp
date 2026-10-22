// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/NetPlayChatUI.h"

#include <imgui.h>

std::unique_ptr<NetPlayChatUI> g_netplay_chat_ui;

namespace
{
// Minimum window size, in unscaled pixels.
constexpr float MIN_WIDTH = 220.0f;
constexpr float MIN_HEIGHT = 400.0f;

// Lines kept in the log: once there are more than this, the oldest goes when a line arrives.
constexpr std::size_t LOG_LIMIT = 100;
}  // namespace

NetPlayChatUI::NetPlayChatUI(std::function<void(const std::string&)> callback)
    : m_on_send{std::move(callback)}
{
}

NetPlayChatUI::~NetPlayChatUI() = default;

void NetPlayChatUI::Display()
{
  const float s = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowPos(ImVec2(10.0f * s, 10.0f * s), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(MIN_WIDTH * s, MIN_HEIGHT * s),
                                      ImGui::GetIO().DisplaySize);

  const bool open = ImGui::Begin("Chat", nullptr, ImGuiWindowFlags_None);
  if (open)
  {
    // The log fills the window except for the input row at the bottom.
    ImGui::BeginChild("Scrolling", ImVec2(0, -30 * s), true, ImGuiWindowFlags_None);
    for (const auto& line : m_log)
    {
      const Color& rgb = line.second;
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f), "%s", line.first.c_str());
      ImGui::PopTextWrapPos();
    }

    if (m_pending_scroll)
    {
      ImGui::SetScrollHereY(1.0f);
      m_pending_scroll = false;
    }
    // New lines only scroll the log if the user hasn't scrolled up.
    m_following_tail = ImGui::GetScrollY() == ImGui::GetScrollMaxY();
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::PushItemWidth(-50.0f * s);
    const bool entered = ImGui::InputText("##NetplayMessageBuffer", m_input, IM_ARRAYSIZE(m_input),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    if (entered)
      SendChatMessage();

    if (m_focus_requested)
    {
      // Focus the input field just submitted.
      ImGui::SetKeyboardFocusHere(-1);
      m_focus_requested = false;
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Send"))
      SendChatMessage();
  }
  ImGui::End();
}

void NetPlayChatUI::AppendChat(std::string message, Color color)
{
  if (m_log.size() > LOG_LIMIT)
    m_log.pop_front();
  m_log.emplace_back(std::move(message), color);

  if (m_following_tail)
    m_pending_scroll = true;
}

void NetPlayChatUI::SendChatMessage()
{
  if (m_input[0] == '\0')
    return;

  if (m_on_send)
    m_on_send(m_input);
  m_input[0] = '\0';
}

void NetPlayChatUI::Activate()
{
  if (ImGui::IsItemFocused())
    ImGui::SetWindowFocus(nullptr);
  else
    m_focus_requested = true;
}
