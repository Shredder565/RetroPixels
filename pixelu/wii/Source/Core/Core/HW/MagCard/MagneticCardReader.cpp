// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/MagCard/MagneticCardReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"

namespace MagCard
{

namespace
{
using namespace std::chrono_literals;

constexpr u8 STX = 0x02;
constexpr u8 ETX = 0x03;
constexpr u8 ENQ = 0x05;
constexpr u8 ACK = 0x06;
constexpr u8 NAK = 0x15;

// Every ENQ advances the simulation by this much.
constexpr auto POLL_TIME = 50ms;
// The player puts a card in after this long without one ...
constexpr auto INSERT_DELAY = 500ms;
// ... and takes an ejected card this long after the reader pushed it out.
constexpr auto TAKE_DELAY = 100ms;

constexpr int MESSAGE_TIME = 4000;

constexpr std::string_view VERSION_TEXT = "AP:S1234-5678,OS:S9012-3456,0000";

// Mode bytes.
constexpr u8 MODE_STANDARD = 0x30;
constexpr u8 MODE_DISPENSE_DEFAULT = 0x31;
constexpr u8 MODE_CHECK_OR_CAPTURE = 0x32;

constexpr u8 CMD_CANCEL = 0x40;

std::string_view PositionName(u8 position)
{
  static constexpr std::array<std::string_view, 5> names{
      "NO_CARD", "READ_WRITE_HEAD", "THERMAL_HEAD", "DISPENSER_THERMAL", "ENTRY_SLOT"};
  return position < names.size() ? names[position] : std::string_view{};
}

// The tracks a track selector byte names, in order.
std::vector<std::size_t> SelectedTracks(u8 selector)
{
  switch (selector)
  {
  case 0x30:
    return {0};
  case 0x31:
    return {1};
  case 0x32:
    return {2};
  case 0x33:
    return {0, 1};
  case 0x34:
    return {0, 2};
  case 0x35:
    return {1, 2};
  case 0x36:
    return {0, 1, 2};
  default:
    return {};
  }
}
}  // namespace

MagneticCardReader::MagneticCardReader(Settings* settings) : m_card_settings{settings}
{
}

MagneticCardReader::~MagneticCardReader()
{
  if (IsCardLoaded())
    WARN_LOG_FMT(SERIALINTERFACE_CARD, "Shutdown before ejecting card!");
}

// ---- Small helpers ---------------------------------------------------------------------------

bool MagneticCardReader::IsCardPresent() const
{
  return m_status.r != R::NO_CARD;
}

bool MagneticCardReader::IsCardLoaded() const
{
  return IsCardPresent() && m_status.r != R::ENTRY_SLOT;
}

bool MagneticCardReader::IsReadyForCard()
{
  return !IsCardPresent();
}

void MagneticCardReader::FinishCommand()
{
  m_step = 0;
  if (m_status.s != S::ILLEGAL_COMMAND)
    m_status.s = S::NO_JOB;
}

void MagneticCardReader::SetPError(P error_code)
{
  WARN_LOG_FMT(SERIALINTERFACE_CARD, "SetPError: {:02x}", static_cast<u8>(error_code));
  m_status.p = error_code;
  FinishCommand();
}

void MagneticCardReader::SetSError(S error_code)
{
  WARN_LOG_FMT(SERIALINTERFACE_CARD, "SetSError: {:02x}", static_cast<u8>(error_code));
  m_status.s = error_code;
  FinishCommand();
}

void MagneticCardReader::MoveCard(R new_position)
{
  if (m_status.r == new_position)
    return;
  INFO_LOG_FMT(SERIALINTERFACE_CARD, "MoveCard: {}", PositionName(static_cast<u8>(new_position)));
  m_status.r = new_position;
  m_machine_time = {};
}

void MagneticCardReader::ClearTracks()
{
  m_tracks.fill(std::nullopt);
}

void MagneticCardReader::EjectCard()
{
  if (!IsCardLoaded())
    return;
  NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Ejecting card.");
  ClearTracks();
  MoveCard(R::ENTRY_SLOT);
  Core::DisplayMessage("Ejected Magnetic Card", MESSAGE_TIME);
}

void MagneticCardReader::DispenseCard()
{
  if (IsCardLoaded())
  {
    SetSError(S::ILLEGAL_COMMAND);
    return;
  }
  if (m_card_settings->report_dispenser_empty)
  {
    m_status.s = S::DISPENSER_EMPTY;
    return;
  }
  NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Dispensing a new card.");
  Core::DisplayMessage("Created Magnetic Card", MESSAGE_TIME);
  MoveCard(R::DISPENSER_THERMAL);
}

void MagneticCardReader::AppendTrack(std::size_t index)
{
  const auto& track = *m_tracks[index];
  m_reply.insert(m_reply.end(), track.begin(), track.end());
}

// ---- Card file -------------------------------------------------------------------------------

std::string MagneticCardReader::CardFilePath() const
{
  return m_card_settings->card_path + m_card_settings->card_name;
}

bool MagneticCardReader::CardFileExists() const
{
  return File::Exists(CardFilePath());
}

void MagneticCardReader::LoadCardFile()
{
  NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Reading card data from: {}", m_card_settings->card_name);
  ClearTracks();

  File::DirectIOFile file(CardFilePath(), File::AccessMode::Read);
  const u64 size = file.GetSize();
  if (size == 0 || size % TRACK_SIZE != 0 || size > TRACK_SIZE * NUM_TRACKS)
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "ReadCardFile: Incorrect card file size: {}", size);

  const std::size_t count = static_cast<std::size_t>(std::min<u64>(size / TRACK_SIZE, NUM_TRACKS));
  for (std::size_t k = 0; k != count; ++k)
  {
    auto& track = m_tracks[k].emplace();
    if (!file.Read(track))
    {
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "File read failed.");
      m_tracks[k].reset();
      return;
    }
    DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "ReadCardFile: track:{} data:{:02x}", k,
                  fmt::join(track, ""));
  }
}

void MagneticCardReader::SaveCardFile()
{
  NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Writing card data to: {}", m_card_settings->card_name);

  File::DirectIOFile file(CardFilePath(), File::AccessMode::Write);
  for (std::size_t k = 0; k != NUM_TRACKS; ++k)
  {
    if (!m_tracks[k])
    {
      DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "WriteCardFile: Skipping empty track:{}", k);
      continue;
    }
    if (!file.OffsetWrite(k * TRACK_SIZE, std::span<const u8>(*m_tracks[k])))
    {
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "File write failed.");
      return;
    }
  }
}

// ---- Wire protocol ---------------------------------------------------------------------------

void MagneticCardReader::Update()
{
  while (!GetRxByteSpan().empty())
  {
    const std::span<const u8> rx = GetRxByteSpan();
    switch (rx[0])
    {
    case ENQ:
      DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "Process: ENQ");
      StepCommand();
      StepPerson();
      SendStatus();
      ConsumeRxBytes(1);
      break;

    case STX:
    {
      if (rx.size() < 2)
        return;
      const std::size_t size = rx[1];
      if (rx.size() < size + 2)
        return;
      if (AcceptPacket(rx.subspan(2, size)))
      {
        DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "Writing ACK");
        WriteTxByte(ACK);
      }
      else
      {
        WARN_LOG_FMT(SERIALINTERFACE_CARD, "Writing NACK");
        WriteTxByte(NAK);
      }
      ConsumeRxBytes(size + 2);
      break;
    }

    default:
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Process: Unexpected {:02x}", rx[0]);
      ConsumeRxBytes(1);
      break;
    }
  }
}

bool MagneticCardReader::AcceptPacket(std::span<const u8> packet)
{
  const std::size_t size = packet.size();
  if (size < 6)
  {
    WARN_LOG_FMT(SERIALINTERFACE_CARD, "ReceivePacket: Bad packet size!");
    return false;
  }
  if (packet[size - 2] != ETX)
  {
    WARN_LOG_FMT(SERIALINTERFACE_CARD, "ReceivePacket: Missing END_OF_TEXT!");
    return false;
  }
  u8 check = static_cast<u8>(size);
  for (const u8 b : packet)
    check ^= b;
  if (check != 0)
  {
    WARN_LOG_FMT(SERIALINTERFACE_CARD, "ReceivePacket: Bad checksum!");
    return false;
  }

  DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "ReceivePacket: {:02x}", fmt::join(packet, " "));

  if (IsRunning())
  {
    if (packet[0] == CMD_CANCEL)
    {
      INFO_LOG_FMT(SERIALINTERFACE_CARD, "ReceivePacket: Cancelling command: {:02x}", m_command);
    }
    else
    {
      WARN_LOG_FMT(SERIALINTERFACE_CARD,
                   "ReceivePacket: Running command: {:02x} interrupted by command: {:02x}",
                   m_command, packet[0]);
    }
  }

  // Command, three host status bytes, parameters, ETX, check byte.
  m_command = packet[0];
  m_current_packet.assign(packet.begin() + 4, packet.end() - 2);
  m_status.SoftReset();
  m_status.s = S::RUNNING_COMMAND;
  m_step = 1;
  m_reply.clear();
  return true;
}

void MagneticCardReader::SendStatus()
{
  std::vector<u8> body{static_cast<u8>(m_reply.size() + 6), m_command, GetPositionValue(),
                       static_cast<u8>(m_status.p), static_cast<u8>(m_status.s)};
  body.insert(body.end(), m_reply.begin(), m_reply.end());
  body.push_back(ETX);
  u8 check = 0;
  for (const u8 b : body)
    check ^= b;

  WriteTxByte(STX);
  WriteTxBytes(body);
  WriteTxByte(check);
}

// ---- Simulation ------------------------------------------------------------------------------

void MagneticCardReader::StepCommand()
{
  m_machine_time += POLL_TIME;
  if (!IsRunning())
    return;

  DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "StepStateMachine command: {:02x}", m_command);
  RunCommand();
  if (IsRunning())
    ++m_step;
}

void MagneticCardReader::StepPerson()
{
  m_person_time += POLL_TIME;
  if (IsCardLoaded())
    return;

  if (!IsCardPresent())
  {
    if (m_card_settings->insert_card_when_possible && IsReadyForCard() &&
        m_person_time > INSERT_DELAY && CardFileExists())
    {
      NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Inserting card.");
      m_status.r = R::ENTRY_SLOT;
      m_person_time = {};
    }
    return;
  }

  // The card sits in the entry slot.
  const bool machine_moved_last = m_machine_time < m_person_time;
  if ((machine_moved_last && m_machine_time > TAKE_DELAY) || !CardFileExists())
  {
    NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Removing card.");
    m_status.r = R::NO_CARD;
    m_person_time = {};
  }
}

// ---- Commands --------------------------------------------------------------------------------

void MagneticCardReader::RunCommand()
{
  switch (m_command)
  {
  case 0x10:
    Command_10_Initialize();
    break;
  case 0x20:  // ReadStatus
  case 0x40:  // Cancel
  case 0x78:  // PrintSettings
  case 0x7A:  // RegisterFont
  case 0xC0:  // ControlLED
  case 0xC1:  // SetPrintRetry
    m_status.SoftReset();
    FinishCommand();
    break;
  case 0x33:
    ReadData();
    break;
  case 0x35:
    GetData();
    break;
  case 0x53:
    WriteData();
    break;
  case 0x7B:  // PrintImage
    if (!IsCardPresent())
    {
      SetPError(P::PRINT_ERR);
      break;
    }
    m_status.SoftReset();
    FinishCommand();
    break;
  case 0x7C:  // PrintLine
    PrintLine(true);
    break;
  case 0x7D:  // Erase
    PrintLine(false);
    break;
  case 0x7E:
    PrintBarcode();
    break;
  case 0x80:  // EjectCard
    if (m_step >= 3)
    {
      EjectCard();
      FinishCommand();
    }
    break;
  case 0xA0:
    Clean();
    break;
  case 0xB0:
    Dispense();
    break;
  case 0xD0:
    Command_D0_ControlShutter();
    break;
  case 0xF0:  // GetVersion
    m_reply.insert(m_reply.end(), VERSION_TEXT.begin(), VERSION_TEXT.end());
    m_status.SoftReset();
    FinishCommand();
    break;
  default:
    WARN_LOG_FMT(SERIALINTERFACE_CARD, "HandlePacket: Unhandled command {:02x}", m_command);
    SetSError(S::ILLEGAL_COMMAND);
    break;
  }
}

void MagneticCardReader::Command_10_Initialize()
{
  EjectCard();
  m_status.SoftReset();
  FinishCommand();
}

void MagneticCardReader::Command_D0_ControlShutter()
{
  SetSError(S::ILLEGAL_COMMAND);
}

void MagneticCardReader::ReadData()
{
  if (m_current_packet.size() < 3)
  {
    SetPError(P::SYSTEM_ERR);
    return;
  }
  const u8 mode = m_current_packet[0];
  const u8 selector = m_current_packet[2];

  if (m_step == 1)
  {
    DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "Command_33_ReadData: mode {:02x} track {:02x}", mode,
                  selector);
    if (mode == MODE_CHECK_OR_CAPTURE)
      INFO_LOG_FMT(SERIALINTERFACE_CARD, "Command_33_ReadData: CardCapture");
  }
  else if (m_step == 2)
  {
    if (mode == MODE_CHECK_OR_CAPTURE)
    {
      if (!IsCardPresent())
      {
        // Keep waiting at this step until the player puts a card in.
        m_status.s = S::WAITING_FOR_CARD;
        --m_step;
      }
      else if (m_status.r != R::READ_WRITE_HEAD)
      {
        NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Capturing Card.");
        MoveCard(R::READ_WRITE_HEAD);
        LoadCardFile();
        Core::DisplayMessage("Inserted Magnetic Card", MESSAGE_TIME);
      }
    }
    else
    {
      const std::vector<std::size_t> tracks = SelectedTracks(selector);
      if (!IsCardLoaded() || tracks.empty())
      {
        SetPError(P::ILLEGAL_ERR);
      }
      else
      {
        for (const std::size_t t : tracks)
        {
          if (!m_tracks[t])
          {
            SetPError(P::READ_ERR);
            WARN_LOG_FMT(SERIALINTERFACE_CARD, "Command_33_ReadData: Empty track {:02x}", t);
            break;
          }
          AppendTrack(t);
        }
      }
    }
  }

  if (m_step >= 2 && IsRunning())
    FinishCommand();
}

void MagneticCardReader::GetData()
{
  if (m_step == 2)
  {
    if (!IsCardLoaded())
    {
      SetPError(P::ILLEGAL_ERR);
    }
    else
    {
      for (std::size_t t = 0; t != NUM_TRACKS; ++t)
      {
        if (!m_tracks[t])
        {
          SetPError(P::READ_ERR);
          break;
        }
        AppendTrack(t);
      }
    }
  }

  if (m_step >= 2 && IsRunning())
    FinishCommand();
}

void MagneticCardReader::WriteData()
{
  if (m_current_packet.size() < 3)
  {
    SetPError(P::SYSTEM_ERR);
    return;
  }
  if (m_step < 2)
    return;

  const u8 mode = m_current_packet[0];
  const std::vector<std::size_t> tracks = SelectedTracks(m_current_packet[2]);
  const std::span<const u8> data = std::span<const u8>(m_current_packet).subspan(3);

  if (mode != MODE_STANDARD || !IsCardLoaded() || tracks.empty() ||
      data.size() != TRACK_SIZE * tracks.size())
  {
    WARN_LOG_FMT(SERIALINTERFACE_CARD, "Command_53_WriteData: Failed.");
    SetPError(P::ILLEGAL_ERR);
    return;
  }

  INFO_LOG_FMT(SERIALINTERFACE_CARD, "Command_53_WriteData: Writing {} track(s) to memory.",
               tracks.size());
  for (std::size_t k = 0; k != tracks.size(); ++k)
  {
    auto& track = m_tracks[tracks[k]].emplace();
    std::ranges::copy(data.subspan(k * TRACK_SIZE, TRACK_SIZE), track.begin());
  }
  Core::DisplayMessage("Writing Magnetic Card", MESSAGE_TIME);
  SaveCardFile();
  FinishCommand();
}

void MagneticCardReader::PrintLine(bool check_size)
{
  if (check_size && m_current_packet.size() < 3)
  {
    SetPError(P::SYSTEM_ERR);
    return;
  }

  if (m_step == 1)
  {
    if (!IsCardPresent())
      SetPError(P::PRINT_ERR);
    else
      MoveCard(R::THERMAL_HEAD);
  }
  else if (m_step == 2)
  {
    MoveCard(R::READ_WRITE_HEAD);
  }

  if (m_step >= 2 && IsRunning())
    FinishCommand();
}

void MagneticCardReader::PrintBarcode()
{
  if (m_step == 2 && !IsCardPresent())
    SetPError(P::ILLEGAL_ERR);

  if (m_step >= 2 && IsRunning())
    FinishCommand();
}

void MagneticCardReader::Clean()
{
  if (m_step == 2)
  {
    if (!IsCardLoaded())
    {
      NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Inserting cleaning card.");
      Core::DisplayMessage("Inserting cleaning card", MESSAGE_TIME);
      MoveCard(R::THERMAL_HEAD);
    }
  }
  else if (m_step == 3)
  {
    EjectCard();
  }

  if (m_step >= 3 && IsRunning())
    FinishCommand();
}

void MagneticCardReader::Dispense()
{
  const u8 mode = m_current_packet.empty() ? MODE_DISPENSE_DEFAULT : m_current_packet[0];

  if (m_step == 2)
  {
    if (mode == MODE_CHECK_OR_CAPTURE)
      m_status.s = m_card_settings->report_dispenser_empty ? S::DISPENSER_EMPTY : S::CARD_FULL;
    else
      DispenseCard();
  }
  else if (m_step == 3)
  {
    if (m_status.r == R::DISPENSER_THERMAL)
      MoveCard(R::READ_WRITE_HEAD);
  }

  if (m_step >= 3 && IsRunning())
    FinishCommand();
}

// ---- Savestate -------------------------------------------------------------------------------

void MagneticCardReader::DoState(PointerWrap& p)
{
  SerialDevice::DoState(p);
  p.Do(m_status.r);
  p.Do(m_status.p);
  p.Do(m_status.s);
  p.Do(m_command);
  p.Do(m_reply);
  p.Do(m_current_packet);
  p.Do(m_step);
  for (Track& track : m_tracks)
    p.Do(track);
  p.Do(m_machine_time);
  p.Do(m_person_time);
}

}  // namespace MagCard
