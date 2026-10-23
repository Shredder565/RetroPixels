// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// File locations on Windows: SUPERMODEL_HOME when set (RetroPixels.h),
// otherwise folders next to the program (relative to the current folder),
// with the log and screenshots in the current folder itself.

#include "FileSystemPath.h"
#include "OSD/RetroPixels.h"

#include <string>

namespace FileSystemPath
{
    std::string GetPath(PathType pathType)
    {
        std::string home = RetroPixels::HomePath(pathType);
        if (!home.empty())
            return home;

        switch (pathType)
        {
        case Analysis:    return "Analysis/";
        case Config:
        case Data:        return "Config/";
        case NVRAM:       return "NVRAM/";
        case Saves:       return "Saves/";
        case Assets:      return "Assets/";
        case Log:
        case Screenshots:
        default:          return "";
        }
    }
}
