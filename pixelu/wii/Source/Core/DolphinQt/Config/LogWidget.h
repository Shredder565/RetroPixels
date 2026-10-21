// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <mutex>
#include <string>
#include <utility>

#include <QDockWidget>

#include "Common/FixedSizeQueue.h"
#include "Common/Logging/LogManager.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QTimer;

// The Log dock. Messages arrive from any thread into a bounded queue and
// are moved into the text view by a timer while the dock is visible.
class LogWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit LogWidget(QWidget* parent = nullptr);
  ~LogWidget() override;

protected:
  void closeEvent(QCloseEvent*) override;

private:
  // The log manager owns its listeners, while Qt owns this widget, so a
  // small forwarding listener is registered instead of the widget itself.
  class Forwarder final : public Common::Log::LogListener
  {
  public:
    explicit Forwarder(LogWidget* target) : m_target(target) {}

  private:
    void Log(Common::Log::LogLevel level, const char* text) override
    {
      m_target->Enqueue(level, text);
    }

    LogWidget* m_target;
  };

  using Line = std::pair<std::string, Common::Log::LogLevel>;

  // The text view keeps at most this many lines, and so does the queue.
  static constexpr int MAX_LINES = 5000;

  void Enqueue(Common::Log::LogLevel level, const char* text);
  void Drain();
  void ApplyFont();
  void ApplyWrap();
  void RestoreState();
  void StoreState();

  QCheckBox* m_wrap;
  QComboBox* m_font;
  QPushButton* m_clear;
  QPlainTextEdit* m_text;

  QTimer* m_timer;

  std::mutex m_pending_lock;
  Common::FixedSizeQueue<Line, MAX_LINES> m_pending;
};
