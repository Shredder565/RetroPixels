// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

#include "Common/BitSet.h"
#include "Common/Config/Config.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"

namespace Common::Log
{
// The verbosity the user chose. Only read it to recompute the effective level, and only change
// it when the user picks a new verbosity; everything else uses
// LogManager::GetEffectiveLogLevel(), which is clamped to what this build compiles in.
extern const Config::Info<LogLevel> LOGGER_VERBOSITY;

// Receives finished log lines.
class LogListener
{
public:
  virtual ~LogListener() = default;
  virtual void Log(LogLevel level, const char* msg) = 0;

  enum LISTENER
  {
    FILE_LISTENER = 0,
    CONSOLE_LISTENER,
    LOG_WINDOW_LISTENER,

    NUMBER_OF_LISTENERS  // keep last
  };
};

class LogManager final
{
public:
  struct LogContainer
  {
    const char* m_short_name;
    const char* m_full_name;
    bool m_enable = false;
  };

  ~LogManager();

  static LogManager* GetInstance();
  static void Init();
  static void Shutdown();

  // `file` is a full source path; the part up to the source tree root is dropped.
  void Log(LogLevel level, LogType type, const char* file, int line, const char* message);
  // Like Log, but prints `file` as given and does not check whether the log type is enabled.
  void LogWithFullPath(LogLevel level, LogType type, const char* file, int line,
                       const char* message);

  LogLevel GetEffectiveLogLevel() const;
  void SetConfigLogLevel(LogLevel level);

  void SetEnable(LogType type, bool enable);
  bool IsEnabled(LogType type, LogLevel level = LogLevel::LNOTICE) const;

  std::vector<LogContainer> GetLogTypes();

  const char* GetShortName(LogType type) const;
  const char* GetFullName(LogType type) const;

  void RegisterListener(LogListener::LISTENER id, std::unique_ptr<LogListener> listener);
  void EnableListener(LogListener::LISTENER id, bool enable);
  bool IsListenerEnabled(LogListener::LISTENER id) const;

  void SaveSettings();

private:
  LogManager();

  LogManager(const LogManager&) = delete;
  LogManager& operator=(const LogManager&) = delete;
  LogManager(LogManager&&) = delete;
  LogManager& operator=(LogManager&&) = delete;

  static std::string GetTimestamp();
  void SetEffectiveLogLevel();

  std::atomic<LogLevel> m_effective_level{LogLevel::LNOTICE};
  Config::ConfigChangedCallbackID m_config_callback;
  EnumMap<LogContainer, LAST_LOG_TYPE> m_types{};
  std::array<std::unique_ptr<LogListener>, LogListener::NUMBER_OF_LISTENERS> m_listeners{};
  BitSet32 m_active_listeners;
  size_t m_source_root_length = 0;
};
}  // namespace Common::Log
