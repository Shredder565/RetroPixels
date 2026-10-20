// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/NetKDRequest.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/HMAC.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/Random.h"
#include "Common/ScopeGuard.h"
#include "Common/SettingsHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/CommonTitles.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Network/KD/NetKDTime.h"
#include "Core/IOS/Network/KD/VFF/VFFUtil.h"
#include "Core/IOS/Network/KD/WC24File.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"
#include "Core/WC24PatchEngine.h"

namespace IOS::HLE
{
namespace
{
enum : u32
{
  IOCTL_NWC24_SUSPEND_SCHEDULER = 0x01,
  IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER = 0x02,
  IOCTL_NWC24_EXEC_RESUME_SCHEDULER = 0x03,
  IOCTL_NWC24_KD_GET_TIME_TRIGGERS = 0x04,
  IOCTL_NWC24_SET_SCHEDULE_SPAN = 0x05,
  IOCTL_NWC24_STARTUP_SOCKET = 0x06,
  IOCTL_NWC24_CLEANUP_SOCKET = 0x07,
  IOCTL_NWC24_LOCK_SOCKET = 0x08,
  IOCTL_NWC24_UNLOCK_SOCKET = 0x09,
  IOCTL_NWC24_CHECK_MAIL_NOW = 0x0A,
  IOCTL_NWC24_SEND_MAIL_NOW = 0x0B,
  IOCTL_NWC24_RECEIVE_MAIL_NOW = 0x0C,
  IOCTL_NWC24_SAVE_MAIL_NOW = 0x0D,
  IOCTL_NWC24_DOWNLOAD_NOW_EX = 0x0E,
  IOCTL_NWC24_REQUEST_GENERATED_USER_ID = 0x0F,
  IOCTL_NWC24_REQUEST_REGISTER_USER_ID = 0x10,
  IOCTL_NWC24_GET_SCHEDULER_STAT = 0x1E,
  IOCTL_NWC24_SET_FILTER_MODE = 0x1F,
  IOCTL_NWC24_SET_DEBUG_MODE = 0x20,
  IOCTL_NWC24_KD_SET_NEXT_WAKEUP = 0x21,
  IOCTL_NWC24_SET_SCRIPT_MODE = 0x22,
  IOCTL_NWC24_REQUEST_SHUTDOWN = 0x28,
};

// Indices into the scheduler status buffer.
constexpr size_t STAT_PERMISSIONS = 1;
constexpr size_t STAT_LAST_ERROR = 2;
constexpr size_t STAT_CURRENT_FUNCTION = 4;
constexpr size_t STAT_ERROR_COUNT = 5;
constexpr size_t STAT_MAIL_CHECKS = 11;
constexpr size_t STAT_MAIL_SENDS = 14;
constexpr size_t STAT_ERROR_HISTORY = 32;
constexpr u32 ERROR_HISTORY_LENGTH = 32;
// GET_SCHEDULER_STAT copies at most this many bytes of the buffer.
constexpr u32 MAX_STAT_BYTES = 256;

constexpr std::string_view CGI_SUCCESS = "100";

// Values of the "hardware model" field mixed into the WiiConnect24 ID.
enum class HardwareModel : u8
{
  RVT = 0,
  RVV = 0,
  RVL = 1,
  RVD = 2,
  Unknown = 7
};

u8 AreaToCode(std::string_view area)
{
  struct AreaCode
  {
    std::string_view area;
    u8 code;
  };
  static constexpr AreaCode AREAS[] = {
      {"JPN", 0}, {"USA", 1}, {"EUR", 2}, {"AUS", 2}, {"BRA", 1}, {"TWN", 3}, {"ROC", 3},
      {"KOR", 4}, {"HKG", 5}, {"ASI", 5}, {"LTN", 1}, {"SAF", 2}, {"CHN", 6},
  };
  for (const AreaCode& entry : AREAS)
  {
    if (entry.area == area)
      return entry.code;
  }
  return 7;
}

HardwareModel ModelFromName(std::string_view model)
{
  if (model == "RVL")
    return HardwareModel::RVL;
  if (model == "RVT")
    return HardwareModel::RVT;
  if (model == "RVV")
    return HardwareModel::RVV;
  if (model == "RVD")
    return HardwareModel::RVD;
  return HardwareModel::Unknown;
}

constexpr u8 ByteOf(u64 value, u32 index)
{
  return static_cast<u8>(value >> (8 * index));
}

constexpr u64 WithByte(u64 value, u32 index, u8 byte)
{
  const u32 shift = 8 * index;
  return (value & ~(u64{0xff} << shift)) | (u64{byte} << shift);
}

// Builds the 16-digit WiiConnect24 ID from the console ID, the ID counter, the hardware model
// and the area, the way KD does: a 53-bit value with a 10-bit check field, scrambled by a
// nibble substitution, a byte permutation, a rotation and two XOR masks.
s32 MakeWiiNumber(u64* out, u32 console_id, u16 counter, HardwareModel model, u8 area)
{
  static constexpr u8 NIBBLE_MAP[16] = {0x4, 0xB, 0x7, 0x9, 0xF, 0x1, 0xD, 0x3,
                                        0xC, 0x2, 0x6, 0xE, 0x8, 0x0, 0xA, 0x5};
  static constexpr u8 BYTE_DESTINATION[6] = {1, 5, 0, 4, 2, 3};
  constexpr u64 MASK_53 = 0x001FFFFFFFFFFFFFULL;

  const u64 fields = (u64{area} << 50) | (u64{static_cast<u8>(model)} << 47) |
                     (u64{console_id} << 15) | (u64{counter} << 10);

  // Polynomial remainder (generator 0x635) of the fields, worked from bit 52 down to bit 10.
  u64 remainder = fields;
  for (u32 bit = 52; bit >= 10; --bit)
  {
    if ((remainder >> bit) & 1)
      remainder ^= u64{0x635} << (bit - 10);
  }

  u64 id = (fields | (remainder & 0xFFFFFFFFULL)) ^ 0x0000B3B3B3B3B3B3ULL;
  // Move the low 10 bits up to bits 43..52.
  id = (id >> 10) | ((id & 0x3FF) << 43);

  for (u32 index = 0; index < 6; ++index)
  {
    const u8 byte = ByteOf(id, index);
    const u8 mapped = static_cast<u8>((NIBBLE_MAP[byte >> 4] << 4) | NIBBLE_MAP[byte & 0xF]);
    id = WithByte(id, index, mapped);
  }

  const u64 substituted = id;
  for (u32 index = 0; index < 6; ++index)
    id = WithByte(id, BYTE_DESTINATION[index], ByteOf(substituted, index));

  id &= MASK_53;
  id = (id << 1) | ((id >> 52) & 1);
  id = (id ^ 0x00005E5E5E5E5E5EULL) & MASK_53;

  *out = id;
  // Must fit in 16 decimal digits.
  return id > 9999999999999999ULL ? NWC24::WC24_ERR_FATAL : NWC24::WC24_OK;
}

std::string SettingsFilePath()
{
  return Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING;
}

std::span<const u8> AsBytes(const std::string& text)
{
  return {reinterpret_cast<const u8*>(text.data()), text.size()};
}
}  // namespace

NetKDRequestDevice::NetKDRequestDevice(EmulationKernel& ios, const std::string& device_name,
                                       const std::shared_ptr<NetKDTimeDevice>& time_device)
    : EmulationDevice(ios, device_name), m_config{ios.GetFS()}, m_dl_list{ios.GetFS()},
      m_send_list{ios.GetFS()}, m_friend_list{ios.GetFS()}, m_time_device{time_device}
{
  // All WiiConnect24 permissions granted.
  m_scheduler_buffer[STAT_PERMISSIONS] = 0xffffffff;

  m_work_queue.Reset("WiiConnect24 Worker", [this](const AsyncTask& task) {
    const s32 result = task.handler().return_value;
    std::lock_guard lock(m_async_reply_lock);
    m_async_replies.push(AsyncReply{task.request, result});
  });

  // Mail needs a real console ID (a shared default one could never register) and a usable
  // send list.
  m_handle_mail = !ios.GetIOSC().IsUsingDefaultId() && !m_send_list.IsDisabled();
  m_scheduler_work_queue.Reset("WiiConnect24 Scheduler Worker");
  m_scheduler_timer_thread = std::thread([this] { SchedulerTimer(); });
}

NetKDRequestDevice::~NetKDRequestDevice()
{
  if (const auto sockets = GetEmulationKernel().GetSocketManager())
    sockets->Clean();

  {
    std::lock_guard lock(m_scheduler_lock);
    if (!m_scheduler_timer_thread.joinable())
      return;
    m_shutdown_event.Set();
  }
  m_scheduler_timer_thread.join();
  m_scheduler_work_queue.Shutdown();
  m_work_queue.Shutdown();
}

std::optional<IPCReply>
NetKDRequestDevice::RunLater(const IOCtlRequest& request,
                             IPCReply (NetKDRequestDevice::*handler)(const IOCtlRequest&))
{
  m_work_queue.EmplaceItem(
      AsyncTask{request, [this, handler, request] { return (this->*handler)(request); }});
  return std::nullopt;
}

void NetKDRequestDevice::Update()
{
  std::lock_guard lock(m_async_reply_lock);
  for (; !m_async_replies.empty(); m_async_replies.pop())
  {
    const AsyncReply& reply = m_async_replies.front();
    GetEmulationKernel().EnqueueIPCReply(reply.request, reply.return_value);
  }
}

void NetKDRequestDevice::SchedulerTimer()
{
  Common::SetCurrentThreadName("KD Scheduler Timer");
  // Minutes since each job last ran.
  u32 minutes_since_mail = 0;
  u32 minutes_since_download = 0;
  for (;;)
  {
    {
      std::lock_guard lock(m_scheduler_lock);
      if (m_handle_mail && minutes_since_mail >= m_mail_span)
      {
        INFO_LOG_FMT(IOS_WC24, "KD scheduler: starting the mail job");
        m_scheduler_work_queue.Push([this] { SchedulerWorker(SchedulerEvent::Mail); });
        minutes_since_mail = 0;
      }
      if (!m_dl_list.IsDisabled() && minutes_since_download >= m_download_span)
      {
        INFO_LOG_FMT(IOS_WC24, "KD scheduler: starting the download job");
        m_scheduler_work_queue.Push([this] { SchedulerWorker(SchedulerEvent::Download); });
        minutes_since_download = 0;
      }
    }

    if (m_shutdown_event.WaitFor(std::chrono::minutes{1}))
      return;
    ++minutes_since_mail;
    ++minutes_since_download;
  }
}

void NetKDRequestDevice::SchedulerWorker(const SchedulerEvent event)
{
  if (event == SchedulerEvent::Mail)
  {
    if (!m_config.IsRegistered())
      return;

    u32 mail_flag = 0;
    u32 interval = 0;
    if (const NWC24::ErrorCode code = KDCheckMail(&mail_flag, &interval); code != NWC24::WC24_OK)
      LogError(ErrorType::CheckMail, code);
    if (const NWC24::ErrorCode code = KDSendMail(); code != NWC24::WC24_OK)
      LogError(ErrorType::SendMail, code);
    return;
  }

  u16 entry_index = 0;
  std::optional<u8> subtask_id;
  NWC24::ErrorCode code = DetermineDownloadTask(&entry_index, &subtask_id);
  if (code == NWC24::WC24_OK)
    code = KDDownload(entry_index, subtask_id);
  else if (code == NWC24::WC24_ERR_NOT_FOUND)
  {
    // Nothing is due: look again less often.
    std::lock_guard lock(m_scheduler_lock);
    m_download_span = DEFAULT_SCHEDULER_SPAN_MINUTES;
  }

  if (code != NWC24::WC24_OK)
    LogError(ErrorType::KD_Download, code);
}

std::string NetKDRequestDevice::GetValueFromCGIResponse(const std::string& response,
                                                        const std::string& key)
{
  // The server answers with "key=value" lines. Lines that do not split into exactly two parts
  // at '=' are ignored.
  for (const std::string& line : SplitString(response, '\n'))
  {
    const std::vector<std::string> parts = SplitString(line, '=');
    if (parts.size() == 2 && parts[0] == key)
      return std::string(StripWhitespace(parts[1]));
  }
  return {};
}

void NetKDRequestDevice::LogError(ErrorType error_type, s32 error_code)
{
  // Each kind of failure has its own range of six-digit codes, shown negated.
  s32 recorded = 0;
  switch (error_type)
  {
  case ErrorType::Account:
    recorded = error_code - 101200;
    break;
  case ErrorType::Client:
    recorded = error_code - 107300;
    break;
  case ErrorType::KD_Download:
    recorded = error_code - 107200;
    break;
  case ErrorType::Server:
    recorded = -117000 - error_code;
    break;
  case ErrorType::CheckMail:
    recorded = error_code - 102200;
    break;
  case ErrorType::SendMail:
    recorded = error_code - 105000;
    break;
  case ErrorType::ReceiveMail:
    recorded = error_code - 100300;
    break;
  case ErrorType::CGI:
    recorded = -110000 - error_code;
    break;
  }

  const u32 recorded_be = Common::swap32(static_cast<u32>(recorded));
  std::lock_guard lock(m_scheduler_buffer_lock);
  m_scheduler_buffer[STAT_ERROR_HISTORY + m_error_count % ERROR_HISTORY_LENGTH] = recorded_be;
  ++m_error_count;
  m_scheduler_buffer[STAT_ERROR_COUNT] = Common::swap32(m_error_count);
  m_scheduler_buffer[STAT_LAST_ERROR] = recorded_be;
}

void NetKDRequestDevice::SetCurrentFunction(CurrentFunction function)
{
  std::lock_guard lock(m_scheduler_buffer_lock);
  m_scheduler_buffer[STAT_CURRENT_FUNCTION] = Common::swap32(static_cast<u32>(function));
}

NWC24::ErrorCode NetKDRequestDevice::KDCheckMail(u32* mail_flag, u32* interval)
{
  bool succeeded = false;
  Common::ScopeGuard finish([&] {
    std::lock_guard lock(m_scheduler_buffer_lock);
    if (succeeded)
    {
      m_scheduler_buffer[STAT_MAIL_CHECKS] =
          Common::swap32(Common::swap32(m_scheduler_buffer[STAT_MAIL_CHECKS]) + 1);
    }
    m_scheduler_buffer[STAT_CURRENT_FUNCTION] = 0;
  });
  SetCurrentFunction(CurrentFunction::Check);

  // The server proves itself by answering a random challenge with an HMAC.
  u64 challenge = 0;
  Common::Random::Generate(&challenge, sizeof(challenge));
  const std::string check_url = m_config.GetCheckURL();
  const Common::HttpRequest::Response response =
      m_http.Post(check_url, fmt::format("mlchkid={}&chlng={}", m_config.GetMlchkid(), challenge));
  if (!response)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD mail check: no answer from {}", check_url);
    return NWC24::WC24_ERR_SERVER;
  }

  const std::string body(response->begin(), response->end());
  const std::string code = GetValueFromCGIResponse(body, "cd");
  if (code != CGI_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD mail check: server answered with code {}", code);
    return NWC24::WC24_ERR_SERVER;
  }

  const std::string server_hmac = GetValueFromCGIResponse(body, "res");
  const std::string server_mail_flag = GetValueFromCGIResponse(body, "mail.flag");
  const std::string server_interval = GetValueFromCGIResponse(body, "interval");

  // The answer must be HMAC-SHA1(challenge, ID, flag, interval) as lowercase hex (compared
  // case-sensitively, like KD does). The HMAC key is Nintendo's and is not included; the owner
  // can give it as wc24_mail_check_key in wii_keys.txt. Without it the answer is not checked.
  const std::optional<std::vector<u8>> mail_check_key =
      IOS::HLE::IOSC::LoadOwnerKey("wc24_mail_check_key", 20);
  if (mail_check_key)
  {
    const std::string signed_text = fmt::format("{}\nw{:016}\n{}\n{}", challenge,
                                                m_config.Id(), server_mail_flag, server_interval);
    std::array<u8, 20> expected{};
    Common::HMAC::HMACWithSHA1(*mail_check_key, AsBytes(signed_text), expected.data());
    if (Common::BytesToHexString(expected) != server_hmac)
    {
      ERROR_LOG_FMT(IOS_WC24, "KD mail check: the server's HMAC does not match");
      return NWC24::WC24_ERR_SERVER;
    }
  }
  else
  {
    WARN_LOG_FMT(IOS_WC24, "KD mail check: no mail check key, so the server's HMAC is not checked");
  }

  // New mail is waiting when the server's flag differs from the one in the send list.
  *mail_flag = std::strncmp(server_mail_flag.data(), m_send_list.GetMailFlag().data(), 22) != 0;

  {
    std::lock_guard lock(m_scheduler_lock);
    // The server may also change how often the scheduler runs; 0 counts as 1.
    if (TryParse(m_http.GetHeaderValue("X-Wii-Mail-Check-Span"), interval))
    {
      *interval = std::max(*interval, 1u);
      m_mail_span = *interval;
    }
    if (TryParse(m_http.GetHeaderValue("X-Wii-Download-Span"), &m_download_span))
      m_download_span = std::max(m_download_span, 1u);
  }

  succeeded = true;
  return NWC24::WC24_OK;
}

NWC24::ErrorCode NetKDRequestDevice::DetermineDownloadTask(u16* entry_index,
                                                           std::optional<u8>* subtask_id) const
{
  // The scheduler picks the due entry (not skipped by the scheduler) that has waited longest.
  // An entry with subtasks only counts if one of its subtasks is due; checking one stores that
  // subtask in `subtask_id`, even if a later entry is picked in the end.
  constexpr u64 NONE_FOUND = std::numeric_limits<u64>::max();
  const u64 now = m_time_device->GetAdjustedUTC();
  u64 oldest_due = NONE_FOUND;

  for (u16 index = 0; index < NWC24::NWC24Dl::MAX_ENTRIES; ++index)
  {
    if (!m_dl_list.DoesEntryExist(index) || m_dl_list.SkipSchedulerDownload(index))
      continue;

    const u64 due = m_dl_list.GetNextDownloadTime(index);
    if (now < due || due >= oldest_due)
      continue;

    if (m_dl_list.HasSubtask(index) && DetermineSubtask(index, subtask_id) != NWC24::WC24_OK)
      continue;

    oldest_due = due;
    *entry_index = index;
  }

  return oldest_due == NONE_FOUND ? NWC24::WC24_ERR_NOT_FOUND : NWC24::WC24_OK;
}

NWC24::ErrorCode NetKDRequestDevice::DetermineSubtask(u16 entry_index,
                                                      std::optional<u8>* subtask_id) const
{
  if (m_dl_list.IsSubtaskDownloadDisabled(entry_index))
    return NWC24::WC24_ERR_DISABLED;

  // The first valid subtask whose next download time has passed.
  const u64 now = m_time_device->GetAdjustedUTC();
  for (u8 subtask = 0; subtask < 32; ++subtask)
  {
    if (m_dl_list.IsValidSubtask(entry_index, subtask) &&
        m_dl_list.GetLastSubtaskDownloadTime(entry_index, subtask) < now)
    {
      *subtask_id = subtask;
      return NWC24::WC24_OK;
    }
  }
  return NWC24::WC24_ERR_INVALID_VALUE;
}

NWC24::ErrorCode NetKDRequestDevice::KDSendMail()
{
  bool succeeded = false;
  Common::ScopeGuard finish([&] {
    std::lock_guard lock(m_scheduler_buffer_lock);
    if (succeeded)
    {
      m_scheduler_buffer[STAT_MAIL_SENDS] =
          Common::swap32(Common::swap32(m_scheduler_buffer[STAT_MAIL_SENDS]) + 1);
    }
    m_scheduler_buffer[STAT_CURRENT_FUNCTION] = 0;
    // The list is saved whatever happened.
    m_send_list.WriteSendList();
  });
  SetCurrentFunction(CurrentFunction::Send);

  m_send_list.ReadSendList();
  std::vector<Common::HttpRequest::Multiform> form{
      {"mlid", fmt::format("mlid=w{:016}\r\npasswd={}", m_config.Id(), m_config.GetPassword())}};

  // Messages that are too big or unreadable are dropped from the list (and the pass). The pass
  // keeps walking the original number of slots of the shrinking list, so the message after a
  // dropped one is skipped and the last slots are visited again, as they always have been.
  std::vector<u32> slots = m_send_list.GetMailToSend();
  const size_t pass_length = slots.size();
  size_t kept = slots.size();
  const auto drop = [&](u32 index, NWC24::ErrorCode reason) {
    LogError(ErrorType::SendMail, reason);
    if (const NWC24::ErrorCode deleted = m_send_list.DeleteMessage(index);
        deleted != NWC24::WC24_OK)
    {
      LogError(ErrorType::SendMail, deleted);
    }
    const auto end = slots.begin() + static_cast<std::ptrdiff_t>(kept);
    const auto found = std::find(slots.begin(), end, index);
    if (found != end)
    {
      std::copy(found + 1, end, found);
      --kept;
    }
  };

  for (size_t position = 0; position < pass_length; ++position)
  {
    const u32 index = slots[position];
    const u32 entry_id = m_send_list.GetEntryId(index);
    const u32 size = m_send_list.GetMailSize(index);
    if (size > MAX_MAIL_SIZE)
    {
      WARN_LOG_FMT(IOS_WC24, "KD send mail: message {} is too large to send", entry_id);
      drop(index, NWC24::WC24_MSG_TOO_BIG);
      continue;
    }

    std::vector<u8> message(size);
    const NWC24::ErrorCode read = NWC24::ReadFromVFF(
        NWC24::Mail::SEND_BOX_PATH, m_send_list.GetMailPath(index), m_ios.GetFS(), message);
    if (read != NWC24::WC24_OK)
    {
      ERROR_LOG_FMT(IOS_WC24, "KD send mail: reading message {} failed ({})", entry_id,
                    static_cast<s32>(read));
      drop(index, NWC24::WC24_MSG_DAMAGED);
      continue;
    }
    form.push_back({fmt::format("m{}", entry_id), std::string(message.begin(), message.end())});
  }
  slots.resize(kept);

  const std::string send_url = m_config.GetSendURL();
  const Common::HttpRequest::Response response = m_http.PostMultiform(send_url, form);
  if (!response)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD send mail: no answer from {}", send_url);
    return NWC24::WC24_ERR_SERVER;
  }

  const std::string body(response->begin(), response->end());
  const std::string code = GetValueFromCGIResponse(body, "cd");
  if (code != CGI_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD send mail: server answered with code {}", code);
    return NWC24::WC24_ERR_SERVER;
  }

  FinishSentMail(body, slots);
  succeeded = true;
  return NWC24::WC24_OK;
}

void NetKDRequestDevice::FinishSentMail(const std::string& response, const std::vector<u32>& sent)
{
  // Every message that went out is removed, last first, so that new messages fill the list
  // from the front. Per-message codes are only logged; an unreadable one ends the loop (after
  // its message is removed too).
  for (auto it = sent.rbegin(); it != sent.rend(); ++it)
  {
    const u32 index = *it;
    const u32 entry_id = m_send_list.GetEntryId(index);
    const std::string value = GetValueFromCGIResponse(response, fmt::format("cd{}", entry_id));

    s32 cgi_code = 0;
    const bool parsed = TryParse(value, &cgi_code);
    if (!parsed)
    {
      ERROR_LOG_FMT(IOS_WC24, "KD send mail: unreadable result for message {}", entry_id);
      LogError(ErrorType::CGI, NWC24::WC24_ERR_SERVER);
    }
    else if (cgi_code != 100)
    {
      ERROR_LOG_FMT(IOS_WC24, "KD send mail: the server did not take message {}", entry_id);
      LogError(ErrorType::CGI, cgi_code);
    }

    if (const NWC24::ErrorCode deleted = m_send_list.DeleteMessage(index);
        deleted != NWC24::WC24_OK)
    {
      LogError(ErrorType::SendMail, deleted);
    }
    if (!parsed)
      break;
  }
}

NWC24::ErrorCode NetKDRequestDevice::KDDownload(const u16 entry_index,
                                                const std::optional<u8> subtask_id)
{
  const NWC24::ErrorCode result = FetchDownload(entry_index, subtask_id);

  // Whatever happened, schedule the next attempt (after the entry's margin on success, after
  // its retry time on failure) and save the list.
  const u64 now = m_time_device->GetAdjustedUTC();
  const u64 wait = result == NWC24::WC24_OK ? m_dl_list.GetDownloadMargin(entry_index) :
                                              m_dl_list.GetRetryTime(entry_index);
  m_dl_list.SetNextDownloadTime(entry_index, now + wait, subtask_id);
  m_dl_list.WriteDlList();
  return result;
}

NWC24::ErrorCode NetKDRequestDevice::FetchDownload(u16 entry_index, std::optional<u8> subtask_id)
{
  const std::string content_name = m_dl_list.GetVFFContentName(entry_index, subtask_id);
  std::string url = m_dl_list.GetDownloadURL(entry_index, subtask_id);

  // Content without a name belongs in the mailbox, which is not supported; it counts as done.
  if (content_name.empty())
    return NWC24::WC24_OK;

  const std::vector<std::string> parts = SplitString(url, '/');
  if (parts.size() < 3)
  {
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_SERVER);
    return NWC24::WC24_ERR_SERVER;
  }

  // parts[2] is the host; a network patch may send it elsewhere.
  const std::string& host = parts[2];
  if (const std::optional<std::string> replacement =
          WC24PatchEngine::GetNetworkPatch(host, WC24PatchEngine::IsKD{true}))
  {
    url.replace(url.find(host), host.size(), *replacement);
  }
  INFO_LOG_FMT(IOS_WC24, "KD download: {} -> {}", url, content_name);

  const Common::HttpRequest::Response response = m_http.Get(url);
  if (!response)
  {
    // An HTTP status is a server error; no status at all (no connection) is a client error.
    const s32 status = m_http.GetLastResponseCode();
    ERROR_LOG_FMT(IOS_WC24, "Failed to request data at {}. HTTP Status Code: {}", url, status);
    if (status != 0)
      LogError(ErrorType::Server, status);
    else
      LogError(ErrorType::Client, NWC24::WC24_ERR_NULL);
    return NWC24::WC24_ERR_SERVER;
  }

  std::vector<u8> content;
  if (!m_dl_list.IsRSASigned(entry_index))
  {
    // Unsigned content has no WC24 header.
    if (response->empty())
    {
      ERROR_LOG_FMT(IOS_WC24, "File at {} is empty.", url);
      LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
      return NWC24::WC24_ERR_BROKEN;
    }
    content = *response;
  }
  else
  {
    constexpr size_t HEADER_SIZE = sizeof(NWC24::WC24File);
    if (response->size() < HEADER_SIZE)
    {
      ERROR_LOG_FMT(IOS_WC24, "File at {} is too small to be a valid file.", url);
      LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
      return NWC24::WC24_ERR_BROKEN;
    }

    NWC24::WC24File header;
    std::memcpy(&header, response->data(), HEADER_SIZE);
    std::vector<u8> payload(response->begin() + HEADER_SIZE, response->end());

    if (!m_dl_list.IsEncrypted(entry_index))
    {
      content = std::move(payload);
    }
    else
    {
      // Encrypted with the title's own key (from its wc24pubk.mod) in OFB mode.
      const std::optional<NWC24::WC24PubkMod> title_keys = m_dl_list.GetWC24PubkMod(entry_index);
      if (!title_keys)
      {
        ERROR_LOG_FMT(IOS_WC24, "Failed to get wc24pubk.mod for the current task.");
        LogError(ErrorType::KD_Download, NWC24::WC24_ERR_FILE_OPEN);
        return NWC24::WC24_ERR_FILE_OPEN;
      }
      content.resize(payload.size());
      Common::AES::CryptOFB(title_keys->aes_key, header.iv, header.iv, payload.data(),
                            content.data(), payload.size());
    }
  }

  const NWC24::ErrorCode written =
      NWC24::WriteToVFF(m_dl_list.GetVFFPath(entry_index), content_name, m_ios.GetFS(), content);
  if (written != NWC24::WC24_OK)
    LogError(ErrorType::KD_Download, written);
  return written;
}

IPCReply NetKDRequestDevice::HandleNWC24CheckMailNow(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  if (!m_handle_mail)
  {
    LogError(ErrorType::CheckMail, NWC24::WC24_ERR_BROKEN);
    WriteReturnValue(memory, NWC24::WC24_ERR_BROKEN, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  u32 mail_flag = 0;
  u32 interval = 0;
  const NWC24::ErrorCode result = KDCheckMail(&mail_flag, &interval);
  WriteReturnValue(memory, result, request.buffer_out);
  memory.Write_U32(mail_flag, request.buffer_out + 4);
  memory.Write_U32(interval, request.buffer_out + 8);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleNWC24SendMailNow(const IOCtlRequest& request)
{
  const NWC24::ErrorCode result = KDSendMail();
  WriteReturnValue(GetSystem().GetMemory(), result, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleNWC24DownloadNowEx(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const auto fail = [&](NWC24::ErrorCode code) {
    LogError(ErrorType::KD_Download, code);
    WriteReturnValue(memory, code, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  };

  // The list is reloaded, as the title may just have changed it.
  if (m_dl_list.IsDisabled() || !m_dl_list.ReadDlList())
    return fail(NWC24::WC24_ERR_BROKEN);

  // in: flags (u32), entry index (u16 at offset 6), subtask mask (u32).
  const u32 flags = memory.Read_U32(request.buffer_in);
  const u16 entry_index = memory.Read_U16(request.buffer_in + 6);
  const u32 subtasks = memory.Read_U32(request.buffer_in + 8);
  INFO_LOG_FMT(IOS_WC24, "KD download now: flags {}, entry {}, subtasks {}", flags, entry_index,
               subtasks);

  if (entry_index >= NWC24::NWC24Dl::MAX_ENTRIES)
    return fail(NWC24::WC24_ERR_INVALID_VALUE);
  if (!m_dl_list.DoesEntryExist(entry_index))
    return fail(NWC24::WC24_ERR_NOT_FOUND);

  NWC24::ErrorCode result;
  if ((flags & (1u << 2)) == 0)
  {
    result = KDDownload(entry_index, std::nullopt);
  }
  else
  {
    // Every subtask in the mask, stopping at the first failure. An empty mask leaves the
    // result at "broken", which makes the title repair the entry.
    result = NWC24::WC24_ERR_BROKEN;
    for (u8 subtask = 0; subtask < 32; ++subtask)
    {
      if (((subtasks >> subtask) & 1) == 0)
        continue;
      result = KDDownload(entry_index, subtask);
      if (result != NWC24::WC24_OK)
        break;
    }
  }

  WriteReturnValue(memory, result, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleRequestRegisterUserId(const IOS::HLE::IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const auto finish = [&](NWC24::ErrorCode code) {
    WriteReturnValue(memory, code, request.buffer_out);
    return IPCReply{IPC_SUCCESS};
  };
  const auto fail = [&](ErrorType type, s32 logged, NWC24::ErrorCode code) {
    WriteReturnValue(memory, code, request.buffer_out);
    LogError(type, logged);
    return IPCReply{IPC_SUCCESS};
  };
  const auto save_registered = [&] {
    m_config.SetCreationStage(NWC24::NWC24CreationStage::Registered);
    m_config.SetChecksum(m_config.CalculateNwc24ConfigChecksum());
    m_config.WriteConfig();
    m_config.WriteCBK();
  };

  // The second word of the reply is always 0.
  memory.Write_U32(0, request.buffer_out + 4);

  if (m_ios.GetIOSC().IsUsingDefaultId())
  {
    // Everybody with the default console ID has the same Wii Number, which the server would
    // refuse as already registered, so registration is just assumed.
    save_registered();
    return finish(NWC24::WC24_OK);
  }

  if (m_config.IsRegistered())
    return fail(ErrorType::Account, NWC24::WC24_ERR_ID_REGISTERED, NWC24::WC24_ERR_ID_REGISTERED);
  if (!m_config.IsGenerated())
  {
    return fail(ErrorType::Account, NWC24::WC24_ERR_ID_NOT_GENERATED,
                NWC24::WC24_ERR_ID_NOT_GENERATED);
  }

  // The request includes the console's serial number from setting.txt.
  const auto file = m_ios.GetFS()->OpenFile(PID_KD, PID_KD, SettingsFilePath(), FS::Mode::Read);
  if (!file)
    return fail(ErrorType::Account, NWC24::WC24_ERR_FILE_OPEN, NWC24::WC24_ERR_FILE_OPEN);
  Common::SettingsBuffer settings;
  if (!file->Read(settings.data(), settings.size()))
    return fail(ErrorType::Account, NWC24::WC24_ERR_FILE_READ, NWC24::WC24_ERR_FILE_READ);
  const std::string serial = Common::SettingsReader{settings}.GetValue("SERNO");

  const std::string account_url = m_config.GetAccountURL();
  const Common::HttpRequest::Response response = m_http.Post(
      account_url, fmt::format("mlid=w{:016}&hdid={}&rgncd={}", m_config.Id(),
                               m_ios.GetIOSC().GetDeviceId(), serial));
  if (!response)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD registration: no answer from {}", account_url);
    return fail(ErrorType::Account, NWC24::WC24_ERR_SERVER, NWC24::WC24_ERR_SERVER);
  }

  const std::string body(response->begin(), response->end());
  s32 cgi_code = 0;
  if (!TryParse(GetValueFromCGIResponse(body, "cd"), &cgi_code))
  {
    // Logged, then handled as code 0.
    ERROR_LOG_FMT(IOS_WC24, "KD registration: unreadable result code");
    LogError(ErrorType::Account, NWC24::WC24_ERR_SERVER);
  }
  if (cgi_code != 100)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD registration: server answered with code {}", cgi_code);
    return fail(ErrorType::CGI, cgi_code, NWC24::WC24_ERR_SERVER);
  }

  const std::string password = GetValueFromCGIResponse(body, "passwd");
  const std::string mail_check_id = GetValueFromCGIResponse(body, "mlchkid");
  if (mail_check_id.size() != 32)
  {
    ERROR_LOG_FMT(IOS_WC24, "KD registration: invalid mail check ID {}", mail_check_id);
    return fail(ErrorType::Account, NWC24::WC24_ERR_SERVER, NWC24::WC24_ERR_SERVER);
  }

  m_config.SetPassword(password);
  m_config.SetMailCheckID(mail_check_id);
  save_registered();
  return finish(NWC24::WC24_OK);
}

void NetKDRequestDevice::HandleRequestGeneratedUserId(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();

  if (m_config.IsCreated())
  {
    // A new ID needs the area and model from setting.txt.
    std::string area;
    std::string model;
    if (const auto file =
            m_ios.GetFS()->OpenFile(PID_KD, PID_KD, SettingsFilePath(), FS::Mode::Read))
    {
      Common::SettingsBuffer settings;
      if (file->Read(settings.data(), settings.size()))
      {
        const Common::SettingsReader reader{settings};
        area = reader.GetValue("AREA");
        model = reader.GetValue("MODEL");
      }
    }

    if (area.empty() || model.empty())
    {
      LogError(ErrorType::Account, NWC24::WC24_ERR_INVALID_VALUE);
      WriteReturnValue(memory, NWC24::WC24_ERR_FATAL, request.buffer_out);
    }
    else
    {
      u64 id = 0;
      // Only the low 8 bits of the counter are used.
      const s32 made =
          MakeWiiNumber(&id, m_ios.GetIOSC().GetDeviceId(), static_cast<u8>(m_config.IdGen()),
                        ModelFromName(model), AreaToCode(area));
      m_config.SetId(id);
      m_config.IncrementIdGen();
      m_config.SetCreationStage(NWC24::NWC24CreationStage::Generated);
      m_config.SetChecksum(m_config.CalculateNwc24ConfigChecksum());
      m_config.WriteConfig();
      m_config.WriteCBK();
      WriteReturnValue(memory, made, request.buffer_out);
    }
  }
  else if (m_config.IsGenerated())
  {
    WriteReturnValue(memory, NWC24::WC24_ERR_ID_GENERATED, request.buffer_out);
  }
  else if (m_config.IsRegistered())
  {
    WriteReturnValue(memory, NWC24::WC24_ERR_ID_REGISTERED, request.buffer_out);
  }

  // out: result, ID (u64), creation stage.
  memory.Write_U64(m_config.Id(), request.buffer_out + 4);
  memory.Write_U32(static_cast<u32>(m_config.CreationStage()), request.buffer_out + 12);
}

std::optional<IPCReply> NetKDRequestDevice::IOCtl(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  // Output buffers must be present and word aligned.
  const auto bad_out = [&](u32 min_size) {
    return request.buffer_out == 0 || request.buffer_out % 4 != 0 ||
           request.buffer_out_size < min_size;
  };

  switch (request.request)
  {
  // Slow requests are answered from the worker thread.
  case IOCTL_NWC24_REQUEST_REGISTER_USER_ID:
    return RunLater(request, &NetKDRequestDevice::HandleRequestRegisterUserId);
  case IOCTL_NWC24_CHECK_MAIL_NOW:
    return RunLater(request, &NetKDRequestDevice::HandleNWC24CheckMailNow);
  case IOCTL_NWC24_DOWNLOAD_NOW_EX:
    return RunLater(request, &NetKDRequestDevice::HandleNWC24DownloadNowEx);
  case IOCTL_NWC24_SEND_MAIL_NOW:
    return RunLater(request, &NetKDRequestDevice::HandleNWC24SendMailNow);

  case IOCTL_NWC24_SUSPEND_SCHEDULER:
  case IOCTL_NWC24_EXEC_RESUME_SCHEDULER:
    // The scheduler keeps running; report success.
    WriteReturnValue(memory, 0, request.buffer_out);
    return IPCReply(IPC_SUCCESS);

  case IOCTL_NWC24_STARTUP_SOCKET:
    WriteReturnValue(memory, 0, request.buffer_out);
    memory.Write_U32(0, request.buffer_out + 4);
    return IPCReply(IPC_SUCCESS);

  case IOCTL_NWC24_CLEANUP_SOCKET:
    GetEmulationKernel().GetSocketManager()->Clean();
    return IPCReply(IPC_SUCCESS);

  case IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER:
  case IOCTL_NWC24_LOCK_SOCKET:
  case IOCTL_NWC24_UNLOCK_SOCKET:
  case IOCTL_NWC24_SAVE_MAIL_NOW:
    INFO_LOG_FMT(IOS_WC24, "KD: ioctl {:#x} accepted without effect", request.request);
    return IPCReply(IPC_SUCCESS);

  case IOCTL_NWC24_REQUEST_GENERATED_USER_ID:
    HandleRequestGeneratedUserId(request);
    return IPCReply(IPC_SUCCESS);

  case IOCTL_NWC24_GET_SCHEDULER_STAT:
  {
    if (bad_out(16))
      return IPCReply(IPC_EINVAL);
    std::lock_guard lock(m_scheduler_buffer_lock);
    memory.CopyToEmu(request.buffer_out, m_scheduler_buffer.data(),
                     std::min(request.buffer_out_size, MAX_STAT_BYTES));
    return IPCReply(IPC_SUCCESS);
  }

  case IOCTL_NWC24_REQUEST_SHUTDOWN:
  {
    const bool bad_in =
        request.buffer_in == 0 || request.buffer_in % 4 != 0 || request.buffer_in_size < 8;
    if (bad_in || bad_out(4))
      return IPCReply(IPC_EINVAL);
    // The shutdown kind (first input word) does not matter here: all sockets get closed.
    [[maybe_unused]] const u32 kind = memory.Read_U32(request.buffer_in);
    GetEmulationKernel().GetSocketManager()->Clean();
    return IPCReply(IPC_SUCCESS);
  }

  case IOCTL_NWC24_KD_GET_TIME_TRIGGERS:
  {
    if (bad_out(12))
      return IPCReply(IPC_EINVAL);
    std::lock_guard lock(m_scheduler_buffer_lock);
    memory.Write_U32(m_mail_span, request.buffer_out + 4);
    memory.Write_U32(m_download_span, request.buffer_out + 8);
    WriteReturnValue(memory, NWC24::WC24_OK, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  default:
    request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);
    return IPCReply(IPC_SUCCESS);
  }
}
}  // namespace IOS::HLE
