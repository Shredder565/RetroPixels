// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

/*
 * RetroPixels.h
 *
 * Additions made for RetroPixels (https://github.com/shredder565/retropixels),
 * which starts Supermodel from its own window.
 *
 * - SUPERMODEL_HOME: when this environment variable names a folder, the
 *   player's own files (Supermodel.ini, NVRAM, save states, the log and
 *   screenshots) go there, in Config/, NVRAM/, Saves/, Log/ and
 *   Screenshots/ sub-folders. Games.xml, Music.xml and Assets/ are still read
 *   from where Supermodel normally keeps them.
 * - -rp-record=<dir> with -rp-ffmpeg=<ffmpeg>: saves the game's picture to
 *   <dir>/Frames/supermodel_framedump<N>.mkv (encoded by that ffmpeg while
 *   the game runs, a new file each time the picture size changes) and its
 *   sound to <dir>/Audio/supermodel_dspdump.wav (44.1 kHz, 16-bit stereo).
 *   One video frame is written per emulated frame at 60 fps, matching the
 *   735 sound samples Supermodel makes per frame, so picture and sound stay
 *   in step however fast the PC is.
 */

#ifndef INCLUDED_RETROPIXELS_H
#define INCLUDED_RETROPIXELS_H

#include "OSD/FileSystemPath.h"
#include <string>

#define RETROPIXELS_PATCH_LEVEL "retropixels-1"

namespace RetroPixels
{
  // Folder (ending in a slash) for one kind of the player's own files when
  // SUPERMODEL_HOME is set, created if needed; "" otherwise, and always ""
  // for Data and Assets (read-only files that come with Supermodel).
  std::string HomePath(FileSystemPath::PathType pathType);

  // Joins a folder from GetPath() and a file name. Exposed for tests.
  std::string JoinPath(const std::string &dir, const std::string &name);

  namespace Recorder
  {
    // Starts recording into dir with the ffmpeg program given. Returns false
    // (and records nothing) if the folders can't be made.
    bool Start(const std::string &dir, const std::string &ffmpeg);
    bool Active();
    // Called with the GL context current, just before the frame is shown:
    // reads the whole window (width x height) and queues it for ffmpeg.
    void Frame(unsigned width, unsigned height);
    // One frame of sound, the same buffers OutputAudio() gets.
    void Audio(unsigned numSamples, const float *leftFront, const float *rightFront, const float *leftRear, const float *rightRear);
    // While paused, frames and sound aren't kept.
    void SetPaused(bool paused);
    // Finishes the files (ffmpeg is given the end of the stream and waited for).
    void Stop();
  }
}

#endif // INCLUDED_RETROPIXELS_H
