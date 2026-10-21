// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/InterfacePane.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QWidget>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigRadio.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

namespace
{
// Interface languages: each language's name in that language, and its locale code.
struct InterfaceLanguage
{
  const char16_t* native_name;
  const char* code;
};

constexpr InterfaceLanguage INTERFACE_LANGUAGES[] = {
    {u"Bahasa Melayu", "ms"}, {u"Català", "ca"},
    {u"Čeština", "cs"},       {u"Dansk", "da"},
    {u"Deutsch", "de"},       {u"English", "en"},
    {u"Español", "es"},       {u"Français", "fr"},
    {u"Hrvatski", "hr"},      {u"Italiano", "it"},
    {u"Magyar", "hu"},        {u"Nederlands", "nl"},
    {u"Norsk bokmål", "nb"},  {u"Polski", "pl"},
    {u"Português", "pt"},     {u"Português (Brasil)", "pt_BR"},
    {u"Română", "ro"},        {u"Srpski", "sr"},
    {u"Suomi", "fi"},         {u"Svenska", "sv"},
    {u"Türkçe", "tr"},        {u"Ελληνικά", "el"},
    {u"Русский", "ru"},       {u"العربية", "ar"},
    {u"فارسی", "fa"},         {u"한국어", "ko"},
    {u"日本語", "ja"},        {u"简体中文", "zh_CN"},
    {u"繁體中文", "zh_TW"},
};

ConfigStringChoice* CreateLanguageChoice()
{
  // The first entry (empty code) follows the system language.
  std::vector<std::pair<QString, QString>> entries;
  entries.emplace_back(QObject::tr("<System Language>"), QString{});
  for (const InterfaceLanguage& language : INTERFACE_LANGUAGES)
    entries.emplace_back(QString::fromUtf16(language.native_name),
                         QString::fromLatin1(language.code));

  auto* const choice = new ConfigStringChoice(entries, Config::MAIN_INTERFACE_LANGUAGE);
  // Sizing to contents on first show stalls the first opening of the settings window (the
  // non-Latin names are expensive to measure); this policy is much cheaper.
  choice->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  return choice;
}

QVBoxLayout* AddGroup(QVBoxLayout* page, const QString& title)
{
  auto* const group = new QGroupBox(title);
  auto* const contents = new QVBoxLayout;
  group->setLayout(contents);
  page->addWidget(group);
  return contents;
}
}  // namespace

InterfacePane::InterfacePane(QWidget* parent) : QWidget(parent)
{
  auto* const page = new QVBoxLayout;
  BuildInterfaceGroup(page);
  BuildRenderWindowGroup(page);
  page->addStretch(1);
  setLayout(page);

  RefreshDebugToggle();
  SelectCurrentStyle();
  WireUp();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &InterfacePane::RefreshDebugToggle);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &InterfacePane::OnEmulationStateChanged);

  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

void InterfacePane::BuildInterfaceGroup(QVBoxLayout* page)
{
  QVBoxLayout* const group = AddGroup(page, tr("User Interface"));

  auto* const form = new QFormLayout;
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  group->addLayout(form);

  m_language = CreateLanguageChoice();
  form->addRow(tr("&Language:"), m_language);
  m_language->SetTitle(tr("Language"));
  m_language->SetDescription(
      tr("Sets the language displayed by Dolphin's user interface.<br><br>Changes to this "
         "setting only take effect once Dolphin is restarted.<br><br><dolphin_emphasis>If "
         "unsure, select &lt;System Language&gt;.</dolphin_emphasis>"));

  // Themes are the folders found in the user and system theme directories.
  const std::string user_themes = File::GetUserPath(D_THEMES_IDX);
  const std::string system_themes = File::GetSysDirectory() + THEMES_DIR;
  const std::array<std::string_view, 2> theme_dirs{user_themes, system_themes};
  std::vector<std::string> themes;
  for (const std::string& theme_path : Common::DoFileSearch(theme_dirs))
    themes.push_back(PathToFileName(theme_path));

  m_theme = new ConfigStringChoice(themes, Config::MAIN_THEME_NAME);
  form->addRow(tr("&Theme:"), m_theme);
  m_theme->SetTitle(tr("Theme"));
  m_theme->SetDescription(tr("Changes the appearance and color of Dolphin's "
                             "buttons.<br><br><dolphin_emphasis>If unsure, select "
                             "PixelU.</dolphin_emphasis>"));

  m_style = new ToolTipComboBox;
  m_style_label = new QLabel(tr("Style:"));
  form->addRow(m_style_label, m_style);
  FillStyleCombo();
  m_style->SetTitle(tr("Style"));
  m_style->SetDescription(
      tr("Sets the style of Dolphin's user interface. Any custom styles that you have added "
         "will be presented here, allowing you to switch to them.<br><br><dolphin_emphasis>If "
         "unsure, select (System).</dolphin_emphasis>"));

  m_builtin_titles = new ConfigBool(tr("Use Built-In Database of Game Names"),
                                    Config::MAIN_USE_BUILT_IN_TITLE_DATABASE);
  m_builtin_titles->SetDescription(
      tr("Uses Dolphin's database of properly formatted names in the game list's Title "
         "column.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_covers = new ConfigBool(tr("Download Game Covers from GameTDB.com for Use in Grid Mode"),
                            Config::MAIN_USE_GAME_COVERS);
  m_covers->SetDescription(
      tr("Downloads full game covers from GameTDB.com to display in the game list's Grid View. "
         "If this setting is unchecked, the game list displays a banner from the game's save "
         "data, and if the game has no save file, displays a generic banner "
         "instead.<br><br>List View will always use the save file "
         "banners.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_debug_ui = new ToolTipCheckBox(tr("Enable Debugging UI"));

  m_focused_hotkeys =
      new ConfigBool(tr("Hotkeys Require Window Focus"), Config::MAIN_FOCUSED_HOTKEYS);
  m_focused_hotkeys->SetDescription(
      tr("Requires the render window to be focused for hotkeys to take "
         "effect.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_inhibit_screensaver =
      new ConfigBool(tr("Inhibit Screensaver During Emulation"), Config::MAIN_DISABLE_SCREENSAVER);
  m_inhibit_screensaver->SetDescription(
      tr("Disables your screensaver while running a game.<br><br><dolphin_emphasis>If unsure, "
         "leave this checked.</dolphin_emphasis>"));

  m_play_time = new ConfigBool(tr("Enable Play Time Tracking"), Config::MAIN_TIME_TRACKING);
  m_play_time->SetDescription(
      tr("Tracks the time you spend playing games and shows it in the List View (as "
         "hours/minutes).<br><br>This setting cannot be changed while emulation is "
         "active.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  for (QWidget* const toggle :
       std::initializer_list<QWidget*>{m_builtin_titles, m_covers, m_debug_ui, m_focused_hotkeys,
                                       m_inhibit_screensaver, m_play_time})
  {
    group->addWidget(toggle);
  }
}

void InterfacePane::FillStyleCombo()
{
  const std::vector<std::string> user_styles =
      Common::DoFileSearch(File::GetUserPath(D_STYLES_IDX));

  const auto add_builtin = [this](const QString& name, Settings::StyleType type) {
    m_style->addItem(name, static_cast<int>(type));
  };
  add_builtin(tr("(System)"), Settings::StyleType::System);
  // TODO: Support forcing light/dark on other OSes too.
#ifdef _WIN32
  add_builtin(tr("(Light)"), Settings::StyleType::Light);
  add_builtin(tr("(Dark)"), Settings::StyleType::Dark);
#endif
  add_builtin(tr("(Fusion Light)"), Settings::StyleType::FusionLight);
  add_builtin(tr("(Fusion Dark Gray)"), Settings::StyleType::FusionDarkGray);
  add_builtin(tr("(Fusion Dark)"), Settings::StyleType::FusionDark);

  // User styles carry their file name (a string) instead of a style type number.
  for (const std::string& style_path : user_styles)
  {
    const QFileInfo style_file(QString::fromStdString(style_path));
    m_style->addItem(style_file.completeBaseName(), style_file.fileName());
  }
}

void InterfacePane::BuildRenderWindowGroup(QVBoxLayout* page)
{
  QVBoxLayout* const group = AddGroup(page, tr("Render Window"));

  m_keep_on_top = new ConfigBool(tr("Keep Window on Top"), Config::MAIN_KEEP_WINDOW_ON_TOP);
  m_keep_on_top->SetDescription(tr("Forces the render window to stay on top of other windows and "
                                   "applications.<br><br><dolphin_emphasis>If unsure, leave this "
                                   "unchecked.</dolphin_emphasis>"));

  m_confirm_stop = new ConfigBool(tr("Confirm on Stop"), Config::MAIN_CONFIRM_ON_STOP);
  m_confirm_stop->SetDescription(
      tr("Prompts you to confirm that you want to end emulation when you press "
         "Stop.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_panic_handlers = new ConfigBool(tr("Use Panic Handlers"), Config::MAIN_USE_PANIC_HANDLERS);
  m_panic_handlers->SetDescription(
      tr("In the event of an error, Dolphin will halt to inform you of the error and present "
         "choices on how to proceed. With this option disabled, Dolphin will \"ignore\" all "
         "errors. Emulation will not be halted and you will not be "
         "notified.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_title_in_window =
      new ConfigBool(tr("Show Active Title in Window Title"), Config::MAIN_SHOW_ACTIVE_TITLE);
  m_title_in_window->SetDescription(
      tr("Shows the active game title in the render window's title "
         "bar.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_pause_unfocused = new ConfigBool(tr("Pause on Focus Loss"), Config::MAIN_PAUSE_ON_FOCUS_LOST);
  m_pause_unfocused->SetDescription(
      tr("Pauses the game whenever the render window isn't focused.<br><br><dolphin_emphasis>If "
         "unsure, leave this unchecked.</dolphin_emphasis>"));

  auto* const cursor_box = new QGroupBox(tr("Mouse Cursor Visibility"));
  auto* const cursor_column = new QVBoxLayout;
  cursor_box->setLayout(cursor_column);

  const auto cursor_option = [](const QString& label, Config::ShowCursor mode) {
    return new ConfigRadioInt(label, Config::MAIN_SHOW_CURSOR, static_cast<int>(mode));
  };
  m_cursor_on_movement = cursor_option(tr("On Movement"), Config::ShowCursor::OnMovement);
  m_cursor_never = cursor_option(tr("Never"), Config::ShowCursor::Never);
  m_cursor_always = cursor_option(tr("Always"), Config::ShowCursor::Constantly);
  m_cursor_on_movement->SetDescription(
      tr("Shows the mouse cursor briefly whenever it has recently moved, then hides "
         "it.<br><br><dolphin_emphasis>If unsure, select this mode.</dolphin_emphasis>"));
  m_cursor_never->SetDescription(
      tr("Hides the mouse cursor whenever it is inside the render window and the render window "
         "is focused.<br><br><dolphin_emphasis>If unsure, select &quot;On "
         "Movement&quot;.</dolphin_emphasis>"));
  m_cursor_always->SetDescription(
      tr("Shows the mouse cursor at all times.<br><br><dolphin_emphasis>If unsure, select "
         "&quot;On Movement&quot;.</dolphin_emphasis>"));
  cursor_column->addWidget(m_cursor_on_movement);
  cursor_column->addWidget(m_cursor_never);
  cursor_column->addWidget(m_cursor_always);

  // Cursor locking is only offered on Windows. Owning it here keeps it from leaking elsewhere.
  m_lock_cursor = new ConfigBool(tr("Lock Mouse Cursor"), Config::MAIN_LOCK_CURSOR);
  m_lock_cursor->setParent(this);
  m_lock_cursor->SetDescription(
      tr("Locks the mouse cursor to the Render Widget as long as it has focus. You can set a "
         "hotkey to unlock it.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));

  for (QWidget* const item :
       std::initializer_list<QWidget*>{m_keep_on_top, m_confirm_stop, m_panic_handlers,
                                       m_title_in_window, m_pause_unfocused, cursor_box})
  {
    group->addWidget(item);
  }
#ifdef _WIN32
  group->addWidget(m_lock_cursor);
#else
  m_lock_cursor->hide();
#endif
}

void InterfacePane::WireUp()
{
  Settings* const settings = &Settings::Instance();

  connect(m_builtin_titles, &QCheckBox::toggled, settings, &Settings::GameListRefreshRequested);
  connect(m_covers, &QCheckBox::toggled, settings, &Settings::MetadataRefreshRequested);
  connect(m_debug_ui, &QCheckBox::toggled, settings, &Settings::SetDebugModeEnabled);
  connect(m_theme, &QComboBox::currentIndexChanged, settings, &Settings::ThemeChanged);
  connect(m_style, &QComboBox::currentIndexChanged, this, &InterfacePane::OnStyleSelected);
  connect(m_language, &QComboBox::currentIndexChanged, this, &InterfacePane::OnLanguageSelected);
  connect(m_keep_on_top, &QCheckBox::toggled, settings, &Settings::KeepWindowOnTopChanged);
  for (ConfigRadioInt* const cursor : {m_cursor_on_movement, m_cursor_never, m_cursor_always})
    connect(cursor, &ConfigRadioInt::OnSelected, settings, &Settings::CursorVisibilityChanged);
  connect(m_lock_cursor, &QCheckBox::toggled, settings, &Settings::LockCursorChanged);
}

void InterfacePane::RefreshDebugToggle()
{
  SignalBlocking(m_debug_ui)->setChecked(Settings::Instance().IsDebugModeEnabled());

  // The debugger is off-limits in hardcore mode.
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  SignalBlocking(m_debug_ui)->setEnabled(!hardcore);

  const QString about =
      tr("Shows Dolphin's debugging user interface. This lets you view and modify a game's code "
         "and memory contents, set debugging breakpoints, examine network requests, and "
         "more.<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  if (!hardcore)
  {
    m_debug_ui->SetDescription(about);
    return;
  }
  const QString blocked = tr("<dolphin_emphasis>Disabled in Hardcore Mode.</dolphin_emphasis>");
  m_debug_ui->SetDescription(tr("%1<br><br>%2").arg(about).arg(blocked));
}

void InterfacePane::SelectCurrentStyle()
{
  Settings& settings = Settings::Instance();
  const Settings::StyleType type = settings.GetStyleType();
  const QString user_style = settings.GetUserStyleName();

  // User styles are keyed by file name, built-in ones by type number.
  const int item = type == Settings::StyleType::User ? m_style->findData(user_style) :
                                                       m_style->findData(static_cast<int>(type));
  if (item > 0)
    SignalBlocking(m_style)->setCurrentIndex(item);
}

void InterfacePane::OnStyleSelected()
{
  const QVariant chosen = m_style->currentData();
  bool builtin = false;
  const int type_number = chosen.toInt(&builtin);

  Settings& settings = Settings::Instance();
  settings.SetStyleType(builtin ? static_cast<Settings::StyleType>(type_number) :
                                  Settings::StyleType::User);
  if (!builtin)
    settings.SetUserStyleName(chosen.toString());
  settings.ApplyStyle();
}

void InterfacePane::OnLanguageSelected()
{
  ModalMessageBox::information(
      this, tr("Restart Required"),
      tr("You must restart Dolphin in order for the change to take effect."));
}

void InterfacePane::OnEmulationStateChanged(Core::State state)
{
  // Play time can't be toggled mid-game.
  m_play_time->setEnabled(state == Core::State::Uninitialized);
}
