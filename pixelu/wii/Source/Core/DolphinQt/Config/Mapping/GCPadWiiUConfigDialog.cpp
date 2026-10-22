// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/GCPadWiiUConfigDialog.h"

#include <chrono>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"
#include "InputCommon/GCAdapter.h"

GCPadWiiUConfigDialog::GCPadWiiUConfigDialog(int port, QWidget* parent)
    : QDialog(parent), m_port{port}
{
  BuildLayout();

  // Show the stored values first, so that setting them does not write them back.
  ReadConfig();
  connect(m_rumble, &QCheckBox::toggled, this, &GCPadWiiUConfigDialog::WriteConfig);
  connect(m_bongos, &QCheckBox::toggled, this, &GCPadWiiUConfigDialog::WriteConfig);
}

void GCPadWiiUConfigDialog::BuildLayout()
{
  setWindowTitle(tr("GameCube Controller Adapter at Port %1").arg(m_port + 1));

  auto* const layout = new QVBoxLayout();
  m_status = new QLabel();
  m_poll_rate = new QLabel;
  m_rumble = new QCheckBox(tr("Enable Rumble"));
  m_bongos = new QCheckBox(tr("Simulate DK Bongos"));
  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Ok);

  // The adapter can be plugged in or out while the dialog is open: poll twice a second.
  RefreshStatus();
  auto* const poll_timer = new QTimer{this};
  connect(poll_timer, &QTimer::timeout, this, &GCPadWiiUConfigDialog::RefreshStatus);
  poll_timer->start(std::chrono::milliseconds{500});

  for (QWidget* const widget :
       std::initializer_list<QWidget*>{m_status, m_poll_rate, m_rumble, m_bongos, buttons})
  {
    layout->addWidget(widget);
  }
  setLayout(layout);

  connect(buttons, &QDialogButtonBox::accepted, this, &GCPadWiiUConfigDialog::accept);
}

void GCPadWiiUConfigDialog::RefreshStatus()
{
  const char* open_error = nullptr;
  const bool found = GCAdapter::IsDetected(&open_error);

  if (found)
    m_status->setText(tr("Adapter Detected"));
  else if (open_error != nullptr)
    m_status->setText(tr("Error Opening Adapter: %1").arg(QString::fromUtf8(open_error)));
  else
    m_status->setText(tr("No Adapter Detected"));

  // The poll rate is only known (non-zero) while the adapter is being read.
  const auto rate = GCAdapter::GetCurrentPollRate();
  if (rate == 0)
    m_poll_rate->clear();
  else
    m_poll_rate->setText(tr("Poll Rate: %1 Hz").arg(rate, 0, 'f', 2));

  // Both options need an adapter to talk to.
  m_rumble->setEnabled(found);
  m_bongos->setEnabled(found);
}

void GCPadWiiUConfigDialog::ReadConfig()
{
  m_rumble->setChecked(Config::Get(Config::GetInfoForAdapterRumble(m_port)));
  m_bongos->setChecked(Config::Get(Config::GetInfoForSimulateKonga(m_port)));
}

void GCPadWiiUConfigDialog::WriteConfig()
{
  Config::SetBaseOrCurrent(Config::GetInfoForAdapterRumble(m_port), m_rumble->isChecked());
  Config::SetBaseOrCurrent(Config::GetInfoForSimulateKonga(m_port), m_bongos->isChecked());
}
