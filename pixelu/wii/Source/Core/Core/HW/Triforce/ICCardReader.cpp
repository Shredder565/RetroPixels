// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/ICCardReader.h"

#include <algorithm>
#include <optional>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/HW/DVD/AMMediaboard.h"

namespace Triforce
{

namespace
{
constexpr std::size_t PAGE_SIZE = 8;
constexpr std::size_t PAGE_COUNT = 256;
constexpr std::size_t CARD_SIZE = PAGE_SIZE * PAGE_COUNT;
constexpr u16 PAGE_MASK = 0xFF;

// Page 4 holds the card's serial number and cannot be written.
constexpr std::size_t SERIAL_OFFSET = 32;
constexpr std::size_t SERIAL_END = 40;
constexpr std::size_t USE_COUNT_OFFSET = 0x28;

constexpr u16 STATUS_OK = 0x0000;
constexpr u16 STATUS_FAILED = 0x0080;

constexpr u8 REPLY_MARK = 0x10;

// Request framing: mark, command, BE16 payload size, payload, check byte.
constexpr std::size_t FRAME_OVERHEAD = 5;

constexpr u8 EJECT_TIME = 120;
constexpr u8 EJECT_REPORT_THRESHOLD = 60;

enum Command : u8
{
  GetStatus = 0x10,
  SetBaudrate = 0x11,
  FieldOn = 0x14,
  FieldOff = 0x15,
  Eject = 0x16,
  InsertCheck = 0x20,
  AntiCollision = 0x21,
  SelectCard = 0x22,
  ReadPage = 0x24,
  WritePage = 0x25,
  DecreaseUseCount = 0x26,
  HaltCard = 0x27,
  ReadUseCount = 0x33,
  ReadPages = 0x34,
  WritePages = 0x35,
};

u16 BE16At(std::span<const u8> bytes, std::size_t at)
{
  return static_cast<u16>((bytes[at] << 8) | bytes[at + 1]);
}

void PutBE16(std::vector<u8>& out, u16 value)
{
  out.push_back(static_cast<u8>(value >> 8));
  out.push_back(static_cast<u8>(value & 0xFF));
}

u8 Xor(std::span<const u8> bytes)
{
  u8 x = 0;
  for (const u8 b : bytes)
    x ^= b;
  return x;
}
}  // namespace

struct ICCardReader::Tag
{
  using UID = std::array<u8, PAGE_SIZE>;

  std::array<u8, CARD_SIZE> data{};
  std::string path;
  UID uid{};
  bool halted = false;

  // Saves bytes [offset, offset + size) to the tag's file.
  void Flush(std::size_t offset, std::size_t size) const
  {
    File::DirectIOFile file(path, File::AccessMode::Write, File::OpenMode::Always);
    if (!file.OffsetWrite(offset, std::span<const u8>(data).subspan(offset, size)))
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "FlushData: Failed to write to: {}", path);
  }

  bool Load()
  {
    File::IOFile file(path, "rb");
    if (!File::Exists(path))
      return false;

    const u64 size = file.GetSize();
    if (size > CARD_SIZE)
      WARN_LOG_FMT(SERIALINTERFACE_CARD, "Unexpected size of {} for file: {}", size, path);

    const std::size_t to_read = static_cast<std::size_t>(std::min<u64>(size, CARD_SIZE));
    if (!file.ReadBytes(data.data(), to_read))
    {
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Failed to read from file: {}", path);
      return false;
    }
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "Loaded {} bytes from file: {}", to_read, path);
    return true;
  }

  void Initialize()
  {
    if (Load())
      return;

    NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "Creating new IC Card data.");
    data[SERIAL_OFFSET] = 0x95;
    data[SERIAL_OFFSET + 1] = 0x71;
    switch (AMMediaboard::GetGameType())
    {
    case AMMediaboard::KeyOfAvalon:
    {
      u32 digits = 0;
      for (std::size_t i = SERIAL_OFFSET + 4; i != SERIAL_END; ++i)
        digits += u32{data[i]} / 16 + u32{data[i]} % 16;
      data[SERIAL_OFFSET + 2] = 0x26;
      data[SERIAL_OFFSET + 3] = static_cast<u8>(0x40 | (digits % 10));
      break;
    }
    case AMMediaboard::VirtuaStriker4:
    case AMMediaboard::VirtuaStriker4_2006:
      data[SERIAL_OFFSET + 2] = 0x44;
      data[SERIAL_OFFSET + 3] = 0x00;
      break;
    case AMMediaboard::GekitouProYakyuu:
      data[SERIAL_OFFSET + 2] = 0x76;
      data[SERIAL_OFFSET + 3] = 0x50;
      break;
    default:
      break;
    }
    data[USE_COUNT_OFFSET] = 0xFF;
    data[USE_COUNT_OFFSET + 1] = 0xFF;
    Flush(SERIAL_OFFSET, 16);
  }

  // Nothing on failure (or for zero pages).
  std::optional<std::span<const u8>> Read(u16 page, u16 count) const
  {
    const std::size_t offset = std::size_t{page} * PAGE_SIZE;
    const std::size_t size = std::size_t{count} * PAGE_SIZE;
    if (offset + size > CARD_SIZE)
    {
      WARN_LOG_FMT(SERIALINTERFACE_CARD, "ReadData: Attempt to read beyond end of card.");
      return std::nullopt;
    }
    if (size == 0)
      return std::nullopt;
    const std::span<const u8> result = std::span<const u8>(data).subspan(offset, size);
    DEBUG_LOG_FMT(SERIALINTERFACE_CARD, "\n{}", fmt::join(result, " "));
    return result;
  }

  bool Write(u16 page, std::span<const u8> bytes)
  {
    const std::size_t offset = std::size_t{page} * PAGE_SIZE;
    if (offset < SERIAL_END && offset + bytes.size() > SERIAL_OFFSET)
    {
      WARN_LOG_FMT(SERIALINTERFACE_CARD, "WriteData: Read-only page.");
      return false;
    }
    if (offset + bytes.size() > CARD_SIZE)
    {
      WARN_LOG_FMT(SERIALINTERFACE_CARD, "WriteData: Attempt to write beyond end of card.");
      return false;
    }
    std::ranges::copy(bytes, data.begin() + static_cast<std::ptrdiff_t>(offset));
    Flush(offset, bytes.size());
    return true;
  }

  u16 DecreaseUseCount(u16 page, u16 amount)
  {
    const std::size_t offset = std::size_t{page} * PAGE_SIZE;
    const u16 before = BE16At(data, offset);
    const u16 after = before > amount ? static_cast<u16>(before - amount) : u16{0};
    NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "DecreaseUseCount: {} -> {}", before, after);
    data[offset] = static_cast<u8>(after >> 8);
    data[offset + 1] = static_cast<u8>(after & 0xFF);
    Flush(offset, 2);
    return after;
  }
};

ICCardReader::ICCardReader(u8 slot_index) : m_slot_index{slot_index}
{
}

ICCardReader::~ICCardReader() = default;

void ICCardReader::MakeTags(std::size_t count)
{
  m_tags.clear();
  const std::string game_id = SConfig::GetInstance().GetGameID();
  for (std::size_t i = 0; i != count; ++i)
  {
    std::string name = fmt::format("slot{}", m_slot_index + 1);
    if (i > 0)
      name.push_back(static_cast<char>('a' + i));

    auto tag = std::make_unique<Tag>();
    tag->path = fmt::format("{}tricard_{}_{}.bin", File::GetUserPath(D_TRIUSER_IDX), game_id, name);
    tag->uid = {0x00, 0x00, 0x54, 0x4D, 0x50, 0x00, m_slot_index, static_cast<u8>(i)};
    m_tags.push_back(std::move(tag));
  }
}

bool ICCardReader::IsCardPresent() const
{
  return !m_tags.empty();
}

bool ICCardReader::IsEjecting() const
{
  return m_eject_timer > EJECT_REPORT_THRESHOLD;
}

bool ICCardReader::IsReadyToInsertCard() const
{
  return !IsCardPresent() && m_eject_timer == 0;
}

void ICCardReader::InsertCard()
{
  if (IsCardPresent())
    return;
  NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "InsertCard");
  MakeTags(1);
  m_tags.front()->Initialize();
}

void ICCardReader::EjectCard()
{
  if (!IsCardPresent())
    return;
  NOTICE_LOG_FMT(SERIALINTERFACE_CARD, "EjectCard");
  m_tags.clear();
  m_eject_timer = EJECT_TIME;
  m_insert_checks = 0;
}

void ICCardReader::Update()
{
  if (m_eject_timer != 0)
    --m_eject_timer;

  const std::span<const u8> rx = GetRxByteSpan();
  if (rx.size() < 4)
    return;
  const std::size_t payload_size = BE16At(rx, 2);
  const std::size_t frame_size = payload_size + FRAME_OVERHEAD;
  if (rx.size() < frame_size)
    return;

  const std::span<const u8> frame = rx.first(frame_size);
  if (Xor(frame) != 0)
  {
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Bad checksum!");
    ConsumeRxBytes(1);
    return;
  }

  HandleRequest(frame[1], frame.subspan(4, payload_size));
  ConsumeRxBytes(frame_size);
}

void ICCardReader::HandleRequest(u8 command, std::span<const u8> payload)
{
  if (command != InsertCheck)
    m_insert_checks = 0;

  u16 status = STATUS_OK;
  std::vector<u8> reply;

  const std::size_t size = payload.size();
  const auto need = [&](std::size_t wanted) {
    if (size < wanted)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Undersized payload size {} for command: {:02x}", size,
                    command);
      status = STATUS_FAILED;
      return false;
    }
    if (size > wanted)
    {
      WARN_LOG_FMT(SERIALINTERFACE_CARD, "Oversized payload size {} for command: {:02x}", size,
                   command);
    }
    return true;
  };

  const auto session_tag = [&](u16 session) -> Tag* {
    const std::size_t index = session & 0xFF;
    if ((session & 0xFF00) == 0x2300 && index < m_tags.size())
      return m_tags[index].get();
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Unexpected card session: {:04x}", session);
    status = STATUS_FAILED;
    return nullptr;
  };

  const auto read_into_reply = [&](Tag& tag, u16 page, u16 count) {
    const std::optional<std::span<const u8>> bytes = tag.Read(page, count);
    if (bytes)
      reply.assign(bytes->begin(), bytes->end());
    else
      status = STATUS_FAILED;
  };

  const auto first_awake = [&]() {
    return std::ranges::find_if(m_tags, [](const auto& tag) { return !tag->halted; });
  };

  switch (command)
  {
  case GetStatus:
    if (!need(0))
      break;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "GetStatus");
    status = m_field_on ? 0x30 : 0x20;
    break;

  case SetBaudrate:
    if (!need(8))
      break;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "SetBaudrate: {:02x}", fmt::join(payload, " "));
    break;

  case FieldOn:
    if (!need(0))
      break;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "FieldOn");
    m_field_on = true;
    break;

  case FieldOff:
    if (!need(0))
      break;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "FieldOff");
    m_field_on = false;
    for (const auto& tag : m_tags)
      tag->halted = false;
    break;

  case Eject:
    if (!need(0))
      break;
    EjectCard();
    break;

  case InsertCheck:
  {
    if (!need(8))
      break;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "InsertCheck: {}", BE16At(payload, 0));
    const bool present = first_awake() != m_tags.end();
    if (present && m_insert_checks < 0xFF)
      ++m_insert_checks;
    status = present ? STATUS_OK : STATUS_FAILED;
    break;
  }

  case AntiCollision:
  {
    if (!need(16))
      break;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "AntiCollision: unk0:{:04x} unk1:{:02x}",
                 BE16At(payload, 0), fmt::join(payload.subspan(2, 8), " "));
    const auto awake = first_awake();
    if (awake == m_tags.end())
    {
      status = STATUS_FAILED;
      break;
    }
    const auto awake_count =
        std::ranges::count_if(m_tags, [](const auto& tag) { return !tag->halted; });
    status = awake_count > 1 ? 1 : 0;
    reply.assign((*awake)->uid.begin(), (*awake)->uid.end());
    break;
  }

  case SelectCard:
  {
    if (!need(8))
      break;
    const std::span<const u8> uid = payload.first(8);
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "SelectCard: {:02x}", fmt::join(uid, " "));
    const auto match = std::ranges::find_if(
        m_tags, [&](const auto& tag) { return std::ranges::equal(tag->uid, uid); });
    if (match == m_tags.end())
    {
      WARN_LOG_FMT(SERIALINTERFACE_CARD, "SelectCard: Unexpected Card ID.");
      status = STATUS_FAILED;
      break;
    }
    const u16 session = static_cast<u16>(0x2300 | (match - m_tags.begin()));
    PutBE16(reply, session);
    reply.resize(8, 0);
    break;
  }

  case ReadPage:
  case ReadUseCount:
  {
    if (!need(8))
      break;
    const u16 session = BE16At(payload, 0);
    const u16 page = BE16At(payload, 2) & PAGE_MASK;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ReadPage: session:{:04x} page:{}", session, page);
    if (Tag* tag = session_tag(session))
      read_into_reply(*tag, page, 1);
    break;
  }

  case WritePage:
  {
    if (!need(16))
      break;
    const u16 session = BE16At(payload, 0);
    const u16 unknown = BE16At(payload, 2);
    const u16 page = BE16At(payload, 4) & PAGE_MASK;
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "WritePage: session:{:04x} unk:{} page:{}", session,
                 unknown, page);
    if (Tag* tag = session_tag(session))
    {
      if (!tag->Write(page, payload.subspan(6, PAGE_SIZE)))
        status = STATUS_FAILED;
    }
    break;
  }

  case DecreaseUseCount:
  {
    if (!need(8))
      break;
    const u16 session = BE16At(payload, 0);
    const u16 page = BE16At(payload, 2) & PAGE_MASK;
    const u16 amount = BE16At(payload, 4);
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "DecreaseUseCount: session:{:04x}, page:{} amount:{}",
                 session, page, amount);
    if (Tag* tag = session_tag(session))
    {
      PutBE16(reply, tag->DecreaseUseCount(page, amount));
      reply.resize(8, 0);
    }
    break;
  }

  case HaltCard:
  {
    if (!need(8))
      break;
    const u16 session = BE16At(payload, 0);
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "HaltCard: session:{:04x}", session);
    if (Tag* tag = session_tag(session))
      tag->halted = true;
    break;
  }

  case ReadPages:
  {
    if (!need(8))
      break;
    const u16 session = BE16At(payload, 0);
    const u16 page = BE16At(payload, 2) & PAGE_MASK;
    const u16 count = BE16At(payload, 4);
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "ReadPages: session:{:04x} page:{} page_count:{}",
                 session, page, count);
    if (Tag* tag = session_tag(session))
      read_into_reply(*tag, page, count);
    break;
  }

  case WritePages:
  {
    if (size < 8)
    {
      ERROR_LOG_FMT(SERIALINTERFACE_CARD, "WritePages: Undersized payload size: {}", size);
      break;
    }
    const u16 session = BE16At(payload, 0);
    const u16 page = BE16At(payload, 2) & PAGE_MASK;
    const u16 count = BE16At(payload, 4);
    INFO_LOG_FMT(SERIALINTERFACE_CARD, "WritePages: session:{:04x} page:{} page_count:{}",
                 session, page, count);
    Tag* const tag = session_tag(session);
    if (tag == nullptr)
      break;
    if (!need(8 + std::size_t{count} * PAGE_SIZE))
      break;
    // The page data start right after the three numbers.
    if (!tag->Write(page, payload.subspan(6, std::size_t{count} * PAGE_SIZE)))
      status = STATUS_FAILED;
    break;
  }

  default:
    ERROR_LOG_FMT(SERIALINTERFACE_CARD, "Unknown command: {:02x}", command);
    break;
  }

  // Reply: mark, command, BE16 size of status + payload, status, payload, check byte.
  std::vector<u8> frame{REPLY_MARK, command};
  PutBE16(frame, static_cast<u16>(2 + reply.size()));
  PutBE16(frame, status);
  frame.insert(frame.end(), reply.begin(), reply.end());
  frame.push_back(Xor(frame));
  WriteTxBytes(frame);
}

void ICCardReader::DoState(PointerWrap& p)
{
  SerialDevice::DoState(p);

  u8 count = static_cast<u8>(m_tags.size());
  p.Do(count);
  if (count != m_tags.size())
    MakeTags(count);
  for (const auto& tag : m_tags)
  {
    p.DoArray(tag->data);
    p.DoArray(tag->uid);
    u8 state = tag->halted ? 1 : 0;
    p.Do(state);
    tag->halted = state == 1;
  }
  p.Do(m_field_on);
  p.Do(m_eject_timer);
  p.Do(m_insert_checks);
}

}  // namespace Triforce
