// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Cabinet outputs (lamps, the drive motor's raw value, billboard digits)
// passed on to outside programs such as MAMEHooker. COutputs keeps each
// output's value and tells the subclass when one changes; the subclasses
// send it on (NetOutputs: TCP/UDP, WinOutputs: Windows messages).

#ifndef INCLUDED_OUTPUTS_H
#define INCLUDED_OUTPUTS_H

#include "Game.h"
#include "Types.h"

// The outputs, numbered as the MAMEHooker protocol sees them (id = value + 1).
enum EOutputs
{
  OutputUnknown = -1,
  OutputPause = 0,
  OutputLampStart,
  OutputLampView1,
  OutputLampView2,
  OutputLampView3,
  OutputLampView4,
  OutputLampLeader,
  OutputRawDrive,
  OutputRawLamps,
  OutputBill1,
  OutputBill2,
  OutputBill3,
  OutputBill4,
  OutputBill5
};

#define NUM_OUTPUTS 14

class COutputs
{
public:
  // An output's protocol name ("LampStart"...), or NULL for none.
  static const char *GetOutputName(EOutputs output);

  // The output with this name (any case), or OutputUnknown.
  static EOutputs GetOutputByName(const char *name);

  virtual ~COutputs();

  // Sets the outputs up before they are attached. False on failure.
  virtual bool Initialize() = 0;

  // Called once the emulator has the outputs (and the game is known).
  virtual void Attached() = 0;

  const Game &GetGame() const;
  void SetGame(const Game &game);

  UINT8 GetValue(EOutputs output) const;

  // Stores the value; the subclass hears about it the first time and
  // whenever it changes.
  void SetValue(EOutputs output, UINT8 value);

  // True once the game has set this output at least once.
  bool HasValue(EOutputs output);

protected:
  COutputs();

  // An output got a new value.
  virtual void SendOutput(EOutputs output, UINT8 prevValue, UINT8 value) = 0;

private:
  Game m_game;
  bool m_set[NUM_OUTPUTS];
  UINT8 m_values[NUM_OUTPUTS];
};

#endif // INCLUDED_OUTPUTS_H
