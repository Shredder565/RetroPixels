// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPalette>
#include <QRadioButton>
#include <QSize>
#include <QStyle>
#include <QStyleHints>
#include <QThread>
#include <QWidget>

#include "AudioCommon/AudioCommon.h"

#include "Common/Config/Config.h"
#include "Common/Contains.h"
#include "Common/FileUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/IOS/IOS.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/QueueOnObject.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/NetPlayChatUI.h"
#include "VideoCommon/NetPlayGolfUI.h"

namespace
{
// The application palette from before any style was applied, restored for the plain styles.
std::unique_ptr<QPalette> s_startup_palette;

// --- Qt.ini helpers ---------------------------------------------------------------------------

bool ReadFlag(const QString& key, bool fallback = false)
{
  return Settings::GetQSettings().value(key, fallback).toBool();
}

// Stores `value` unless it is already what ReadFlag(key, fallback) returns. Returns whether
// anything changed, i.e. whether the caller should announce it.
bool WriteFlag(const QString& key, bool value, bool fallback = false)
{
  if (ReadFlag(key, fallback) == value)
    return false;
  Settings::GetQSettings().setValue(key, value);
  return true;
}

// --- Palettes ----------------------------------------------------------------------------------

QColor Gray(int level)
{
  return QColor(level, level, level);
}

// A colour role with its normal colour and the one for disabled widgets.
struct RoleColors
{
  QPalette::ColorRole role;
  QColor normal;
  QColor disabled;
};

void Paint(QPalette& palette, std::span<const RoleColors> colors)
{
  for (const RoleColors& entry : colors)
  {
    palette.setColor(QPalette::All, entry.role, entry.normal);
    palette.setColor(QPalette::Disabled, entry.role, entry.disabled);
  }
}

QPalette FusionLightPalette()
{
  const QColor highlight(48, 140, 198);
  const RoleColors colors[] = {
      {QPalette::Window, Gray(239), Gray(239)},
      {QPalette::WindowText, Gray(0), Gray(190)},
      {QPalette::Base, Gray(255), Gray(239)},
      {QPalette::AlternateBase, Gray(247), Gray(247)},
      {QPalette::ToolTipBase, QColor(255, 255, 220), QColor(255, 255, 220)},
      {QPalette::ToolTipText, Gray(0), Gray(0)},
      {QPalette::PlaceholderText, Gray(119), Gray(119)},
      {QPalette::Text, Gray(0), Gray(190)},
      {QPalette::Button, Gray(239), Gray(239)},
      {QPalette::ButtonText, Gray(0), Gray(190)},
      {QPalette::BrightText, Gray(255), Gray(255)},
      {QPalette::Light, Gray(255), Gray(255)},
      {QPalette::Midlight, Gray(202), Gray(202)},
      {QPalette::Dark, Gray(159), Gray(190)},
      {QPalette::Mid, Gray(184), Gray(184)},
      {QPalette::Shadow, Gray(118), Gray(177)},
      {QPalette::Highlight, highlight, Gray(145)},
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
      {QPalette::Accent, highlight, Gray(145).darker()},
#endif
      {QPalette::HighlightedText, Gray(255), Gray(255)},
      {QPalette::Link, QColor(0, 0, 255), QColor(0, 0, 255)},
      {QPalette::LinkVisited, QColor(255, 0, 255), QColor(255, 0, 255)},
  };
  QPalette palette;
  Paint(palette, colors);
  return palette;
}

// The two dark Fusion palettes share their accent colours' treatment: disabled is darker.
QPalette FusionDarkGrayPalette()
{
  const QColor highlight(45, 140, 225);
  const QColor link(40, 130, 220);
  const QColor visited(110, 70, 150);
  const RoleColors colors[] = {
      {QPalette::Window, Gray(50), Gray(55)},
      {QPalette::WindowText, Gray(200), Gray(108)},
      {QPalette::Base, Gray(25), Gray(30)},
      {QPalette::AlternateBase, Gray(38), Gray(42)},
      {QPalette::ToolTipBase, Gray(45), Gray(45)},
      {QPalette::ToolTipText, Gray(200), Gray(200)},
      {QPalette::PlaceholderText, Gray(90), Gray(90)},
      {QPalette::Text, Gray(200), Gray(108)},
      {QPalette::Button, Gray(54), Gray(54)},
      {QPalette::ButtonText, Gray(200), Gray(108)},
      {QPalette::BrightText, Gray(75), Gray(255)},
      {QPalette::Light, Gray(26), Gray(26)},
      {QPalette::Midlight, Gray(40), Gray(40)},
      {QPalette::Dark, Gray(108), Gray(108)},
      {QPalette::Mid, Gray(71), Gray(71)},
      {QPalette::Shadow, Gray(25), Gray(37)},
      {QPalette::Highlight, highlight, highlight.darker()},
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
      {QPalette::Accent, highlight, highlight.darker()},
#endif
      {QPalette::HighlightedText, Gray(255), Gray(40)},
      {QPalette::Link, link, link.darker()},
      {QPalette::LinkVisited, visited, visited.darker()},
  };
  QPalette palette;
  Paint(palette, colors);
  return palette;
}

QPalette FusionDarkPalette()
{
  const QColor highlight(35, 130, 200);
  const QColor link(40, 130, 220);
  const QColor visited(110, 70, 150);
  const RoleColors colors[] = {
      {QPalette::Window, Gray(22), Gray(30)},
      {QPalette::WindowText, Gray(180), Gray(90)},
      {QPalette::Base, Gray(35), Gray(30)},
      {QPalette::AlternateBase, Gray(40), Gray(35)},
      {QPalette::ToolTipBase, Gray(0), Gray(0)},
      {QPalette::ToolTipText, Gray(170), Gray(170)},
      {QPalette::PlaceholderText, Gray(100), Gray(100)},
      {QPalette::Text, Gray(200), Gray(90)},
      {QPalette::Button, Gray(30), Gray(20)},
      {QPalette::ButtonText, Gray(180), Gray(90)},
      {QPalette::BrightText, Gray(75), Gray(255)},
      {QPalette::Light, Gray(0), Gray(0)},
      {QPalette::Midlight, Gray(40), Gray(40)},
      {QPalette::Dark, Gray(90), Gray(90)},
      {QPalette::Mid, Gray(60), Gray(60)},
      {QPalette::Shadow, Gray(10), Gray(20)},
      {QPalette::Highlight, highlight, highlight.darker()},
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
      {QPalette::Accent, highlight, highlight.darker()},
#endif
      {QPalette::HighlightedText, Gray(240), Gray(35)},
      {QPalette::Link, link, link.darker()},
      {QPalette::LinkVisited, visited, visited.darker()},
  };
  QPalette palette;
  Paint(palette, colors);
  return palette;
}

#ifdef _WIN32
// Windows styles have no dark palette of their own, so one is supplied for the "(Dark)" style
// and for any style while Windows itself is in dark mode.
QPalette WindowsDarkPalette()
{
  struct RoleColor
  {
    QPalette::ColorRole role;
    QColor color;
  };
  const QColor accent(0, 120, 215);
  const QColor link(100, 160, 220);
  const RoleColor colors[] = {
      {QPalette::Window, Gray(32)},          {QPalette::WindowText, Gray(220)},
      {QPalette::Base, Gray(32)},            {QPalette::AlternateBase, Gray(48)},
      {QPalette::PlaceholderText, Gray(126)}, {QPalette::Text, Gray(220)},
      {QPalette::Button, Gray(48)},          {QPalette::ButtonText, Gray(220)},
      {QPalette::BrightText, Gray(255)},     {QPalette::Highlight, accent},
      {QPalette::HighlightedText, Gray(255)}, {QPalette::Link, link},
      {QPalette::LinkVisited, link},
  };

  QPalette palette = qApp->style()->standardPalette();
  for (const RoleColor& entry : colors)
    palette.setColor(entry.role, entry.color);
  return palette;
}
#endif

QString ReadTextFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QFile::ReadOnly))
    return {};
  // Read as a C string: anything after a NUL byte is ignored.
  const QByteArray contents = file.readAll();
  return QString::fromUtf8(contents.constData());
}

QString HexColor(const QColor& color)
{
  return QString::number(color.rgba(), 16);
}
}  // namespace

// --- Construction ---------------------------------------------------------------------------

Settings::Settings()
{
  qRegisterMetaType<Core::State>();
  WatchCoreState();
  WatchConfig();
  WatchInputDevices();
}

Settings::~Settings()
{
  Config::RemoveConfigChangedCallback(m_config_callback);
}

void Settings::WatchCoreState()
{
  m_core_state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    QueueOnObject(this, [this, state] {
      // A held frame advance pauses and resumes every frame; only the first and last step of
      // such a run are announced.
      if (m_frame_stepping)
        return;
      emit EmulationStateChanged(state);
    });
  });
}

void Settings::WatchConfig()
{
  // Config changes can come in bursts and from any thread. One ConfigChanged is queued per burst
  // and emitted on the GUI thread, so widgets never react half-way through a change made by
  // another thread.
  m_config_callback = Config::AddConfigChangedCallback([this] {
    if (m_config_signal_pending.exchange(true))
      return;
    QueueOnObject(this, [this] {
      m_config_signal_pending = false;
      emit ConfigChanged();
    });
  });
}

void Settings::WatchInputDevices()
{
  m_devices_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
    if (QThread::currentThread() == qApp->thread())
    {
      emit DevicesChanged();
      return;
    }

    // From another thread, ask everyone to drop their device references right now (some devices
    // cannot be reopened while an old handle is alive), then refresh on the GUI thread.
    emit ReleaseDevices();
    QueueOnObject(this, [this] { emit DevicesChanged(); });
  });
}

void Settings::UnregisterDevicesChangedCallback()
{
  m_devices_hook.reset();
}

Settings& Settings::Instance()
{
  static Settings instance;
  return instance;
}

QSettings& Settings::GetQSettings()
{
  static QSettings qt_ini(
      QStringLiteral("%1/Qt.ini").arg(QString::fromStdString(File::GetUserPath(D_CONFIG_IDX))),
      QSettings::IniFormat);
  return qt_ini;
}

// --- Style ---------------------------------------------------------------------------------

void Settings::TriggerThemeChanged()
{
  emit ThemeChanged();
}

QString Settings::GetUserStyleName() const
{
  const QString name_key = QStringLiteral("userstyle/name");
  if (GetQSettings().contains(name_key))
    return GetQSettings().value(name_key).toString();

  // Older versions stored the full path of the style sheet.
  return QFileInfo(GetQSettings().value(QStringLiteral("userstyle/path")).toString()).fileName();
}

void Settings::SetUserStyleName(const QString& stylesheet_name)
{
  GetQSettings().setValue(QStringLiteral("userstyle/name"), stylesheet_name);
}

void Settings::InitDefaultPalette()
{
  s_startup_palette = std::make_unique<QPalette>(qApp->palette());
}

bool Settings::IsSystemDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  return qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
  return false;
#endif
}

bool Settings::IsThemeDark()
{
  return qApp->palette().color(QPalette::Base).valueF() < 0.5;
}

Settings::StyleType Settings::GetStyleType() const
{
  const QString type_key = QStringLiteral("userstyle/styletype");
  if (GetQSettings().contains(type_key))
  {
    bool is_number = false;
    const int stored = GetQSettings().value(type_key).toInt(&is_number);
    const bool in_range = stored >= static_cast<int>(StyleType::MinValue) &&
                          stored <= static_cast<int>(StyleType::MaxValue);
    if (is_number && in_range)
      return static_cast<StyleType>(stored);
  }

  // Missing or bad: fall back to the older on/off switch for user styles.
  return ReadFlag(QStringLiteral("userstyle/enabled")) ? StyleType::User : StyleType::System;
}

void Settings::SetStyleType(StyleType type)
{
  GetQSettings().setValue(QStringLiteral("userstyle/styletype"), static_cast<int>(type));
  // Kept up to date for older versions, which only know this switch.
  GetQSettings().setValue(QStringLiteral("userstyle/enabled"), type == StyleType::User);
}

void Settings::ApplyStyle()
{
  const StyleType type = GetStyleType();

  // The Fusion variants need Qt's Fusion style; everything else uses the platform style that was
  // active when the first style was applied.
  static const QString platform_style = QApplication::style()->name();
  const bool fusion = type == StyleType::FusionLight || type == StyleType::FusionDarkGray ||
                      type == StyleType::FusionDark;
  const QString wanted_style = fusion ? QStringLiteral("fusion") : platform_style;
  if (QApplication::style()->name() != wanted_style)
    QApplication::setStyle(wanted_style);

  QString style_sheet;
  const QString user_sheet = GetUserStyleName();
  if (type == StyleType::User && !user_sheet.isEmpty())
  {
    const QDir styles_folder(QString::fromStdString(File::GetUserPath(D_STYLES_IDX)));
    style_sheet = ReadTextFile(styles_folder.filePath(user_sheet));
  }

  QPalette palette;
  switch (type)
  {
  case StyleType::FusionLight:
    palette = FusionLightPalette();
    break;
  case StyleType::FusionDarkGray:
    palette = FusionDarkGrayPalette();
    break;
  case StyleType::FusionDark:
    palette = FusionDarkPalette();
    break;
  default:
  {
#ifdef _WIN32
    const bool dark = type == StyleType::Dark || (type != StyleType::Light && IsSystemDark());
    if (dark)
    {
      // Our own dark style sheet, unless a user style sheet is in use.
      if (style_sheet.isEmpty())
        style_sheet = ReadTextFile(QStringLiteral(":/dolphin_dark_win/dark.qss"));
      palette = WindowsDarkPalette();
      break;
    }
#endif
    if (s_startup_palette)
      palette = *s_startup_palette;
    break;
  }
  }

  qApp->setPalette(palette);

  // Tool tips get a style of their own unless the style sheet already has one.
  if (!style_sheet.contains(QStringLiteral("QToolTip"), Qt::CaseSensitive))
  {
    QColor background;
    QColor text;
    QColor emphasis;
    QColor border;
    GetToolTipStyle(background, text, emphasis, border, palette, palette);

    const int padding = QFontMetrics(QFont()).height() / 2;
    const QString tooltip_rule =
        QStringLiteral("QToolTip { background-color: #%1; color: #%2; padding: %3px; "
                       "border: 1px; border-style: solid; border-color: #%4; }")
            .arg(HexColor(background), HexColor(text), QString::number(padding), HexColor(border));
    style_sheet.append(tooltip_rule);
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  // Fusion's group boxes waste a lot of space by default; tighten them unless the style sheet
  // styles group boxes itself.
  if (wanted_style.compare(QStringLiteral("fusion"), Qt::CaseInsensitive) == 0 &&
      !style_sheet.contains(QStringLiteral("QGroupBox")))
  {
    style_sheet.append(QStringLiteral(
        "QGroupBox { margin-top: 0.6em; padding-top: 0.5em; padding-bottom: 0; padding-left: 1px;"
        " padding-right: 1px;} QGroupBox::title { subcontrol-origin: margin;"
        " subcontrol-position: top left; left: 0.7em; padding-top: 1px; min-width: 0;}"));
  }
#endif

  qApp->setStyleSheet(style_sheet);
}

void Settings::GetToolTipStyle(QColor& window_color, QColor& text_color,
                               QColor& emphasis_text_color, QColor& border_color,
                               const QPalette& palette, const QPalette& high_contrast_palette) const
{
  const bool light_theme = palette.color(QPalette::Base).toHsv().value() > 128;
  const QColor yellow = Qt::yellow;
  const QColor light_blue(QStringLiteral("#0090ff"));

  if (!Config::Get(Config::MAIN_USE_HIGH_CONTRAST_TOOLTIPS))
  {
    // Plain tool tips follow the theme.
    window_color = palette.color(QPalette::Window);
    text_color = palette.color(QPalette::Text);
    border_color = palette.color(QPalette::Text);
    emphasis_text_color = light_theme ? light_blue : yellow;
    return;
  }

  // High-contrast tool tips are the opposite of the theme: dark on light themes and vice versa.
  window_color = light_theme ? QColor(72, 72, 72) : QColor(Qt::white);
  text_color = light_theme ? QColor(Qt::white) : QColor(Qt::black);
  emphasis_text_color = light_theme ? yellow : light_blue;
  border_color = high_contrast_palette.color(QPalette::Window).darker(160);
}

// --- Widgets -------------------------------------------------------------------------------

bool Settings::IsLogVisible() const
{
  return ReadFlag(QStringLiteral("logging/logvisible"));
}

void Settings::SetLogVisible(bool visible)
{
  if (WriteFlag(QStringLiteral("logging/logvisible"), visible))
    emit LogVisibilityChanged(visible);
}

bool Settings::IsLogConfigVisible() const
{
  return ReadFlag(QStringLiteral("logging/logconfigvisible"));
}

void Settings::SetLogConfigVisible(bool visible)
{
  if (WriteFlag(QStringLiteral("logging/logconfigvisible"), visible))
    emit LogConfigVisibilityChanged(visible);
}

bool Settings::IsToolBarVisible() const
{
  return ReadFlag(QStringLiteral("toolbar/visible"), true);
}

void Settings::SetToolBarVisible(bool visible)
{
  if (WriteFlag(QStringLiteral("toolbar/visible"), visible, true))
    emit ToolBarVisibilityChanged(visible);
}

bool Settings::AreWidgetsLocked() const
{
  return ReadFlag(QStringLiteral("widgets/locked"), true);
}

void Settings::SetWidgetsLocked(bool locked)
{
  if (WriteFlag(QStringLiteral("widgets/locked"), locked, true))
    emit WidgetLockChanged(locked);
}

void Settings::RefreshWidgetVisibility()
{
  // Re-announces the stored states so freshly created widgets pick them up.
  emit DebugModeToggled(IsDebugModeEnabled());
  emit LogVisibilityChanged(IsLogVisible());
  emit LogConfigVisibilityChanged(IsLogConfigVisible());
  emit GameCountVisibilityChanged(IsGameCountVisible());
}

// --- Game list -----------------------------------------------------------------------------

QStringList Settings::GetPaths() const
{
  QStringList paths;
  for (const std::string& path : Config::GetIsoPaths())
    paths.append(QString::fromStdString(path));
  return paths;
}

void Settings::AddPath(const QString& qpath)
{
  const std::string path = qpath.toStdString();
  std::vector<std::string> paths = Config::GetIsoPaths();
  if (Common::Contains(paths, path))
    return;

  paths.push_back(path);
  Config::SetIsoPaths(paths);
  emit PathAdded(qpath);
}

void Settings::RemovePath(const QString& qpath)
{
  const std::string path = qpath.toStdString();
  std::vector<std::string> paths = Config::GetIsoPaths();
  if (std::erase(paths, path) == 0)
    return;

  Config::SetIsoPaths(paths);
  emit PathRemoved(qpath);
}

bool Settings::GetPreferredView() const
{
  return ReadFlag(QStringLiteral("PreferredView"), true);
}

void Settings::SetPreferredView(bool list)
{
  GetQSettings().setValue(QStringLiteral("PreferredView"), list);
}

QString Settings::GetDefaultGame() const
{
  return QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO));
}

void Settings::SetDefaultGame(const QString& path)
{
  if (path == GetDefaultGame())
    return;
  Config::SetBase(Config::MAIN_DEFAULT_ISO, path.toStdString());
  emit DefaultGameChanged(path);
}

// Game list traffic: requests go out to the game list's tracker, progress comes back from it.
void Settings::ReloadTitleDB()
{
  emit TitleDBReloadRequested();
}

void Settings::NotifyMetadataRefreshComplete()
{
  emit MetadataRefreshCompleted();
}

void Settings::RefreshGameList()
{
  emit GameListRefreshRequested();
}

void Settings::NotifyRefreshGameListComplete()
{
  emit GameListRefreshCompleted();
}

void Settings::NotifyRefreshGameListStarted()
{
  emit GameListRefreshStarted();
}

bool Settings::IsAutoRefreshEnabled() const
{
  return ReadFlag(QStringLiteral("gamelist/autorefresh"), true);
}

void Settings::SetAutoRefreshEnabled(bool enabled)
{
  if (WriteFlag(QStringLiteral("gamelist/autorefresh"), enabled, true))
    emit AutoRefreshToggled(enabled);
}

bool Settings::IsGameCountVisible() const
{
  return ReadFlag(QStringLiteral("GameCount/Visible"), true);
}

void Settings::SetGameCountVisible(const bool visible)
{
  if (WriteFlag(QStringLiteral("GameCount/Visible"), visible, true))
    emit GameCountVisibilityChanged(visible);
}

// --- Emulation -----------------------------------------------------------------------------

int Settings::GetStateSlot() const
{
  return GetQSettings().value(QStringLiteral("Emulation/StateSlot"), 1).toInt();
}

void Settings::SetStateSlot(int slot)
{
  GetQSettings().setValue(QStringLiteral("Emulation/StateSlot"), slot);
}

bool Settings::IsBatchModeEnabled() const
{
  return m_batch_mode;
}

void Settings::SetBatchModeEnabled(bool batch)
{
  m_batch_mode = batch;
}

bool Settings::IsSDCardInserted() const
{
  return Config::Get(Config::MAIN_WII_SD_CARD);
}

void Settings::SetSDCardInserted(bool inserted)
{
  if (inserted != IsSDCardInserted())
    Config::SetBaseOrCurrent(Config::MAIN_WII_SD_CARD, inserted);
}

bool Settings::IsUSBKeyboardConnected() const
{
  return Config::Get(Config::MAIN_WII_KEYBOARD);
}

void Settings::SetUSBKeyboardConnected(bool connected)
{
  if (connected != IsUSBKeyboardConnected())
    Config::SetBaseOrCurrent(Config::MAIN_WII_KEYBOARD, connected);
}

bool Settings::IsWiiSpeakMuted() const
{
  return Config::Get(Config::MAIN_WII_SPEAK_MUTED);
}

void Settings::SetWiiSpeakMuted(bool muted)
{
  if (muted == IsWiiSpeakMuted())
    return;
  Config::SetBaseOrCurrent(Config::MAIN_WII_SPEAK_MUTED, muted);
  emit WiiSpeakMuteChanged(muted);
}

void Settings::SetIsContinuouslyFrameStepping(bool is_stepping)
{
  m_frame_stepping = is_stepping;
}

bool Settings::GetIsContinuouslyFrameStepping() const
{
  return m_frame_stepping;
}

// --- Graphics ------------------------------------------------------------------------------

Config::ShowCursor Settings::GetCursorVisibility() const
{
  return Config::Get(Config::MAIN_SHOW_CURSOR);
}

bool Settings::GetLockCursor() const
{
  return Config::Get(Config::MAIN_LOCK_CURSOR);
}

void Settings::SetKeepWindowOnTop(bool top)
{
  // Only announced here; the setting itself is written by whoever owns it.
  if (top != IsKeepWindowOnTopEnabled())
    emit KeepWindowOnTopChanged(top);
}

bool Settings::IsKeepWindowOnTopEnabled() const
{
  return Config::Get(Config::MAIN_KEEP_WINDOW_ON_TOP);
}

bool Settings::GetGraphicModsEnabled() const
{
  return Config::Get(Config::GFX_MODS_ENABLE);
}

void Settings::SetGraphicModsEnabled(bool enabled)
{
  if (enabled == GetGraphicModsEnabled())
    return;
  Config::SetBaseOrCurrent(Config::GFX_MODS_ENABLE, enabled);
  emit EnableGfxModsChanged(enabled);
}

// --- Audio ---------------------------------------------------------------------------------

int Settings::GetVolume() const
{
  return Config::Get(Config::MAIN_AUDIO_VOLUME);
}

void Settings::SetVolume(int volume)
{
  if (volume != GetVolume())
    Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME, volume);
}

void Settings::IncreaseVolume(int volume)
{
  AudioCommon::IncreaseVolume(Core::System::GetInstance(), static_cast<u16>(volume));
}

void Settings::DecreaseVolume(int volume)
{
  AudioCommon::DecreaseVolume(Core::System::GetInstance(), static_cast<u16>(volume));
}

// --- NetPlay -------------------------------------------------------------------------------

std::shared_ptr<NetPlay::NetPlayClient> Settings::GetNetPlayClient()
{
  return m_netplay_client;
}

void Settings::ResetNetPlayClient(NetPlay::NetPlayClient* client)
{
  m_netplay_client.reset(client);

  // The on-screen chat and golf overlays belong to the session that just ended (or changed).
  g_netplay_chat_ui.reset();
  g_netplay_golf_ui.reset();
}

std::shared_ptr<NetPlay::NetPlayServer> Settings::GetNetPlayServer()
{
  return m_netplay_server;
}

void Settings::ResetNetPlayServer(NetPlay::NetPlayServer* server)
{
  m_netplay_server.reset(server);
}

bool Settings::GetCheatsEnabled() const
{
  return Config::Get(Config::MAIN_ENABLE_CHEATS);
}

// --- Debugger ------------------------------------------------------------------------------

void Settings::SetDebugModeEnabled(bool enabled)
{
  // Hardcore achievements rule out debugging.
  const bool allowed = enabled && !AchievementManager::GetInstance().IsHardcoreModeActive();
  if (allowed == IsDebugModeEnabled())
    return;
  Config::SetBaseOrCurrent(Config::MAIN_ENABLE_DEBUGGING, allowed);
  emit DebugModeToggled(allowed);
}

bool Settings::IsDebugModeEnabled() const
{
  return Config::Get(Config::MAIN_ENABLE_DEBUGGING);
}

bool Settings::IsRegistersVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showregisters"));
}

void Settings::SetRegistersVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showregisters"), enabled))
    emit RegistersVisibilityChanged(enabled);
}

bool Settings::IsThreadsVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showthreads"));
}

void Settings::SetThreadsVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showthreads"), enabled))
    emit ThreadsVisibilityChanged(enabled);
}

bool Settings::IsWatchVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showwatch"));
}

void Settings::SetWatchVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showwatch"), enabled))
    emit WatchVisibilityChanged(enabled);
}

bool Settings::IsBreakpointsVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showbreakpoints"));
}

void Settings::SetBreakpointsVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showbreakpoints"), enabled))
    emit BreakpointsVisibilityChanged(enabled);
}

bool Settings::IsCodeVisible() const
{
  // The code view is the one debugger panel shown by default.
  return ReadFlag(QStringLiteral("debugger/showcode"), true);
}

void Settings::SetCodeVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showcode"), enabled, true))
    emit CodeVisibilityChanged(enabled);
}

bool Settings::IsMemoryVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showmemory"));
}

void Settings::SetMemoryVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showmemory"), enabled))
    emit MemoryVisibilityChanged(enabled);
}

bool Settings::IsNetworkVisible() const
{
  return ReadFlag(QStringLiteral("debugger/shownetwork"));
}

void Settings::SetNetworkVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/shownetwork"), enabled))
    emit NetworkVisibilityChanged(enabled);
}

bool Settings::IsJITVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showjit"));
}

void Settings::SetJITVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showjit"), enabled))
    emit JITVisibilityChanged(enabled);
}

bool Settings::IsAssemblerVisible() const
{
  return ReadFlag(QStringLiteral("debugger/showassembler"));
}

void Settings::SetAssemblerVisible(bool enabled)
{
  if (WriteFlag(QStringLiteral("debugger/showassembler"), enabled))
    emit AssemblerVisibilityChanged(enabled);
}

QFont Settings::GetDebugFont() const
{
  // Default: the system's fixed-width family at 9 points.
  QFont fallback(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  fallback.setPointSizeF(9.0);
  return GetQSettings().value(QStringLiteral("debugger/font"), fallback).value<QFont>();
}

void Settings::SetDebugFont(const QFont& font)
{
  if (font == GetDebugFont())
    return;
  GetQSettings().setValue(QStringLiteral("debugger/font"), font);
  emit DebugFontChanged(font);
}

// This one setting lives in the application's default QSettings store, not in Qt.ini.
bool Settings::IsShowDemangledNames() const
{
  return QSettings().value(QStringLiteral("debugger/showdemanglednames")).toBool();
}

void Settings::SetShowDemangledNames(bool enabled)
{
  if (enabled == IsShowDemangledNames())
    return;
  QSettings().setValue(QStringLiteral("debugger/showdemanglednames"), enabled);
  emit ShowDemangledNamesChanged(enabled);
}

// --- Updates, region, analytics ---------------------------------------------------------------

QString Settings::GetAutoUpdateTrack() const
{
  return QString::fromStdString(Config::Get(Config::MAIN_AUTOUPDATE_UPDATE_TRACK));
}

void Settings::SetAutoUpdateTrack(const QString& mode)
{
  if (mode == GetAutoUpdateTrack())
    return;
  Config::SetBase(Config::MAIN_AUTOUPDATE_UPDATE_TRACK, mode.toStdString());
  emit AutoUpdateTrackChanged(mode);
}

DiscIO::Region Settings::GetFallbackRegion() const
{
  return Config::Get(Config::MAIN_FALLBACK_REGION);
}

void Settings::SetFallbackRegion(const DiscIO::Region& region)
{
  if (region == GetFallbackRegion())
    return;
  Config::SetBase(Config::MAIN_FALLBACK_REGION, region);
  emit FallbackRegionChanged(region);
}

bool Settings::IsAnalyticsEnabled() const
{
  return Config::Get(Config::MAIN_ANALYTICS_ENABLED);
}

void Settings::SetAnalyticsEnabled(bool enabled)
{
  if (enabled == IsAnalyticsEnabled())
    return;
  Config::SetBase(Config::MAIN_ANALYTICS_ENABLED, enabled);
  emit AnalyticsToggled(enabled);
}
