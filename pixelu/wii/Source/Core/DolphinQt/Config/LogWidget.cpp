// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/LogWidget.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <tuple>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include "DolphinQt/Settings.h"

namespace
{
// How often the queue is drained while the dock is visible, in ms.
constexpr int DRAIN_INTERVAL_MS = 100;
// Lines moved into the view per drain.
constexpr size_t LINES_PER_DRAIN = 200;
// Each message starts with a timestamp of this many characters, which is
// shown uncoloured.
constexpr size_t STAMP_CHARS = 10;

QString Utf8(std::string_view text)
{
  return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

const char* LevelColour(Common::Log::LogLevel level)
{
  switch (level)
  {
  case Common::Log::LogLevel::LERROR:
    return "red";
  case Common::Log::LogLevel::LWARNING:
    return "yellow";
  case Common::Log::LogLevel::LNOTICE:
    return "lime";
  case Common::Log::LogLevel::LINFO:
    return "cyan";
  case Common::Log::LogLevel::LDEBUG:
    return "lightgrey";
  }
  return "white";
}
}  // namespace

LogWidget::LogWidget(QWidget* parent) : QDockWidget(parent), m_timer(new QTimer(this))
{
  setWindowTitle(tr("Log"));
  setObjectName(QStringLiteral("log"));
  setHidden(!Settings::Instance().IsLogVisible());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  m_text = new QPlainTextEdit;
  m_wrap = new QCheckBox(tr("Word Wrap"));
  m_font = new QComboBox;
  m_clear = new QPushButton(tr("Clear"));
  m_font->addItems({tr("Default Font"), tr("Monospaced Font"), tr("Selected Font")});

  auto* const grid = new QGridLayout;
  grid->addWidget(m_wrap, 0, 0);
  grid->addWidget(m_font, 0, 1);
  grid->addWidget(m_clear, 0, 2);
  grid->addWidget(m_text, 1, 0, 1, -1);
  auto* const body = new QWidget;
  body->setLayout(grid);
  setWidget(body);

  m_text->setReadOnly(true);
  m_text->setUndoRedoEnabled(false);
  m_text->setMaximumBlockCount(MAX_LINES);
  m_text->setStyleSheet(
      QStringLiteral("QPlainTextEdit { background-color: black; color: white; }"));

  RestoreState();

  connect(m_clear, &QPushButton::clicked, [this] {
    m_text->clear();
    const std::lock_guard guard(m_pending_lock);
    m_pending.clear();
  });
  connect(m_wrap, &QCheckBox::toggled, this, &LogWidget::StoreState);
  connect(m_font, &QComboBox::currentIndexChanged, this, &LogWidget::StoreState);
  connect(this, &QDockWidget::topLevelChanged, this, &LogWidget::StoreState);
  connect(&Settings::Instance(), &Settings::LogVisibilityChanged, this, &LogWidget::setVisible);

  // Only poll the queue while the dock can be seen.
  connect(m_timer, &QTimer::timeout, this, &LogWidget::Drain);
  connect(this, &QDockWidget::visibilityChanged, [this](bool visible) {
    if (visible)
      m_timer->start(DRAIN_INTERVAL_MS);
    else
      m_timer->stop();
  });

  connect(&Settings::Instance(), &Settings::DebugFontChanged, this, &LogWidget::ApplyFont);

  Common::Log::LogManager::GetInstance()->RegisterListener(
      Common::Log::LogListener::LOG_WINDOW_LISTENER, std::make_unique<Forwarder>(this));
}

LogWidget::~LogWidget()
{
  StoreState();
  Common::Log::LogManager::GetInstance()->RegisterListener(
      Common::Log::LogListener::LOG_WINDOW_LISTENER, nullptr);
}

void LogWidget::Enqueue(Common::Log::LogLevel level, const char* text)
{
  // Trailing newlines are dropped; the view adds its own line breaks.
  size_t length = std::strlen(text);
  while (length != 0 && text[length - 1] == '\n')
    --length;

  const std::lock_guard guard(m_pending_lock);
  m_pending.emplace(std::piecewise_construct, std::forward_as_tuple(text, length),
                    std::forward_as_tuple(level));
}

void LogWidget::Drain()
{
  std::vector<Line> batch;
  {
    const std::lock_guard guard(m_pending_lock);
    if (m_pending.empty())
      return;
    batch.reserve(std::min(LINES_PER_DRAIN, m_pending.size()));
    while (!m_pending.empty() && batch.size() < LINES_PER_DRAIN)
      batch.push_back(m_pending.pop_front());
  }

  for (const Line& line : batch)
  {
    const std::string_view message = line.first;
    m_text->appendHtml(QStringLiteral("%1<span style=\"color: %2; white-space: pre\">%3</span>")
                           .arg(Utf8(message.substr(0, STAMP_CHARS)),
                                QString::fromUtf8(LevelColour(line.second)),
                                Utf8(message.substr(STAMP_CHARS)).toHtmlEscaped()));
  }
}

void LogWidget::ApplyFont()
{
  // 0: default font, 1: the system monospace font, 2: the debugger font.
  QFont font;
  const int choice = m_font->currentIndex();
  if (choice == 1)
    font = QFont(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  else if (choice == 2)
    font = Settings::Instance().GetDebugFont();
  m_text->setFont(font);
}

void LogWidget::ApplyWrap()
{
  const bool wrap = m_wrap->isChecked();
  m_text->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
  m_text->setHorizontalScrollBarPolicy(wrap ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOn);
}

void LogWidget::RestoreState()
{
  QSettings& settings = Settings::GetQSettings();

  restoreGeometry(settings.value(QStringLiteral("logwidget/geometry")).toByteArray());
  setFloating(settings.value(QStringLiteral("logwidget/floating")).toBool());

  m_wrap->setChecked(settings.value(QStringLiteral("logging/wraplines")).toBool());
  ApplyWrap();

  m_font->setCurrentIndex(settings.value(QStringLiteral("logging/font")).toInt());
  ApplyFont();
}

void LogWidget::StoreState()
{
  QSettings& settings = Settings::GetQSettings();

  settings.setValue(QStringLiteral("logwidget/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("logwidget/floating"), isFloating());

  settings.setValue(QStringLiteral("logging/wraplines"), m_wrap->isChecked());
  ApplyWrap();

  settings.setValue(QStringLiteral("logging/font"), m_font->currentIndex());
  ApplyFont();
}

void LogWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetLogVisible(false);
}
