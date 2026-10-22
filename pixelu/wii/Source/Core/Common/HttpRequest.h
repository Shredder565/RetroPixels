// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
// A reusable HTTP(S) client (libcurl easy handle). Requests are blocking.
class HttpRequest final
{
public:
  enum class AllowedReturnCodes : u8
  {
    Ok_Only,  // anything but 200 is a failure
    All
  };

  // Called during transfers with byte counts; return false to abort.
  using ProgressCallback = std::function<bool(s64 dltotal, s64 dlnow, s64 ultotal, s64 ulnow)>;

  // `timeout_ms` limits connecting and how long a transfer may stall.
  explicit HttpRequest(std::chrono::milliseconds timeout_ms = std::chrono::milliseconds{3000},
                       ProgressCallback callback = nullptr);
  ~HttpRequest();
  bool IsValid() const;

  // The body, or nullopt on failure.
  using Response = std::optional<std::vector<u8>>;
  // Request headers. A null value removes a default header; an empty one sends "Name;" (an
  // empty header).
  using Headers = std::map<std::string, std::optional<std::string>>;

  struct Multiform
  {
    std::string name;
    std::string data;
  };

  void SetCookies(const std::string& cookies);
  void UseIPv4();
  void FollowRedirects(long max = 1);
  s32 GetLastResponseCode() const;
  // Percent-encodes `string` for use in a URL.
  std::string EscapeComponent(const std::string& string);
  // A header of the last response (exact name match), or empty.
  std::string GetHeaderValue(std::string_view name) const;

  Response Get(const std::string& url, const Headers& headers = {},
               AllowedReturnCodes codes = AllowedReturnCodes::Ok_Only);
  Response Post(const std::string& url, std::span<const u8> payload, const Headers& headers = {},
                AllowedReturnCodes codes = AllowedReturnCodes::Ok_Only);
  Response Post(const std::string& url, std::string_view payload, const Headers& headers = {},
                AllowedReturnCodes codes = AllowedReturnCodes::Ok_Only);
  // multipart/form-data POST.
  Response PostMultiform(const std::string& url, std::span<Multiform> multiform,
                         const Headers& headers = {},
                         AllowedReturnCodes codes = AllowedReturnCodes::Ok_Only);

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace Common
