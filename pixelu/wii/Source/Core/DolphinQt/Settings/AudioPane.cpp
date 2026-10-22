// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/AudioPane.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSpacerItem>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/WASAPIStream.h"

#ifdef HAVE_CUBEB
#include "AudioCommon/CubebUtils.h"
#endif

#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/System.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"

#include "DolphinQt/Settings.h"

namespace
{
using ChoiceList = std::vector<std::pair<QString, QString>>;

// The volume read-out ("75%"). Translated in the generic QWidget context.
QString PercentText(int percent)
{
  return QWidget::tr("%1%").arg(percent);
}

// The buffer size slider moves in steps of this many milliseconds.
constexpr int BUFFER_STEP_MS = 8;
}  // namespace

AudioPane::AudioPane()
{
  m_has_latency_setting =
      std::ranges::any_of(AudioCommon::GetSoundBackends(), AudioCommon::SupportsLatencyControl);

  QGroupBox* const dsp = BuildDspBox();
  QGroupBox* const volume = BuildVolumeBox();
  QGroupBox* const backend = BuildBackendBox();
  dsp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  QGroupBox* const playback = BuildPlaybackBox();
#ifdef HAVE_CUBEB
  m_routing_box = BuildRoutingBox();
#endif

  // Settings stacked on the left, the tall volume slider on the right.
  auto* const left = new QVBoxLayout;
  left->addWidget(dsp);
  left->addWidget(backend);
  left->addWidget(playback);
#ifdef HAVE_CUBEB
  left->addWidget(m_routing_box);
#endif
  auto* const columns = new QHBoxLayout;
  columns->addLayout(left);
  columns->addWidget(volume);
  setLayout(columns);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  SetTooltips();
  WireUp();
  OnBackendChanged();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    OnEmulationStateChanged(state != Core::State::Uninitialized);
  });
  OnEmulationStateChanged(!Core::IsUninitialized(Core::System::GetInstance()));
}

QGroupBox* AudioPane::BuildDspBox()
{
  auto* const box = new QGroupBox(tr("DSP Options"));
  auto* const row = new QHBoxLayout;
  box->setLayout(row);

  auto* const label = new QLabel(tr("DSP Emulation Engine:"));
  // Two booleans (HLE, JIT) encode the engine; HLE+interpreter can't normally happen and is
  // shown as HLE.
  m_dsp_engine = new ConfigComplexChoice(Config::MAIN_DSP_HLE, Config::MAIN_DSP_JIT);
  m_dsp_engine->Add(tr("HLE (recommended)"), true, true);
  m_dsp_engine->Add(tr("LLE Recompiler (slow)"), false, true);
  m_dsp_engine->Add(tr("LLE Interpreter (very slow)"), false, false);
  m_dsp_engine->SetDefault(0);
  m_dsp_engine->Refresh();

  row->addWidget(label);
  row->addWidget(m_dsp_engine, Qt::AlignLeft);
  return box;
}

QGroupBox* AudioPane::BuildVolumeBox()
{
  auto* const box = new QGroupBox(tr("Volume"));
  auto* const column = new QVBoxLayout{box};

  m_volume = new ConfigSlider(0, 100, Config::MAIN_AUDIO_VOLUME);
  m_volume->setOrientation(Qt::Vertical);

  m_volume_value = new QLabel;
  m_volume_value->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
  const auto show_volume = [this] { m_volume_value->setText(PercentText(m_volume->value())); };
  show_volume();
  connect(m_volume, &QSlider::valueChanged, this, show_volume);

  // At least as wide as "100%" and as the box title (a narrower box clips the title on
  // Windows).
  const QFontMetrics metrics{font()};
  const int widest = std::max(metrics.boundingRect(PercentText(100)).width(),
                              metrics.boundingRect(box->title()).width());
  m_volume_value->setFixedWidth(widest);

  column->addWidget(m_volume, 0, Qt::AlignHCenter);
  column->addWidget(m_volume_value, 0, Qt::AlignHCenter);
  return box;
}

QGroupBox* AudioPane::BuildBackendBox()
{
  auto* const box = new QGroupBox(tr("Backend Settings"));
  auto* const form = new QFormLayout;
  box->setLayout(form);
  m_backend_label = new QLabel(tr("Audio Backend:"));

  // Backend names are translatable for display; the untranslated name is what's stored.
  ChoiceList backends;
  for (const std::string& name : AudioCommon::GetSoundBackends())
    backends.emplace_back(tr(name.c_str()), QString::fromStdString(name));
  m_backend = new ConfigStringChoice(backends, Config::MAIN_AUDIO_BACKEND);

  m_dpl2 = new ConfigBool(tr("Dolby Pro Logic II Decoder"), Config::MAIN_DPL2_DECODER);
  m_dpl2_quality_label = new QLabel(tr("Decoding Quality:"));
  const QStringList qualities{tr("Lowest (Latency ~10 ms)"), tr("Low (Latency ~20 ms)"),
                              tr("High (Latency ~40 ms)"), tr("Highest (Latency ~80 ms)")};
  m_dpl2_quality = new ConfigChoice(qualities, Config::MAIN_DPL2_QUALITY);

  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(m_backend_label, m_backend);

#ifdef _WIN32
  ChoiceList devices;
  devices.emplace_back(tr("Default Device"),
                       QString::fromStdString(Config::MAIN_WASAPI_DEVICE.GetDefaultValue()));
  for (const std::string& device : WASAPIStream::GetAvailableDevices())
    devices.emplace_back(QString::fromStdString(device), QString::fromStdString(device));

  m_wasapi_device_label = new QLabel(tr("Output Device:"));
  m_wasapi_device = new ConfigStringChoice(devices, Config::MAIN_WASAPI_DEVICE);
  form->addRow(m_wasapi_device_label, m_wasapi_device);
#endif

  if (m_has_latency_setting)
  {
    m_latency = new ConfigSlider(0, 200, Config::MAIN_AUDIO_LATENCY);
    m_latency_label = new QLabel(tr("Latency: %1 ms").arg(m_latency->value()));
    m_latency_label->setFixedWidth(
        QFontMetrics(font()).boundingRect(tr("Latency:  000 ms")).width());
    form->addRow(m_latency_label, m_latency);
  }

  form->addRow(m_dpl2);
  form->addRow(m_dpl2_quality_label, m_dpl2_quality);
  return box;
}

QGroupBox* AudioPane::BuildPlaybackBox()
{
  auto* const box = new QGroupBox(tr("Audio Playback Settings"));
  auto* const grid = new QGridLayout;
  box->setLayout(grid);

  auto* const buffer = new ConfigSlider(16, 512, Config::MAIN_AUDIO_BUFFER_SIZE, BUFFER_STEP_MS);
  auto* const buffer_value = new QLabel;
  buffer->setSingleStep(BUFFER_STEP_MS);
  buffer->setPageStep(BUFFER_STEP_MS);
  buffer->SetDescription(
      tr("Controls the number of audio samples buffered. Lower values reduce latency but may "
         "cause more crackling or stuttering.<br><br><dolphin_emphasis>If unsure, set this to "
         "80 ms.</dolphin_emphasis>"));

  // Snap to whole steps and keep the read-out in sync.
  connect(buffer, &QSlider::valueChanged, this, [buffer, buffer_value](int value) {
    const int snapped = value / BUFFER_STEP_MS * BUFFER_STEP_MS;
    buffer->setValue(snapped);
    buffer_value->setText(tr("%1 ms").arg(snapped));
  });
  buffer_value->setText(tr("%1 ms").arg(buffer->value()));
  buffer_value->setFixedWidth(QFontMetrics(font()).boundingRect(tr(" 000 ms")).width());

  m_fill_gaps = new ConfigBool(tr("Fill Audio Gaps"), Config::MAIN_AUDIO_FILL_GAPS);
  m_preserve_pitch = new ConfigBool(tr("Preserve Audio Pitch"), Config::MAIN_AUDIO_PRESERVE_PITCH);
  m_mute_when_unthrottled = new ConfigBool(tr("Mute When Disabling Speed Limit"),
                                           Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT);

  auto* const buffer_row = new QHBoxLayout;
  buffer_row->addWidget(new ConfigSliderLabel(tr("Audio Buffer Size:"), buffer));
  buffer_row->addWidget(buffer);
  buffer_row->addWidget(buffer_value);

  grid->addLayout(buffer_row, 0, 0);
  grid->addWidget(m_fill_gaps, 1, 0);
  grid->addWidget(m_preserve_pitch, 2, 0);
  grid->addWidget(m_mute_when_unthrottled, 3, 0);
  grid->setRowStretch(4, 1);
  box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return box;
}

#ifdef HAVE_CUBEB
QGroupBox* AudioPane::BuildRoutingBox()
{
  auto* const box = new QGroupBox(tr("Wii Remote Audio Routing"));
  auto* const column = new QVBoxLayout;
  box->setLayout(column);

  m_routing = new ConfigBool(tr("Enable Wii Remote Audio Routing"),
                             Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED);
  column->addWidget(m_routing);

  // One device list shared by all four selectors; the empty id means the default device.
  ChoiceList devices;
  devices.emplace_back(tr("Default Device"), QStringLiteral(""));
  for (const auto& [id, name] : CubebUtils::ListOutputDevices())
    devices.emplace_back(QString::fromStdString(name), QString::fromStdString(id));

  for (std::size_t remote = 0; remote < m_routed_remote.size(); ++remote)
  {
    auto* const line = new QWidget;
    auto* const line_layout = new QHBoxLayout(line);
    line_layout->setContentsMargins(0, 0, 0, 0);

    m_routed_remote[remote] = new ConfigBool(tr("Wii Remote %1").arg(remote + 1),
                                             Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[remote]);
    m_routed_device[remote] =
        new ConfigStringChoice(devices, Config::MAIN_WIIMOTE_AUDIO_OUTPUT_DEVICE[remote]);
    line_layout->addWidget(m_routed_remote[remote]);
    line_layout->addWidget(m_routed_device[remote], 1);

    column->addWidget(line);
  }
  return box;
}
#endif

void AudioPane::WireUp()
{
  connect(m_backend, &QComboBox::currentIndexChanged, this, &AudioPane::OnBackendChanged);
  connect(m_dpl2, &ConfigBool::toggled, this, &AudioPane::OnDspChanged);
  connect(m_dsp_engine, &ConfigComplexChoice::currentIndexChanged, this, &AudioPane::OnDspChanged);
  connect(m_volume, &QSlider::valueChanged, this,
          [] { AudioCommon::UpdateSoundStream(Core::System::GetInstance()); });

  if (m_has_latency_setting)
  {
    connect(m_latency, &QSlider::valueChanged, this,
            [this](int ms) { m_latency_label->setText(tr("Latency: %1 ms").arg(ms)); });
  }

#ifdef HAVE_CUBEB
  const auto refresh = [this](bool) { RefreshRoutingState(); };
  connect(m_routing, &ConfigBool::toggled, this, refresh);
  for (ConfigBool* const remote : m_routed_remote)
    connect(remote, &ConfigBool::toggled, this, refresh);
  // Speaker data, Bluetooth passthrough and Wii Remote sources live elsewhere; follow them.
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &AudioPane::RefreshRoutingState);
  RefreshRoutingState();
#endif
}

void AudioPane::OnDspChanged()
{
  // Dolby Pro Logic II needs an LLE DSP and a backend that can decode it.
  const std::string backend = Config::Get(Config::MAIN_AUDIO_BACKEND);
  const bool available =
      AudioCommon::SupportsDPL2Decoder(backend) && !Config::Get(Config::MAIN_DSP_HLE);
  const bool quality_available = available && m_dpl2->isChecked();
  m_dpl2->setEnabled(available);
  m_dpl2_quality_label->setEnabled(quality_available);
  m_dpl2_quality->setEnabled(quality_available);
}

void AudioPane::OnBackendChanged()
{
  OnDspChanged();

  const std::string backend = Config::Get(Config::MAIN_AUDIO_BACKEND);

  if (m_has_latency_setting)
  {
    m_latency_label->setEnabled(AudioCommon::SupportsLatencyControl(backend));
    m_latency->setEnabled(AudioCommon::SupportsLatencyControl(backend));
  }

#ifdef _WIN32
  const bool wasapi = backend == BACKEND_WASAPI;
  m_wasapi_device_label->setHidden(!wasapi);
  m_wasapi_device->setHidden(!wasapi);
#endif

  m_volume->setEnabled(AudioCommon::SupportsVolumeChanges(backend));
  m_volume_value->setEnabled(AudioCommon::SupportsVolumeChanges(backend));

#ifdef HAVE_CUBEB
  RefreshRoutingState();
#endif
}

void AudioPane::OnEmulationStateChanged(bool running)
{
  const bool idle = !running;
  m_dsp_engine->setEnabled(idle);
  m_backend_label->setEnabled(idle);
  m_backend->setEnabled(idle);

  // Only touch the Dolby options when OnDspChanged() left them usable.
  if (AudioCommon::SupportsDPL2Decoder(Config::Get(Config::MAIN_AUDIO_BACKEND)) &&
      !Config::Get(Config::MAIN_DSP_HLE))
  {
    m_dpl2->setEnabled(idle);
    m_dpl2_quality_label->setEnabled(idle && m_dpl2->isChecked());
    m_dpl2_quality->setEnabled(idle && m_dpl2->isChecked());
  }

  if (m_has_latency_setting &&
      AudioCommon::SupportsLatencyControl(Config::Get(Config::MAIN_AUDIO_BACKEND)))
  {
    m_latency_label->setEnabled(idle);
    m_latency->setEnabled(idle);
  }

#ifdef _WIN32
  m_wasapi_device->setEnabled(idle);
#endif

#ifdef HAVE_CUBEB
  RefreshRoutingState();
#endif
}

void AudioPane::RefreshRoutingState()
{
#ifdef HAVE_CUBEB
  if (m_routing_box == nullptr)
    return;

  // Routing needs Cubeb, speaker data, no Bluetooth passthrough, and no running game.
  const bool running = Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized;
  const bool cubeb = Config::Get(Config::MAIN_AUDIO_BACKEND) == BACKEND_CUBEB;
  const bool speaker_data = Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER);
  const bool passthrough = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED);
  const bool usable = !running && cubeb && speaker_data && !passthrough;

  m_routing->setEnabled(usable);
  const bool routing = usable && m_routing->isChecked();

  // Only emulated Wii Remotes have a speaker stream to route.
  for (std::size_t remote = 0; remote < m_routed_remote.size(); ++remote)
  {
    const auto source = Config::Get(Config::GetInfoForWiimoteSource(static_cast<int>(remote)));
    const bool active = routing && source == WiimoteSource::Emulated;
    m_routed_remote[remote]->setEnabled(active);
    m_routed_device[remote]->setEnabled(active && m_routed_remote[remote]->isChecked());
  }
#endif
}

void AudioPane::SetTooltips()
{
  m_dsp_engine->SetTitle(tr("DSP Emulation Engine"));
  m_dsp_engine->SetDescription(
      tr("Selects how the Digital Signal Processor (DSP) is emulated. Determines how the audio "
         "is processed and what system features are available.<br><br><b>HLE</b> - High Level "
         "Emulation of the DSP. Fast, but not always accurate. Lacks Dolby Pro Logic II "
         "decoding.<br><br><b>LLE Recompiler</b> - Low Level Emulation of the DSP, via a "
         "recompiler. Slower, but more accurate. Enables Dolby Pro Logic II decoding on certain "
         "audio backends.<br><br><b>LLE Interpreter</b> - Low Level Emulation of the DSP, via "
         "an interpreter. Slowest, for debugging purposes only. Not "
         "recommended.<br><br><dolphin_emphasis>If unsure, select HLE.</dolphin_emphasis>"));

  m_backend->SetTitle(tr("Audio Backend"));
  m_backend->SetDescription(
      tr("Selects which audio API to use internally.<br><br><dolphin_emphasis>If unsure, select "
         "%1.</dolphin_emphasis>")
          .arg(QString::fromStdString(AudioCommon::GetDefaultSoundBackend())));

  m_dpl2->SetTitle(tr("Dolby Pro Logic II Decoder"));
  m_dpl2->SetDescription(
      tr("Enables Dolby Pro Logic II emulation using 5.1 surround. Certain backends only. "
         "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>"));

  m_dpl2_quality->SetTitle(tr("Decoding Quality"));
  m_dpl2_quality->SetDescription(
      tr("Adjusts the quality setting of the Dolby Pro Logic II decoder. Higher presets "
         "increases audio latency.<br><br><dolphin_emphasis>If unsure, select "
         "High.</dolphin_emphasis>"));

#ifdef _WIN32
  m_wasapi_device->SetTitle(tr("Output Device"));
  m_wasapi_device->SetDescription(
      tr("Selects an output device to use.<br><br><dolphin_emphasis>If unsure, select Default "
         "Device.</dolphin_emphasis>"));
#endif

  m_volume->SetTitle(tr("Volume"));
  m_volume->SetDescription(tr("Adjusts audio output volume.<br><br><dolphin_emphasis>If unsure, "
                              "leave this at 100%.</dolphin_emphasis>"));

  if (m_has_latency_setting)
  {
    m_latency->SetTitle(tr("Latency"));
    m_latency->SetDescription(
        tr("Sets the audio latency in milliseconds. Higher values may reduce audio crackling. "
           "Certain backends only.<br><br><dolphin_emphasis>If unsure, leave this at 20 "
           "ms.</dolphin_emphasis>"));
  }

  m_mute_when_unthrottled->SetTitle(tr("Mute When Disabling Speed Limit"));
  m_mute_when_unthrottled->SetDescription(
      tr("Mutes the audio when overriding the emulation speed limit (default hotkey: Tab). "
         "<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>"));

  m_fill_gaps->SetTitle(tr("Fill Audio Gaps"));
  m_fill_gaps->SetDescription(tr("Repeat existing audio during lag spikes to prevent "
                                 "stuttering.<br><br><dolphin_emphasis>If unsure, leave this "
                                 "checked.</dolphin_emphasis>"));

  m_preserve_pitch->SetTitle(tr("Preserve Audio Pitch"));
  m_preserve_pitch->SetDescription(
      tr("Keeps audio at normal pitch when changing emulation speed. Without this, audio pitch "
         "changes proportionally with speed.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));

#ifdef HAVE_CUBEB
  if (m_routing_box == nullptr)
    return;

  m_routing->SetTitle(tr("Enable Wii Remote Audio Routing"));
  m_routing->SetDescription(
      tr("Routes each Wii Remote's speaker audio to a separate audio output device. This "
         "setting cannot be changed while emulation is active.<br><br>This setting is disabled "
         "when Enable Speaker Data is disabled, Passthrough a Bluetooth adapter is selected, or "
         "an audio backend other than Cubeb is selected.<br><br><dolphin_emphasis>If unsure, "
         "leave this unchecked.</dolphin_emphasis>"));
  for (std::size_t remote = 0; remote < m_routed_remote.size(); ++remote)
  {
    m_routed_remote[remote]->SetDescription(
        tr("Enables routing this Wii Remote's speaker audio to a separate output device."));
    m_routed_device[remote]->SetTitle(tr("Wii Remote %1").arg(remote + 1));
    m_routed_device[remote]->SetDescription(
        tr("Selects the audio output device for this Wii Remote's speaker audio.<br><br>This "
           "setting is disabled when this Wii Remote is not set to Emulated Wii Remote."));
  }
#endif
}
