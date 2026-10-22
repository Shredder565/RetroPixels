// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/NonAutodismissibleMenu.h"

#include <QAction>
#include <QMouseEvent>

namespace QtUtils
{
void NonAutodismissibleMenu::mouseReleaseEvent(QMouseEvent* const event)
{
  if (event == nullptr)
    return;

  QAction* const under_mouse = activeAction();
  const bool toggle_in_place =
      under_mouse != nullptr && under_mouse->isEnabled() && under_mouse->isCheckable();
  if (!toggle_in_place)
  {
    QMenu::mouseReleaseEvent(event);
    return;
  }

  // Trigger the action ourselves instead of letting QMenu do it (which would close the menu).
  under_mouse->trigger();
  event->accept();
}
}  // namespace QtUtils
