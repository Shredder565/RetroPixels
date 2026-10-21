// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Resources.h"

#include <array>

#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QPixmap>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"

bool Resources::s_can_load_svg;
QList<QIcon> Resources::s_platform_icons;
QList<QIcon> Resources::s_flag_icons;
QList<QIcon> Resources::s_misc_icons;

namespace
{
// File names, in the order of DiscIO::Platform, DiscIO::Country and Resources::MiscID.
constexpr std::array<std::string_view, 5> PLATFORM_ICON_NAMES{
    "Platform_Gamecube", "Platform_Triforce", "Platform_Wii", "Platform_Wad", "Platform_File"};
constexpr std::array<std::string_view, 14> FLAG_ICON_NAMES{
    "Flag_Europe",      "Flag_Japan",  "Flag_USA",    "Flag_Australia",
    "Flag_France",      "Flag_Germany", "Flag_Italy",  "Flag_Korea",
    "Flag_Netherlands", "Flag_Russia", "Flag_Spain",  "Flag_Taiwan",
    "Flag_International", "Flag_Unknown"};
constexpr std::array<std::string_view, 2> MISC_ICON_NAMES{"nobanner", "dolphin_logo"};

// PNG variants for 1x, 2x and 4x pixel ratios ("name.png", "name@2x.png", "name@4x.png").
constexpr std::array<int, 3> PNG_SCALES{1, 2, 4};

QString PngPath(const QString& stem, int scale)
{
  if (scale == 1)
    return stem + QStringLiteral(".png");
  return stem + QString::fromLatin1("@%1x.png").arg(scale);
}

QString SysResourcesFolder()
{
  return QString::fromStdString(File::GetSysDirectory() + "Resources");
}

QString CurrentThemeFolder()
{
  return QString::fromStdString(File::GetThemeDir(Config::Get(Config::MAIN_THEME_NAME)));
}
}  // namespace

QIcon Resources::LoadNamedIcon(std::string_view name, const QString& dir)
{
  const QString stem = dir + QLatin1Char{'/'} + QString::fromLatin1(name);

  // An SVG, when there is one and Qt can read it, scales to anything.
  const QString svg = stem + QStringLiteral(".svg");
  if (s_can_load_svg && QFileInfo(svg).exists())
    return QIcon(svg);

  // Otherwise add every PNG size present now. QIcon caches what it is given, so adding them all
  // up front means a later change of pixel ratio still finds the best match.
  QIcon icon;
  for (const int scale : PNG_SCALES)
  {
    QPixmap pixmap(PngPath(stem, scale));
    if (pixmap.isNull())
      continue;
    pixmap.setDevicePixelRatio(scale);
    icon.addPixmap(pixmap);
  }

  if (icon.isNull())
    ERROR_LOG_FMT(COMMON, "LoadNamedIcon: \"{}\" could not be loaded.", name);

  return icon;
}

QIcon Resources::GetResourceIcon(std::string_view name)
{
  return LoadNamedIcon(name, SysResourcesFolder());
}

QIcon Resources::GetThemeIcon(std::string_view name)
{
  return LoadNamedIcon(name, CurrentThemeFolder());
}

void Resources::Init()
{
  s_can_load_svg = QImageReader::supportedImageFormats().contains("svg");

  for (const std::string_view name : PLATFORM_ICON_NAMES)
    s_platform_icons.append(GetResourceIcon(name));
  for (const std::string_view name : FLAG_ICON_NAMES)
    s_flag_icons.append(GetResourceIcon(name));
  for (const std::string_view name : MISC_ICON_NAMES)
    s_misc_icons.append(GetResourceIcon(name));
}

QIcon Resources::GetPlatform(DiscIO::Platform platform)
{
  return s_platform_icons[static_cast<int>(platform)];
}

QIcon Resources::GetCountry(DiscIO::Country country)
{
  return s_flag_icons[static_cast<int>(country)];
}

QIcon Resources::GetMisc(MiscID id)
{
  return s_misc_icons[static_cast<int>(id)];
}

QIcon Resources::GetAppIcon()
{
  return GetMisc(MiscID::Logo);
}
