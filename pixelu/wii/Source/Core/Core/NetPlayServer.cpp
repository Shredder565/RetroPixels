// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/NetPlayServer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/CommonPaths.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/SFMLHelper.h"
#include "Common/StringUtil.h"
#include "Common/UPnP.h"
#include "Common/Version.h"

#include "Core/AchievementManager.h"
#include "Core/ActionReplay.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardDirectory.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiiSave.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayCommon.h"
#include "Core/SyncIdentifier.h"

#include "DiscIO/Enums.h"
#include "DiscIO/RiivolutionPatcher.h"

#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

#include "UICommon/GameFile.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#ifdef __HAIKU__
#define _BSD_SOURCE
#include <bsd/ifaddrs.h>
#elif !defined ANDROID
#include <ifaddrs.h>
#endif
#endif

namespace NetPlay
{
namespace
{
constexpr bool KEEP = true;
constexpr bool KICK = false;

// Each ENet peer carries the ID of the player behind it once its hello was accepted.
PlayerId* PlayerIdOf(ENetPeer* peer)
{
  return static_cast<PlayerId*>(peer->data);
}

void ForgetPlayerId(ENetPeer* peer)
{
  delete PlayerIdOf(peer);
  peer->data = nullptr;
}

bool IsValidPort(PadIndex port)
{
  return port >= 0 && port < 4;
}

sf::Packet MessageOf(MessageID id)
{
  sf::Packet packet;
  packet << id;
  return packet;
}

const char* SlotLetter(bool slot_a)
{
  return slot_a ? "A" : "B";
}
}  // namespace

// Everything collected on the host before the saves go out.
struct SaveSyncInfo
{
  u8 save_count = 0;
  std::shared_ptr<const UICommon::GameFile> game;
  bool has_wii_save = false;
  std::unique_ptr<IOS::HLE::FS::FileSystem> configured_fs;
  std::optional<std::vector<u8>> mii_data;
  std::vector<std::pair<u64, WiiSave::StoragePointer>> wii_saves;
  std::optional<DiscIO::Riivolution::SavegameRedirect> redirected_save;
};

// ---- Lifetime (GUI thread) ----

NetPlayServer::~NetPlayServer()
{
  if (is_connected)
  {
    // The chunk sender may be waiting on either of its events; wake it for both.
    m_do_loop = false;
    for (Common::Event* wake : {&m_chunked_data_event, &m_chunked_data_complete_event})
      wake->Set();
    if (m_chunked_data_thread.joinable())
      m_chunked_data_thread.join();
    m_thread.join();
    enet_host_destroy(m_server);

    // A traversal host is owned by the shared traversal client.
    if (Common::g_MainNetHost.get() == m_server)
      Common::g_MainNetHost.release();

    if (m_traversal_client)
    {
      Common::g_TraversalClient->m_Client = nullptr;
      Common::ReleaseTraversalClient();
    }
  }

#ifdef USE_UPNP
  Common::UPnP::StopPortmapping();
#endif
}

NetPlayServer::NetPlayServer(const u16 port, [[maybe_unused]] const bool forward_port,
                             NetPlayUI* dialog,
                             const NetTraversalConfig& traversal_config)
    : m_dialog(dialog)
{
  if (enet_initialize() != 0)
    PanicAlertFmtT("Enet Didn't Initialize");

  m_pad_map.fill(0);
  m_gba_config.fill({});
  m_wiimote_map.fill(0);

  if (!traversal_config.use_traversal)
  {
    ENetAddress listen_address;
    listen_address.host = ENET_HOST_ANY;
    listen_address.port = port;
    m_server = enet_host_create(&listen_address, 10, CHANNEL_COUNT, 0, 0);
    if (m_server != nullptr)
    {
      m_server->mtu = std::min(m_server->mtu, NetPlay::MAX_ENET_MTU);
      m_server->intercept = Common::ENet::InterceptCallback;
    }
    SetupIndex();
  }
  else
  {
    if (!Common::EnsureTraversalClient(traversal_config.traversal_host,
                                       traversal_config.traversal_port,
                                       traversal_config.traversal_port_alt, port))
    {
      return;
    }
    Common::g_TraversalClient->m_Client = this;
    m_traversal_client = Common::g_TraversalClient.get();
    m_server = Common::g_MainNetHost.get();
    if (Common::g_TraversalClient->HasFailed())
      Common::g_TraversalClient->ReconnectToServer();
  }

  if (m_server == nullptr)
    return;

  is_connected = true;
  m_do_loop = true;
  m_thread = std::thread(&NetPlayServer::ThreadFunc, this);
  // The default pad buffer, then the thread that sends chunked data.
  m_target_buffer_size = 5;
  m_chunked_data_thread = std::thread(&NetPlayServer::ChunkedDataThreadFunc, this);

#ifdef USE_UPNP
  if (forward_port && !traversal_config.use_traversal)
    Common::UPnP::TryPortmapping(port);
#endif
}

// Lists the session on the public index when the user asked for that.
void NetPlayServer::SetupIndex()
{
  const std::string index_name = Config::Get(Config::NETPLAY_INDEX_NAME);
  const std::string index_region = Config::Get(Config::NETPLAY_INDEX_REGION);
  if (!Config::Get(Config::NETPLAY_USE_INDEX) || index_name.empty() || index_region.empty())
    return;

  const std::string password = Config::Get(Config::NETPLAY_INDEX_PASSWORD);

  NetPlaySession session;
  session.name = index_name;
  session.region = index_region;
  session.has_password = !password.empty();
  session.method = m_traversal_client ? "traversal" : "direct";
  session.game_id = m_selected_game_name.empty() ? "UNKNOWN" : m_selected_game_name;
  session.player_count = static_cast<int>(m_players.size());
  session.in_game = m_is_running;
  session.port = GetPort();

  if (m_traversal_client)
  {
    if (!m_traversal_client->IsConnected())
      return;
    session.server_id = std::string(Common::g_TraversalClient->GetHostID().data(), 8);
  }
  else
  {
    session.server_id = GetExternalIPAddress();
    if (session.server_id.empty())
      return;
  }

  session.EncryptID(password);

  const bool added = m_index.Add(session);
  if (m_dialog != nullptr)
    m_dialog->OnIndexAdded(added, added ? "" : m_index.GetLastError());

  m_index.SetErrorCallback([this] {
    if (m_dialog != nullptr)
      m_dialog->OnIndexRefreshFailed(m_index.GetLastError());
  });
}

// ---- Network thread ----

void NetPlayServer::BroadcastPing()
{
  // The key only tells replies apart; truncating the clock is fine.
  m_ping_key = static_cast<u32>(Common::Timer::NowMs());

  sf::Packet ping = MessageOf(MessageID::Ping);
  ping << m_ping_key;
  m_ping_timer.Start();
  SendToClients(ping);

  m_index.SetPlayerCount(static_cast<int>(m_players.size()));
  m_index.SetGame(m_selected_game_name);
  m_index.SetInGame(m_is_running);
  m_update_pings = false;
}

void NetPlayServer::DrainOutgoingQueue()
{
  while (!m_outgoing.Empty())
  {
    {
      std::lock_guard lock(m_players_mutex);
      const OutgoingPacket& next = m_outgoing.Front();
      if (next.audience == Audience::AllExcept)
      {
        SendToClients(next.packet, next.pid, next.channel_id);
      }
      else if (const auto found = m_players.find(next.pid); found != m_players.end())
      {
        Send(found->second.socket, next.packet, next.channel_id);
      }
    }
    m_outgoing.Pop();
  }
}

void NetPlayServer::HandleReceive(ENetPeer* peer, sf::Packet& packet)
{
  if (peer->data == nullptr)
  {
    // The first packet of a new peer is its hello.
    ConnectionError verdict;
    {
      INFO_LOG_FMT(NETPLAY, "Initializing peer {:x}:{}", peer->address.host, peer->address.port);
      std::lock_guard lock(m_game_mutex);
      verdict = OnConnect(peer, packet);
    }
    if (verdict == ConnectionError::NoError)
      return;

    INFO_LOG_FMT(NETPLAY, "Error {} initializing peer {:x}:{}", static_cast<u8>(verdict),
                 peer->address.host, peer->address.port);
    sf::Packet refusal;
    refusal << verdict;
    // Not in the player list, so no lock is needed.
    Send(peer, refusal);
    ForgetPlayerId(peer);
    enet_peer_disconnect_later(peer, 0);
    return;
  }

  Client& sender = m_players.find(*PlayerIdOf(peer))->second;
  if (OnData(packet, sender) == KEEP)
    return;

  INFO_LOG_FMT(NETPLAY, "Invalid packet from client {}, disconnecting.", sender.pid);
  std::lock_guard lock(m_game_mutex);
  OnDisconnect(sender);
  ForgetPlayerId(peer);
}

void NetPlayServer::HandlePeerGone(ENetPeer* peer)
{
  std::lock_guard lock(m_game_mutex);
  if (peer->data == nullptr)
  {
    ERROR_LOG_FMT(NETPLAY, "enet_host_service: no peer data");
    return;
  }

  const PlayerId pid = *PlayerIdOf(peer);
  const auto found = m_players.find(pid);
  if (found == m_players.end())
  {
    ERROR_LOG_FMT(NETPLAY, "Invalid player {} to disconnect.", pid);
    return;
  }
  INFO_LOG_FMT(NETPLAY, "Disconnecting client {}.", pid);
  OnDisconnect(found->second);
  ForgetPlayerId(peer);
}

void NetPlayServer::ThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "NetPlayServer starting.");

  while (m_do_loop)
  {
    // Pings go out every second, and right after someone joined.
    if (m_update_pings || m_ping_timer.ElapsedMs() > 1000)
      BroadcastPing();

    if (m_traversal_client)
      m_traversal_client->HandleResends();

    ENetEvent event;
    const int serviced = enet_host_service(m_server, &event, 1000);
    DrainOutgoingQueue();

    if (serviced < 0)
    {
      ERROR_LOG_FMT(NETPLAY, "enet_host_service error: {}", serviced);
      continue;
    }
    if (serviced == 0)
      continue;

    switch (event.type)
    {
    case ENET_EVENT_TYPE_CONNECT:
      // Nothing to do until the hello arrives.
      INFO_LOG_FMT(NETPLAY, "Peer connected from: {:x}:{}", event.peer->address.host,
                   event.peer->address.port);
      break;
    case ENET_EVENT_TYPE_RECEIVE:
    {
      sf::Packet received;
      received.append(event.packet->data, event.packet->dataLength);
      HandleReceive(event.peer, received);
      enet_packet_destroy(event.packet);
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
      HandlePeerGone(event.peer);
      break;
    default:
      if (static_cast<int>(event.type) != Common::ENet::SKIPPABLE_EVENT)
      {
        ERROR_LOG_FMT(NETPLAY, "enet_host_service: unknown event type: {}",
                      static_cast<int>(event.type));
      }
      break;
    }
  }

  INFO_LOG_FMT(NETPLAY, "NetPlayServer shutting down.");

  for (auto& [pid, client] : m_players)
  {
    ForgetPlayerId(client.socket);
    enet_peer_disconnect(client.socket, 0);
  }
  m_players.clear();
}

// ---- Joining and leaving ----

ConnectionError NetPlayServer::OnConnect(ENetPeer* socket, sf::Packet& received_packet)
{
  std::string scm_revision;
  received_packet >> scm_revision;
  if (scm_revision != Common::GetScmRevGitStr())
    return ConnectionError::VersionMismatch;
  if (m_is_running || m_start_pending)
    return ConnectionError::GameRunning;
  if (m_players.size() >= 255)
    return ConnectionError::ServerFull;

  Client joining{};
  joining.pid = GiveFirstAvailableIDTo(socket);
  joining.socket = socket;
  received_packet >> joining.revision >> joining.name;
  if (StringUTF8CodePointCount(joining.name) > MAX_NAME_LENGTH)
    return ConnectionError::NameTooLong;

  const auto peer_timeout = static_cast<enet_uint32>(PEER_TIMEOUT.count());
  enet_peer_timeout(socket, 0, peer_timeout, peer_timeout);
  m_update_pings = true;

  AssignNewUserAPad(joining);

  // Everyone already here learns about the newcomer.
  sf::Packet announcement = MessageOf(MessageID::PlayerJoin);
  announcement << joining.pid << joining.name << joining.revision;
  SendToClients(announcement);

  // The newcomer learns its ID, the session state and who else is here.
  sf::Packet welcome = MessageOf(MessageID::ConnectionSuccessful);
  welcome << joining.pid;
  Send(socket, welcome);

  if (!m_selected_game_name.empty())
  {
    sf::Packet game = MessageOf(MessageID::ChangeGame);
    WriteSyncIdentifier(game, m_selected_game_identifier);
    game << m_selected_game_name;
    Send(socket, game);
  }

  if (!m_host_input_authority)
  {
    sf::Packet buffer = MessageOf(MessageID::PadBuffer);
    buffer << m_target_buffer_size;
    Send(socket, buffer);
  }

  sf::Packet authority = MessageOf(MessageID::HostInputAuthority);
  authority << m_host_input_authority;
  Send(socket, authority);

  for (const auto& [pid, other] : m_players)
  {
    sf::Packet join = MessageOf(MessageID::PlayerJoin);
    join << other.pid << other.name << other.revision;
    Send(socket, join);

    sf::Packet status = MessageOf(MessageID::GameStatus);
    status << other.pid << static_cast<u8>(other.game_status);
    Send(socket, status);
  }

  if (Config::Get(Config::NETPLAY_ENABLE_QOS))
    joining.qos_session = Common::QoSSession(socket);

  {
    std::lock_guard lock(m_players_mutex);
    const PlayerId key = *PlayerIdOf(socket);
    m_players.emplace(key, std::move(joining));
    UpdatePadMapping();
    UpdateGBAConfig();
    UpdateWiimoteMapping();
  }
  return ConnectionError::NoError;
}

// Ends the running game for everyone when a player who owns controllers leaves (unless it is the
// host itself).
void NetPlayServer::StopGameBecauseOf(PlayerId pid)
{
  if (pid == 1)
    return;

  const auto owns = [pid](const PadMappingArray& map) {
    return std::find(map.begin(), map.end(), pid) != map.end();
  };
  if (!owns(m_pad_map) && !owns(m_wiimote_map))
    return;

  std::lock_guard lock(m_game_mutex);
  m_is_running = false;
  SendToClients(MessageOf(MessageID::DisableGame));
}

void NetPlayServer::OnDisconnect(const Client& player)
{
  const PlayerId pid = player.pid;

  if (m_is_running)
    StopGameBecauseOf(pid);

  if (m_start_pending)
  {
    ChunkedDataAbort();
    m_dialog->OnGameStartAborted();
    m_start_pending = false;
  }

  sf::Packet farewell = MessageOf(MessageID::PlayerLeave);
  farewell << pid;

  enet_peer_disconnect(player.socket, 0);

  std::lock_guard lock(m_players_mutex);
  m_players.erase(pid);
  SendToClients(farewell);

  // Free the leaver's ports, telling everyone after each one.
  for (std::size_t port = 0; port < m_pad_map.size(); ++port)
  {
    if (m_pad_map[port] != pid)
      continue;
    m_pad_map[port] = 0;
    m_gba_config[port].enabled = false;
    UpdatePadMapping();
    UpdateGBAConfig();
  }
  for (PlayerId& owner : m_wiimote_map)
  {
    if (owner != pid)
      continue;
    owner = 0;
    UpdateWiimoteMapping();
  }
}

void NetPlayServer::AssignNewUserAPad(const Client& player)
{
  // A newcomer takes the first free GC port and the first free Wii Remote.
  for (PadMappingArray* map : {&m_pad_map, &m_wiimote_map})
  {
    const auto free_port = std::find(map->begin(), map->end(), PlayerId{0});
    if (free_port != map->end())
      *free_port = player.pid;
  }
}

PlayerId NetPlayServer::GiveFirstAvailableIDTo(ENetPeer* player)
{
  // The lowest ID nobody has; the host is always 1.
  PlayerId pid = 1;
  while (m_players.contains(pid))
    ++pid;
  player->data = new PlayerId(pid);
  return pid;
}

void NetPlayServer::KickPlayer(PlayerId player)
{
  for (auto& [pid, client] : m_players)
  {
    if (client.pid != player)
      continue;
    enet_peer_disconnect(client.socket, 0);
    return;
  }
}

bool NetPlayServer::PlayerHasControllerMapped(const PlayerId pid) const
{
  const auto owns = [pid](const PadMappingArray& map) {
    return std::find(map.begin(), map.end(), pid) != map.end();
  };
  return owns(m_pad_map) || owns(m_wiimote_map);
}

// ---- Messages from clients ----

NetPlayServer::Verdict NetPlayServer::OnData(sf::Packet& packet, Client& player)
{
  MessageID id;
  packet >> id;
  INFO_LOG_FMT(NETPLAY, "Got client message: {:x} from client {}", static_cast<u8>(id), player.pid);

  // Only this thread changes the player list, so reading it here needs no lock.
  switch (id)
  {
  case MessageID::ChatMessage:
    return RelayChat(packet, player);
  case MessageID::ChunkedDataProgress:
    return NoteChunkProgress(packet, player);
  case MessageID::ChunkedDataComplete:
    return NoteChunkComplete(packet, player);
  case MessageID::PadData:
    return RelayPads(packet, player);
  case MessageID::PadHostData:
    return RelayGolferPads(packet, player);
  case MessageID::WiimoteData:
    return RelayWiimotes(packet, player);
  case MessageID::GolfRequest:
    return GolfRequest(packet, player);
  case MessageID::GolfRelease:
    return GolfRelease(packet, player);
  case MessageID::GolfAcquire:
    return GolfAcquire(packet, player);
  case MessageID::GolfPrepare:
    return GolfPrepared(packet, player);
  case MessageID::Pong:
    return NotePong(packet, player);
  case MessageID::StartGame:
    return NoteGameStarted(packet, player);
  case MessageID::StopGame:
    return StopRequested(packet, player);
  case MessageID::GameStatus:
    return RelayGameStatus(packet, player);
  case MessageID::ClientCapabilities:
    return NoteCapabilities(packet, player);
  case MessageID::PowerButton:
    return RelayPowerButton(packet, player);
  case MessageID::TimeBase:
    return CompareTimeBase(packet, player);
  case MessageID::GameDigestProgress:
    return RelayDigestProgress(packet, player);
  case MessageID::GameDigestResult:
  case MessageID::GameDigestError:
  {
    // Both carry one string and are relayed under the same ID.
    std::string text;
    packet >> text;
    sf::Packet relay = MessageOf(id);
    relay << player.pid << text;
    SendToClients(relay);
    return KEEP;
  }
  case MessageID::SyncSaveData:
    return SaveSyncReply(packet, player);
  case MessageID::SyncCodes:
    return CodeSyncReply(packet, player);
  default:
    PanicAlertFmtT("Unknown message with id:{0} received from player:{1} Kicking player!",
                   static_cast<u8>(id), player.pid);
    return KICK;
  }
}

NetPlayServer::Verdict NetPlayServer::RelayChat(sf::Packet& packet, Client& player)
{
  std::string text;
  packet >> text;
  sf::Packet relay = MessageOf(MessageID::ChatMessage);
  relay << player.pid << text;
  SendToClients(relay, player.pid);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::NoteChunkProgress(sf::Packet& packet, Client& player)
{
  u32 transfer;
  packet >> transfer;
  const u64 received = Common::PacketReadU64(packet);
  m_dialog->SetChunkedProgress(player.pid, received);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::NoteChunkComplete(sf::Packet& packet, Client&)
{
  u32 transfer;
  packet >> transfer;
  if (const auto found = m_chunked_data_complete_count.find(transfer);
      found != m_chunked_data_complete_count.end())
  {
    ++found->second;
    m_chunked_data_complete_event.Set();
  }
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::RelayPads(sf::Packet& packet, Client& player)
{
  // Late input from the previous game is dropped.
  if (player.current_game != m_current_game)
    return KEEP;

  // With host input authority everything goes to the golfer, who answers for all.
  sf::Packet relay =
      MessageOf(m_host_input_authority ? MessageID::PadHostData : MessageID::PadData);
  while (!packet.endOfPacket())
  {
    PadIndex port;
    packet >> port;
    // Players may only send for ports they own.
    if (!IsValidPort(port) || m_pad_map[static_cast<std::size_t>(port)] != player.pid)
      return KICK;

    const bool buttons_only = m_gba_config[static_cast<std::size_t>(port)].enabled;
    GCPadStatus pad;
    ReadPadState(packet, &pad, buttons_only);
    relay << port;
    WritePadState(relay, pad, buttons_only);
  }

  if (!m_host_input_authority)
  {
    SendToClients(relay, player.pid);
    return KEEP;
  }
  // The golfer may have left before the game stopped.
  if (m_current_golfer != 0)
  {
    if (const auto golfer = m_players.find(m_current_golfer); golfer != m_players.end())
      Send(golfer->second.socket, relay);
  }
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::RelayGolferPads(sf::Packet& packet, Client& player)
{
  if (m_current_golfer != 0 && player.pid != m_current_golfer)
    return KICK;

  sf::Packet relay = MessageOf(MessageID::PadData);
  while (!packet.endOfPacket())
  {
    PadIndex port;
    packet >> port;
    if (!IsValidPort(port))
      return KICK;

    const bool buttons_only = m_gba_config[static_cast<std::size_t>(port)].enabled;
    GCPadStatus pad;
    ReadPadState(packet, &pad, buttons_only);
    relay << port;
    WritePadState(relay, pad, buttons_only);
  }
  SendToClients(relay, player.pid);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::RelayWiimotes(sf::Packet& packet, Client& player)
{
  if (player.current_game != m_current_game)
    return KEEP;

  sf::Packet relay = MessageOf(MessageID::WiimoteData);
  while (!packet.endOfPacket())
  {
    PadIndex port;
    packet >> port;
    if (!IsValidPort(port) || m_wiimote_map[static_cast<std::size_t>(port)] != player.pid)
      return KICK;

    WiimoteEmu::SerializedWiimoteState state{};
    packet >> state.length;
    if (state.length > state.data.size())
      return KICK;
    relay << port << state.length;
    for (u8 i = 0; i < state.length; ++i)
    {
      packet >> state.data[i];
      relay << state.data[i];
    }
  }
  SendToClients(relay, player.pid);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::GolfRequest(sf::Packet& packet, Client& player)
{
  PlayerId candidate;
  packet >> candidate;

  // Spectators cannot ask, and only real players can be handed control.
  if (!m_players.contains(candidate) || !PlayerHasControllerMapped(player.pid))
    return KEEP;

  const bool can_switch = m_host_input_authority && m_settings.golf_mode &&
                          m_pending_golfer == 0 && m_current_golfer != candidate &&
                          PlayerHasControllerMapped(candidate);
  if (!can_switch)
    return KEEP;

  m_pending_golfer = candidate;
  Send(m_players[candidate].socket, MessageOf(MessageID::GolfPrepare));
  return KEEP;
}

// The handover: the candidate prepares (GolfPrepare back), everyone is told there is no golfer
// for now, the old golfer releases and everyone is told the new one, who then acquires.
NetPlayServer::Verdict NetPlayServer::GolfPrepared(sf::Packet&, Client&)
{
  if (m_pending_golfer == 0)
    return KEEP;
  m_current_golfer = 0;
  sf::Packet nobody = MessageOf(MessageID::GolfSwitch);
  nobody << PlayerId{0};
  SendToClients(nobody);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::GolfRelease(sf::Packet&, Client&)
{
  if (m_pending_golfer == 0)
    return KEEP;
  sf::Packet next = MessageOf(MessageID::GolfSwitch);
  next << m_pending_golfer;
  SendToClients(next);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::GolfAcquire(sf::Packet&, Client&)
{
  if (m_pending_golfer == 0)
    return KEEP;
  m_current_golfer = m_pending_golfer;
  m_pending_golfer = 0;
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::NotePong(sf::Packet& packet, Client& player)
{
  // Under 49 days, so the cast cannot truncate.
  const auto round_trip = static_cast<u32>(m_ping_timer.ElapsedMs());
  u32 key = 0;
  packet >> key;
  if (key == m_ping_key)
    player.ping = round_trip;

  sf::Packet report = MessageOf(MessageID::PlayerPingData);
  report << player.pid << player.ping;
  SendToClients(report);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::NoteGameStarted(sf::Packet& packet, Client& player)
{
  packet >> player.current_game;
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::StopRequested(sf::Packet&, Client&)
{
  if (!m_is_running)
    return KEEP;
  m_is_running = false;

  std::lock_guard lock(m_players_mutex);
  SendToClients(MessageOf(MessageID::StopGame));
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::RelayGameStatus(sf::Packet& packet, Client& player)
{
  SyncIdentifierComparison status;
  packet >> status;
  m_players[player.pid].game_status = status;

  sf::Packet relay = MessageOf(MessageID::GameStatus);
  relay << player.pid << status;
  SendToClients(relay);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::NoteCapabilities(sf::Packet& packet, Client& player)
{
  Client& entry = m_players[player.pid];
  packet >> entry.has_ipl_dump >> entry.has_hardware_fma;
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::RelayPowerButton(sf::Packet&, Client& player)
{
  SendToClients(MessageOf(MessageID::PowerButton), player.pid);
  return KEEP;
}

// Every client reports its time base once a second. When all reports for a frame are in and
// they differ, the player whose value nobody else shares is blamed (or nobody, 0) and the session
// is marked desynced; later reports are ignored.
NetPlayServer::Verdict NetPlayServer::CompareTimeBase(sf::Packet& packet, Client& player)
{
  const u64 timebase = Common::PacketReadU64(packet);
  u32 frame = 0;
  packet >> frame;
  if (m_desync_detected)
    return KEEP;

  auto& reports = m_timebase_by_frame[frame];
  reports.emplace_back(player.pid, timebase);
  if (reports.size() < m_players.size())
    return KEEP;

  const u64 first_value = reports.front().second;
  const bool agree = std::all_of(reports.begin(), reports.end(),
                                 [first_value](const auto& r) { return r.second == first_value; });
  if (!agree)
  {
    int culprit = 0;
    for (const auto& [pid, value] : reports)
    {
      const bool shared = std::any_of(reports.begin(), reports.end(), [&](const auto& other) {
        return other.first != pid && other.second == value;
      });
      if (!shared)
      {
        culprit = pid;
        break;
      }
    }

    sf::Packet alarm = MessageOf(MessageID::DesyncDetected);
    alarm << culprit << frame;
    SendToClients(alarm);
    m_desync_detected = true;
  }
  m_timebase_by_frame.erase(frame);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::RelayDigestProgress(sf::Packet& packet, Client& player)
{
  int percent = 0;
  packet >> percent;
  sf::Packet relay = MessageOf(MessageID::GameDigestProgress);
  relay << player.pid << percent;
  SendToClients(relay);
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::SaveSyncReply(sf::Packet& packet, Client& player)
{
  SyncSaveDataID reply;
  packet >> reply;
  INFO_LOG_FMT(NETPLAY, "Got client SyncSaveData message: {:x} from client {}",
               static_cast<u8>(reply), player.pid);

  if (reply == SyncSaveDataID::Failure)
  {
    m_dialog->AppendChat(Common::FmtFormatT("{0} failed to synchronize.", player.name));
    m_dialog->OnGameStartAborted();
    ChunkedDataAbort();
    m_start_pending = false;
    return KEEP;
  }
  if (reply != SyncSaveDataID::Success)
  {
    PanicAlertFmtT(
        "Unknown SYNC_SAVE_DATA message with id:{0} received from player:{1} Kicking player!",
        static_cast<u8>(reply), player.pid);
    return KICK;
  }

  if (!m_start_pending)
  {
    INFO_LOG_FMT(NETPLAY, "SyncSaveData: Start not pending.");
    return KEEP;
  }
  // Everyone but the host has to confirm.
  const std::size_t needed = m_players.size() - 1;
  if (++m_save_data_synced_players < needed)
  {
    INFO_LOG_FMT(NETPLAY, "SyncSaveData: Not all players synchronized. ({} < {})",
                 m_save_data_synced_players, needed);
    return KEEP;
  }
  INFO_LOG_FMT(NETPLAY, "SyncSaveData: All players synchronized. ({} >= {})",
               m_save_data_synced_players, needed);
  m_dialog->AppendChat(Common::GetStringT("All players' saves synchronized."));
  m_saves_synced = true;
  CheckSyncAndStartGame();
  return KEEP;
}

NetPlayServer::Verdict NetPlayServer::CodeSyncReply(sf::Packet& packet, Client& player)
{
  SyncCodeID reply;
  packet >> reply;
  INFO_LOG_FMT(NETPLAY, "Got client SyncCodes message: {:x} from client {}",
               static_cast<u8>(reply), player.pid);

  if (reply == SyncCodeID::Failure)
  {
    m_dialog->AppendChat(Common::FmtFormatT("{0} failed to synchronize codes.", player.name));
    m_dialog->OnGameStartAborted();
    m_start_pending = false;
    return KEEP;
  }
  if (reply != SyncCodeID::Success)
  {
    PanicAlertFmtT(
        "Unknown SYNC_GECKO_CODES message with id:{0} received from player:{1} Kicking player!",
        static_cast<u8>(reply), player.pid);
    return KICK;
  }

  if (!m_start_pending)
  {
    INFO_LOG_FMT(NETPLAY, "SyncCodes: Start not pending.");
    return KEEP;
  }
  const std::size_t needed = m_players.size() - 1;
  if (++m_codes_synced_players < needed)
  {
    INFO_LOG_FMT(NETPLAY, "SyncCodes: Not all players synchronized. ({} < {})",
                 m_codes_synced_players, needed);
    return KEEP;
  }
  INFO_LOG_FMT(NETPLAY, "SyncCodes: All players synchronized. ({} >= {})", m_codes_synced_players,
               needed);
  m_dialog->AppendChat(Common::GetStringT("All players' codes synchronized."));
  m_codes_synced = true;
  CheckSyncAndStartGame();
  return KEEP;
}

// ---- Traversal ----

void NetPlayServer::OnTraversalStateChanged()
{
  const Common::TraversalClient::State state = m_traversal_client->GetState();

  // Once the traversal server gave us a host code the session can be listed.
  if (Common::g_TraversalClient->GetHostID()[0] != '\0')
    SetupIndex();

  if (!m_dialog)
    return;
  if (state == Common::TraversalClient::State::Failure)
    m_dialog->OnTraversalError(m_traversal_client->GetFailureReason());
  m_dialog->OnTraversalStateChanged(state);
}

void NetPlayServer::OnTtlDetermined(u8 ttl)
{
  m_dialog->OnTtlDetermined(ttl);
}

// ---- Controller assignment (GUI and network threads) ----

PadMappingArray NetPlayServer::GetPadMapping() const
{
  return m_pad_map;
}

GBAConfigArray NetPlayServer::GetGBAConfig() const
{
  return m_gba_config;
}

PadMappingArray NetPlayServer::GetWiimoteMapping() const
{
  return m_wiimote_map;
}

void NetPlayServer::SetPadMapping(const PadMappingArray& mappings)
{
  m_pad_map = mappings;
  UpdatePadMapping();
}

void NetPlayServer::SetGBAConfig([[maybe_unused]] const GBAConfigArray& mappings,
                                 [[maybe_unused]] bool update_rom)
{
  // Without mGBA there are no GBAs to configure; the clients just get the current state again.
#ifdef HAS_LIBMGBA
  m_gba_config = mappings;
  if (update_rom)
  {
    for (std::size_t port = 0; port < m_gba_config.size(); ++port)
    {
      GBAConfig& gba = m_gba_config[port];
      if (!gba.enabled)
        continue;
      const std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[port]);
      gba.has_rom = HW::GBA::Core::GetRomInfo(rom_path.c_str(), gba.hash, gba.title);
    }
  }
#endif
  UpdateGBAConfig();
}

void NetPlayServer::SetWiimoteMapping(const PadMappingArray& mappings)
{
  m_wiimote_map = mappings;
  UpdateWiimoteMapping();
}

void NetPlayServer::UpdatePadMapping()
{
  sf::Packet mapping = MessageOf(MessageID::PadMapping);
  for (const PlayerId owner : m_pad_map)
    mapping << owner;
  SendToClients(mapping);
}

void NetPlayServer::UpdateGBAConfig()
{
  sf::Packet configs = MessageOf(MessageID::GBAConfig);
  for (const GBAConfig& gba : m_gba_config)
  {
    configs << gba.enabled << gba.has_rom << gba.title;
    for (const u8 hash_byte : gba.hash)
      configs << hash_byte;
  }
  SendToClients(configs);
}

void NetPlayServer::UpdateWiimoteMapping()
{
  sf::Packet mapping = MessageOf(MessageID::WiimoteMapping);
  for (const PlayerId owner : m_wiimote_map)
    mapping << owner;
  SendToClients(mapping);
}

void NetPlayServer::AdjustPadBufferSize(unsigned int size)
{
  std::lock_guard lock(m_game_mutex);
  m_target_buffer_size = size;

  // Clients under host input authority manage their own buffering.
  if (m_host_input_authority)
    return;
  sf::Packet buffer = MessageOf(MessageID::PadBuffer);
  buffer << m_target_buffer_size;
  SendAsyncToClients(std::move(buffer));
}

void NetPlayServer::SetHostInputAuthority(const bool enable)
{
  std::lock_guard lock(m_game_mutex);
  m_host_input_authority = enable;

  sf::Packet authority = MessageOf(MessageID::HostInputAuthority);
  authority << m_host_input_authority;
  SendAsyncToClients(std::move(authority));

  // Back to normal buffering: the clients need the buffer size again.
  if (!m_host_input_authority)
    AdjustPadBufferSize(m_target_buffer_size);
}

// ---- Sending ----

void NetPlayServer::SendAsync(sf::Packet&& packet, const PlayerId pid, const u8 channel_id)
{
  {
    std::lock_guard lock(m_outgoing_mutex);
    m_outgoing.Push(OutgoingPacket{std::move(packet), pid, Audience::Only, channel_id});
  }
  Common::ENet::WakeupThread(m_server);
}

void NetPlayServer::SendAsyncToClients(sf::Packet&& packet, const PlayerId skip_pid,
                                       const u8 channel_id)
{
  {
    std::lock_guard lock(m_outgoing_mutex);
    m_outgoing.Push(OutgoingPacket{std::move(packet), skip_pid, Audience::AllExcept, channel_id});
  }
  Common::ENet::WakeupThread(m_server);
}

void NetPlayServer::SendChunked(sf::Packet&& packet, const PlayerId pid, const std::string& title)
{
  {
    std::lock_guard lock(m_chunked_mutex);
    m_chunked_queue.Push(ChunkedTransfer{std::move(packet), pid, Audience::Only, title});
  }
  m_chunked_data_event.Set();
}

void NetPlayServer::SendChunkedToClients(sf::Packet&& packet, const PlayerId skip_pid,
                                         const std::string& title)
{
  {
    std::lock_guard lock(m_chunked_mutex);
    m_chunked_queue.Push(ChunkedTransfer{std::move(packet), skip_pid, Audience::AllExcept, title});
  }
  m_chunked_data_event.Set();
}

void NetPlayServer::SendToClients(const sf::Packet& packet, const PlayerId skip_pid,
                                  const u8 channel_id)
{
  for (const auto& [key, client] : m_players)
  {
    // pid 0 never names a client; skip_pid 0 skips nobody.
    if (client.pid != 0 && client.pid != skip_pid)
      Send(client.socket, packet, channel_id);
  }
}

void NetPlayServer::Send(ENetPeer* socket, const sf::Packet& packet, const u8 channel_id)
{
  Common::ENet::SendPacket(socket, packet, channel_id);
}

void NetPlayServer::SendChatMessage(const std::string& msg)
{
  sf::Packet chat = MessageOf(MessageID::ChatMessage);
  // Messages from the host itself carry player ID 0.
  chat << PlayerId{0} << msg;
  SendAsyncToClients(std::move(chat));
}

// ---- Game selection ----

bool NetPlayServer::ChangeGame(const SyncIdentifier& sync_identifier,
                               const std::string& netplay_name)
{
  std::lock_guard lock(m_game_mutex);
  INFO_LOG_FMT(NETPLAY, "Changing game to {} ({:02x}).", netplay_name,
               fmt::join(sync_identifier.sync_hash, ""));

  m_selected_game_identifier = sync_identifier;
  m_selected_game_name = netplay_name;

  sf::Packet change = MessageOf(MessageID::ChangeGame);
  WriteSyncIdentifier(change, m_selected_game_identifier);
  change << m_selected_game_name;
  SendAsyncToClients(std::move(change));
  return true;
}

bool NetPlayServer::ComputeGameDigest(const SyncIdentifier& sync_identifier)
{
  sf::Packet request = MessageOf(MessageID::ComputeGameDigest);
  WriteSyncIdentifier(request, sync_identifier);
  SendAsyncToClients(std::move(request));
  return true;
}

bool NetPlayServer::AbortGameDigest()
{
  SendAsyncToClients(MessageOf(MessageID::GameDigestAbort));
  return true;
}

bool NetPlayServer::DoAllPlayersHaveIPLDump() const
{
  for (const auto& [pid, client] : m_players)
  {
    if (!client.has_ipl_dump)
      return false;
  }
  return true;
}

bool NetPlayServer::DoAllPlayersHaveHardwareFMA() const
{
  for (const auto& [pid, client] : m_players)
  {
    if (!client.has_hardware_fma)
      return false;
  }
  return true;
}

// ---- Game start (GUI thread unless noted) ----

// Reads the host's settings, with the game's ini layers applied, into m_settings.
bool NetPlayServer::SetupNetSettings()
{
  const auto game = m_dialog->FindGameFile(m_selected_game_identifier);
  if (game == nullptr)
  {
    ERROR_LOG_FMT(NETPLAY, "Game {:02x} not found in game list.",
                  fmt::join(m_selected_game_identifier.sync_hash, ""));
    PanicAlertFmtT("Selected game doesn't exist in game list!");
    return false;
  }
  INFO_LOG_FMT(NETPLAY, "Loading game settings for {:02x}.",
               fmt::join(m_selected_game_identifier.sync_hash, ""));

  Config::AddLayer(
      ConfigLoaders::GenerateGlobalGameConfigLoader(game->GetGameID(), game->GetRevision()));
  Config::AddLayer(
      ConfigLoaders::GenerateLocalGameConfigLoader(game->GetGameID(), game->GetRevision()));

  using namespace Config;
  NetSettings out;

  // Core.
  out.cpu_thread = Get(MAIN_CPU_THREAD);
  out.cpu_core = Get(MAIN_CPU_CORE);
  out.selected_language = Get(MAIN_GC_LANGUAGE);
  out.override_region_settings = Get(MAIN_OVERRIDE_REGION_SETTINGS);
  out.fallback_region = Get(MAIN_FALLBACK_REGION);
  out.allow_sd_writes = Get(MAIN_ALLOW_SD_WRITES);
  out.ram_override_enable = Get(MAIN_RAM_OVERRIDE_ENABLE);
  out.mem1_size = Get(MAIN_MEM1_SIZE);
  out.mem2_size = Get(MAIN_MEM2_SIZE);
  out.oc_enable = Get(MAIN_OVERCLOCK_ENABLE);
  out.oc_factor = Get(MAIN_OVERCLOCK);
  out.vi_oc_enable = Get(MAIN_VI_OVERCLOCK_ENABLE);
  out.vi_oc_factor = Get(MAIN_VI_OVERCLOCK);
  out.dsp_hle = Get(MAIN_DSP_HLE);
  out.dsp_enable_jit = Get(MAIN_DSP_JIT);
  out.float_exceptions = Get(MAIN_FLOAT_EXCEPTIONS);
  out.divide_by_zero_exceptions = Get(MAIN_DIVIDE_BY_ZERO_EXCEPTIONS);
  out.fprf = Get(MAIN_FPRF);
  out.accurate_nans = Get(MAIN_ACCURATE_NANS);
  out.accurate_fmadds = Get(MAIN_ACCURATE_FMADDS);
  out.disable_icache = Get(MAIN_DISABLE_ICACHE);
  out.sync_on_skip_idle = Get(MAIN_SYNC_ON_SKIP_IDLE);
  out.sync_gpu = Get(MAIN_SYNC_GPU);
  out.sync_gpu_max_distance = Get(MAIN_SYNC_GPU_MAX_DISTANCE);
  out.sync_gpu_min_distance = Get(MAIN_SYNC_GPU_MIN_DISTANCE);
  out.sync_gpu_overclock = Get(MAIN_SYNC_GPU_OVERCLOCK);
  out.jit_follow_branch = Get(MAIN_JIT_FOLLOW_BRANCH);
  out.fast_disc_speed = Get(MAIN_FAST_DISC_SPEED);
  out.mmu = Get(MAIN_MMU);
  out.fastmem = Get(MAIN_FASTMEM);
  out.memcard_size_override = Get(MAIN_MEMORY_CARD_SIZE);

  // The IPL is only used when every player has a dump of it.
  const bool everyone_has_ipl = DoAllPlayersHaveIPLDump();
  out.skip_ipl = Get(MAIN_SKIP_IPL) || !everyone_has_ipl;
  out.load_ipl_dump = Get(SESSION_LOAD_IPL_DUMP) && everyone_has_ipl;
  out.use_fma = DoAllPlayersHaveHardwareFMA();
  out.enable_cheats = AreCheatsEnabled();
  out.enable_hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();

  for (const ExpansionInterface::Slot slot : ExpansionInterface::SLOTS)
  {
    // A broadband adapter cannot be kept in sync, so SP1 is always empty.
    out.exi_device[slot] = slot == ExpansionInterface::Slot::SP1 ?
                               ExpansionInterface::EXIDeviceType::None :
                               Get(GetInfoForEXIDevice(slot));
  }

  for (std::size_t index = 0; index < SYSCONF_SETTINGS.size(); ++index)
  {
    std::visit(
        [&](auto* info) {
          static_assert(sizeof(info->GetDefaultValue()) <= sizeof(u32));
          out.sysconf_settings[index] = static_cast<u32>(Get(*info));
        },
        SYSCONF_SETTINGS[index].config_info);
  }

  // Graphics.
  out.internal_resolution = Get(GFX_EFB_SCALE);
  out.efb_scaled_copy = Get(GFX_HACK_COPY_EFB_SCALED);
  out.fast_depth_calc = Get(GFX_FAST_DEPTH_CALC);
  out.enable_pixel_lighting = Get(GFX_ENABLE_PIXEL_LIGHTING);
  out.widescreen_hack = Get(GFX_WIDESCREEN_HACK);
  out.disable_fog = Get(GFX_DISABLE_FOG);
  out.safe_texture_cache_color_samples = Get(GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  out.perf_queries_enable = Get(GFX_PERF_QUERIES_ENABLE);
  out.enable_gpu_texture_decoding = Get(GFX_ENABLE_GPU_TEXTURE_DECODING);
  out.force_texture_filtering = Get(GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
  out.max_anisotropy = Get(GFX_ENHANCE_MAX_ANISOTROPY);
  out.force_true_color = Get(GFX_ENHANCE_FORCE_TRUE_COLOR);
  out.disable_copy_filter = Get(GFX_ENHANCE_DISABLE_COPY_FILTER);
  out.arbitrary_mipmap_detection = Get(GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
  out.arbitrary_mipmap_detection_threshold =
      Get(GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD);
  out.efb_access_enable = Get(GFX_HACK_EFB_ACCESS_ENABLE);
  out.efb_access_tile_size = Get(GFX_HACK_EFB_ACCESS_TILE_SIZE);
  out.efb_access_defer_invalidation = Get(GFX_HACK_EFB_DEFER_INVALIDATION);
  out.bbox_enable = Get(GFX_HACK_BBOX_ENABLE);
  out.force_progressive = Get(GFX_HACK_FORCE_PROGRESSIVE);
  out.efb_to_texture_enable = Get(GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  out.xfb_to_texture_enable = Get(GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  out.disable_copy_to_vram = Get(GFX_HACK_DISABLE_COPY_TO_VRAM);
  out.immediate_xfb_enable = Get(GFX_HACK_IMMEDIATE_XFB);
  out.efb_emulate_format_changes = Get(GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  out.vertex_rounding = Get(GFX_HACK_VERTEX_ROUNDING);
  out.defer_efb_copies = Get(GFX_HACK_DEFER_EFB_COPIES);

  // NetPlay itself. Writing and syncing every Wii save both depend on loading saves at all.
  out.savedata_load = Get(NETPLAY_SAVEDATA_LOAD);
  out.savedata_write = out.savedata_load && Get(NETPLAY_SAVEDATA_WRITE);
  out.savedata_sync_all_wii = out.savedata_load && Get(NETPLAY_SAVEDATA_SYNC_ALL_WII);
  out.strict_settings_sync = Get(NETPLAY_STRICT_SETTINGS_SYNC);
  out.sync_codes = Get(NETPLAY_SYNC_CODES);
  out.golf_mode = Get(NETPLAY_NETWORK_MODE) == "golf";
  out.hide_remote_gbas = Get(NETPLAY_HIDE_REMOTE_GBAS);

  RemoveLayer(LayerType::GlobalGame);
  RemoveLayer(LayerType::LocalGame);

  m_settings = out;
  return true;
}

bool NetPlayServer::RequestStartGame()
{
  INFO_LOG_FMT(NETPLAY, "Start Game requested.");
  if (!SetupNetSettings())
    return false;

  // Alone, or with nothing to sync, the game starts right away.
  bool wait_for_clients = false;

  if (m_settings.savedata_load)
  {
    std::optional<SaveSyncInfo> saves = CollectSaveSyncInfo();
    if (!saves)
    {
      PanicAlertFmtT("Error collecting save data!");
      m_start_pending = false;
      return false;
    }

    if (saves->has_wii_save)
    {
      // The host boots from its own NAND with these titles.
      std::vector<u64> titles;
      titles.reserve(saves->wii_saves.size());
      for (const auto& [title_id, storage] : saves->wii_saves)
        titles.push_back(title_id);
      m_dialog->SetHostWiiSyncData(std::move(titles), saves->redirected_save ?
                                                           saves->redirected_save->m_target_path :
                                                           "");
    }

    if (m_players.size() > 1)
    {
      wait_for_clients = true;
      m_start_pending = true;
      if (!SyncSaveData(*saves))
      {
        PanicAlertFmtT("Error synchronizing save data!");
        m_start_pending = false;
        return false;
      }
    }
  }

  if (m_settings.sync_codes && m_players.size() > 1)
  {
    wait_for_clients = true;
    m_start_pending = true;
    if (!SyncCodes())
    {
      PanicAlertFmtT("Error synchronizing cheat codes!");
      m_start_pending = false;
      return false;
    }
  }

  if (!wait_for_clients)
    return StartGame();

  INFO_LOG_FMT(NETPLAY, "Waiting for data sync with clients.");
  return true;
}

// Also called from the network thread once all syncs completed.
bool NetPlayServer::StartGame()
{
  INFO_LOG_FMT(NETPLAY, "Starting game.");

  m_timebase_by_frame.clear();
  m_desync_detected = false;
  std::lock_guard lock(m_game_mutex);
  // A game number, not a time; truncation is fine.
  m_current_game = static_cast<u32>(Common::Timer::NowMs());

  // Repeat the buffer size so every client starts from it.
  if (!m_host_input_authority)
    AdjustPadBufferSize(m_target_buffer_size);

  m_current_golfer = 1;
  m_pending_golfer = 0;

  u64 initial_rtc = GetInitialNetPlayRTC();
  const auto game = m_dialog->FindGameFile(m_selected_game_identifier);
  std::string region =
      Config::GetDirectoryForRegion(Config::ToGameCubeRegion(game->GetRegion()));

  // Everyone boots with the host's GC SRAM.
  SConfig::GetInstance().m_strSRAM = File::GetUserPath(F_GCSRAM_IDX);
  InitSRAM(&m_settings.sram, SConfig::GetInstance().m_strSRAM);

  sf::Packet start = MessageOf(MessageID::StartGame);
  start << m_current_game;
  VisitStartGameSettings(m_settings, initial_rtc, region,
                         [&start](const auto& value) { start << value; });
  SendAsyncToClients(std::move(start));

  m_start_pending = false;
  m_is_running = true;
  return true;
}

void NetPlayServer::AbortGameStart()
{
  if (!m_start_pending)
  {
    INFO_LOG_FMT(NETPLAY, "Aborting game start but no game start pending.");
    return;
  }
  INFO_LOG_FMT(NETPLAY, "Aborting game start.");
  m_dialog->OnGameStartAborted();
  ChunkedDataAbort();
  m_start_pending = false;
}

void NetPlayServer::CheckSyncAndStartGame()
{
  if (!m_saves_synced || !m_codes_synced)
  {
    INFO_LOG_FMT(NETPLAY, "Not synchronized.");
    return;
  }
  INFO_LOG_FMT(NETPLAY, "Synchronized, starting game.");
  StartGame();
}

u64 NetPlayServer::GetInitialNetPlayRTC() const
{
  if (Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE))
    return Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
  return Common::Timer::GetLocalTimeSinceJan1970();
}

std::optional<SaveSyncInfo> NetPlayServer::CollectSaveSyncInfo()
{
  INFO_LOG_FMT(NETPLAY, "Collecting saves.");
  SaveSyncInfo info;

  // Memory cards: a raw card per the synced settings, a GCI folder per the host's config.
  for (const ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    if (m_settings.exi_device[slot] == ExpansionInterface::EXIDeviceType::MemoryCard)
    {
      INFO_LOG_FMT(NETPLAY, "Adding memory card (raw) in slot {}.", slot);
      ++info.save_count;
    }
    else if (Config::Get(Config::GetInfoForEXIDevice(slot)) ==
             ExpansionInterface::EXIDeviceType::MemoryCardFolder)
    {
      INFO_LOG_FMT(NETPLAY, "Adding memory card (folder) in slot {}.", slot);
      ++info.save_count;
    }
  }

  info.game = m_dialog->FindGameFile(m_selected_game_identifier);
  if (info.game == nullptr)
  {
    PanicAlertFmtT("Selected game doesn't exist in game list!");
    return std::nullopt;
  }

  const DiscIO::Platform platform = info.game->GetPlatform();
  const bool wii_title =
      platform == DiscIO::Platform::WiiDisc || platform == DiscIO::Platform::WiiWAD;
  if (m_settings.savedata_load && (wii_title || platform == DiscIO::Platform::ELFOrDOL))
  {
    INFO_LOG_FMT(NETPLAY, "Adding Wii saves.");
    info.has_wii_save = true;
    ++info.save_count;

    info.configured_fs = IOS::HLE::FS::MakeFileSystem(IOS::HLE::FS::Location::Configured);
    if (m_settings.savedata_sync_all_wii)
    {
      // Every installed title with a complete, readable save.
      IOS::HLE::Kernel ios;
      for (const u64 title : ios.GetESCore().GetInstalledTitles())
      {
        auto save = WiiSave::MakeNandStorage(info.configured_fs.get(), title);
        const bool readable = save && save->ReadHeader().has_value() &&
                              save->ReadBkHeader().has_value() && save->ReadFiles().has_value();
        if (!readable)
        {
          INFO_LOG_FMT(NETPLAY, "Skipping Wii save of title {:016x}.", title);
          continue;
        }
        info.wii_saves.emplace_back(title, std::move(save));
      }
    }
    else if (wii_title)
    {
      const u64 title = info.game->GetTitleID();
      info.wii_saves.emplace_back(title, WiiSave::MakeNandStorage(info.configured_fs.get(), title));
    }

    if (auto mii_file = info.configured_fs->OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                                                     Common::GetMiiDatabasePath(),
                                                     IOS::HLE::FS::Mode::Read))
    {
      std::vector<u8> miis(mii_file->GetStatus()->size);
      if (!mii_file->Read(miis.data(), miis.size()))
        return std::nullopt;
      info.mii_data = std::move(miis);
    }

    if (info.game->GetBlobType() == DiscIO::BlobType::MOD_DESCRIPTOR)
    {
      if (const auto boot = BootParameters::GenerateFromFile(info.game->GetFilePath()))
        info.redirected_save =
            DiscIO::Riivolution::ExtractSavegameRedirect(boot->riivolution_patches);
    }
  }

  for (std::size_t port = 0; port < m_gba_config.size(); ++port)
  {
    if (!m_gba_config[port].enabled || !m_gba_config[port].has_rom)
      continue;
    INFO_LOG_FMT(NETPLAY, "Adding GBA save in slot {}.", port);
    ++info.save_count;
  }
  return info;
}

// Each save goes out as its own chunked transfer to every client but the host (pid 1).
bool NetPlayServer::SyncSaveData(const SaveSyncInfo& sync_info)
{
  INFO_LOG_FMT(NETPLAY, "Sending {} savegame chunks to clients.", sync_info.save_count);
  m_saves_synced = false;
  m_save_data_synced_players = 0;

  {
    sf::Packet notify = MessageOf(MessageID::SyncSaveData);
    notify << SyncSaveDataID::Notify << sync_info.save_count;
    // On the chunked channel so it arrives before the transfers.
    SendAsyncToClients(std::move(notify), 0, CHUNKED_DATA_CHANNEL);
  }
  if (sync_info.save_count == 0)
    return true;

  const DiscIO::Region game_region = sync_info.game->GetRegion();
  const DiscIO::Region gc_region = Config::ToGameCubeRegion(game_region);
  const std::string region_dir = Config::GetDirectoryForRegion(gc_region);

  for (const ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    const bool slot_a = slot == ExpansionInterface::Slot::A;
    const char* const letter = SlotLetter(slot_a);

    if (m_settings.exi_device[slot] == ExpansionInterface::EXIDeviceType::MemoryCard)
    {
      const int size_override = m_settings.memcard_size_override;
      const bool sized = size_override >= 0 && size_override <= 4;
      const u16 mbits =
          sized ? static_cast<u16>(Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_override) :
                  Memcard::MBIT_SIZE_MEMORY_CARD_2043;
      const std::string card_path = Config::GetMemcardPath(slot, game_region, mbits);

      sf::Packet card = MessageOf(MessageID::SyncSaveData);
      card << SyncSaveDataID::RawData << slot_a << region_dir << size_override;
      if (!File::Exists(card_path))
      {
        // An absent card is sent as an empty file.
        INFO_LOG_FMT(NETPLAY, "Sending empty marker for raw memcard {} in slot {}.", card_path,
                     letter);
        card << u64{0};
      }
      else
      {
        INFO_LOG_FMT(NETPLAY, "Sending data of raw memcard {} in slot {}.", card_path, letter);
        if (!CompressFileIntoPacket(card_path, card))
          return false;
      }
      SendChunkedToClients(std::move(card), 1,
                           fmt::format("Memory Card {} Synchronization", letter));
      continue;
    }

    if (Config::Get(Config::GetInfoForEXIDevice(slot)) !=
        ExpansionInterface::EXIDeviceType::MemoryCardFolder)
    {
      continue;
    }

    const std::string folder = Config::GetGCIFolderPath(slot, gc_region);
    sf::Packet gci = MessageOf(MessageID::SyncSaveData);
    gci << SyncSaveDataID::GCIData << slot_a;
    if (!File::IsDirectory(folder))
    {
      INFO_LOG_FMT(NETPLAY, "Sending empty marker for GCI memcard {} in slot {}.", folder, letter);
      gci << u8{0};
    }
    else
    {
      const std::vector<std::string> files =
          GCMemcardDirectory::GetFileNamesForGameID(folder + DIR_SEP, sync_info.game->GetGameID());
      INFO_LOG_FMT(NETPLAY, "Sending data of GCI memcard {} in slot {} ({} files).", folder, letter,
                   files.size());
      gci << static_cast<u8>(files.size());
      for (const std::string& file : files)
      {
        const std::string name = file.substr(file.find_last_of('/') + 1);
        INFO_LOG_FMT(NETPLAY, "Sending GCI {}.", name);
        gci << name;
        if (!CompressFileIntoPacket(file, gci))
          return false;
      }
    }
    SendChunkedToClients(std::move(gci), 1, fmt::format("GCI Folder {} Synchronization", letter));
  }

  if (sync_info.has_wii_save)
  {
    sf::Packet wii = MessageOf(MessageID::SyncSaveData);
    wii << SyncSaveDataID::WiiData;

    // The Mii database leads.
    wii << sync_info.mii_data.has_value();
    if (sync_info.mii_data && !CompressBufferIntoPacket(*sync_info.mii_data, wii))
      return false;

    INFO_LOG_FMT(NETPLAY, "Sending {} Wii saves.", sync_info.wii_saves.size());
    wii << static_cast<u32>(sync_info.wii_saves.size());
    for (const auto& [title_id, storage] : sync_info.wii_saves)
    {
      wii << u64{title_id};
      if (!storage->SaveExists())
      {
        INFO_LOG_FMT(NETPLAY, "No data for Wii save of title {:016x}.", title_id);
        wii << false;
        continue;
      }

      const std::optional<WiiSave::Header> header = storage->ReadHeader();
      const std::optional<WiiSave::BkHeader> bk_header = storage->ReadBkHeader();
      const std::optional<std::vector<WiiSave::Storage::SaveFile>> files = storage->ReadFiles();
      if (!header || !bk_header || !files)
      {
        INFO_LOG_FMT(NETPLAY, "Wii save of title {:016x} is corrupted.", title_id);
        return false;
      }
      INFO_LOG_FMT(NETPLAY, "Sending Wii save of title {:016x}.", title_id);
      wii << true;

      const u32 banner_size = header->banner_size;
      wii << u64{header->tid} << banner_size << header->permissions << header->unk1;
      for (const u8 md5_byte : header->md5)
        wii << md5_byte;
      wii << u16{header->unk2};
      const std::size_t banner_bytes = std::min<std::size_t>(banner_size, sizeof(header->banner));
      for (std::size_t offset = 0; offset < banner_bytes; ++offset)
        wii << header->banner[offset];

      for (const u32 word :
           {u32{bk_header->size}, u32{bk_header->magic}, u32{bk_header->ngid},
            u32{bk_header->number_of_files}, u32{bk_header->size_of_files}, u32{bk_header->unk1},
            u32{bk_header->unk2}, u32{bk_header->total_size}})
      {
        wii << word;
      }
      for (const u8 unk3_byte : bk_header->unk3)
        wii << unk3_byte;
      wii << u64{bk_header->tid};
      for (const u8 mac_byte : bk_header->mac_address)
        wii << mac_byte;

      for (const WiiSave::Storage::SaveFile& entry : *files)
      {
        INFO_LOG_FMT(NETPLAY, "Sending Wii save data of type {} at {}", static_cast<u8>(entry.type),
                     entry.path);
        wii << entry.mode << entry.attributes << entry.type << entry.path;
        if (entry.type != WiiSave::Storage::SaveFile::Type::File)
          continue;
        const std::optional<std::vector<u8>>& contents = *entry.data;
        if (!contents || !CompressBufferIntoPacket(*contents, wii))
          return false;
      }
    }

    // A Riivolution redirect folder closes the message.
    wii << sync_info.redirected_save.has_value();
    if (sync_info.redirected_save)
    {
      INFO_LOG_FMT(NETPLAY, "Sending redirected save at {}.",
                   sync_info.redirected_save->m_target_path);
      if (!CompressFolderIntoPacket(sync_info.redirected_save->m_target_path, wii))
        return false;
    }

    SendChunkedToClients(std::move(wii), 1, "Wii Save Synchronization");
  }

  for (std::size_t port = 0; port < m_gba_config.size(); ++port)
  {
    if (!m_gba_config[port].enabled || !m_gba_config[port].has_rom)
      continue;

    sf::Packet gba = MessageOf(MessageID::SyncSaveData);
    gba << SyncSaveDataID::GBAData << static_cast<u8>(port);

    std::string save_path;
#ifdef HAS_LIBMGBA
    save_path = HW::GBA::Core::GetSavePath(Config::Get(Config::MAIN_GBA_ROM_PATHS[port]),
                                           static_cast<int>(port));
#endif
    if (!File::Exists(save_path))
    {
      INFO_LOG_FMT(NETPLAY, "Sending empty marker for GBA save at {} for slot {}.", save_path,
                   port);
      gba << u64{0};
    }
    else
    {
      INFO_LOG_FMT(NETPLAY, "Sending data of GBA save at {} for slot {}.", save_path, port);
      if (!CompressFileIntoPacket(save_path, gba))
        return false;
    }
    SendChunkedToClients(std::move(gba), 1,
                         fmt::format("GBA{} Save File Synchronization", port + 1));
  }
  return true;
}

// Sends the host's enabled Gecko and AR codes: for each kind a line count, then the lines.
bool NetPlayServer::SyncCodes()
{
  INFO_LOG_FMT(NETPLAY, "Sending codes to clients.");
  m_codes_synced = false;

  const auto game = m_dialog->FindGameFile(m_selected_game_identifier);
  if (game == nullptr)
  {
    PanicAlertFmtT("Selected game doesn't exist in game list!");
    return false;
  }

  const std::string_view game_id = game->GetGameID();
  const auto revision = game->GetRevision();
  const std::vector<std::string> ini_names = ConfigLoaders::GetGameIniFilenames(game_id, revision);
  Common::IniFile global_ini;
  Common::IniFile local_ini;
  for (const std::string& name : ini_names)
    global_ini.Load(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name, true);
  for (const std::string& name : ini_names)
    local_ini.Load(File::GetUserPath(D_GAMESETTINGS_IDX) + name, true);

  m_codes_synced_players = 0;
  {
    sf::Packet notify = MessageOf(MessageID::SyncCodes);
    notify << SyncCodeID::Notify;
    SendAsyncToClients(std::move(notify));
  }

  const auto send_lines = [this](SyncCodeID count_id, SyncCodeID data_id, const char* kind,
                                 const auto& codes, const auto& lines_of, const auto& words_of) {
    u16 line_count = 0;
    for (const auto& code : codes)
    {
      INFO_LOG_FMT(NETPLAY, "Indexing {}", code.name);
      line_count = static_cast<u16>(line_count + lines_of(code).size());
    }
    INFO_LOG_FMT(NETPLAY, "Sending {} {} codelines", line_count, kind);

    sf::Packet count = MessageOf(MessageID::SyncCodes);
    count << count_id << line_count;
    SendAsyncToClients(std::move(count));

    sf::Packet data = MessageOf(MessageID::SyncCodes);
    data << data_id;
    for (const auto& code : codes)
    {
      INFO_LOG_FMT(NETPLAY, "Sending {}", code.name);
      for (const auto& line : lines_of(code))
      {
        const auto [first, second] = words_of(line);
        INFO_LOG_FMT(NETPLAY, "{:08x} {:08x}", first, second);
        data << first << second;
      }
    }
    SendAsyncToClients(std::move(data));
  };

  {
    std::vector<Gecko::GeckoCode> codes = Gecko::LoadCodes(global_ini, local_ini);
#ifdef USE_RETRO_ACHIEVEMENTS
    AchievementManager::GetInstance().FilterApprovedGeckoCodes(codes, game_id, revision);
#endif
    // This also makes them the host's active codes.
    const std::vector<Gecko::GeckoCode> active = Gecko::SetAndReturnActiveCodes(codes);
    send_lines(
        SyncCodeID::NotifyGecko, SyncCodeID::GeckoData, "Gecko", active,
        [](const Gecko::GeckoCode& code) -> const auto& { return code.codes; },
        [](const Gecko::GeckoCode::Code& line) { return std::pair{line.address, line.data}; });
  }

  {
    std::vector<ActionReplay::ARCode> codes = ActionReplay::LoadCodes(global_ini, local_ini);
#ifdef USE_RETRO_ACHIEVEMENTS
    AchievementManager::GetInstance().FilterApprovedARCodes(codes, game_id, revision);
#endif
    const std::vector<ActionReplay::ARCode> active = ActionReplay::ApplyAndReturnCodes(codes);
    send_lines(
        SyncCodeID::NotifyAR, SyncCodeID::ARData, "AR", active,
        [](const ActionReplay::ARCode& code) -> const auto& { return code.ops; },
        [](const ActionReplay::AREntry& op) { return std::pair{op.cmd_addr, op.value}; });
  }
  return true;
}

// ---- Chunked data thread ----

void NetPlayServer::ChunkedDataSend(sf::Packet&& packet, const PlayerId pid,
                                    const Audience audience)
{
  if (audience == Audience::Only)
    SendAsync(std::move(packet), pid, CHUNKED_DATA_CHANNEL);
  else
    SendAsyncToClients(std::move(packet), pid, CHUNKED_DATA_CHANNEL);
}

void NetPlayServer::ChunkedDataAbort()
{
  m_abort_chunked_data = true;
  m_chunked_data_event.Set();
  m_chunked_data_complete_event.Set();
}

// Each queued transfer becomes Start (ID, title, size), Payload pieces of up to 16 KiB, then End;
// the thread waits for every receiver's Complete before the next one. An abort sends Abort
// instead and drops what is still queued.
void NetPlayServer::ChunkedDataThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "Starting Chunked Data Thread.");

  while (m_do_loop)
  {
    m_chunked_data_event.Wait();

    if (m_abort_chunked_data)
    {
      while (!m_chunked_queue.Empty())
        m_chunked_queue.Pop();
      m_abort_chunked_data = false;
    }

    while (!m_chunked_queue.Empty())
    {
      if (!m_do_loop)
        return;
      if (m_abort_chunked_data)
        break;

      ChunkedTransfer& transfer = m_chunked_queue.Front();
      const u32 id = m_next_chunked_data_id++;
      const std::size_t total = transfer.packet.getDataSize();
      m_chunked_data_complete_count[id] = 0;

      std::vector<int> receivers;
      if (transfer.audience == Audience::Only)
      {
        receivers.push_back(transfer.pid);
      }
      else
      {
        for (const auto& [key, client] : m_players)
        {
          if (client.pid != transfer.pid)
            receivers.push_back(client.pid);
        }
      }
      const std::size_t receiver_count = receivers.size();
      INFO_LOG_FMT(NETPLAY, "Informing players {} of data chunk {} start.",
                   fmt::join(receivers, ", "), id);

      sf::Packet start = MessageOf(MessageID::ChunkedDataStart);
      start << id << transfer.title << u64{total};
      ChunkedDataSend(std::move(start), transfer.pid, transfer.audience);
      if (transfer.audience == Audience::AllExcept && transfer.pid == 1)
        m_dialog->ShowChunkedProgressDialog(transfer.title, total, receivers);

      const bool throttle = Config::Get(Config::NETPLAY_ENABLE_CHUNKED_UPLOAD_LIMIT);
      const float kbits = static_cast<float>(
          std::max(Config::Get(Config::NETPLAY_CHUNKED_UPLOAD_LIMIT), 1u));
      const float bytes_per_second = kbits / 8.0f * 1024.0f;
      const std::chrono::duration<double> interval(CHUNKED_DATA_UNIT_SIZE / bytes_per_second);

      // Receivers that left are not waited for.
      bool receiver_gone = false;
      std::size_t offset = 0;
      do
      {
        if (!m_do_loop)
          return;
        if (m_abort_chunked_data)
        {
          INFO_LOG_FMT(NETPLAY, "Informing players of data chunk {} abort.", id);
          sf::Packet abort = MessageOf(MessageID::ChunkedDataAbort);
          abort << id;
          ChunkedDataSend(std::move(abort), transfer.pid, transfer.audience);
          break;
        }
        if (transfer.audience == Audience::Only && !m_players.contains(transfer.pid))
        {
          receiver_gone = true;
          break;
        }

        const auto piece_started = std::chrono::steady_clock::now();
        const std::size_t length = std::min(CHUNKED_DATA_UNIT_SIZE, total - offset);
        sf::Packet piece = MessageOf(MessageID::ChunkedDataPayload);
        piece << id;
        piece.append(static_cast<const u8*>(transfer.packet.getData()) + offset, length);
        INFO_LOG_FMT(NETPLAY, "Sending data chunk of {} ({} bytes at {}/{}).", id, length, offset,
                     total);
        ChunkedDataSend(std::move(piece), transfer.pid, transfer.audience);
        offset += CHUNKED_DATA_UNIT_SIZE;

        if (throttle)
        {
          const std::chrono::duration<double> spent =
              std::chrono::steady_clock::now() - piece_started;
          std::this_thread::sleep_for(interval - spent);
        }
      } while (offset < total);

      if (!m_abort_chunked_data)
      {
        INFO_LOG_FMT(NETPLAY, "Informing players of data chunk {} end.", id);
        sf::Packet end = MessageOf(MessageID::ChunkedDataEnd);
        end << id;
        ChunkedDataSend(std::move(end), transfer.pid, transfer.audience);
      }

      while (m_chunked_data_complete_count[id] < receiver_count && m_do_loop &&
             !m_abort_chunked_data && !receiver_gone)
      {
        m_chunked_data_complete_event.Wait();
      }
      m_chunked_data_complete_count.erase(id);
      m_dialog->HideChunkedProgressDialog();
      m_chunked_queue.Pop();
    }
  }

  INFO_LOG_FMT(NETPLAY, "Stopping Chunked Data Thread.");
}

// ---- Interfaces (GUI thread) ----

u16 NetPlayServer::GetPort() const
{
  return m_server->address.port;
}

std::unordered_set<std::string> NetPlayServer::GetInterfaceSet() const
{
  std::unordered_set<std::string> names;
  for (const auto& [name, address] : GetInterfaceListInternal())
    names.insert(name);
  return names;
}

std::string NetPlayServer::GetInterfaceHost(const std::string& inter) const
{
  for (const auto& [name, address] : GetInterfaceListInternal())
  {
    if (name == inter)
      return fmt::format("{}:{}", address, GetPort());
  }
  return "?";
}

// The IPv4 address of each non-loopback interface, by interface name.
std::vector<std::pair<std::string, std::string>> NetPlayServer::GetInterfaceListInternal() const
{
  std::vector<std::pair<std::string, std::string>> interfaces;
#if defined(_WIN32)
  ULONG needed = 0;
  GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &needed);
  std::vector<char> storage(needed);
  auto* const first = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data());
  const ULONG status = GetAdaptersAddresses(AF_INET, 0, nullptr, first, &needed);
  if (status != NO_ERROR)
  {
    WARN_LOG_FMT(NETPLAY, "GetAdaptersAddresses: {}", status);
  }
  else
  {
    for (auto* adapter = first; adapter != nullptr; adapter = adapter->Next)
    {
      if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->FirstUnicastAddress == nullptr)
        continue;
      const auto* ipv4 =
          reinterpret_cast<const sockaddr_in*>(adapter->FirstUnicastAddress->Address.lpSockaddr);
      char text[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
      interfaces.emplace_back(WStringToUTF8(adapter->FriendlyName), text);
    }
  }
#elif defined(ANDROID)
  // Android has no getifaddrs; nothing uses this there.
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != -1)
  {
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
      const sockaddr* address = entry->ifa_addr;
      if (address == nullptr || address->sa_family != AF_INET)
        continue;
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
      if (ntohl(ipv4->sin_addr.s_addr) == INADDR_LOOPBACK)
        continue;
      char text[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) == nullptr)
        continue;
      interfaces.emplace_back(entry->ifa_name, text);
    }
    freeifaddrs(list);
  }
#endif
  if (interfaces.empty())
    interfaces.emplace_back("!local!", "127.0.0.1");
  return interfaces;
}
}  // namespace NetPlay
