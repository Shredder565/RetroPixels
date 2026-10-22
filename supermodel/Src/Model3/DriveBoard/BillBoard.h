// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The billboard: a Z80 board driving two players' 2-digit 7-segment
// displays and their lamps. Its outputs go to the OSD outputs (OutputBill1-5),
// as written by the board: segments a-g and the dot in bits 0-7, lamps P1/P2
// in bits 0/1, 0 = lit.

#ifndef INCLUDED_BILLBOARD_H
#define INCLUDED_BILLBOARD_H

#include "DriveBoard.h"
#include "Util/NewConfig.h"

class CBillBoard : public CDriveBoard
{
public:
  Game::DriveBoardType GetType(void) const;

  // (no force feedback: always 0)
  unsigned GetForceFeedbackStrength(void) const;
  void SetForceFeedbackStrength(unsigned strength);

  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // (no inputs)
  void AttachInputs(CInputs *inputs, unsigned gameInputFlags);

  CBillBoard(const Util::Config::Node &config);
  ~CBillBoard(void);

  UINT8 IORead8(UINT32 portNum);
  void IOWrite8(UINT32 portNum, UINT8 data);
};

#endif // INCLUDED_BILLBOARD_H
