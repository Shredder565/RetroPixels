// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <QDialog>

class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QVBoxLayout;
class QWidget;

// Collects every player's SHA1 of the chosen game, shows per-player progress while hashing runs,
// and says whether all results agree once each player has reported.
class GameDigestDialog : public QDialog
{
  Q_OBJECT
public:
  explicit GameDigestDialog(QWidget* parent);

  void show(const QString& title);
  void SetProgress(int pid, int progress);
  void SetResult(int pid, const std::string& result);

  void reject() override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void UseOnlyButton(int standard_button);
  QLabel* StatusLabelFor(int pid) const;

  std::map<int, QProgressBar*> m_progress_bars;
  std::map<int, QLabel*> m_status_labels;

  std::vector<std::string> m_results;

  QGroupBox* m_progress_box = nullptr;
  QVBoxLayout* m_progress_layout = nullptr;
  QVBoxLayout* m_main_layout = nullptr;
  QLabel* m_check_label = nullptr;
  QDialogButtonBox* m_button_box = nullptr;
};
