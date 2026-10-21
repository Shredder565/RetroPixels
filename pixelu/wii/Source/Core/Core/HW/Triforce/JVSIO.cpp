// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/JVSIO.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"

namespace Triforce
{

// clang-format off
// Frame status and per-command report bytes; both count up from 1.
enum class JVSIOStatusCode : u8
{
  Okay = 1, UnknownCommand, ChecksumError, ResponseOverflow,
};
enum class JVSIOReportCode : u8
{
  Okay = 1, ParameterSizeError, ParameterDataError, Busy,
};

// Command bytes. Each group is numbered consecutively from its first entry.
enum class JVSIOCommand : u8
{
  // 0x10..0x15: identification.
  IOIdentify = 0x10, CommandRevision, JVSRevision, CommVersion, FeatureCheck, MainID,
  // 0x20..0x2F: inputs.
  SwitchInput = 0x20, CoinInput, AnalogInput, RotaryInput, KeycodeInput, ScreenPositionInput,
  MiscSwitchInput, RemainingPayout = 0x2E, DataRetransmit,
  // 0x30..0x38: outputs.
  CoinCounterDec = 0x30, PayoutCounterInc, GenericOutput, AnalogOutput, CharacterOutput,
  CoinCounterInc, PayoutCounterDec, GenericOutputByte, GenericOutputBit,
  // Vendor and broadcast commands.
  NamcoCommand = 0x70, Reset = 0xF0, SetAddress, CommMethodChange,
};
// clang-format on

// Reads a decoded request (the payload without the checksum) front to back.
class JVSIORequestReader
{
public:
  explicit JVSIORequestReader(std::span<const u8> bytes) : m_bytes{bytes} {}

  std::size_t Remaining() const { return m_bytes.size() - m_pos; }
  bool Empty() const { return Remaining() == 0; }

  std::span<const u8> Peek() const { return m_bytes.subspan(m_pos); }

  std::optional<u8> ReadByte()
  {
    if (Empty())
      return std::nullopt;
    return m_bytes[m_pos++];
  }

  // All `count` bytes or nothing.
  std::optional<std::span<const u8>> ReadBytes(std::size_t count)
  {
    if (Remaining() < count)
      return std::nullopt;
    const std::span<const u8> result = m_bytes.subspan(m_pos, count);
    m_pos += count;
    return result;
  }

  void Skip(std::size_t count) { m_pos += count; }

private:
  std::span<const u8> m_bytes;
  std::size_t m_pos = 0;
};

// Appends data bytes to the response frame being built.
class JVSIOResponseWriter
{
public:
  explicit JVSIOResponseWriter(std::vector<u8>& frame) : m_frame{frame} {}

  void Byte(u8 value) { m_frame.push_back(value); }
  void Bytes(std::span<const u8> values) { m_frame.insert(m_frame.end(), values.begin(), values.end()); }
  void BE16(u16 value)
  {
    Byte(static_cast<u8>(value >> 8));
    Byte(static_cast<u8>(value & 0xFF));
  }

private:
  std::vector<u8>& m_frame;
};

namespace
{
constexpr u8 SYNC = 0xE0;
constexpr u8 MARK = 0xD0;
constexpr u8 BROADCAST = 0xFF;
constexpr u8 HOST = 0x00;

constexpr u8 RESET_MAGIC = 0xD9;

using Status = JVSIOStatusCode;
using Report = JVSIOReportCode;

u8 Sum8(std::span<const u8> bytes)
{
  return static_cast<u8>(std::accumulate(bytes.begin(), bytes.end(), u32{0}));
}

// Unescapes `count` bytes of rx starting at `pos` into `out`. Returns the rx
// position after them, or nothing if rx ends first.
std::optional<std::size_t> Unescape(std::span<const u8> rx, std::size_t pos, std::size_t count,
                                    std::vector<u8>& out)
{
  while (count != 0)
  {
    if (pos >= rx.size())
      return std::nullopt;
    u8 value = rx[pos++];
    if (value == MARK)
    {
      if (pos >= rx.size())
        return std::nullopt;
      value = static_cast<u8>(rx[pos++] + 1);
    }
    out.push_back(value);
    --count;
  }
  return pos;
}

u8 AsNumber(Status s)
{
  return static_cast<u8>(s);
}

u8 AsNumber(Report r)
{
  return static_cast<u8>(r);
}
}  // namespace

JVSIOBoard::JVSIOBoard(IOPorts* io_ports) : m_ports{io_ports}
{
}

void JVSIOBoard::SendLastResponse()
{
  WriteTxByte(SYNC);
  for (const u8 value : m_last_response)
  {
    if (value == SYNC || value == MARK)
    {
      WriteTxByte(MARK);
      WriteTxByte(static_cast<u8>(value - 1));
    }
    else
    {
      WriteTxByte(value);
    }
  }
}

void JVSIOBoard::Update()
{
  const std::span<const bool, IOPorts::COIN_SLOT_COUNT> coins = m_ports->GetCoinInputs();
  for (std::size_t slot = 0; slot != IOPorts::COIN_SLOT_COUNT; ++slot)
  {
    if (coins[slot] && !m_coin_was_down[slot])
      ++m_coins[slot];
    m_coin_was_down[slot] = coins[slot];
  }

  while (!GetRxByteSpan().empty())
  {
    const std::span<const u8> rx = GetRxByteSpan();
    if (rx[0] != SYNC)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "Expected JVSIO_SYNC");
      ConsumeRxBytes(1);
      continue;
    }

    std::vector<u8> header;
    const std::optional<std::size_t> after_header = Unescape(rx, 1, 2, header);
    if (!after_header)
      return;
    const u8 destination = header[0];
    const u8 length = header[1];

    std::vector<u8> payload;
    const std::optional<std::size_t> after_payload =
        Unescape(rx, *after_header, length, payload);
    if (!after_payload)
      return;

    // From here on the frame is used up, whatever it holds.
    const std::size_t frame_size = *after_payload;

    if (length == 0)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "Empty payload");
      ConsumeRxBytes(frame_size);
      continue;
    }

    const u8 expected =
        static_cast<u8>(destination + length + Sum8(std::span(payload).first(length - 1u)));
    if (expected != payload.back())
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "Bad checksum");
      m_last_response = {HOST, 0x02, AsNumber(Status::ChecksumError)};
      m_last_response.push_back(Sum8(m_last_response));
      SendLastResponse();
      ConsumeRxBytes(frame_size);
      continue;
    }

    JVSIORequestReader request{std::span(payload).first(length - 1u)};
    if (destination == BROADCAST)
      HandleBroadcast(request);
    else if (destination == m_address)
      HandleUnicast(request);
    else
      WARN_LOG_FMT(SERIALINTERFACE_JVSIO, "Unexpected destination node: 0x{:02x}", destination);

    if (!request.Empty())
      WARN_LOG_FMT(SERIALINTERFACE_JVSIO, "Excess request bytes: {}", request.Remaining());

    ConsumeRxBytes(frame_size);
  }
}

void JVSIOBoard::HandleBroadcast(JVSIORequestReader& request)
{
  const std::optional<u8> command = request.ReadByte();
  if (!command)
    return;

  switch (static_cast<JVSIOCommand>(*command))
  {
  case JVSIOCommand::Reset:
  {
    const std::optional<u8> magic = request.ReadByte();
    if (!magic)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "Reset: ParameterSizeError");
      return;
    }
    if (*magic == RESET_MAGIC)
    {
      NOTICE_LOG_FMT(SERIALINTERFACE_JVSIO, "Reset");
      m_address = 0;
      m_ports->GetStatusSwitches()[1] |= 0x01;
    }
    m_ports->ResetGenericOutputs();
    m_last_response.clear();
    return;
  }
  case JVSIOCommand::SetAddress:
  {
    const std::optional<u8> address = request.ReadByte();
    if (!address)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "SetAddress: ParameterSizeError");
      return;
    }
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "SetAddress: {}", *address);
    m_address = *address;
    m_ports->GetStatusSwitches()[1] &= static_cast<u8>(0xFE);
    m_last_response = {HOST, 0x03, AsNumber(Status::Okay), AsNumber(Report::Okay)};
    m_last_response.push_back(Sum8(m_last_response));
    SendLastResponse();
    return;
  }
  default:
    ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "ProcessBroadcastFrame: UnknownCommand: {:02x}",
                  *command);
    return;
  }
}

void JVSIOBoard::HandleUnicast(JVSIORequestReader& request)
{
  if (!request.Empty() &&
      request.Peek()[0] == static_cast<u8>(JVSIOCommand::DataRetransmit))
  {
    request.Skip(1);
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "DataRetransmit");
    SendLastResponse();
    return;
  }

  // Destination, count and status are filled in when the frame is finished.
  m_last_response = {HOST, 0x00, 0x00};
  JVSIOResponseWriter response{m_last_response};

  while (true)
  {
    if (request.Empty())
    {
      FinishResponse(Status::Okay);
      return;
    }

    const std::size_t report_at = m_last_response.size();
    m_last_response.push_back(0);
    const u8 command = *request.ReadByte();

    const HandlerResponse result =
        HandleCommand(static_cast<JVSIOCommand>(command), FrameContext{request, response});
    if (!result)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "HandleCommand: cmd:{:02x} StatusCode:{}", command,
                    AsNumber(result.error()));
      FinishResponse(result.error());
      return;
    }

    m_last_response[report_at] = AsNumber(*result);
    if (*result != Report::Okay)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "HandleCommand: cmd:{:02x} ReportCode:{}", command,
                    AsNumber(*result));
      FinishResponse(Status::Okay);
      return;
    }
  }
}

void JVSIOBoard::FinishResponse(JVSIOStatusCode status)
{
  const std::size_t count = m_last_response.size() - 1;
  if (count > 0xFF)
  {
    // Too long to describe; only the zeroed header goes out.
    m_last_response.resize(3);
  }
  else
  {
    m_last_response[1] = static_cast<u8>(count);
    m_last_response[2] = AsNumber(status);
    m_last_response.push_back(Sum8(m_last_response));
  }
  SendLastResponse();
}

JVSIOBoard::HandlerResponse JVSIOBoard::HandleCommand(JVSIOCommand cmd, FrameContext ctx)
{
  JVSIORequestReader& request = ctx.request;
  JVSIOResponseWriter& response = ctx.response;

  switch (cmd)
  {
  case JVSIOCommand::CommandRevision:
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "CommandRevision");
    response.Byte(0x11);
    return Report::Okay;

  case JVSIOCommand::JVSRevision:
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "JVSRevision");
    response.Byte(0x20);
    return Report::Okay;

  case JVSIOCommand::CommVersion:
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "CommVersion");
    response.Byte(0x10);
    return Report::Okay;

  case JVSIOCommand::MainID:
  {
    const std::span<const u8> rest = request.Peek();
    const auto nul = std::ranges::find(rest, u8{0});
    if (nul == rest.end())
      return Report::ParameterDataError;
    const std::size_t length = static_cast<std::size_t>(nul - rest.begin());
    const std::string_view text(reinterpret_cast<const char*>(rest.data()), length);
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "MainID: {}", text);
    request.Skip(length + 1);
    return Report::Okay;
  }

  case JVSIOCommand::SwitchInput:
  {
    const std::optional<std::span<const u8>> params = request.ReadBytes(2);
    if (!params)
      return Report::ParameterSizeError;
    const u8 players = (*params)[0];
    const u8 bytes_per_player = (*params)[1];
    DEBUG_LOG_FMT(SERIALINTERFACE_JVSIO, "SwitchInput: player_count:{} bytes_per_player:{}",
                  players, bytes_per_player);

    response.Byte(*m_ports->GetSystemInputs());
    for (u32 player = 0; player != players; ++player)
    {
      std::size_t written = 0;
      if (player < IOPorts::PLAYER_COUNT)
      {
        const std::span<const u8> switches = std::as_const(*m_ports).GetSwitchInputs(player);
        written = std::min<std::size_t>(bytes_per_player, switches.size());
        response.Bytes(switches.first(written));
      }
      else
      {
        // Only two players have inputs; the rest read as zero (spec 13.4). The check is kept so
        // the request is still reported.
        ASSERT(player < IOPorts::PLAYER_COUNT);
      }
      for (; written < bytes_per_player; ++written)
        response.Byte(0);
    }
    return Report::Okay;
  }

  case JVSIOCommand::CoinInput:
  {
    const std::optional<u8> slots = request.ReadByte();
    if (!slots)
      return Report::ParameterSizeError;
    DEBUG_LOG_FMT(SERIALINTERFACE_JVSIO, "CoinInput: slot_count:{}", *slots);
    for (std::size_t slot = 0; slot != *slots; ++slot)
    {
      if (slot < IOPorts::COIN_SLOT_COUNT)
        response.BE16(static_cast<u16>(m_coins[slot] & 0x3FFF));
      else
        response.BE16(static_cast<u16>(2u << 14));
    }
    return Report::Okay;
  }

  case JVSIOCommand::AnalogInput:
  {
    const std::optional<u8> channels = request.ReadByte();
    if (!channels)
      return Report::ParameterSizeError;
    DEBUG_LOG_FMT(SERIALINTERFACE_JVSIO, "AnalogInput: channel_count:{}", *channels);
    const std::span<const u16> analog = std::as_const(*m_ports).GetAnalogInputs();
    for (std::size_t channel = 0; channel != *channels; ++channel)
      response.BE16(channel < analog.size() ? analog[channel] : IOPorts::NEUTRAL_ANALOG_VALUE);
    return Report::Okay;
  }

  case JVSIOCommand::CoinCounterDec:
  case JVSIOCommand::CoinCounterInc:
  {
    const std::optional<std::span<const u8>> params = request.ReadBytes(3);
    if (!params)
      return Report::ParameterSizeError;
    const u8 slot = (*params)[0];
    int adjustment = ((*params)[1] << 8) | (*params)[2];
    if (cmd == JVSIOCommand::CoinCounterDec)
      adjustment = -adjustment;
    NOTICE_LOG_FMT(SERIALINTERFACE_JVSIO, "CoinCounter: slot:{} adjustment:{}", slot, adjustment);
    if (slot < IOPorts::COIN_SLOT_COUNT)
      m_coins[slot] = static_cast<u16>(std::clamp(m_coins[slot] + adjustment, 0, 0x3FFF));
    return Report::Okay;
  }

  case JVSIOCommand::GenericOutput:
  {
    const std::optional<u8> count = request.ReadByte();
    if (!count)
      return Report::ParameterSizeError;
    const std::optional<std::span<const u8>> bits = request.ReadBytes(*count);
    if (!bits)
      return Report::ParameterSizeError;
    DEBUG_LOG_FMT(SERIALINTERFACE_JVSIO, "GenericOutput: {}", fmt::join(*bits, " "));
    m_ports->SetGenericOutputs(*bits);
    return Report::Okay;
  }

  default:
    return std::unexpected(Status::UnknownCommand);
  }
}

namespace
{
using Reply = std::expected<JVSIOReportCode, JVSIOStatusCode>;

Reply ReplyIdentify(std::string_view name, JVSIOResponseWriter& response)
{
  INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "IOIdentify");
  for (const char c : name)
    response.Byte(static_cast<u8>(c));
  response.Byte(0);
  return Report::Okay;
}

Reply ReplyFeatures(std::span<const u8> features, JVSIOResponseWriter& response)
{
  INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "FeatureCheck");
  response.Bytes(features);
  return Report::Okay;
}
}  // namespace

JVSIOBoard::HandlerResponse Namco_FCA_JVSIOBoard::HandleCommand(JVSIOCommand cmd,
                                                                FrameContext ctx)
{
  static constexpr std::array<u8, 21> features{
      0x01, 0x01, 0x10, 0x00,  // switches: 1 player, 16 each
      0x02, 0x02, 0x00, 0x00,  // coins: 2 slots
      0x03, 0x07, 0x00, 0x00,  // analog: 7 channels
      0x12, 0x06, 0x00, 0x00,  // generic outputs: 6
      0x13, 0x04, 0x00, 0x00,  // analog outputs: 4
      0x00,
  };

  switch (cmd)
  {
  case JVSIOCommand::IOIdentify:
    return ReplyIdentify("namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder",
                         ctx.response);
  case JVSIOCommand::FeatureCheck:
    return ReplyFeatures(features, ctx.response);
  case JVSIOCommand::NamcoCommand:
  {
    const std::optional<u8> sub = ctx.request.ReadByte();
    if (!sub)
      return Report::ParameterSizeError;
    if (*sub != 0x18)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_JVSIO, "Unknown NAMCO_COMMAND: {:02x}", *sub);
      return Report::ParameterDataError;
    }
    const std::optional<std::span<const u8>> args = ctx.request.ReadBytes(4);
    if (!args)
      return Report::ParameterSizeError;
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "NAMCO_COMMAND({:02x}): {:02x}", *sub,
                 fmt::join(*args, " "));
    ctx.response.Byte(0xFF);
    return Report::Okay;
  }
  default:
    return JVSIOBoard::HandleCommand(cmd, ctx);
  }
}

JVSIOBoard::HandlerResponse Sega_837_13551_JVSIOBoard::HandleCommand(JVSIOCommand cmd,
                                                                     FrameContext ctx)
{
  static constexpr std::array<u8, 17> features{
      0x01, 0x02, 0x0D, 0x00,  // switches: 2 players, 13 each
      0x02, 0x02, 0x00, 0x00,  // coins: 2 slots
      0x03, 0x08, 0x00, 0x00,  // analog: 8 channels
      0x12, 0x06, 0x00, 0x00,  // generic outputs: 6
      0x00,
  };

  switch (cmd)
  {
  case JVSIOCommand::IOIdentify:
    return ReplyIdentify("SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10",
                         ctx.response);
  case JVSIOCommand::FeatureCheck:
    return ReplyFeatures(features, ctx.response);
  default:
    return JVSIOBoard::HandleCommand(cmd, ctx);
  }
}

JVSIOBoard::HandlerResponse Sega_837_13844_JVSIOBoard::HandleCommand(JVSIOCommand cmd,
                                                                     FrameContext ctx)
{
  static constexpr std::array<u8, 17> features{
      0x01, 0x02, 0x0C, 0x00,  // switches: 2 players, 12 each
      0x02, 0x02, 0x00, 0x00,  // coins: 2 slots
      0x03, 0x08, 0x00, 0x00,  // analog: 8 channels
      0x12, 0x16, 0x00, 0x00,  // generic outputs: 22
      0x00,
  };

  switch (cmd)
  {
  case JVSIOCommand::IOIdentify:
    return ReplyIdentify("SEGA ENTERPRISES,LTD.;837-13844-01 I/O CNTL BD2 ;Ver1.00;99/07",
                         ctx.response);
  case JVSIOCommand::FeatureCheck:
    return ReplyFeatures(features, ctx.response);
  default:
    return JVSIOBoard::HandleCommand(cmd, ctx);
  }
}

void JVSIOBoard::DoState(PointerWrap& p)
{
  SerialDevice::DoState(p);
  p.Do(m_address);
  p.Do(m_last_response);
  p.DoArray(m_coins);
  p.DoArray(m_coin_was_down);
}

}  // namespace Triforce
