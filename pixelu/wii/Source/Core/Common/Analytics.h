// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/HttpRequest.h"
#include "Common/SPSCQueue.h"

// Anonymous usage/performance reports.
//
// A report is a flat sequence of key/value pairs in a small binary wire format (see
// Analytics.cpp), built with AnalyticsReportBuilder and handed to an AnalyticsReporter, which
// sends it from a background thread through a pluggable backend:
//
//   reporter.Send(reporter.Builder().AddData("my_key", 42).AddData("other_key", false));
//
// Nothing is sent unless a backend was set, which requires the user's consent.

namespace Common
{
class AnalyticsReportingBackend
{
public:
  virtual ~AnalyticsReportingBackend() = default;
  // Runs on the reporter thread.
  virtual void Send(std::string report) = 0;
};

// Accumulates serialised key/value pairs. Thread-safe.
class AnalyticsReportBuilder
{
public:
  AnalyticsReportBuilder();
  ~AnalyticsReportBuilder() = default;

  AnalyticsReportBuilder(const AnalyticsReportBuilder& other) : m_report{other.Get()} {}
  AnalyticsReportBuilder(AnalyticsReportBuilder&& other) : m_report{other.Consume()} {}

  const AnalyticsReportBuilder& operator=(const AnalyticsReportBuilder& other)
  {
    if (this != &other)
    {
      std::string copy = other.Get();
      std::lock_guard guard{m_lock};
      m_report = std::move(copy);
    }
    return *this;
  }

  // Appends the other builder's pairs (its version byte included, as the format expects).
  AnalyticsReportBuilder& AddBuilder(const AnalyticsReportBuilder& other)
  {
    // Copy first: `other` may be *this.
    std::string addition = other.Get();
    std::lock_guard guard{m_lock};
    m_report += addition;
    return *this;
  }

  template <typename T>
  AnalyticsReportBuilder& AddData(std::string_view key, const T& value)
  {
    std::lock_guard guard{m_lock};
    AppendSerializedValue(&m_report, key);
    AppendSerializedValue(&m_report, value);
    return *this;
  }

  template <typename T>
  AnalyticsReportBuilder& AddData(std::string_view key, const std::vector<T>& value)
  {
    std::lock_guard guard{m_lock};
    AppendSerializedValue(&m_report, key);
    AppendSerializedValueVector(&m_report, value);
    return *this;
  }

  std::string Get() const
  {
    std::shared_lock guard{m_lock};
    return m_report;
  }

  // Takes the serialised report out of the builder.
  std::string Consume()
  {
    std::lock_guard guard{m_lock};
    return std::move(m_report);
  }

protected:
  static void AppendSerializedValue(std::string* report, std::string_view v);
  // Needed so string literals don't pick the bool overload.
  static void AppendSerializedValue(std::string* report, const char* v);
  static void AppendSerializedValue(std::string* report, bool v);
  static void AppendSerializedValue(std::string* report, u64 v);
  static void AppendSerializedValue(std::string* report, s64 v);
  static void AppendSerializedValue(std::string* report, u32 v);
  static void AppendSerializedValue(std::string* report, s32 v);
  static void AppendSerializedValue(std::string* report, float v);

  static void AppendSerializedValueVector(std::string* report, const std::vector<u32>& v);

  mutable std::shared_mutex m_lock;
  std::string m_report;
};

// Queues reports and sends them on its own thread.
class AnalyticsReporter
{
public:
  AnalyticsReporter();
  ~AnalyticsReporter();

  // Enables sending (queued reports go out now). Only with the user's consent.
  void SetBackend(std::unique_ptr<AnalyticsReportingBackend> backend)
  {
    m_backend = std::move(backend);
    m_reporter_event.Set();
  }

  // Fields added here are copied into every report made with Builder(). Don't send this one.
  AnalyticsReportBuilder& BaseBuilder() { return m_base_builder; }
  // A copy of the base builder to fill in and send.
  AnalyticsReportBuilder Builder() const { return m_base_builder; }

  // Queues the report (consuming the builder). Dropped if too many are already waiting, and
  // always dropped in builds without USE_ANALYTICS.
  void Send(AnalyticsReportBuilder&& report);
  void Send(AnalyticsReportBuilder& report) { Send(std::move(report)); }

protected:
  void ThreadProc();

  std::shared_ptr<AnalyticsReportingBackend> m_backend;
  AnalyticsReportBuilder m_base_builder;

  std::thread m_reporter_thread;
  Common::Event m_reporter_event;
  Common::Flag m_reporter_stop_request;
  SPSCQueue<std::string> m_reports_queue;
};

// Prints a hex dump of each report to stdout (for debugging).
class StdoutAnalyticsBackend : public AnalyticsReportingBackend
{
public:
  void Send(std::string report) override;
};

// POSTs each report to an HTTP(S) endpoint.
class HttpAnalyticsBackend : public AnalyticsReportingBackend
{
public:
  explicit HttpAnalyticsBackend(std::string endpoint);
  ~HttpAnalyticsBackend() override;

  void Send(std::string report) override;

protected:
  std::string m_endpoint;
  HttpRequest m_http{std::chrono::seconds{5}};
};
}  // namespace Common
