// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <SFML/Network/Packet.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Common/Event.h"
#include "Common/QoSSession.h"
#include "Common/SPSCQueue.h"
#include "Common/Timer.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "UICommon/NetPlayIndex.h"

namespace NetPlay
{
class NetPlayUI;
struct SaveSyncInfo;

// The host side of a NetPlay session. It relays input and chat between the clients, decides the
// settings everyone plays with and sends large data (saves) in chunks from a second thread.
class NetPlayServer : public Common::TraversalClientClient
{
public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, PlayerId pid, u8 channel_id = DEFAULT_CHANNEL);
  void SendAsyncToClients(sf::Packet&& packet, PlayerId skip_pid = 0,
                          u8 channel_id = DEFAULT_CHANNEL);
  void SendChunked(sf::Packet&& packet, PlayerId pid, const std::string& title = "");
  void SendChunkedToClients(sf::Packet&& packet, PlayerId skip_pid = 0,
                            const std::string& title = "");

  NetPlayServer(u16 port, bool forward_port, NetPlayUI* dialog,
                const NetTraversalConfig& traversal_config);
  ~NetPlayServer() override;

  bool ChangeGame(const SyncIdentifier& sync_identifier, const std::string& netplay_name);
  bool ComputeGameDigest(const SyncIdentifier& sync_identifier);
  bool AbortGameDigest();
  void SendChatMessage(const std::string& msg);

  bool DoAllPlayersHaveIPLDump() const;
  bool DoAllPlayersHaveHardwareFMA() const;
  bool StartGame();
  bool RequestStartGame();
  void AbortGameStart();

  PadMappingArray GetPadMapping() const;
  void SetPadMapping(const PadMappingArray& mappings);

  GBAConfigArray GetGBAConfig() const;
  void SetGBAConfig(const GBAConfigArray& configs, bool update_rom);

  PadMappingArray GetWiimoteMapping() const;
  void SetWiimoteMapping(const PadMappingArray& mappings);

  void AdjustPadBufferSize(unsigned int size);
  void SetHostInputAuthority(bool enable);

  void KickPlayer(PlayerId player);

  u16 GetPort() const;

  std::unordered_set<std::string> GetInterfaceSet() const;
  std::string GetInterfaceHost(const std::string& inter) const;

  bool is_connected = false;

private:
  // A connected player.
  class Client
  {
  public:
    PlayerId pid{};
    std::string name;
    std::string revision;
    SyncIdentifierComparison game_status = SyncIdentifierComparison::Unknown;
    bool has_ipl_dump = false;
    bool has_hardware_fma = false;

    ENetPeer* socket = nullptr;
    u32 ping = 0;
    // The game number the client last reported starting.
    u32 current_game = 0;

    Common::QoSSession qos_session;

    bool operator==(const Client& other) const { return this == &other; }
    bool IsHost() const { return pid == 1; }
  };

  // Whom a queued packet goes to: one player, or everyone but one (0 for nobody).
  enum class Audience
  {
    Only,
    AllExcept
  };

  struct OutgoingPacket
  {
    sf::Packet packet;
    PlayerId pid{};
    Audience audience{};
    u8 channel_id = 0;
  };

  struct ChunkedTransfer
  {
    sf::Packet packet;
    PlayerId pid{};
    Audience audience{};
    std::string title;
  };

  // A message handler's verdict: false drops the sender.
  using Verdict = bool;

  // Game start.
  bool SetupNetSettings();
  std::optional<SaveSyncInfo> CollectSaveSyncInfo();
  bool SyncSaveData(const SaveSyncInfo& sync_info);
  bool SyncCodes();
  void CheckSyncAndStartGame();
  u64 GetInitialNetPlayRTC() const;

  // Transport.
  void SendToClients(const sf::Packet& packet, PlayerId skip_pid = 0,
                     u8 channel_id = DEFAULT_CHANNEL);
  void Send(ENetPeer* socket, const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL);
  void DrainOutgoingQueue();
  void BroadcastPing();

  // Connections.
  ConnectionError OnConnect(ENetPeer* socket, sf::Packet& received_packet);
  void OnDisconnect(const Client& player);
  void HandleReceive(ENetPeer* peer, sf::Packet& packet);
  void HandlePeerGone(ENetPeer* peer);
  void AssignNewUserAPad(const Client& player);
  PlayerId GiveFirstAvailableIDTo(ENetPeer* player);
  void StopGameBecauseOf(PlayerId pid);

  // Messages from clients.
  Verdict OnData(sf::Packet& packet, Client& player);
  Verdict RelayChat(sf::Packet& packet, Client& player);
  Verdict NoteChunkProgress(sf::Packet& packet, Client& player);
  Verdict NoteChunkComplete(sf::Packet& packet, Client& player);
  Verdict RelayPads(sf::Packet& packet, Client& player);
  Verdict RelayGolferPads(sf::Packet& packet, Client& player);
  Verdict RelayWiimotes(sf::Packet& packet, Client& player);
  Verdict GolfRequest(sf::Packet& packet, Client& player);
  Verdict GolfRelease(sf::Packet& packet, Client& player);
  Verdict GolfAcquire(sf::Packet& packet, Client& player);
  Verdict GolfPrepared(sf::Packet& packet, Client& player);
  Verdict NotePong(sf::Packet& packet, Client& player);
  Verdict NoteGameStarted(sf::Packet& packet, Client& player);
  Verdict StopRequested(sf::Packet& packet, Client& player);
  Verdict RelayGameStatus(sf::Packet& packet, Client& player);
  Verdict NoteCapabilities(sf::Packet& packet, Client& player);
  Verdict RelayPowerButton(sf::Packet& packet, Client& player);
  Verdict CompareTimeBase(sf::Packet& packet, Client& player);
  Verdict RelayDigestProgress(sf::Packet& packet, Client& player);
  Verdict SaveSyncReply(sf::Packet& packet, Client& player);
  Verdict CodeSyncReply(sf::Packet& packet, Client& player);

  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress) override {}
  void OnConnectFailed(Common::TraversalConnectFailedReason) override {}
  void OnTtlDetermined(u8 ttl) override;

  void UpdatePadMapping();
  void UpdateGBAConfig();
  void UpdateWiimoteMapping();
  std::vector<std::pair<std::string, std::string>> GetInterfaceListInternal() const;

  void ChunkedDataThreadFunc();
  void ChunkedDataSend(sf::Packet&& packet, PlayerId pid, Audience audience);
  void ChunkedDataAbort();

  void SetupIndex();
  bool PlayerHasControllerMapped(PlayerId pid) const;

  NetSettings m_settings;

  bool m_is_running = false;
  std::atomic<bool> m_do_loop = false;
  Common::Timer m_ping_timer;
  u32 m_ping_key = 0;
  bool m_update_pings = false;
  u32 m_current_game = 0;
  unsigned int m_target_buffer_size = 0;
  PadMappingArray m_pad_map;
  GBAConfigArray m_gba_config;
  PadMappingArray m_wiimote_map;

  // Game start waits for every client to confirm both syncs.
  unsigned int m_save_data_synced_players = 0;
  unsigned int m_codes_synced_players = 0;
  bool m_saves_synced = true;
  bool m_codes_synced = true;
  bool m_start_pending = false;

  bool m_host_input_authority = false;
  PlayerId m_current_golfer = 1;
  PlayerId m_pending_golfer = 0;

  // By player ID.
  std::map<PlayerId, Client> m_players;

  // Desync detection: the time base each player reported for a frame.
  std::unordered_map<u32, std::vector<std::pair<PlayerId, u64>>> m_timebase_by_frame;
  bool m_desync_detected = false;

  // Lock order: game, then players, then the two queues.
  std::recursive_mutex m_game_mutex;
  std::recursive_mutex m_players_mutex;
  std::recursive_mutex m_outgoing_mutex;
  std::recursive_mutex m_chunked_mutex;

  Common::SPSCQueue<OutgoingPacket> m_outgoing;
  Common::SPSCQueue<ChunkedTransfer> m_chunked_queue;

  SyncIdentifier m_selected_game_identifier;
  std::string m_selected_game_name;
  std::thread m_thread;

  Common::Event m_chunked_data_event;
  Common::Event m_chunked_data_complete_event;
  std::thread m_chunked_data_thread;
  u32 m_next_chunked_data_id = 0;
  // Completion acknowledgements received per transfer.
  std::unordered_map<u32, unsigned int> m_chunked_data_complete_count;
  bool m_abort_chunked_data = false;

  ENetHost* m_server = nullptr;
  Common::TraversalClient* m_traversal_client = nullptr;
  NetPlayUI* m_dialog = nullptr;
  NetPlayIndex m_index;
};
}  // namespace NetPlay
