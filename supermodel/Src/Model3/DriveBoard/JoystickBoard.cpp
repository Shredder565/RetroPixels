// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The joystick drive board (see JoystickBoard.h).
//
// Z80 ports (besides the common ones):
//   in  20/21 DIP switches 1/2      24-27 ADC channels, one bit per read
//       28 the PowerPC's command   2C encoder status (0: no error)
//   out 20-23 display digits        24-27 start an ADC conversion
//       29 reply to the PowerPC    2A X motor, 2D Y motor, 2E motor control
//       1C-1F parallel ports (ignored)
// ADC channels: 1 joystick Y, 2 joystick X (both 0x80 until the board is
// initialized, so its centering check passes), 3 and 4 0x80.
//
// Simulation is unfinished and never enabled.

#include "JoystickBoard.h"
#include "Supermodel.h"

namespace
{
  void SendEffect(CInput *input, EForceFeedback id, float force)
  {
    ForceFeedbackCmd cmd;
    cmd.id = id;
    cmd.force = force;
    input->SendForceFeedbackCmd(cmd);
  }
}

Game::DriveBoardType CJoyBoard::GetType(void) const
{
  return Game::DRIVE_BOARD_JOYSTICK;
}

void CJoyBoard::Get7SegDisplays(UINT8 &seg1Digit1, UINT8 &seg1Digit2, UINT8 &seg2Digit1, UINT8 &seg2Digit2) const
{
  seg1Digit1 = m_digit[0];
  seg1Digit2 = m_digit[1];
  seg2Digit1 = m_digit[2];
  seg2Digit2 = m_digit[3];
}

// After the common block: "JoystickBoard", laid out like "WheelBoard".
void CJoyBoard::SaveState(CBlockFile *SaveState)
{
  CDriveBoard::SaveState(SaveState);
  SaveState->NewBlock("JoystickBoard", __FILE__);
  SaveState->Write(m_simulate);
  if (m_simulate)
    return;
  SaveState->Write(m_dipA);
  SaveState->Write(m_dipB);
  SaveState->Write(m_adcPort);
  SaveState->Write(m_adcBitsLeft);
  SaveState->Write(m_uncenter[0]);
  SaveState->Write(m_uncenter[1]);
}

void CJoyBoard::LoadState(CBlockFile *SaveState)
{
  // The common block first (unlike the wheel board).
  CDriveBoard::LoadState(SaveState);

  if (SaveState->FindBlock("JoystickBoard") != Result::OKAY)
  {
    ErrorLog("Unable to load joystick drive board state. Save state file is corrupt.");
    Halt();
    return;
  }
  bool wasSimulated;
  SaveState->Read(&wasSimulated, sizeof(wasSimulated));
  if (wasSimulated)
  {
    ErrorLog("Save state contains unexpected data. Halting drive board emulation.");
    Halt();
    return;
  }
  SaveState->Read(m_dipA);
  SaveState->Read(m_dipB);
  SaveState->Read(m_adcPort);
  SaveState->Read(m_adcBitsLeft);
  SaveState->Read(m_uncenter[0]);
  SaveState->Read(m_uncenter[1]);
}

void CJoyBoard::Halt(void)
{
  StopEffects();
  CDriveBoard::Halt();
}

void CJoyBoard::Reset(void)
{
  CDriveBoard::Reset();

  for (UINT8 &d : m_digit)
    d = 0xFF;
  m_adcPort = 0;
  m_adcBitsLeft = 0;
  m_motorX = m_motorY = m_motorControl = 0;
  m_lastMotorX = m_lastMotorY = m_lastMotorControl = 0;
  m_uncenter[0] = m_uncenter[1] = 0;
  m_sentForceX = 0;
  m_sentForceY = 0;
  m_sentCentering = 0;
  m_sentFriction = 0;
  m_sentVibration = 0;
  m_simulate = false;

  if (!m_config["ForceFeedback"].ValueAsDefault<bool>(false))
    Halt();
  if (!Inactive())
    StopEffects();
}

UINT8 CJoyBoard::Read(void)
{
  if (Inactive())
    return 0xFF;
  return m_simulate ? SimReply() : CDriveBoard::Read();
}

void CJoyBoard::Write(UINT8 data)
{
  if (Inactive())
    return;
  if (m_simulate)
    SimCommand(data);
  else
  {
    CDriveBoard::Write(data);
    if (data == 0xCB)
      m_ready = false;
  }
}

void CJoyBoard::RunFrame(void)
{
  // (CDriveBoard::RunFrame() checks for a disabled board)
  if (m_simulate)
    SimTick();
  else
    CDriveBoard::RunFrame();
}


/******************************************************************************
 Simulation (unused)
******************************************************************************/

UINT8 CJoyBoard::SimReply(void)
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
  case 0: return m_simStatus;
  case 1: return m_dipA;
  case 2: return m_dipB;
  case 3: return m_simCenter;
  case 5: return (UINT8)m_in->analogJoyX->value;
  case 7: return m_simEcho;
  case 4:
  case 6: return 0x80;
  default: return 0xFF;
  }
}

void CJoyBoard::SimCommand(UINT8 cmd)
{
  const UINT8 val = cmd & 0xF;
  switch (cmd >> 4)
  {
  case 0x1: SetCentering(val * 0x11); break;
  case 0x2: SetFriction(val * 0x11); break;
  case 0x3: SetVibration(val * 0x11); break;
  case 0x5: SetForce((val + 1) * 0x5); break;
  case 0x6: SetForce(-(val + 1) * 0x5); break;
  case 0x8:
    switch (val & 7)
    {
    case 0: StopEffects(); break;
    case 1: SetForce(20); break;
    case 2: SetForce(-20); break;
    case 5: m_simCenter = (UINT8)m_in->analogJoyX->value; break;
    default: break;
    }
    break;
  case 0xC:
    StopEffects();
    if (val >= 0xB)
    {
      m_ready = false;
      m_startupFrames = 0;
    }
    else
      m_simMode = val;
    break;
  case 0xD: m_simSelect = val & 7; break;
  case 0xF: m_simEcho = val; break;
  default: break;
  }
}

void CJoyBoard::SimTick(void)
{
  if (!m_ready)
    m_startupFrames++;
}


/******************************************************************************
 Z80 ports
******************************************************************************/

UINT8 CJoyBoard::AdcValue(UINT32 port) const
{
  switch (port)
  {
  case 0x24: return m_ready ? (UINT8)m_in->analogJoyY->value : 0x80;
  case 0x25: return m_ready ? (UINT8)m_in->analogJoyX->value : 0x80;
  default: return 0x80;   // 26, 27
  }
}

UINT8 CJoyBoard::IORead8(UINT32 portNum)
{
  switch (portNum)
  {
  case 0x20: return m_dipA;
  case 0x21: return m_dipB;
  case 0x24: case 0x25: case 0x26: case 0x27:
    if (portNum == m_adcPort && m_adcBitsLeft-- > 0)
      return (AdcValue(portNum) >> m_adcBitsLeft) & 1;
    return 0xFF;
  case 0x28: return m_command;
  case 0x2C: return 0x00;
  default: return 0xFF;
  }
}

void CJoyBoard::IOWrite8(UINT32 portNum, UINT8 data)
{
  CDriveBoard::IOWrite8(portNum, data);
  switch (portNum)
  {
  case 0x20: case 0x21: case 0x22: case 0x23:
    m_digit[portNum - 0x20] = data;
    break;
  case 0x24: case 0x25: case 0x26: case 0x27:
    m_adcPort = (UINT16)portNum;
    m_adcBitsLeft = 8;
    break;
  case 0x29:
    m_reply = data;
    if (data == 0xCB)
      m_ready = true;
    break;
  case 0x2A:
    m_motorX = data;
    MotorUpdate();
    break;
  case 0x2D:
    m_motorY = data;
    MotorUpdate();
    break;
  case 0x2E:
    m_motorControl = data;
    break;
  default:
    break;
  }
}

// Acts on the motor ports when any of them changed since last time.
// Motor values are centered on 0x7F.
void CJoyBoard::MotorUpdate(void)
{
  if (m_motorX == m_lastMotorX && m_motorControl == m_lastMotorControl && m_motorY == m_lastMotorY)
    return;

  const UINT8 x = m_motorX, y = m_motorY;
  const bool centered = x == 0x7F && y == 0x81;   // both motors at rest: hold the stick centered
  switch (m_motorControl)
  {
  case 0xEE:   // constant force on both axes (Y inverted)
    SetForce((INT8)(2 * (x - 0x7F)));
    SetForceY((INT8)(-2 * (y - 0x7F)));
    SetCentering(centered ? 255 : 0);
    break;
  case 0xFF:
    if (x == 0xFF && y == 0xFF)
      StopEffects();
    break;
  case 0xCE:   // X force from the Y port
    if (x == 0x7F && y != 0x81)
      SetForce((INT8)(2 * y));
    if (centered)
      SetCentering(255);
    break;
  case 0xEC:   // Y force from the X port
    if (y == 0x81 && x != 0x7F)
      SetForceY((INT8)(2 * x));
    if (centered)
      SetCentering(255);
    break;
  default:
    break;   // (00, 99, CC, DD: start-up sequences)
  }

  m_lastMotorX = m_motorX;
  m_lastMotorControl = m_motorControl;
  m_lastMotorY = m_motorY;
}


/******************************************************************************
 Effects
******************************************************************************/

void CJoyBoard::StopEffects(void)
{
  ForceFeedbackCmd cmd{};
  cmd.id = FFStop;
  m_in->analogJoyX->SendForceFeedbackCmd(cmd);
  m_in->analogJoyY->SendForceFeedbackCmd(cmd);
  m_sentForceX = 0;
  m_sentCentering = 0;
  m_sentFriction = 0;
  m_sentVibration = 0;
  m_sentForceY = 0;
}

void CJoyBoard::SetForce(INT8 val)
{
  if (val == m_sentForceX)
    return;
  SendEffect(m_in->analogJoyX.get(), FFConstantForce, (float)val / (val >= 0 ? 127.0f : 128.0f));
  m_sentForceX = val;
}

void CJoyBoard::SetForceY(INT8 val)
{
  if (val == m_sentForceY)
    return;
  SendEffect(m_in->analogJoyY.get(), FFConstantForce, (float)val / (val >= 0 ? 127.0f : 128.0f));
  m_sentForceY = val;
}

void CJoyBoard::SetCentering(UINT8 val)
{
  if (val == m_sentCentering)
    return;
  SendEffect(m_in->analogJoyX.get(), FFSelfCenter, (float)val / 255.0f);
  SendEffect(m_in->analogJoyY.get(), FFSelfCenter, (float)val / 255.0f);
  m_sentCentering = val;
}

void CJoyBoard::SetFriction(UINT8 val)
{
  if (val == m_sentFriction)
    return;
  SendEffect(m_in->analogJoyX.get(), FFFriction, (float)val / 255.0f);
  m_sentFriction = val;
}

void CJoyBoard::SetVibration(UINT8 val)
{
  if (val == m_sentVibration)
    return;
  SendEffect(m_in->analogJoyX.get(), FFVibrate, (float)val / 255.0f);
  m_sentVibration = val;
}

CJoyBoard::CJoyBoard(const Util::Config::Node &config)
  : CDriveBoard(config)
{
  m_dipA = 0xCF;
  m_dipB = 0xFF;
}

CJoyBoard::~CJoyBoard(void)
{
}
