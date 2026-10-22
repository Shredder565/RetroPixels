// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class QPixmap;

namespace UICommon
{
struct GameBanner;
}

// A game banner as a pixmap (the banner's pixels have red and blue the other way round from
// Qt's RGBX format).
QPixmap ToQPixmap(const UICommon::GameBanner& banner);
