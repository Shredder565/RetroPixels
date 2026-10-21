// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/DeckReader.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"

namespace Triforce
{

namespace
{
constexpr u8 REPLY_MARK = 0xAA;
constexpr u8 FIRMWARE_TIMEOUT = 240;
constexpr std::size_t VERSION_FIELD_SIZE = 48;

enum DeckCommand : u8
{
  ShutterAuto = 0x61,
  BootVersion = 0x62,
  SensLock = 0x63,
  SensCard = 0x65,
  FirmwareUpdate = 0x66,
  ShutterGet = 0x67,
  CameraCheck = 0x68,
  Shutter = 0x69,
  ProgramChecksum = 0x6B,
  ShutterAlt = 0x6C,
  BootChecksum = 0x6D,
  ShutterLoad = 0x6F,
  ReadCard = 0x72,
  ShutterSave = 0x73,
  SelfTest = 0x74,
  ProgramVersion = 0x76,
};

// A double from the deck file as an integer type: in-range values as a plain
// conversion would give, anything else clamped (NaN as 0).
template <typename T>
T ToInteger(double value)
{
  if (std::isnan(value))
    return 0;
  if (value <= static_cast<double>(std::numeric_limits<T>::min()))
    return std::numeric_limits<T>::min();
  if (value >= static_cast<double>(std::numeric_limits<T>::max()))
    return std::numeric_limits<T>::max();
  return static_cast<T>(value);
}

void PutText(std::vector<u8>& out, std::string_view text, std::size_t field_size)
{
  out.insert(out.end(), text.begin(), text.end());
  out.resize(out.size() + (field_size - text.size()), 0);
}
}  // namespace

std::vector<u8> DeckReader::ReadDeck() const
{
  const std::string path = fmt::format("{}tricard_{}_deck.json", File::GetUserPath(D_TRIUSER_IDX),
                                       SConfig::GetInstance().GetGameID());
  std::string text;
  File::ReadFileToString(path, text);

  picojson::value root;
  const std::string error = picojson::parse(root, text);
  if (!error.empty())
  {
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "LoadCardDeckFromFile: {}", error);
    return {};
  }
  if (!root.is<picojson::object>())
  {
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "LoadCardDeckFromFile: Invalid JSON root object.");
    return {};
  }
  const picojson::value& cards = root.get("cards");
  if (!cards.is<picojson::array>())
  {
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "LoadCardDeckFromFile: Invalid \"cards\" array.");
    return {};
  }

  std::vector<u8> entries;
  for (const picojson::value& card : cards.get<picojson::array>())
  {
    if (!card.is<picojson::object>())
      continue;

    const picojson::value& index = card.get("index");
    if (!index.is<double>())
    {
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "LoadCardDeckFromFile: Invalid \"index\" field.");
      continue;
    }
    const picojson::value& table = card.get("table");
    const picojson::value& quantity = card.get("quantity");

    const u16 card_index = ToInteger<u16>(index.get<double>());
    const u8 card_table = table.is<double>() ? ToInteger<u8>(table.get<double>()) : u8{0};
    const u8 copies = quantity.is<double>() ? ToInteger<u8>(quantity.get<double>()) : u8{1};

    for (u32 i = 0; i != copies; ++i)
    {
      entries.push_back(card_table);
      entries.push_back(static_cast<u8>(card_index >> 8));
      entries.push_back(static_cast<u8>(card_index & 0xFF));
    }
  }
  return entries;
}

void DeckReader::RunDeckCommand(u8 command)
{
  std::vector<u8> reply{REPLY_MARK, command};

  switch (command)
  {
  case SelfTest:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "SelfTest");
    reply.push_back(0x00);
    break;
  case SensLock:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "SensLock");
    reply.push_back(0x01);
    break;
  case ProgramVersion:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ProgramVersion");
    PutText(reply, "Version 1.22,2003/09/19,171-8213B", VERSION_FIELD_SIZE);
    break;
  case BootVersion:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "BootVersion");
    PutText(reply, "Version 1.04,2003/06/17,171-8213B", VERSION_FIELD_SIZE);
    break;
  case ProgramChecksum:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ProgramChecksum");
    reply.insert(reply.end(), {0x89, 0xAB, 0xCD, 0xEF});
    break;
  case BootChecksum:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "BootChecksum");
    reply.insert(reply.end(), {0x01, 0x23, 0x45, 0x67});
    break;
  case CameraCheck:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "CameraCheck");
    reply.resize(reply.size() + 10, 0);
    break;
  case ShutterGet:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ShutterGet");
    reply.insert(reply.end(), {0x49, 0x96, 0x02, 0xD2});  // 1234567890
    break;
  case ShutterAuto:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ShutterAuto");
    reply.insert(reply.end(), {0x49, 0x96, 0x02, 0xD2});
    break;
  case Shutter:
  case ShutterAlt:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "Shutter");
    break;
  case ShutterLoad:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ShutterLoad");
    break;
  case ShutterSave:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ShutterSave");
    break;
  case SensCard:
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "SensCard");
    reply.push_back(0x01);
    break;
  case ReadCard:
  {
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ReadCard");
    const std::vector<u8> entries = ReadDeck();
    reply.push_back(REPLY_MARK);
    reply.push_back(ReadCard);
    reply.push_back(static_cast<u8>(entries.size()));
    reply.insert(reply.end(), entries.begin(), entries.end());
    break;
  }
  case FirmwareUpdate:
  {
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "FirmwareUpdate");
    m_firmware_timeout = FIRMWARE_TIMEOUT;
    const std::string path = File::GetUserPath(D_TRIUSER_IDX) + "card_deck_reader_firmware.bin";
    if (m_firmware_file.Open(path, File::AccessMode::Write))
      NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Writing firmware to: {}", path);
    else
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Failed to open: {}", path);
    break;
  }
  default:
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Unknown command: {:02x}", command);
    break;
  }

  reply.push_back(0x00);
  WriteTxBytes(reply);
}

void DeckReader::RunFirmwareMode()
{
  const std::span<const u8> rx = GetRxByteSpan();
  if (rx.empty())
  {
    if (--m_firmware_timeout == 0)
    {
      static constexpr std::array<u8, 3> done{REPLY_MARK, FirmwareUpdate, 0x00};
      WriteTxBytes(done);
      INFO_LOG_FMT(SERIALINTERFACE_CARD, "Fake FirmwareUpdate done.");
      m_firmware_file.Close();
    }
    return;
  }

  if (m_firmware_file.IsOpen() && !m_firmware_file.Write(rx))
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Failed to write {} bytes to firmware.", rx.size());
  ConsumeRxBytes(rx.size());
  m_firmware_timeout = FIRMWARE_TIMEOUT;
}

void DeckReader::Step()
{
  if (m_firmware_timeout != 0)
  {
    RunFirmwareMode();
    return;
  }

  const std::span<const u8> rx = GetRxByteSpan();
  if (rx.empty())
    return;

  if (rx[0] == 0x00)
  {
    // An IC card request: hand the whole frame to the card reader.
    if (rx.size() < 4)
      return;
    const std::size_t frame_size = static_cast<std::size_t>((rx[2] << 8) | rx[3]) + 5;
    if (rx.size() < frame_size)
      return;
    m_card_reader.WriteRxBytes(rx.first(frame_size));
    ConsumeRxBytes(frame_size);
    return;
  }

  RunDeckCommand(rx[0]);
  ConsumeRxBytes(1);
}

void DeckReader::Update()
{
  Step();
  m_card_reader.Update();
  PassThroughTxBytes(m_card_reader);
}

void DeckReader::DoState(PointerWrap& p)
{
  SerialDevice::DoState(p);
  m_card_reader.DoState(p);
  p.Do(m_firmware_timeout);
  if (p.IsReadMode())
    m_firmware_file.Close();
}

}  // namespace Triforce
