// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <vector>

#include <QDialog>
#include <QWidget>

namespace PatchEngine
{
struct Patch;
struct PatchEntry;
}  // namespace PatchEngine

class QDialogButtonBox;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

struct NewPatchEntry;

// Edits a patch's name and entries; the patch is only written on Save.
// There is no Q_OBJECT here, so texts translate in the QDialog context.
class NewPatchDialog : public QDialog
{
public:
  explicit NewPatchDialog(QWidget* parent, PatchEngine::Patch& patch);
  ~NewPatchDialog() override;

private:
  QGroupBox* MakeEntryBox(const PatchEngine::PatchEntry& entry);
  void AppendBlankEntry();
  bool AllFieldsValid() const;

  void accept() override;

  QLineEdit* m_name;
  QWidget* m_entries_widget;
  QVBoxLayout* m_entries_column;
  QPushButton* m_add_entry;
  QDialogButtonBox* m_buttons;

  // Owned separately from the widgets because the edit handlers hold
  // pointers to them.
  std::vector<std::unique_ptr<NewPatchEntry>> m_entries;

  PatchEngine::Patch& m_patch;
};
