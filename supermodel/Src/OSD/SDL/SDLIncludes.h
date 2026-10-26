// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The SDL headers in one place: the video/input/audio library and its
// networking add-on (left out of SUPERMODEL_NO_NET builds).

#ifndef INCLUDED_SDLINCLUDES_H
#define INCLUDED_SDLINCLUDES_H

#include <SDL.h>
#include <SDL_audio.h>
#ifndef SUPERMODEL_NO_NET
#include <SDL_net.h>
#endif

#endif // INCLUDED_SDLINCLUDES_H
