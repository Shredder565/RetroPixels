// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <utility>

#include <QCheckBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>

namespace Core
{
enum class State;
}
namespace DiscIO
{
class Volume;
}

// The Verify tab: runs the volume verifier and shows its problems, summary,
// hashes and Redump status.
class VerifyWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit VerifyWidget(std::shared_ptr<DiscIO::Volume> volume);

private:
  // A read-only hash field with its "Calculate" box, added as a form row.
  std::pair<QCheckBox*, QLineEdit*> AddHashRow(QFormLayout* form, const QString& caption);

  void OnEmulationStateChanged(Core::State state);
  bool RedumpAllowed() const;
  void RefreshRedumpBox();
  void RunVerification();
  void PutProblemCell(int row, int column, const QString& text);

  std::shared_ptr<DiscIO::Volume> m_volume;
  QTableWidget* m_problems;
  QTextEdit* m_summary;
  QFormLayout* m_hash_form;
  QFormLayout* m_redump_form;
  QCheckBox* m_crc32_box;
  QCheckBox* m_md5_box;
  QCheckBox* m_sha1_box;
  QCheckBox* m_redump_box;
  QLineEdit* m_crc32_field;
  QLineEdit* m_md5_field;
  QLineEdit* m_sha1_field;
  QLineEdit* m_redump_field;
  QPushButton* m_verify_button;
};
