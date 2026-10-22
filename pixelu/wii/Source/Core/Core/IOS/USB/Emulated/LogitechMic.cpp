// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Requests and descriptors follow the public USB Device Class Definition for Audio Devices 1.0
// (usb.org, audio10.pdf).

#include "Core/IOS/USB/Emulated/LogitechMic.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <string>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
constexpr u16 VENDOR_ID = 0x046d;
constexpr u16 PRODUCT_ID = 0x0a03;
constexpr u8 EP_AUDIO_IN = 0x84;

// Audio Class request codes (bRequest). Get requests have the top bit set.
constexpr u8 SET_CUR = 0x01;
constexpr u8 GET_CUR = 0x81;
constexpr u8 GET_MIN = 0x82;
constexpr u8 GET_MAX = 0x83;
constexpr u8 GET_RES = 0x84;

// Control selectors (high byte of wValue).
constexpr u8 FU_MUTE = 0x01;
constexpr u8 FU_VOLUME = 0x02;
constexpr u8 FU_AUTOMATIC_GAIN = 0x07;
constexpr u8 EP_SAMPLING_FREQ = 0x01;

// wIndex of the feature unit requests: unit 2 on interface 0.
constexpr u16 FEATURE_UNIT = 0x0200;

// Volume range reported to the game, in 1/256 dB, and how the 0-255 setting maps onto it.
constexpr u16 VOLUME_MIN = 0x8001;
constexpr u16 VOLUME_MAX = 0x0800;
constexpr u16 VOLUME_STEP = 0x0088;
constexpr int VOLUME_SPAN = 0x8800;
constexpr int VOLUME_BASE = 0x8000;

// Completion delay of an isochronous transfer.
constexpr u32 ISO_TRANSFER_US = 1000;

const DeviceDescriptor DEVICE_DESCRIPTOR{
    .bLength = 0x12,
    .bDescriptorType = 0x01,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 0x08,
    .idVendor = VENDOR_ID,
    .idProduct = PRODUCT_ID,
    .bcdDevice = 0x0001,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

const ConfigDescriptor CONFIG_DESCRIPTOR{
    .bLength = 0x09,
    .bDescriptorType = 0x02,
    .wTotalLength = 0x0079,
    .bNumInterfaces = 2,
    .bConfigurationValue = 1,
    .iConfiguration = 3,
    .bmAttributes = 0x80,
    .MaxPower = 0x3c,
};

constexpr InterfaceDescriptor AudioInterface(u8 number, u8 alt, u8 endpoints, u8 subclass)
{
  return {.bLength = 0x09,
          .bDescriptorType = 0x04,
          .bInterfaceNumber = number,
          .bAlternateSetting = alt,
          .bNumEndpoints = endpoints,
          .bInterfaceClass = 0x01,  // audio
          .bInterfaceSubClass = subclass,
          .bInterfaceProtocol = 0,
          .iInterface = 0};
}
constexpr u8 AUDIO_CONTROL = 0x01;
constexpr u8 AUDIO_STREAMING = 0x02;

const EndpointDescriptor AUDIO_IN_ENDPOINT{
    .bLength = 0x09,
    .bDescriptorType = 0x05,
    .bEndpointAddress = EP_AUDIO_IN,
    .bmAttributes = 0x0d,  // isochronous, synchronous
    .wMaxPacketSize = 0x0060,
    .bInterval = 1,
};

// The whole configuration as GET_DESCRIPTOR returns it (121 bytes, multi-byte fields LE).
constexpr std::array<u8, 121> CONFIGURATION_BLOB = {
    // Configuration: 2 interfaces, total length 0x79, self-powered, 120 mA.
    0x09, 0x02, 0x79, 0x00, 0x02, 0x01, 0x03, 0x80, 0x3c,
    // Interface 0: audio control, no endpoints.
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    // Class-specific AC header: ADC 1.00, 0x27 bytes, one streaming interface (1).
    0x09, 0x24, 0x01, 0x00, 0x01, 0x27, 0x00, 0x01, 0x01,
    // Input terminal 0x0d: microphone (0x0201), one channel.
    0x0c, 0x24, 0x02, 0x0d, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    // Feature unit 2 fed by terminal 0x0d: mute and volume on the master channel.
    0x09, 0x24, 0x06, 0x02, 0x0d, 0x01, 0x03, 0x00, 0x00,
    // Output terminal 0x0a: USB streaming (0x0101), fed by unit 2.
    0x09, 0x24, 0x03, 0x0a, 0x01, 0x01, 0x00, 0x02, 0x00,
    // Interface 1 alt 0: audio streaming, no bandwidth.
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    // Interface 1 alt 1: audio streaming, one endpoint.
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    // Class-specific AS general: linked to terminal 0x0a, PCM.
    0x07, 0x24, 0x01, 0x0a, 0x00, 0x01, 0x00,
    // Type I format: mono, 2-byte subframes, 16 bits, five discrete rates
    // (8000, 11025, 22050, 44100 and 48000 Hz, 3 bytes LE each).
    0x17, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x05,  //
    0x40, 0x1f, 0x00, 0x11, 0x2b, 0x00, 0x22, 0x56, 0x00, 0x44, 0xac, 0x00, 0x80, 0xbb, 0x00,
    // Standard isochronous endpoint 0x84 (audio-class form with bRefresh/bSynchAddress).
    0x09, 0x05, 0x84, 0x0d, 0x60, 0x00, 0x01, 0x00, 0x00,
    // Class-specific endpoint: sampling frequency control, lock delay 1 ms.
    0x07, 0x25, 0x01, 0x01, 0x02, 0x01, 0x00};

// The channel a request addresses (low byte of wValue): 0 is the master channel and 0xff is
// "all channels"; only those are accepted.
bool IsMasterChannel(u16 value)
{
  const u8 channel = static_cast<u8>(value - 1);
  return channel == 0 || channel == 0xff;
}

u8 SelectorOf(const CtrlMessage& cmd)
{
  return static_cast<u8>(cmd.value >> 8);
}

class LogitechMicrophone final : public Microphone
{
public:
  LogitechMicrophone(const LogitechMicState& sampler, u8 index)
      : Microphone(sampler, fmt::format("Logitech Mic {}", index)), m_state(sampler),
        m_mic_index(index)
  {
  }

private:
#ifdef HAVE_CUBEB
  std::string GetInputDeviceId() const override
  {
    return Config::Get(Config::MAIN_LOGITECH_MIC_MICROPHONE[m_mic_index]);
  }
  std::string GetCubebStreamName() const override
  {
    return fmt::format("Dolphin Emulated Logitech USB Microphone {}", m_mic_index);
  }
  s16 GetVolumeModifier() const override
  {
    return Config::Get(Config::MAIN_LOGITECH_MIC_VOLUME_MODIFIER[m_mic_index]);
  }
  bool AreSamplesByteSwapped() const override { return false; }
#endif
  bool IsMicrophoneMuted() const override
  {
    return Config::Get(Config::MAIN_LOGITECH_MIC_MUTED[m_mic_index]);
  }
  // Room for 128 ms of audio.
  u32 GetStreamSize() const override { return BUFF_SIZE_SAMPLES * m_state.sample_rate / 250; }

  const LogitechMicState& m_state;
  const u8 m_mic_index;
};
}  // namespace

bool LogitechMicState::IsSampleOn() const
{
  return true;
}

bool LogitechMicState::IsMuted() const
{
  return mute;
}

u32 LogitechMicState::GetDefaultSamplingRate() const
{
  return DEFAULT_SAMPLING_RATE;
}

LogitechMic::LogitechMic(u8 index) : m_index(index)
{
  assert(index <= 3);
  m_id = u64{VENDOR_ID} << 32 | u64{PRODUCT_ID} << 16 | u64{9} << 8 | u64{index} + 1;
}

DeviceDescriptor LogitechMic::GetDeviceDescriptor() const
{
  return DEVICE_DESCRIPTOR;
}

std::vector<ConfigDescriptor> LogitechMic::GetConfigurations() const
{
  return {CONFIG_DESCRIPTOR};
}

std::vector<InterfaceDescriptor> LogitechMic::GetInterfaces(u8) const
{
  return {AudioInterface(0, 0, 0, AUDIO_CONTROL), AudioInterface(1, 0, 0, AUDIO_STREAMING),
          AudioInterface(1, 1, 1, AUDIO_STREAMING)};
}

std::vector<EndpointDescriptor> LogitechMic::GetEndpoints(u8, u8 iface, u8 alt) const
{
  // Only the streaming interface's active setting has an endpoint.
  if (iface != 1 || alt != 1)
    return {};
  return {AUDIO_IN_ENDPOINT};
}

bool LogitechMic::Attach()
{
  if (!m_device_attached)
  {
    DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Opening device", VENDOR_ID, PRODUCT_ID, m_index);
    if (m_microphone == nullptr)
    {
      m_microphone = std::make_unique<LogitechMicrophone>(m_sampler, m_index);
      m_microphone->Initialize();
    }
    m_device_attached = true;
  }
  return true;
}

bool LogitechMic::AttachAndChangeInterface(const u8 iface)
{
  if (!Attach())
    return false;
  return iface == m_active_interface || ChangeInterface(iface) == 0;
}

int LogitechMic::CancelTransfer(const u8 endpoint)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}:{}] Cancelling transfers (endpoint {:#x})", VENDOR_ID,
                PRODUCT_ID, m_index, m_active_interface, endpoint);
  return IPC_SUCCESS;
}

int LogitechMic::ChangeInterface(const u8 iface)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}:{}] Changing interface to {}", VENDOR_ID, PRODUCT_ID,
                m_index, m_active_interface, iface);
  m_active_interface = iface;
  return 0;
}

int LogitechMic::GetNumberOfAltSettings(u8 iface)
{
  return iface == 1 ? 2 : 0;
}

int LogitechMic::SetAltSetting(u8)
{
  return 0;
}

int LogitechMic::FeatureUnitGet(const CtrlMessage& cmd) const
{
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  if (cmd.index != FEATURE_UNIT)
    return IPC_STALL;

  const u8 selector = SelectorOf(cmd);
  if (selector == FU_MUTE && cmd.request == GET_CUR)
  {
    memory.Write_U8(m_sampler.mute ? 1 : 0, cmd.data_address);
    return 1;
  }
  if (selector != FU_VOLUME || !IsMasterChannel(cmd.value))
    return IPC_STALL;

  u16 answer;
  switch (cmd.request)
  {
  case GET_CUR:
    // 0-255 scaled onto VOLUME_BASE..VOLUME_BASE+VOLUME_SPAN, rounded.
    answer = static_cast<u16>((m_sampler.volume * VOLUME_SPAN + 127) / 255 + VOLUME_BASE);
    break;
  case GET_MIN:
    answer = VOLUME_MIN;
    break;
  case GET_MAX:
    answer = VOLUME_MAX;
    break;
  case GET_RES:
    answer = VOLUME_STEP;
    break;
  default:
    return IPC_STALL;
  }
  memory.Write_U16(answer, cmd.data_address);
  return 2;
}

int LogitechMic::FeatureUnitSet(const CtrlMessage& cmd)
{
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  if (cmd.index != FEATURE_UNIT || cmd.request != SET_CUR)
    return IPC_STALL;

  switch (SelectorOf(cmd))
  {
  case FU_MUTE:
    m_sampler.mute = (memory.Read_U8(cmd.data_address) & 1) != 0;
    return 0;
  case FU_VOLUME:
  {
    if (!IsMasterChannel(cmd.value))
      return IPC_STALL;
    // The inverse of the GET_CUR mapping, in 16-bit arithmetic. Some games (Lego Rock Band)
    // send values outside the reported range; they wrap the same way.
    const u16 raw = memory.Read_U16(cmd.data_address);
    const u16 offset = static_cast<u16>(raw - VOLUME_BASE);
    const u16 scaled = static_cast<u16>((offset * 255 + VOLUME_SPAN / 2) / VOLUME_SPAN);
    m_sampler.volume = static_cast<u8>(std::min<u16>(scaled, 255));
    DEBUG_LOG_FMT(IOS_USB, "Logitech Mic: volume {:02x} (from {:04x})", m_sampler.volume.load(),
                  raw);
    return 0;
  }
  case FU_AUTOMATIC_GAIN:
    // Accepted and ignored.
    return 0;
  default:
    return IPC_STALL;
  }
}

int LogitechMic::EndpointRequest(const CtrlMessage& cmd)
{
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  if (SelectorOf(cmd) != EP_SAMPLING_FREQ || cmd.index != EP_AUDIO_IN)
    return IPC_STALL;

  if (cmd.request == SET_CUR)
  {
    // The new rate is 3 bytes, little-endian, and only taken for "all channels".
    if (static_cast<u8>(cmd.value - 1) == 0xff)
    {
      u32 rate = 0;
      for (u32 i = 0; i < 3; ++i)
        rate |= u32{memory.Read_U8(cmd.data_address + i)} << (8 * i);
      if (rate != m_sampler.sample_rate)
      {
        m_sampler.sample_rate = rate;
        if (m_microphone != nullptr)
        {
          DEBUG_LOG_FMT(IOS_USB, "Logitech Mic: sampling rate {}", rate);
          m_microphone->SetSamplingRate(rate);
        }
      }
    }
    return 0;
  }
  if (cmd.request == GET_CUR)
  {
    // Reported most significant byte first (unlike SET_CUR), written from the last byte.
    const u32 rate = m_sampler.sample_rate;
    for (u32 i = 0; i < 3; ++i)
      memory.Write_U8(static_cast<u8>(rate >> (8 * i)), cmd.data_address + 2 - i);
    return 3;
  }
  return IPC_STALL;
}

int LogitechMic::SubmitTransfer(std::unique_ptr<CtrlMessage> message)
{
  CtrlMessage& cmd = *message;
  EmulationKernel& ios = cmd.GetEmulationKernel();
  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}:{}] Control: bRequestType={:02x} bRequest={:02x} wValue={:04x}"
                " wIndex={:04x} wLength={:04x}",
                VENDOR_ID, PRODUCT_ID, m_index, m_active_interface, cmd.request_type, cmd.request,
                cmd.value, cmd.index, cmd.length);

  const u8 type = cmd.request_type;
  const u8 request = cmd.request;
  // Audio Class requests 1-4 (SET_*) and 0x81-0x84 (GET_*).
  const bool is_set = request >= 0x01 && request <= 0x04;
  const bool is_get = request >= 0x81 && request <= 0x84;

  const u8 standard_device_in = 0x80;
  const u8 standard_interface_out = 0x01;
  const u8 class_interface_out = 0x21;
  const u8 class_interface_in = 0xa1;
  const u8 class_endpoint_out = 0x22;

  if (type == standard_device_in && request == REQUEST_GET_DESCRIPTOR)
  {
    // Games read the first 9 bytes, then the full length given there.
    cmd.FillBuffer(CONFIGURATION_BLOB.data(),
                   std::min<size_t>(cmd.length, CONFIGURATION_BLOB.size()));
    ios.EnqueueIPCReply(cmd.ios_request, IPC_SUCCESS);
    return IPC_SUCCESS;
  }
  if (type == standard_interface_out && request == REQUEST_SET_INTERFACE)
  {
    const u8 iface = static_cast<u8>(cmd.index);
    if (iface != m_active_interface)
    {
      if (const int error = ChangeInterface(iface); error < 0)
        return error;
    }
    const int result = SetAltSetting(static_cast<u8>(cmd.value));
    if (result == 0)
      ios.EnqueueIPCReply(cmd.ios_request, cmd.length);
    return result;
  }

  int result = IPC_STALL;
  // Feature unit reads come as class IN requests on the interface; writes as class OUT, though
  // SET requests sent as IN are accepted too. Endpoint requests are all class OUT.
  if (type == class_interface_in && is_get)
    result = FeatureUnitGet(cmd);
  else if ((type == class_interface_in || type == class_interface_out) && is_set)
    result = FeatureUnitSet(cmd);
  else if (type == class_endpoint_out && (is_get || is_set))
    result = EndpointRequest(cmd);
  else
    NOTICE_LOG_FMT(IOS_USB, "Unknown command");

  if (result < 0)
  {
    WARN_LOG_FMT(IOS_USB, "Logitech Mic: request {:02x}/{:02x} (value {:04x} index {:04x}) stalled",
                 type, request, cmd.value, cmd.index);
    result = IPC_STALL;
  }
  ios.EnqueueIPCReply(cmd.ios_request, result);
  return IPC_SUCCESS;
}

int LogitechMic::SubmitTransfer(std::unique_ptr<BulkMessage> cmd)
{
  cmd->GetEmulationKernel().EnqueueIPCReply(cmd->ios_request, IPC_SUCCESS);
  return IPC_SUCCESS;
}

int LogitechMic::SubmitTransfer(std::unique_ptr<IntrMessage> cmd)
{
  cmd->GetEmulationKernel().EnqueueIPCReply(cmd->ios_request, IPC_SUCCESS);
  return IPC_SUCCESS;
}

int LogitechMic::SubmitTransfer(std::unique_ptr<IsoMessage> message)
{
  IsoMessage& cmd = *message;
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  u8* const buffer = memory.GetPointerForRange(cmd.data_address, cmd.length);
  if (buffer == nullptr)
  {
    ERROR_LOG_FMT(IOS_USB, "Logitech USB Microphone command invalid");
    return IPC_EINVAL;
  }

  if (cmd.endpoint == EP_AUDIO_IN)
  {
    u16 filled = 0;
    const u32 wanted_samples = static_cast<u32>(cmd.length / sizeof(s16));
    if (m_microphone != nullptr && m_microphone->HasData(wanted_samples))
      filled = m_microphone->ReadIntoBuffer(buffer, cmd.length);
    const u16 lost = cmd.SetPacketsReturnValueFromSize(filled);
    if (lost != 0)
      WARN_LOG_FMT(IOS_USB, "Logitech Mic: {} byte(s) did not fit the isochronous packets", lost);
  }
  else
  {
    WARN_LOG_FMT(IOS_USB, "Logitech Mic: isochronous transfer on unsupported endpoint {:02x}",
                 cmd.endpoint);
  }

  // The buffer is written in place; this goes through the regular copy path (and its checks).
  cmd.FillBuffer(buffer, cmd.length);
  cmd.ScheduleTransferCompletion(static_cast<s32>(cmd.length), ISO_TRANSFER_US);
  return IPC_SUCCESS;
}
}  // namespace IOS::HLE::USB
