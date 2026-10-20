// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The emulated remote's side of the HID protocol: output reports from the console, the replies
// and acknowledgements they cause, extension hot-plugging and savestates.

#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"

namespace WiimoteEmu
{
using namespace WiimoteCommon;

namespace
{
// Read replies carry at most 16 bytes.
constexpr u16 READ_CHUNK = 16;

// Battery levels below this set the low-battery flag.
constexpr u8 LOW_BATTERY_LEVEL = 0x20;
}  // namespace

// Runs `handler` with the report payload, unless the report is too short for it.
template <typename T, typename H>
void Wiimote::InvokeHandler(H&& handler, const WiimoteCommon::OutputReportGeneric& rpt, u32 size)
{
  if (size >= sizeof(T))
  {
    (this->*handler)(Common::BitCastPtr<T>(&rpt.data[0]));
    return;
  }

  ERROR_LOG_FMT(WIIMOTE, "Output report {:#04x} is too short ({} bytes)",
                static_cast<u8>(rpt.rpt_id), size);
}

void Wiimote::EventLinked()
{
  Reset();
}

void Wiimote::EventUnlinked()
{
  Reset();
}

void Wiimote::InterruptDataOutput(const u8* data, u32 size)
{
  if (size == 0)
  {
    ERROR_LOG_FMT(WIIMOTE, "Empty output report");
    return;
  }

  const auto& rpt = *reinterpret_cast<const OutputReportGeneric*>(data);
  const u32 payload_size = size - static_cast<u32>(OutputReportGeneric::HEADER_SIZE);

  if (payload_size == 0)
  {
    ERROR_LOG_FMT(WIIMOTE, "Output report {:#04x} without payload", static_cast<u8>(rpt.rpt_id));
    return;
  }

  // Bit 0 of the first payload byte drives the rumble motor in every output report (WiiBrew).
  InvokeHandler<OutputReportRumble>(&Wiimote::HandleReportRumble, rpt, payload_size);

  switch (rpt.rpt_id)
  {
  case OutputReportID::Rumble:
    // Already done.
    break;
  case OutputReportID::LED:
    InvokeHandler<OutputReportLeds>(&Wiimote::HandleReportLeds, rpt, payload_size);
    break;
  case OutputReportID::ReportMode:
    InvokeHandler<OutputReportMode>(&Wiimote::HandleReportMode, rpt, payload_size);
    break;
  case OutputReportID::IRLogicEnable:
    InvokeHandler<OutputReportEnableFeature>(&Wiimote::HandleIRLogicEnable, rpt, payload_size);
    break;
  case OutputReportID::SpeakerEnable:
    InvokeHandler<OutputReportEnableFeature>(&Wiimote::HandleSpeakerEnable, rpt, payload_size);
    break;
  case OutputReportID::RequestStatus:
    InvokeHandler<OutputReportRequestStatus>(&Wiimote::HandleRequestStatus, rpt, payload_size);
    break;
  case OutputReportID::WriteData:
    InvokeHandler<OutputReportWriteData>(&Wiimote::HandleWriteData, rpt, payload_size);
    break;
  case OutputReportID::ReadData:
    InvokeHandler<OutputReportReadData>(&Wiimote::HandleReadData, rpt, payload_size);
    break;
  case OutputReportID::SpeakerData:
    InvokeHandler<OutputReportSpeakerData>(&Wiimote::HandleSpeakerData, rpt, payload_size);
    break;
  case OutputReportID::SpeakerMute:
    InvokeHandler<OutputReportEnableFeature>(&Wiimote::HandleSpeakerMute, rpt, payload_size);
    break;
  case OutputReportID::IRLogicEnable2:
    InvokeHandler<OutputReportEnableFeature>(&Wiimote::HandleIRLogicEnable2, rpt, payload_size);
    break;
  default:
    PanicAlertFmt("Unknown Wii Remote output report {:#04x}", static_cast<u8>(rpt.rpt_id));
    break;
  }
}

void Wiimote::SendAck(OutputReportID rpt_id, ErrorCode error_code)
{
  TypedInputData<InputReportAck> report(InputReportID::Ack);
  report.payload.buttons = m_status.buttons;
  report.payload.rpt_id = rpt_id;
  report.payload.error_code = error_code;

  InterruptDataInputCallback(report.GetData(), report.GetSize());
}

void Wiimote::HandleReportRumble(const WiimoteCommon::OutputReportRumble& rpt)
{
  // Never acknowledged by a real remote.
  SetRumble(rpt.rumble);
}

void Wiimote::HandleReportLeds(const WiimoteCommon::OutputReportLeds& rpt)
{
  m_status.leds = rpt.leds;

  if (rpt.ack)
    SendAck(OutputReportID::LED, ErrorCode::Success);
}

void Wiimote::HandleReportMode(const OutputReportMode& dr)
{
  // A real remote ignores the whole report when the mode is unknown.
  if (!DataReportBuilder::IsValidMode(dr.mode))
  {
    WARN_LOG_FMT(WIIMOTE, "Ignoring unknown data reporting mode {:#04x}",
                 static_cast<u8>(dr.mode));
    return;
  }

  // TODO: A real remote also sends a data report right away (after the ack), even in
  // non-continuous core mode with unchanged buttons.
  m_reporting_continuous = dr.continuous;
  m_reporting_mode = dr.mode;

  if (dr.ack)
    SendAck(OutputReportID::ReportMode, ErrorCode::Success);
}

void Wiimote::HandleIRLogicEnable(const WiimoteCommon::OutputReportEnableFeature& rpt)
{
  // WiiBrew calls 0x13 "IR pixel clock enable", but on hardware it sets the status bit and
  // decides whether the camera answers on the i2c bus.
  m_status.ir = rpt.enable;
  m_camera_logic.SetEnabled(m_status.ir);

  if (rpt.ack)
    SendAck(OutputReportID::IRLogicEnable, ErrorCode::Success);
}

void Wiimote::HandleIRLogicEnable2(const WiimoteCommon::OutputReportEnableFeature& rpt)
{
  // Not emulated: the camera data is produced either way.
  if (rpt.ack)
    SendAck(OutputReportID::IRLogicEnable2, ErrorCode::Success);
}

void Wiimote::HandleSpeakerMute(const WiimoteCommon::OutputReportEnableFeature& rpt)
{
  m_speaker_mute = rpt.enable;

  if (rpt.ack)
    SendAck(OutputReportID::SpeakerMute, ErrorCode::Success);
}

void Wiimote::HandleSpeakerEnable(const WiimoteCommon::OutputReportEnableFeature& rpt)
{
  m_status.speaker = rpt.enable;

  if (rpt.ack)
    SendAck(OutputReportID::SpeakerEnable, ErrorCode::Success);
}

void Wiimote::HandleSpeakerData(const WiimoteCommon::OutputReportSpeakerData& rpt)
{
  // Speaker data is not acknowledged. (Real remotes sometimes answer data that arrives too fast
  // with an error ack; that is not emulated.)
  // TODO: Find out whether mute or the speaker enable bit stop the decoder on hardware.
  if (m_speaker_mute)
    return;

  if (rpt.length > std::size(rpt.data))
  {
    ERROR_LOG_FMT(WIIMOTE, "Speaker data report with bad length {}", rpt.length);
    return;
  }

  // The remote writes the samples to the speaker's data register.
  m_i2c_bus.BusWrite(SpeakerLogic::I2C_ADDR, SpeakerLogic::SPEAKER_DATA_OFFSET, rpt.length,
                     std::data(rpt.data));
}

void Wiimote::HandleRequestStatus(const OutputReportRequestStatus&)
{
  // The buttons were already set in Update(), which keeps them deterministic.
  m_status.extension = m_extension_port.IsDeviceConnected();

  // Deterministic sessions always report a full battery. (The setting is read regardless, as
  // its expression may keep state.)
  const double battery = m_battery_setting.GetValue() / ciface::BATTERY_INPUT_MAX_VALUE;
  m_status.SetEstimatedCharge(Core::WantsDeterminism() ? 1.f : float(battery));
  m_status.battery_low = m_status.battery < LOW_BATTERY_LEVEL;

  TypedInputData<InputReportStatus> report(InputReportID::Status);
  report.payload = m_status;
  InterruptDataInputCallback(report.GetData(), report.GetSize());
}

void Wiimote::HandleWriteData(const OutputReportWriteData& wd)
{
  // Hardware sometimes answers this with "busy"; games may depend on it working, so it does.
  if (m_read_request.size != 0)
    WARN_LOG_FMT(WIIMOTE, "Write while a read request is active");

  const u16 address = Common::swap16(wd.address);

  DEBUG_LOG_FMT(WIIMOTE, "Write {:#04x} @ {:#04x} @ {:#04x} ({} bytes)", wd.space,
                wd.slave_address, address, wd.size);

  // Silently ignored by a real remote.
  if (wd.size == 0 || wd.size > 16)
  {
    WARN_LOG_FMT(WIIMOTE, "Ignoring write of {} bytes", wd.size);
    return;
  }

  ErrorCode result = ErrorCode::Success;
  const auto space = static_cast<AddressSpace>(wd.space);

  if (space == AddressSpace::EEPROM)
  {
    if (address + wd.size > EEPROM_FREE_SIZE)
    {
      WARN_LOG_FMT(WIIMOTE, "EEPROM write out of range");
      result = ErrorCode::InvalidAddress;
    }
    else
    {
      std::copy_n(wd.data, wd.size, m_eeprom.data.data() + address);
      m_eeprom_dirty = true;
    }
  }
  else if (space == AddressSpace::I2CBus || space == AddressSpace::I2CBusAlt)
  {
    // The EEPROM cannot be reached over i2c (error 8). Note that this has always looked at the
    // slave address of the current *read* request, which is kept for compatibility.
    if (m_read_request.slave_address == EEPROM_I2C_ADDR)
    {
      WARN_LOG_FMT(WIIMOTE, "Refusing direct i2c write to the EEPROM");
      result = ErrorCode::InvalidAddress;
    }
    // Only the low address byte reaches the bus. A short write (usually a missing device) is a
    // NACK (error 7).
    else if (m_i2c_bus.BusWrite(wd.slave_address, u8(address), wd.size, wd.data) != wd.size)
    {
      result = ErrorCode::Nack;
    }
  }
  else
  {
    WARN_LOG_FMT(WIIMOTE, "Write to unknown address space {:#x}", wd.space);
    result = ErrorCode::InvalidSpace;
  }

  // Writes are always acknowledged.
  SendAck(OutputReportID::WriteData, result);
}

void Wiimote::HandleReadData(const OutputReportReadData& rd)
{
  // A second read while one is running is answered with "busy" (seen on a Wii Remote Plus).
  if (m_read_request.size != 0)
  {
    WARN_LOG_FMT(WIIMOTE, "Read requested while another one is active");
    SendAck(OutputReportID::ReadData, ErrorCode::Busy);
    return;
  }

  // Served by ProcessReadDataRequest, 16 bytes at a time; a size of zero does nothing.
  m_read_request.space = static_cast<AddressSpace>(rd.space);
  m_read_request.slave_address = rd.slave_address;
  m_read_request.address = Common::swap16(rd.address);
  m_read_request.size = Common::swap16(rd.size);

  DEBUG_LOG_FMT(WIIMOTE, "Read {} @ {:#04x} @ {:#04x} ({} bytes)",
                static_cast<u8>(m_read_request.space), m_read_request.slave_address,
                m_read_request.address, m_read_request.size);

  // The first reply goes out right away; the rest follow on later updates. Reads are not
  // otherwise acknowledged.
  ProcessReadDataRequest();
}

bool Wiimote::ProcessReadDataRequest()
{
  const u16 chunk = std::min(READ_CHUNK, m_read_request.size);
  if (chunk == 0)
    return false;

  TypedInputData<InputReportReadDataReply> report(InputReportID::ReadDataReply);
  InputReportReadDataReply& reply = report.payload;

  reply.buttons = m_status.buttons;
  reply.address = Common::swap16(m_read_request.address);
  // Unread bytes (errors, short chunks) are zero.
  std::fill(std::begin(reply.data), std::end(reply.data), u8(0));

  ErrorCode result = ErrorCode::Success;

  switch (m_read_request.space)
  {
  case AddressSpace::EEPROM:
    // A request reaching past the end fails as a whole, even its readable start. Games rely on
    // this: some only read the calibration after their first read at 0x1770 failed.
    if (m_read_request.address + m_read_request.size > EEPROM_FREE_SIZE)
    {
      result = ErrorCode::InvalidAddress;
    }
    else
    {
      std::copy_n(m_eeprom.data.data() + m_read_request.address, chunk, reply.data);
      reply.size_minus_one = static_cast<u8>(chunk - 1) & 0xf;
    }
    break;

  case AddressSpace::I2CBus:
  case AddressSpace::I2CBusAlt:
  {
    if (m_read_request.slave_address == EEPROM_I2C_ADDR)
    {
      WARN_LOG_FMT(WIIMOTE, "Refusing direct i2c read of the EEPROM");
      result = ErrorCode::InvalidAddress;
      break;
    }

    // Only the low address byte reaches the bus; the reply still echoes all 16 bits.
    const int got = m_i2c_bus.BusRead(m_read_request.slave_address, u8(m_read_request.address),
                                      chunk, reply.data);
    if (got != chunk)
    {
      DEBUG_LOG_FMT(WIIMOTE, "i2c read failed (error 7) @ {:#x} @ {:#x} ({} bytes)",
                    m_read_request.slave_address, m_read_request.address, m_read_request.size);
      result = ErrorCode::Nack;
      break;
    }

    reply.size_minus_one = static_cast<u8>(got - 1) & 0xf;
    break;
  }

  default:
    WARN_LOG_FMT(WIIMOTE, "Read from unknown address space {:#x}",
                 static_cast<int>(m_read_request.space));
    result = ErrorCode::InvalidSpace;
    break;
  }

  if (result == ErrorCode::Success)
  {
    // Advance; a size of zero ends the request.
    m_read_request.address = static_cast<u16>(m_read_request.address + chunk);
    m_read_request.size = static_cast<u16>(m_read_request.size - chunk);
  }
  else
  {
    // An error ends the request and reports the largest size.
    m_read_request.size = 0;
    reply.size_minus_one = 0xf;
  }

  reply.error = static_cast<u8>(result);

  InterruptDataInputCallback(report.GetData(), report.GetSize());
  return true;
}

void Wiimote::HandleExtensionSwap(ExtensionNumber desired_extension_number,
                                  bool desired_motion_plus)
{
  // A balance board takes no attachments.
  if (m_index == WIIMOTE_BALANCE_BOARD)
    return;

  // Attaching a device to a port also puts it on that port's i2c bus.

  // Removing the MotionPlus takes the whole update, and also unplugs what was plugged into it.
  if (m_is_motion_plus_attached && !desired_motion_plus)
  {
    INFO_LOG_FMT(WIIMOTE, "Remote {} (slot {}): unplugging MotionPlus", m_index,
                 m_bt_device_index);

    m_extension_port.AttachExtension(GetNoneExtension());
    m_is_motion_plus_attached = false;

    m_active_extension = ExtensionNumber::NONE;
    m_motion_plus.GetExtPort().AttachExtension(GetNoneExtension());
    return;
  }

  // The MotionPlus goes in first: unplug any extension (below) and plug the MotionPlus in on a
  // later update, once the port is empty.
  if (desired_motion_plus && !m_is_motion_plus_attached)
  {
    if (GetActiveExtensionNumber() == ExtensionNumber::NONE)
    {
      INFO_LOG_FMT(WIIMOTE, "Remote {} (slot {}): plugging in MotionPlus", m_index,
                   m_bt_device_index);

      m_is_motion_plus_attached = true;
      m_extension_port.AttachExtension(&m_motion_plus);
      m_motion_plus.Reset();
    }
    else
    {
      desired_extension_number = ExtensionNumber::NONE;
    }
  }

  if (GetActiveExtensionNumber() == desired_extension_number)
    return;

  // Changing extensions goes through "none": unplug now, plug the new one in next time.
  if (GetActiveExtensionNumber() == ExtensionNumber::NONE)
  {
    INFO_LOG_FMT(WIIMOTE, "Remote {} (slot {}): plugging in extension {}", m_index,
                 m_bt_device_index, std::to_underlying(desired_extension_number));
    m_active_extension = desired_extension_number;
  }
  else
  {
    INFO_LOG_FMT(WIIMOTE, "Remote {} (slot {}): unplugging extension", m_index,
                 m_bt_device_index);
    m_active_extension = ExtensionNumber::NONE;
  }

  ExtensionPort& port =
      m_is_motion_plus_attached ? m_motion_plus.GetExtPort() : m_extension_port;
  port.AttachExtension(GetActiveExtension());

  GetActiveExtension()->Reset();
}

void Wiimote::DoState(PointerWrap& p)
{
  // Not saved: the index (fixed per object), the sensor bar position (a console setting) and
  // the IMU pointing state (part of the user's physical controller).

  p.Do(m_reporting_mode);
  p.Do(m_reporting_continuous);

  p.Do(m_speaker_mute);

  p.Do(m_status);
  p.Do(m_eeprom);
  p.Do(m_read_request);

  m_speaker_logic.DoState(p);
  m_camera_logic.DoState(p);

  // The camera's enable line follows the status bit.
  if (p.IsReadMode())
    m_camera_logic.SetEnabled(m_status.ir);

  p.Do(m_is_motion_plus_attached);
  p.Do(m_active_extension);

  // Rebuild the port connections for the (possibly loaded) attachment state.
  if (m_is_motion_plus_attached)
  {
    m_extension_port.AttachExtension(&m_motion_plus);
    m_motion_plus.GetExtPort().AttachExtension(GetActiveExtension());
    m_motion_plus.DoState(p);
  }
  else
  {
    m_extension_port.AttachExtension(GetNoneExtension());
    m_extension_port.AttachExtension(GetActiveExtension());
  }

  if (m_active_extension != ExtensionNumber::NONE)
    GetActiveExtension()->DoState(p);

  p.Do(m_swing_state);
  p.Do(m_tilt_state);
  p.Do(m_point_state);
  p.Do(m_shake_state);

  p.DoMarker("Wiimote");
}

ExtensionNumber Wiimote::GetActiveExtensionNumber() const
{
  return m_active_extension;
}

ControllerEmu::SubscribableSettingValue<bool>& Wiimote::GetMotionPlusSetting()
{
  return m_motion_plus_setting;
}

}  // namespace WiimoteEmu
