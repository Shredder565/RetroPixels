// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/AdvancedPane.h"

#include <cmath>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QDateTimeEdit>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/Config/Enums.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/QtUtils/AnalyticsPrompt.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

#include "UICommon/UICommon.h"

namespace
{
// Untranslated display names of the CPU cores; translated where they're shown.
const char* CoreDisplayName(PowerPC::CPUCore core)
{
  switch (core)
  {
  case PowerPC::CPUCore::Interpreter:
    return QT_TR_NOOP("Interpreter (slowest)");
  case PowerPC::CPUCore::CachedInterpreter:
    return QT_TR_NOOP("Cached Interpreter (slower)");
  case PowerPC::CPUCore::JIT64:
    return QT_TR_NOOP("JIT Recompiler for x86-64 (recommended)");
  case PowerPC::CPUCore::JITARM64:
    return QT_TR_NOOP("JIT Recompiler for ARM64 (recommended)");
  default:
    return "";
  }
}

// Group box with a vertical layout, appended to the page.
QVBoxLayout* AddGroup(QVBoxLayout* page, const QString& title)
{
  auto* const group = new QGroupBox(title);
  auto* const contents = new QVBoxLayout();
  group->setLayout(contents);
  page->addWidget(group);
  return contents;
}

// A margin-less row holding a slider and its right-aligned value read-out.
void AddSliderRow(QVBoxLayout* group, QWidget* slider, QLabel* value, int value_width)
{
  auto* const row = new QHBoxLayout();
  row->setContentsMargins(0, 0, 0, 0);
  group->addLayout(row);
  row->addWidget(slider);
  value->setFixedWidth(value_width);
  value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  row->addWidget(value);
}
}  // namespace

AdvancedPane::AdvancedPane(QWidget* parent) : QWidget(parent)
{
  auto* const page = new QVBoxLayout();
  setLayout(page);

  AddCpuGroup(page);
  AddTimingGroup(page);

  // One width for all value read-outs keeps the sliders lined up.
  const int value_width =
      QFontMetrics{font()}.boundingRect(QStringLiteral(" 500% (000.00 VPS)")).width();
  AddClockGroup(page, value_width);
  AddVbiGroup(page, value_width);
  AddMemoryGroup(page, value_width);
  AddRtcGroup(page);
  AddResetGroup(page);
  page->addStretch(1);

  RefreshState();
  HookUpConfigWriters();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &AdvancedPane::RefreshState);
}

void AdvancedPane::AddCpuGroup(QVBoxLayout* page)
{
  QVBoxLayout* const group = AddGroup(page, tr("CPU Options"));

  auto* const engine_form = new QFormLayout;
  engine_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  engine_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  group->addLayout(engine_form);

  std::vector<std::pair<QString, PowerPC::CPUCore>> cores;
  for (const PowerPC::CPUCore core : PowerPC::AvailableCPUCores())
    cores.emplace_back(tr(CoreDisplayName(core)), core);
  m_cpu_core = new ConfigChoiceMap<PowerPC::CPUCore>(cores, Config::MAIN_CPU_CORE);
  engine_form->addRow(tr("CPU Emulation Engine:"), m_cpu_core);

  m_mmu = new ConfigBool(tr("Enable MMU"), Config::MAIN_MMU);
  m_mmu->SetDescription(tr("Enables the Memory Management Unit, needed for some games. (ON = "
                           "Compatible, OFF = Fast)<br><br><dolphin_emphasis>If unsure, leave "
                           "this unchecked.</dolphin_emphasis>"));
  group->addWidget(m_mmu);

  m_pause_on_panic = new ConfigBool(tr("Pause on Panic"), Config::MAIN_PAUSE_ON_PANIC);
  m_pause_on_panic->SetDescription(
      tr("Pauses the emulation if a Read/Write or Unknown Instruction panic "
         "occurs.<br>Enabling will affect performance.<br>The performance impact is the same as "
         "having Enable MMU on.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
  group->addWidget(m_pause_on_panic);

  m_write_back_cache =
      new ConfigBool(tr("Enable Write-Back Cache (slow)"), Config::MAIN_ACCURATE_CPU_CACHE);
  m_write_back_cache->SetDescription(
      tr("Enables emulation of the CPU write-back cache.<br>Enabling will have a "
         "significant impact on performance.<br>This should be left disabled unless absolutely "
         "needed.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
  group->addWidget(m_write_back_cache);
}

void AdvancedPane::AddTimingGroup(QVBoxLayout* page)
{
  auto* const box = new QGroupBox(tr("Timing"));
  page->addWidget(box);
  auto* const group = new QVBoxLayout{box};

  // i18n: Correct is a verb
  auto* const drift = new ConfigBool{tr("Correct Time Drift"), Config::MAIN_CORRECT_TIME_DRIFT};
  // i18n: Internet play refers to services like Wiimmfi, not the NetPlay feature in Dolphin
  drift->SetDescription(tr("Allow the emulated console to run fast after stutters,<br>pursuing "
                           "accurate overall elapsed time unless paused or "
                           "speed-adjusted.<br><br>This may be useful for internet "
                           "play.<br><br><dolphin_emphasis>If unsure, leave this "
                           "unchecked.</dolphin_emphasis>"));
  group->addWidget(drift);

  // i18n: "Rush" is a verb
  auto* const rush =
      new ConfigBool{tr("Rush Frame Presentation"), Config::MAIN_RUSH_FRAME_PRESENTATION};
  rush->SetDescription(
      tr("Limits throttling between input and frame output, speeding through emulation to "
         "reach presentation, displaying sooner, and thus reducing input latency.<br><br>This "
         "will generally make frame pacing worse.<br>This setting can work either with or "
         "without Immediately Present XFB.<br>An Audio Buffer Size of at least 80 ms is "
         "recommended to ensure full effect.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
  group->addWidget(rush);

  // i18n: "Smooth" is a verb
  auto* const smooth =
      new ConfigBool{tr("Smooth Early Presentation"), Config::MAIN_SMOOTH_EARLY_PRESENTATION};
  smooth->SetDescription(
      tr("Adaptively adjusts the timing of early frame presentation.<br><br>This can improve "
         "frame pacing with Immediately Present XFB and/or Rush Frame Presentation, while "
         "still maintaining most of the input latency benefits.<br><br><dolphin_emphasis>If "
         "unsure, leave this unchecked.</dolphin_emphasis>"));
  group->addWidget(smooth);
}

void AdvancedPane::AddClockGroup(QVBoxLayout* page, int value_width)
{
  QVBoxLayout* const group = AddGroup(page, tr("Clock Override"));

  m_clock_override =
      new ConfigBool(tr("Enable Emulated CPU Clock Override"), Config::MAIN_OVERCLOCK_ENABLE);
  group->addWidget(m_clock_override);
  connect(m_clock_override, &QCheckBox::toggled, this, &AdvancedPane::RefreshState);

  m_clock_slider = new ConfigFloatSlider(0.01f, 5.0f, Config::MAIN_OVERCLOCK, 0.01f);
  m_clock_value = new QLabel();
  AddSliderRow(group, m_clock_slider, m_clock_value, value_width);

  RefreshClockText();
  connect(m_clock_slider, &QSlider::valueChanged, this, &AdvancedPane::RefreshClockText);

  m_clock_override->SetDescription(
      tr("Adjusts the emulated CPU's clock rate.<br><br>On games that have an unstable frame "
         "rate despite full emulation speed, higher values can improve their performance, "
         "requiring a powerful device. Lower values reduce the emulated console's "
         "performance, but improve the emulation speed.<br><br>WARNING: Changing this from "
         "the default (100%) can and will break games and cause glitches. Do so at your own "
         "risk. Please do not report bugs that occur with a non-default "
         "clock.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
}

void AdvancedPane::AddVbiGroup(QVBoxLayout* page, int value_width)
{
  QVBoxLayout* const group = AddGroup(page, tr("VBI Frequency Override"));

  m_vbi_override =
      new ConfigBool(tr("Enable VBI Frequency Override"), Config::MAIN_VI_OVERCLOCK_ENABLE);
  group->addWidget(m_vbi_override);
  connect(m_vbi_override, &QCheckBox::toggled, this, &AdvancedPane::RefreshState);

  m_vbi_slider = new ConfigFloatSlider(0.01f, 5.0f, Config::MAIN_VI_OVERCLOCK, 0.01f);
  m_vbi_value = new QLabel();
  AddSliderRow(group, m_vbi_slider, m_vbi_value, value_width);

  RefreshVbiText();
  connect(m_vbi_slider, &QSlider::valueChanged, this, &AdvancedPane::RefreshVbiText);

  m_vbi_override->SetDescription(
      tr("Adjusts the VBI frequency. Also adjusts the emulated CPU's clock rate, to keep it "
         "relatively the same.<br><br>Makes games run at a different frame rate, making the "
         "emulation less demanding when lowered, or improving smoothness when increased. "
         "This may affect gameplay speed, as it is often tied to the frame "
         "rate.<br><br>WARNING: Changing this from the default (100%) can and will break "
         "games and cause glitches. Do so at your own risk. Please do not report bugs that "
         "occur with a non-default frequency.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
}

void AdvancedPane::AddMemoryGroup(QVBoxLayout* page, int value_width)
{
  QVBoxLayout* const group = AddGroup(page, tr("Memory Override"));

  m_memory_override =
      new ConfigBool(tr("Enable Emulated Memory Size Override"), Config::MAIN_RAM_OVERRIDE_ENABLE);
  group->addWidget(m_memory_override);
  connect(m_memory_override, &QCheckBox::toggled, this, &AdvancedPane::RefreshState);

  // Sizes are shown in MiB; the setting itself is stored in bytes.
  constexpr u32 MIB = 0x100000;

  m_mem1_slider = new ConfigSliderU32(24, 64, Config::MAIN_MEM1_SIZE, MIB);
  m_mem1_value = new QLabel(tr("%1 MB (MEM1)").arg(QString::number(m_mem1_slider->value())));
  AddSliderRow(group, m_mem1_slider, m_mem1_value, value_width);
  connect(m_mem1_slider, &QSlider::valueChanged, this, [this](int megabytes) {
    m_mem1_value->setText(tr("%1 MB (MEM1)").arg(QString::number(megabytes)));
  });

  m_mem2_slider = new ConfigSliderU32(64, 128, Config::MAIN_MEM2_SIZE, MIB);
  m_mem2_value = new QLabel(tr("%1 MB (MEM2)").arg(QString::number(m_mem2_slider->value())));
  AddSliderRow(group, m_mem2_slider, m_mem2_value, value_width);
  connect(m_mem2_slider, &QSlider::valueChanged, this, [this](int megabytes) {
    m_mem2_value->setText(tr("%1 MB (MEM2)").arg(QString::number(megabytes)));
  });

  m_memory_override->SetDescription(
      tr("Sets the amount of RAM in the emulated console to the values provided.<br><br><b>"
         "WARNING</b>: Enabling this will completely break many games. By default, Dolphin "
         "determines what value is required based on the game "
         "information.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
}

void AdvancedPane::AddRtcGroup(QVBoxLayout* page)
{
  auto* const box = new QGroupBox(tr("Custom RTC Options"));
  box->setLayout(new QVBoxLayout());
  page->addWidget(box);

  m_custom_rtc = new ConfigBool(tr("Enable Custom RTC"), Config::MAIN_CUSTOM_RTC_ENABLE);
  box->layout()->addWidget(m_custom_rtc);
  connect(m_custom_rtc, &QCheckBox::toggled, this, &AdvancedPane::RefreshState);

  m_rtc_value = new QDateTimeEdit();
  // Include seconds in the displayed time.
  QString format = m_rtc_value->displayFormat();
  m_rtc_value->setDisplayFormat(format.replace(QStringLiteral("mm"), QStringLiteral("mm:ss")));
  QtUtils::ShowFourDigitYear(m_rtc_value);

  // The console clock covers 2000-01-01 00:00:00 to 2099-12-31 23:59:59, in UTC.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto utc = QTimeZone::UTC;
#else
  const auto utc = Qt::UTC;
#endif
  m_rtc_value->setDateTimeRange(QDateTime({2000, 1, 1}, {0, 0, 0}, utc),
                                QDateTime({2099, 12, 31}, {23, 59, 59}, utc));
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  m_rtc_value->setTimeZone(QTimeZone::UTC);
#else
  m_rtc_value->setTimeSpec(Qt::UTC);
#endif
  box->layout()->addWidget(m_rtc_value);

  m_custom_rtc->SetDescription(
      tr("This setting allows you to set a custom real time clock (RTC) separate from your "
         "current system time.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));
}

void AdvancedPane::AddResetGroup(QVBoxLayout* page)
{
  auto* const box = new QGroupBox(tr("Reset Dolphin Settings"));
  box->setLayout(new QVBoxLayout());
  page->addWidget(box);

  m_reset_all = new NonDefaultQPushButton(tr("Reset All Settings"));
  connect(m_reset_all, &QPushButton::clicked, this, &AdvancedPane::ConfirmAndReset);
  box->layout()->addWidget(m_reset_all);
}

void AdvancedPane::HookUpConfigWriters()
{
  m_memory_override->setChecked(Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE));
  connect(m_memory_override, &QCheckBox::toggled, this, [this](bool enabled) {
    Config::SetBaseOrCurrent(Config::MAIN_RAM_OVERRIDE_ENABLE, enabled);
    RefreshState();
  });

  connect(m_rtc_value, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime& when) {
    Config::SetBaseOrCurrent(Config::MAIN_CUSTOM_RTC_VALUE,
                             static_cast<u32>(when.toSecsSinceEpoch()));
    RefreshState();
  });
}

void AdvancedPane::RefreshClockText()
{
  const float factor = Config::Get(Config::MAIN_OVERCLOCK);
  const u32 ticks = Core::System::GetInstance().GetSystemTimers().GetTicksPerSecond();
  const auto base_mhz = static_cast<int>(ticks / 1'000'000.0);

  const QString percent = QString::number(static_cast<int>(std::round(factor * 100.f)));
  const QString mhz =
      QString::number(static_cast<int>(std::round(factor * static_cast<float>(base_mhz))));
  m_clock_value->setText(tr("%1% (%2 MHz)").arg(percent, mhz));
}

void AdvancedPane::RefreshVbiText()
{
  const int percent = static_cast<int>(std::round(Config::Get(Config::MAIN_VI_OVERCLOCK) * 100.f));

  // Without a running game (or with the override off) estimate from NTSC's 59.94 Hz.
  auto rate =
      static_cast<float>(Core::System::GetInstance().GetVideoInterface().GetTargetRefreshRate());
  if (rate == 0.0f || !Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE))
    rate = 59.94f * Config::Get(Config::MAIN_VI_OVERCLOCK);

  m_vbi_value->setText(
      tr("%1% (%2 VPS)").arg(QString::number(percent), QString::number(rate, 'f', 2)));
}

void AdvancedPane::RefreshState()
{
  const bool idle = Core::IsUninitialized(Core::System::GetInstance());
  const bool clock_on = Config::Get(Config::MAIN_OVERCLOCK_ENABLE);
  const bool vbi_on = Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE);
  const bool memory_on = Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE);
  const bool rtc_editable = Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE) && idle;

  // These only take effect at boot.
  for (QWidget* const boot_only :
       std::initializer_list<QWidget*>{m_cpu_core, m_mmu, m_pause_on_panic})
  {
    boot_only->setEnabled(idle);
  }

  // Bold marks a value overridden by a layer above the base config (e.g. a game INI).
  const auto emphasis = [this](bool overridden) {
    QFont shown = font();
    shown.setBold(overridden);
    return shown;
  };

  {
    const QFont clock_font = emphasis(
        Config::GetActiveLayerForConfig(Config::MAIN_OVERCLOCK_ENABLE) != Config::LayerType::Base);
    const QSignalBlocker quiet(m_clock_override);
    m_clock_override->setFont(clock_font);
    m_clock_override->setChecked(clock_on);
  }
  m_clock_slider->setEnabled(clock_on);
  m_clock_value->setEnabled(clock_on);

  // Unlike the clock box above, this one is updated with signals live.
  m_vbi_override->setFont(
      emphasis(Config::GetActiveLayerForConfig(Config::MAIN_VI_OVERCLOCK_ENABLE) !=
               Config::LayerType::Base));
  m_vbi_override->setChecked(vbi_on);
  m_vbi_slider->setEnabled(vbi_on);
  m_vbi_value->setEnabled(vbi_on);

  m_memory_override->setEnabled(idle);
  SignalBlocking(m_memory_override)->setChecked(memory_on);
  const bool memory_editable = memory_on && idle;
  for (QWidget* const part :
       std::initializer_list<QWidget*>{m_mem1_slider, m_mem1_value, m_mem2_slider, m_mem2_value})
  {
    part->setEnabled(memory_editable);
  }

  m_custom_rtc->setEnabled(idle);
  SignalBlocking(m_custom_rtc)->setChecked(Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE));

  QDateTime rtc_time;
  rtc_time.setSecsSinceEpoch(Config::Get(Config::MAIN_CUSTOM_RTC_VALUE));
  m_rtc_value->setEnabled(rtc_editable);
  SignalBlocking(m_rtc_value)->setDateTime(rtc_time);

  m_reset_all->setEnabled(idle);
}

void AdvancedPane::ConfirmAndReset()
{
  const auto answer = ModalMessageBox::question(
      this, tr("Reset Dolphin Settings"),
      tr("Are you sure you want to restore all Dolphin settings to their default values? This "
         "action cannot be undone!\nAll customizations or changes you have made will be "
         "lost.\n\nDo you want to proceed?"),
      ModalMessageBox::StandardButtons(ModalMessageBox::Yes | ModalMessageBox::No),
      ModalMessageBox::No, Qt::WindowModality::WindowModal);
  if (answer == ModalMessageBox::No)
    return;

  SConfig::ResetAllSettings();
  UICommon::SetUserDirectory(File::GetUserPath(D_USER_IDX));

  emit Settings::Instance().ConfigChanged();

#if defined(USE_ANALYTICS) && USE_ANALYTICS
  ShowAnalyticsPrompt(this);
#endif
}
