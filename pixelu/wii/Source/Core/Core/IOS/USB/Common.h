// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"

// Shared pieces of the emulated IOS USB stack: the standard USB descriptor layouts (USB 2.0
// specification, chapter 9), the transfer commands that the host interfaces (OH0, VEN, HID)
// build from IPC requests, and the abstract device every backend implements.

namespace IOS::HLE::USB
{
// IOS (and the guests using it) only ever talks to the first configuration of a device.
constexpr u8 DEFAULT_CONFIG_NUM = 0;

// bRequest values of the standard requests that need special handling by a backend.
enum StandardDeviceRequestCodes
{
  REQUEST_GET_DESCRIPTOR = 6,
  REQUEST_SET_CONFIGURATION = 9,
  REQUEST_GET_INTERFACE = 10,
  REQUEST_SET_INTERFACE = 11,
};

// Fields of bmRequestType: direction (bit 7), type (bits 5-6) and recipient (bits 0-4).
enum ControlRequestTypes
{
  DIR_HOST2DEVICE = 0,
  DIR_DEVICE2HOST = 1,
  TYPE_STANDARD = 0,
  TYPE_CLASS = 1,
  TYPE_VENDOR = 2,
  TYPE_RESERVED = 3,
  REC_DEVICE = 0,
  REC_INTERFACE = 1,
  REC_ENDPOINT = 2,
  REC_OTHER = 3,
};

// Packs bmRequestType and bRequest into one value (type in the high byte) so that a backend
// can switch over both at once.
constexpr u16 USBHDR(u8 dir, u8 type, u8 recipient, u8 request)
{
  const u32 request_type = (u32{dir} << 7) | (u32{type} << 5) | u32{recipient};
  return static_cast<u16>((request_type << 8) | u32{request});
}

// The descriptor structs below are kept in host byte order. Swap() turns the multi-byte fields
// into the big-endian order the PPC expects (and back).
struct DeviceDescriptor
{
  void Swap();
  u8 bLength;
  u8 bDescriptorType;
  u16 bcdUSB;
  u8 bDeviceClass;
  u8 bDeviceSubClass;
  u8 bDeviceProtocol;
  u8 bMaxPacketSize0;
  u16 idVendor;
  u16 idProduct;
  u16 bcdDevice;
  u8 iManufacturer;
  u8 iProduct;
  u8 iSerialNumber;
  u8 bNumConfigurations;
};

struct ConfigDescriptor
{
  void Swap();
  u8 bLength;
  u8 bDescriptorType;
  u16 wTotalLength;
  u8 bNumInterfaces;
  u8 bConfigurationValue;
  u8 iConfiguration;
  u8 bmAttributes;
  u8 MaxPower;
};

struct InterfaceDescriptor
{
  void Swap();
  u8 bLength;
  u8 bDescriptorType;
  u8 bInterfaceNumber;
  u8 bAlternateSetting;
  u8 bNumEndpoints;
  u8 bInterfaceClass;
  u8 bInterfaceSubClass;
  u8 bInterfaceProtocol;
  u8 iInterface;
};

struct EndpointDescriptor
{
  void Swap();
  u8 bLength;
  u8 bDescriptorType;
  u8 bEndpointAddress;
  u8 bmAttributes;
  u16 wMaxPacketSize;
  u8 bInterval;
};

// One transfer submitted by the PPC. It remembers the IPC request to answer and where the
// transfer's data lives in emulated memory.
struct TransferCommand
{
  Request ios_request;
  u32 data_address = 0;

  TransferCommand(EmulationKernel& ios, const Request& ios_request_, u32 data_address_)
      : ios_request(ios_request_), data_address(data_address_), m_ios(ios)
  {
  }
  virtual ~TransferCommand() = default;

  EmulationKernel& GetEmulationKernel() const;
  // Answers the IPC request once the transfer is done. Subclasses may post-process the data
  // first.
  virtual void OnTransferComplete(s32 return_value) const;
  // Answers the IPC request after a delay given in microseconds.
  void ScheduleTransferCompletion(s32 return_value, u32 expected_time_us) const;
  // Copies `size` bytes of the transfer data out of emulated memory.
  std::unique_ptr<u8[]> MakeBuffer(size_t size) const;
  // Copies `size` bytes into the transfer data in emulated memory.
  void FillBuffer(const u8* src, size_t size) const;

protected:
  EmulationKernel& m_ios;
};

struct CtrlMessage : TransferCommand
{
  u8 request_type = 0;
  u8 request = 0;
  u16 value = 0;
  u16 index = 0;
  u16 length = 0;
  using TransferCommand::TransferCommand;
};

struct BulkMessage : TransferCommand
{
  u32 length = 0;
  u8 endpoint = 0;
  using TransferCommand::TransferCommand;
};

struct IntrMessage : TransferCommand
{
  u32 length = 0;
  u8 endpoint = 0;
  using TransferCommand::TransferCommand;
};

struct IsoMessage : TransferCommand
{
  // Guest array of u16 packet lengths; IOS writes the actual lengths back into it.
  u32 packet_sizes_addr = 0;
  std::vector<u16> packet_sizes;
  u32 length = 0;
  u8 num_packets = 0;
  u8 endpoint = 0;
  using TransferCommand::TransferCommand;
  void SetPacketReturnValue(size_t packet_num, u16 return_value) const;
  // Spreads data_size bytes over the packets in order (each packet takes at most its requested
  // length), writes every packet's length back and returns what did not fit.
  u16 SetPacketsReturnValueFromSize(u16 data_size) const;
};

// A USB device as seen by the emulated host controllers, either passed through from the host
// (LibusbDevice) or emulated.
class Device
{
public:
  virtual ~Device();
  u64 GetId() const;
  u16 GetVid() const;
  u16 GetPid() const;
  // True if the device class or the class of any interface of the first configuration matches.
  bool HasClass(u8 device_class) const;

  virtual DeviceDescriptor GetDeviceDescriptor() const = 0;
  virtual std::vector<ConfigDescriptor> GetConfigurations() const = 0;
  virtual std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const = 0;
  virtual std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const = 0;

  virtual std::string GetErrorName(int error_code) const;
  // Gets the device ready for transfers.
  virtual bool Attach() = 0;
  // Like Attach, and also makes `iface` the active interface. Switching interfaces can reset
  // the alternate setting, so Attach is the better choice when the interface does not matter
  // (control transfers, for example).
  virtual bool AttachAndChangeInterface(u8 iface) = 0;
  virtual int CancelTransfer(u8 endpoint) = 0;
  virtual int ChangeInterface(u8 iface) = 0;
  virtual int GetNumberOfAltSettings(u8 iface) = 0;
  virtual int SetAltSetting(u8 alt_setting) = 0;
  virtual int SubmitTransfer(std::unique_ptr<CtrlMessage> message) = 0;
  virtual int SubmitTransfer(std::unique_ptr<BulkMessage> message) = 0;
  virtual int SubmitTransfer(std::unique_ptr<IntrMessage> message) = 0;
  virtual int SubmitTransfer(std::unique_ptr<IsoMessage> message) = 0;

protected:
  u64 m_id = 0xFFFFFFFFFFFFFFFF;
};
}  // namespace IOS::HLE::USB
