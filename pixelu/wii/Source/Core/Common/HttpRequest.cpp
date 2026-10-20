// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/HttpRequest.h"

#include <chrono>
#include <cstddef>
#include <mutex>

#include <curl/curl.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace Common
{
namespace
{
using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

struct CurlListDeleter
{
  void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};
struct CurlMimeDeleter
{
  void operator()(curl_mime* mime) const { curl_mime_free(mime); }
};

size_t AppendBody(char* data, size_t size, size_t count, void* userdata)
{
  auto* body = static_cast<std::vector<u8>*>(userdata);
  const size_t bytes = size * count;
  body->insert(body->end(), data, data + bytes);
  return bytes;
}

// Collects "Name: value" header lines (the first occurrence of a name wins). Status lines and
// the blank end line have no colon and are skipped.
size_t CollectHeader(char* data, size_t size, size_t count, void* userdata)
{
  auto* headers = static_cast<HttpRequest::Headers*>(userdata);
  const std::string_view line(data, count);
  const size_t colon = line.find(':');
  if (colon != std::string_view::npos)
  {
    headers->emplace(std::string(line.substr(0, colon)),
                     std::string(StripWhitespace(line.substr(colon + 1))));
  }
  return size * count;
}

// Header list in curl's syntax: "Name:" removes a header, "Name;" sends it empty.
curl_slist* BuildHeaderList(const HttpRequest::Headers& headers)
{
  curl_slist* list = nullptr;
  for (const auto& [name, value] : headers)
  {
    std::string line;
    if (!value)
      line = name + ':';
    else if (value->empty())
      line = name + ';';
    else
      line = name + ": " + *value;
    list = curl_slist_append(list, line.c_str());
  }
  return list;
}
}  // namespace

class HttpRequest::Impl final
{
public:
  enum class Method
  {
    GET,
    POST,
  };

  Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback);

  bool IsValid() const { return m_curl != nullptr; }
  std::string GetHeaderValue(std::string_view name) const;
  void SetCookies(const std::string& cookies);
  void UseIPv4();
  void FollowRedirects(long max);
  s32 GetLastResponseCode() const;
  std::string EscapeComponent(const std::string& string);
  Response Fetch(const std::string& url, Method method, const Headers& headers, const u8* payload,
                 size_t size, AllowedReturnCodes codes, std::span<Multiform> multiform = {});

private:
  static int OnProgress(void* self, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                        curl_off_t ulnow);

  static inline std::once_flag s_global_init;

  ProgressCallback m_callback;
  Headers m_response_headers;
  CurlHandle m_curl{nullptr, curl_easy_cleanup};
  std::string m_error_buffer;
};

HttpRequest::Impl::Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback)
    : m_callback(std::move(callback))
{
  std::call_once(s_global_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  m_curl.reset(curl_easy_init());
  if (!m_curl)
    return;
  CURL* const curl = m_curl.get();

  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, m_callback ? 0L : 1L);
  if (m_callback)
  {
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Impl::OnProgress);
  }

  m_error_buffer.resize(CURL_ERROR_SIZE);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, m_error_buffer.data());

  // Resolver timeouts must not use signals (curl may lack an async resolver).
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms.count()));
  // A transfer slower than 1 byte/s for the timeout (in whole seconds) is aborted.
  curl_easy_setopt(
      curl, CURLOPT_LOW_SPEED_TIME,
      static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(timeout_ms).count()));
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
}

int HttpRequest::Impl::OnProgress(void* self, curl_off_t dltotal, curl_off_t dlnow,
                                  curl_off_t ultotal, curl_off_t ulnow)
{
  const bool keep_going = static_cast<Impl*>(self)->m_callback(
      static_cast<s64>(dltotal), static_cast<s64>(dlnow), static_cast<s64>(ultotal),
      static_cast<s64>(ulnow));
  return keep_going ? 0 : 1;
}

s32 HttpRequest::Impl::GetLastResponseCode() const
{
  long code = 0;
  curl_easy_getinfo(m_curl.get(), CURLINFO_RESPONSE_CODE, &code);
  return static_cast<s32>(code);
}

void HttpRequest::Impl::SetCookies(const std::string& cookies)
{
  curl_easy_setopt(m_curl.get(), CURLOPT_COOKIE, cookies.c_str());
}

void HttpRequest::Impl::UseIPv4()
{
  curl_easy_setopt(m_curl.get(), CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
}

void HttpRequest::Impl::FollowRedirects(long max)
{
  curl_easy_setopt(m_curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(m_curl.get(), CURLOPT_MAXREDIRS, max);
}

std::string HttpRequest::Impl::GetHeaderValue(std::string_view name) const
{
  const auto it = m_response_headers.find(std::string(name));
  if (it == m_response_headers.end())
    return {};
  return it->second.value_or("");
}

std::string HttpRequest::Impl::EscapeComponent(const std::string& string)
{
  char* const escaped =
      curl_easy_escape(m_curl.get(), string.c_str(), static_cast<int>(string.size()));
  if (escaped == nullptr)
    return {};
  std::string result(escaped);
  curl_free(escaped);
  return result;
}

HttpRequest::Response HttpRequest::Impl::Fetch(const std::string& url, Method method,
                                               const Headers& headers, const u8* payload,
                                               size_t size, AllowedReturnCodes codes,
                                               std::span<Multiform> multiform)
{
  CURL* const curl = m_curl.get();
  const bool is_post = method == Method::POST;
  m_response_headers.clear();

  curl_easy_setopt(curl, CURLOPT_POST, is_post ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (is_post && multiform.empty())
  {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
  }

  std::unique_ptr<curl_mime, CurlMimeDeleter> form;
  if (!multiform.empty())
  {
    form.reset(curl_mime_init(curl));
    for (const Multiform& field : multiform)
    {
      curl_mimepart* const part = curl_mime_addpart(form.get());
      curl_mime_name(part, field.name.c_str());
      curl_mime_data(part, field.data.c_str(), field.data.size());
    }
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form.get());
  }

  const std::unique_ptr<curl_slist, CurlListDeleter> header_list(BuildHeaderList(headers));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.get());

  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CollectHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, static_cast<void*>(&m_response_headers));

  std::vector<u8> body;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &AppendBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&body));

  const CURLcode result = curl_easy_perform(curl);

  // Do not leave a pointer to the soon-freed header list in the reusable handle. (The form
  // unbinds itself from the handle when freed.)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<curl_slist*>(nullptr));

  const char* const verb = is_post ? "POST" : "GET";
  if (result != CURLE_OK)
  {
    ERROR_LOG_FMT(COMMON, "{} {} failed: {}", verb, url, m_error_buffer);
    return std::nullopt;
  }

  if (codes == AllowedReturnCodes::All)
    return body;

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (code == 200)
    return body;

  if (body.empty())
  {
    ERROR_LOG_FMT(COMMON, "{} {} failed: HTTP status {}", verb, url, code);
  }
  else
  {
    ERROR_LOG_FMT(COMMON, "{} {} failed: HTTP status {}, body:\n\x1b[0m{}", verb, url, code,
                  std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
  }
  return std::nullopt;
}

HttpRequest::HttpRequest(std::chrono::milliseconds timeout_ms, ProgressCallback callback)
    : m_impl(std::make_unique<Impl>(timeout_ms, std::move(callback)))
{
}

HttpRequest::~HttpRequest() = default;

bool HttpRequest::IsValid() const
{
  return m_impl->IsValid();
}

void HttpRequest::FollowRedirects(long max)
{
  m_impl->FollowRedirects(max);
}

void HttpRequest::UseIPv4()
{
  m_impl->UseIPv4();
}

void HttpRequest::SetCookies(const std::string& cookies)
{
  m_impl->SetCookies(cookies);
}

s32 HttpRequest::GetLastResponseCode() const
{
  return m_impl->GetLastResponseCode();
}

std::string HttpRequest::GetHeaderValue(std::string_view name) const
{
  return m_impl->GetHeaderValue(name);
}

std::string HttpRequest::EscapeComponent(const std::string& string)
{
  return m_impl->EscapeComponent(string);
}

HttpRequest::Response HttpRequest::Post(const std::string& url, std::span<const u8> payload,
                                        const Headers& headers, AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::POST, headers, payload.data(), payload.size(), codes);
}

HttpRequest::Response HttpRequest::Post(const std::string& url, std::string_view payload,
                                        const Headers& headers, AllowedReturnCodes codes)
{
  // Text payloads are sent as their bytes.
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(payload.data()), payload.size());
  return Post(url, bytes, headers, codes);
}

HttpRequest::Response HttpRequest::PostMultiform(const std::string& url,
                                                 std::span<Multiform> multiform,
                                                 const Headers& headers, AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::POST, headers, nullptr, 0, codes, multiform);
}

HttpRequest::Response HttpRequest::Get(const std::string& url, const Headers& headers,
                                       AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::GET, headers, nullptr, 0, codes);
}
}  // namespace Common
