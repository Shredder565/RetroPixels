// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/EmulatedUSB/WiiSpeakWindow.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QString>
#include <QVBoxLayout>

#ifdef HAVE_CUBEB
#include "AudioCommon/CubebUtils.h"
#endif
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
constexpr int GAIN_FLOOR = -50;
constexpr int GAIN_CEILING = 50;

QString GainCaption(int db)
{
  return WiiSpeakWindow::tr("Volume modifier (value: %1dB)").arg(db);
}
}  // namespace

WiiSpeakWindow::WiiSpeakWindow(QWidget* parent) : QWidget(parent)
{
  // i18n: Window for managing the Wii Speak microphone
  setWindowTitle(tr("Wii Speak Manager"));
  setWindowIcon(Resources::GetAppIcon());
  setObjectName(QStringLiteral("wii_speak_manager"));
  setMinimumSize(QSize(700, 200));

  BuildLayout();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &WiiSpeakWindow::OnEmulationStateChanged);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

WiiSpeakWindow::~WiiSpeakWindow() = default;

void WiiSpeakWindow::BuildLayout()
{
  auto* column = new QVBoxLayout();

  auto* notice = new QLabel();
  notice->setText(QStringLiteral("<center><i>%1</i></center>")
                      .arg(tr("Some settings cannot be changed when emulation is running.")));
  column->addWidget(notice);

  // Plugged in or not; only changeable while nothing is running.
  auto* presence_box = new QGroupBox();
  auto* presence_row = new QHBoxLayout();
  presence_row->setAlignment(Qt::AlignHCenter);
  m_plugged_in = new QCheckBox(tr("Emulate Wii Speak"), this);
  m_plugged_in->setChecked(Config::Get(Config::MAIN_EMULATE_WII_SPEAK));
  connect(m_plugged_in, &QCheckBox::toggled, this, &WiiSpeakWindow::EmulateWiiSpeak);
  presence_row->addWidget(m_plugged_in);
  presence_box->setLayout(presence_row);
  column->addWidget(presence_box);

  column->addWidget(BuildMicrophoneBox());

  setLayout(column);
}

QGroupBox* WiiSpeakWindow::BuildMicrophoneBox()
{
  auto* box = new QGroupBox(tr("Microphone Configuration"));
  auto* row = new QHBoxLayout();

  // Mute follows the shared setting both ways (it can also be toggled by hotkey).
  auto* mute = new QCheckBox(tr("Mute"), this);
  Settings& settings = Settings::Instance();
  mute->setChecked(settings.IsWiiSpeakMuted());
  connect(&settings, &Settings::WiiSpeakMuteChanged, mute, &QCheckBox::setChecked);
  connect(mute, &QCheckBox::toggled, &settings, &Settings::SetWiiSpeakMuted);
  mute->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  row->addWidget(mute);

  auto* gain_grid = new QGridLayout();
  const int gain = std::clamp<int>(Config::Get(Config::MAIN_WII_SPEAK_VOLUME_MODIFIER), GAIN_FLOOR,
                                   GAIN_CEILING);
  auto* gain_slider = new QSlider(Qt::Horizontal, this);
  auto* gain_caption = new QLabel(GainCaption(gain));
  connect(gain_slider, &QSlider::valueChanged, this, [gain_caption](int db) {
    Config::SetBaseOrCurrent(Config::MAIN_WII_SPEAK_VOLUME_MODIFIER, db);
    gain_caption->setText(GainCaption(db));
  });
  gain_slider->setMinimum(GAIN_FLOOR);
  gain_slider->setMaximum(GAIN_CEILING);
  gain_slider->setValue(gain);
  gain_slider->setTickPosition(QSlider::TicksBothSides);
  gain_slider->setTickInterval(10);
  gain_slider->setSingleStep(1);
  gain_grid->addWidget(new QLabel(QStringLiteral("%1dB").arg(GAIN_FLOOR)), 0, 0, Qt::AlignLeft);
  gain_grid->addWidget(gain_caption, 0, 1, Qt::AlignCenter);
  gain_grid->addWidget(new QLabel(QStringLiteral("%1dB").arg(GAIN_CEILING)), 0, 2, Qt::AlignRight);
  gain_grid->addWidget(gain_slider, 1, 0, 1, 3);
  row->addLayout(gain_grid);
  row->setStretch(1, 3);

  m_input_device = new QComboBox();
#ifndef HAVE_CUBEB
  m_input_device->addItem(QLatin1String("(%1)").arg(tr("Audio backend unsupported")), QString{});
#else
  m_input_device->addItem(QLatin1String("(%1)").arg(tr("Autodetect preferred microphone")),
                          QString{});
  for (const auto& [id, name] : CubebUtils::ListInputDevices())
    m_input_device->addItem(QString::fromStdString(name), QString::fromStdString(id));
#endif
  // Connected before the saved device is selected, so selecting it stores it again.
  connect(m_input_device, &QComboBox::currentIndexChanged, this,
          &WiiSpeakWindow::OnInputDeviceChange);
  const QString saved_id = QString::fromStdString(Config::Get(Config::MAIN_WII_SPEAK_MICROPHONE));
  m_input_device->setCurrentIndex(m_input_device->findData(saved_id));
  row->addWidget(m_input_device);

  box->setLayout(row);
  return box;
}

void WiiSpeakWindow::EmulateWiiSpeak(bool emulate)
{
  Config::SetBaseOrCurrent(Config::MAIN_EMULATE_WII_SPEAK, emulate);
}

void WiiSpeakWindow::OnEmulationStateChanged(Core::State state)
{
  const bool locked = state != Core::State::Uninitialized;
  m_plugged_in->setEnabled(!locked);
  m_input_device->setEnabled(!locked);
}

void WiiSpeakWindow::OnInputDeviceChange()
{
  const QVariant chosen = m_input_device->currentData();
  if (!chosen.isValid())
    return;
  Config::SetBaseOrCurrent(Config::MAIN_WII_SPEAK_MICROPHONE, chosen.toString().toStdString());
}
