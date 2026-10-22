// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <utility>

#include <QObject>
#include <QProgressDialog>
#include <QString>

#include "Common/Flag.h"

// A QProgressDialog that worker threads can update. The dialog itself lives on the GUI thread;
// the setters below only emit signals, which Qt delivers to the dialog on its own thread.
class ParallelProgressDialog final : public QObject
{
  Q_OBJECT

public:
  ParallelProgressDialog(const ParallelProgressDialog&) = delete;
  ParallelProgressDialog& operator=(const ParallelProgressDialog&) = delete;
  ParallelProgressDialog(ParallelProgressDialog&&) = delete;
  ParallelProgressDialog& operator=(ParallelProgressDialog&&) = delete;

  // GUI thread only. The arguments are passed on to QProgressDialog's constructor.
  template <typename... Args>
  ParallelProgressDialog(Args&&... args) : m_dialog{std::forward<Args>(args)...}
  {
    setParent(m_dialog.parent());
    WireUp();
  }

  // GUI thread only.
  QProgressDialog* GetRaw() { return &m_dialog; }

  // Any thread.
  void Cancel() { emit CancelSignal(); }
  void Reset() { emit ResetSignal(); }
  void SetCancelButtonText(const QString& text) { emit SetCancelButtonTextSignal(text); }
  void SetLabelText(const QString& text) { emit SetLabelTextSignal(text); }
  void SetMaximum(int maximum) { emit SetMaximumSignal(maximum); }
  void SetMinimum(int minimum) { emit SetMinimumSignal(minimum); }
  void SetMinimumDuration(int ms) { emit SetMinimumDurationSignal(ms); }
  void SetRange(int minimum, int maximum) { emit SetRangeSignal(minimum, maximum); }
  void SetValue(int progress) { emit SetValueSignal(progress); }
  bool WasCanceled() const { return m_cancelled.IsSet(); }

signals:
  void CancelSignal();
  void ResetSignal();
  void SetCancelButtonTextSignal(const QString& cancel_button_text);
  void SetLabelTextSignal(const QString& text);
  void SetMaximumSignal(int maximum);
  void SetMinimumSignal(int minimum);
  void SetMinimumDurationSignal(int ms);
  void SetRangeSignal(int minimum, int maximum);
  void SetValueSignal(int progress);

  void Canceled();
  void Finished(int result);

private:
  void WireUp()
  {
    QProgressDialog* const dialog = &m_dialog;

    // Requests from any thread, forwarded to the dialog.
    connect(this, &ParallelProgressDialog::CancelSignal, dialog, &QProgressDialog::cancel);
    connect(this, &ParallelProgressDialog::ResetSignal, dialog, &QProgressDialog::reset);
    connect(this, &ParallelProgressDialog::SetCancelButtonTextSignal, dialog,
            &QProgressDialog::setCancelButtonText);
    connect(this, &ParallelProgressDialog::SetLabelTextSignal, dialog,
            &QProgressDialog::setLabelText);
    connect(this, &ParallelProgressDialog::SetMaximumSignal, dialog,
            &QProgressDialog::setMaximum);
    connect(this, &ParallelProgressDialog::SetMinimumSignal, dialog,
            &QProgressDialog::setMinimum);
    connect(this, &ParallelProgressDialog::SetMinimumDurationSignal, dialog,
            &QProgressDialog::setMinimumDuration);
    connect(this, &ParallelProgressDialog::SetRangeSignal, dialog, &QProgressDialog::setRange);
    connect(this, &ParallelProgressDialog::SetValueSignal, this,
            &ParallelProgressDialog::ApplyValue);

    // Notifications from the dialog.
    connect(dialog, &QProgressDialog::canceled, this, [this] { m_cancelled.Set(); });
    connect(dialog, &QProgressDialog::canceled, this, &ParallelProgressDialog::Canceled);
    connect(dialog, &QProgressDialog::finished, this, &ParallelProgressDialog::Finished);
  }

  // QProgressDialog::setValue can process events, which may deliver further queued SetValue
  // requests and re-enter here (see QTBUG-10561); unbounded, that overflows the stack. A
  // re-entrant call only records the newest value, and the outermost call keeps applying values
  // until no newer one has arrived.
  void ApplyValue(int progress)
  {
    m_latest_value = progress;
    if (m_applying_value)
      return;

    m_applying_value = true;
    int applied;
    do
    {
      applied = m_latest_value;
      m_dialog.setValue(applied);
    } while (applied != m_latest_value);
    m_applying_value = false;
  }

  QProgressDialog m_dialog;
  Common::Flag m_cancelled;
  int m_latest_value = 0;
  bool m_applying_value = false;
};
