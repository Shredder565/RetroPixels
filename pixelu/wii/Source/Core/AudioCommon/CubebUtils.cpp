// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/CubebUtils.h"

#include <cstdarg>
#include <cstring>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"

#include <cubeb/cubeb.h>

#ifdef _WIN32
#include <objbase.h>
#endif

namespace
{
const char* OrNullText(const char* text)
{
  return text != nullptr ? text : "(null)";
}

const char* StateName(cubeb_device_state state)
{
  switch (state)
  {
  case CUBEB_DEVICE_STATE_DISABLED:
    return "disabled";
  case CUBEB_DEVICE_STATE_UNPLUGGED:
    return "unplugged";
  case CUBEB_DEVICE_STATE_ENABLED:
    return "enabled";
  default:
    return "unknown?";
  }
}

// cubeb's log lines are printf-style and start with "%s:%d:" for the source file and line.
// They are forwarded to our log as INFO messages of the AUDIO type with that file and line.
void ForwardCubebLog(const char* format, ...)
{
  auto* const log = Common::Log::LogManager::GetInstance();
  if (log == nullptr || !log->IsEnabled(Common::Log::LogType::AUDIO, Common::Log::LogLevel::LINFO))
    return;

  va_list args;
  va_start(args, format);

  const char* file = va_arg(args, const char*);
  const std::string_view file_view(file);
  const std::size_t separator = file_view.find_last_of("/\\");
  if (separator != std::string_view::npos)
    file += separator + 1;
  const int line = va_arg(args, int);

  constexpr std::string_view location_prefix = "%s:%d:";
  const std::string rest_format(StripWhitespace(format + location_prefix.size()));
  const std::string message = StringFromFormatV(rest_format.c_str(), args);
  va_end(args);

  log->LogWithFullPath(Common::Log::LogLevel::LINFO, Common::Log::LogType::AUDIO, file, line,
                       message.c_str());
}

void ReleaseContext(cubeb* context)
{
  cubeb_destroy(context);
  if (cubeb_set_log_callback(CUBEB_LOG_DISABLED, nullptr) != CUBEB_OK)
    ERROR_LOG_FMT(AUDIO, "Error removing cubeb log callback");
}

// Looks up a device by its ID string. `needs_context` makes a missing context fail quietly
// (output lookups); input lookups pass a missing context straight to cubeb.
cubeb_devid FindDevice(std::string_view id, cubeb_device_type type, bool needs_context,
                       const char* enumerate_error, const char* not_found_warning)
{
  if (id.empty())
    return nullptr;

  const std::shared_ptr<cubeb> context = CubebUtils::GetContext();
  if (needs_context && !context)
    return nullptr;

  cubeb_device_collection devices;
  if (cubeb_enumerate_devices(context.get(), type, &devices) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "{}", enumerate_error);
    return nullptr;
  }

  cubeb_devid found = nullptr;
  for (std::size_t i = 0; i < devices.count; ++i)
  {
    const cubeb_device_info& device = devices.device[i];
    if (device.device_id != nullptr && id == device.device_id)
    {
      found = device.devid;
      break;
    }
  }

  if (found == nullptr)
    WARN_LOG_FMT(AUDIO, "{}", not_found_warning);

  cubeb_device_collection_destroy(context.get(), &devices);
  return found;
}
}  // namespace

namespace CubebUtils
{
std::shared_ptr<cubeb> GetContext()
{
  static std::weak_ptr<cubeb> s_context;

  if (std::shared_ptr<cubeb> existing = s_context.lock())
    return existing;

  if (cubeb_set_log_callback(CUBEB_LOG_NORMAL, ForwardCubebLog) != CUBEB_OK)
    ERROR_LOG_FMT(AUDIO, "Error setting cubeb log callback");

  cubeb* raw = nullptr;
  if (cubeb_init(&raw, "Dolphin Emulator", nullptr) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Error initializing cubeb library");
    return nullptr;
  }
  INFO_LOG_FMT(AUDIO, "Cubeb initialized using {} backend", cubeb_get_backend_id(raw));

  std::shared_ptr<cubeb> context(raw, ReleaseContext);
  s_context = context;
  return context;
}

std::vector<std::pair<std::string, std::string>> ListInputDevices()
{
  std::vector<std::pair<std::string, std::string>> result;

  const std::shared_ptr<cubeb> context = GetContext();
  cubeb_device_collection devices;
  if (cubeb_enumerate_devices(context.get(), CUBEB_DEVICE_TYPE_INPUT, &devices) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Error listing cubeb input devices");
    return result;
  }

  INFO_LOG_FMT(AUDIO, "Listing cubeb input devices:");
  for (std::size_t i = 0; i < devices.count; ++i)
  {
    // cubeb documents the vendor name as optional; other strings can be null in practice too.
    const cubeb_device_info& device = devices.device[i];
    INFO_LOG_FMT(AUDIO,
                 "[{}] Device ID: {}\n"
                 "\tName: {}\n"
                 "\tGroup ID: {}\n"
                 "\tVendor: {}\n"
                 "\tState: {}",
                 i, OrNullText(device.device_id), OrNullText(device.friendly_name),
                 OrNullText(device.group_id), OrNullText(device.vendor_name),
                 StateName(device.state));

    if (device.device_id != nullptr && device.state == CUBEB_DEVICE_STATE_ENABLED)
      result.emplace_back(device.device_id, OrNullText(device.friendly_name));
  }

  cubeb_device_collection_destroy(context.get(), &devices);
  return result;
}

std::vector<std::pair<std::string, std::string>> ListOutputDevices()
{
  std::vector<std::pair<std::string, std::string>> result;

  const std::shared_ptr<cubeb> context = GetContext();
  if (!context)
    return result;

  cubeb_device_collection devices;
  if (cubeb_enumerate_devices(context.get(), CUBEB_DEVICE_TYPE_OUTPUT, &devices) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Error listing cubeb output devices");
    return result;
  }

  for (std::size_t i = 0; i < devices.count; ++i)
  {
    const cubeb_device_info& device = devices.device[i];
    if (device.device_id == nullptr || device.state != CUBEB_DEVICE_STATE_ENABLED)
      continue;
    // Fall back to the ID when the device has no friendly name.
    const char* name = device.friendly_name != nullptr ? device.friendly_name : device.device_id;
    result.emplace_back(device.device_id, name);
  }

  cubeb_device_collection_destroy(context.get(), &devices);
  return result;
}

const void* GetInputDeviceById(std::string_view id)
{
  return FindDevice(id, CUBEB_DEVICE_TYPE_INPUT, false, "Error enumerating cubeb input devices",
                    "Failed to find selected input device, defaulting to system preferences");
}

const void* GetOutputDeviceById(std::string_view id)
{
  return FindDevice(id, CUBEB_DEVICE_TYPE_OUTPUT, true, "Error enumerating cubeb output devices",
                    "Failed to find selected output device, defaulting to system preferences");
}

CoInitSyncWorker::CoInitSyncWorker([[maybe_unused]] std::string worker_name)
{
#ifdef _WIN32
  m_thread.Reset(std::move(worker_name));
  m_thread.PushBlocking([this] {
    const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    m_com_ready = result == S_OK;
    m_com_needs_uninit = m_com_ready || result == S_FALSE;
  });
#endif
}

CoInitSyncWorker::~CoInitSyncWorker()
{
#ifdef _WIN32
  if (m_com_needs_uninit)
  {
    m_thread.PushBlocking([this] {
      m_com_needs_uninit = false;
      CoUninitialize();
    });
  }
  m_com_ready = false;
#endif
}

bool CoInitSyncWorker::Execute(FunctionType f)
{
#ifdef _WIN32
  if (!m_com_ready)
    return false;
  m_thread.PushBlocking(std::move(f));
#else
  f();
#endif
  return true;
}
}  // namespace CubebUtils
