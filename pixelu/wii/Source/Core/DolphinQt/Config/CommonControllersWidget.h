// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class QCheckBox;
class QGroupBox;
class QVBoxLayout;
class QPushButton;

// The "Common" box of the Controllers page: background input and the
// buttons that open the alternate input source and SDL hint windows.
class CommonControllersWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit CommonControllersWidget(QWidget* parent);

private:
  template <class Window>
  void OpenModal();

  void Reload();
  void StoreBackgroundInput();

  QCheckBox* m_background_input;
  QPushButton* m_input_sources_button;
  QPushButton* m_sdl_hints_button;
};
