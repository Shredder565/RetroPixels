// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QTabWidget>

class QEvent;

// A tab widget whose tabs have close buttons, except the ones marked with setTabUnclosable().
class PartiallyClosableTabWidget : public QTabWidget
{
  Q_OBJECT
public:
  PartiallyClosableTabWidget(QWidget* parent = nullptr);

  void setTabUnclosable(int index);
};
