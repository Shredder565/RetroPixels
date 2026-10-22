// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The joystick drive board (force feedback on the analog joystick). It
// looks like the wheel board's hardware with a second motor: motor commands
// come out as effects on the joystick's X and Y axes.

#ifndef INCLUDED_JOYBOARD_H
#define INCLUDED_JOYBOARD_H

#include "DriveBoard.h"
#include "Util/NewConfig.h"
#include "Game.h"

class CJoyBoard : public CDriveBoard
{
public:
  Game::DriveBoardType GetType(void) const;

  void Get7SegDisplays(UINT8 &seg1Digit1, UINT8 &seg1Digit2, UINT8 &seg2Digit1, UINT8 &seg2Digit2) const;

  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);
  void Reset(void);
  UINT8 Read(void);
  void Write(UINT8 data);
  void RunFrame(void);

  CJoyBoard(const Util::Config::Node &config);
  ~CJoyBoard(void);

  UINT8 IORead8(UINT32 portNum);
  void IOWrite8(UINT32 portNum, UINT8 data);

protected:
  void Halt(void);

private:
  UINT8 m_digit[4] = { 0, 0, 0, 0 };

  // Serial ADC (saved: keep the types)
  UINT16 m_adcPort = 0;
  UINT8 m_adcBitsLeft = 0;

  // Motor ports and what was last acted on
  UINT8 m_motorX = 0;             // port 2A
  UINT8 m_motorY = 0;             // port 2D
  UINT8 m_motorControl = 0;       // port 2E
  UINT8 m_lastMotorX = 0;
  UINT8 m_lastMotorY = 0;
  UINT8 m_lastMotorControl = 0;
  UINT8 m_uncenter[2] = { 0, 0 }; // (saved; not used by this board)

  // Effects last sent
  INT8 m_sentForceX = 0;
  INT8 m_sentForceY = 0;
  UINT8 m_sentCentering = 0;
  UINT8 m_sentFriction = 0;
  UINT8 m_sentVibration = 0;

  UINT8 SimReply(void);
  void SimCommand(UINT8 data);
  void SimTick(void);
  void MotorUpdate(void);
  void StopEffects(void);
  void SetForce(INT8 val);
  void SetForceY(INT8 val);
  void SetCentering(UINT8 val);
  void SetFriction(UINT8 val);
  void SetVibration(UINT8 val);
  UINT8 AdcValue(UINT32 port) const;
};

#endif // INCLUDED_JOYBOARD_H
