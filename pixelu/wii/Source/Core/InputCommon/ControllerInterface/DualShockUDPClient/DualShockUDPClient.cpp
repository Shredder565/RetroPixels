// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/UdpSocket.hpp>
#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Random.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPProto.h"

namespace ciface::DualShockUDPClient
{
namespace Settings
{
const Config::Info<std::string> SERVER_ADDRESS{
    {Config::System::DualShockUDPClient, "Server", "IPAddress"}, ""};
const Config::Info<int> SERVER_PORT{{Config::System::DualShockUDPClient, "Server", "Port"}, 0};
const Config::Info<std::string> SERVERS{{Config::System::DualShockUDPClient, "Server", "Entries"},
                                        ""};
const Config::Info<bool> SERVERS_ENABLED{{Config::System::DualShockUDPClient, "Server", "Enabled"},
                                         false};
}  // namespace Settings

namespace
{
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

constexpr std::string_view SOURCE_NAME = "DSUClient";

// Servers stream a slot's data for a few seconds after each request; re-request every second.
constexpr auto PAD_DATA_REQUEST_PERIOD = 1s;
// Each server is asked to list its slots every second.
constexpr auto LIST_PORTS_PERIOD = 1s;
// A server that stays silent this long after its last answer loses its devices.
constexpr auto SERVER_SILENCE_LIMIT = 1s;
// Longest single wait of the hotplug thread, so stopping it never takes long.
constexpr auto LONGEST_WAIT = 250ms;
// Slots asked for in each port listing.
constexpr u32 LISTED_SLOTS = 4;

// The touchpad is turned into a relative pointer within these bounds.
constexpr int TOUCH_X_LIMIT = 1000;
constexpr int TOUCH_Y_LIMIT = 500;
// Resolution of a DualShock 4 touchpad, used for the resting position.
constexpr int DS4_TOUCH_WIDTH = 1919;
constexpr int DS4_TOUCH_HEIGHT = 941;

// A stick or analog button: raw value plus `bias`, divided by `scale`.
template <typename Raw, bool Detectable = true>
class ScaledInput : public Core::Device::Input
{
public:
  ScaledInput(const char* name, const Raw& raw, ControlState scale, ControlState bias = 0)
      : m_name(name), m_raw(raw), m_scale(scale), m_bias(bias)
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return (ControlState(m_raw) + m_bias) / m_scale; }
  bool IsDetectable() const override { return Detectable; }

private:
  const char* const m_name;
  const Raw& m_raw;
  const ControlState m_scale;
  const ControlState m_bias;
};

// Touch position and motion data are never picked up by input detection.
using TouchAxis = ScaledInput<int, false>;
using MotionAxis = ScaledInput<float, false>;

class FlagInput final : public Core::Device::Input
{
public:
  FlagInput(const char* name, const u8& bits, u8 mask) : m_name(name), m_bits(bits), m_mask(mask) {}

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return (m_bits & m_mask) != 0; }

private:
  const char* const m_name;
  const u8& m_bits;
  const u8 m_mask;
};

class BatteryLevel final : public Core::Device::Input
{
public:
  explicit BatteryLevel(const Proto::DsBattery& battery) : m_battery(battery) {}

  std::string GetName() const override { return "Battery"; }
  bool IsDetectable() const override { return false; }

  ControlState GetState() const override
  {
    // While charging the server does not report a level.
    if (m_battery == Proto::DsBattery::Charging || m_battery == Proto::DsBattery::Charged)
      return BATTERY_INPUT_MAX_VALUE;
    return ControlState(m_battery) / ControlState(Proto::DsBattery::Full) * BATTERY_INPUT_MAX_VALUE;
  }

private:
  const Proto::DsBattery& m_battery;
};

// One slot of one server.
class Device final : public Core::Device
{
public:
  Device(std::string name, int slot, std::string server_address, u16 server_port, u32 client_id);

  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return std::string(SOURCE_NAME); }
  std::optional<int> GetPreferredId() const override { return m_slot; }
  // Hotplugged devices go at the end of the list.
  int GetSortPriority() const override { return -5; }

  Core::DeviceRemoval UpdateInput() override;

private:
  void AddControls();
  void ResetState();
  void RequestPadData();
  // Takes in the pad data just decoded into m_pad.
  void Absorb();

  const std::string m_name;
  const int m_slot;
  const std::string m_server_address;
  const u16 m_server_port;
  const u32 m_client_id;

  sf::UdpSocket m_socket;
  SteadyClock::time_point m_next_request = SteadyClock::time_point::min();

  Proto::PadDataResponse m_pad{};

  // The touchpad as a relative pointer: last touch seen and the accumulated position.
  Proto::Touch m_last_touch{};
  bool m_last_touch_known = false;
  int m_touch_x = 0;
  int m_touch_y = 0;
};

Device::Device(std::string name, int slot, std::string server_address, u16 server_port,
               u32 client_id)
    : m_name{std::move(name)}, m_slot{slot}, m_server_address{std::move(server_address)},
      m_server_port{server_port}, m_client_id(client_id)
{
  m_socket.setBlocking(false);
  AddControls();
  ResetState();
}

void Device::AddControls()
{
  // Pressure-sensitive buttons.
  using AnalogField = decltype(&Proto::PadDataResponse::trigger_l2);
  static constexpr std::pair<const char*, AnalogField> analog_buttons[] = {
      {"Pad W", &Proto::PadDataResponse::button_dpad_left_analog},
      {"Pad S", &Proto::PadDataResponse::button_dpad_down_analog},
      {"Pad E", &Proto::PadDataResponse::button_dpad_right_analog},
      {"Pad N", &Proto::PadDataResponse::button_dpad_up_analog},
      {"Square", &Proto::PadDataResponse::button_square_analog},
      {"Cross", &Proto::PadDataResponse::button_cross_analog},
      {"Circle", &Proto::PadDataResponse::button_circle_analog},
      {"Triangle", &Proto::PadDataResponse::button_triangle_analog},
      {"L1", &Proto::PadDataResponse::button_l1_analog},
      {"R1", &Proto::PadDataResponse::button_r1_analog},
      {"L2", &Proto::PadDataResponse::trigger_l2},
      {"R2", &Proto::PadDataResponse::trigger_r2},
  };
  for (const auto& [name, field] : analog_buttons)
    AddInput(new ScaledInput<u8>(name, m_pad.*field, 255));

  // Digital-only buttons.
  AddInput(new FlagInput("L3", m_pad.button_states1, 0x2));
  AddInput(new FlagInput("R3", m_pad.button_states1, 0x4));
  AddInput(new FlagInput("Share", m_pad.button_states1, 0x1));
  AddInput(new FlagInput("Options", m_pad.button_states1, 0x8));
  AddInput(new FlagInput("PS", m_pad.button_ps, 0x1));
  AddInput(new FlagInput("Touch Button", m_pad.button_touch, 0x1));

  // Sticks: bytes centred on 128 (the Y axes pointing down), one input per direction.
  struct StickAxis
  {
    const char* negative_name;
    const char* positive_name;
    u8 Proto::PadDataResponse::*field;
  };
  static constexpr StickAxis stick_axes[] = {
      {"Left X-", "Left X+", &Proto::PadDataResponse::left_stick_x},
      {"Left Y-", "Left Y+", &Proto::PadDataResponse::left_stick_y_inverted},
      {"Right X-", "Right X+", &Proto::PadDataResponse::right_stick_x},
      {"Right Y-", "Right Y+", &Proto::PadDataResponse::right_stick_y_inverted},
  };
  for (const StickAxis& axis : stick_axes)
  {
    const u8& raw = m_pad.*axis.field;
    AddInput(new ScaledInput<u8>(axis.negative_name, raw, -128, -128));
    AddInput(new ScaledInput<u8>(axis.positive_name, raw, 127, -128));
  }

  AddInput(new TouchAxis("Touch X-", m_touch_x, -TOUCH_X_LIMIT));
  AddInput(new TouchAxis("Touch X+", m_touch_x, TOUCH_X_LIMIT));
  AddInput(new TouchAxis("Touch Y-", m_touch_y, -TOUCH_Y_LIMIT));
  AddInput(new TouchAxis("Touch Y+", m_touch_y, TOUCH_Y_LIMIT));

  // The accelerometer reports g; inputs are in m/s².
  constexpr ControlState accel_scale = 1.0 / MathUtil::GRAVITY_ACCELERATION;
  AddInput(new MotionAxis("Accel Up", m_pad.accelerometer_y_g, -accel_scale));
  AddInput(new MotionAxis("Accel Down", m_pad.accelerometer_y_g, accel_scale));
  AddInput(new MotionAxis("Accel Left", m_pad.accelerometer_x_g, accel_scale));
  AddInput(new MotionAxis("Accel Right", m_pad.accelerometer_x_g, -accel_scale));
  AddInput(new MotionAxis("Accel Forward", m_pad.accelerometer_z_g, accel_scale));
  AddInput(new MotionAxis("Accel Backward", m_pad.accelerometer_z_g, -accel_scale));

  // The gyroscope reports degrees per second; inputs are in radians per second.
  constexpr ControlState gyro_scale = 360.0 / MathUtil::TAU;
  AddInput(new MotionAxis("Gyro Pitch Up", m_pad.gyro_pitch_deg_s, gyro_scale));
  AddInput(new MotionAxis("Gyro Pitch Down", m_pad.gyro_pitch_deg_s, -gyro_scale));
  AddInput(new MotionAxis("Gyro Roll Left", m_pad.gyro_roll_deg_s, -gyro_scale));
  AddInput(new MotionAxis("Gyro Roll Right", m_pad.gyro_roll_deg_s, gyro_scale));
  AddInput(new MotionAxis("Gyro Yaw Left", m_pad.gyro_yaw_deg_s, -gyro_scale));
  AddInput(new MotionAxis("Gyro Yaw Right", m_pad.gyro_yaw_deg_s, gyro_scale));

  AddInput(new BatteryLevel(m_pad.slot.battery_status));
}

void Device::ResetState()
{
  m_pad = {};

  // Rest in the middle of the touchpad and of both sticks rather than at zero.
  m_touch_x = static_cast<int>(DS4_TOUCH_WIDTH / 2.0);
  m_touch_y = static_cast<int>(DS4_TOUCH_HEIGHT / 2.0);
  m_pad.left_stick_x = 128;
  m_pad.left_stick_y_inverted = 128;
  m_pad.right_stick_x = 128;
  m_pad.right_stick_y_inverted = 128;
  m_pad.touch1.x = static_cast<s16>(m_touch_x);
  m_pad.touch1.y = static_cast<s16>(m_touch_y);
}

void Device::RequestPadData()
{
  auto request = Proto::NewMessage<Proto::PadDataRequest>(m_client_id);
  request.register_flags = Proto::RegisterFlags::PadID;
  request.pad_id_to_register = static_cast<u8>(m_slot);
  Proto::Seal(&request);

  const sf::IpAddress server = sf::IpAddress::resolve(m_server_address).value();
  if (m_socket.send(&request, sizeof(request), server, m_server_port) != sf::Socket::Status::Done)
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient UpdateInput send failed");
}

void Device::Absorb()
{
  // Touch movement is applied while the same finger stays down.
  const Proto::Touch& touch = m_pad.touch1;
  if (m_last_touch_known && touch.id == m_last_touch.id)
  {
    m_touch_x = std::clamp(m_touch_x + (touch.x - m_last_touch.x), -TOUCH_X_LIMIT, TOUCH_X_LIMIT);
    m_touch_y = std::clamp(m_touch_y + (touch.y - m_last_touch.y), -TOUCH_Y_LIMIT, TOUCH_Y_LIMIT);
  }
  m_last_touch = touch;
  m_last_touch_known = true;
}

Core::DeviceRemoval Device::UpdateInput()
{
  const auto now = SteadyClock::now();
  if (now >= m_next_request)
  {
    m_next_request = now + PAD_DATA_REQUEST_PERIOD;
    RequestPadData();
  }

  // Drain everything that arrived. The buffer is shared by the datagrams of this update, so a
  // short datagram is checked against the leftovers of a previous one.
  Proto::ReceiveBuffer buffer;
  std::size_t length;
  std::optional<sf::IpAddress> sender;
  u16 sender_port;
  while (m_socket.receive(buffer.bytes.data(), buffer.bytes.size(), length, sender, sender_port) ==
         sf::Socket::Status::Done)
  {
    // (Decoded straight into the device's state, which a rejected datagram leaves alone.)
    if (buffer.Decode(&m_pad))
      Absorb();
  }

  return Core::DeviceRemoval::Keep;
}

struct Server
{
  Server(std::string description_, std::string address_, u16 port_)
      : description(std::move(description_)), address(std::move(address_)), port(port_)
  {
  }

  // Marks every slot as disconnected.
  void ForgetSlots()
  {
    for (size_t slot = 0; slot != slots.size(); ++slot)
    {
      slots[slot] = {};
      slots[slot].slot.pad_id = static_cast<u8>(slot);
    }
  }

  bool AnySlotConnected() const
  {
    return std::ranges::any_of(slots, [](const Proto::PortInfo& info) {
      return info.slot.pad_state == Proto::DsState::Connected;
    });
  }

  std::string description;
  std::string address;
  u16 port;
  std::array<Proto::PortInfo, Proto::PORT_COUNT> slots{};
  sf::UdpSocket socket;
  SteadyClock::time_point silent_after = SteadyClock::now();
};

// Whether two slot descriptions name the same controller (the battery level may differ).
bool SameController(const Proto::SlotInfo& a, const Proto::SlotInfo& b)
{
  return a.pad_id == b.pad_id && a.pad_state == b.pad_state && a.model == b.model &&
         a.connection_type == b.connection_type && a.pad_mac_address == b.pad_mac_address;
}

class InputBackend final : public ciface::InputBackend
{
public:
  explicit InputBackend(ControllerInterface* controller_interface);
  ~InputBackend() override;
  void PopulateDevices() override;

private:
  void MigrateOldSetting();
  void OnConfigChanged();
  void Restart();
  void RepopulateDevices();

  void StartWatching();
  void StopWatching();
  void WatchServers();
  void ListPorts(std::vector<bool>* awaiting_reply);
  void ReceivePortInfo(sf::SocketSelector& selector, std::vector<bool>* awaiting_reply);
  void DropSilentServers(const std::vector<bool>& awaiting_reply);

  bool m_enabled = false;
  std::vector<std::unique_ptr<Server>> m_servers;
  u32 m_client_id = 0;
  SteadyClock::time_point m_next_listing;
  std::thread m_watcher;
  Common::Flag m_watching;
  Config::ConfigChangedCallbackID m_config_callback;
};

InputBackend::InputBackend(ControllerInterface* controller_interface)
    : ciface::InputBackend(controller_interface)
{
  MigrateOldSetting();

  m_config_callback = Config::AddConfigChangedCallback([this] { OnConfigChanged(); });
  OnConfigChanged();
}

InputBackend::~InputBackend()
{
  Config::RemoveConfigChangedCallback(m_config_callback);
  StopWatching();
}

void InputBackend::MigrateOldSetting()
{
  const std::string old_address = Config::Get(Settings::SERVER_ADDRESS);
  const int old_port = Config::Get(Settings::SERVER_PORT);
  if (old_address.empty() || old_port == 0)
    return;

  const std::string servers = Config::Get(Settings::SERVERS);
  Config::SetBaseOrCurrent(Settings::SERVERS,
                           servers + fmt::format("{}:{}:{};", "DS4", old_address, old_port));
  Config::SetBase(Settings::SERVER_ADDRESS, "");
  Config::SetBase(Settings::SERVER_PORT, 0);
}

void InputBackend::OnConfigChanged()
{
  const bool enabled = Config::Get(Settings::SERVERS_ENABLED);
  const std::string wanted = Config::Get(Settings::SERVERS);

  std::string current;
  for (const std::unique_ptr<Server>& server : m_servers)
    current += fmt::format("{}:{}:{};", server->description, server->address, server->port);

  if (enabled == m_enabled && wanted == current)
    return;

  // The server list may only change while the hotplug thread is stopped.
  StopWatching();

  m_enabled = enabled;
  m_servers.clear();

  for (const std::string& entry : SplitString(wanted, ';'))
  {
    const std::vector<std::string> fields = SplitString(entry, ':');
    if (fields.size() < 3)
      continue;

    const int port = std::stoi(fields[2]);
    if (port >= std::numeric_limits<u16>::max())
      continue;

    m_servers.push_back(std::make_unique<Server>(fields[0], fields[1], static_cast<u16>(port)));
  }

  Restart();
}

void InputBackend::Restart()
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient Restart");

  StopWatching();

  for (const std::unique_ptr<Server>& server : m_servers)
    server->ForgetSlots();

  // With every slot forgotten this only removes our devices.
  RepopulateDevices();

  m_client_id = Common::Random::GenerateValue<u32>();
  m_next_listing = SteadyClock::now();

  if (m_enabled && !m_servers.empty())
    StartWatching();
}

void InputBackend::RepopulateDevices()
{
  GetControllerInterface().PlatformPopulateDevices([this] { PopulateDevices(); });
}

// Called from the host thread and from the hotplug thread (always through
// PlatformPopulateDevices). The server list itself only changes while the hotplug thread is
// stopped.
void InputBackend::PopulateDevices()
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient PopulateDevices");

  // Remove all of ours first, so re-added devices get the same ids again. Two servers with the
  // same address both add their devices; they do not conflict.
  GetControllerInterface().RemoveDevice(
      [](const Core::Device* device) { return device->GetSource() == SOURCE_NAME; });

  for (const std::unique_ptr<Server>& server : m_servers)
  {
    for (size_t slot = 0; slot != server->slots.size(); ++slot)
    {
      if (server->slots[slot].slot.pad_state != Proto::DsState::Connected)
        continue;

      GetControllerInterface().AddDevice(std::make_shared<Device>(
          server->description, static_cast<int>(slot), server->address, server->port, m_client_id));
    }
  }
}

void InputBackend::StartWatching()
{
  if (!m_watching.TestAndSet())
    return;

  m_watcher = std::thread([this] { WatchServers(); });
}

void InputBackend::StopWatching()
{
  if (!m_watching.TestAndClear())
    return;

  m_watcher.join();

  // Unblock any socket operation still pending.
  for (const std::unique_ptr<Server>& server : m_servers)
    server->socket.unbind();
}

void InputBackend::ListPorts(std::vector<bool>* awaiting_reply)
{
  for (size_t i = 0; i != m_servers.size(); ++i)
  {
    Server& server = *m_servers[i];

    auto request = Proto::NewMessage<Proto::ListPorts>(m_client_id);
    request.pad_request_count = LISTED_SLOTS;
    request.pad_ids = {0, 1, 2, 3};
    Proto::Seal(&request);

    const std::optional<sf::IpAddress> address = sf::IpAddress::resolve(server.address);
    if (!address)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient failed to resolve {}", server.address);
    }
    else if (server.socket.send(&request, sizeof(request), *address, server.port) !=
             sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient HotplugThreadFunc send failed");
    }

    (*awaiting_reply)[i] = true;
  }
}

void InputBackend::ReceivePortInfo(sf::SocketSelector& selector, std::vector<bool>* awaiting_reply)
{
  // The selector does not say which server answered; look at each.
  for (size_t i = 0; i != m_servers.size(); ++i)
  {
    Server& server = *m_servers[i];
    if (!selector.isReady(server.socket))
      continue;

    Proto::ReceiveBuffer buffer;
    std::size_t length;
    std::optional<sf::IpAddress> sender;
    u16 sender_port;
    if (server.socket.receive(buffer.bytes.data(), buffer.bytes.size(), length, sender,
                              sender_port) != sf::Socket::Status::Done)
    {
      continue;
    }

    Proto::PortInfo info;
    if (!buffer.Decode(&info))
      continue;

    // One valid answer per round is enough to count the server as alive, even if this thread
    // got to it late.
    server.silent_after = SteadyClock::now() + SERVER_SILENCE_LIMIT;
    (*awaiting_reply)[i] = false;

    // Slots beyond the four a server has are ignored.
    const u8 slot = info.slot.pad_id;
    if (slot >= server.slots.size())
      continue;

    if (SameController(info.slot, server.slots[slot].slot))
      continue;

    server.slots[slot] = info;
    // Simplest to rebuild all our devices.
    RepopulateDevices();
  }
}

void InputBackend::DropSilentServers(const std::vector<bool>& awaiting_reply)
{
  for (size_t i = 0; i != m_servers.size(); ++i)
  {
    Server& server = *m_servers[i];
    if (!awaiting_reply[i] || SteadyClock::now() < server.silent_after)
      continue;

    const bool had_devices = server.AnySlotConnected();
    server.ForgetSlots();
    // Devices are not tagged with their server, so all of ours are rebuilt.
    if (had_devices)
      RepopulateDevices();
  }
}

void InputBackend::WatchServers()
{
  Common::SetCurrentThreadName("DualShockUDPClient Hotplug Thread");
  INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient hotplug thread started");
  Common::ScopeGuard log_stop{
      [] { INFO_LOG_FMT(CONTROLLERINTERFACE, "DualShockUDPClient hotplug thread stopped"); }};

  // Servers asked for their ports that have not answered yet.
  std::vector<bool> awaiting_reply(m_servers.size(), false);

  while (m_watching.IsSet())
  {
    const auto now = SteadyClock::now();
    if (now >= m_next_listing)
    {
      m_next_listing = now + LIST_PORTS_PERIOD;
      ListPorts(&awaiting_reply);
    }

    sf::SocketSelector selector;
    for (const std::unique_ptr<Server>& server : m_servers)
      selector.add(server->socket);

    // Listen until the next listing is due, in short waits so a stop request is noticed. This
    // also runs when nothing was sent, to catch late answers and notice silent servers.
    auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(m_next_listing - SteadyClock::now());
    do
    {
      // A zero timeout would mean "forever" to the selector.
      const auto wait = std::max(std::min(remaining, std::chrono::milliseconds(LONGEST_WAIT)),
                                 std::chrono::milliseconds(1ms));
      remaining -= wait;

      if (selector.wait(sf::milliseconds(static_cast<std::int32_t>(wait.count()))))
        ReceivePortInfo(selector, &awaiting_reply);

      if (!m_watching.IsSet())
        return;
    } while (remaining > 0ms);

    DropSilentServers(awaiting_reply);
  }
}

}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}

}  // namespace ciface::DualShockUDPClient
