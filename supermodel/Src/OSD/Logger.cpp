// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The loggers declared in Logger.h. A message is formatted into at most
// kMaxLine-1 characters (prefix and line ending included), so one runaway
// message can't flood a file.

#include "OSD/Logger.h"
#include "Util/Format.h"

#include <algorithm>
#include <set>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <syslog.h>
#endif

namespace
{
  constexpr size_t kMaxLine = 4096;

  std::shared_ptr<CLogger> s_logger;

  // The message alone, cut like the line it goes into.
  std::string Format(const char *fmt, va_list vl)
  {
    char buf[kMaxLine];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    return buf;
  }

  // prefix + message (+ newline), cut to the line limit.
  std::string MakeLine(const char *prefix, bool newline, const char *fmt, va_list vl)
  {
    std::string line(prefix);
    line += Format(fmt, vl);
    if (newline)
      line += '\n';
    if (line.size() > kMaxLine - 1)
      line.resize(kMaxLine - 1);
    return line;
  }

  const char *const kDebugPrefix = "[Debug] ";
  const char *const kInfoPrefix = "[Info]  ";
  const char *const kErrorPrefix = "[Error] ";
}

std::shared_ptr<CLogger> GetLogger()
{
  return s_logger;
}

void SetLogger(std::shared_ptr<CLogger> logger)
{
  s_logger = std::move(logger);
}

void DebugLog(const char *fmt, ...)
{
  if (!s_logger)
    return;
  va_list vl;
  va_start(vl, fmt);
  s_logger->DebugLog(fmt, vl);
  va_end(vl);
}

void InfoLog(const char *fmt, ...)
{
  if (!s_logger)
    return;
  va_list vl;
  va_start(vl, fmt);
  s_logger->InfoLog(fmt, vl);
  va_end(vl);
}

Result ErrorLog(const char *fmt, ...)
{
  if (s_logger)
  {
    va_list vl;
    va_start(vl, fmt);
    s_logger->ErrorLog(fmt, vl);
    va_end(vl);
  }
  return Result::FAIL;
}

std::shared_ptr<CLogger> CreateLogger(const Util::Config::Node &config)
{
  // Threshold
  std::string levelName = Util::ToLower(config["LogLevel"].ValueAsDefault<std::string>("info"));
  CLogger::LogLevel level;
  if (levelName == "all")
    level = CLogger::LogLevel::All;
  else if (levelName == "debug")
    level = CLogger::LogLevel::Debug;
  else if (levelName == "info")
    level = CLogger::LogLevel::Info;
  else if (levelName == "error")
    level = CLogger::LogLevel::Error;
  else
  {
    ErrorLog("Invalid log level: %s", levelName.c_str());
    return std::shared_ptr<CLogger>();
  }

  // Outputs: a comma-separated list of stdout, stderr, syslog (any case)
  // and file names (kept as written, without surrounding spaces).
  bool toStdout = false, toStderr = false, toSyslog = false;
  std::set<std::string> files;
  std::string outputs = config["LogOutput"].ValueAsDefault<std::string>(std::string());
  for (const std::string &item : Util::Format(outputs).Split(','))
  {
    std::string name = Util::TrimWhiteSpace(item);
    std::string lower = Util::ToLower(name);
    if (lower == "stdout")
      toStdout = true;
    else if (lower == "stderr")
      toStderr = true;
    else if (lower == "syslog")
      toSyslog = true;
    else if (!name.empty())
      files.insert(name);
  }

  std::vector<std::shared_ptr<CLogger>> loggers;
  loggers.push_back(std::make_shared<CConsoleErrorLogger>());   // errors always reach the console

  std::vector<FILE *> streams;
  if (toStdout)
    streams.push_back(stdout);
  if (toStderr)
    streams.push_back(stderr);
  if (!files.empty() || !streams.empty())
    loggers.push_back(std::make_shared<CFileLogger>(level, std::vector<std::string>(files.begin(), files.end()), streams));

  if (toSyslog)
    loggers.push_back(std::make_shared<CSystemLogger>(level));

  return std::make_shared<CMultiLogger>(loggers);
}

// ------------------------------------------------------------ CMultiLogger

CMultiLogger::CMultiLogger(std::vector<std::shared_ptr<CLogger>> loggers)
  : m_loggers(std::move(loggers))
{
}

// Each logger consumes the argument list, so each gets its own copy.
#define FOR_EACH_LOGGER(call)                 \
  for (auto &logger : m_loggers)              \
  {                                           \
    va_list copy;                             \
    va_copy(copy, vl);                        \
    logger->call(fmt, copy);                  \
    va_end(copy);                             \
  }

void CMultiLogger::DebugLog(const char *fmt, va_list vl)
{
  FOR_EACH_LOGGER(DebugLog)
}

void CMultiLogger::InfoLog(const char *fmt, va_list vl)
{
  FOR_EACH_LOGGER(InfoLog)
}

void CMultiLogger::ErrorLog(const char *fmt, va_list vl)
{
  FOR_EACH_LOGGER(ErrorLog)
}

#undef FOR_EACH_LOGGER

// ----------------------------------------------------- CConsoleErrorLogger

// Debug and info text goes to the console only through a CFileLogger on
// stdout or stderr.
void CConsoleErrorLogger::DebugLog(const char *, va_list)
{
}

void CConsoleErrorLogger::InfoLog(const char *, va_list)
{
}

void CConsoleErrorLogger::ErrorLog(const char *fmt, va_list vl)
{
  std::string text = Format(fmt, vl);
  fprintf(stderr, "Error: %s\n", text.c_str());
}

// ------------------------------------------------------------- CFileLogger

CFileLogger::CFileLogger(CLogger::LogLevel level, const std::vector<std::string> &filenames)
  : m_logLevel(level), m_logFilenames(filenames)
{
  OpenAll(std::ios::out);
}

CFileLogger::CFileLogger(CLogger::LogLevel level, const std::vector<std::string> &filenames, const std::vector<FILE *> &systemFiles)
  : m_logLevel(level), m_logFilenames(filenames), m_systemFiles(systemFiles)
{
  OpenAll(std::ios::out);
}

// (Re)opens every log file; one that can't be opened is skipped until the
// next time.
void CFileLogger::OpenAll(std::ios_base::openmode mode)
{
  m_logFiles.clear();
  for (const std::string &name : m_logFilenames)
  {
    std::ofstream f(name.c_str(), mode);
    if (f.is_open() && f.good())
      m_logFiles.push_back(std::move(f));
  }
}

void CFileLogger::Emit(LogLevel level, const char *prefix, bool newline, bool flush, const char *fmt, va_list vl)
{
  if (m_logLevel > level)
    return;
  std::string line = MakeLine(prefix, newline, fmt, vl);
  std::lock_guard<std::mutex> lock(m_mtx);
  for (std::ofstream &f : m_logFiles)
    f << line;
  for (FILE *fp : m_systemFiles)
    fputs(line.c_str(), fp);
  // Closing and reopening puts the line on disk even if the program dies
  // right after. Debug output is too frequent for that.
  if (flush)
    OpenAll(std::ios::app);
}

void CFileLogger::DebugLog(const char *fmt, va_list vl)
{
  Emit(LogLevel::Debug, kDebugPrefix, false, false, fmt, vl);
}

void CFileLogger::InfoLog(const char *fmt, va_list vl)
{
  Emit(LogLevel::Info, kInfoPrefix, true, true, fmt, vl);
}

void CFileLogger::ErrorLog(const char *fmt, va_list vl)
{
  Emit(LogLevel::Error, kErrorPrefix, true, true, fmt, vl);
}

// ----------------------------------------------------------- CSystemLogger

namespace
{
  void ToSystemLog(int priority, const std::string &line)
  {
#ifdef _WIN32
    (void) priority;
    OutputDebugStringA(line.c_str());
#else
    syslog(priority, "%s", line.c_str());
#endif
  }

#ifdef _WIN32
  constexpr int kSysDebug = 0, kSysInfo = 0, kSysError = 0;
#else
  constexpr int kSysDebug = LOG_DEBUG, kSysInfo = LOG_INFO, kSysError = LOG_ERR;
#endif
}

CSystemLogger::CSystemLogger(CLogger::LogLevel level)
  : m_logLevel(level)
{
}

void CSystemLogger::DebugLog(const char *fmt, va_list vl)
{
  if (m_logLevel <= LogLevel::Debug)
    ToSystemLog(kSysDebug, MakeLine(kDebugPrefix, false, fmt, vl));
}

void CSystemLogger::InfoLog(const char *fmt, va_list vl)
{
  if (m_logLevel <= LogLevel::Info)
    ToSystemLog(kSysInfo, MakeLine(kInfoPrefix, true, fmt, vl));
}

void CSystemLogger::ErrorLog(const char *fmt, va_list vl)
{
  if (m_logLevel <= LogLevel::Error)
    ToSystemLog(kSysError, MakeLine(kErrorPrefix, true, fmt, vl));
}
