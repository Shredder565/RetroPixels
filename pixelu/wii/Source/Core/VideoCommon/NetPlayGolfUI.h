// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

namespace NetPlay
{
class NetPlayClient;
}

// The NetPlay golf-mode window (ImGui): shows who has control and lets the local player take
// it or hand it to another player.
class NetPlayGolfUI
{
public:
  explicit NetPlayGolfUI(std::weak_ptr<NetPlay::NetPlayClient> netplay_client);
  ~NetPlayGolfUI();

  void Display();

private:
  std::weak_ptr<NetPlay::NetPlayClient> m_client;
};

extern std::unique_ptr<NetPlayGolfUI> g_netplay_golf_ui;
