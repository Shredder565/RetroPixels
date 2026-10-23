// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Message logging. Code anywhere calls DebugLog(), InfoLog() or ErrorLog();
// they go to the logger the front end installs with SetLogger(). Loggers:
//
//   CConsoleErrorLogger  errors only, as "Error: ..." on stderr
//   CFileLogger          files and/or stdout/stderr, with a level threshold;
//                        info and error lines are flushed to disk at once
//   CSystemLogger        syslog (OutputDebugString on Windows)
//   CMultiLogger         passes each message on to several loggers
//
// CreateLogger() builds the usual combination from the LogLevel and
// LogOutput settings.

#ifndef INCLUDED_LOGGER_H
#define INCLUDED_LOGGER_H

#include "Types.h"
#include "Version.h"
#include "Util/NewConfig.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

class CLogger
{
public:
  // Thresholds, lowest first: a logger set to Info drops debug messages.
  enum LogLevel : int
  {
    All = 0,
    Debug,
    Info,
    Error
  };

  virtual ~CLogger()
  {
  }

  // printf-style entry points. Debug text is written as given; info and
  // error messages get a line ending added.
  void DebugLog(const char *fmt, ...)
  {
    va_list vl;
    va_start(vl, fmt);
    DebugLog(fmt, vl);
    va_end(vl);
  }

  void InfoLog(const char *fmt, ...)
  {
    va_list vl;
    va_start(vl, fmt);
    InfoLog(fmt, vl);
    va_end(vl);
  }

  void ErrorLog(const char *fmt, ...)
  {
    va_list vl;
    va_start(vl, fmt);
    ErrorLog(fmt, vl);
    va_end(vl);
  }

  virtual void DebugLog(const char *fmt, va_list vl) = 0;
  virtual void InfoLog(const char *fmt, va_list vl) = 0;
  virtual void ErrorLog(const char *fmt, va_list vl) = 0;
};

class CMultiLogger : public CLogger
{
public:
  CMultiLogger(std::vector<std::shared_ptr<CLogger>> loggers);

  void DebugLog(const char *fmt, va_list vl);
  void InfoLog(const char *fmt, va_list vl);
  void ErrorLog(const char *fmt, va_list vl);

private:
  std::vector<std::shared_ptr<CLogger>> m_loggers;
};

class CConsoleErrorLogger : public CLogger
{
  void DebugLog(const char *fmt, va_list vl);
  void InfoLog(const char *fmt, va_list vl);
  void ErrorLog(const char *fmt, va_list vl);
};

class CFileLogger : public CLogger
{
public:
  // The files are created empty; systemFiles (stdout, stderr) get the same
  // lines.
  CFileLogger(LogLevel level, const std::vector<std::string> &filenames);
  CFileLogger(LogLevel level, const std::vector<std::string> &filenames, const std::vector<FILE *> &systemFiles);

  void DebugLog(const char *fmt, va_list vl);
  void InfoLog(const char *fmt, va_list vl);
  void ErrorLog(const char *fmt, va_list vl);

private:
  void Emit(LogLevel level, const char *prefix, bool newline, bool flush, const char *fmt, va_list vl);
  void OpenAll(std::ios_base::openmode mode);

  std::mutex m_mtx;
  LogLevel m_logLevel;
  const std::vector<std::string> m_logFilenames;
  std::vector<std::ofstream> m_logFiles;
  std::vector<FILE *> m_systemFiles;
};

class CSystemLogger : public CLogger
{
public:
  CSystemLogger(LogLevel level);

  void DebugLog(const char *fmt, va_list vl);
  void InfoLog(const char *fmt, va_list vl);
  void ErrorLog(const char *fmt, va_list vl);

private:
  LogLevel m_logLevel;
};

// Debugging output; include line endings yourself.
extern void DebugLog(const char *fmt, ...);

// An error (it need not be fatal). No line ending needed. Always returns
// Result::FAIL so it can end a failing function: return ErrorLog(...).
extern Result ErrorLog(const char *fmt, ...);

// Information for the log only (not the console). No line ending needed.
extern void InfoLog(const char *fmt, ...);

// The logger messages go to; null drops them.
extern void SetLogger(std::shared_ptr<CLogger> logger);
extern std::shared_ptr<CLogger> GetLogger(void);

// Builds the logger the LogLevel and LogOutput settings ask for; null if the
// level isn't one of all, debug, info and error.
std::shared_ptr<CLogger> CreateLogger(const Util::Config::Node &config);

#endif // INCLUDED_LOGGER_H
