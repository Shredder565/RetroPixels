// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// RetroPixels for Android: what Supermodel needs on a phone besides SDL.
// The app's SupermodelActivity (an SDLActivity in its own process) loads
// libsupermodel.so, sets SUPERMODEL_HOME and RP_FLIGHTSTICK, calls
// nativePrepare() with the folder holding Config/Games.xml, Config/Music.xml
// and Assets/, and starts the program with the game's zip on the command
// line. From there:
//   - Start() (from Run(), before the window and inputs) sets SDL's Android
//     hints, sends the console output to Log/console.txt and adds the touch
//     controls as a virtual game controller, so they map like any pad;
//   - the touch controls' state arrives with nativeTouch() from the app's
//     UI thread and is handed to SDL whenever it reads the pads;
//   - the recorder (RetroPixels::Recorder, declared in OSD/RetroPixels.h)
//     gives the app's GameRecorder each frame's picture and sound while the
//     app says to record (nativeSetRecording()).

#ifndef INCLUDED_OSD_ANDROID_ANDROID_H
#define INCLUDED_OSD_ANDROID_ANDROID_H

namespace RetroPixels
{
  namespace Android
  {
    void Start();
    void Stop();
  }
}

#endif // INCLUDED_OSD_ANDROID_ANDROID_H
