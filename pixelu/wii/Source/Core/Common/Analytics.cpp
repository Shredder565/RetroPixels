// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Analytics.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

// Wire format (what the analytics server parses):
//
//   report := version:u8(=1) (key value)*      a key is a STRING value
//   value  := type:u8 payload
//     STRING (0): varint length, then the bytes
//     BOOL   (1): one byte, 0xFF or 0x00
//     UINT   (2): varint
//     SINT   (3): sign byte (0xFF if >= 0, else 0x00), varint of the magnitude
//     FLOAT  (4): the 4 raw bytes of the float (native order)
//     UINT|ARRAY (0x82): varint count, then that many varints
//   varint := little-endian groups of 7 bits, high bit set on every byte but the last

namespace Common
{
namespace
{
constexpr char WIRE_FORMAT_VERSION = 1;

enum TypeTag : u8
{
  TAG_STRING = 0,
  TAG_BOOL = 1,
  TAG_UINT = 2,
  TAG_SINT = 3,
  TAG_FLOAT = 4,
  TAG_ARRAY_FLAG = 0x80,
};

void PutTag(std::string* out, u8 tag)
{
  out->push_back(static_cast<char>(tag));
}

void PutFlag(std::string* out, bool value)
{
  out->push_back(value ? '\xFF' : '\x00');
}

void PutVarInt(std::string* out, u64 value)
{
  for (;;)
  {
    const u8 low = value & 0x7F;
    value >>= 7;
    if (value == 0)
    {
      out->push_back(static_cast<char>(low));
      return;
    }
    out->push_back(static_cast<char>(low | 0x80));
  }
}
}  // namespace

AnalyticsReportBuilder::AnalyticsReportBuilder()
{
  m_report.push_back(WIRE_FORMAT_VERSION);
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, std::string_view v)
{
  PutTag(report, TAG_STRING);
  PutVarInt(report, static_cast<u32>(v.size()));
  report->append(v.data(), static_cast<u32>(v.size()));
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, const char* v)
{
  AppendSerializedValue(report, std::string_view(v));
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, bool v)
{
  PutTag(report, TAG_BOOL);
  PutFlag(report, v);
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, u64 v)
{
  PutTag(report, TAG_UINT);
  PutVarInt(report, v);
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, s64 v)
{
  PutTag(report, TAG_SINT);
  PutFlag(report, v >= 0);
  // Magnitude computed in unsigned arithmetic so INT64_MIN works too.
  const u64 bits = static_cast<u64>(v);
  PutVarInt(report, v >= 0 ? bits : ~bits + 1);
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, u32 v)
{
  AppendSerializedValue(report, static_cast<u64>(v));
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, s32 v)
{
  AppendSerializedValue(report, static_cast<s64>(v));
}

void AnalyticsReportBuilder::AppendSerializedValue(std::string* report, float v)
{
  PutTag(report, TAG_FLOAT);
  char bytes[sizeof(v)];
  std::memcpy(bytes, &v, sizeof(v));
  report->append(bytes, sizeof(bytes));
}

void AnalyticsReportBuilder::AppendSerializedValueVector(std::string* report,
                                                         const std::vector<u32>& v)
{
  PutTag(report, TAG_UINT | TAG_ARRAY_FLAG);
  PutVarInt(report, v.size());
  for (const u32 element : v)
    PutVarInt(report, element);
}

AnalyticsReporter::AnalyticsReporter()
{
  // Started here rather than in the initialiser list: the thread uses members declared after
  // m_reporter_thread.
  m_reporter_thread = std::thread([this] { ThreadProc(); });
}

AnalyticsReporter::~AnalyticsReporter()
{
  m_reporter_stop_request.Set();
  m_reporter_event.Set();
  m_reporter_thread.join();
}

void AnalyticsReporter::Send(AnalyticsReportBuilder&& report)
{
#if defined(USE_ANALYTICS) && USE_ANALYTICS
  // Don't let reports pile up without bound (e.g. while no backend is set).
  constexpr std::size_t MAX_QUEUED_REPORTS = 25;
  if (m_reports_queue.Size() >= MAX_QUEUED_REPORTS)
    return;
  m_reports_queue.Push(report.Consume());
  m_reporter_event.Set();
#else
  static_cast<void>(report);
#endif
}

void AnalyticsReporter::ThreadProc()
{
  SetCurrentThreadName("Analytics");

  for (;;)
  {
    m_reporter_event.Wait();
    if (m_reporter_stop_request.IsSet())
      return;

    // Without a backend, reports stay queued until one is set.
    while (!m_reports_queue.Empty())
    {
      const std::shared_ptr<AnalyticsReportingBackend> backend = m_backend;
      if (!backend)
        break;

      std::string report;
      m_reports_queue.Pop(report);
      backend->Send(std::move(report));

      if (m_reporter_stop_request.IsSet())
        return;
    }
  }
}

void StdoutAnalyticsBackend::Send(std::string report)
{
  std::printf("Analytics report sent:\n%s",
              HexDump(reinterpret_cast<const u8*>(report.data()), report.size()).c_str());
}

HttpAnalyticsBackend::HttpAnalyticsBackend(std::string endpoint) : m_endpoint(std::move(endpoint))
{
}

HttpAnalyticsBackend::~HttpAnalyticsBackend() = default;

void HttpAnalyticsBackend::Send(std::string report)
{
  if (m_http.IsValid())
    m_http.Post(m_endpoint, report);
}
}  // namespace Common
