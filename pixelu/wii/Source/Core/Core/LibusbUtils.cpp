// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/LibusbUtils.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(__LIBUSB__)
#include <libusb.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

namespace LibusbUtils
{
#if defined(__LIBUSB__)
class Context::Impl
{
public:
  Impl()
  {
    if (const int result = libusb_init(&m_context); result != LIBUSB_SUCCESS)
    {
      ERROR_LOG_FMT(IOS_USB, "Failed to init libusb: {}", ErrorWrap(result));
      return;
    }

#ifdef _WIN32
    // UsbDk lets devices be used without replacing their driver; it's optional.
    const int usbdk = libusb_set_option(m_context, LIBUSB_OPTION_USE_USBDK);
    if (usbdk != LIBUSB_SUCCESS && usbdk != LIBUSB_ERROR_NOT_FOUND)
      WARN_LOG_FMT(IOS_USB, "Failed to set LIBUSB_OPTION_USE_USBDK: {}", ErrorWrap(usbdk));
#endif

    m_handling_events = true;
    m_event_thread = std::thread([this] { HandleEvents(); });
  }

  ~Impl()
  {
    if (m_context == nullptr || !m_handling_events.exchange(false))
      return;
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000105
    // Wakes the event thread so it sees the stop request now instead of after its timeout.
    libusb_interrupt_event_handler(m_context);
#endif
    m_event_thread.join();
    libusb_exit(m_context);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  libusb_context* GetContext() const { return m_context; }

  int GetDeviceList(const GetDeviceListCallback& callback) const
  {
    // libusb's device list isn't safe to build from several threads at once.
    std::lock_guard lock{m_list_mutex};

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(m_context, &devices);
    if (count < 0)
      return static_cast<int>(count);

    for (ssize_t i = 0; i < count && callback(devices[i]); ++i)
    {
    }
    libusb_free_device_list(devices, 1);
    return LIBUSB_SUCCESS;
  }

private:
  void HandleEvents()
  {
    Common::SetCurrentThreadName("libusb thread");
    timeval timeout{5, 0};
    while (m_handling_events)
    {
      const int result = libusb_handle_events_timeout_completed(m_context, &timeout, nullptr);
      if (result != LIBUSB_SUCCESS)
        WARN_LOG_FMT(IOS_USB, "libusb_handle_events_timeout_completed failed: {}",
                     ErrorWrap(result));
    }
  }

  libusb_context* m_context = nullptr;
  mutable std::mutex m_list_mutex;
  std::atomic<bool> m_handling_events{false};
  std::thread m_event_thread;
};
#else
class Context::Impl
{
public:
  libusb_context* GetContext() const { return nullptr; }
  int GetDeviceList(const GetDeviceListCallback&) const { return -1; }
};
#endif

Context::Context() : m_impl{std::make_unique<Impl>()}
{
}

Context::~Context() = default;

// A context is usable only when libusb initialised (and is present at all).
bool Context::IsValid() const
{
  return m_impl->GetContext() != nullptr;
}

int Context::GetDeviceList(const GetDeviceListCallback& visit) const
{
  return m_impl->GetDeviceList(visit);
}

Context::operator libusb_context*() const
{
  return m_impl->GetContext();
}

std::pair<int, ConfigDescriptor> MakeConfigDescriptor([[maybe_unused]] libusb_device* device,
                                                      [[maybe_unused]] u8 config_num)
{
  // A failed lookup still hands out a (null) descriptor with a do-nothing deleter.
  const auto none = [] { return ConfigDescriptor{nullptr, [](libusb_config_descriptor*) {}}; };
#if defined(__LIBUSB__)
  libusb_config_descriptor* config = nullptr;
  const int status = libusb_get_config_descriptor(device, config_num, &config);
  if (status != LIBUSB_SUCCESS)
    return {status, none()};
  return {status, ConfigDescriptor{config, libusb_free_config_descriptor}};
#else
  return {-1, none()};
#endif
}

#if !defined(__LIBUSB__)
namespace
{
constexpr char kNoLibusb[] = "__LIBUSB__ not defined";
}
#endif

const char* ErrorWrap::GetName() const
{
#if defined(__LIBUSB__)
  return libusb_error_name(m_error);
#else
  return kNoLibusb;
#endif
}

const char* ErrorWrap::GetStrError() const
{
#if defined(__LIBUSB__)
  return libusb_strerror(static_cast<libusb_error>(m_error));
#else
  return kNoLibusb;
#endif
}

std::optional<std::string> GetStringDescriptor(libusb_device_handle* dev_handle, uint8_t desc_index)
{
#if defined(__LIBUSB__)
  // USB string descriptor: bLength, bDescriptorType, then UTF-16LE text (at most 255 bytes).
  struct
  {
    u8 length;
    u8 type;
    char16_t text[127];
  } descriptor{};
  auto* const raw = reinterpret_cast<unsigned char*>(&descriptor);

  // String index 0 (language 0) holds the supported language IDs; the first one is used.
  const int languages = libusb_get_string_descriptor(dev_handle, desc_index, 0, raw, 4);
  if (languages != 4 || descriptor.length < 4 || descriptor.type != LIBUSB_DT_STRING)
  {
    ERROR_LOG_FMT(IOS_USB, "libusb_get_string_descriptor(desc_index={}, lang_id=0) result:{}",
                  static_cast<int>(desc_index), languages);
    return std::nullopt;
  }
  const u16 language = descriptor.text[0];

  const int read = libusb_get_string_descriptor(dev_handle, desc_index, language, raw, 255);
  if (read < 2 || descriptor.length > read || descriptor.type != LIBUSB_DT_STRING)
  {
    ERROR_LOG_FMT(IOS_USB, "libusb_get_string_descriptor(desc_index={}, lang_id={}) result:{}",
                  static_cast<int>(desc_index), language, read);
    return std::nullopt;
  }

  // Only reported: the declared length should be even and match what was read.
  if (descriptor.length % 2 != 0 || descriptor.length != read)
  {
    WARN_LOG_FMT(IOS_USB, "GetStringDescriptor: buffer.length:{} result:{}", descriptor.length,
                 read);
  }

  const auto characters = static_cast<std::size_t>(std::max(0, descriptor.length - 2) / 2);
  return UTF16ToUTF8(std::u16string_view{descriptor.text, characters});
#else
  return "__LIBUSB__ not defined";
#endif
}

}  // namespace LibusbUtils
