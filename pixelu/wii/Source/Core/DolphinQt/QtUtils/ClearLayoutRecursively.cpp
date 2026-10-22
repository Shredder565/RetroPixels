// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/ClearLayoutRecursively.h"

#include <QLayout>
#include <QLayoutItem>
#include <QWidget>

void ClearLayoutRecursively(QLayout* layout)
{
  for (QLayoutItem* item = layout->takeAt(0); item != nullptr; item = layout->takeAt(0))
  {
    if (QWidget* const widget = item->widget())
    {
      layout->removeWidget(widget);
      widget->deleteLater();
    }
    else
    {
      if (QLayout* const nested = item->layout())
        ClearLayoutRecursively(nested);
      layout->removeItem(item);
    }
    delete item;
  }
}
