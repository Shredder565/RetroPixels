// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class QLayout;

// Empties a layout: child widgets are scheduled for deletion, nested layouts are emptied the same
// way, and every layout item is deleted.
void ClearLayoutRecursively(QLayout* layout);
