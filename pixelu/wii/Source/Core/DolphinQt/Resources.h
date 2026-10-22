// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QIcon>
#include <QList>
#include <string_view>

namespace DiscIO
{
enum class Country;
enum class Platform;
}  // namespace DiscIO

// Icons used all over the UI. The platform, flag and misc icons are loaded once by Init();
// theme and resource icons are loaded on request.
class Resources final
{
public:
  enum class MiscID
  {
    BannerMissing,
    Logo,
  };

  static void Init();

  static QIcon GetPlatform(DiscIO::Platform platform);
  static QIcon GetCountry(DiscIO::Country country);

  static QIcon GetMisc(MiscID id);

  // An icon from Sys/Resources, or from the current theme's folder.
  static QIcon GetResourceIcon(std::string_view name);
  static QIcon GetThemeIcon(std::string_view name);
  static QIcon GetAppIcon();

private:
  Resources() {}
  static QIcon LoadNamedIcon(std::string_view name, const QString& dir);

  static bool s_can_load_svg;
  static QList<QIcon> s_platform_icons;
  static QList<QIcon> s_flag_icons;
  static QList<QIcon> s_misc_icons;
};
