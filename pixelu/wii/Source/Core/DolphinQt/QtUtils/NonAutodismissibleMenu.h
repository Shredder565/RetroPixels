// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QMenu>

namespace QtUtils
{
// A QMenu that stays open when one of its checkable actions is clicked, so several options can be
// toggled in one go. Other actions close it as usual.
class NonAutodismissibleMenu : public QMenu
{
public:
  using QMenu::QMenu;

protected:
  void mouseReleaseEvent(QMouseEvent* event) override;
};
}  // namespace QtUtils
