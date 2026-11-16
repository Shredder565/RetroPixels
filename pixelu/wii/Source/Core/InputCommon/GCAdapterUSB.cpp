// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/GCAdapterUSB.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonFuncs.h"
#include "Common/StringUtil.h"

#if defined(_WIN32)
#include <windows.h>

#include <cwctype>
#include <setupapi.h>
#include <winusb.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace GCAdapterUSB
{
namespace
{
// Error text shared by both platforms, guarded so the read and write threads can both set it.
class ErrorText
{
public:
  void Set(std::string text)
  {
    std::lock_guard lock(m_mutex);
    m_text = std::move(text);
  }
  std::string Get() const
  {
    std::lock_guard lock(m_mutex);
    return m_text;
  }

private:
  mutable std::mutex m_mutex;
  std::string m_text;
};

#if defined(_WIN32)
// GUID_DEVINTERFACE_USB_DEVICE: every USB device registers an interface of this class, and for a
// device whose function driver is WinUSB that interface is the one WinUSB is opened through.
constexpr GUID USB_DEVICE_INTERFACE = {
    0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

class WinUSBDevice final : public Device
{
public:
  WinUSBDevice(HANDLE file, WINUSB_INTERFACE_HANDLE usb, u8 in_pipe, u8 out_pipe)
      : m_file(file), m_usb(usb), m_in(in_pipe), m_out(out_pipe)
  {
  }

  ~WinUSBDevice() override
  {
    WinUsb_Free(m_usb);
    CloseHandle(m_file);
  }

  bool ControlOut(u8 request_type, u8 request, u16 value, u16 index,
                  unsigned timeout_ms) override
  {
    SetTimeout(0, timeout_ms);
    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = request_type;
    setup.Request = request;
    setup.Value = value;
    setup.Index = index;
    setup.Length = 0;
    ULONG moved = 0;
    if (WinUsb_ControlTransfer(m_usb, setup, nullptr, 0, &moved, nullptr))
      return true;
    Fail("control transfer");
    return false;
  }

  int ReadInterrupt(u8* data, int size, unsigned timeout_ms) override
  {
    SetTimeout(m_in, timeout_ms);
    ULONG moved = 0;
    if (WinUsb_ReadPipe(m_usb, m_in, data, static_cast<ULONG>(size), &moved, nullptr))
      return static_cast<int>(moved);
    Fail("read");
    return -1;
  }

  int WriteInterrupt(const u8* data, int size, unsigned timeout_ms) override
  {
    SetTimeout(m_out, timeout_ms);
    ULONG moved = 0;
    if (WinUsb_WritePipe(m_usb, m_out, const_cast<u8*>(data), static_cast<ULONG>(size), &moved,
                         nullptr))
    {
      return static_cast<int>(moved);
    }
    Fail("write");
    return -1;
  }

  std::string LastError() const override { return m_error.Get(); }
  bool IsGone() const override { return m_gone; }

private:
  void SetTimeout(u8 pipe, unsigned timeout_ms)
  {
    std::atomic<unsigned>& current = pipe == 0 ? m_control_timeout :
                                     pipe == m_in ? m_in_timeout :
                                                    m_out_timeout;
    if (current.exchange(timeout_ms) == timeout_ms)
      return;
    ULONG value = timeout_ms;
    WinUsb_SetPipePolicy(m_usb, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(value), &value);
  }

  void Fail(const char* what)
  {
    const DWORD error = GetLastError();
    if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_BAD_COMMAND ||
        error == ERROR_FILE_NOT_FOUND || error == ERROR_GEN_FAILURE)
    {
      m_gone = true;
    }
    m_error.Set(fmt::format("USB {} failed: {}", what, Common::GetWin32ErrorString(error)));
  }

  HANDLE m_file;
  WINUSB_INTERFACE_HANDLE m_usb;
  u8 m_in;
  u8 m_out;
  std::atomic<unsigned> m_control_timeout{~0u};
  std::atomic<unsigned> m_in_timeout{~0u};
  std::atomic<unsigned> m_out_timeout{~0u};
  std::atomic<bool> m_gone{false};
  ErrorText m_error;
};

// The device paths Windows lists for present USB devices with this vendor and product ID.
std::vector<std::wstring> FindDevicePaths(u16 vendor_id, u16 product_id)
{
  std::vector<std::wstring> paths;
  const HDEVINFO set = SetupDiGetClassDevsW(&USB_DEVICE_INTERFACE, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
    return paths;

  const std::wstring wanted =
      UTF8ToWString(fmt::format("vid_{:04x}&pid_{:04x}", vendor_id, product_id));
  for (DWORD index = 0;; ++index)
  {
    SP_DEVICE_INTERFACE_DATA entry{};
    entry.cbSize = sizeof(entry);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &USB_DEVICE_INTERFACE, index, &entry))
      break;

    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &entry, nullptr, 0, &needed, nullptr);
    if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
      continue;
    std::vector<u8> buffer(needed);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &entry, detail, needed, nullptr, nullptr))
      continue;

    std::wstring path = detail->DevicePath;
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (lower.find(wanted) != std::wstring::npos)
      paths.push_back(std::move(path));
  }
  SetupDiDestroyDeviceInfoList(set);
  return paths;
}

std::unique_ptr<Device> OpenPlatform(u16 vendor_id, u16 product_id, bool* found,
                                     std::string* error)
{
  const std::vector<std::wstring> paths = FindDevicePaths(vendor_id, product_id);
  *found = !paths.empty();
  for (const std::wstring& path : paths)
  {
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
      *error = fmt::format("Couldn't open the adapter: {}", Common::GetLastErrorString());
      continue;
    }

    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb))
    {
      *error = "The adapter isn't using the WinUSB driver. Install WinUSB for it (for example "
               "with Zadig), then plug it in again.";
      CloseHandle(file);
      continue;
    }

    u8 in_pipe = 0, out_pipe = 0;
    USB_INTERFACE_DESCRIPTOR interface_descriptor{};
    if (WinUsb_QueryInterfaceSettings(usb, 0, &interface_descriptor))
    {
      for (UCHAR i = 0; i < interface_descriptor.bNumEndpoints; ++i)
      {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usb, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeInterrupt)
          continue;
        if ((pipe.PipeId & 0x80) != 0)
          in_pipe = pipe.PipeId;
        else
          out_pipe = pipe.PipeId;
      }
    }
    if (in_pipe == 0 || out_pipe == 0)
    {
      *error = "The adapter has no interrupt endpoints.";
      WinUsb_Free(usb);
      CloseHandle(file);
      continue;
    }
    return std::make_unique<WinUSBDevice>(file, usb, in_pipe, out_pipe);
  }
  return nullptr;
}

#elif defined(__linux__)

// Reads a small sysfs attribute as text ("" if missing).
std::string ReadAttribute(const std::string& path)
{
  std::ifstream file(path);
  std::string value;
  std::getline(file, value);
  return value;
}

class UsbfsDevice final : public Device
{
public:
  UsbfsDevice(int fd, u8 in_endpoint, u8 out_endpoint)
      : m_fd(fd), m_in(in_endpoint), m_out(out_endpoint)
  {
  }

  ~UsbfsDevice() override
  {
    unsigned int interface_number = 0;
    ioctl(m_fd, USBDEVFS_RELEASEINTERFACE, &interface_number);
    close(m_fd);
  }

  bool ControlOut(u8 request_type, u8 request, u16 value, u16 index,
                  unsigned timeout_ms) override
  {
    usbdevfs_ctrltransfer transfer{};
    transfer.bRequestType = request_type;
    transfer.bRequest = request;
    transfer.wValue = value;
    transfer.wIndex = index;
    transfer.wLength = 0;
    transfer.timeout = timeout_ms;
    transfer.data = nullptr;
    if (ioctl(m_fd, USBDEVFS_CONTROL, &transfer) >= 0)
      return true;
    Fail("control transfer", errno);
    return false;
  }

  int ReadInterrupt(u8* data, int size, unsigned timeout_ms) override
  {
    return Transfer(m_in, data, size, timeout_ms);
  }

  int WriteInterrupt(const u8* data, int size, unsigned timeout_ms) override
  {
    return Transfer(m_out, const_cast<u8*>(data), size, timeout_ms);
  }

  std::string LastError() const override { return m_error.Get(); }
  bool IsGone() const override { return m_gone; }

private:
  // One submitted request. It lives on the heap: if the device vanishes before the kernel hands
  // it back, it is left allocated rather than freed under the kernel's feet.
  struct Pending
  {
    std::vector<u8> buffer;
    bool done = false;
    // Last: the kernel's struct ends in a (here unused) flexible array.
    usbdevfs_urb urb{};
  };

  // Collects every finished request (any thread's). Called with m_reap_mutex held.
  void ReapFinished()
  {
    for (;;)
    {
      void* finished = nullptr;
      if (ioctl(m_fd, USBDEVFS_REAPURBNDELAY, &finished) < 0)
      {
        if (errno == ENODEV)
          m_gone = true;
        return;
      }
      static_cast<Pending*>(static_cast<usbdevfs_urb*>(finished)->usercontext)->done = true;
    }
  }

  // Interrupt transfers go through the asynchronous request interface, which (unlike the
  // synchronous bulk call) handles interrupt endpoints on every kernel. Whichever thread waits
  // reaps all finished requests and marks them, so the read and write threads can share the fd.
  int Transfer(u8 endpoint, u8* data, int size, unsigned timeout_ms)
  {
    auto* pending = new Pending;
    const bool in = (endpoint & 0x80) != 0;
    pending->buffer.assign(data, data + (in ? 0 : size));
    pending->buffer.resize(static_cast<size_t>(size));
    pending->urb.type = USBDEVFS_URB_TYPE_INTERRUPT;
    pending->urb.endpoint = endpoint;
    pending->urb.buffer = pending->buffer.data();
    pending->urb.buffer_length = size;
    pending->urb.usercontext = pending;

    if (ioctl(m_fd, USBDEVFS_SUBMITURB, &pending->urb) < 0)
    {
      Fail(in ? "read" : "write", errno);
      delete pending;
      return -1;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    bool discarded = false;
    for (;;)
    {
      std::lock_guard lock(m_reap_mutex);
      ReapFinished();
      if (pending->done)
        break;
      if (m_gone && Clock::now() > deadline + std::chrono::seconds(1))
      {
        // The kernel never returned it; leave it allocated.
        m_error.Set("The adapter was unplugged.");
        return -1;
      }
      if (!discarded && Clock::now() >= deadline)
      {
        ioctl(m_fd, USBDEVFS_DISCARDURB, &pending->urb);
        discarded = true;
      }
      pollfd waiting{m_fd, POLLOUT, 0};
      // Short waits, so the other thread gets the lock regularly.
      poll(&waiting, 1, 5);
    }

    const int status = pending->urb.status;
    const int moved = pending->urb.actual_length;
    if (in && status == 0)
      std::copy_n(pending->buffer.begin(), moved, data);
    delete pending;

    if (status != 0)
    {
      if (status == -ENOENT || status == -ECONNRESET)
        m_error.Set(fmt::format("USB {} timed out", in ? "read" : "write"));
      else
        Fail(in ? "read" : "write", -status);
      return -1;
    }
    return moved;
  }

  void Fail(const char* what, int error)
  {
    if (error == ENODEV || error == ESHUTDOWN || error == EPROTO)
      m_gone = true;
    m_error.Set(fmt::format("USB {} failed: {}", what, Common::StrerrorString(error)));
  }

  int m_fd;
  u8 m_in;
  u8 m_out;
  std::mutex m_reap_mutex;
  std::atomic<bool> m_gone{false};
  ErrorText m_error;
};

// Finds interface 0's interrupt endpoints in the descriptors usbfs returns (the device
// descriptor, then the configuration descriptors).
bool FindEndpoints(int fd, u8* in_endpoint, u8* out_endpoint)
{
  u8 raw[4096];
  const ssize_t size = read(fd, raw, sizeof(raw));
  if (size < 18)
    return false;

  bool in_first_interface = false;
  bool seen_config = false;
  for (ssize_t at = raw[0]; at + 2 <= size;)
  {
    const u8 length = raw[at];
    const u8 type = raw[at + 1];
    if (length < 2 || at + length > size)
      break;
    if (type == 2)  // configuration: only the first one counts
    {
      if (seen_config)
        break;
      seen_config = true;
    }
    else if (type == 4 && length >= 4)  // interface
    {
      in_first_interface = raw[at + 2] == 0 && raw[at + 3] == 0;
    }
    else if (type == 5 && length >= 4 && in_first_interface)  // endpoint
    {
      const u8 address = raw[at + 2];
      if ((raw[at + 3] & 3) == 3)  // interrupt
      {
        if ((address & 0x80) != 0)
          *in_endpoint = address;
        else
          *out_endpoint = address;
      }
    }
    at += length;
  }
  return *in_endpoint != 0 && *out_endpoint != 0;
}

std::unique_ptr<Device> OpenPlatform(u16 vendor_id, u16 product_id, bool* found,
                                     std::string* error)
{
  *found = false;
  const std::string root = "/sys/bus/usb/devices/";
  DIR* dir = opendir(root.c_str());
  if (dir == nullptr)
    return nullptr;

  const std::string vendor = fmt::format("{:04x}", vendor_id);
  const std::string product = fmt::format("{:04x}", product_id);
  std::unique_ptr<Device> device;
  while (const dirent* entry = readdir(dir))
  {
    const std::string base = root + entry->d_name + "/";
    if (ReadAttribute(base + "idVendor") != vendor || ReadAttribute(base + "idProduct") != product)
      continue;
    *found = true;

    const std::string bus = ReadAttribute(base + "busnum");
    const std::string number = ReadAttribute(base + "devnum");
    if (bus.empty() || number.empty())
      continue;
    const std::string node =
        fmt::format("/dev/bus/usb/{:03d}/{:03d}", std::stoi(bus), std::stoi(number));

    const int fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
      *error = errno == EACCES ?
                   fmt::format("No permission to open {} (a udev rule giving your user access to "
                               "the adapter is needed).",
                               node) :
                   fmt::format("Couldn't open {}: {}", node, Common::LastStrerrorString());
      continue;
    }

    u8 in_endpoint = 0, out_endpoint = 0;
    if (!FindEndpoints(fd, &in_endpoint, &out_endpoint))
    {
      *error = "The adapter has no interrupt endpoints.";
      close(fd);
      continue;
    }

    // Take the interface from a kernel driver bound to it ("no driver" is fine).
    usbdevfs_ioctl detach{};
    detach.ifno = 0;
    detach.ioctl_code = USBDEVFS_DISCONNECT;
    detach.data = nullptr;
    if (ioctl(fd, USBDEVFS_IOCTL, &detach) < 0 && errno != ENODATA)
    {
      *error = fmt::format("Couldn't detach the kernel driver: {}", Common::LastStrerrorString());
      close(fd);
      continue;
    }

    unsigned int interface_number = 0;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface_number) < 0)
    {
      *error = fmt::format("Couldn't claim the adapter: {}", Common::LastStrerrorString());
      close(fd);
      continue;
    }

    device = std::make_unique<UsbfsDevice>(fd, in_endpoint, out_endpoint);
    break;
  }
  closedir(dir);
  return device;
}

#else

std::unique_ptr<Device> OpenPlatform(u16, u16, bool* found, std::string*)
{
  *found = false;
  return nullptr;
}

#endif
}  // namespace

std::unique_ptr<Device> Device::Open(u16 vendor_id, u16 product_id, bool* found,
                                     std::string* error)
{
  error->clear();
  return OpenPlatform(vendor_id, product_id, found, error);
}
}  // namespace GCAdapterUSB
