// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/WiimoteControllersWidget.h"

#include <chrono>
#include <future>
#include <optional>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QStyleOptionButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/WorkQueueThread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "Core/USBUtils.h"
#include "Core/WiiUtils.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/USBDevicePicker.h"

#if defined(_WIN32)
#include "Core/HW/WiimoteReal/IOWin.h"
#endif

namespace
{
// Slot source menu rows, matching the WiimoteSource values.
constexpr int SOURCE_EMULATED = 1;

Core::State CurrentState()
{
  return Core::GetState(Core::System::GetInstance());
}

// Width of a radio button's indicator plus its gap, so the rows under each
// radio button line up with its text.
int RadioIndent()
{
  const QStyle* const style = QApplication::style();
  QStyleOptionButton option;
  // The macOS style reports the contents rect differently.
  if (style->objectName() == QStringLiteral("macintosh"))
    return style->subElementRect(QStyle::SE_RadioButtonIndicator, &option).width();
  return style->subElementRect(QStyle::SE_RadioButtonContents, &option).left();
}

// The grid's real horizontal spacing: an unset spacing reports -1, so fall
// back to the style's value, then to Qt's built-in default of 6 pixels.
int GridHorizontalGap(const QGridLayout* grid)
{
  const int own = grid->horizontalSpacing();
  if (own >= 0)
    return own;

  const int from_style =
      grid->parentWidget()->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing);
  if (from_style >= 0)
    return from_style;

  const float ratio = static_cast<float>(QGuiApplication::primaryScreen()->devicePixelRatio());
  return static_cast<int>(ratio * 6);
}
}  // namespace

WiimoteControllersWidget::WiimoteControllersWidget(QWidget* parent) : QWidget(parent)
{
  BuildLayout();
  WireUp();

  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          [this] { ShowConfig(CurrentState()); });
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          [this](Core::State state) { ShowConfig(state); });
  ShowConfig(CurrentState());

  m_scan_thread.Reset("Bluetooth Adapter Refresh Thread");
  ScanForAdapters();
}

WiimoteControllersWidget::~WiimoteControllersWidget()
{
  m_scan_thread.WaitForCompletion();
}

void WiimoteControllersWidget::BuildLayout()
{
  m_grid = new QGridLayout();
  m_group = new QGroupBox(tr("Wii Remotes"));
  m_group->setLayout(m_grid);

  m_passthrough_bt = new QRadioButton(tr("Passthrough a Bluetooth adapter"));
  m_adapter_caption = new QLabel(tr("Adapter"));
  m_adapters = new QComboBox();
  m_rescan_adapters = new NonDefaultQPushButton(tr("Refresh"));
  m_sync = new NonDefaultQPushButton(tr("Sync"));
  m_reset = new NonDefaultQPushButton(tr("Reset"));

  // The refresh button's menu holds the Windows-only host pairing actions;
  // the spinner beside it turns while one of them runs.
  m_spinner = new QLabel{};
  m_spinner->hide();
  m_refresh = new QToolButton();
  auto* const refresh_action = new QAction(tr("Refresh"), m_refresh);
  m_refresh->setDefaultAction(refresh_action);
  connect(refresh_action, &QAction::triggered, this,
          &WiimoteControllersWidget::RefreshRealRemotes);
  m_refresh->setPopupMode(QToolButton::ToolButtonPopupMode::MenuButtonPopup);

#if defined(_WIN32)
  m_spinner->setPixmap(style()->standardIcon(QStyle::SP_BrowserReload).pixmap(16, 16));

  auto* const pair_action = new QAction(tr("Sync"), m_refresh);
  m_refresh->addAction(pair_action);
  connect(pair_action, &QAction::triggered, this, &WiimoteControllersWidget::PairHostRemotes);

  auto* const forget_action = new QAction(tr("Reset"), m_refresh);
  m_refresh->addAction(forget_action);
  connect(forget_action, &QAction::triggered, this, &WiimoteControllersWidget::ForgetHostRemotes);
#endif

  m_passthrough_captions[0] = new QLabel(tr("Sync real Wii Remotes and pair them"));
  m_passthrough_captions[1] = new QLabel(tr("Reset all saved Wii Remote pairings"));
  m_emulate_bt = new QRadioButton(tr("Emulate the Wii's Bluetooth adapter"));
  m_continuous_scanning = new QCheckBox(tr("Continuous Scanning"));
  m_balance_board = new QCheckBox(tr("Real Balance Board"));
  m_speaker_data = new QCheckBox(tr("Enable Speaker Data"));
  m_for_ciface = new QCheckBox(tr("Connect Wii Remotes for Emulated Controllers"));

  // Column 0 is only as wide as a radio indicator, so columns 1.. line up
  // with the radio buttons' text.
  m_grid->setVerticalSpacing(7);
  m_grid->setColumnMinimumWidth(0, RadioIndent() - GridHorizontalGap(m_grid));
  m_grid->setColumnStretch(2, 1);

  const auto next_row = [this] { return m_grid->rowCount(); };

  m_grid->addWidget(m_passthrough_bt, next_row(), 0, 1, -1);

  int row = next_row();
  m_grid->addWidget(m_adapter_caption, row, 1, 1, 1);
  m_grid->addWidget(m_adapters, row, 2, 1, 1);
  m_grid->addWidget(m_rescan_adapters, row, 3, 1, 1);

  row = next_row();
  m_grid->addWidget(m_passthrough_captions[0], row, 1, 1, 2);
  m_grid->addWidget(m_sync, row, 3);

  row = next_row();
  m_grid->addWidget(m_passthrough_captions[1], row, 1, 1, 2);
  m_grid->addWidget(m_reset, row, 3);

  m_grid->addWidget(m_emulate_bt, next_row(), 0, 1, -1);

  for (size_t slot = 0; slot < SLOTS; ++slot)
  {
    m_slot_labels[slot] = new QLabel(tr("Wii Remote %1").arg(slot + 1));
    m_slot_sources[slot] = new QComboBox();
    m_slot_configure[slot] = new NonDefaultQPushButton(tr("Configure"));
    for (const QString& source : {tr("None"), tr("Emulated Wii Remote"), tr("Real Wii Remote")})
      m_slot_sources[slot]->addItem(source);

    row = next_row();
    m_grid->addWidget(m_slot_labels[slot], row, 1);
    m_grid->addWidget(m_slot_sources[slot], row, 2);
    m_grid->addWidget(m_slot_configure[slot], row, 3);
  }

  m_grid->addWidget(m_balance_board, next_row(), 1, 1, -1);
  m_grid->addWidget(m_speaker_data, next_row(), 1, 1, -1);
  m_grid->addWidget(m_for_ciface, next_row(), 0, 1, -1);

  row = next_row();
  auto* const scanning_row = new QHBoxLayout;
  scanning_row->addWidget(m_continuous_scanning);
  scanning_row->addStretch(1);
  scanning_row->addWidget(m_spinner);
  m_grid->addLayout(scanning_row, row, 0, 1, 3);
  m_grid->addWidget(m_refresh, row, 3);

  auto* const column = new QVBoxLayout;
  column->setContentsMargins(0, 0, 0, 0);
  column->setAlignment(Qt::AlignTop);
  column->addWidget(m_group);
  setLayout(column);
}

void WiimoteControllersWidget::WireUp()
{
  connect(m_passthrough_bt, &QRadioButton::toggled, this,
          &WiimoteControllersWidget::StoreAndReload);
  connect(m_for_ciface, &QCheckBox::toggled, this, [this] {
    StoreAndReload();
    WiimoteReal::HandleWiimotesInControllerInterfaceSettingChange();
  });
  connect(m_continuous_scanning, &QCheckBox::toggled, this,
          &WiimoteControllersWidget::StoreAndReload);

  connect(m_balance_board, &QCheckBox::toggled, this, &WiimoteControllersWidget::StoreConfig);
  connect(m_speaker_data, &QCheckBox::toggled, this, &WiimoteControllersWidget::StoreConfig);
  connect(m_adapters, &QComboBox::activated, this, &WiimoteControllersWidget::OnAdapterPicked);
  connect(m_rescan_adapters, &QPushButton::clicked, this,
          &WiimoteControllersWidget::ScanForAdapters);
  connect(m_sync, &QPushButton::clicked, this, &WiimoteControllersWidget::SyncPassthroughRemotes);
  connect(m_reset, &QPushButton::clicked, this,
          &WiimoteControllersWidget::ResetPassthroughPairings);

  for (size_t slot = 0; slot < SLOTS; ++slot)
  {
    connect(m_slot_sources[slot], &QComboBox::currentIndexChanged, this,
            [this] { StoreAndReload(); });
    connect(m_slot_configure[slot], &QPushButton::clicked, this,
            [this, slot] { ConfigureSlot(slot); });
  }
}

void WiimoteControllersWidget::StoreAndReload()
{
  StoreConfig();
  ShowConfig(CurrentState());
}

// Lists Bluetooth adapters on the worker thread; the menu shows a
// placeholder until the result comes back.
void WiimoteControllersWidget::ScanForAdapters()
{
#ifdef __LIBUSB__
  if (m_scanning)
    return;

  m_adapters->clear();
  m_adapters->setDisabled(true);
  m_adapters->addItem(tr("Scanning for adapters..."));
  m_scanning = true;

  m_scan_thread.Push([this] {
    INFO_LOG_FMT(COMMON, "Refreshing Bluetooth adapter list...");
    std::vector<USBUtils::DeviceInfo> found =
        USBUtils::ListDevices(LibUSBBluetoothAdapter::IsBluetoothDevice);
    INFO_LOG_FMT(COMMON, "{} Bluetooth adapters available.", found.size());
    QueueOnObject(this, [this, adapters = std::move(found)] { OnAdaptersScanned(adapters); });
  });
#endif
}

// Menu layout: "Automatic", the adapters found, then (if the configured
// adapter wasn't found) a separator and that adapter marked disconnected,
// then a separator and "More Options...".
void WiimoteControllersWidget::OnAdaptersScanned(std::span<const USBUtils::DeviceInfo> devices)
{
  const int wanted_vid = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID);
  const int wanted_pid = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID);
  // -1 means no adapter is configured, so there is nothing to look for.
  bool selected = wanted_vid == -1 || wanted_pid == -1;

  m_adapters->clear();
  m_scanning = false;
  RefreshAdapterControls(CurrentState());

  m_adapters->addItem(tr("Automatic"));

  for (const USBUtils::DeviceInfo& device : devices)
  {
    m_adapters->addItem(QString::fromStdString(device.ToDisplayString()),
                        QVariant::fromValue(device));
    if (selected || !LibUSBBluetoothAdapter::IsConfiguredBluetoothDevice(device.vid, device.pid))
      continue;
    selected = true;
    m_adapters->setCurrentIndex(m_adapters->count() - 1);
  }

  if (!selected)
  {
    const QString marker = QLatin1Char{'['} + tr("disconnected") + QLatin1Char(']');
    USBUtils::DeviceInfo missing;
    missing.vid = static_cast<u16>(wanted_vid);
    missing.pid = static_cast<u16>(wanted_pid);

    m_adapters->insertSeparator(m_adapters->count());
    m_adapters->addItem(QString::fromStdString(missing.ToDisplayString(marker.toStdString())),
                        QVariant::fromValue(missing));
    m_adapters->setCurrentIndex(m_adapters->count() - 1);
  }

  m_adapters->insertSeparator(m_adapters->count());
  m_adapters->addItem(tr("More Options..."));
}

void WiimoteControllersWidget::OnAdapterPicked(int row)
{
  // "Automatic" forgets the configured adapter.
  if (row == 0)
  {
    Config::DeleteKey(Config::GetActiveLayerForConfig(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID),
                      Config::MAIN_BLUETOOTH_PASSTHROUGH_PID);
    Config::DeleteKey(Config::GetActiveLayerForConfig(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID),
                      Config::MAIN_BLUETOOTH_PASSTHROUGH_VID);
    return;
  }

  std::optional<USBUtils::DeviceInfo> choice;
  const bool from_picker = row == m_adapters->count() - 1;
  if (from_picker)
  {
    // "More Options..." lets any USB device be chosen.
    choice = USBDevicePicker::Run(this, tr("Select a Bluetooth Device"));
  }
  else
  {
    const QVariant stored = m_adapters->itemData(row);
    if (!stored.isValid() || !stored.canConvert<USBUtils::DeviceInfo>())
    {
      ERROR_LOG_FMT(COMMON, "Invalid Bluetooth device info selected in WiimoteControllersWidget");
      return;
    }
    choice = stored.value<USBUtils::DeviceInfo>();
  }

  if (choice)
  {
    Config::SetBaseOrCurrent(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID, choice->pid);
    Config::SetBaseOrCurrent(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID, choice->vid);
  }
  // The picked device may not be in the menu yet.
  if (from_picker)
    ScanForAdapters();
}

void WiimoteControllersWidget::ResetPassthroughPairings()
{
  if (!Core::System::GetInstance().GetIOS())
  {
    ModalMessageBox::warning(
        this, tr("Warning"),
        tr("Saved Wii Remote pairings can only be reset when a Wii game is running."));
    return;
  }

  // Holding the adapter's sync button clears its pairings.
  if (const auto adapter = WiiUtils::GetBluetoothRealDevice())
    adapter->TriggerSyncButtonHeldEvent();
}

void WiimoteControllersWidget::SyncPassthroughRemotes()
{
  if (!Core::System::GetInstance().GetIOS())
  {
    ModalMessageBox::warning(this, tr("Warning"),
                             tr("A sync can only be triggered when a Wii game is running."));
    return;
  }

  if (const auto adapter = WiiUtils::GetBluetoothRealDevice())
    adapter->TriggerSyncButtonPressedEvent();
}

void WiimoteControllersWidget::RefreshRealRemotes()
{
  WiimoteReal::Refresh();
}

void WiimoteControllersWidget::ConfigureSlot(size_t slot)
{
  // Only emulated remotes have mappings.
  if (m_slot_sources[slot]->currentIndex() != SOURCE_EMULATED)
    return;

  auto* const window =
      new MappingWindow(this, MappingWindow::Type::MAPPING_WIIMOTE_EMU, static_cast<int>(slot));
  window->setAttribute(Qt::WA_DeleteOnClose, true);
  window->setWindowModality(Qt::WindowModality::WindowModal);
  window->show();
}

void WiimoteControllersWidget::RefreshAdapterControls(const Core::State state)
{
  // The adapter can't change under a running Wii game, nor be picked while
  // a scan is running.
  const bool running = state != Core::State::Uninitialized;
  const bool wii_running = running && Core::System::GetInstance().IsWii();
  const bool can_rescan = m_passthrough_bt->isChecked() && !wii_running;
  const bool can_pick = can_rescan && !m_scanning;

  m_adapter_caption->setEnabled(can_pick);
  m_adapters->setEnabled(can_pick);
  m_rescan_adapters->setEnabled(can_rescan);
}

void WiimoteControllersWidget::ShowConfig(Core::State state)
{
  for (size_t slot = 0; slot < SLOTS; ++slot)
  {
    const WiimoteSource source = Config::Get(Config::GetInfoForWiimoteSource(static_cast<int>(slot)));
    SignalBlocking(m_slot_sources[slot])->setCurrentIndex(static_cast<int>(source));
  }
  SignalBlocking(m_balance_board)
      ->setChecked(Config::Get(Config::WIIMOTE_BB_SOURCE) == WiimoteSource::Real);
  SignalBlocking(m_speaker_data)->setChecked(Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER));
  SignalBlocking(m_for_ciface)
      ->setChecked(Config::Get(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE));
  SignalBlocking(m_continuous_scanning)
      ->setChecked(Config::Get(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING));

  QRadioButton* const mode =
      Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED) ? m_passthrough_bt : m_emulate_bt;
  SignalBlocking(mode)->setChecked(true);

  // Applies the continuous scanning setting.
  WiimoteReal::Initialize(::Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);

  const bool running = state != Core::State::Uninitialized;
  m_emulate_bt->setEnabled(!running);
  m_passthrough_bt->setEnabled(!running);

  // While a GameCube game runs, nothing Bluetooth related can be changed.
  const bool gc_running = running && !Core::System::GetInstance().IsWii();
  const bool passthrough = m_passthrough_bt->isChecked();
  const bool passthrough_usable = passthrough && !gc_running;
  const bool emulation_usable = !passthrough && !gc_running;
  const bool netplay = NetPlay::IsNetPlayRunning();
  const bool netplay_running = running && netplay;

  RefreshAdapterControls(state);

  m_sync->setEnabled(passthrough_usable);
  m_reset->setEnabled(passthrough_usable);
  for (QLabel* const caption : m_passthrough_captions)
    caption->setEnabled(passthrough_usable);

  // In netplay only this player's own remotes can be configured.
  const int local_slots = netplay ? NetPlay::NumLocalWiimotes() : 4;
  for (size_t slot = 0; slot < SLOTS; ++slot)
  {
    m_slot_labels[slot]->setEnabled(emulation_usable);
    m_slot_sources[slot]->setEnabled(emulation_usable && !netplay_running);
    const bool emulated = m_slot_sources[slot]->currentIndex() == SOURCE_EMULATED;
    m_slot_configure[slot]->setEnabled(emulation_usable && emulated &&
                                       static_cast<int>(slot) < local_slots);
  }

  m_balance_board->setEnabled(emulation_usable && !netplay_running);
  m_speaker_data->setEnabled(emulation_usable && !netplay_running);

  // Real remotes are looked for when emulating or when they feed the
  // controller interface; a manual refresh is pointless while scanning.
  const bool real_remotes_used = emulation_usable || m_for_ciface->isChecked();
  m_refresh->setEnabled(real_remotes_used && !m_continuous_scanning->isChecked());
  m_continuous_scanning->setEnabled(real_remotes_used);
}

void WiimoteControllersWidget::StoreConfig()
{
  {
    const Config::ConfigChangeCallbackGuard batch;
    Config::SetBaseOrCurrent(Config::MAIN_WIIMOTE_ENABLE_SPEAKER, m_speaker_data->isChecked());
    Config::SetBaseOrCurrent(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE,
                             m_for_ciface->isChecked());
    Config::SetBaseOrCurrent(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
                             m_continuous_scanning->isChecked());
    Config::SetBaseOrCurrent(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED,
                             m_passthrough_bt->isChecked());
    Config::SetBaseOrCurrent(Config::WIIMOTE_BB_SOURCE, m_balance_board->isChecked() ?
                                                            WiimoteSource::Real :
                                                            WiimoteSource::None);
    for (size_t slot = 0; slot < SLOTS; ++slot)
    {
      Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(static_cast<int>(slot)),
                               static_cast<WiimoteSource>(m_slot_sources[slot]->currentIndex()));
    }
  }

  SConfig::GetInstance().SaveSettings();
}

#if defined(_WIN32)
// Runs a host pairing task in the background, disabling the refresh button
// and turning the spinner a quarter turn every 250 ms until it finishes.
void WiimoteControllersWidget::RunWithSpinner(std::invocable<> auto task)
{
  m_refresh->setEnabled(false);
  m_spinner->show();

  auto pending = std::async(std::launch::async, std::move(task));

  auto* const ticker = new QTimer{this};
  connect(ticker, &QTimer::timeout, this, [this, ticker, pending = std::move(pending)] {
    m_spinner->setPixmap(m_spinner->pixmap().transformed(QTransform().rotate(90)));

    if (pending.wait_for(std::chrono::seconds{}) != std::future_status::ready)
      return;

    ticker->deleteLater();
    m_spinner->hide();
    m_refresh->setEnabled(true);
  });
  ticker->start(250);
}

void WiimoteControllersWidget::PairHostRemotes()
{
  RunWithSpinner(WiimoteReal::WiimoteScannerWindows::FindAndAuthenticateWiimotes);
}

void WiimoteControllersWidget::ForgetHostRemotes()
{
  RunWithSpinner(WiimoteReal::WiimoteScannerWindows::RemoveRememberedWiimotes);
}
#endif
