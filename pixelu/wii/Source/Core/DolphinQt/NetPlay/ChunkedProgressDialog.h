// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <span>

#include <QDialog>

#include "Common/CommonTypes.h"

class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QVBoxLayout;
class QWidget;

// Shows one progress bar and one status line per player while a chunked transfer (for example a
// save or SD card sync) runs. Closing it as host aborts the pending game start.
class ChunkedProgressDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ChunkedProgressDialog(QWidget* parent);

  void show(const QString& title, u64 data_size, std::span<const int> players);
  void SetProgress(int pid, u64 progress);

  void reject() override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void ClearPlayerRows();
  void ShowSingleButton(bool is_host);

  std::map<int, QProgressBar*> m_progress_bars;
  std::map<int, QLabel*> m_status_labels;
  u64 m_data_size = 0;

  QGroupBox* m_progress_box = nullptr;
  QVBoxLayout* m_progress_layout = nullptr;
  QVBoxLayout* m_main_layout = nullptr;
  QDialogButtonBox* m_button_box = nullptr;
};
