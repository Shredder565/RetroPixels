// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// File locations on Linux and other Unix systems, in this order:
//
//   1. SUPERMODEL_HOME, when set, for the player's own files (RetroPixels.h);
//   2. the current folder, when it has a Config folder (a "portable" copy) or
//      there is no home folder: Config/, NVRAM/, ... next to the program,
//      with the log and screenshots in the current folder itself;
//   3. ~/.supermodel/<Kind>, when ~/.supermodel exists;
//   4. otherwise the XDG folders: settings in ~/.config/supermodel/Config
//      (or $XDG_CONFIG_HOME/supermodel/Config when ~/.config/supermodel is
//      missing), the rest in ~/.local/share/supermodel/<Kind> (or under
//      $XDG_DATA_HOME likewise).
//
// Folders are made as they are asked for.

#include "FileSystemPath.h"
#include "OSD/RetroPixels.h"

#include <cstdlib>
#include <string>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace FileSystemPath
{
    bool PathExists(std::string fileSystemPath)
    {
        struct stat st;
        return stat(fileSystemPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    int MakeDir(std::string dir)
    {
        return PathExists(dir) ? 0 : mkdir(dir.c_str(), 0775);
    }

    static const char *FolderName(PathType type)
    {
        switch (type)
        {
        case Analysis:    return "Analysis";
        case Config:      return "Config";
        case Log:         return "Log";
        case NVRAM:       return "NVRAM";
        case Saves:       return "Saves";
        case Screenshots: return "Screenshots";
        case Assets:      return "Assets";
        default:          return "";
        }
    }

    static std::string UserHome()
    {
        if (const struct passwd *pw = getpwuid(getuid()))
            return pw->pw_dir ? pw->pw_dir : "";
        const char *env = getenv("HOME");
        return env ? env : "";
    }

    static std::string EnvOr(const char *name, const std::string &fallback)
    {
        const char *v = getenv(name);
        return v ? std::string(v) : fallback;
    }

    // base/sub, both made if missing.
    static std::string MakeUnder(const std::string &base, const std::string &sub)
    {
        MakeDir(base);
        std::string dir = base + "/" + sub;
        MakeDir(dir);
        return dir;
    }

    std::string GetPath(PathType pathType)
    {
        std::string home = RetroPixels::HomePath(pathType);
        if (!home.empty())
            return home;
        if (pathType == Data)
            pathType = Config;

        const std::string name = FolderName(pathType);
        const std::string user = UserHome();
        std::string dir;

        if (PathExists("Config") || user.empty())
        {
            // Portable copy: the log and screenshots stay in the current folder.
            if (pathType != Log && pathType != Screenshots)
            {
                dir = name;
                MakeDir(dir);
            }
        }
        else if (PathExists(user + "/.supermodel"))
        {
            dir = user + "/.supermodel/" + name;
            MakeDir(dir);
        }
        else if (pathType == Config)
        {
            std::string base = user + "/.config/supermodel";
            std::string xdg = EnvOr("XDG_CONFIG_HOME", "");
            if (!PathExists(base) && !xdg.empty())
                base = xdg + "/supermodel";
            dir = MakeUnder(base, "Config");
        }
        else
        {
            std::string base = user + "/.local/share/supermodel";
            std::string xdg = EnvOr("XDG_DATA_HOME", "");
            if (!PathExists(base) && !xdg.empty())
                base = xdg + "/supermodel";
            dir = MakeUnder(base, name);
        }

        return dir.empty() ? dir : dir + "/";
    }
}
