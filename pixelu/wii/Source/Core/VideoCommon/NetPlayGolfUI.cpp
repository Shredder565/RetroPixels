// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/NetPlayGolfUI.h"

#include <string>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include "Core/NetPlayClient.h"

std::unique_ptr<NetPlayGolfUI> g_netplay_golf_ui;

namespace
{
// Minimum window size, in unscaled pixels. The window sits to the right of the chat window.
constexpr float MIN_WIDTH = 220.0f;
constexpr float MIN_HEIGHT = 45.0f;
}  // namespace

NetPlayGolfUI::NetPlayGolfUI(std::weak_ptr<NetPlay::NetPlayClient> netplay_client)
    : m_client{std::move(netplay_client)}
{
}

NetPlayGolfUI::~NetPlayGolfUI() = default;

void NetPlayGolfUI::Display()
{
  const std::shared_ptr<NetPlay::NetPlayClient> client = m_client.lock();
  if (!client)
    return;

  const float s = ImGui::GetIO().DisplayFramebufferScale.x;
  ImGui::SetNextWindowPos(ImVec2((20.0f + MIN_WIDTH) * s, 10.0f * s), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(MIN_WIDTH * s, MIN_HEIGHT * s),
                                      ImGui::GetIO().DisplaySize);

  // The overlay font has no glyphs for other languages, so these texts are not translated.
  if (ImGui::Begin("Golf Mode", nullptr, ImGuiWindowFlags_None))
  {
    ImGui::Text("Current Golfer: %s", client->GetCurrentGolfer().c_str());

    if (client->LocalPlayerHasControllerMapped())
    {
      if (ImGui::Button("Take Control"))
        client->RequestGolfControl();

      for (const auto* player : client->GetPlayers())
      {
        // Only remote players that have a controller can be given control.
        const bool eligible =
            !client->IsLocalPlayer(player->pid) && client->PlayerHasControllerMapped(player->pid);
        if (!eligible)
          continue;

        const std::string label = fmt::format("Give Control to {}", player->name);
        if (ImGui::Button(label.c_str()))
          client->RequestGolfControl(player->pid);
      }
    }
  }
  ImGui::End();
}
