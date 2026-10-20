// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

class nexService;

// Services handed over here are updated by a shared background thread, which
// also deletes them once they are marked for destruction.
void nexThread_registerService(nexService* service);
bool nexThread_isCurrentThread();
