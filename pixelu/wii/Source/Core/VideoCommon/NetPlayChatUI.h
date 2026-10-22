// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// The NetPlay chat window drawn over the game (ImGui): a scrolling log and an input line.
class NetPlayChatUI
{
public:
  // `callback` receives each line the user sends.
  explicit NetPlayChatUI(std::function<void(const std::string&)> callback);
  ~NetPlayChatUI();

  // RGB, 0..1.
  using Color = std::array<float, 3>;

  void Display();
  void AppendChat(std::string message, Color color);
  void SendChatMessage();
  // Moves keyboard focus to the input line, or away from it when it already has focus.
  void Activate();

private:
  char m_input[256] = {};
  bool m_pending_scroll = false;
  bool m_focus_requested = false;
  bool m_following_tail = true;

  std::deque<std::pair<std::string, Color>> m_log;
  std::function<void(const std::string&)> m_on_send;
};

extern std::unique_ptr<NetPlayChatUI> g_netplay_chat_ui;
