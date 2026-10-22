// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include <QDialog>
#include <QWidget>

#include "Common/CommonTypes.h"

class QCheckBox;
class QGroupBox;
class QLineEdit;
class QVBoxLayout;

namespace Core
{
enum class State;
}

namespace IOS::HLE::USB
{
enum class FigureUIPosition : u8;
}

// The Disney Infinity base: whether it is plugged in, and the figure file on each of its nine
// positions (three discs, two players and their abilities).
class InfinityBaseWindow : public QWidget
{
  Q_OBJECT
public:
  explicit InfinityBaseWindow(QWidget* parent = nullptr);
  ~InfinityBaseWindow() override;

protected:
  std::array<QLineEdit*, 9> m_edit_figures;

private:
  void BuildLayout();
  void AddPositionRow(QVBoxLayout* rows, const QString& caption,
                      IOS::HLE::USB::FigureUIPosition position);
  QLineEdit* FigureField(IOS::HLE::USB::FigureUIPosition position) const;

  void OnEmulationStateChanged(Core::State state);
  void EmulateBase(bool emulate);
  void ClearFigure(IOS::HLE::USB::FigureUIPosition slot);
  void LoadFigure(IOS::HLE::USB::FigureUIPosition slot);
  void CreateFigure(IOS::HLE::USB::FigureUIPosition slot);
  void LoadFigurePath(IOS::HLE::USB::FigureUIPosition slot, const QString& path);

  QCheckBox* m_plugged_in;
  QGroupBox* m_positions_box;
};

// Picks a character for one base position and writes a blank figure file for it.
class CreateFigureDialog : public QDialog
{
  Q_OBJECT

public:
  explicit CreateFigureDialog(QWidget* parent, IOS::HLE::USB::FigureUIPosition slot);
  QString GetFilePath() const;

protected:
  QString m_file_path;

private:
  void OnCreate(const QString& number_text);
};
