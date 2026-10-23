// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Supermodel.ini written on first start, and the default control
// assignments, which DefaultConfig() (Main.cpp) uses too.

#ifndef INCLUDED_DEFAULTCONFIGFILE_H
#define INCLUDED_DEFAULTCONFIGFILE_H

#include <string>

namespace DefaultSettings
{
  struct InputDefault
  {
    const char *name;      // the setting is "Input" + name
    const char *mapping;   // KEY_..., JOY1_..., MOUSE_..., comma separated
  };

  inline const InputDefault kInputs[] =
  {
    // coins, start, service and test
    { "Start1", "KEY_1,JOY1_BUTTON9" },
    { "Start2", "KEY_2,JOY2_BUTTON9" },
    { "Coin1", "KEY_3,JOY1_BUTTON10" },
    { "Coin2", "KEY_4,JOY2_BUTTON10" },
    { "ServiceA", "KEY_5" },
    { "ServiceB", "KEY_7" },
    { "TestA", "KEY_6" },
    { "TestB", "KEY_8" },
    // 4-way joysticks
    { "JoyUp", "KEY_UP,JOY1_UP" },
    { "JoyDown", "KEY_DOWN,JOY1_DOWN" },
    { "JoyLeft", "KEY_LEFT,JOY1_LEFT" },
    { "JoyRight", "KEY_RIGHT,JOY1_RIGHT" },
    { "JoyUp2", "JOY2_UP" },
    { "JoyDown2", "JOY2_DOWN" },
    { "JoyLeft2", "JOY2_LEFT" },
    { "JoyRight2", "JOY2_RIGHT" },
    // fighting games
    { "Punch", "KEY_A,JOY1_BUTTON1" },
    { "Kick", "KEY_S,JOY1_BUTTON2" },
    { "Guard", "KEY_D,JOY1_BUTTON3" },
    { "Escape", "KEY_F,JOY1_BUTTON4" },
    { "Punch2", "JOY2_BUTTON1" },
    { "Kick2", "JOY2_BUTTON2" },
    { "Guard2", "JOY2_BUTTON3" },
    { "Escape2", "JOY2_BUTTON4" },
    // Spikeout
    { "Shift", "KEY_A,JOY1_BUTTON1" },
    { "Beat", "KEY_S,JOY1_BUTTON2" },
    { "Charge", "KEY_D,JOY1_BUTTON3" },
    { "Jump", "KEY_F,JOY1_BUTTON4" },
    // Virtua Striker
    { "ShortPass", "KEY_A,JOY1_BUTTON1" },
    { "LongPass", "KEY_S,JOY1_BUTTON2" },
    { "Shoot", "KEY_D,JOY1_BUTTON3" },
    { "ShortPass2", "JOY2_BUTTON1" },
    { "LongPass2", "JOY2_BUTTON2" },
    { "Shoot2", "JOY2_BUTTON3" },
    // wheel and pedals
    { "SteeringLeft", "KEY_LEFT" },
    { "SteeringRight", "KEY_RIGHT" },
    { "Steering", "JOY1_XAXIS" },
    { "Accelerator", "KEY_UP,JOY1_UP" },
    { "Brake", "KEY_DOWN,JOY1_DOWN" },
    // gear shifts (sequential, then 4-speed)
    { "GearShiftUp", "KEY_Y" },
    { "GearShiftDown", "KEY_H" },
    { "GearShift1", "KEY_Q,JOY1_BUTTON5" },
    { "GearShift2", "KEY_W,JOY1_BUTTON6" },
    { "GearShift3", "KEY_E,JOY1_BUTTON7" },
    { "GearShift4", "KEY_R,JOY1_BUTTON8" },
    { "GearShiftN", "KEY_T" },
    // view buttons
    { "VR1", "KEY_A,JOY1_BUTTON1" },
    { "VR2", "KEY_S,JOY1_BUTTON2" },
    { "VR3", "KEY_D,JOY1_BUTTON3" },
    { "VR4", "KEY_F,JOY1_BUTTON4" },
    { "ViewChange", "KEY_A,JOY1_BUTTON1" },
    // Sega Rally 2 and Harley-Davidson extras
    { "HandBrake", "KEY_S,JOY1_BUTTON2" },
    { "RearBrake", "KEY_S,JOY1_BUTTON2" },
    { "MusicSelect", "KEY_D,JOY1_BUTTON3" },
    // Virtual On twin sticks
    { "TwinJoyTurnLeft", "KEY_Q,JOY1_RXAXIS_NEG" },
    { "TwinJoyTurnRight", "KEY_W,JOY1_RXAXIS_POS" },
    { "TwinJoyForward", "KEY_UP,JOY1_YAXIS_NEG" },
    { "TwinJoyReverse", "KEY_DOWN,JOY1_YAXIS_POS" },
    { "TwinJoyStrafeLeft", "KEY_LEFT,JOY1_XAXIS_NEG" },
    { "TwinJoyStrafeRight", "KEY_RIGHT,JOY1_XAXIS_POS" },
    { "TwinJoyJump", "KEY_E,JOY1_BUTTON1" },
    { "TwinJoyCrouch", "KEY_R,JOY1_BUTTON2" },
    { "TwinJoyLeft1", "NONE" },
    { "TwinJoyLeft2", "NONE" },
    { "TwinJoyRight1", "NONE" },
    { "TwinJoyRight2", "NONE" },
    { "TwinJoyUp1", "NONE" },
    { "TwinJoyUp2", "NONE" },
    { "TwinJoyDown1", "NONE" },
    { "TwinJoyDown2", "NONE" },
    { "TwinJoyShot1", "KEY_A,JOY1_BUTTON5" },
    { "TwinJoyShot2", "KEY_S,JOY1_BUTTON6" },
    { "TwinJoyTurbo1", "KEY_Z,JOY1_BUTTON7" },
    { "TwinJoyTurbo2", "KEY_X,JOY1_BUTTON8" },
    // analog stick (Star Wars Trilogy)
    { "AnalogJoyLeft", "KEY_LEFT" },
    { "AnalogJoyRight", "KEY_RIGHT" },
    { "AnalogJoyUp", "KEY_UP" },
    { "AnalogJoyDown", "KEY_DOWN" },
    { "AnalogJoyX", "JOY_XAXIS,MOUSE_XAXIS" },
    { "AnalogJoyY", "JOY_YAXIS,MOUSE_YAXIS" },
    { "AnalogJoyTrigger", "KEY_A,JOY_BUTTON1,MOUSE_LEFT_BUTTON" },
    { "AnalogJoyEvent", "KEY_S,JOY_BUTTON2,MOUSE_RIGHT_BUTTON" },
    { "AnalogJoyTrigger2", "KEY_D,JOY_BUTTON2" },
    { "AnalogJoyEvent2", "NONE" },
    // light guns (The Lost World)
    { "GunLeft", "KEY_LEFT" },
    { "GunRight", "KEY_RIGHT" },
    { "GunUp", "KEY_UP" },
    { "GunDown", "KEY_DOWN" },
    { "GunX", "MOUSE_XAXIS,JOY1_XAXIS" },
    { "GunY", "MOUSE_YAXIS,JOY1_YAXIS" },
    { "Trigger", "KEY_A,JOY1_BUTTON1,MOUSE_LEFT_BUTTON" },
    { "Offscreen", "KEY_S,JOY1_BUTTON2,MOUSE_RIGHT_BUTTON" },
    { "AutoTrigger", "0" },
    { "GunLeft2", "NONE" },
    { "GunRight2", "NONE" },
    { "GunUp2", "NONE" },
    { "GunDown2", "NONE" },
    { "GunX2", "JOY2_XAXIS" },
    { "GunY2", "JOY2_YAXIS" },
    { "Trigger2", "JOY2_BUTTON1" },
    { "Offscreen2", "JOY2_BUTTON2" },
    { "AutoTrigger2", "0" },
    // analog guns (Ocean Hunter, L.A. Machineguns)
    { "AnalogGunLeft", "KEY_LEFT" },
    { "AnalogGunRight", "KEY_RIGHT" },
    { "AnalogGunUp", "KEY_UP" },
    { "AnalogGunDown", "KEY_DOWN" },
    { "AnalogGunX", "MOUSE_XAXIS,JOY1_XAXIS" },
    { "AnalogGunY", "MOUSE_YAXIS,JOY1_YAXIS" },
    { "AnalogTriggerLeft", "KEY_A,JOY1_BUTTON1,MOUSE_LEFT_BUTTON" },
    { "AnalogTriggerRight", "KEY_S,JOY1_BUTTON2,MOUSE_RIGHT_BUTTON" },
    { "AnalogGunLeft2", "NONE" },
    { "AnalogGunRight2", "NONE" },
    { "AnalogGunUp2", "NONE" },
    { "AnalogGunDown2", "NONE" },
    { "AnalogGunX2", "NONE" },
    { "AnalogGunY2", "NONE" },
    { "AnalogTriggerLeft2", "NONE" },
    { "AnalogTriggerRight2", "NONE" },
    // Ski Champ
    { "SkiLeft", "KEY_LEFT" },
    { "SkiRight", "KEY_RIGHT" },
    { "SkiUp", "KEY_UP" },
    { "SkiDown", "KEY_DOWN" },
    { "SkiX", "JOY1_XAXIS" },
    { "SkiY", "JOY1_YAXIS" },
    { "SkiPollLeft", "KEY_A,JOY1_BUTTON1" },
    { "SkiPollRight", "KEY_S,JOY1_BUTTON2" },
    { "SkiSelect1", "KEY_Q,JOY1_BUTTON3" },
    { "SkiSelect2", "KEY_W,JOY1_BUTTON4" },
    { "SkiSelect3", "KEY_E,JOY1_BUTTON5" },
    // Magical Truck Adventure
    { "MagicalLeverUp1", "KEY_UP" },
    { "MagicalLeverDown1", "KEY_DOWN" },
    { "MagicalLeverUp2", "NONE" },
    { "MagicalLeverDown2", "NONE" },
    { "MagicalLever1", "JOY1_YAXIS" },
    { "MagicalLever2", "JOY2_YAXIS" },
    { "MagicalPedal1", "KEY_A,JOY1_BUTTON1" },
    { "MagicalPedal2", "KEY_S,JOY2_BUTTON1" },
    // Sega Bass Fishing / Get Bass
    { "FishingRodLeft", "KEY_LEFT" },
    { "FishingRodRight", "KEY_RIGHT" },
    { "FishingRodUp", "KEY_UP" },
    { "FishingRodDown", "KEY_DOWN" },
    { "FishingStickLeft", "KEY_A" },
    { "FishingStickRight", "KEY_D" },
    { "FishingStickUp", "KEY_W" },
    { "FishingStickDown", "KEY_S" },
    { "FishingRodX", "JOY1_XAXIS" },
    { "FishingRodY", "JOY1_YAXIS" },
    { "FishingStickX", "JOY1_RXAXIS" },
    { "FishingStickY", "JOY1_RYAXIS" },
    { "FishingReel", "KEY_SPACE,JOY1_ZAXIS_POS" },
    { "FishingCast", "KEY_Z,JOY1_BUTTON1" },
    { "FishingSelect", "KEY_X,JOY1_BUTTON2" },
    { "FishingTension", "KEY_T,JOY1_ZAXIS_NEG" },
  };

  // The text of a new Supermodel.ini.
  inline std::string IniText()
  {
    std::string text =
      "; Supermodel.ini\n"
      ";\n"
      "; Settings for the Model 3 engine. Lines are \"Name = value\"; text after a\n"
      "; ';' is a comment. Names must be spelled exactly, case included: a name the\n"
      "; program doesn't know is ignored (Supermodel.log lists what was read).\n"
      ";\n"
      "; [ Global ] applies to every game. A section named after a ROM set applies\n"
      "; to that game only and wins over Global, for example:\n"
      ";\n"
      ";   [ swtrilgy ]\n"
      ";   MusicVolume = 150\n"
      ";\n"
      "; Command line options win over both.\n"
      "\n"
      "[ Global ]\n"
      "\n"
      "; Picture\n"
      "New3DEngine = true\n"
      "QuadRendering = false\n"
      "WideScreen = false\n"
      "Stretch = false\n"
      "WideBackground = false\n"
      "Supersampling = 1\n"
      "; Colours as on an arcade monitor: 0 off, 1 ARI/D93 (Japanese games),\n"
      "; 2 PVM_20M2U/D93, 3 BT601_525/D93, 4 BT601_525/D65 (American games),\n"
      "; 5 BT601_625/D65 (European and Australian games)\n"
      "CRTcolors = 0\n"
      "; Smoothing of the scaled-up 2D layers: 0 none, 1 biquintic, 2 bilinear,\n"
      "; 3 bicubic\n"
      "UpscaleMode = 2\n"
      "; Frames per second. The real board runs at 57.524; 60 moves more evenly on\n"
      "; most monitors.\n"
      "RefreshRate = 60.000\n"
      "\n"
      "; Sound: the older SCSP DSP, for games the current one plays wrongly\n"
      "LegacySoundDSP = false\n"
      "\n"
      "; Linked cabinets (net board)\n"
      "Network = false\n"
      "SimulateNet = true\n"
      "PortIn = 1970\n"
      "PortOut = 1971\n"
      "AddressOut = \"127.0.0.1\"\n"
      "\n"
      "; Controls\n";
    for (const InputDefault &input : kInputs)
      text += std::string("Input") + input.name + " = \"" + input.mapping + "\"\n";
    return text;
  }
}

#endif // INCLUDED_DEFAULTCONFIGFILE_H
