// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

class PointerWrap;

namespace IOS::HLE
{
// /dev/sdio/slot0: the SD card slot on the front of the console, backed by a card image file.
class SDIOSlot0Device : public EmulationDevice
{
public:
  SDIOSlot0Device(EmulationKernel& ios, const std::string& device_name);
  ~SDIOSlot0Device() override;

  void DoState(PointerWrap& p) override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

private:
  // Host controller registers with special write behaviour.
  enum
  {
    HCR_CLOCKCONTROL = 0x2C,
    HCR_SOFTWARERESET = 0x2F,
  };

  // ioctl numbers
  enum
  {
    IOCTL_WRITEHCR = 0x01,
    IOCTL_READHCR = 0x02,
    IOCTL_RESETCARD = 0x04,
    IOCTL_SETCLK = 0x06,
    IOCTL_SENDCMD = 0x07,
    IOCTL_GETSTATUS = 0x0B,
    IOCTL_GETOCR = 0x0C,
  };

  // ioctlv numbers
  enum
  {
    IOCTLV_SENDCMD = 0x07,
  };

  // ExecuteCommand results. RET_EVENT_REGISTER only means "reply later".
  enum
  {
    RET_OK,
    RET_FAIL,
    RET_LOCKED = -10,
    RET_EVENT_REGISTER,
  };

  // Card status bits (IOCTL_GETSTATUS)
  enum
  {
    CARD_NOT_EXIST = 0,
    CARD_INSERTED = 0x1,
    CARD_LOCKED = 0x4,
    CARD_INITIALIZED = 0x10000,
    CARD_SDHC = 0x100000,
  };

  // SD commands (and the IOS-specific event commands)
  enum
  {
    GO_IDLE_STATE = 0x00,
    ALL_SEND_CID = 0x02,
    SEND_RELATIVE_ADDR = 0x03,
    SELECT_CARD = 0x07,
    SEND_IF_COND = 0x08,
    SEND_CSD = 0x09,
    SEND_CID = 0x0A,
    SEND_STATUS = 0x0D,
    SET_BLOCKLEN = 0x10,
    READ_MULTIPLE_BLOCK = 0x12,
    WRITE_MULTIPLE_BLOCK = 0x19,
    APP_CMD_NEXT = 0x37,

    ACMD_SETBUSWIDTH = 0x06,
    ACMD_SENDOPCOND = 0x29,
    ACMD_SENDSCR = 0x33,

    EVENT_REGISTER = 0x40,
    EVENT_UNREGISTER = 0x41,
  };

  enum EventType
  {
    EVENT_INSERT = 1,
    EVENT_REMOVE = 2,
    // What a cancelled event is answered with.
    EVENT_INVALID = 0xc210000
  };

  enum class SDProtocol
  {
    V1,
    V2,
  };

  // Images larger than this are presented as SDHC cards.
  static constexpr u64 SDSC_MAX_SIZE = 0x80000000;

  // A pending insert/remove notification request.
  struct Event
  {
    Event(EventType type_, Request request_) : type(type_), request(request_) {}
    EventType type;
    Request request;
  };

  void RefreshConfig();

  void EventNotify();

  IPCReply WriteHCRegister(const IOCtlRequest& request);
  IPCReply ReadHCRegister(const IOCtlRequest& request);
  IPCReply ResetCard(const IOCtlRequest& request);
  IPCReply SetClk(const IOCtlRequest& request);
  std::optional<IPCReply> SendCommand(const IOCtlRequest& request);
  IPCReply GetStatus(const IOCtlRequest& request);
  IPCReply GetOCRegister(const IOCtlRequest& request);

  IPCReply SendCommand(const IOCtlVRequest& request);

  s32 ExecuteCommand(const Request& request, u32 buffer_in, u32 buffer_in_size, u32 rw_buffer,
                     u32 rw_buffer_size, u32 buffer_out, u32 buffer_out_size);
  void OpenInternal();

  u32 GetOCRegister() const;

  std::array<u32, 4> GetCSDv1() const;
  std::array<u32, 4> GetCSDv2() const;
  void InitSDHC();

  u64 GetAddressFromRequest(u32 arg) const;

  std::unique_ptr<Event> m_event;

  u32 m_status = CARD_NOT_EXIST;
  SDProtocol m_protocol = SDProtocol::V1;

  // Whether this IOS version initialises SDHC cards by itself (others need SEND_IF_COND).
  const bool m_sdhc_supported;

  u32 m_block_length = 0;
  u32 m_bus_width = 0;

  std::array<u32, 0x200 / sizeof(u32)> m_registers{};

  File::IOFile m_card;

  CPUThreadConfigCallback::ConfigChangedCallbackID m_config_callback_id;
  bool m_sd_card_inserted = false;
};
}  // namespace IOS::HLE
