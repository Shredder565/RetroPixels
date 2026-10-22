// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The billboard (see BillBoard.h).
//
// Z80 ports:
//   in  20 DIP switch 1 (0x80: segment test)   21 the PowerPC's command
//       26 lamp status (FF)                    others FF
//   out 22, 23 player 1 digits   24, 25 player 2 digits   26 lamps
//       (each also becomes the reply to the PowerPC)
//       28 03 enables the Z80's interrupt     2E 00 marks the board started
// It has no CTC (ports 10-13 are not passed on).

#include "BillBoard.h"
#include "Supermodel.h"

Game::DriveBoardType CBillBoard::GetType(void) const
{
  return Game::DRIVE_BOARD_BILLBOARD;
}

unsigned CBillBoard::GetForceFeedbackStrength() const
{
  return 0;
}

void CBillBoard::SetForceFeedbackStrength(unsigned)
{
}

// After the common block: "BillBoard" with DIP switch 1.
void CBillBoard::SaveState(CBlockFile *SaveState)
{
  CDriveBoard::SaveState(SaveState);
  SaveState->NewBlock("BillBoard", __FILE__);
  SaveState->Write(&m_dipA, sizeof(m_dipA));
}

void CBillBoard::LoadState(CBlockFile *SaveState)
{
  CDriveBoard::LoadState(SaveState);
  if (SaveState->FindBlock("BillBoard") != Result::OKAY)
  {
    ErrorLog("Unable to load billboard state. Save state file is corrupt.");
    return;
  }
  SaveState->Read(&m_dipA, sizeof(m_dipA));
}

void CBillBoard::AttachInputs(CInputs *, unsigned)
{
}

UINT8 CBillBoard::IORead8(UINT32 portNum)
{
  switch (portNum)
  {
  case 0x20: return m_dipA;
  case 0x21: return m_command;
  default: return 0xFF;   // (26: no lamp test)
  }
}

void CBillBoard::IOWrite8(UINT32 portNum, UINT8 data)
{
  static const EOutputs kOutput[] = { OutputBill1, OutputBill2, OutputBill3, OutputBill4, OutputBill5 };
  switch (portNum)
  {
  case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
    if (m_out != nullptr)
      m_out->SetValue(kOutput[portNum - 0x22], data);
    m_reply = data;
    break;
  case 0x28:
    if (data == 0x03)
      m_intEnabled = true;
    break;
  case 0x2E:
    if (data == 0x00)
      m_ready = true;
    break;
  default:
    break;
  }
}

CBillBoard::CBillBoard(const Util::Config::Node &config)
  : CDriveBoard(config)
{
  m_dipA = 0x0F;
  m_simulate = false;
  m_clockMHz = 8.0f;
}

CBillBoard::~CBillBoard(void)
{
}
