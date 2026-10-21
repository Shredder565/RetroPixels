// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// The register layout follows public community documentation of the Wii Speak (wiibrew).

#include "Core/IOS/USB/Emulated/WiiSpeak.h"

#include <algorithm>
#include <array>
#include <string>

#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
constexpr u16 VENDOR_ID = 0x057E;
constexpr u16 PRODUCT_ID = 0x0308;

constexpr u8 EP_AUDIO_IN = 0x81;
constexpr u8 EP_AUDIO_OUT = 0x03;
constexpr u8 EP_DATA_OUT = 0x02;

// Vendor requests (interface recipient).
constexpr u8 VENDOR_RESET = 0;
constexpr u8 VENDOR_WRITE_REGISTER = 1;
constexpr u8 VENDOR_READ_REGISTER = 2;

// Registers. Their low bit is ignored.
constexpr u8 REG_SAMPLER = 0x00;
constexpr u8 REG_FREQUENCY = 0x02;
constexpr u8 REG_GAIN = 0x04;
constexpr u8 REG_MUTE = 0x0c;
constexpr u8 REG_ECHO_CANCEL = 0x14;
constexpr u8 REG_SPEECH = 0x38;

// Sub-commands of REG_SPEECH, given in the register's value.
constexpr u16 SPEECH_ENABLE = 0x1010;
constexpr u16 SPEECH_SIN = 0x2001;
constexpr u16 SPEECH_SOUT = 0x2004;  // reads back the loudness level
constexpr u16 SPEECH_RIN = 0x200d;

// Register codes for the sampling rates and gains; anything unknown means the last entry
// (16 kHz, 36 dB), which is also the device default.
struct CodedValue
{
  u16 code;
  int value;
};
constexpr std::array<CodedValue, 3> FREQUENCIES{{{0, 8000}, {1, 11025}, {3, 16000}}};
constexpr std::array<CodedValue, 4> GAINS{{{0, 0}, {1, 15}, {2, 30}, {3, 36}}};
// Gain readings carry these bits on top of the code.
constexpr u16 GAIN_READ_BITS = 0x300;

template <size_t N>
int ValueForCode(const std::array<CodedValue, N>& table, u16 code, const char* what)
{
  for (const CodedValue& entry : table)
  {
    if (entry.code == code)
      return entry.value;
  }
  WARN_LOG_FMT(IOS_USB, "Wii Speak: unknown {} code {:04x}, using the default", what, code);
  return table.back().value;
}

template <size_t N>
u16 CodeForValue(const std::array<CodedValue, N>& table, int value, const char* what)
{
  for (const CodedValue& entry : table)
  {
    if (entry.value == value)
      return entry.code;
  }
  WARN_LOG_FMT(IOS_USB, "Wii Speak: {} {} has no register code, reporting the default", what,
               value);
  return table.back().code;
}

const DeviceDescriptor DEVICE_DESCRIPTOR{
    .bLength = 0x12,
    .bDescriptorType = 0x01,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 0x10,
    .idVendor = VENDOR_ID,
    .idProduct = PRODUCT_ID,
    .bcdDevice = 0x0214,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

const ConfigDescriptor CONFIG_DESCRIPTOR{
    .bLength = 0x09,
    .bDescriptorType = 0x02,
    .wTotalLength = 0x0030,
    .bNumInterfaces = 1,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = 0x80,
    .MaxPower = 0x32,
};

constexpr InterfaceDescriptor VendorInterface(u8 alt, u8 endpoints)
{
  return {.bLength = 0x09,
          .bDescriptorType = 0x04,
          .bInterfaceNumber = 0,
          .bAlternateSetting = alt,
          .bNumEndpoints = endpoints,
          .bInterfaceClass = 0xFF,
          .bInterfaceSubClass = 0xFF,
          .bInterfaceProtocol = 0xFF,
          .iInterface = 0};
}

constexpr EndpointDescriptor Endpoint(u8 address, u8 attributes, u16 max_packet, u8 interval)
{
  return {.bLength = 0x07,
          .bDescriptorType = 0x05,
          .bEndpointAddress = address,
          .bmAttributes = attributes,
          .wMaxPacketSize = max_packet,
          .bInterval = interval};
}

constexpr u8 ISOCHRONOUS = 1;
constexpr u8 BULK = 2;

// Completion delay of an isochronous transfer. Games show the "talking" cue only if audio
// arrives fast enough, and sending faster makes it choppier.
constexpr u32 ISO_TRANSFER_US = 2000;

class WiiSpeakMicrophone final : public Microphone
{
public:
  explicit WiiSpeakMicrophone(const WiiSpeakState& sampler)
      : Microphone(sampler, "Wii Speak Worker")
  {
  }

private:
#ifdef HAVE_CUBEB
  std::string GetInputDeviceId() const override
  {
    return Config::Get(Config::MAIN_WII_SPEAK_MICROPHONE);
  }
  std::string GetCubebStreamName() const override { return "Dolphin Emulated Wii Speak"; }
  s16 GetVolumeModifier() const override
  {
    return Config::Get(Config::MAIN_WII_SPEAK_VOLUME_MODIFIER);
  }
  // The Wii expects big-endian samples.
  bool AreSamplesByteSwapped() const override { return true; }
#endif
  bool IsMicrophoneMuted() const override { return Config::Get(Config::MAIN_WII_SPEAK_MUTED); }
  u32 GetStreamSize() const override { return BUFF_SIZE_SAMPLES * 500; }
};
}  // namespace

bool WiiSpeakState::IsSampleOn() const
{
  return sample_on;
}

bool WiiSpeakState::IsMuted() const
{
  return mute;
}

u32 WiiSpeakState::GetDefaultSamplingRate() const
{
  return DEFAULT_SAMPLING_RATE;
}

WiiSpeak::WiiSpeak()
{
  m_id = u64{VENDOR_ID} << 32 | u64{PRODUCT_ID} << 16 | u64{9} << 8 | u64{1};
}

WiiSpeak::~WiiSpeak() = default;

DeviceDescriptor WiiSpeak::GetDeviceDescriptor() const
{
  return DEVICE_DESCRIPTOR;
}

std::vector<ConfigDescriptor> WiiSpeak::GetConfigurations() const
{
  return {CONFIG_DESCRIPTOR};
}

std::vector<InterfaceDescriptor> WiiSpeak::GetInterfaces(u8) const
{
  return {VendorInterface(0, 0), VendorInterface(1, 3)};
}

std::vector<EndpointDescriptor> WiiSpeak::GetEndpoints(u8, u8, u8) const
{
  return {Endpoint(EP_AUDIO_IN, ISOCHRONOUS, 0x0020, 1), Endpoint(EP_DATA_OUT, BULK, 0x0020, 0),
          Endpoint(EP_AUDIO_OUT, ISOCHRONOUS, 0x0040, 1)};
}

bool WiiSpeak::Attach()
{
  if (!m_device_attached)
  {
    DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Opening device", VENDOR_ID, PRODUCT_ID);
    if (m_microphone == nullptr)
    {
      m_microphone = std::make_unique<WiiSpeakMicrophone>(m_sampler);
      m_microphone->Initialize();
    }
    m_device_attached = true;
  }
  return true;
}

bool WiiSpeak::AttachAndChangeInterface(const u8 f)
{
  if (!Attach())
    return false;
  return f == m_active_interface || ChangeInterface(f) == 0;
}

int WiiSpeak::CancelTransfer(const u8 endpoint)
{
  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Cancelling transfers (endpoint {:#x})", VENDOR_ID,
               PRODUCT_ID, m_active_interface, endpoint);
  return IPC_SUCCESS;
}

int WiiSpeak::ChangeInterface(const u8 iface)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Changing interface to {}", VENDOR_ID, PRODUCT_ID,
                m_active_interface, iface);
  m_active_interface = iface;
  return 0;
}

int WiiSpeak::GetNumberOfAltSettings(u8)
{
  return 0;
}

int WiiSpeak::SetAltSetting(u8)
{
  return 0;
}

int WiiSpeak::SubmitTransfer(std::unique_ptr<CtrlMessage> message)
{
  CtrlMessage& cmd = *message;
  EmulationKernel& ios = cmd.GetEmulationKernel();
  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}] Control: bRequestType={:02x} bRequest={:02x} wValue={:04x}"
                " wIndex={:04x} wLength={:04x}",
                VENDOR_ID, PRODUCT_ID, m_active_interface, cmd.request_type, cmd.request,
                cmd.value, cmd.index, cmd.length);

  const u16 setup = static_cast<u16>(cmd.request_type << 8 | cmd.request);
  if (setup == USBHDR(DIR_DEVICE2HOST, TYPE_STANDARD, REC_INTERFACE, REQUEST_GET_INTERFACE))
  {
    const u8 current_alt = 1;
    cmd.FillBuffer(&current_alt, 1);
    cmd.ScheduleTransferCompletion(1, 100);
    return IPC_SUCCESS;
  }
  if (setup == USBHDR(DIR_HOST2DEVICE, TYPE_STANDARD, REC_INTERFACE, REQUEST_SET_INTERFACE))
  {
    INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] REQUEST_SET_INTERFACE index={:04x} value={:04x}",
                 VENDOR_ID, PRODUCT_ID, m_active_interface, cmd.index, cmd.value);
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

  if (setup == USBHDR(DIR_HOST2DEVICE, TYPE_VENDOR, REC_INTERFACE, VENDOR_RESET))
  {
    m_ready = false;
  }
  else if (setup == USBHDR(DIR_DEVICE2HOST, TYPE_VENDOR, REC_INTERFACE, REQUEST_GET_DESCRIPTOR))
  {
    // Answers 0 the first time after a reset, 1 afterwards.
    const u8 ready = m_ready ? 1 : 0;
    cmd.FillBuffer(&ready, 1);
    m_ready = true;
  }
  else if (setup == USBHDR(DIR_HOST2DEVICE, TYPE_VENDOR, REC_INTERFACE, VENDOR_WRITE_REGISTER))
  {
    WriteRegister(cmd);
  }
  else if (setup == USBHDR(DIR_DEVICE2HOST, TYPE_VENDOR, REC_INTERFACE, VENDOR_READ_REGISTER))
  {
    ReadRegister(cmd);
  }
  else
  {
    NOTICE_LOG_FMT(IOS_USB, "Unknown command");
  }
  ios.EnqueueIPCReply(cmd.ios_request, IPC_SUCCESS);
  return IPC_SUCCESS;
}

int WiiSpeak::SubmitTransfer(std::unique_ptr<BulkMessage> message)
{
  message->GetEmulationKernel().EnqueueIPCReply(message->ios_request, IPC_SUCCESS);
  return IPC_SUCCESS;
}

int WiiSpeak::SubmitTransfer(std::unique_ptr<IntrMessage> message)
{
  message->GetEmulationKernel().EnqueueIPCReply(message->ios_request, IPC_SUCCESS);
  return IPC_SUCCESS;
}

int WiiSpeak::SubmitTransfer(std::unique_ptr<IsoMessage> message)
{
  IsoMessage& cmd = *message;
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  u8* const buffer = memory.GetPointerForRange(cmd.data_address, cmd.length);
  if (buffer == nullptr)
  {
    ERROR_LOG_FMT(IOS_USB, "Wii Speak command invalid");
    return IPC_EINVAL;
  }

  if (cmd.endpoint == EP_AUDIO_IN)
  {
    // Hand over captured audio only when a whole transfer's worth is ready.
    u16 filled = 0;
    const u32 wanted_samples = static_cast<u32>(cmd.length / sizeof(s16));
    if (m_microphone != nullptr && m_microphone->HasData(wanted_samples))
      filled = m_microphone->ReadIntoBuffer(buffer, cmd.length);
    const u16 lost = cmd.SetPacketsReturnValueFromSize(filled);
    if (lost != 0)
      WARN_LOG_FMT(IOS_USB, "Wii Speak: {} byte(s) did not fit the isochronous packets", lost);
  }
  else if (cmd.endpoint != EP_AUDIO_OUT)
  {
    // Audio sent to the device's speaker is ignored; anything else is unexpected.
    WARN_LOG_FMT(IOS_USB, "Wii Speak: isochronous transfer on unsupported endpoint {:02x}",
                 cmd.endpoint);
  }

  DEBUG_LOG_FMT(IOS_USB, "Wii Speak isochronous transfer: length={:04x} endpoint={:02x} packets={}",
                cmd.length, cmd.endpoint, cmd.num_packets);
  cmd.ScheduleTransferCompletion(IPC_SUCCESS, ISO_TRANSFER_US);
  return IPC_SUCCESS;
}

void WiiSpeak::WriteRegister(const CtrlMessage& cmd)
{
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  const u8 reg = memory.Read_U8(cmd.data_address + 1) & 0xFE;
  const u16 value = memory.Read_U16(cmd.data_address + 2);
  const u16 extra = memory.Read_U16(cmd.data_address + 4);
  DEBUG_LOG_FMT(IOS_USB, "Wii Speak register write: reg={:02x} value={:04x} extra={:04x}", reg,
                value, extra);

  switch (reg)
  {
  case REG_SAMPLER:
    m_sampler.sample_on = value != 0;
    break;
  case REG_MUTE:
    m_sampler.mute = value != 0;
    break;
  case REG_ECHO_CANCEL:
    m_sampler.ec_reset = value != 0;
    break;
  case REG_FREQUENCY:
    m_sampler.freq = ValueForCode(FREQUENCIES, value, "sampling rate");
    if (m_microphone != nullptr)
      m_microphone->SetSamplingRate(static_cast<u32>(m_sampler.freq));
    break;
  case REG_GAIN:
    // Stored and reported back, but not applied to the audio.
    m_sampler.gain = ValueForCode(GAINS, static_cast<u16>(value & ~GAIN_READ_BITS), "gain");
    break;
  case REG_SPEECH:
    if (value == SPEECH_ENABLE)
      m_sampler.sp_on = extra == 0;
    else if (value != SPEECH_SIN && value != SPEECH_SOUT && value != SPEECH_RIN)
      WARN_LOG_FMT(IOS_USB, "Wii Speak: unknown speech write {:04x} {:04x}", value, extra);
    break;
  default:
    WARN_LOG_FMT(IOS_USB, "Wii Speak: write to unknown register {:02x} ({:04x} {:04x})", reg,
                 value, extra);
    break;
  }
}

void WiiSpeak::ReadRegister(const CtrlMessage& cmd) const
{
  auto& memory = cmd.GetEmulationKernel().GetSystem().GetMemory();
  const u8 reg = memory.Read_U8(cmd.data_address + 1) & 0xFE;
  const u32 value_address = cmd.data_address + 2;
  const u32 extra_address = cmd.data_address + 4;
  DEBUG_LOG_FMT(IOS_USB, "Wii Speak register read: reg={:02x}", reg);

  switch (reg)
  {
  case REG_SAMPLER:
    memory.Write_U16(m_sampler.sample_on ? 1 : 0, value_address);
    break;
  case REG_MUTE:
    memory.Write_U16(m_sampler.mute ? 1 : 0, value_address);
    break;
  case REG_ECHO_CANCEL:
    memory.Write_U16(m_sampler.ec_reset ? 1 : 0, value_address);
    break;
  case REG_FREQUENCY:
    memory.Write_U16(CodeForValue(FREQUENCIES, m_sampler.freq, "sampling rate"), value_address);
    break;
  case REG_GAIN:
    memory.Write_U16(static_cast<u16>(GAIN_READ_BITS | CodeForValue(GAINS, m_sampler.gain, "gain")),
                     value_address);
    break;
  case REG_SPEECH:
  {
    const u16 query = memory.Read_U16(value_address);
    if (query == SPEECH_ENABLE)
    {
      memory.Write_U16(1, extra_address);
    }
    else if (query == SPEECH_SOUT)
    {
      const u16 level = m_microphone != nullptr ? m_microphone->GetLoudnessLevel() : 0;
      memory.Write_U16(level, extra_address);
    }
    else if (query != SPEECH_SIN && query != SPEECH_RIN)
    {
      WARN_LOG_FMT(IOS_USB, "Wii Speak: unknown speech read {:04x}", query);
    }
    break;
  }
  default:
    WARN_LOG_FMT(IOS_USB, "Wii Speak: read of unknown register {:02x}", reg);
    break;
  }
}
}  // namespace IOS::HLE::USB
