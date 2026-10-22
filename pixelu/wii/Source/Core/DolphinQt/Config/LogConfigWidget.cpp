// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/LogConfigWidget.h"

#include <array>
#include <utility>

#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "DolphinQt/Settings.h"

namespace
{
using Common::Log::LogLevel;
using Common::Log::LogListener;
using Common::Log::LogType;

constexpr int TYPE_COUNT = static_cast<int>(LogType::NUMBER_OF_LOGS);

Common::Log::LogManager* Manager()
{
  return Common::Log::LogManager::GetInstance();
}
}  // namespace

LogConfigWidget::LogConfigWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Log Configuration"));
  setObjectName(QStringLiteral("logconfig"));
  setHidden(!Settings::Instance().IsLogConfigVisible());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  Build();
  ReadState();
  Wire();
}

LogConfigWidget::~LogConfigWidget()
{
  WriteState();
}

void LogConfigWidget::Build()
{
  auto* const column = new QVBoxLayout;

  auto* const verbosity_group = new QGroupBox(tr("Verbosity"));
  auto* const verbosity_column = new QVBoxLayout;
  verbosity_group->setLayout(verbosity_column);
  m_notice = new QRadioButton(tr("Notice"));
  m_error = new QRadioButton(tr("Error"));
  m_warning = new QRadioButton(tr("Warning"));
  m_info = new QRadioButton(tr("Info"));
  m_debug = new QRadioButton(tr("Debug"));
  // Builds that compile out debug logging can't select it.
  if constexpr (Common::Log::MAX_EFFECTIVE_LOGLEVEL < LogLevel::LDEBUG)
    m_debug->setVisible(false);

  auto* const output_group = new QGroupBox(tr("Logger Outputs"));
  auto* const output_column = new QVBoxLayout;
  output_group->setLayout(output_column);
  m_to_file = new QCheckBox(tr("Write to File"));
  m_to_console = new QCheckBox(tr("Write to Console"));
  m_to_window = new QCheckBox(tr("Write to Window"));

  auto* const type_group = new QGroupBox(tr("Log Types"));
  auto* const type_column = new QVBoxLayout;
  type_group->setLayout(type_column);
  m_toggle_types = new QPushButton(tr("Toggle All Log Types"));
  m_types = new QListWidget;

  // One checkable row per log type, "Full Name (SHORT)".
  const Common::Log::LogManager* const manager = Manager();
  for (int index = 0; index < TYPE_COUNT; ++index)
  {
    const auto type = static_cast<LogType>(index);
    auto* const row = new QListWidgetItem(
        QStringLiteral("%1 (%2)").arg(QString::fromUtf8(manager->GetFullName(type)),
                                      QString::fromUtf8(manager->GetShortName(type))));
    row->setCheckState(Qt::Unchecked);
    m_types->addItem(row);
  }

  column->addWidget(verbosity_group);
  for (QRadioButton* const button : {m_notice, m_error, m_warning, m_info, m_debug})
    verbosity_column->addWidget(button);

  column->addWidget(output_group);
  for (QCheckBox* const box : {m_to_file, m_to_console, m_to_window})
    output_column->addWidget(box);

  column->addWidget(type_group);
  type_column->addWidget(m_toggle_types);
  type_column->addWidget(m_types);

  auto* const body = new QWidget;
  body->setLayout(column);
  setWidget(body);
}

void LogConfigWidget::Wire()
{
  const std::array<std::pair<QRadioButton*, LogLevel>, 5> levels{{
      {m_notice, LogLevel::LNOTICE},
      {m_error, LogLevel::LERROR},
      {m_warning, LogLevel::LWARNING},
      {m_info, LogLevel::LINFO},
      {m_debug, LogLevel::LDEBUG},
  }};
  for (const auto& [button, level] : levels)
  {
    connect(button, &QRadioButton::clicked, [chosen = level](const bool checked) {
      if (checked)
        Manager()->SetConfigLogLevel(chosen);
    });
  }
  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &LogConfigWidget::HighlightVerbosity);

  for (QCheckBox* const box : {m_to_file, m_to_console, m_to_window})
    connect(box, &QCheckBox::toggled, this, &LogConfigWidget::WriteState);

  connect(m_toggle_types, &QPushButton::clicked, [this] { ToggleAllTypes(); });
  connect(m_types, &QListWidget::itemChanged, this, &LogConfigWidget::WriteState);

  connect(&Settings::Instance(), &Settings::LogConfigVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
}

// Checks the button of the effective level. Its text is bold when the level
// comes from a layer above the base one.
void LogConfigWidget::HighlightVerbosity()
{
  const LogLevel effective = Manager()->GetEffectiveLogLevel();
  const std::array<std::pair<QRadioButton*, LogLevel>, 5> levels{{
      {m_notice, LogLevel::LNOTICE},
      {m_error, LogLevel::LERROR},
      {m_warning, LogLevel::LWARNING},
      {m_info, LogLevel::LINFO},
      {m_debug, LogLevel::LDEBUG},
  }};
  for (const auto& [button, level] : levels)
  {
    QFont font = button->font();
    bool bold = false;
    if (level == effective)
    {
      bold = Config::GetActiveLayerForConfig(Common::Log::LOGGER_VERBOSITY) !=
             Config::LayerType::Base;
      button->setChecked(true);
    }
    font.setBold(bold);
    button->setFont(font);
  }
}

void LogConfigWidget::ToggleAllTypes()
{
  m_all_on = !m_all_on;

  m_suspend_writes = true;
  const Qt::CheckState state = m_all_on ? Qt::Checked : Qt::Unchecked;
  for (int row = 0; row < m_types->count(); ++row)
    m_types->item(row)->setCheckState(state);
  m_suspend_writes = false;

  WriteState();
}

void LogConfigWidget::ReadState()
{
  const Common::Log::LogManager* const manager = Manager();
  const QSettings& settings = Settings::GetQSettings();

  restoreGeometry(settings.value(QStringLiteral("logconfigwidget/geometry")).toByteArray());
  setFloating(settings.value(QStringLiteral("logconfigwidget/floating")).toBool());

  const LogLevel effective = manager->GetEffectiveLogLevel();
  m_notice->setChecked(effective == LogLevel::LNOTICE);
  m_error->setChecked(effective == LogLevel::LERROR);
  m_warning->setChecked(effective == LogLevel::LWARNING);
  m_info->setChecked(effective == LogLevel::LINFO);
  m_debug->setChecked(effective == LogLevel::LDEBUG);

  m_to_file->setChecked(manager->IsListenerEnabled(LogListener::FILE_LISTENER));
  m_to_console->setChecked(manager->IsListenerEnabled(LogListener::CONSOLE_LISTENER));
  m_to_window->setChecked(manager->IsListenerEnabled(LogListener::LOG_WINDOW_LISTENER));

  for (int index = 0; index < TYPE_COUNT; ++index)
  {
    const bool on = manager->IsEnabled(static_cast<LogType>(index));
    if (!on)
      m_all_on = false;
    m_types->item(index)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
  }
}

void LogConfigWidget::WriteState()
{
  if (m_suspend_writes)
    return;

  QSettings& settings = Settings::GetQSettings();
  settings.setValue(QStringLiteral("logconfigwidget/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("logconfigwidget/floating"), isFloating());

  Common::Log::LogManager* const manager = Manager();
  manager->EnableListener(LogListener::FILE_LISTENER, m_to_file->isChecked());
  manager->EnableListener(LogListener::CONSOLE_LISTENER, m_to_console->isChecked());
  manager->EnableListener(LogListener::LOG_WINDOW_LISTENER, m_to_window->isChecked());

  // Only types whose state changed are touched.
  for (int index = 0; index < TYPE_COUNT; ++index)
  {
    const auto type = static_cast<LogType>(index);
    const bool wanted = m_types->item(index)->checkState() == Qt::Checked;
    if (wanted != manager->IsEnabled(type))
      manager->SetEnable(type, wanted);
  }
}

void LogConfigWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetLogConfigVisible(false);
}
