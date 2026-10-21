// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/EmulatedUSB/LogitechMicWindow.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QString>

#ifdef HAVE_CUBEB
#include "AudioCommon/CubebUtils.h"
#endif
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
// The volume offset slider covers -50 dB .. +50 dB.
constexpr int GAIN_FLOOR = -50;
constexpr int GAIN_CEILING = 50;

QString GainCaption(int db)
{
  return LogitechMicWindow::tr("Volume modifier (value: %1dB)").arg(db);
}

QLabel* GainEndLabel(int db)
{
  return new QLabel(QStringLiteral("%1dB").arg(db));
}
}  // namespace

LogitechMicWindow::LogitechMicWindow(QWidget* parent) : QWidget(parent)
{
  setWindowTitle(tr("Logitech USB Microphone Manager"));
  setWindowIcon(Resources::GetAppIcon());
  setObjectName(QStringLiteral("logitech_mic_manager"));
  setMinimumSize(QSize(700, 200));

  BuildLayout();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &LogitechMicWindow::OnEmulationStateChanged);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

void LogitechMicWindow::BuildLayout()
{
  auto* const column = new QVBoxLayout;

  auto* const notice = new QLabel;
  notice->setText(QStringLiteral("<center><i>%1</i></center>")
                      .arg(tr("Some settings cannot be changed when emulation is running.")));
  column->addWidget(notice);

  column->addWidget(BuildPresenceBox());
  column->addWidget(BuildMicrophonesBox());

  setLayout(column);
}

QGroupBox* LogitechMicWindow::BuildPresenceBox()
{
  auto* box = new QGroupBox();
  auto* row = new QHBoxLayout();
  row->setAlignment(Qt::AlignHCenter);

  for (std::size_t mic = 0; mic < m_plugged_in.size(); ++mic)
  {
    m_plugged_in[mic] = new ConfigBool(tr("Emulate Logitech USB Mic %1").arg(mic + 1),
                                       Config::MAIN_EMULATE_LOGITECH_MIC[mic]);
    row->addWidget(m_plugged_in[mic]);
  }

  box->setLayout(row);
  return box;
}

QGroupBox* LogitechMicWindow::BuildMicrophonesBox()
{
  auto* box = new QGroupBox(tr("Microphone Configuration"));
  auto* stack = new QVBoxLayout();

  for (std::size_t mic = 0; mic < m_input_device.size(); ++mic)
    stack->addWidget(BuildMicrophoneBox(mic));

  box->setLayout(stack);
  return box;
}

QGroupBox* LogitechMicWindow::BuildMicrophoneBox(std::size_t mic)
{
  // i18n: %1 is a number from 1 to 4.
  auto* box = new QGroupBox(tr("Microphone %1 Configuration").arg(mic + 1));
  auto* const row = new QHBoxLayout();

  auto* const mute = new ConfigBool(tr("Mute"), Config::MAIN_LOGITECH_MIC_MUTED[mic]);
  mute->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  row->addWidget(mute);

  // Volume offset: a captioned slider with the range ends labelled.
  auto* gain_grid = new QGridLayout();
  const int gain = std::clamp<int>(Config::Get(Config::MAIN_LOGITECH_MIC_VOLUME_MODIFIER[mic]),
                                   GAIN_FLOOR, GAIN_CEILING);
  auto* const gain_slider = new QSlider(Qt::Horizontal, this);
  auto* const gain_caption = new QLabel(GainCaption(gain));
  connect(gain_slider, &QSlider::valueChanged, this, [gain_caption, mic](int db) {
    Config::SetBaseOrCurrent(Config::MAIN_LOGITECH_MIC_VOLUME_MODIFIER[mic], db);
    gain_caption->setText(GainCaption(db));
  });
  gain_slider->setMinimum(GAIN_FLOOR);
  gain_slider->setMaximum(GAIN_CEILING);
  gain_slider->setValue(gain);
  gain_slider->setTickPosition(QSlider::TicksBothSides);
  gain_slider->setTickInterval(10);
  gain_slider->setSingleStep(1);
  gain_grid->addWidget(GainEndLabel(GAIN_FLOOR), 0, 0, Qt::AlignLeft);
  gain_grid->addWidget(gain_caption, 0, 1, Qt::AlignCenter);
  gain_grid->addWidget(GainEndLabel(GAIN_CEILING), 0, 2, Qt::AlignRight);
  gain_grid->addWidget(gain_slider, 1, 0, 1, 3);
  row->addLayout(gain_grid);
  row->setStretch(1, 3);

  // Host input device. The first entry (empty id) means "let the backend choose".
  QComboBox* const device = new QComboBox();
  m_input_device[mic] = device;
#ifndef HAVE_CUBEB
  device->addItem(QLatin1String("(%1)").arg(tr("Audio backend unsupported")), QString{});
#else
  device->addItem(QLatin1String("(%1)").arg(tr("Autodetect preferred microphone")), QString{});
  for (const auto& [id, name] : CubebUtils::ListInputDevices())
    device->addItem(QString::fromStdString(name), QString::fromStdString(id));
#endif
  const QString saved_id =
      QString::fromStdString(Config::Get(Config::MAIN_LOGITECH_MIC_MICROPHONE[mic]));
  device->setCurrentIndex(device->findData(saved_id));
  connect(device, &QComboBox::currentIndexChanged, this, [this, mic] { OnInputDeviceChange(mic); });
  row->addWidget(device);

  box->setLayout(row);
  return box;
}

void LogitechMicWindow::OnEmulationStateChanged(Core::State state)
{
  // Plugging microphones in or out only takes effect at boot.
  const bool locked = state != Core::State::Uninitialized;
  for (ConfigBool* plugged_in : m_plugged_in)
    plugged_in->setEnabled(!locked);
}

void LogitechMicWindow::OnInputDeviceChange(std::size_t index)
{
  const QVariant chosen = m_input_device[index]->currentData();
  if (!chosen.isValid())
    return;
  Config::SetBaseOrCurrent(Config::MAIN_LOGITECH_MIC_MICROPHONE[index],
                           chosen.toString().toStdString());
}
