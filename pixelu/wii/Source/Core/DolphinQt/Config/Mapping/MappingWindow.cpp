// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/MappingWindow.h"

#include <string>

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScreen>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HotkeyManager.h"

#include "DolphinQt/Config/Mapping/FreeLookGeneral.h"
#include "DolphinQt/Config/Mapping/FreeLookRotation.h"
#include "DolphinQt/Config/Mapping/GBAPadEmu.h"
#include "DolphinQt/Config/Mapping/GCKeyboardEmu.h"
#include "DolphinQt/Config/Mapping/GCMicrophone.h"
#include "DolphinQt/Config/Mapping/GCPadEmu.h"
#include "DolphinQt/Config/Mapping/Hotkey3D.h"
#include "DolphinQt/Config/Mapping/HotkeyControllerProfile.h"
#include "DolphinQt/Config/Mapping/HotkeyDebugging.h"
#include "DolphinQt/Config/Mapping/HotkeyGBA.h"
#include "DolphinQt/Config/Mapping/HotkeyGeneral.h"
#include "DolphinQt/Config/Mapping/HotkeyGraphics.h"
#include "DolphinQt/Config/Mapping/HotkeyStates.h"
#include "DolphinQt/Config/Mapping/HotkeyStatesOther.h"
#include "DolphinQt/Config/Mapping/HotkeyTAS.h"
#include "DolphinQt/Config/Mapping/HotkeyUSBEmu.h"
#include "DolphinQt/Config/Mapping/HotkeyWii.h"
#include "DolphinQt/Config/Mapping/MappingCommon.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuExtension.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuExtensionMotionInput.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuExtensionMotionSimulation.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuGeneral.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuMotionControl.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuMotionControlIMU.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WindowActivationEventFilter.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#include "DolphinQt/Settings.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/InputConfig.h"

namespace
{
// Tab positions in a Wii Remote window: General, Motion Simulation, Motion Input, Extension,
// then the two Nunchuk motion tabs while they are shown.
constexpr int EXTENSION_TAB = 3;
constexpr int NUNCHUK_SIMULATION_TAB = 4;
constexpr int NUNCHUK_INPUT_TAB = 5;

// Indicators start out ticking at 10 Hz until the first tick reads the screen refresh rate.
constexpr int FIRST_TICK_MS = 100;
}  // namespace

MappingWindow::MappingWindow(QWidget* parent, Type type, int port_num)
    : QDialog(parent), m_port(port_num)
{
  setWindowTitle(tr("Port %1").arg(port_num + 1));

  QGroupBox* const devices = BuildDeviceBox();
  QGroupBox* const profiles = BuildProfileBox();
  QGroupBox* const reset = BuildResetBox();
  BuildWindow(devices, reset, profiles);
  WireUp();
  AddPages(type);

  StartIndicatorTimer();

  {
    const auto state_lock = GetController()->GetStateLock();
    emit ConfigChanged();
  }

  // Hotkeys stay off while this window has the focus, so mapping a key does not trigger one.
  auto* const activation = new WindowActivationEventFilter(this);
  installEventFilter(activation);
  connect(activation, &WindowActivationEventFilter::windowDeactivated, activation,
          [] { HotkeyManagerEmu::Enable(true); });
  connect(activation, &WindowActivationEventFilter::windowActivated, activation,
          [] { HotkeyManagerEmu::Enable(false); });

  MappingCommon::CreateMappingProcessor(this);

  QtUtils::AdjustSizeWithinScreen(this);
}

void MappingWindow::StartIndicatorTimer()
{
  auto* const tick = new QTimer(this);
  connect(tick, &QTimer::timeout, this, [this, tick] {
    // Follow the refresh rate of whichever screen the window is on.
    const double hz = screen()->refreshRate();
    tick->setInterval(static_cast<int>(1000 / hz));

    const auto state_lock = GetController()->GetStateLock();
    emit Update();
  });
  tick->start(FIRST_TICK_MS);
}

QGroupBox* MappingWindow::BuildDeviceBox()
{
  auto* const row = new QHBoxLayout();
  auto* const box = new QGroupBox(tr("Device"));
  m_devices = new QComboBox();

  // "Refresh" is the button's own action; the other entries are in its drop-down menu.
  auto* const menu_button = new QToolButton();
  menu_button->setPopupMode(QToolButton::ToolButtonPopupMode::MenuButtonPopup);

  auto* const refresh = new QAction(tr("Refresh"), menu_button);
  connect(refresh, &QAction::triggered, this, [] { g_controller_interface.RefreshDevices(); });

  const auto add_option = [menu_button](const QString& text) {
    auto* const option = new QAction(text, menu_button);
    option->setCheckable(true);
    return option;
  };
  m_map_other_devices = add_option(tr("Create Mappings for Other Devices"));
  m_wait_for_alternates = add_option(tr("Wait for Alternate Input Mappings"));
  m_iterative_mapping = add_option(tr("Enable Iterative Input Mapping"));

  for (QAction* const action :
       {refresh, m_map_other_devices, m_wait_for_alternates, m_iterative_mapping})
  {
    menu_button->addAction(action);
  }
  menu_button->setDefaultAction(refresh);

  m_devices->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
  menu_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  row->addWidget(m_devices);
  row->addWidget(menu_button);
  box->setLayout(row);
  return box;
}

QGroupBox* MappingWindow::BuildProfileBox()
{
  auto* const row = new QHBoxLayout();
  auto* const box = new QGroupBox(tr("Profile"));
  m_profiles = new QComboBox();
  m_load_profile = new NonDefaultQPushButton(tr("Load"));
  m_save_profile = new NonDefaultQPushButton(tr("Save"));

  // A plain arrow button that opens a menu with the less common actions.
  auto* const more = new QToolButton();
  more->setPopupMode(QToolButton::InstantPopup);
  more->setArrowType(Qt::DownArrow);
  // Hide the extra menu indicator; the arrow already says it is a menu.
  more->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; }"));
  m_delete_profile = new QAction(tr("Delete"), this);
  m_open_profile_folder = new QAction(tr("Open Folder"), this);
  more->addAction(m_delete_profile);
  more->addAction(m_open_profile_folder);

  // The combo box takes typed names for new profiles.
  m_profiles->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_profiles->setMinimumWidth(100);
  m_profiles->setEditable(true);

  auto* const buttons = new QHBoxLayout();
  row->addWidget(m_profiles);
  buttons->addWidget(m_load_profile);
  buttons->addWidget(m_save_profile);
  buttons->addWidget(more);
  row->addLayout(buttons);

  box->setLayout(row);
  return box;
}

QGroupBox* MappingWindow::BuildResetBox()
{
  auto* const row = new QHBoxLayout();
  auto* const box = new QGroupBox(tr("Reset"));
  m_reset_clear = new NonDefaultQPushButton(tr("Clear"));
  m_reset_defaults = new NonDefaultQPushButton(tr("Default"));

  box->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  row->addWidget(m_reset_defaults);
  row->addWidget(m_reset_clear);
  box->setLayout(row);
  return box;
}

void MappingWindow::BuildWindow(QGroupBox* devices, QGroupBox* reset, QGroupBox* profiles)
{
  auto* const column = new QVBoxLayout();
  auto* const top_row = new QHBoxLayout();
  m_tabs = new QTabWidget();
  auto* const close_box = new QDialogButtonBox(QDialogButtonBox::Close);

  m_tabs->setTabBarAutoHide(true);

  top_row->addWidget(devices);
  top_row->addWidget(reset);
  top_row->addWidget(profiles);

  column->addLayout(top_row);
  column->addWidget(m_tabs);
  column->addWidget(close_box);
  setLayout(column);

  connect(close_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void MappingWindow::WireUp()
{
  // Note: this ConfigChanged connection is made before any page connects to the signal, so
  // the device list is refreshed before the pages are.
  connect(&Settings::Instance(), &Settings::DevicesChanged, this, &MappingWindow::ConfigChanged);
  connect(this, &MappingWindow::ConfigChanged, this, &MappingWindow::FillDeviceList);
  connect(m_devices, &QComboBox::currentIndexChanged, this, &MappingWindow::DeviceChosen);

  connect(m_reset_clear, &QPushButton::clicked, this, &MappingWindow::ClearMappings);
  connect(m_reset_defaults, &QPushButton::clicked, this, &MappingWindow::ResetToDefaults);
  connect(m_save_profile, &QPushButton::clicked, this, &MappingWindow::SaveProfile);
  connect(m_load_profile, &QPushButton::clicked, this, &MappingWindow::LoadProfile);
  connect(m_delete_profile, &QAction::triggered, this, &MappingWindow::DeleteProfile);
  connect(m_open_profile_folder, &QAction::triggered, this, &MappingWindow::OpenProfileFolder);

  connect(m_profiles, &QComboBox::currentIndexChanged, this, &MappingWindow::RefreshProfileButtons);
  connect(m_profiles, &QComboBox::editTextChanged, this, &MappingWindow::RefreshProfileButtons);

  // The window has only a "Close" button, and closing it keeps the changes: save on reject.
  connect(this, &QDialog::rejected, this, [this] { emit Save(); });

  // Switching tabs abandons any input detection in progress.
  connect(m_tabs, &QTabWidget::currentChanged, this, &MappingWindow::CancelMapping);
}

QString MappingWindow::MotionSimulationTabTitle() const
{
  return tr("Extension Motion Simulation");
}

QString MappingWindow::MotionInputTabTitle() const
{
  return tr("Extension Motion Input");
}

QWidget* MappingWindow::AddPage(const QString& title, QWidget* page)
{
  // Pages scroll when they do not fit on the screen.
  QWidget* const scroller = GetWrappedWidget(page);
  m_tabs->addTab(scroller, title);
  return scroller;
}

void MappingWindow::AddPages(Type type)
{
  // The page whose configuration (and controller) the window edits.
  MappingWidget* main_page = nullptr;
  const int port_number = GetPort() + 1;

  switch (type)
  {
  case Type::MAPPING_GC_GBA:
    main_page = new GBAPadEmu(this);
    setWindowTitle(tr("Game Boy Advance at Port %1").arg(port_number));
    AddPage(tr("Game Boy Advance"), main_page);
    break;

  case Type::MAPPING_GC_KEYBOARD:
    main_page = new GCKeyboardEmu(this);
    setWindowTitle(tr("GameCube Keyboard at Port %1").arg(port_number));
    AddPage(tr("GameCube Keyboard"), main_page);
    break;

  // Bongos, wheels and dance mats are configured as standard controllers.
  case Type::MAPPING_GC_BONGOS:
  case Type::MAPPING_GC_STEERINGWHEEL:
  case Type::MAPPING_GC_DANCEMAT:
  case Type::MAPPING_GCPAD:
    main_page = CreateStandardControllerMappingWidget(this);
    setWindowTitle(tr("GameCube Controller at Port %1").arg(port_number));
    AddPage(tr("GameCube Controller"), main_page);
    break;

  case Type::MAPPING_GC_MICROPHONE:
    main_page = new GCMicrophone(this);
    // Microphones sit in memory card slot A or B rather than in a port.
    setWindowTitle(tr("GameCube Microphone Slot %1")
                       .arg(GetPort() == 0 ? QLatin1Char{'A'} : QLatin1Char{'B'}));
    AddPage(tr("Microphone"), main_page);
    break;

  case Type::MAPPING_WIIMOTE_EMU:
  {
    // The extension pages exist before the general page, which switches between them.
    auto* const extension = new WiimoteEmuExtension(this);
    auto* const nunchuk_input = new WiimoteEmuExtensionMotionInput(this);
    auto* const nunchuk_simulation = new WiimoteEmuExtensionMotionSimulation(this);
    main_page = new WiimoteEmuGeneral(this, extension);
    setWindowTitle(tr("Wii Remote %1").arg(port_number));
    AddPage(tr("General and Options"), main_page);
    AddPage(tr("Motion Simulation"), new WiimoteEmuMotionControl(this));
    AddPage(tr("Motion Input"), new WiimoteEmuMotionControlIMU(this));
    AddPage(tr("Extension"), extension);
    m_motion_simulation_tab = AddPage(MotionSimulationTabTitle(), nunchuk_simulation);
    m_motion_input_tab = AddPage(MotionInputTabTitle(), nunchuk_input);
    // Hidden until a Nunchuk gets selected.
    ShowExtensionMotionTabs(false);
    break;
  }

  case Type::MAPPING_HOTKEYS:
    main_page = new HotkeyGeneral(this);
    AddPage(tr("General"), main_page);
    // i18n: TAS is short for tool-assisted speedrun. Read http://tasvideos.org/ for details.
    // Frame advance is an example of a typical TAS tool.
    AddPage(tr("TAS Tools"), new HotkeyTAS(this));
    AddPage(tr("Debugging"), new HotkeyDebugging(this));
    AddPage(tr("Wii and Wii Remote"), new HotkeyWii(this));
    AddPage(tr("Controller Profile"), new HotkeyControllerProfile(this));
    AddPage(tr("Graphics"), new HotkeyGraphics(this));
    AddPage(tr("USB Emulation"), new HotkeyUSBEmu(this));
    // i18n: Stereoscopic 3D
    AddPage(tr("3D"), new Hotkey3D(this));
    AddPage(tr("Save and Load State"), new HotkeyStates(this));
    AddPage(tr("Other State Management"), new HotkeyStatesOther(this));
    AddPage(tr("Game Boy Advance"), new HotkeyGBA(this));
    setWindowTitle(tr("Hotkey Settings"));
    break;

  case Type::MAPPING_FREELOOK:
    main_page = new FreeLookGeneral(this);
    AddPage(tr("General"), main_page);
    AddPage(tr("Rotation"), new FreeLookRotation(this));
    setWindowTitle(tr("Free Look Controller %1").arg(port_number));
    break;

  case Type::MAPPING_AM_BASEBOARD:
    main_page = CreateAMBaseboardMappingWidget(this);
    setWindowTitle(tr("Triforce Baseboard at Port %1").arg(port_number));
    AddPage(tr("Triforce Baseboard"), main_page);
    break;
  }

  if (main_page == nullptr)
    return;

  main_page->LoadSettings();
  m_config = main_page->GetConfig();
  m_controller = m_config->GetController(GetPort());

  FillProfileList();
}

int MappingWindow::GetPort() const
{
  return m_port;
}

ControllerEmu::EmulatedController* MappingWindow::GetController() const
{
  return m_controller;
}

bool MappingWindow::IsCreateOtherDeviceMappingsEnabled() const
{
  return m_map_other_devices->isChecked();
}

bool MappingWindow::IsWaitForAlternateMappingsEnabled() const
{
  return m_wait_for_alternates->isChecked();
}

bool MappingWindow::IsIterativeMappingEnabled() const
{
  return m_iterative_mapping->isChecked();
}

void MappingWindow::ShowExtensionMotionTabs(bool show)
{
  if (!show)
  {
    // Highest index first, so the other index stays valid.
    m_tabs->removeTab(NUNCHUK_INPUT_TAB);
    m_tabs->removeTab(NUNCHUK_SIMULATION_TAB);
    return;
  }

  m_tabs->addTab(m_motion_simulation_tab, MotionSimulationTabTitle());
  m_tabs->addTab(m_motion_input_tab, MotionInputTabTitle());
}

void MappingWindow::ActivateExtensionTab()
{
  m_tabs->setCurrentIndex(EXTENSION_TAB);
}

void MappingWindow::FillDeviceList()
{
  const QSignalBlocker quiet(m_devices);
  m_devices->clear();

  // Every entry keeps the device string itself as its item data.
  for (const std::string& device : g_controller_interface.GetAllDeviceStrings())
  {
    const QString name = QString::fromStdString(device);
    m_devices->addItem(name, name);
  }

  const std::string selected = m_controller->GetDefaultDevice().ToString();
  if (selected.empty())
    return;

  const QString selected_name = QString::fromStdString(selected);
  const int found = m_devices->findText(selected_name);
  if (found != -1)
  {
    m_devices->setCurrentIndex(found);
    return;
  }

  // The configured device is not connected right now: list it after a separator.
  m_devices->insertSeparator(m_devices->count());
  m_devices->addItem(QLatin1Char{'['} + tr("disconnected") + QStringLiteral("] ") + selected_name,
                     selected_name);
  m_devices->setCurrentIndex(m_devices->count() - 1);
}

void MappingWindow::DeviceChosen()
{
  const std::string device = m_devices->currentData().toString().toStdString();
  m_controller->SetDefaultDevice(device);

  emit ConfigChanged();
  m_controller->UpdateReferences(g_controller_interface);
}

void MappingWindow::FillProfileList()
{
  m_profiles->clear();

  // The item data of each entry is the profile's full path.
  const auto add_profiles = [this](const std::string& directory, bool stock) {
    for (const std::string& path : Common::DoFileSearch(directory, ".ini"))
    {
      std::string name;
      SplitPath(path, nullptr, &name, nullptr);
      // A file called just ".ini" would have no name to show or type in.
      if (name.empty())
        continue;

      const QString shown = QString::fromStdString(name);
      // i18n: "Stock" refers to input profiles included with Dolphin
      m_profiles->addItem(stock ? tr("%1 (Stock)").arg(shown) : shown,
                          QString::fromStdString(path));
    }
  };

  // The user's own profiles, a separator, then the ones that ship with the emulator.
  add_profiles(m_config->GetUserProfileDirectoryPath(), false);
  m_profiles->insertSeparator(m_profiles->count());
  add_profiles(m_config->GetSysProfileDirectoryPath(), true);

  m_profiles->setCurrentIndex(-1);
}

void MappingWindow::MatchTypedProfileName()
{
  // A typed name only updates the edit text: select the matching entry, if there is one, so
  // currentIndex() and currentData() describe it.
  const QString typed = m_profiles->currentText();
  const int match = m_profiles->findText(typed);
  m_profiles->setCurrentIndex(match);
  if (match == -1)
    m_profiles->setCurrentText(typed);
}

void MappingWindow::RefreshProfileButtons()
{
  // Stock profiles (under the Sys directory) can be neither overwritten nor deleted.
  bool stock = false;
  if (m_profiles->findText(m_profiles->currentText()) != -1)
  {
    const std::string sys_directory = ReplaceAll(File::GetSysDirectory(), "\\", DIR_SEP);
    stock = m_profiles->currentData().toString().startsWith(QString::fromStdString(sys_directory));
  }

  m_save_profile->setEnabled(!stock);
  m_delete_profile->setEnabled(!stock);
}

void MappingWindow::ShowMissingProfileError(const QString& name)
{
  ModalMessageBox error(this);
  error.setIcon(QMessageBox::Critical);
  error.setWindowTitle(tr("Error"));
  error.setText(tr("The profile '%1' does not exist").arg(name));
  error.exec();
}

void MappingWindow::DeleteProfile()
{
  MatchTypedProfileName();

  const QString name = m_profiles->currentText();
  const QString path = m_profiles->currentData().toString();

  if (m_profiles->currentIndex() == -1 || !File::Exists(path.toStdString()))
  {
    ShowMissingProfileError(name);
    return;
  }

  ModalMessageBox confirm(this);
  confirm.setIcon(QMessageBox::Warning);
  confirm.setWindowTitle(tr("Confirm"));
  confirm.setText(tr("Are you sure that you want to delete '%1'?").arg(name));
  confirm.setInformativeText(tr("This cannot be undone!"));
  confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
  if (confirm.exec() != QMessageBox::Yes)
    return;

  m_profiles->removeItem(m_profiles->currentIndex());
  m_profiles->setCurrentIndex(-1);

  File::Delete(path.toStdString());
}

void MappingWindow::LoadProfile()
{
  MatchTypedProfileName();

  if (m_profiles->currentIndex() == -1)
  {
    ShowMissingProfileError(m_profiles->currentText());
    return;
  }

  Common::IniFile profile;
  profile.Load(m_profiles->currentData().toString().toStdString());
  m_controller->LoadConfig(profile.GetOrCreateSection("Profile"));

  m_controller->UpdateReferences(g_controller_interface);
  m_controller->GetConfig()->GenerateControllerTextures();

  const auto state_lock = GetController()->GetStateLock();
  emit ConfigChanged();
}

void MappingWindow::SaveProfile()
{
  const QString name = m_profiles->currentText();
  if (name.isEmpty())
    return;

  const std::string path = m_config->GetUserProfileDirectoryPath() + name.toStdString() + ".ini";
  File::CreateFullPath(path);

  Common::IniFile profile;
  m_controller->SaveConfig(profile.GetOrCreateSection("Profile"));
  profile.Save(path);

  // A new profile: rebuild the list and select it.
  if (m_profiles->findText(name) == -1)
  {
    FillProfileList();
    m_profiles->setCurrentIndex(m_profiles->findText(name));
  }
}

void MappingWindow::OpenProfileFolder()
{
  const std::string folder = m_config->GetUserProfileDirectoryPath();
  File::CreateDirs(folder);
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder)));
}

void MappingWindow::ReloadMappings()
{
  m_controller->UpdateReferences(g_controller_interface);
  m_controller->GetConfig()->GenerateControllerTextures();

  const auto state_lock = GetController()->GetStateLock();
  emit ConfigChanged();
  emit Save();
}

void MappingWindow::ResetToDefaults()
{
  m_controller->LoadDefaults(g_controller_interface);
  ReloadMappings();
}

void MappingWindow::ClearMappings()
{
  // Loading an empty section clears every mapping and setting; the device is kept.
  const auto device = m_controller->GetDefaultDevice();
  Common::IniFile::Section empty;
  m_controller->LoadConfig(&empty);
  m_controller->SetDefaultDevice(device);

  ReloadMappings();
}
