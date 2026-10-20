// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Logging/LogManager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/ConsoleListener.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace Common::Log
{
// Settings in Logger.ini.
const Config::Info<bool> LOGGER_WRITE_TO_FILE{{Config::System::Logger, "Options", "WriteToFile"},
                                              false};
const Config::Info<bool> LOGGER_WRITE_TO_CONSOLE{
    {Config::System::Logger, "Options", "WriteToConsole"}, true};
const Config::Info<bool> LOGGER_WRITE_TO_WINDOW{
    {Config::System::Logger, "Options", "WriteToWindow"}, true};
const Config::Info<LogLevel> LOGGER_VERBOSITY{{Config::System::Logger, "Options", "Verbosity"},
                                              LogLevel::LNOTICE};

namespace
{
// Whether a log type is enabled is stored as [Logs] <short name> = True/False.
Config::Info<bool> LogTypeSetting(const char* short_name)
{
  return Config::Info<bool>{{Config::System::Logger, "Logs", short_name}, false};
}

// Appends every line to the log file, flushing each one so nothing is lost on a crash.
class FileLogListener final : public LogListener
{
public:
  explicit FileLogListener(const std::string& path)
  {
    File::OpenFStream(m_stream, path, std::ios::app);
  }

  void Log(LogLevel, const char* msg) override
  {
    std::lock_guard lock(m_lock);
    if (!m_stream.good())
      return;
    m_stream << msg;
    m_stream.flush();
  }

private:
  std::mutex m_lock;
  std::ofstream m_stream;
};

// Number of characters of __FILE__ before the path relative to Source/Core, so that log lines
// show "Core/HW/..." instead of the full build path. 0 if the layout is not recognised.
size_t SourceRootLength()
{
  std::string path = __FILE__;
  Common::ToLower(&path);
  std::replace(path.begin(), path.end(), '\\', '/');
  constexpr std::string_view marker = "/source/core/";
  const size_t at = path.find(marker);
  return at == std::string::npos ? 0 : at + marker.size();
}

std::unique_ptr<LogManager> s_instance;
}  // namespace

void GenericLogFmtImpl(LogLevel level, LogType type, const char* file, int line,
                       fmt::string_view format, const fmt::format_args& args)
{
  LogManager* const manager = LogManager::GetInstance();
  if (manager == nullptr || !manager->IsEnabled(type, level))
    return;

  const std::string message = fmt::vformat(format, args);
  manager->Log(level, type, file, line, message.c_str());
}

LogManager::LogManager()
{
  // Short names are the keys in Logger.ini (and appear in every log line); full names are shown
  // in the log configuration window.
  const auto name = [this](LogType type, const char* short_name, const char* full_name) {
    m_types[type] = LogContainer{short_name, full_name};
  };
  name(LogType::ACHIEVEMENTS, "RetroAchievements", "Achievements");
  name(LogType::ACTIONREPLAY, "ActionReplay", "Action Replay");
  name(LogType::AUDIO, "Audio", "Audio Emulator");
  name(LogType::AUDIO_INTERFACE, "AI", "Audio Interface");
  name(LogType::BOOT, "BOOT", "Boot");
  name(LogType::COMMANDPROCESSOR, "CP", "Command Processor");
  name(LogType::COMMON, "COMMON", "Common");
  name(LogType::CONSOLE, "CONSOLE", "Dolphin Console");
  name(LogType::CONTROLLERINTERFACE, "CI", "Controller Interface");
  name(LogType::CORE, "CORE", "Core");
  name(LogType::DISCIO, "DIO", "Disc IO");
  name(LogType::DSPHLE, "DSPHLE", "DSP HLE");
  name(LogType::DSPLLE, "DSPLLE", "DSP LLE");
  name(LogType::DSP_MAIL, "DSPMails", "DSP Mails");
  name(LogType::DSPINTERFACE, "DSP", "DSP Interface");
  name(LogType::DVDINTERFACE, "DVD", "DVD Interface");
  name(LogType::AMMEDIABOARD, "AMMB", "AMMB Interface");
  name(LogType::AMMEDIABOARD_NET, "AMMB_NET", "AMMB - Network");
  name(LogType::DYNA_REC, "JIT", "JIT Dynamic Recompiler");
  name(LogType::EXPANSIONINTERFACE, "EXI", "Expansion Interface");
  name(LogType::FILEMON, "FileMon", "File Monitor");
  name(LogType::FRAMEDUMP, "FRAMEDUMP", "FrameDump");
  name(LogType::GDB_STUB, "GDB_STUB", "GDB Stub");
  name(LogType::GPFIFO, "GP", "GatherPipe FIFO");
  name(LogType::HOST_GPU, "Host GPU", "Host GPU");
  name(LogType::HSP, "HSP", "High-Speed Port");
  name(LogType::IOS, "IOS", "IOS");
  name(LogType::IOS_DI, "IOS_DI", "IOS - Drive Interface");
  name(LogType::IOS_ES, "IOS_ES", "IOS - ETicket Services");
  name(LogType::IOS_FS, "IOS_FS", "IOS - Filesystem Services");
  name(LogType::IOS_NET, "IOS_NET", "IOS - Network");
  name(LogType::IOS_SD, "IOS_SD", "IOS - SDIO");
  name(LogType::IOS_SSL, "IOS_SSL", "IOS - SSL");
  name(LogType::IOS_STM, "IOS_STM", "IOS - State Transition Manager");
  name(LogType::IOS_USB, "IOS_USB", "IOS - USB");
  name(LogType::IOS_WC24, "IOS_WC24", "IOS - WiiConnect24");
  name(LogType::IOS_WFS, "IOS_WFS", "IOS - WFS");
  name(LogType::IOS_WIIMOTE, "IOS_WIIMOTE", "IOS - Wii Remote");
  name(LogType::MASTER_LOG, "MASTER", "Master Log");
  name(LogType::MEMMAP, "MI", "Memory Interface & Memory Map");
  name(LogType::MEMCARD_MANAGER, "MemCard Manager", "Memory Card Manager");
  name(LogType::NETPLAY, "NETPLAY", "Netplay");
  name(LogType::OSHLE, "HLE", "OSHLE");
  name(LogType::OSREPORT, "OSREPORT", "OSReport EXI");
  name(LogType::OSREPORT_HLE, "OSREPORT_HLE", "OSReport HLE");
  name(LogType::PIXELENGINE, "PE", "Pixel Engine");
  name(LogType::PROCESSORINTERFACE, "PI", "Processor Interface");
  name(LogType::POWERPC, "PowerPC", "PowerPC IBM CPU");
  name(LogType::SERIALINTERFACE, "SI", "Serial Interface");
  name(LogType::SERIALINTERFACE_AMBB, "SI_AMBB", "AMBB Interface");
  name(LogType::SERIALINTERFACE_CARD, "SI_CARD", "CARD Interface");
  name(LogType::SERIALINTERFACE_JVSIO, "SI_JVS", "JVS-I/O");
  name(LogType::SP1, "SP1", "Serial Port 1");
  name(LogType::SYMBOLS, "SYMBOLS", "Symbols");
  name(LogType::VIDEO, "Video", "Video Backend");
  name(LogType::VIDEOINTERFACE, "VI", "Video Interface");
  name(LogType::WII_IPC, "WII_IPC", "WII IPC");
  name(LogType::WIIMOTE, "Wiimote", "Wii Remote");

  RegisterListener(LogListener::FILE_LISTENER,
                   std::make_unique<FileLogListener>(File::GetUserPath(F_MAINLOG_IDX)));
  RegisterListener(LogListener::CONSOLE_LISTENER, std::make_unique<ConsoleListener>());

  SetEffectiveLogLevel();
  EnableListener(LogListener::FILE_LISTENER, Config::Get(LOGGER_WRITE_TO_FILE));
  EnableListener(LogListener::CONSOLE_LISTENER, Config::Get(LOGGER_WRITE_TO_CONSOLE));
  EnableListener(LogListener::LOG_WINDOW_LISTENER, Config::Get(LOGGER_WRITE_TO_WINDOW));

  for (LogContainer& entry : m_types)
    entry.m_enable = Config::Get(LogTypeSetting(entry.m_short_name));

  m_source_root_length = SourceRootLength();

  m_config_callback = Config::AddConfigChangedCallback([this] { SetEffectiveLogLevel(); });
}

LogManager::~LogManager()
{
  Config::RemoveConfigChangedCallback(m_config_callback);
}

void LogManager::SaveSettings()
{
  // Write everything first, then notify once.
  Config::ConfigChangeCallbackGuard batch;

  Config::SetBaseOrCurrent(LOGGER_WRITE_TO_FILE, IsListenerEnabled(LogListener::FILE_LISTENER));
  Config::SetBaseOrCurrent(LOGGER_WRITE_TO_CONSOLE,
                           IsListenerEnabled(LogListener::CONSOLE_LISTENER));
  Config::SetBaseOrCurrent(LOGGER_WRITE_TO_WINDOW,
                           IsListenerEnabled(LogListener::LOG_WINDOW_LISTENER));

  for (const LogContainer& entry : m_types)
    Config::SetBaseOrCurrent(LogTypeSetting(entry.m_short_name), entry.m_enable);

  Config::Save();
}

void LogManager::Log(LogLevel level, LogType type, const char* file, int line, const char* message)
{
  if (!m_active_listeners || !IsEnabled(type, level))
    return;

  const char* relative = file;
  if (std::strlen(file) > m_source_root_length)
    relative += m_source_root_length;
  LogWithFullPath(level, type, relative, line, message);
}

std::string LogManager::GetTimestamp()
{
  // "MM:SS:mmm". The Qt log window relies on this exact width.
  const auto now = std::chrono::system_clock::now();
  const auto whole_seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - whole_seconds);
  return fmt::format("{:%M:%S}:{:03}", whole_seconds, millis.count());
}

void LogManager::LogWithFullPath(LogLevel level, LogType type, const char* file, int line,
                                 const char* message)
{
  const std::string text =
      fmt::format("{} {}:{} {}[{}]: {}\n", GetTimestamp(), file, line,
                  LOG_LEVEL_TO_CHAR[static_cast<int>(level)], GetShortName(type), message);

  for (const int id : m_active_listeners)
  {
    if (const auto& listener = m_listeners[id])
      listener->Log(level, text.c_str());
  }
}

LogLevel LogManager::GetEffectiveLogLevel() const
{
  return m_effective_level.load(std::memory_order_relaxed);
}

void LogManager::SetConfigLogLevel(LogLevel level)
{
  Config::SetBaseOrCurrent(LOGGER_VERBOSITY, level);
  SetEffectiveLogLevel();
}

void LogManager::SetEffectiveLogLevel()
{
  const LogLevel wanted = Config::Get(LOGGER_VERBOSITY);
  const LogLevel effective = std::clamp(wanted, LogLevel::LNOTICE, MAX_EFFECTIVE_LOGLEVEL);
  m_effective_level.store(effective, std::memory_order_relaxed);
}

void LogManager::SetEnable(LogType type, bool enable)
{
  m_types[type].m_enable = enable;
}

bool LogManager::IsEnabled(LogType type, LogLevel level) const
{
  return m_types[type].m_enable && level <= GetEffectiveLogLevel();
}

std::vector<LogManager::LogContainer> LogManager::GetLogTypes()
{
  return std::vector<LogContainer>(m_types.begin(), m_types.end());
}

const char* LogManager::GetShortName(LogType type) const
{
  return m_types[type].m_short_name;
}

const char* LogManager::GetFullName(LogType type) const
{
  return m_types[type].m_full_name;
}

void LogManager::RegisterListener(LogListener::LISTENER id, std::unique_ptr<LogListener> listener)
{
  m_listeners[id] = std::move(listener);
}

void LogManager::EnableListener(LogListener::LISTENER id, bool enable)
{
  m_active_listeners[id] = enable;
}

bool LogManager::IsListenerEnabled(LogListener::LISTENER id) const
{
  return m_active_listeners[id];
}

LogManager* LogManager::GetInstance()
{
  return s_instance.get();
}

void LogManager::Init()
{
  s_instance.reset(new LogManager());
}

void LogManager::Shutdown()
{
  if (s_instance != nullptr)
    s_instance->SaveSettings();
  s_instance.reset();
}
}  // namespace Common::Log
