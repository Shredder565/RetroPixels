// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/ImageConverter.h"

#include <QPixmap>

#include "UICommon/GameFile.h"

QPixmap ToQPixmap(const UICommon::GameBanner& banner)
{
  // Wrap the pixels without copying, then swap red and blue (which makes the copy we keep).
  const QImage view(reinterpret_cast<const uchar*>(banner.buffer.data()),
                    static_cast<int>(banner.width), static_cast<int>(banner.height),
                    QImage::Format_RGBX8888);
  return QPixmap::fromImage(view.rgbSwapped());
}
