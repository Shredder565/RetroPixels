// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Ski Champ's vibrating ski pad. No ROM for its board is used: it is
// simulated, answering the PowerPC's commands directly, and its vibration
// goes to the ski input as force feedback.

#ifndef INCLUDED_SKIBOARD_H
#define INCLUDED_SKIBOARD_H

#include "DriveBoard.h"
#include "Util/NewConfig.h"
#include "Game.h"

class CSkiBoard : public CDriveBoard
{
public:
  Game::DriveBoardType GetType(void) const;

  // (no strength setting: always 0)
  unsigned GetForceFeedbackStrength(void) const;
  void SetForceFeedbackStrength(unsigned strength);

  void LoadState(CBlockFile *SaveState);
  Result Init(const UINT8 *romPtr);
  void Reset(void);
  UINT8 Read(void);
  void Write(UINT8 data);
  void RunFrame(void);

  CSkiBoard(const Util::Config::Node &config);
  ~CSkiBoard(void);

  // (no Z80 memory)
  UINT8 Read8(UINT32 addr);
  void Write8(UINT32 addr, UINT8 data);

protected:
  void Halt(void);

private:
  UINT8 m_sentVibration = 0;

  UINT8 SimReply(void);
  void SimCommand(UINT8 cmd);
  void SetVibration(UINT8 val);
};

#endif // INCLUDED_SKIBOARD_H
