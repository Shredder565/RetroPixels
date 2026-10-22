// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The ski pad (see SkiBoard.h).
//
// Commands from the PowerPC (high nibble, low nibble n):
//   1 0    vibration off           4 n    vibration n * 0x11
//   A 5    motor test (0x55)       A 6    end of motor test
//   D n    read mode n & 7         others ignored
// Replies: CF, CE, CD, CC (five frames each) while starting up, then 80;
// once started, by read mode: 0 a status byte that counts up on every read
// (the game reports an error if it stays the same; its bit 4 is shown as the
// foot clutch in the output test), 1 DIP switch 1, others FF (4: both foot
// sensors pressed).

#include "SkiBoard.h"
#include "Supermodel.h"
#include "Inputs/Input.h"

Game::DriveBoardType CSkiBoard::GetType(void) const
{
  return Game::DRIVE_BOARD_SKI;
}

unsigned CSkiBoard::GetForceFeedbackStrength() const
{
  return 0;
}

void CSkiBoard::SetForceFeedbackStrength(unsigned)
{
}

void CSkiBoard::LoadState(CBlockFile *SaveState)
{
  CDriveBoard::LoadState(SaveState);
  if (!Inactive())
    SetVibration(0);
}

void CSkiBoard::Halt(void)
{
  SetVibration(0);
  CDriveBoard::Halt();
}

Result CSkiBoard::Init(const UINT8 *romPtr)
{
  const Result result = CDriveBoard::Init(romPtr);
  m_simulate = true;
  return result;
}

void CSkiBoard::Reset(void)
{
  CDriveBoard::Reset();
  m_sentVibration = 0;
  if (!m_config["ForceFeedback"].ValueAsDefault<bool>(false))
    Halt();
  if (!Inactive())
    SetVibration(0);
}

UINT8 CSkiBoard::Read(void)
{
  if (Inactive())
    return 0xFF;
  return SimReply();
}

void CSkiBoard::Write(UINT8 data)
{
  if (Inactive())
    return;
  if (m_simulate)
    SimCommand(data);
}

UINT8 CSkiBoard::SimReply(void)
{
  if (!m_ready)
  {
    static const UINT8 kStartup[] = { 0xCF, 0xCE, 0xCD, 0xCC };
    if (m_startupFrames / 5 < 4)
      return kStartup[m_startupFrames / 5];
    m_ready = true;
    return 0x80;
  }
  switch (m_simSelect)
  {
  case 0: return m_simStatus++;
  case 1: return m_dipA;
  default: return 0xFF;
  }
}

void CSkiBoard::SimCommand(UINT8 cmd)
{
  const UINT8 val = cmd & 0xF;
  switch (cmd >> 4)
  {
  case 0x1:
    if (val == 0)
      SetVibration(0);
    break;
  case 0x4:
    SetVibration(val * 0x11);
    break;
  case 0xA:   // service menu tests
    if (val == 5)
      SetVibration(5 * 0x11);
    else if (val == 6)
      SetVibration(0);
    break;
  case 0xD:
    m_simSelect = val & 7;
    break;
  default:
    break;   // (0x: nothing, 88: board test passed?, Cx: game state)
  }
}

void CSkiBoard::RunFrame(void)
{
  if (m_simulate && !m_ready)
    m_startupFrames++;
}

UINT8 CSkiBoard::Read8(UINT32)
{
  return 0xFF;
}

void CSkiBoard::Write8(UINT32, UINT8)
{
}

void CSkiBoard::SetVibration(UINT8 val)
{
  if (val == m_sentVibration)
    return;
  ForceFeedbackCmd cmd;
  cmd.id = FFVibrate;
  cmd.force = (float)val / 255.0f;
  m_in->skiX->SendForceFeedbackCmd(cmd);
  m_sentVibration = val;
}

CSkiBoard::CSkiBoard(const Util::Config::Node &config)
  : CDriveBoard(config)
{
  m_present = false;
  m_simulate = false;
  m_ready = false;
  m_dipA = 0xCF;
}

CSkiBoard::~CSkiBoard(void)
{
}
