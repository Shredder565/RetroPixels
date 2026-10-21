// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GamecubeControllersWidget.h"

#include <optional>
#include <utility>

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "DolphinQt/Config/Mapping/GCPadWiiUConfigDialog.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

namespace
{
using SerialInterface::SIDevices;

struct DeviceChoice
{
  SIDevices device;
  const char* label;  // untranslated; translated when shown
};

// The port menu, in display order. A device missing here (for instance an
// emulated GBA in a build without mGBA) leaves the menu as it is.
constexpr DeviceChoice DEVICE_MENU[] = {
    {SerialInterface::SIDEVICE_NONE, _trans("None")},
    {SerialInterface::SIDEVICE_GC_CONTROLLER, _trans("Standard Controller")},
    {SerialInterface::SIDEVICE_WIIU_ADAPTER, _trans("GameCube Controller Adapter (USB)")},
    {SerialInterface::SIDEVICE_GC_STEERING, _trans("Steering Wheel")},
    {SerialInterface::SIDEVICE_DANCEMAT, _trans("Dance Mat")},
    {SerialInterface::SIDEVICE_GC_TARUKONGA, _trans("DK Bongos")},
#ifdef HAS_LIBMGBA
    {SerialInterface::SIDEVICE_GC_GBA_EMULATED, _trans("GBA (Integrated)")},
#endif
    {SerialInterface::SIDEVICE_GC_GBA, _trans("GBA (TCP)")},
    {SerialInterface::SIDEVICE_GC_KEYBOARD, _trans("Keyboard Controller")},
    {SerialInterface::SIDEVICE_AM_BASEBOARD, _trans("Triforce Baseboard")},
};

std::optional<int> MenuRowFor(SIDevices device)
{
  int row = 0;
  for (const DeviceChoice& choice : DEVICE_MENU)
  {
    if (choice.device == device)
      return row;
    ++row;
  }
  return std::nullopt;
}

SIDevices DeviceAtRow(int row)
{
  return DEVICE_MENU[row].device;
}

// The mapping window for a device, if it has one.
std::optional<MappingWindow::Type> MappingFor(SIDevices device)
{
  switch (device)
  {
  case SerialInterface::SIDEVICE_GC_CONTROLLER:
    return MappingWindow::Type::MAPPING_GCPAD;
  case SerialInterface::SIDEVICE_GC_STEERING:
    return MappingWindow::Type::MAPPING_GC_STEERINGWHEEL;
  case SerialInterface::SIDEVICE_DANCEMAT:
    return MappingWindow::Type::MAPPING_GC_DANCEMAT;
  case SerialInterface::SIDEVICE_GC_TARUKONGA:
    return MappingWindow::Type::MAPPING_GC_BONGOS;
  case SerialInterface::SIDEVICE_GC_GBA_EMULATED:
    return MappingWindow::Type::MAPPING_GC_GBA;
  case SerialInterface::SIDEVICE_GC_KEYBOARD:
    return MappingWindow::Type::MAPPING_GC_KEYBOARD;
  case SerialInterface::SIDEVICE_AM_BASEBOARD:
    return MappingWindow::Type::MAPPING_AM_BASEBOARD;
  default:
    return std::nullopt;
  }
}
}  // namespace

GamecubeControllersWidget::GamecubeControllersWidget(QWidget* parent) : QWidget(parent)
{
  m_group = new QGroupBox(tr("GameCube Controllers"));
  m_grid = new QGridLayout();
  m_grid->setVerticalSpacing(7);
  m_grid->setColumnStretch(1, 1);

  for (size_t port = 0; port < PORTS; ++port)
  {
    auto* const caption = new QLabel(tr("Port %1").arg(port + 1));
    m_devices[port] = new QComboBox();
    m_configure[port] = new NonDefaultQPushButton(tr("Configure"));
    for (const DeviceChoice& choice : DEVICE_MENU)
      m_devices[port]->addItem(tr(choice.label));

    const int row = m_grid->rowCount();
    m_grid->addWidget(caption, row, 0);
    m_grid->addWidget(m_devices[port], row, 1);
    m_grid->addWidget(m_configure[port], row, 2);
  }
  m_group->setLayout(m_grid);

  auto* const column = new QVBoxLayout;
  column->setContentsMargins(0, 0, 0, 0);
  column->setAlignment(Qt::AlignTop);
  column->addWidget(m_group);
  setLayout(column);

  for (size_t port = 0; port < PORTS; ++port)
  {
    connect(m_devices[port], &QComboBox::currentIndexChanged, this, [this, port] {
      RefreshConfigureButton(port);
      StoreConfig();
    });
    connect(m_configure[port], &QPushButton::clicked, this, [this, port] { ConfigurePort(port); });
  }

  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          [this] { ShowConfig(Core::GetState(Core::System::GetInstance())); });
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          [this](Core::State state) { ShowConfig(state); });
  ShowConfig(Core::GetState(Core::System::GetInstance()));
}

void GamecubeControllersWidget::RefreshConfigureButton(size_t port)
{
  // Nothing to configure for an empty port or a GBA over TCP.
  const SIDevices device = DeviceAtRow(m_devices[port]->currentIndex());
  m_configure[port]->setEnabled(device != SerialInterface::SIDEVICE_NONE &&
                                device != SerialInterface::SIDEVICE_GC_GBA);
}

void GamecubeControllersWidget::ConfigurePort(size_t port)
{
  const SIDevices device = DeviceAtRow(m_devices[port]->currentIndex());

  // The USB adapter has its own small dialog instead of a mapping window.
  if (device == SerialInterface::SIDEVICE_WIIU_ADAPTER)
  {
    GCPadWiiUConfigDialog dialog(static_cast<int>(port), this);
    dialog.exec();
    return;
  }

  const std::optional<MappingWindow::Type> mapping = MappingFor(device);
  if (!mapping)
    return;

  auto* const window = new MappingWindow(this, *mapping, static_cast<int>(port));
  window->setAttribute(Qt::WA_DeleteOnClose, true);
  window->setWindowModality(Qt::WindowModality::WindowModal);
  window->show();
}

void GamecubeControllersWidget::ShowConfig(Core::State state)
{
  // During netplay the devices can't change while a game is running.
  const bool running = state != Core::State::Uninitialized;
  for (size_t port = 0; port < PORTS; ++port)
  {
    const SIDevices device = Config::Get(Config::GetInfoForSIDevice(static_cast<int>(port)));
    const std::optional<int> row = MenuRowFor(device);
    if (!row)
      continue;

    SignalBlocking(m_devices[port])->setCurrentIndex(*row);
    m_devices[port]->setEnabled(NetPlay::IsNetPlayRunning() ? !running : true);
    RefreshConfigureButton(port);
  }
}

void GamecubeControllersWidget::StoreConfig()
{
  {
    // All four ports are written before anyone is told of the change.
    const Config::ConfigChangeCallbackGuard batch;
    for (size_t port = 0; port < PORTS; ++port)
    {
      Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(static_cast<int>(port)),
                               DeviceAtRow(m_devices[port]->currentIndex()));
    }
  }
  SConfig::GetInstance().SaveSettings();
}
