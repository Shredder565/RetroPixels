// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceAMBaseboard.h"

#include <algorithm>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/MagCard/C1231BR.h"
#include "Core/HW/MagCard/C1231LR.h"
#include "Core/HW/Triforce/DeckReader.h"
#include "Core/HW/Triforce/FZeroAX.h"
#include "Core/HW/Triforce/ICCardReader.h"
#include "Core/HW/Triforce/JVSIO.h"
#include "Core/HW/Triforce/MarioKartGP.h"
#include "Core/HW/Triforce/SerialDevice.h"
#include "Core/HW/Triforce/Touchscreen.h"

namespace SerialInterface
{

namespace
{
constexpr u8 CMD_STATUS = 0x00;
constexpr u8 CMD_BASEBOARD = 0x70;

// The last byte of a reply holds its checksum.
constexpr std::size_t REPLY_LIMIT = 127;

// Serial port replies are cut into pieces of at most this many bytes.
constexpr std::size_t SERIAL_CHUNK = 0x1F;

constexpr std::string_view BASEBOARD_SERIAL = "AADE-01B98394904";

constexpr std::array<u8, 20> REGION_TABLE{0x00, 0x00, 0x30, 0x00, 0x02, 0xFD, 0x00, 0x00, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Walks the packed sub-commands of one 0x70 request and collects the reply.
class PackedRequest
{
public:
  PackedRequest(const u8* begin, const u8* end, std::span<u8> out)
      : m_begin{begin}, m_pos{begin}, m_end{end}, m_out{out}
  {
  }

  bool Done() const { return m_pos == m_end; }
  u8 Take() { return *m_pos++; }
  const u8* Here() const { return m_pos; }
  void Skip(std::size_t count) { m_pos += count; }
  std::size_t OutSize() const { return m_offset; }

  // Checks that `n_in` request bytes are left and `n_out` reply bytes fit. On
  // failure the rest of the request is dropped.
  bool Check(std::size_t n_in, std::size_t n_out, std::string_view name)
  {
    const std::size_t left = static_cast<std::size_t>(m_end - m_pos);
    if (left >= n_in && m_offset + n_out <= REPLY_LIMIT)
      return true;

    if (left < n_in)
      ERROR_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: data_in overflow in {}", name);
    else
      ERROR_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: data_out overflow in {}", name);
    ERROR_LOG_FMT(SERIALINTERFACE_AMBB,
                  "Overflow details:\n - data_in(begin={}, current={}, end={}, n_in={})\n"
                  " - data_out(offset={}, size={}, n_out={})",
                  fmt::ptr(m_begin), fmt::ptr(m_pos), fmt::ptr(m_end), n_in, m_offset,
                  m_out.size(), n_out);
    m_pos = m_end;
    return false;
  }

  void Put(u8 value) { m_out[m_offset++] = value; }
  void Put(std::span<const u8> values)
  {
    std::ranges::copy(values, m_out.begin() + static_cast<std::ptrdiff_t>(m_offset));
    m_offset += values.size();
  }

private:
  const u8* const m_begin;
  const u8* m_pos;
  const u8* const m_end;
  std::span<u8> m_out;
  std::size_t m_offset = 2;
};

// Appends what a serial device has queued, under the reply command `tag`.
void DrainSerial(PackedRequest& request, u8 tag, Triforce::SerialDevice& device,
                 std::size_t limit)
{
  const std::size_t count = std::min(device.GetTxByteCount(), limit);
  if (count == 0)
    return;
  if (!request.Check(0, count + 2, "SerialDevice"))
    return;

  std::array<u8, 0x100> bytes{};
  const std::span<u8> chunk = std::span(bytes).first(count);
  device.TakeTxBytes(chunk);
  request.Put(tag);
  request.Put(static_cast<u8>(count));
  request.Put(chunk);
}
}  // namespace

CSIDevice_AMBaseboard::CSIDevice_AMBaseboard(Core::System& system, SIDevices device,
                                             int device_number)
    : ISIDevice(system, device, device_number)
{
  m_card_settings.card_path = File::GetUserPath(D_TRIUSER_IDX);
  m_card_settings.card_name = fmt::format("tricard_{}.bin", SConfig::GetInstance().GetGameID());

  m_io_ports.AddIOAdapter(std::make_unique<Triforce::Common_IOAdapter>(system));

  using namespace Triforce;
  const u32 game_type = AMMediaboard::GetGameType();
  switch (game_type)
  {
  case AMMediaboard::FZeroAX:
  {
    m_jvs = std::make_unique<Sega_837_13844_JVSIOBoard>(&m_io_ports);
    auto wheel = std::make_unique<FZeroAXSteeringWheel>();
    m_io_ports.AddIOAdapter(std::make_unique<FZeroAXCommon_IOAdapter>(wheel.get()));
    m_io_ports.AddIOAdapter(std::make_unique<FZeroAXDeluxe_IOAdapter>());
    m_serial_a = std::move(wheel);
    m_serial_b = std::make_unique<MagCard::C1231BR>(&m_card_settings);
    break;
  }
  case AMMediaboard::FZeroAXMonster:
  {
    auto wheel = std::make_unique<FZeroAXSteeringWheel>();
    m_io_ports.AddIOAdapter(std::make_unique<FZeroAXCommon_IOAdapter>(wheel.get()));
    m_io_ports.AddIOAdapter(std::make_unique<FZeroAXMonster_IOAdapter>());
    m_serial_a = std::move(wheel);
    break;
  }
  case AMMediaboard::MarioKartGP:
  case AMMediaboard::MarioKartGP2:
  {
    m_jvs = std::make_unique<Namco_FCA_JVSIOBoard>(&m_io_ports);
    auto wheel = std::make_unique<MarioKartGPSteeringWheel>();
    m_io_ports.AddIOAdapter(std::make_unique<MarioKartGPCommon_IOAdapter>(wheel.get()));
    m_serial_a = std::move(wheel);
    m_serial_b = std::make_unique<MagCard::C1231LR>(&m_card_settings);
    break;
  }
  case AMMediaboard::VirtuaStriker3:
    m_io_ports.AddIOAdapter(std::make_unique<VirtuaStriker3_IOAdapter>());
    break;
  case AMMediaboard::VirtuaStriker4:
  case AMMediaboard::VirtuaStriker4_2006:
  case AMMediaboard::GekitouProYakyuu:
  {
    auto reader_a = std::make_unique<ICCardReader>(u8{0});
    auto reader_b = std::make_unique<ICCardReader>(u8{1});
    if (game_type == AMMediaboard::GekitouProYakyuu)
    {
      m_io_ports.AddIOAdapter(
          std::make_unique<GekitouProYakyuu_IOAdapter>(reader_a.get(), reader_b.get()));
    }
    else
    {
      m_io_ports.AddIOAdapter(
          std::make_unique<VirtuaStriker4Common_IOAdapter>(reader_a.get(), reader_b.get()));
      if (game_type == AMMediaboard::VirtuaStriker4_2006)
      {
        m_io_ports.AddIOAdapter(
            std::make_unique<VirtuaStriker4_2006_IOAdapter>(reader_a.get(), reader_b.get()));
      }
    }
    m_serial_a = std::move(reader_a);
    m_serial_b = std::move(reader_b);
    break;
  }
  case AMMediaboard::KeyOfAvalon:
  {
    auto deck = std::make_unique<DeckReader>();
    m_io_ports.AddIOAdapter(std::make_unique<KeyOfAvalon_IOAdapter>(deck->GetICCardReader()));
    m_serial_a = std::move(deck);
    m_serial_b = std::make_unique<Touchscreen>();
    break;
  }
  default:
    break;
  }

  if (!m_jvs)
    m_jvs = std::make_unique<Sega_837_13551_JVSIOBoard>(&m_io_ports);
}

CSIDevice_AMBaseboard::~CSIDevice_AMBaseboard() = default;

int CSIDevice_AMBaseboard::RunBuffer(u8* buffer, int request_length)
{
  ISIDevice::RunBuffer(buffer, request_length);
  m_io_ports.Update();

  switch (buffer[0])
  {
  case CMD_STATUS:
    CreateStatusResponse(SI_AM_BASEBOARD | 0x100, buffer);
    return 3;
  case CMD_BASEBOARD:
    return RunBaseboardCommand(buffer, request_length);
  default:
    ERROR_LOG_FMT(SERIALINTERFACE, "Unknown SI command (0x{:08x})", buffer[0]);
    PanicAlertFmt("SI: Unknown command");
    return -1;
  }
}

int CSIDevice_AMBaseboard::RunBaseboardCommand(u8* buffer, int request_length)
{
  const u32 length = buffer[1];
  if (static_cast<int>(2 + length) > request_length)
  {
    ERROR_LOG_FMT(SERIALINTERFACE_AMBB, "Bad command_length: {} for request_length: {}", length,
                  request_length);
    return -1;
  }

  std::array<u8, RESPONSE_SIZE>& out = m_responses[m_response_index];
  PackedRequest request(buffer + 2, buffer + 2 + length, out);

  while (!request.Done())
  {
    const u8 command = request.Take();
    const u8* const in = request.Here();

    switch (command)
    {
    case 0x10:
      if (!request.Check(1, 4, "StatusSwitches"))
        break;
      DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x10, {:02x} (READ STATUS&SWITCHES)",
                    in[0]);
      request.Skip(1);
      request.Put(0x10);
      request.Put(0x02);
      request.Put(std::as_const(m_io_ports).GetStatusSwitches());
      break;

    case 0x11:
    {
      if (!request.Check(1, 18, "SerialNumber"))
        break;
      NOTICE_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x11, {:02x} (READ SERIAL NR)", in[0]);
      request.Skip(1);
      request.Put(0x11);
      request.Put(0x10);
      for (const char c : BASEBOARD_SERIAL)
        request.Put(static_cast<u8>(c));
      break;
    }

    case 0x12:
    case 0x14:
    case 0x23:
    case 0x24:
    {
      const std::string name = fmt::format("Unknown_{:02X}", command);
      if (!request.Check(2, 2, name))
        break;
      if (command == 0x12)
        NOTICE_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x12, {:02x} {:02x}", in[0], in[1]);
      else if (command == 0x14)
        NOTICE_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x14, {:02x} {:02x}", in[0], in[1]);
      else if (command == 0x23)
        DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x23, {:02x} {:02x}", in[0], in[1]);
      else
        DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x24, {:02x} {:02x}", in[0], in[1]);
      request.Skip(2);
      request.Put(command);
      request.Put(0x00);
      break;
    }

    case 0x15:
      if (!request.Check(1, 4, "FirmVersion"))
        break;
      NOTICE_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x15, {:02x} (READ FIRM VERSION)",
                     in[0]);
      request.Skip(1);
      request.Put(std::array<u8, 4>{0x15, 0x02, 0x00, 0x26});
      break;

    case 0x16:
      if (!request.Check(1, 4, "FPGAVersion"))
        break;
      NOTICE_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x16, {:02x} (READ FPGA VERSION)",
                     in[0]);
      request.Skip(1);
      request.Put(std::array<u8, 4>{0x16, 0x02, 0x07, 0x06});
      break;

    case 0x1F:
      if (!request.Check(5, 22, "RegionSettings"))
        break;
      NOTICE_LOG_FMT(SERIALINTERFACE_AMBB,
                     "GC-AM: Command 0x1F, {:02x} {:02x} {:02x} {:02x} {:02x} (REGION)", in[0],
                     in[1], in[2], in[3], in[4]);
      request.Skip(5);
      request.Put(0x1F);
      request.Put(static_cast<u8>(REGION_TABLE.size()));
      request.Put(REGION_TABLE);
      break;

    case 0x21:
      if (!request.Check(4, 0, "Unknown_21"))
        break;
      DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x21, {:02x}, {:02x}, {:02x}, {:02x}",
                    in[0], in[1], in[2], in[3]);
      request.Skip(4);
      break;

    case 0x22:
    case 0x60:
    {
      // A counted block; the count byte itself is skipped with it.
      const std::string name = fmt::format("Unknown_{:02X}", command);
      if (!request.Check(command == 0x22 ? 7 : 3, 0, name))
        break;
      if (command == 0x22)
      {
        DEBUG_LOG_FMT(SERIALINTERFACE_AMBB,
                      "GC-AM: Command 0x22, {:02x}, {:02x}, {:02x}, {:02x}, {:02x}, {:02x}, {:02x}",
                      in[0], in[1], in[2], in[3], in[4], in[5], in[6]);
      }
      else
      {
        NOTICE_LOG_FMT(SERIALINTERFACE_AMBB, "GC-AM: Command 0x60, {:02x} {:02x} {:02x}", in[0],
                       in[1], in[2]);
      }
      const std::size_t skip = std::size_t{in[0]} + 1;
      if (!request.Check(skip, 0, name))
        break;
      request.Skip(skip);
      break;
    }

    case 0x31:
    case 0x32:
    {
      const std::string_view name = command == 0x31 ? "SerialA" : "SerialB";
      if (!request.Check(1, 0, name))
        break;
      const std::size_t count = request.Take();
      if (!request.Check(count, 0, name))
        break;
      Triforce::SerialDevice* const device =
          command == 0x31 ? m_serial_a.get() : m_serial_b.get();
      if (device != nullptr)
        device->WriteRxBytes(std::span(request.Here(), count));
      request.Skip(count);
      break;
    }

    case 0x40:
    case 0x41:
    {
      if (!request.Check(1, 0, "JVSIO"))
        break;
      const std::size_t count = request.Take();
      if (!request.Check(count, 0, "JVSIO"))
        break;
      m_jvs->WriteRxBytes(std::span(request.Here(), count));
      request.Skip(count);
      m_jvs->Update();

      // A JVS reply goes out whole or waits for a later request.
      const std::size_t reply_size = m_jvs->GetTxByteCount();
      if (reply_size == 0)
        break;
      if (!request.Check(0, reply_size + 2, "SerialDevice"))
        break;
      std::array<u8, RESPONSE_SIZE> bytes{};
      const std::span<u8> reply = std::span(bytes).first(reply_size);
      m_jvs->TakeTxBytes(reply);
      request.Put(command);
      request.Put(static_cast<u8>(reply_size));
      request.Put(reply);
      break;
    }

    default:
    {
      const std::string name = fmt::format("Unknown_{}", command);
      if (!request.Check(5, 0, name))
        break;
      ERROR_LOG_FMT(SERIALINTERFACE_AMBB,
                    "GC-AM: Command {:02x} (unknown) {:02x} {:02x} {:02x} {:02x} {:02x}", command,
                    in[0], in[1], in[2], in[3], in[4]);
      break;
    }
    }
  }

  if (m_serial_a)
  {
    m_serial_a->Update();
    DrainSerial(request, 0x31, *m_serial_a, SERIAL_CHUNK);
  }
  if (m_serial_b)
  {
    m_serial_b->Update();
    DrainSerial(request, 0x32, *m_serial_b, SERIAL_CHUNK);
  }

  const std::size_t size = request.OutSize();
  out[0] = 0x01;
  out[1] = static_cast<u8>(size - 2);
  std::fill(out.begin() + static_cast<std::ptrdiff_t>(size), out.end() - 1, u8{0});
  const u32 sum = std::accumulate(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size), u32{0});
  out[REPLY_LIMIT] = static_cast<u8>(0xFF - (sum & 0xFF));
  DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "Command response: {:02x}",
                fmt::join(std::span(out).first(size), " "));

  // Games expect the answer to the previous request.
  m_response_index ^= 1;
  const std::array<u8, RESPONSE_SIZE>& previous = m_responses[m_response_index];
  if (previous[0] == 0)
    return 0;
  std::ranges::copy(previous, buffer);
  return static_cast<int>(RESPONSE_SIZE);
}

DataResponse CSIDevice_AMBaseboard::GetData(u32&, u32&)
{
  return DataResponse::NoData;
}

void CSIDevice_AMBaseboard::SendCommand(u32 command, u8 poll)
{
  WARN_LOG_FMT(SERIALINTERFACE_AMBB, "Unhandled SendCommand(0x{:08x}, 0x{:02x})", command, poll);
}

void CSIDevice_AMBaseboard::DoState(PointerWrap& p)
{
  for (auto& response : m_responses)
    p.DoArray(response);
  p.Do(m_response_index);
  m_io_ports.DoState(p);
  m_jvs->DoState(p);
  if (m_serial_a)
    m_serial_a->DoState(p);
  if (m_serial_b)
    m_serial_b->DoState(p);
}

}  // namespace SerialInterface
