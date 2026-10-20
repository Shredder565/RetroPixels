// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/MACUtils.h"

#include <optional>
#include <string>

#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"

namespace IOS::Net
{
namespace
{
// Used instead of the configured address when the run must be reproducible.
constexpr const char* DETERMINISTIC_MAC = "12:34:56:78:9a:bc";
}  // namespace

Common::MACAddress GetMACAddress()
{
  // The setting is read even when it is then replaced.
  std::string configured = Config::Get(Config::MAIN_WIRELESS_MAC);
  if (Core::WantsDeterminism())
    configured = DETERMINISTIC_MAC;

  if (const std::optional<Common::MACAddress> parsed = Common::StringToMacAddress(configured))
  {
    INFO_LOG_FMT(IOS_NET, "Using MAC address: {}", Common::MacAddressToString(*parsed));
    return *parsed;
  }

  const Common::MACAddress generated = Common::GenerateMacAddress(Common::MACConsumer::IOS);
  const std::string text = Common::MacAddressToString(generated);
  Config::SetBaseOrCurrent(Config::MAIN_WIRELESS_MAC, text);
  Config::Save();
  if (!configured.empty())
  {
    ERROR_LOG_FMT(IOS_NET, "The MAC provided ({}) is invalid. We have generated another one for you.",
                  text);
  }

  INFO_LOG_FMT(IOS_NET, "Using MAC address: {}", text);
  return generated;
}
}  // namespace IOS::Net
