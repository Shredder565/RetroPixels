// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The steering wheel drive board (Scud Race, Daytona 2, Sega Rally 2, ...):
// its Z80 program turns the game's commands into motor commands, which come
// out here as force feedback effects on the steering input. It also has two
// 7-segment displays and reads the wheel through a serial ADC.

#ifndef INCLUDED_WHEELBOARD_H
#define INCLUDED_WHEELBOARD_H

#include "DriveBoard.h"
#include "Util/NewConfig.h"
#include "Game.h"

class CWheelBoard : public CDriveBoard
{
public:
  Game::DriveBoardType GetType(void) const;

  // Segment patterns of the two 2-digit displays.
  void Get7SegDisplays(UINT8 &seg1Digit1, UINT8 &seg1Digit2, UINT8 &seg2Digit1, UINT8 &seg2Digit2) const;

  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);
  void Reset(void);
  UINT8 Read(void);
  void Write(UINT8 data);
  void RunFrame(void);

  CWheelBoard(const Util::Config::Node &config);
  ~CWheelBoard(void);

  UINT8 IORead8(UINT32 portNum);
  void IOWrite8(UINT32 portNum, UINT8 data);

protected:
  void Halt(void);

private:
  UINT8 m_digit[4] = { 0, 0, 0, 0 };   // display 1 left, right; display 2 left, right

  // Serial ADC: the port being read and the bits still to come. (Saved:
  // keep the types.)
  UINT16 m_adcPort = 0;
  UINT8 m_adcBitsLeft = 0;

  // Motor ports and what was last acted on
  UINT8 m_motorData = 0;          // port 2A
  UINT8 m_motorControl = 0;       // port 2E
  UINT8 m_lastMotorData = 0;
  UINT8 m_lastMotorControl = 0;
  UINT8 m_uncenter[2] = { 0, 0 }; // the two halves of an uncentering command (saved)

  // Effects last sent (repeats are not sent again)
  INT8 m_sentForce = 0;
  UINT8 m_sentCentering = 0;
  UINT8 m_sentFriction = 0;
  UINT8 m_sentVibration = 0;

  UINT8 SimReply(void);
  void SimCommand(UINT8 data);
  void SimTick(void);
  void MotorUpdate(void);
  void StopEffects(void);
  void SetForce(INT8 val);
  void SetCentering(UINT8 val);
  void SetFriction(UINT8 val);
  void SetVibration(UINT8 val);
  UINT8 AdcValue(UINT32 port) const;
};

#endif // INCLUDED_WHEELBOARD_H
