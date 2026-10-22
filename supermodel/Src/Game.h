// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// What Config/Games.xml says about one game: its identity, the board
// stepping and extra hardware it needs, and which control sets it uses.
// GameLoader fills it in; the emulator, the input system and the outputs
// read it.

#ifndef INCLUDED_GAME_H
#define INCLUDED_GAME_H

#include <cstdint>
#include <memory>
#include <string>

struct Game
{
  // Identity (<game name= parent=> and <identity>)
  std::string name;           // ROM set name, e.g. "swtrilgy"
  std::string parent;         // parent ROM set of a clone, else empty
  std::string title;
  std::string version;
  std::string manufacturer;
  unsigned year = 0;

  // <hardware>
  std::string stepping;       // "1.0", "1.5", "2.0" or "2.1"
  std::string mpeg_board;     // "DSB1", "DSB2" or empty

  // How the DSB and the two SCSPs are mixed to the host's speakers
  // (<audio>: Mono, Stereo, StereoReversed, QuadFrontRear, ...).
  enum AudioTypes
  {
    MONO = 0,               // everything to one channel
    STEREO_LR,              // left/right (the usual case)
    STEREO_RL,              // right/left
    QUAD_1_FLR_2_RLR,       // DSB+SCSP1 front L/R, SCSP2 rear L/R
    QUAD_1_FRL_2_RRL,       // DSB+SCSP1 front R/L, SCSP2 rear R/L
    QUAD_1_RLR_2_FLR,       // DSB+SCSP1 rear L/R, SCSP2 front L/R
    QUAD_1_RRL_2_FRL,       // DSB+SCSP1 rear R/L, SCSP2 front R/L
    QUAD_1_LR_2_FR_MIX,     // SCSP2's two outputs split between front and rear
  };
  AudioTypes audio = STEREO_LR;

  uint32_t encryption_key = 0;    // security board key, 0 = no security board
  bool netboard_present = false;

  // Control sets (<inputs><input type=...>), one bit each.
  enum Inputs
  {
    INPUT_UI              = 0,          // the emulator's own UI controls
    INPUT_COMMON          = 0x00000001, // coins, start, service, test
    INPUT_VEHICLE         = 0x00000002, // wheel and pedals
    INPUT_JOYSTICK1       = 0x00000004,
    INPUT_JOYSTICK2       = 0x00000008,
    INPUT_FIGHTING        = 0x00000010, // punch/kick/guard/escape
    INPUT_VR4             = 0x00000020, // four view buttons
    INPUT_VIEWCHANGE      = 0x00000040, // one view button
    INPUT_SHIFT4          = 0x00000080, // H-pattern four-speed shifter
    INPUT_SHIFTUPDOWN     = 0x00000100, // sequential shifter
    INPUT_HANDBRAKE       = 0x00000200,
    INPUT_HARLEY          = 0x00000400, // Harley-Davidson handlebars
    INPUT_GUN1            = 0x00000800, // light guns
    INPUT_GUN2            = 0x00001000,
    INPUT_ANALOG_JOYSTICK = 0x00002000, // flight stick
    INPUT_TWIN_JOYSTICKS  = 0x00004000, // Virtual On
    INPUT_SOCCER          = 0x00008000,
    INPUT_SPIKEOUT        = 0x00010000,
    INPUT_ANALOG_GUN1     = 0x00020000, // positional guns
    INPUT_ANALOG_GUN2     = 0x00040000,
    INPUT_SKI             = 0x00080000,
    INPUT_MAGTRUCK        = 0x00100000,
    INPUT_FISHING         = 0x00200000,
    INPUT_ALL             = 0x003FFFFF
  };
  uint32_t inputs = 0;

  // <drive_board>: the force feedback or billboard board on the drive port.
  enum DriveBoardType
  {
    DRIVE_BOARD_NONE = 0,
    DRIVE_BOARD_WHEEL,
    DRIVE_BOARD_JOYSTICK,
    DRIVE_BOARD_SKI,
    DRIVE_BOARD_BILLBOARD
  };
  DriveBoardType driveboard_type = DriveBoardType::DRIVE_BOARD_NONE;
};

#endif // INCLUDED_GAME_H
