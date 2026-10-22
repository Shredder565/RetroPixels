// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/HttpRequest.h"
#include "Common/WorkQueueThread.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/Network/KD/Mail/WC24FriendList.h"
#include "Core/IOS/Network/KD/Mail/WC24Send.h"
#include "Core/IOS/Network/KD/NWC24Config.h"
#include "Core/IOS/Network/KD/NWC24DL.h"
#include "Core/IOS/Network/KD/NetKDTime.h"

namespace IOS::HLE
{
// /dev/net/kd/request: KD, the WiiConnect24 daemon. It registers the console with the mail
// server, checks for and sends mail, and downloads channel content into VFFs, both when asked
// and from a background scheduler.
class NetKDRequestDevice : public EmulationDevice
{
public:
  NetKDRequestDevice(EmulationKernel& ios, const std::string& device_name,
                     const std::shared_ptr<NetKDTimeDevice>& time_device);
  IPCReply HandleNWC24DownloadNowEx(const IOCtlRequest& request);
  NWC24::ErrorCode KDDownload(const u16 entry_index, const std::optional<u8> subtask_id);
  IPCReply HandleNWC24CheckMailNow(const IOCtlRequest& request);
  ~NetKDRequestDevice() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  void Update() override;

private:
  // Slow requests run on a worker thread; their results are handed back in Update().
  struct AsyncTask
  {
    IOS::HLE::Request request;
    std::function<IPCReply()> handler;
  };

  struct AsyncReply
  {
    IOS::HLE::Request request;
    s32 return_value;
  };

  std::optional<IPCReply> RunLater(const IOCtlRequest& request,
                                   IPCReply (NetKDRequestDevice::*handler)(const IOCtlRequest&));

  // Value of scheduler status word 4 while a job runs (stored big endian).
  enum class CurrentFunction : u32
  {
    None = 0,
    Account = 1,
    Check = 2,
    Receive = 3,
    Send = 5,
    Save = 6,
    Download = 7,
  };

  // Decides how an error is turned into the code KD records.
  enum class ErrorType
  {
    Account,
    KD_Download,
    Client,
    Server,
    CheckMail,
    SendMail,
    ReceiveMail,
    CGI,
  };

  enum class SchedulerEvent
  {
    Mail,
    Download,
  };

  IPCReply HandleNWC24SendMailNow(const IOCtlRequest& request);
  IPCReply HandleRequestRegisterUserId(const IOCtlRequest& request);
  void HandleRequestGeneratedUserId(const IOCtlRequest& request);
  NWC24::ErrorCode KDCheckMail(u32* mail_flag, u32* interval);
  NWC24::ErrorCode KDSendMail();
  NWC24::ErrorCode FetchDownload(u16 entry_index, std::optional<u8> subtask_id);
  void FinishSentMail(const std::string& response, const std::vector<u32>& sent);

  void LogError(ErrorType error_type, s32 error_code);
  void SetCurrentFunction(CurrentFunction function);
  void SchedulerTimer();
  void SchedulerWorker(SchedulerEvent event);
  NWC24::ErrorCode DetermineDownloadTask(u16* entry_index, std::optional<u8>* subtask_id) const;
  NWC24::ErrorCode DetermineSubtask(u16 entry_index, std::optional<u8>* subtask_id) const;

  static constexpr u32 MAX_MAIL_SIZE = 208952;
  static std::string GetValueFromCGIResponse(const std::string& response, const std::string& key);

  static constexpr u32 DEFAULT_SCHEDULER_SPAN_MINUTES = 11;

  NWC24::NWC24Config m_config;
  NWC24::NWC24Dl m_dl_list;
  NWC24::Mail::WC24SendList m_send_list;
  NWC24::Mail::WC24FriendList m_friend_list;

  Common::WorkQueueThreadSP<AsyncTask> m_work_queue;
  Common::AsyncWorkThreadSP m_scheduler_work_queue;
  std::mutex m_async_reply_lock;
  std::queue<AsyncReply> m_async_replies;

  // What GET_SCHEDULER_STAT reports (big endian words): [1] permissions, [2] last error,
  // [4] current function, [5] error count, [11] mail checks, [14] mail sends, [32..63] the
  // last 32 errors.
  std::mutex m_scheduler_buffer_lock;
  u32 m_error_count = 0;
  std::array<u32, 256> m_scheduler_buffer{};

  std::shared_ptr<NetKDTimeDevice> m_time_device;
  Common::HttpRequest m_http{std::chrono::minutes{1}};

  // Scheduler periods in minutes, guarded by m_scheduler_lock.
  std::mutex m_scheduler_lock;
  u32 m_download_span = 2;
  u32 m_mail_span = 1;
  bool m_handle_mail;
  Common::Event m_shutdown_event;
  std::thread m_scheduler_timer_thread;
};
}  // namespace IOS::HLE
