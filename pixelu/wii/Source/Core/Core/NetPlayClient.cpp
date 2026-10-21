// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/NetPlayClient.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/QoSSession.h"
#include "Common/SFMLHelper.h"
#include "Common/Timer.h"
#include "Common/Version.h"

#include "Core/ActionReplay.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiiSave.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayCommon.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"

#include "InputCommon/GCAdapter.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace NetPlay
{
// The client of the running session, for the emulator hooks at the bottom of this file.
static std::mutex s_active_client_mutex;
static NetPlayClient* s_active_client = nullptr;
// Set while SI polls all pads in one go (from VI) rather than one pad through MMIO.
static bool s_poll_is_batched = false;

namespace
{
constexpr int NO_LOCAL_PAD = 4;

// Where a client keeps the Wii NAND and the Riivolution redirect folder it receives from the host.
std::string SyncedWiiRoot()
{
  return File::GetUserPath(D_USER_IDX) + "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
}

std::string SyncedRedirectRoot()
{
  return File::GetUserPath(D_USER_IDX) + "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;
}

void RemoveFolderIfPresent(const std::string& path)
{
  if (File::Exists(path))
    File::DeleteDirRecursively(path);
}

// The n-th (counting from 0) in-game port that `pid` owns, or 4 when there are fewer.
int NthPortOf(int n, const PadMappingArray& map, PlayerId pid)
{
  int seen = -1;
  for (int port = 0; port < 4; ++port)
  {
    if (map[static_cast<std::size_t>(port)] == pid)
      ++seen;
    if (seen == n)
      return port;
  }
  return NO_LOCAL_PAD;
}

// How many of `pid`'s ports come before `port`, or 4 when `pid` does not own it.
int RankOfPort(int port, const PadMappingArray& map, PlayerId pid)
{
  if (map[static_cast<std::size_t>(port)] != pid)
    return NO_LOCAL_PAD;
  return static_cast<int>(std::count(map.begin(), map.begin() + port, pid));
}

std::string HashFileForDigest(const std::string& file_path,
                              const std::function<bool(int)>& report_progress)
{
  std::unique_ptr<DiscIO::BlobReader> reader(DiscIO::CreateBlobReader(file_path));
  const u64 total = reader->GetDataSize();
  auto sha1 = Common::SHA1::CreateContext();

  std::vector<u8> block(8 * 1024 * 1024);
  for (u64 done = 0; done < total;)
  {
    const std::size_t step = static_cast<std::size_t>(std::min<u64>(block.size(), total - done));
    if (!reader->Read(done, step, block.data()))
      return "";
    sha1->Update(block.data(), step);
    done += step;

    const float fraction = static_cast<float>(done) / static_cast<float>(total);
    if (!report_progress(static_cast<int>(fraction * 100)))
      return "";
  }
  return fmt::format("{:02x}", fmt::join(sha1->Finish(), ""));
}
}  // namespace

// ---- Lifetime and connection (GUI thread) ----

NetPlayClient::~NetPlayClient()
{
  if (m_is_running)
    StopGame();

  if (m_is_connected)
  {
    m_should_compute_game_digest = false;
    m_dialog->AbortGameDigest();
    if (m_game_digest_thread.joinable())
      m_game_digest_thread.join();
    m_do_loop = false;
    m_thread.join();

    m_chunks.clear();
    m_dialog->HideChunkedProgressDialog();
  }

  if (m_server)
    Disconnect();

  // The traversal host belongs to the shared traversal client, not to us.
  if (Common::g_MainNetHost.get() == m_client)
    Common::g_MainNetHost.release();
  if (m_client)
  {
    enet_host_destroy(m_client);
    m_client = nullptr;
  }

  if (m_traversal_client)
    Common::ReleaseTraversalClient();
}

NetPlayClient::NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog,
                             std::string name, const NetTraversalConfig& traversal_config)
    : m_dialog(dialog), m_player_name(std::move(name))
{
  ClearBuffers();

  const auto peer_timeout = static_cast<enet_uint32>(PEER_TIMEOUT.count());

  if (traversal_config.use_traversal)
  {
    if (address.size() > Common::NETPLAY_CODE_SIZE)
    {
      m_dialog->OnConnectionError(
          _trans("The host code is too long.\nPlease recheck that you have the correct code."));
      return;
    }
    if (!Common::EnsureTraversalClient(traversal_config.traversal_host,
                                       traversal_config.traversal_port))
    {
      return;
    }

    m_client = Common::g_MainNetHost.get();
    m_traversal_client = Common::g_TraversalClient.get();
    // The shared client may have lost the server while nobody was using it.
    if (m_traversal_client->HasFailed())
      m_traversal_client->ReconnectToServer();
    m_traversal_client->m_Client = this;
    m_host_spec = address;
    m_link_state = LinkState::AwaitingTraversalServer;
    OnTraversalStateChanged();
    m_connecting = true;

    // The traversal callbacks move m_link_state along; the host's connect event ends the wait.
    Common::Timer waited;
    waited.Start();
    while (m_connecting)
    {
      if (m_traversal_client)
        m_traversal_client->HandleResends();

      ENetEvent event;
      while (enet_host_service(m_client, &event, 4) > 0)
      {
        if (event.type != ENET_EVENT_TYPE_CONNECT)
          continue;

        m_server = event.peer;
        enet_peer_timeout(m_server, 0, peer_timeout, peer_timeout);
        if (Connect())
        {
          m_link_state = LinkState::Connected;
          m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
        }
        return;
      }
      if (waited.ElapsedMs() > 5000)
        break;
    }
    m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    return;
  }

  // Direct connection.
  m_client = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);
  if (m_client == nullptr)
  {
    m_dialog->OnConnectionError(_trans("Could not create client."));
    return;
  }
  m_client->mtu = std::min(m_client->mtu, NetPlay::MAX_ENET_MTU);

  ENetAddress host_address;
  enet_address_set_host(&host_address, address.c_str());
  host_address.port = port;
  m_server = enet_host_connect(m_client, &host_address, CHANNEL_COUNT, 0);
  if (m_server == nullptr)
  {
    m_dialog->OnConnectionError(_trans("Could not create peer."));
    return;
  }
  enet_peer_timeout(m_server, 0, peer_timeout, peer_timeout);

  ENetEvent event;
  const int serviced = enet_host_service(m_client, &event, 5000);
  if (serviced <= 0 || event.type != ENET_EVENT_TYPE_CONNECT)
  {
    m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    return;
  }
  if (Connect())
  {
    m_client->intercept = Common::ENet::InterceptCallback;
    m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
  }
}

// Introduces this player to the host and waits for its verdict.
bool NetPlayClient::Connect()
{
  INFO_LOG_FMT(NETPLAY, "Connecting to server.");

  sf::Packet hello;
  hello << Common::GetScmRevGitStr() << Common::GetNetplayDolphinVer() << m_player_name;
  Send(hello);
  enet_host_flush(m_client);

  // Wakeups and traversal chatter can arrive first; skip them.
  ENetEvent event;
  int serviced;
  do
  {
    serviced = enet_host_service(m_client, &event, 5000);
  } while (serviced > 0 && static_cast<int>(event.type) == Common::ENet::SKIPPABLE_EVENT);

  if (serviced <= 0 || event.type != ENET_EVENT_TYPE_RECEIVE)
    return false;

  sf::Packet reply;
  reply.append(event.packet->data, event.packet->dataLength);
  enet_packet_destroy(event.packet);

  ConnectionError error;
  reply >> error;
  if (error != ConnectionError::NoError)
  {
    const char* reason;
    switch (error)
    {
    case ConnectionError::ServerFull:
      reason = _trans("The server is full.");
      break;
    case ConnectionError::VersionMismatch:
      reason = _trans("The server and client's NetPlay versions are incompatible.");
      break;
    case ConnectionError::GameRunning:
      reason = _trans("The game is currently running.");
      break;
    case ConnectionError::NameTooLong:
      reason = _trans("Nickname is too long.");
      break;
    default:
      reason = _trans("The server sent an unknown error message.");
      break;
    }
    m_dialog->OnConnectionError(reason);
    Disconnect();
    return false;
  }

  reply >> m_pid;

  Player& self = m_players[m_pid];
  self = Player{};
  self.pid = m_pid;
  self.name = m_player_name;
  self.revision = Common::GetNetplayDolphinVer();
  m_local_player = &self;

  m_dialog->Update();
  m_is_connected = true;
  return true;
}

void NetPlayClient::Disconnect()
{
  m_connecting = false;
  m_link_state = LinkState::Failed;
  if (!m_server)
    return;

  enet_peer_disconnect(m_server, 0);

  // Give the host up to three seconds to confirm, dropping anything else it sends meanwhile.
  ENetEvent event;
  while (enet_host_service(m_client, &event, 3000) > 0)
  {
    if (event.type == ENET_EVENT_TYPE_RECEIVE)
    {
      enet_packet_destroy(event.packet);
    }
    else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
    {
      m_server = nullptr;
      return;
    }
  }
  enet_peer_reset(m_server);
  m_server = nullptr;
}

void NetPlayClient::Send(const sf::Packet& packet, const u8 channel_id)
{
  Common::ENet::SendPacket(m_server, packet, channel_id);
}

void NetPlayClient::SendAsync(sf::Packet&& packet, const u8 channel_id)
{
  {
    std::lock_guard lock(m_outgoing_mutex);
    m_outgoing.Push(QueuedPacket{std::move(packet), channel_id});
  }
  Common::ENet::WakeupThread(m_client);
}

void NetPlayClient::DrainOutgoingQueue()
{
  while (!m_outgoing.Empty())
  {
    INFO_LOG_FMT(NETPLAY, "Processing async queue event.");
    const QueuedPacket& next = m_outgoing.Front();
    Send(next.packet, next.channel_id);
    m_outgoing.Pop();
  }
}

// ---- Network thread ----

void NetPlayClient::ThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "NetPlayClient starting.");

  Common::QoSSession qos_session;
  if (Config::Get(Config::NETPLAY_ENABLE_QOS))
  {
    qos_session = Common::QoSSession(m_server);
    m_dialog->AppendChat(
        qos_session.Successful() ?
            Common::GetStringT("Quality of Service (QoS) was successfully enabled.") :
            Common::GetStringT("Quality of Service (QoS) couldn't be enabled."));
  }

  while (m_do_loop)
  {
    if (m_traversal_client)
      m_traversal_client->HandleResends();

    ENetEvent event;
    const int serviced = enet_host_service(m_client, &event, 250);

    // Whatever other threads queued goes out before the event is handled.
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
      INFO_LOG_FMT(NETPLAY, "enet_host_service: connect event");
      break;
    case ENET_EVENT_TYPE_RECEIVE:
    {
      sf::Packet received;
      received.append(event.packet->data, event.packet->dataLength);
      OnData(received);
      enet_packet_destroy(event.packet);
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
      INFO_LOG_FMT(NETPLAY, "enet_host_service: disconnect event");
      m_dialog->OnConnectionLost();
      if (m_is_running)
        StopGame();
      break;
    default:
      // Wakeups use an event type outside the enum.
      if (static_cast<int>(event.type) != Common::ENet::SKIPPABLE_EVENT)
      {
        ERROR_LOG_FMT(NETPLAY, "enet_host_service: unknown event type: {}",
                      static_cast<int>(event.type));
      }
      break;
    }
  }

  INFO_LOG_FMT(NETPLAY, "NetPlayClient shutting down.");
  Disconnect();
}

NetPlayClient::Handler NetPlayClient::LookupHandler(MessageID id)
{
  static constexpr std::array<std::pair<MessageID, Handler>, 33> table{{
      {MessageID::PlayerJoin, &NetPlayClient::HandlePlayerJoin},
      {MessageID::PlayerLeave, &NetPlayClient::HandlePlayerLeave},
      {MessageID::ChatMessage, &NetPlayClient::HandleChat},
      {MessageID::ChunkedDataStart, &NetPlayClient::HandleChunkStart},
      {MessageID::ChunkedDataEnd, &NetPlayClient::HandleChunkEnd},
      {MessageID::ChunkedDataPayload, &NetPlayClient::HandleChunkPayload},
      {MessageID::ChunkedDataAbort, &NetPlayClient::HandleChunkAbort},
      {MessageID::PadMapping, &NetPlayClient::HandlePadMapping},
      {MessageID::GBAConfig, &NetPlayClient::HandleGBAConfig},
      {MessageID::WiimoteMapping, &NetPlayClient::HandleWiimoteMapping},
      {MessageID::PadData, &NetPlayClient::HandlePadData},
      {MessageID::PadHostData, &NetPlayClient::HandlePadHostData},
      {MessageID::WiimoteData, &NetPlayClient::HandleWiimoteData},
      {MessageID::PadBuffer, &NetPlayClient::HandlePadBuffer},
      {MessageID::HostInputAuthority, &NetPlayClient::HandleHostInputAuthority},
      {MessageID::GolfSwitch, &NetPlayClient::HandleGolfSwitch},
      {MessageID::GolfPrepare, &NetPlayClient::HandleGolfPrepare},
      {MessageID::ChangeGame, &NetPlayClient::HandleChangeGame},
      {MessageID::GameStatus, &NetPlayClient::HandleGameStatus},
      {MessageID::StartGame, &NetPlayClient::HandleStartGame},
      {MessageID::StopGame, &NetPlayClient::HandleStopGame},
      {MessageID::DisableGame, &NetPlayClient::HandleStopGame},
      {MessageID::PowerButton, &NetPlayClient::HandlePowerButton},
      {MessageID::Ping, &NetPlayClient::HandlePing},
      {MessageID::PlayerPingData, &NetPlayClient::HandlePlayerPing},
      {MessageID::DesyncDetected, &NetPlayClient::HandleDesync},
      {MessageID::SyncSaveData, &NetPlayClient::HandleSaveSync},
      {MessageID::SyncCodes, &NetPlayClient::HandleCodeSync},
      {MessageID::ComputeGameDigest, &NetPlayClient::HandleDigestRequest},
      {MessageID::GameDigestProgress, &NetPlayClient::HandleDigestProgress},
      // An error string is shown in the same place as a result.
      {MessageID::GameDigestResult, &NetPlayClient::HandleDigestResult},
      {MessageID::GameDigestError, &NetPlayClient::HandleDigestResult},
      {MessageID::GameDigestAbort, &NetPlayClient::HandleDigestAbort},
  }};
  for (const auto& [message, handler] : table)
  {
    if (message == id)
      return handler;
  }
  return nullptr;
}

void NetPlayClient::OnData(sf::Packet& packet)
{
  MessageID id;
  packet >> id;
  INFO_LOG_FMT(NETPLAY, "Got server message: {:x}", static_cast<u8>(id));

  if (const Handler handler = LookupHandler(id))
    (this->*handler)(packet);
  else
    PanicAlertFmtT("Unknown message received with id : {0}", static_cast<u8>(id));
}

// ---- Players and chat ----

void NetPlayClient::HandlePlayerJoin(sf::Packet& packet)
{
  Player joined{};
  packet >> joined.pid >> joined.name >> joined.revision;
  INFO_LOG_FMT(NETPLAY, "Player {} ({}) using {} joined", joined.name, joined.pid, joined.revision);

  {
    std::lock_guard lock(m_players_mutex);
    m_players[joined.pid] = joined;
  }
  m_dialog->OnPlayerConnect(joined.name);
  m_dialog->Update();
}

void NetPlayClient::HandlePlayerLeave(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lock(m_players_mutex);
    const auto found = m_players.find(pid);
    if (found == m_players.end())
      return;
    INFO_LOG_FMT(NETPLAY, "Player {} ({}) left", found->second.name, pid);
    m_dialog->OnPlayerDisconnect(found->second.name);
    m_players.erase(found);
  }
  m_dialog->Update();
}

void NetPlayClient::HandleChat(sf::Packet& packet)
{
  PlayerId pid;
  std::string text;
  packet >> pid >> text;

  // A sender we have not heard of gets an empty entry, as the player list shows.
  const Player& sender = m_players[pid];
  INFO_LOG_FMT(NETPLAY, "Player {} ({}) wrote: {}", sender.name, sender.pid, text);
  m_dialog->AppendChat(fmt::format("{}[{}]: {}", sender.name, pid, text));
}

void NetPlayClient::HandlePlayerPing(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;
  {
    std::lock_guard lock(m_players_mutex);
    packet >> m_players[pid].ping;
  }
  DisplayPlayersPing();
  m_dialog->Update();
}

void NetPlayClient::HandleGameStatus(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;
  {
    std::lock_guard lock(m_players_mutex);
    packet >> m_players[pid].game_status;
  }
  m_dialog->Update();
}

void NetPlayClient::HandlePing(sf::Packet& packet)
{
  u32 key = 0;
  packet >> key;

  sf::Packet pong;
  pong << MessageID::Pong << key;
  Send(pong);
}

void NetPlayClient::HandleDesync(sf::Packet& packet)
{
  int culprit;
  u32 frame;
  packet >> culprit >> frame;

  std::lock_guard lock(m_players_mutex);
  std::string culprit_name = "??";
  if (const auto found = m_players.find(static_cast<PlayerId>(culprit)); found != m_players.end())
    culprit_name = found->second.name;
  INFO_LOG_FMT(NETPLAY, "Player {} ({}) desynced!", culprit_name, culprit);
  m_dialog->OnDesync(frame, culprit_name);
}

void NetPlayClient::DisplayPlayersPing()
{
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;
  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", GetPlayersMaxPing()),
                       OSD::Duration::SHORT, OSD::Color::CYAN);
}

u32 NetPlayClient::GetPlayersMaxPing() const
{
  // The local player is always listed.
  u32 highest = m_players.begin()->second.ping;
  for (const auto& entry : m_players)
    highest = std::max(highest, entry.second.ping);
  return highest;
}

// ---- Chunked transfers ----

void NetPlayClient::HandleChunkStart(sf::Packet& packet)
{
  u32 id;
  std::string title;
  packet >> id >> title;
  const u64 total = Common::PacketReadU64(packet);
  INFO_LOG_FMT(NETPLAY, "Starting data chunk {}.", id);

  m_chunks.try_emplace(id);
  const std::array<int, 1> receivers{m_local_player->pid};
  m_dialog->ShowChunkedProgressDialog(title, total, receivers);
}

void NetPlayClient::HandleChunkPayload(sf::Packet& packet)
{
  u32 id;
  packet >> id;
  const auto found = m_chunks.find(id);
  if (found == m_chunks.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", id);
    return;
  }

  sf::Packet& assembled = found->second;
  for (u8 byte; !packet.endOfPacket();)
  {
    packet >> byte;
    assembled << byte;
  }
  const u64 received = assembled.getDataSize();
  INFO_LOG_FMT(NETPLAY, "Received {} bytes of data chunk {}.", received, id);
  m_dialog->SetChunkedProgress(m_local_player->pid, received);

  sf::Packet progress;
  progress << MessageID::ChunkedDataProgress << id << received;
  Send(progress, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::HandleChunkEnd(sf::Packet& packet)
{
  u32 id;
  packet >> id;
  const auto found = m_chunks.find(id);
  if (found == m_chunks.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", id);
    return;
  }
  INFO_LOG_FMT(NETPLAY, "Ending data chunk {}.", id);

  // The assembled bytes are an ordinary message.
  OnData(found->second);
  m_chunks.erase(id);
  m_dialog->HideChunkedProgressDialog();

  sf::Packet done;
  done << MessageID::ChunkedDataComplete << id;
  Send(done, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::HandleChunkAbort(sf::Packet& packet)
{
  u32 id;
  packet >> id;
  if (m_chunks.erase(id) == 0)
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", id);
    return;
  }
  INFO_LOG_FMT(NETPLAY, "Aborting data chunk {}.", id);
  m_dialog->HideChunkedProgressDialog();
}

// ---- Controller assignment and input from the host ----

void NetPlayClient::HandlePadMapping(sf::Packet& packet)
{
  for (PlayerId& owner : m_net_settings.pad_map)
    packet >> owner;
  m_dialog->Update();
}

void NetPlayClient::HandleWiimoteMapping(sf::Packet& packet)
{
  for (PlayerId& owner : m_net_settings.wiimote_map)
    packet >> owner;
  m_dialog->Update();
}

void NetPlayClient::HandleGBAConfig(sf::Packet& packet)
{
  for (std::size_t port = 0; port < m_net_settings.gba_config.size(); ++port)
  {
    GBAConfig& gba = m_net_settings.gba_config[port];
    const GBAConfig before = gba;

    packet >> gba.enabled >> gba.has_rom >> gba.title;
    for (u8& hash_byte : gba.hash)
      packet >> hash_byte;

    const bool rom_changed = gba.has_rom != before.has_rom || gba.title != before.title ||
                             gba.hash != before.hash;
    if (!rom_changed)
      continue;

    const int device = static_cast<int>(port);
    m_dialog->OnMsgChangeGBARom(device, gba);
    m_net_settings.gba_rom_paths[port] =
        gba.has_rom ? m_dialog->FindGBARomPath(gba.hash, gba.title, device) : "";
  }

  SendGameStatus();
  m_dialog->Update();
}

// PadData and PadHostData share one layout: (port, state) pairs until the end of the packet.
// Ordinary data joins the pad queues; host data only refreshes the latest known state.
void NetPlayClient::ReadPadEntries(sf::Packet& packet, bool from_host)
{
  while (!packet.endOfPacket())
  {
    PadIndex port;
    packet >> port;

    const auto slot = static_cast<std::size_t>(port);
    const bool known_port = slot < m_net_settings.gba_config.size();
    GCPadStatus pad;
    ReadPadState(packet, &pad, !known_port || m_net_settings.gba_config[slot].enabled);

    if (slot >= 4)
      continue;

    if (!from_host)
    {
      m_pad_buffer[slot].Push(pad);
      m_gc_pad_event.Set();
      continue;
    }

    m_last_pad_status[slot] = pad;
    if (!m_first_pad_status_received[slot])
    {
      m_first_pad_status_received[slot] = true;
      m_first_pad_status_received_event.Set();
    }
  }
}

void NetPlayClient::HandlePadData(sf::Packet& packet)
{
  ReadPadEntries(packet, false);
}

void NetPlayClient::HandlePadHostData(sf::Packet& packet)
{
  ReadPadEntries(packet, true);
}

void NetPlayClient::HandleWiimoteData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex port;
    packet >> port;

    WiimoteEmu::SerializedWiimoteState state{};
    packet >> state.length;
    ASSERT(state.length <= state.data.size());
    if (state.length > state.data.size())
      state.length = 0;
    for (u8 i = 0; i < state.length; ++i)
      packet >> state.data[i];

    const auto slot = static_cast<std::size_t>(port);
    if (slot < m_wiimote_buffer.size())
    {
      m_wiimote_buffer[slot].Push(state);
      m_wii_pad_event.Set();
    }
  }
}

void NetPlayClient::HandlePadBuffer(sf::Packet& packet)
{
  u32 size = 0;
  packet >> size;
  if (size > MAX_TARGET_PAD_BUFFER_SIZE)
  {
    WARN_LOG_FMT(NETPLAY, "Ignoring invalid pad buffer size {}.", size);
    return;
  }
  AdjustPadBufferSize(size);
}

void NetPlayClient::HandleHostInputAuthority(sf::Packet& packet)
{
  packet >> m_host_input_authority;
  m_dialog->OnHostInputAuthorityChanged(m_host_input_authority);
}

void NetPlayClient::HandleGolfSwitch(sf::Packet& packet)
{
  PlayerId new_golfer;
  packet >> new_golfer;

  const PlayerId old_golfer = m_current_golfer;
  m_current_golfer = new_golfer;
  const PlayerId self = m_local_player->pid;
  m_dialog->OnGolferChanged(self == new_golfer,
                            new_golfer != 0 ? m_players[new_golfer].name : "");

  if (self == old_golfer)
  {
    sf::Packet release;
    release << MessageID::GolfRelease;
    Send(release);
    return;
  }
  if (self != new_golfer)
    return;

  sf::Packet acquire;
  acquire << MessageID::GolfAcquire;
  Send(acquire);

  // The pads were calibrated before; there is nothing to wait for.
  m_first_pad_status_received.fill(true);
  m_wait_on_input = false;
  m_wait_on_input_event.Set();
}

void NetPlayClient::HandleGolfPrepare(sf::Packet&)
{
  m_wait_on_input_received = true;
  m_wait_on_input = true;
}

// ---- Game selection and control ----

void NetPlayClient::HandleChangeGame(sf::Packet& packet)
{
  std::string netplay_name;
  {
    std::lock_guard lock(m_game_mutex);
    ReadSyncIdentifier(packet, &m_selected_game);
    packet >> netplay_name;
  }
  INFO_LOG_FMT(NETPLAY, "Game changed to {}", netplay_name);
  m_dialog->OnMsgChangeGame(m_selected_game, netplay_name);

  SendGameStatus();

  sf::Packet capabilities;
  capabilities << MessageID::ClientCapabilities << ExpansionInterface::CEXIIPL::HasIPLDump()
               << Config::Get(Config::SESSION_USE_FMA);
  Send(capabilities);
}

void NetPlayClient::HandleStartGame(sf::Packet& packet)
{
  {
    std::lock_guard lock(m_game_mutex);
    INFO_LOG_FMT(NETPLAY, "Start of game {}", m_selected_game.game_id);

    packet >> m_current_game;
    u64 initial_rtc = 0;
    VisitStartGameSettings(m_net_settings, initial_rtc, m_net_settings.save_data_region,
                           [&packet](auto& value) {
                             using Value = std::remove_cvref_t<decltype(value)>;
                             if constexpr (std::is_same_v<Value, u64>)
                               value = Common::PacketReadU64(packet);
                             else
                               packet >> value;
                           });
    m_initial_rtc = initial_rtc;

    // Writing or syncing all Wii saves only makes sense on top of loading them.
    if (!m_net_settings.savedata_load)
    {
      m_net_settings.savedata_write = false;
      m_net_settings.savedata_sync_all_wii = false;
    }
    m_net_settings.is_hosting = m_local_player->IsHost();
  }
  m_dialog->OnMsgStartGame();
}

void NetPlayClient::HandleStopGame(sf::Packet&)
{
  INFO_LOG_FMT(NETPLAY, "Game stopped");
  StopGame();
  m_dialog->OnMsgStopGame();
}

void NetPlayClient::HandlePowerButton(sf::Packet&)
{
  InvokeStop();
  m_dialog->OnMsgPowerButton();
}

// ---- Save data sync ----

void NetPlayClient::HandleSaveSync(sf::Packet& packet)
{
  SyncSaveDataID kind;
  packet >> kind;

  // The host already has its own saves.
  if (m_local_player->IsHost())
    return;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncSaveData sub id: {}", static_cast<u8>(kind));
  switch (kind)
  {
  case SyncSaveDataID::Notify:
    ReceiveSaveCount(packet);
    return;
  case SyncSaveDataID::RawData:
    ReceiveRawMemcard(packet);
    return;
  case SyncSaveDataID::GCIData:
    ReceiveGCIFolder(packet);
    return;
  case SyncSaveDataID::WiiData:
    ReceiveWiiSaves(packet);
    return;
  case SyncSaveDataID::GBAData:
    ReceiveGBASave(packet);
    return;
  default:
    PanicAlertFmtT("Unknown SYNC_SAVE_DATA message received with id: {0}", static_cast<u8>(kind));
    return;
  }
}

void NetPlayClient::ReceiveSaveCount(sf::Packet& packet)
{
  packet >> m_saves_expected;
  m_saves_received = 0;
  INFO_LOG_FMT(NETPLAY, "Initializing wait for {} savegame chunks.", m_saves_expected);

  if (m_saves_expected == 0)
    ReportSaveSync(true);
  else
    m_dialog->AppendChat(Common::GetStringT("Synchronizing save data..."));
}

void NetPlayClient::ReceiveRawMemcard(sf::Packet& packet)
{
  bool slot_a = false;
  std::string region;
  int size_override = 0;
  packet >> slot_a >> region >> size_override;
  const char slot_letter = slot_a ? 'A' : 'B';
  INFO_LOG_FMT(NETPLAY, "Received raw memcard data for slot {}: region {}, size override {}.",
               slot_letter, region, size_override);

  // The region becomes part of a path, so only the three known names are accepted.
  if (region != JAP_DIR && region != USA_DIR && region != EUR_DIR)
  {
    WARN_LOG_FMT(NETPLAY, "Received invalid raw memory card region.");
    ReportSaveSync(false);
    return;
  }

  std::string blocks_suffix;
  if (size_override >= 0 && size_override <= 4)
  {
    const auto mbits = static_cast<u16>(Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_override);
    blocks_suffix = fmt::format(".{}", Memcard::MbitToFreeBlocks(mbits));
  }

  const std::string path = fmt::format("{}{}{}.{}{}.raw", File::GetUserPath(D_GCUSER_IDX),
                                       GC_MEMCARD_NETPLAY, slot_letter, region, blocks_suffix);
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay memory card. Verify your write permissions.");
    ReportSaveSync(false);
    return;
  }
  ReportSaveSync(DecompressPacketIntoFile(packet, path));
}

void NetPlayClient::ReceiveGCIFolder(sf::Packet& packet)
{
  bool slot_a = false;
  u8 file_count = 0;
  packet >> slot_a >> file_count;
  const char slot_letter = slot_a ? 'A' : 'B';

  const std::string folder = fmt::format("{}{}{}Card {}", File::GetUserPath(D_GCUSER_IDX),
                                         GC_MEMCARD_NETPLAY, DIR_SEP, slot_letter);
  INFO_LOG_FMT(NETPLAY, "Received GCI memcard data for slot {}: {}, {} files.", slot_letter, folder,
               file_count);

  const bool cleared = !File::Exists(folder) || File::DeleteDirRecursively(folder + DIR_SEP);
  if (!cleared || !File::CreateFullPath(folder + DIR_SEP))
  {
    PanicAlertFmtT("Failed to reset NetPlay GCI folder. Verify your write permissions.");
    ReportSaveSync(false);
    return;
  }

  for (unsigned int n = 0; n < file_count; ++n)
  {
    std::string name;
    packet >> name;
    INFO_LOG_FMT(NETPLAY, "Received GCI: {}", name);

    if (!Common::IsFileNameSafe(name) || !DecompressPacketIntoFile(packet, folder + DIR_SEP + name))
    {
      WARN_LOG_FMT(NETPLAY, "Received invalid GCI.");
      ReportSaveSync(false);
      return;
    }
  }
  ReportSaveSync(true);
}

void NetPlayClient::ReceiveWiiSaves(sf::Packet& packet)
{
  const std::string nand_root = SyncedWiiRoot();
  std::string redirect_root = SyncedRedirectRoot();

  if (File::Exists(nand_root) && !File::DeleteDirRecursively(nand_root))
  {
    PanicAlertFmtT("Failed to reset NetPlay NAND folder. Verify your write permissions.");
    ReportSaveSync(false);
    return;
  }
  if (File::Exists(redirect_root) && !File::DeleteDirRecursively(redirect_root))
  {
    PanicAlertFmtT("Failed to reset NetPlay redirect folder. Verify your write permissions.");
    ReportSaveSync(false);
    return;
  }

  auto nand = std::make_unique<IOS::HLE::FS::HostFileSystem>(nand_root);
  constexpr IOS::HLE::FS::Modes everyone_rw{IOS::HLE::FS::Mode::ReadWrite,
                                            IOS::HLE::FS::Mode::ReadWrite,
                                            IOS::HLE::FS::Mode::ReadWrite};
  constexpr auto kernel = IOS::PID_KERNEL;

  // The Mii database comes first.
  bool has_miis = false;
  packet >> has_miis;
  if (has_miis)
  {
    INFO_LOG_FMT(NETPLAY, "Received Mii data.");
    const std::optional<std::vector<u8>> miis = DecompressPacketIntoBuffer(packet);
    nand->CreateFullPath(kernel, kernel, "/shared2/menu/FaceLib/", 0, everyone_rw);
    const auto mii_file =
        nand->CreateAndOpenFile(kernel, kernel, Common::GetMiiDatabasePath(), everyone_rw);
    if (!miis || !mii_file || !mii_file->Write(miis->data(), miis->size()))
    {
      PanicAlertFmtT("Failed to write Mii data.");
      ReportSaveSync(false);
      return;
    }
  }

  // Then the saves, each a title ID, an exists flag and the save's headers and files.
  u32 title_count = 0;
  packet >> title_count;
  INFO_LOG_FMT(NETPLAY, "Received data for {} Wii saves.", title_count);

  std::vector<u64> titles;
  for (u32 n = 0; n < title_count; ++n)
  {
    const u64 title_id = Common::PacketReadU64(packet);
    titles.push_back(title_id);
    nand->CreateFullPath(kernel, kernel, Common::GetTitleDataPath(title_id) + '/', 0, everyone_rw);
    const WiiSave::StoragePointer storage = WiiSave::MakeNandStorage(nand.get(), title_id);

    bool present = false;
    packet >> present;
    if (!present)
    {
      INFO_LOG_FMT(NETPLAY, "No data for Wii save of title {:016x}.", title_id);
      continue;
    }
    INFO_LOG_FMT(NETPLAY, "Received Wii save of title {:016x}.", title_id);

    WiiSave::Header header{};
    packet >> header.tid >> header.banner_size;
    if (!packet || header.banner_size > sizeof(header.banner))
    {
      WARN_LOG_FMT(NETPLAY, "Received invalid Wii save banner size.");
      ReportSaveSync(false);
      return;
    }
    packet >> header.permissions >> header.unk1;
    for (u8& md5_byte : header.md5)
      packet >> md5_byte;
    packet >> header.unk2;
    const u32 banner_size = header.banner_size;
    for (u32 offset = 0; offset < banner_size; ++offset)
      packet >> header.banner[offset];

    WiiSave::BkHeader bk_header{};
    packet >> bk_header.size >> bk_header.magic >> bk_header.ngid >> bk_header.number_of_files >>
        bk_header.size_of_files >> bk_header.unk1 >> bk_header.unk2 >> bk_header.total_size;
    for (u8& unk3_byte : bk_header.unk3)
      packet >> unk3_byte;
    packet >> bk_header.tid;
    for (u8& mac_byte : bk_header.mac_address)
      packet >> mac_byte;

    std::vector<WiiSave::Storage::SaveFile> entries;
    const u32 entry_count = bk_header.number_of_files;
    for (u32 e = 0; e < entry_count; ++e)
    {
      WiiSave::Storage::SaveFile& entry = entries.emplace_back();
      packet >> entry.mode >> entry.attributes >> entry.type >> entry.path;
      INFO_LOG_FMT(NETPLAY, "Received Wii save data of type {} at {}", static_cast<u8>(entry.type),
                   entry.path);

      if (entry.type != WiiSave::Storage::SaveFile::Type::File)
        continue;
      std::optional<std::vector<u8>> contents = DecompressPacketIntoBuffer(packet);
      if (!contents)
      {
        ReportSaveSync(false);
        return;
      }
      entry.data = std::move(*contents);
    }

    if (!storage->WriteHeader(header) || !storage->WriteBkHeader(bk_header) ||
        !storage->WriteFiles(entries))
    {
      PanicAlertFmtT("Failed to write Wii save.");
      ReportSaveSync(false);
      return;
    }
  }

  // Last, a Riivolution redirect folder if the game uses one.
  bool has_redirect = false;
  packet >> has_redirect;
  if (has_redirect)
  {
    INFO_LOG_FMT(NETPLAY, "Received redirected save.");
    if (!DecompressPacketIntoFolder(packet, redirect_root))
    {
      PanicAlertFmtT("Failed to write redirected save.");
      ReportSaveSync(false);
      return;
    }
  }

  SetWiiSyncData(std::move(nand), std::move(titles), std::move(redirect_root));
  ReportSaveSync(true);
}

void NetPlayClient::ReceiveGBASave(sf::Packet& packet)
{
  u8 port = 0;
  packet >> port;
  INFO_LOG_FMT(NETPLAY, "Received GBA save for slot {}.", port);

  const int device_number = port + 1;
  const std::string path = fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX),
                                       GBA_SAVE_NETPLAY, device_number);
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay GBA{0} save file. Verify your write permissions.",
                   device_number);
    ReportSaveSync(false);
    return;
  }
  ReportSaveSync(DecompressPacketIntoFile(packet, path));
}

void NetPlayClient::ReportSaveSync(const bool success)
{
  m_dialog->AppendChat(success ? Common::GetStringT("Data received!") :
                                 Common::GetStringT("Error processing data."));

  // Success is reported once every expected save has arrived; failure right away.
  if (success && ++m_saves_received < m_saves_expected)
    return;

  sf::Packet reply;
  reply << MessageID::SyncSaveData << (success ? SyncSaveDataID::Success : SyncSaveDataID::Failure);
  Send(reply);
}

// ---- Cheat code sync ----

void NetPlayClient::HandleCodeSync(sf::Packet& packet)
{
  SyncCodeID kind;
  packet >> kind;
  INFO_LOG_FMT(NETPLAY, "Processing OnSyncCodes sub id: {}", static_cast<u8>(kind));

  switch (kind)
  {
  case SyncCodeID::Notify:
    // A new sync begins; neither list has arrived yet.
    m_gecko_synced = false;
    m_ar_synced = false;
    return;
  case SyncCodeID::NotifyGecko:
    ReceiveGeckoCount(packet);
    return;
  case SyncCodeID::GeckoData:
    ReceiveGeckoCodes(packet);
    return;
  case SyncCodeID::NotifyAR:
    ReceiveARCount(packet);
    return;
  case SyncCodeID::ARData:
    ReceiveARCodes(packet);
    return;
  default:
    PanicAlertFmtT("Unknown SYNC_CODES message received with id: {0}", static_cast<u8>(kind));
    return;
  }
}

void NetPlayClient::ReceiveGeckoCount(sf::Packet& packet)
{
  if (m_local_player->IsHost())
    return;

  packet >> m_gecko_lines_expected;
  m_gecko_lines_received = 0;
  INFO_LOG_FMT(NETPLAY, "Receiving {} Gecko codelines", m_gecko_lines_expected);

  if (m_gecko_lines_expected != 0)
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing Gecko codes..."));
    return;
  }
  m_gecko_synced = true;
  ReportCodeSync(true);
}

void NetPlayClient::ReceiveGeckoCodes(sf::Packet& packet)
{
  if (m_local_player->IsHost())
    return;

  // All received lines go into one enabled code.
  Gecko::GeckoCode combined{};
  combined.name = "Synced Codes";
  combined.enabled = true;

  const u32 expected = m_gecko_lines_expected;
  for (u32 n = 0; n < expected; ++n)
  {
    Gecko::GeckoCode::Code& line = combined.codes.emplace_back();
    packet >> line.address >> line.data;
    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", line.address, line.data);

    if (++m_gecko_lines_received >= m_gecko_lines_expected)
    {
      m_gecko_synced = true;
      ReportCodeSync(true);
    }
  }

  // No lines means no codes at all, as on the host.
  std::vector<Gecko::GeckoCode> synced;
  if (expected != 0)
    synced.push_back(std::move(combined));
  Gecko::UpdateSyncedCodes(synced);
}

void NetPlayClient::ReceiveARCount(sf::Packet& packet)
{
  if (m_local_player->IsHost())
    return;

  packet >> m_ar_lines_expected;
  m_ar_lines_received = 0;
  INFO_LOG_FMT(NETPLAY, "Receiving {} AR codelines", m_ar_lines_expected);

  if (m_ar_lines_expected != 0)
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing AR codes..."));
    return;
  }
  m_ar_synced = true;
  ReportCodeSync(true);
}

void NetPlayClient::ReceiveARCodes(sf::Packet& packet)
{
  if (m_local_player->IsHost())
    return;

  ActionReplay::ARCode combined{};
  combined.name = "Synced Codes";
  combined.enabled = true;

  const u32 expected = m_ar_lines_expected;
  for (u32 n = 0; n < expected; ++n)
  {
    ActionReplay::AREntry& op = combined.ops.emplace_back();
    packet >> op.cmd_addr >> op.value;
    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", op.cmd_addr, op.value);

    if (++m_ar_lines_received >= m_ar_lines_expected)
    {
      m_ar_synced = true;
      ReportCodeSync(true);
    }
  }

  std::vector<ActionReplay::ARCode> synced;
  if (expected != 0)
    synced.push_back(std::move(combined));
  ActionReplay::UpdateSyncedCodes(synced);
}

void NetPlayClient::ReportCodeSync(const bool success)
{
  sf::Packet reply;
  reply << MessageID::SyncCodes;
  if (!success)
  {
    m_dialog->AppendChat(Common::GetStringT("Error processing codes."));
    reply << SyncCodeID::Failure;
    Send(reply);
    return;
  }

  // Success waits for both lists.
  if (!m_gecko_synced || !m_ar_synced)
    return;
  m_dialog->AppendChat(Common::GetStringT("Codes received!"));
  reply << SyncCodeID::Success;
  Send(reply);
}

// ---- Game digests ----

void NetPlayClient::HandleDigestRequest(sf::Packet& packet)
{
  SyncIdentifier identifier;
  ReadSyncIdentifier(packet, &identifier);
  ComputeGameDigest(identifier);
}

void NetPlayClient::HandleDigestProgress(sf::Packet& packet)
{
  PlayerId pid;
  int percent;
  packet >> pid >> percent;
  m_dialog->SetGameDigestProgress(pid, percent);
}

void NetPlayClient::HandleDigestResult(sf::Packet& packet)
{
  PlayerId pid;
  std::string text;
  packet >> pid >> text;
  m_dialog->SetGameDigestResult(pid, text);
}

void NetPlayClient::HandleDigestAbort(sf::Packet&)
{
  m_should_compute_game_digest = false;
  m_dialog->AbortGameDigest();
}

void NetPlayClient::ComputeGameDigest(const SyncIdentifier& sync_identifier)
{
  if (m_should_compute_game_digest)
    return;

  m_dialog->ShowGameDigestDialog(sync_identifier.game_id);
  m_should_compute_game_digest = true;

  std::string path;
  if (sync_identifier == GetSDCardIdentifier())
    path = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  else if (const auto game = m_dialog->FindGameFile(sync_identifier))
    path = game->GetFilePath();

  if (path.empty() || !File::Exists(path))
  {
    sf::Packet missing;
    missing << MessageID::GameDigestError << "file not found";
    Send(missing);
    return;
  }

  if (m_game_digest_thread.joinable())
    m_game_digest_thread.join();
  m_game_digest_thread = std::thread([this, path] {
    const std::string digest = HashFileForDigest(path, [this](int percent) {
      sf::Packet progress;
      progress << MessageID::GameDigestProgress << percent;
      SendAsync(std::move(progress));
      return m_should_compute_game_digest;
    });

    sf::Packet result;
    result << MessageID::GameDigestResult << digest;
    SendAsync(std::move(result));
  });
}

// ---- GUI thread requests ----

std::vector<const Player*> NetPlayClient::GetPlayers()
{
  std::lock_guard lock(m_players_mutex);
  std::vector<const Player*> list;
  list.reserve(m_players.size());
  for (const auto& entry : m_players)
    list.push_back(&entry.second);
  return list;
}

const NetSettings& NetPlayClient::GetNetSettings() const
{
  return m_net_settings;
}

void NetPlayClient::SendChatMessage(const std::string& msg)
{
  sf::Packet chat;
  chat << MessageID::ChatMessage << msg;
  SendAsync(std::move(chat));
}

void NetPlayClient::SendStartGamePacket()
{
  sf::Packet start;
  start << MessageID::StartGame << m_current_game;
  SendAsync(std::move(start));
}

void NetPlayClient::SendStopGamePacket()
{
  sf::Packet stop;
  stop << MessageID::StopGame;
  SendAsync(std::move(stop));
}

bool NetPlayClient::StartGame(const std::string& path)
{
  std::lock_guard lock(m_game_mutex);
  // The host learns which game number this client runs even when it refuses to start twice.
  SendStartGamePacket();

  if (m_is_running)
  {
    PanicAlertFmtT("Game is already running!");
    return false;
  }

  m_timebase_frame = 0;
  m_current_golfer = 1;
  m_wait_on_input = false;

  m_is_running = true;
  NetPlay_Enable(this);
  ClearBuffers();
  m_first_pad_status_received.fill(false);

  if (m_dialog->IsRecording())
  {
    auto& movie = Core::System::GetInstance().GetMovie();
    if (movie.IsReadOnly())
      movie.SetReadOnly(false);

    Movie::ControllerTypeArray controllers{};
    Movie::WiimoteEnabledArray wiimotes{};
    for (std::size_t port = 0; port < 4; ++port)
    {
      const bool gc_owned = m_net_settings.pad_map[port] > 0;
      if (!gc_owned)
        controllers[port] = Movie::ControllerType::None;
      else if (m_net_settings.gba_config[port].enabled)
        controllers[port] = Movie::ControllerType::GBA;
      else
        controllers[port] = Movie::ControllerType::GC;
      wiimotes[port] = m_net_settings.wiimote_map[port] > 0;
    }
    movie.BeginRecordingInput(controllers, wiimotes);
  }

  auto session = std::make_unique<BootSessionData>();
  INFO_LOG_FMT(NETPLAY,
               "Setting Wii sync data: has FS {}, sync_titles = {:016x}, redirect folder = {}",
               m_wii_sync_fs != nullptr, fmt::join(m_wii_sync_titles, ", "),
               m_wii_sync_redirect_folder);
  // When emulation ends, the synced NAND and redirect folders go away again.
  session->SetWiiSyncData(std::move(m_wii_sync_fs), std::move(m_wii_sync_titles),
                          std::move(m_wii_sync_redirect_folder), [] {
                            RemoveFolderIfPresent(SyncedWiiRoot());
                            RemoveFolderIfPresent(SyncedRedirectRoot());
                          });

  m_net_settings.local_player_id = m_local_player->pid;
  session->SetNetplaySettings(std::make_unique<NetPlay::NetSettings>(m_net_settings));

  m_dialog->BootGame(path, std::move(session));
  return true;
}

bool NetPlayClient::ChangeGame(const std::string&)
{
  return true;
}

void NetPlayClient::ClearBuffers()
{
  // SPSCQueue::Clear is not safe against the other side, so pop one by one.
  for (std::size_t port = 0; port < 4; ++port)
  {
    while (m_pad_buffer[port].Size() != 0)
      m_pad_buffer[port].Pop();
    while (m_wiimote_buffer[port].Size() != 0)
      m_wiimote_buffer[port].Pop();
  }
}

void NetPlayClient::InvokeStop()
{
  m_is_running = false;

  // Release every thread that waits for input.
  m_gc_pad_event.Set();
  m_wii_pad_event.Set();
  m_first_pad_status_received_event.Set();
  m_wait_on_input_event.Set();
}

bool NetPlayClient::StopGame()
{
  InvokeStop();
  NetPlay_Disable();
  m_dialog->StopGame();
  return true;
}

void NetPlayClient::Stop()
{
  if (!m_is_running)
    return;

  InvokeStop();
  // A player with controllers asks the host to stop everyone; a spectator just stops.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
  else
    StopGame();
}

void NetPlayClient::RequestStopGame()
{
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
}

void NetPlayClient::SendPowerButtonEvent()
{
  sf::Packet power;
  power << MessageID::PowerButton;
  SendAsync(std::move(power));
}

void NetPlayClient::RequestGolfControl(const PlayerId pid)
{
  if (!m_host_input_authority || !m_net_settings.golf_mode)
    return;

  sf::Packet request;
  request << MessageID::GolfRequest << pid;
  SendAsync(std::move(request));
}

void NetPlayClient::RequestGolfControl()
{
  RequestGolfControl(m_local_player->pid);
}

std::string NetPlayClient::GetCurrentGolfer()
{
  std::lock_guard lock(m_players_mutex);
  const auto found = m_players.find(m_current_golfer);
  return found != m_players.end() ? found->second.name : "";
}

void NetPlayClient::SendGameStatus()
{
  SyncIdentifierComparison status;
  m_dialog->FindGameFile(m_selected_game, &status);

  // A GBA whose ROM this player lacks counts as a missing game.
  for (std::size_t port = 0; port < 4; ++port)
  {
    const GBAConfig& gba = m_net_settings.gba_config[port];
    if (gba.enabled && gba.has_rom && m_net_settings.gba_rom_paths[port].empty())
      status = SyncIdentifierComparison::DifferentGame;
  }

  sf::Packet report;
  report << MessageID::GameStatus << status;
  Send(report);
}

void NetPlayClient::SendTimeBase()
{
  std::lock_guard lock(s_active_client_mutex);
  NetPlayClient* const client = s_active_client;

  // Once a second at 60 frames per second.
  if (client->m_timebase_frame % 60 == 0)
  {
    const u64 timebase = Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();
    sf::Packet sample;
    sample << MessageID::TimeBase << timebase << client->m_timebase_frame;
    client->SendAsync(std::move(sample));
  }
  ++client->m_timebase_frame;
}

bool NetPlayClient::DoAllPlayersHaveGame()
{
  std::lock_guard lock(m_players_mutex);
  for (const auto& entry : m_players)
  {
    if (entry.second.game_status != SyncIdentifierComparison::SameGame)
      return false;
  }
  return true;
}

// ---- Traversal callbacks (network thread) ----

void NetPlayClient::OnTraversalStateChanged()
{
  const Common::TraversalClient::State state = m_traversal_client->GetState();

  if (m_link_state == LinkState::AwaitingTraversalServer &&
      state == Common::TraversalClient::State::Connected)
  {
    m_link_state = LinkState::AwaitingHostAddress;
    m_traversal_client->ConnectToClient(m_host_spec);
  }
  else if (m_link_state != LinkState::Failed && state == Common::TraversalClient::State::Failure)
  {
    Disconnect();
    m_dialog->OnTraversalError(m_traversal_client->GetFailureReason());
  }
  m_dialog->OnTraversalStateChanged(state);
}

void NetPlayClient::OnConnectReady(ENetAddress addr)
{
  if (m_link_state != LinkState::AwaitingHostAddress)
    return;
  m_link_state = LinkState::Connecting;
  enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);
}

void NetPlayClient::OnConnectFailed(Common::TraversalConnectFailedReason reason)
{
  m_connecting = false;
  m_link_state = LinkState::Failed;

  using Reason = Common::TraversalConnectFailedReason;
  if (reason == Reason::ClientDidntRespond)
    PanicAlertFmtT("Traversal server timed out connecting to the host");
  else if (reason == Reason::ClientFailure)
    PanicAlertFmtT("Server rejected traversal attempt");
  else if (reason == Reason::NoSuchClient)
    PanicAlertFmtT("Invalid host");
  else
    PanicAlertFmtT("Unknown error {0:x}", static_cast<int>(reason));
}

// ---- Input exchange (CPU thread) ----
//
// Every machine sees the same four in-game ports; the host's pad map says which player owns each.
// A player's own controllers are numbered from 0 in port order ("local pads"), so a player owning
// ports 2 and 4 plays them with local pads 0 and 1.
//
// A batched poll (from VI) of the first owned port reads all local pads at once and sends them;
// an unbatched poll (MMIO) reads and sends just the requested one.

void NetPlayClient::AddPadStateToPacket(const int in_game_pad, const GCPadStatus& pad,
                                        sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  WritePadState(packet, pad,
                m_net_settings.gba_config[static_cast<std::size_t>(in_game_pad)].enabled);
}

void NetPlayClient::AddWiimoteStateToPacket(const int in_game_pad,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad) << state.length;
  for (u8 i = 0; i < state.length; ++i)
    packet << state.data[i];
}

bool NetPlayClient::GetNetPads(const int pad_nb, const bool batching, GCPadStatus* pad_status)
{
  // Parked during a golf handover until the new golfer takes over.
  while (m_wait_on_input)
  {
    if (!m_is_running)
      return false;
    if (m_wait_on_input_received)
    {
      sf::Packet ack;
      ack << MessageID::GolfPrepare;
      Send(ack);
      m_wait_on_input_received = false;
    }
    m_wait_on_input_event.Wait();
  }

  if (batching && IsFirstInGamePad(pad_nb))
  {
    sf::Packet pads;
    pads << MessageID::PadData;
    bool any_added = false;
    for (int local_pad = 0, count = NumLocalPads(); local_pad < count; ++local_pad)
    {
      if (PollLocalPad(local_pad, pads))
        any_added = true;
    }
    if (any_added)
      SendAsync(std::move(pads));
    if (m_host_input_authority)
      SendPadHostPoll(-1);
  }

  if (!batching)
  {
    const int local_pad = InGamePadToLocalPad(pad_nb);
    if (local_pad < NO_LOCAL_PAD)
    {
      sf::Packet pads;
      pads << MessageID::PadData;
      if (PollLocalPad(local_pad, pads))
        SendAsync(std::move(pads));
    }
    if (m_host_input_authority)
      SendPadHostPoll(static_cast<PadIndex>(pad_nb));
  }

  const auto port = static_cast<std::size_t>(pad_nb);
  if (m_host_input_authority)
  {
    float speed = 1.0f;
    bool set_speed = true;
    if (m_local_player->pid != m_current_golfer)
    {
      // Run unthrottled while too many states are queued, but only once that has lasted a second:
      // switching speed back and forth quickly looks like frame skipping.
      const auto now = std::chrono::steady_clock::now();
      const bool backlog = m_pad_buffer[port].Size() > m_target_buffer_size + 1;
      if (!backlog)
        m_buffer_under_target_last = now;
      const std::chrono::duration<double> backlog_time = now - m_buffer_under_target_last;
      set_speed = !backlog || backlog_time.count() >= 1.0;
      speed = backlog ? 0.0f : 1.0f;
    }
    // The golfer always runs at normal speed, or it could stay unthrottled.
    if (set_speed)
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, speed);
  }

  // Wait for this port's next state, from this machine or another.
  while (m_pad_buffer[port].Size() == 0)
  {
    if (!m_is_running)
      return false;
    m_gc_pad_event.Wait();
  }
  m_pad_buffer[port].Pop(*pad_status);

  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsRecordingInput())
  {
    movie.RecordInput(pad_status, pad_nb);
    movie.InputUpdate();
  }
  else
  {
    movie.CheckPadStatus(pad_status, pad_nb);
  }
  return true;
}

bool NetPlayClient::PollLocalPad(const int local_pad, sf::Packet& packet)
{
  const int in_game_pad = LocalPadToInGamePad(local_pad);
  const auto port = static_cast<std::size_t>(in_game_pad);

  GCPadStatus status;
  if (m_net_settings.gba_config[port].enabled)
    status = Pad::GetGBAStatus(local_pad);
  else if (Config::Get(Config::GetInfoForSIDevice(local_pad)) ==
           SerialInterface::SIDEVICE_WIIU_ADAPTER)
    status = GCAdapter::Input(local_pad);
  else
    status = Pad::GetStatus(local_pad);

  if (m_host_input_authority)
  {
    // Non-golfers send to the host; the golfer keeps its own state for SendPadHostPoll.
    if (m_local_player->pid == m_current_golfer)
    {
      m_last_pad_status[port] = status;
      m_first_pad_status_received[port] = true;
      return false;
    }
    AddPadStateToPacket(in_game_pad, status, packet);
    return true;
  }

  // Top the queue up to the target size, sending each added state, so the lead over the other
  // players settles at the buffer size.
  bool added = false;
  while (m_pad_buffer[port].Size() <= m_target_buffer_size)
  {
    m_pad_buffer[port].Push(status);
    AddPadStateToPacket(in_game_pad, status, packet);
    added = true;
  }
  return added;
}

bool NetPlayClient::AddLocalWiimoteToBuffer(const int local_wiimote,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  const int in_game_wiimote = LocalWiimoteToInGameWiimote(local_wiimote);
  auto& queue = m_wiimote_buffer[static_cast<std::size_t>(in_game_wiimote)];
  bool added = false;
  while (queue.Size() <= m_target_buffer_size)
  {
    queue.Push(state);
    AddWiimoteStateToPacket(in_game_wiimote, state, packet);
    added = true;
  }
  return added;
}

// With host input authority, the golfer feeds every owned port from the newest state it has seen
// (all ports for a batched poll, pad_num < 0; one port otherwise) and sends those states out. A
// port whose queue still holds a state is skipped so no lasting latency builds up, and nothing
// is sent before a real state has arrived, or pads would calibrate on the defaults.
void NetPlayClient::SendPadHostPoll(const PadIndex pad_num)
{
  if (m_local_player->pid != m_current_golfer)
    return;

  const auto wait_for_first_state = [this](std::size_t port) {
    while (!m_first_pad_status_received[port])
    {
      if (!m_is_running)
        return false;
      m_first_pad_status_received_event.Wait();
    }
    return true;
  };
  const auto feed = [this](std::size_t port, sf::Packet& packet) {
    if (m_pad_buffer[port].Size() != 0)
      return;
    const GCPadStatus& latest = m_last_pad_status[port];
    m_pad_buffer[port].Push(latest);
    AddPadStateToPacket(static_cast<int>(port), latest, packet);
  };

  sf::Packet states;
  states << MessageID::PadHostData;

  const PadMappingArray& owners = m_net_settings.pad_map;
  if (pad_num < 0)
  {
    for (std::size_t port = 0; port < owners.size(); ++port)
    {
      if (owners[port] != 0 && !wait_for_first_state(port))
        return;
    }
    for (std::size_t port = 0; port < owners.size(); ++port)
    {
      if (owners[port] != 0)
        feed(port, states);
    }
  }
  else
  {
    const auto port = static_cast<std::size_t>(pad_num);
    if (owners[port] != 0)
    {
      if (!wait_for_first_state(port))
        return;
      feed(port, states);
    }
  }

  SendAsync(std::move(states));
}

u64 NetPlayClient::GetInitialRTCValue() const
{
  return m_initial_rtc;
}

bool NetPlayClient::WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries)
{
  for (const WiimoteDataBatchEntry& entry : entries)
  {
    const int local_wiimote = InGameWiimoteToLocalWiimote(entry.wiimote);
    DEBUG_LOG_FMT(NETPLAY,
                  "Entering WiimoteUpdate() with wiimote {}, local_wiimote {}, state [{:02x}]",
                  entry.wiimote, local_wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));

    if (local_wiimote < NO_LOCAL_PAD)
    {
      sf::Packet states;
      states << MessageID::WiimoteData;
      if (AddLocalWiimoteToBuffer(local_wiimote, *entry.state, states))
        SendAsync(std::move(states));
    }

    auto& queue = m_wiimote_buffer[static_cast<std::size_t>(entry.wiimote)];
    while (queue.Size() == 0)
    {
      if (!m_is_running)
        return false;
      m_wii_pad_event.Wait();
    }
    queue.Pop(*entry.state);

    DEBUG_LOG_FMT(NETPLAY, "Exiting WiimoteUpdate() with wiimote {}, state [{:02x}]", entry.wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
  }
  return true;
}

// ---- Mapping queries ----

bool NetPlayClient::LocalPlayerHasControllerMapped() const
{
  return PlayerHasControllerMapped(m_local_player->pid);
}

bool NetPlayClient::IsFirstInGamePad(int ingame_pad) const
{
  const auto& owners = m_net_settings.pad_map;
  return std::all_of(owners.begin(), owners.begin() + ingame_pad,
                     [](PlayerId owner) { return owner == 0; });
}

int NetPlayClient::NumLocalPads() const
{
  const auto& owners = m_net_settings.pad_map;
  return static_cast<int>(std::count(owners.begin(), owners.end(), m_local_player->pid));
}

int NetPlayClient::NumLocalWiimotes() const
{
  const auto& owners = m_net_settings.wiimote_map;
  return static_cast<int>(std::count(owners.begin(), owners.end(), m_local_player->pid));
}

int NetPlayClient::InGamePadToLocalPad(int ingame_pad) const
{
  return RankOfPort(ingame_pad, m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::LocalPadToInGamePad(int local_pad) const
{
  return NthPortOf(local_pad, m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::InGameWiimoteToLocalWiimote(int ingame_wiimote) const
{
  return RankOfPort(ingame_wiimote, m_net_settings.wiimote_map, m_local_player->pid);
}

int NetPlayClient::LocalWiimoteToInGameWiimote(int local_wiimote) const
{
  return NthPortOf(local_wiimote, m_net_settings.wiimote_map, m_local_player->pid);
}

bool NetPlayClient::PlayerHasControllerMapped(const PlayerId pid) const
{
  const auto owns = [pid](const PadMappingArray& owners) {
    return std::find(owners.begin(), owners.end(), pid) != owners.end();
  };
  return owns(m_net_settings.pad_map) || owns(m_net_settings.wiimote_map);
}

bool NetPlayClient::IsLocalPlayer(const PlayerId pid) const
{
  return pid == m_local_player->pid;
}

const PlayerId& NetPlayClient::GetLocalPlayerId() const
{
  return m_local_player->pid;
}

// The Wii save data the next game start sends (held until then).
void NetPlayClient::SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs,
                                   std::vector<u64> titles, std::string redirect_folder)
{
  m_wii_sync_fs = std::move(fs);
  m_wii_sync_titles = std::move(titles);
  m_wii_sync_redirect_folder = std::move(redirect_folder);
}

// The mappings as the server last sent them.
const PadMappingArray& NetPlayClient::GetWiimoteMapping() const
{
  return m_net_settings.wiimote_map;
}
const PadMappingArray& NetPlayClient::GetPadMapping() const
{
  return m_net_settings.pad_map;
}
const GBAConfigArray& NetPlayClient::GetGBAConfig() const
{
  return m_net_settings.gba_config;
}

void NetPlayClient::AdjustPadBufferSize(const unsigned int size)
{
  m_target_buffer_size = size;
  m_dialog->OnPadBufferChanged(size);
}

// The virtual SD card is identified by the id "sd" alone.
SyncIdentifier NetPlayClient::GetSDCardIdentifier()
{
  SyncIdentifier card{};
  card.game_id = "sd";
  return card;
}

// ---- Free functions ----

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map)
{
  // "GC1,3|GBA2|Wii1": the ports this player owns, by kind.
  std::array<std::vector<std::size_t>, 3> ports_by_kind;
  for (std::size_t port = 0; port < pad_map.size(); ++port)
  {
    if (pad_map[port] == pid)
      ports_by_kind[gba_config[port].enabled ? 1 : 0].push_back(port + 1);
    if (wiimote_map[port] == pid)
      ports_by_kind[2].push_back(port + 1);
  }

  constexpr std::array<const char*, 3> kind_names{"GC", "GBA", "Wii"};
  std::string text;
  for (std::size_t kind = 0; kind < kind_names.size(); ++kind)
  {
    if (ports_by_kind[kind].empty())
      continue;
    if (!text.empty())
      text += '|';
    text += fmt::format("{}{}", kind_names[kind], fmt::join(ports_by_kind[kind], ","));
  }
  return text.empty() ? "None" : text;
}

bool IsNetPlayRunning()
{
  return s_active_client != nullptr;
}

void SetSIPollBatching(bool state)
{
  s_poll_is_batched = state;
}

void SendPowerButtonEvent()
{
  ASSERT(IsNetPlayRunning());
  s_active_client->SendPowerButtonEvent();
}

std::string GetGBASavePath(int pad_num)
{
  std::lock_guard lock(s_active_client_mutex);

  // Outside NetPlay, and on the host, GBA saves live next to the ROM.
  if (!s_active_client || s_active_client->GetNetSettings().is_hosting)
  {
#ifdef HAS_LIBMGBA
    const std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[pad_num]);
    return HW::GBA::Core::GetSavePath(rom_path, pad_num);
#else
    return {};
#endif
  }

  if (!s_active_client->GetNetSettings().savedata_load)
    return {};
  return fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, pad_num + 1);
}

PadDetails GetPadDetails(int pad_num)
{
  std::lock_guard lock(s_active_client_mutex);

  PadDetails details{};
  details.local_pad = NO_LOCAL_PAD;
  if (!s_active_client)
    return details;

  const PadMappingArray owners = s_active_client->GetPadMapping();
  const PlayerId owner = owners[static_cast<std::size_t>(pad_num)];
  if (owner == 0)
    return details;

  for (const Player* player : s_active_client->GetPlayers())
  {
    if (player->pid == owner)
      details.player_name = player->name;
  }

  // Local ports come first in the local numbering, then everyone else's.
  int local_before = 0;
  int remote_before = 0;
  for (std::size_t port = 0; port < static_cast<std::size_t>(pad_num); ++port)
  {
    if (s_active_client->IsLocalPlayer(owners[port]))
      ++local_before;
    else
      ++remote_before;
  }
  details.is_local = s_active_client->IsLocalPlayer(owner);
  details.local_pad =
      details.is_local ? local_before : s_active_client->NumLocalPads() + remote_before;
  details.hide_gba = !details.is_local && s_active_client->GetNetSettings().hide_remote_gbas &&
                     s_active_client->LocalPlayerHasControllerMapped();
  return details;
}

int NumLocalWiimotes()
{
  std::lock_guard lock(s_active_client_mutex);
  return s_active_client ? s_active_client->NumLocalWiimotes() : 0;
}

void NetPlay_Enable(NetPlayClient* const np)
{
  std::lock_guard lock(s_active_client_mutex);
  s_active_client = np;
}

void NetPlay_Disable()
{
  std::lock_guard lock(s_active_client_mutex);
  s_active_client = nullptr;
}

bool NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries)
{
  std::lock_guard lock(s_active_client_mutex);
  return s_active_client && s_active_client->WiimoteUpdate(entries);
}

unsigned int NetPlay_GetLocalWiimoteForSlot(unsigned int slot)
{
  constexpr unsigned int port_count = std::tuple_size_v<PadMappingArray>;
  if (slot >= port_count)
    return slot;

  std::lock_guard lock(s_active_client_mutex);
  if (!s_active_client)
    return slot;

  // This player's Wii Remotes take the first local slots, in port order; the rest follow.
  const PadMappingArray& owners = s_active_client->GetWiimoteMapping();
  const PlayerId self = s_active_client->GetLocalPlayerId();
  std::array<unsigned int, port_count> slot_map{};
  unsigned int next = 0;
  for (const bool mine : {true, false})
  {
    for (unsigned int port = 0; port < port_count; ++port)
    {
      if ((owners[port] == self) == mine)
        slot_map[port] = next++;
    }
  }

  INFO_LOG_FMT(NETPLAY, "Wiimote slot map: [{}]", fmt::join(slot_map, ", "));
  return slot_map[slot];
}
}  // namespace NetPlay

// ---- Emulator hooks ----

// Every frame, for each polled GC port.
bool SerialInterface::CSIDevice_GCController::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  std::lock_guard lock(NetPlay::s_active_client_mutex);
  if (!NetPlay::s_active_client)
    return false;
  return NetPlay::s_active_client->GetNetPads(pad_num, NetPlay::s_poll_is_batched, status);
}

// The RTC every player boots with (also read when input recording starts).
u64 ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime()
{
  std::lock_guard lock(NetPlay::s_active_client_mutex);
  return NetPlay::s_active_client ? NetPlay::s_active_client->GetInitialRTCValue() : 0;
}

// The local controller that should rumble for an in-game port.
int SerialInterface::CSIDevice_GCController::NetPlay_InGamePadToLocalPad(int pad_num)
{
  std::lock_guard lock(NetPlay::s_active_client_mutex);
  if (!NetPlay::s_active_client)
    return pad_num;
  return NetPlay::s_active_client->InGamePadToLocalPad(pad_num);
}
