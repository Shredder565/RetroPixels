// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The steering wheel drive board (see WheelBoard.h).
//
// Z80 ports (besides the common ones):
//   in  20/21 DIP switches 1/2      24-27 ADC channels, one bit per read
//       28 the PowerPC's command   2C encoder status (0: no error)
//   out 20-23 display digits        24-27 start an ADC conversion
//       29 reply to the PowerPC    2A motor data, 2E motor control
//       1C-1F, 2D ignored
// ADC channels: 1 nothing (0), 2 the wheel (0x80 until the board is
// initialized, so its centering check passes), 3 cockpit bank (0x80), 4 nothing.
//
// Simulation (answering the PowerPC without the Z80) is unfinished and
// never enabled; it follows the Scud Race command set.

#include "WheelBoard.h"
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

Game::DriveBoardType CWheelBoard::GetType(void) const
{
  return Game::DRIVE_BOARD_WHEEL;
}

void CWheelBoard::Get7SegDisplays(UINT8 &seg1Digit1, UINT8 &seg1Digit2, UINT8 &seg2Digit1, UINT8 &seg2Digit2) const
{
  seg1Digit1 = m_digit[0];
  seg1Digit2 = m_digit[1];
  seg2Digit1 = m_digit[2];
  seg2Digit2 = m_digit[3];
}

// After the common block: "WheelBoard" with simulated (bool), then (when
// not simulated) DIP 1, DIP 2, ADC port (16 bits), ADC bit, the two
// uncentering halves.
void CWheelBoard::SaveState(CBlockFile *SaveState)
{
  CDriveBoard::SaveState(SaveState);
  SaveState->NewBlock("WheelBoard", __FILE__);
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

void CWheelBoard::LoadState(CBlockFile *SaveState)
{
  // This board's block is read first; without it nothing is loaded.
  if (SaveState->FindBlock("WheelBoard") != Result::OKAY)
    return;

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

  CDriveBoard::LoadState(SaveState);
}

void CWheelBoard::Halt(void)
{
  StopEffects();
  CDriveBoard::Halt();
}

void CWheelBoard::Reset(void)
{
  CDriveBoard::Reset();

  for (UINT8 &d : m_digit)
    d = 0xFF;
  m_adcPort = 0;
  m_adcBitsLeft = 0;
  m_motorData = m_motorControl = 0;
  m_lastMotorData = m_lastMotorControl = 0;
  m_uncenter[0] = m_uncenter[1] = 0;
  m_sentForce = 0;
  m_sentCentering = 0;
  m_sentFriction = 0;
  m_sentVibration = 0;
  m_simulate = false;

  if (!m_config["ForceFeedback"].ValueAsDefault<bool>(false))
    Halt();
  if (!Inactive())
    StopEffects();   // anything still playing
}

UINT8 CWheelBoard::Read(void)
{
  if (Inactive())
    return 0xFF;
  return m_simulate ? SimReply() : CDriveBoard::Read();
}

void CWheelBoard::Write(UINT8 data)
{
  if (Inactive())
    return;
  if (m_simulate)
    SimCommand(data);
  else
  {
    CDriveBoard::Write(data);
    if (data == 0xCB)   // board reset
      m_ready = false;
  }
}

void CWheelBoard::RunFrame(void)
{
  if (Inactive())
    return;
  if (m_simulate)
    SimTick();
  else
    CDriveBoard::RunFrame();
}


/******************************************************************************
 Simulation (unused)
******************************************************************************/

UINT8 CWheelBoard::SimReply(void)
{
  if (!m_ready)
  {
    // Start-up: CF, CE, CD, CC (centering), five frames each, then 80.
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
  case 5: return (UINT8)m_in->steering->value;
  case 7: return m_simEcho;
  case 4:            // cockpit bank center
  case 6: return 0x80;   // cockpit bank position
  default: return 0xFF;
  }
}

void CWheelBoard::SimCommand(UINT8 cmd)
{
  const UINT8 val = cmd & 0xF;
  switch (cmd >> 4)
  {
  case 0x1: SetCentering(val * 0x11); break;   // centering strength (0 off)
  case 0x2: SetFriction(val * 0x11); break;     // friction (0 off)
  case 0x3: SetVibration(val * 0x11); break;      // uncentering (0 off)
  case 0x5: SetForce((val + 1) * 0x5); break;     // turn right
  case 0x6: SetForce(-(val + 1) * 0x5); break;    // turn left
  case 0x8:   // test mode
    switch (val & 7)
    {
    case 0: StopEffects(); break;
    case 1: SetForce(20); break;
    case 2: SetForce(-20); break;
    case 5: m_simCenter = (UINT8)m_in->steering->value; break;
    default: break;   // clutch, cockpit, lamp: nothing to do
    }
    break;
  case 0xC:   // board mode; CB-CF reset the board
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
  default: break;   // sequences, steering parameters, cabinet type: not simulated
  }
}

void CWheelBoard::SimTick(void)
{
  if (!m_ready)
    m_startupFrames++;
}


/******************************************************************************
 Z80 ports
******************************************************************************/

UINT8 CWheelBoard::AdcValue(UINT32 port) const
{
  switch (port)
  {
  case 0x25: return m_ready ? (UINT8)m_in->steering->value : 0x80;
  case 0x26: return 0x80;
  default: return 0x00;   // 24, 27
  }
}

UINT8 CWheelBoard::IORead8(UINT32 portNum)
{
  switch (portNum)
  {
  case 0x20: return m_dipA;
  case 0x21: return m_dipB;
  case 0x24: case 0x25: case 0x26: case 0x27:
    // the conversion's bits, most significant first
    if (portNum == m_adcPort && m_adcBitsLeft-- > 0)
      return (AdcValue(portNum) >> m_adcBitsLeft) & 1;
    return 0xFF;
  case 0x28: return m_command;
  case 0x2C: return 0x00;
  default: return 0xFF;
  }
}

void CWheelBoard::IOWrite8(UINT32 portNum, UINT8 data)
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
    if (data == 0xCC)
      m_ready = true;
    break;
  case 0x2A:
    m_motorData = data;
    MotorUpdate();
    break;
  case 0x2E:
    m_motorControl = data;
    break;
  default:
    break;
  }
}

// Acts on the motor ports when either changed since last time. The control
// byte selects what the data byte means.
void CWheelBoard::MotorUpdate(void)
{
  if (m_motorData == m_lastMotorData && m_motorControl == m_lastMotorControl)
    return;

  const UINT8 d = m_motorData;
  switch (m_motorControl)
  {
  case 0xFC:
    if (d & 0x04)
    {
      // Centering: bit 7 off, else strength in bits 3-6 (to 0x0F-0xFF).
      if (d & 0x80)
        SetCentering(0);
      else
        SetCentering((UINT8)(((d & 0x78) >> 3) * 0x10 + 0xF));
    }
    else
    {
      // Uncentering, in four nibbles (sequence number in bits 0-1, data
      // in bits 4-7) making two bytes. The mapping to a strength is a guess.
      const UINT8 nibble = (UINT8)((d & 0xF0) >> 4);
      const int seq = d & 3;
      if (seq == 0)
        m_uncenter[0] = (UINT8)(nibble << 4);
      else if (seq == 1)
        m_uncenter[0] |= nibble;
      else if (seq == 2)
        m_uncenter[1] = (UINT8)(nibble << 4);
      else
        m_uncenter[1] |= nibble;

      if (seq == 0 && m_uncenter[0] == 0)
        SetVibration(0);
      else if (seq == 3 && m_uncenter[0] > 0)
        SetVibration((UINT8)(((m_uncenter[0] >> 1) - 7) * 0x50 + ((m_uncenter[1] >> 1) - 5) * 0x10 + 0xF));
    }
    break;

  case 0xFE:
    // Constant force: 0x80 stop (0x7F and 0x81 too, in practice);
    // 0x82-0xC0 left, 0x40-0x7E right, beyond those the maximum.
    if (d > 0x81)
      SetForce(d <= 0xC0 ? (INT8)(2 * (0x81 - d)) : (INT8)-0x80);
    else if (d < 0x7F)
      SetForce(d >= 0x40 ? (INT8)(2 * (0x7F - d)) : (INT8)0x7F);
    else
      SetForce(0);
    break;

  case 0xFF:
    if (d == 0xFF)
      StopEffects();
    break;

  default:
    break;   // (FB: friction during power slides?, FD: unknown)
  }

  m_lastMotorData = m_motorData;
  m_lastMotorControl = m_motorControl;
}


/******************************************************************************
 Effects
******************************************************************************/

void CWheelBoard::StopEffects(void)
{
  if (m_in != nullptr && m_in->steering)
  {
    ForceFeedbackCmd cmd{};
    cmd.id = FFStop;
    m_in->steering->SendForceFeedbackCmd(cmd);
  }
  m_sentForce = 0;
  m_sentCentering = 0;
  m_sentFriction = 0;
  m_sentVibration = 0;
}

void CWheelBoard::SetForce(INT8 val)
{
  if (val == m_sentForce)
    return;
  SendEffect(m_in->steering.get(), FFConstantForce, (float)val / (val >= 0 ? 127.0f : 128.0f));
  m_sentForce = val;
}

void CWheelBoard::SetCentering(UINT8 val)
{
  if (val == m_sentCentering)
    return;
  SendEffect(m_in->steering.get(), FFSelfCenter, (float)val / 255.0f);
  m_sentCentering = val;
}

void CWheelBoard::SetFriction(UINT8 val)
{
  if (val == m_sentFriction)
    return;
  SendEffect(m_in->steering.get(), FFFriction, (float)val / 255.0f);
  m_sentFriction = val;
}

void CWheelBoard::SetVibration(UINT8 val)
{
  if (val == m_sentVibration)
    return;
  SendEffect(m_in->steering.get(), FFVibrate, (float)val / 255.0f);
  m_sentVibration = val;
}

CWheelBoard::CWheelBoard(const Util::Config::Node &config)
  : CDriveBoard(config)
{
  m_dipA = 0xCF;
  m_dipB = 0xFF;
}

CWheelBoard::~CWheelBoard(void)
{
}
