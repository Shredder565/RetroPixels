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
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

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
}

WiimoteControllersWidget::~WiimoteControllersWidget() = default;

void WiimoteControllersWidget::BuildLayout()
{
  m_grid = new QGridLayout();
  m_group = new QGroupBox(tr("Wii Remotes"));
  m_group->setLayout(m_grid);

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

  m_continuous_scanning = new QCheckBox(tr("Continuous Scanning"));
  m_balance_board = new QCheckBox(tr("Real Balance Board"));
  m_speaker_data = new QCheckBox(tr("Enable Speaker Data"));
  m_for_ciface = new QCheckBox(tr("Connect Wii Remotes for Emulated Controllers"));

  m_grid->setVerticalSpacing(7);
  m_grid->setColumnStretch(2, 1);

  const auto next_row = [this] { return m_grid->rowCount(); };

  int row;

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
  connect(m_for_ciface, &QCheckBox::toggled, this, [this] {
    StoreAndReload();
    WiimoteReal::HandleWiimotesInControllerInterfaceSettingChange();
  });
  connect(m_continuous_scanning, &QCheckBox::toggled, this,
          &WiimoteControllersWidget::StoreAndReload);

  connect(m_balance_board, &QCheckBox::toggled, this, &WiimoteControllersWidget::StoreConfig);
  connect(m_speaker_data, &QCheckBox::toggled, this, &WiimoteControllersWidget::StoreConfig);

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

  // Applies the continuous scanning setting.
  WiimoteReal::Initialize(::Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);

  // While a GameCube game runs, nothing Bluetooth related can be changed.
  const bool running = state != Core::State::Uninitialized;
  const bool gc_running = running && !Core::System::GetInstance().IsWii();
  const bool emulation_usable = !gc_running;
  const bool netplay = NetPlay::IsNetPlayRunning();
  const bool netplay_running = running && netplay;

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

  auto job = std::async(std::launch::async, std::move(task));

  auto* const ticker = new QTimer{this};
  connect(ticker, &QTimer::timeout, this, [this, ticker, pending = std::move(job)] {
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
