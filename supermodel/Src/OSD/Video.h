// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Hooks the emulator calls around drawing each frame, so the front end can
// draw its own things over the picture and show it.

#ifndef INCLUDED_VIDEO_H
#define INCLUDED_VIDEO_H

// Before a frame is drawn. False means: don't draw this one.
extern bool BeginFrameVideo();

// After the frame is drawn: overlays, then the frame is shown.
extern void EndFrameVideo();

#endif // INCLUDED_VIDEO_H
