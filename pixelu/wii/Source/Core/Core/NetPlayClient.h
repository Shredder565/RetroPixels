// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <SFML/Network/Packet.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/SPSCQueue.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "InputCommon/GCPadStatus.h"

class BootSessionData;

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace UICommon
{
class GameFile;
}

namespace WiimoteEmu
{
struct SerializedWiimoteState;
}

namespace NetPlay
{
// What the NetPlay window does for the client. Most calls come from the network thread.
class NetPlayUI
{
public:
  virtual ~NetPlayUI() {}
  virtual void BootGame(const std::string& filename,
                        std::unique_ptr<BootSessionData> boot_session_data) = 0;
  virtual void StopGame() = 0;
  virtual bool IsHosting() const = 0;

  virtual void Update() = 0;
  virtual void AppendChat(const std::string& msg) = 0;

  virtual void OnMsgChangeGame(const SyncIdentifier& sync_identifier,
                               const std::string& netplay_name) = 0;
  virtual void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) = 0;
  virtual void OnMsgStartGame() = 0;
  virtual void OnMsgStopGame() = 0;
  virtual void OnMsgPowerButton() = 0;
  virtual void OnPlayerConnect(const std::string& player) = 0;
  virtual void OnPlayerDisconnect(const std::string& player) = 0;
  virtual void OnPadBufferChanged(u32 buffer) = 0;
  virtual void OnHostInputAuthorityChanged(bool enabled) = 0;
  virtual void OnDesync(u32 frame, const std::string& player) = 0;
  virtual void OnConnectionLost() = 0;
  virtual void OnConnectionError(const std::string& message) = 0;
  virtual void OnTraversalError(Common::TraversalClient::FailureReason error) = 0;
  virtual void OnTraversalStateChanged(Common::TraversalClient::State state) = 0;
  virtual void OnGameStartAborted() = 0;
  virtual void OnGolferChanged(bool is_golfer, const std::string& golfer_name) = 0;
  virtual void OnTtlDetermined(u8 ttl) = 0;

  virtual bool IsRecording() = 0;
  virtual std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const SyncIdentifier& sync_identifier,
               SyncIdentifierComparison* found = nullptr) = 0;
  virtual std::string FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                                     int device_number) = 0;
  virtual void ShowGameDigestDialog(const std::string& title) = 0;
  virtual void SetGameDigestProgress(int pid, int progress) = 0;
  virtual void SetGameDigestResult(int pid, const std::string& result) = 0;
  virtual void AbortGameDigest() = 0;

  virtual void OnIndexAdded(bool success, std::string error) = 0;
  virtual void OnIndexRefreshFailed(std::string error) = 0;

  virtual void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                         std::span<const int> players) = 0;
  virtual void HideChunkedProgressDialog() = 0;
  virtual void SetChunkedProgress(int pid, u64 progress) = 0;

  virtual void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) = 0;
};

// Another participant as the client sees them.
class Player
{
public:
  PlayerId pid{};
  std::string name;
  std::string revision;
  u32 ping = 0;
  SyncIdentifierComparison game_status = SyncIdentifierComparison::Unknown;

  bool IsHost() const { return pid == 1; }
};

// One player's connection to a NetPlay host. The constructor connects (directly or through the
// traversal server) and starts the network thread; everything the host sends is handled there.
class NetPlayClient : public Common::TraversalClientClient
{
public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, u8 channel_id = DEFAULT_CHANNEL);

  NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog, std::string name,
                const NetTraversalConfig& traversal_config);
  ~NetPlayClient() override;

  std::vector<const Player*> GetPlayers();
  const NetSettings& GetNetSettings() const;

  // GUI thread.
  bool IsConnected() const { return m_is_connected; }
  bool StartGame(const std::string& path);
  void InvokeStop();
  bool StopGame();
  void Stop();
  bool ChangeGame(const std::string& game);
  void SendChatMessage(const std::string& msg);
  void RequestStopGame();
  void SendPowerButtonEvent();
  void RequestGolfControl(PlayerId pid);
  void RequestGolfControl();
  std::string GetCurrentGolfer();

  // Input exchange, from the CPU thread.
  struct WiimoteDataBatchEntry
  {
    int wiimote;
    WiimoteEmu::SerializedWiimoteState* state;
  };
  bool WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries);
  bool GetNetPads(int pad_nb, bool from_vi, GCPadStatus* pad_status);

  u64 GetInitialRTCValue() const;

  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress addr) override;
  void OnConnectFailed(Common::TraversalConnectFailedReason reason) override;
  void OnTtlDetermined(u8) override {}

  bool IsFirstInGamePad(int ingame_pad) const;
  int NumLocalPads() const;
  int NumLocalWiimotes() const;

  int InGamePadToLocalPad(int ingame_pad) const;
  int LocalPadToInGamePad(int local_pad) const;
  int InGameWiimoteToLocalWiimote(int ingame_wiimote) const;
  int LocalWiimoteToInGameWiimote(int local_wiimote) const;

  bool PlayerHasControllerMapped(PlayerId pid) const;
  bool LocalPlayerHasControllerMapped() const;
  bool IsLocalPlayer(PlayerId pid) const;
  const PlayerId& GetLocalPlayerId() const;

  static void SendTimeBase();
  bool DoAllPlayersHaveGame();

  const PadMappingArray& GetPadMapping() const;
  const GBAConfigArray& GetGBAConfig() const;
  const PadMappingArray& GetWiimoteMapping() const;

  void AdjustPadBufferSize(unsigned int size);

  void SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs, std::vector<u64> titles,
                      std::string redirect_folder);

  static SyncIdentifier GetSDCardIdentifier();

private:
  enum class LinkState
  {
    AwaitingTraversalServer,
    AwaitingHostAddress,
    Connecting,
    AwaitingWelcome,
    Connected,
    Failed
  };

  struct QueuedPacket
  {
    sf::Packet packet;
    u8 channel_id = 0;
  };

  using Handler = void (NetPlayClient::*)(sf::Packet&);
  // The member that handles a message from the host, or nullptr for an unknown one.
  static Handler LookupHandler(MessageID id);

  // Connection and transport.
  bool Connect();
  void Disconnect();
  void Send(const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL);
  void DrainOutgoingQueue();
  void ClearBuffers();

  // Messages from the host.
  void OnData(sf::Packet& packet);
  void HandlePlayerJoin(sf::Packet& packet);
  void HandlePlayerLeave(sf::Packet& packet);
  void HandleChat(sf::Packet& packet);
  void HandleChunkStart(sf::Packet& packet);
  void HandleChunkEnd(sf::Packet& packet);
  void HandleChunkPayload(sf::Packet& packet);
  void HandleChunkAbort(sf::Packet& packet);
  void HandlePadMapping(sf::Packet& packet);
  void HandleWiimoteMapping(sf::Packet& packet);
  void HandleGBAConfig(sf::Packet& packet);
  void HandlePadData(sf::Packet& packet);
  void HandlePadHostData(sf::Packet& packet);
  void HandleWiimoteData(sf::Packet& packet);
  void HandlePadBuffer(sf::Packet& packet);
  void HandleHostInputAuthority(sf::Packet& packet);
  void HandleGolfSwitch(sf::Packet& packet);
  void HandleGolfPrepare(sf::Packet& packet);
  void HandleChangeGame(sf::Packet& packet);
  void HandleGameStatus(sf::Packet& packet);
  void HandleStartGame(sf::Packet& packet);
  void HandleStopGame(sf::Packet& packet);
  void HandlePowerButton(sf::Packet& packet);
  void HandlePing(sf::Packet& packet);
  void HandlePlayerPing(sf::Packet& packet);
  void HandleDesync(sf::Packet& packet);
  void HandleSaveSync(sf::Packet& packet);
  void HandleCodeSync(sf::Packet& packet);
  void HandleDigestRequest(sf::Packet& packet);
  void HandleDigestProgress(sf::Packet& packet);
  void HandleDigestResult(sf::Packet& packet);
  void HandleDigestAbort(sf::Packet& packet);

  // Save data sync.
  void ReceiveSaveCount(sf::Packet& packet);
  void ReceiveRawMemcard(sf::Packet& packet);
  void ReceiveGCIFolder(sf::Packet& packet);
  void ReceiveWiiSaves(sf::Packet& packet);
  void ReceiveGBASave(sf::Packet& packet);
  void ReportSaveSync(bool success);

  // Cheat code sync.
  void ReceiveGeckoCount(sf::Packet& packet);
  void ReceiveGeckoCodes(sf::Packet& packet);
  void ReceiveARCount(sf::Packet& packet);
  void ReceiveARCodes(sf::Packet& packet);
  void ReportCodeSync(bool success);

  // Input.
  bool PollLocalPad(int local_pad, sf::Packet& packet);
  void SendPadHostPoll(PadIndex pad_num);
  bool AddLocalWiimoteToBuffer(int local_wiimote, const WiimoteEmu::SerializedWiimoteState& state,
                               sf::Packet& packet);
  void AddPadStateToPacket(int in_game_pad, const GCPadStatus& pad, sf::Packet& packet);
  void AddWiimoteStateToPacket(int in_game_pad, const WiimoteEmu::SerializedWiimoteState& state,
                               sf::Packet& packet);
  void ReadPadEntries(sf::Packet& packet, bool from_host);

  void SendStartGamePacket();
  void SendStopGamePacket();
  void SendGameStatus();
  void ComputeGameDigest(const SyncIdentifier& sync_identifier);
  void DisplayPlayersPing();
  u32 GetPlayersMaxPing() const;

  // Lock order: game, then players, then the outgoing queue.
  std::recursive_mutex m_game_mutex;
  std::recursive_mutex m_players_mutex;
  std::recursive_mutex m_outgoing_mutex;

  Common::SPSCQueue<QueuedPacket> m_outgoing;

  std::array<Common::SPSCQueue<GCPadStatus>, 4> m_pad_buffer;
  std::array<Common::SPSCQueue<WiimoteEmu::SerializedWiimoteState>, 4> m_wiimote_buffer;

  // Host input authority: the newest state received for each pad, and whether one has arrived.
  std::array<GCPadStatus, 4> m_last_pad_status{};
  std::array<bool, 4> m_first_pad_status_received{};

  std::chrono::steady_clock::time_point m_buffer_under_target_last;

  NetPlayUI* m_dialog = nullptr;

  ENetHost* m_client = nullptr;
  ENetPeer* m_server = nullptr;
  std::thread m_thread;

  SyncIdentifier m_selected_game;
  std::atomic<bool> m_is_running{false};
  std::atomic<bool> m_do_loop{true};

  // Without host input authority: how many pad states each client keeps in flight to the others.
  // With it: how many queued states make this client speed up to drain them.
  unsigned int m_target_buffer_size = 20;
  bool m_host_input_authority = false;
  PlayerId m_current_golfer = 1;

  // Golf handover: GetNetPads parks while m_wait_on_input is set (m_wait_on_input_event wakes it)
  // and acknowledges the request once when m_wait_on_input_received is set.
  bool m_wait_on_input = false;
  bool m_wait_on_input_received = false;

  Player* m_local_player = nullptr;
  u32 m_current_game = 0;
  bool m_is_connected = false;
  LinkState m_link_state = LinkState::Failed;

  PlayerId m_pid = 0;
  NetSettings m_net_settings{};
  std::map<PlayerId, Player> m_players;
  std::string m_host_spec;
  std::string m_player_name;
  bool m_connecting = false;
  Common::TraversalClient* m_traversal_client = nullptr;

  std::thread m_game_digest_thread;
  bool m_should_compute_game_digest = false;

  Common::Event m_gc_pad_event;
  Common::Event m_wii_pad_event;
  Common::Event m_first_pad_status_received_event;
  Common::Event m_wait_on_input_event;

  u8 m_saves_expected = 0;
  u8 m_saves_received = 0;
  u16 m_gecko_lines_expected = 0;
  u16 m_gecko_lines_received = 0;
  bool m_gecko_synced = false;
  u16 m_ar_lines_expected = 0;
  u16 m_ar_lines_received = 0;
  bool m_ar_synced = false;

  // Chunked transfers in progress, by transfer ID.
  std::unordered_map<u32, sf::Packet> m_chunks;

  u64 m_initial_rtc = 0;
  u32 m_timebase_frame = 0;

  std::unique_ptr<IOS::HLE::FS::FileSystem> m_wii_sync_fs;
  std::vector<u64> m_wii_sync_titles;
  std::string m_wii_sync_redirect_folder;
};

void NetPlay_Enable(NetPlayClient* const np);
void NetPlay_Disable();
bool NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries);
unsigned int NetPlay_GetLocalWiimoteForSlot(unsigned int slot);
}  // namespace NetPlay
