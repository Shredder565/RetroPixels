// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The settings window shown when the program is started with no arguments.

#ifndef INCLUDED_SDL_GUI_H
#define INCLUDED_SDL_GUI_H

#include "Util/NewConfig.h"

#include <string>
#include <vector>

// Shows the window until the player starts a game or leaves. Returns the ROM
// set to run (empty to quit). config holds the settings shown and changed;
// they are written to configPath when the player asks for that.
std::vector<std::string> RunGUI(const std::string &configPath, Util::Config::Node &config);

#endif // INCLUDED_SDL_GUI_H
