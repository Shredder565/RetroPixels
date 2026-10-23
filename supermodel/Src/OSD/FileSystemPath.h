// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Where the program's files live on this system. Each platform has its own
// FileSystemPath.cpp (Windows/, Unix/).

#ifndef INCLUDED_FILESYSTEMPATH_H
#define INCLUDED_FILESYSTEMPATH_H

#include <string>

namespace FileSystemPath
{
    // Kinds of files. Data is where Games.xml and Music.xml are read from:
    // the Config folder, unless SUPERMODEL_HOME moves the player's own files
    // elsewhere (RetroPixels.h).
    enum PathType { Analysis, Config, Log, NVRAM, Saves, Screenshots, Assets, Data };

    // True if the folder exists.
    bool PathExists(std::string fileSystemPath);

    // Makes the folder if it isn't there; 0 on success.
    int MakeDir(std::string dir);

    // The folder for a kind of file, ending in a slash, or "" for the
    // current folder.
    std::string GetPath(PathType pathType);
}

#endif // INCLUDED_FILESYSTEMPATH_H
