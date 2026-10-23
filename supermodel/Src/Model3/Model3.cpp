// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3 main board.
//
// PowerPC address map:
//   00000000-007FFFFF  RAM
//   84000000-8400003F  Real3D status registers
//   88000000           Real3D command port (a write flushes)
//   8C000000-8C3FFFFF  Real3D culling RAM, low
//   8E000000-8E0FFFFF  Real3D culling RAM, high
//   90000000-900000FF  Real3D VROM texture port
//   94000000           Real3D texture FIFO
//   98000000-983FFFFF  Real3D polygon RAM
//   9C000000-9CFFFFFF  Real3D configuration registers
//   C0000000-C003FFFF  step 1.5+: net board (shared RAM, registers, program RAM)
//   C0000000-C00000FF  step 1.x without a net board: 53C810 SCSI (if mapped there)
//   C1000000-C10000FF  53C810 SCSI
//   C2000000-C20000FF  Real3D DMA (step 2.x)
//   F0040000-F004003F  I/O board: controls, EEPROM, lamps, drive board, ADC
//   F0080000-F0080007  sound board MIDI ports
//   F00C0000-F00DFFFF  backup RAM
//   F0100000-F010003F  system registers
//   F0140000-F014003F  RTC-72421
//   F0180000-F019FFFF  security board RAM
//   F01A0000-F01A003F  security board registers
//   F0800CF8 / F0C00CF8  MPC105 config address / data (step 1.x)
//   F1000000-F111FFFF  tile generator RAM (patterns, names, palette)
//   F1180000-F11800FF  tile generator registers
//   F8FFF000-F8FFF0FF  MPC105 registers
//   F9000000-F90000FF  53C810 SCSI
//   FEC00000 / FEE00000  MPC106 config address / data (step 2.x)
//   FE0xxxxx           mirror of F0xxxxxx
//   FF000000-FF7FFFFF  CROM bank (CROM00-CROM15, selected at F0100008)
//   FF800000-FFFFFFFF  fixed CROM
//
// RAM and CROM are kept as little-endian 32-bit words, so the PowerPC's
// aligned 32-bit accesses need no conversion; narrower ones flip address
// bits instead. Little-endian devices (tile generator, Real3D, MPC10x) get
// their data byte reversed.
//
// I/O board inputs are active low. Register 04 is two banks (selected by
// bit 0 of register 00): bank 0 coins, test/service A and starts; bank 1
// service/test B and the EEPROM's data out (bit 5). Registers 08 and 0C
// are the game-specific controls of player 1 and 2 (0C also returns the
// drive board's reply), 3C the ADC (one channel per read).

#include "Model3.h"

#include <new>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Supermodel.h"
#include "DriveBoard/BillBoard.h"
#include "DriveBoard/JoystickBoard.h"
#include "DriveBoard/SkiBoard.h"
#include "DriveBoard/WheelBoard.h"
#include "Game.h"
#include "ROMSet.h"
#include "Network/NetBoard.h"
#include "Network/SimNetBoard.h"
#include "OSD/Audio.h"
#include "OSD/Video.h"
#include "Util/Format.h"
#include "Util/ByteSwap.h"
#include <functional>
#include <set>
#include <iostream>
#include <algorithm>

namespace
{
  // Memory pool: one allocation, in this order.
  enum : size_t
  {
    kMB = 0x100000,
    kRAMSize = 8 * kMB,
    kCROMSize = 8 * kMB + 128 * kMB,      // fixed, then banked
    kVROMSize = 64 * kMB,
    kBackupSize = 0x20000,
    kSecuritySize = 0x20000,
    kSoundROMSize = 0x80000,
    kSampleSize = 16 * kMB,
    kDSBROMSize = 0x20000,
    kMPEGSize = 16 * kMB,
    kDriveROMSize = 0x10000,
    kNetBufferSize = 0x20000,
    kNetRAMSize = 0x10000,

    kRAMAt = 0,
    kCROMAt = kRAMAt + kRAMSize,
    kVROMAt = kCROMAt + kCROMSize,
    kBackupAt = kVROMAt + kVROMSize,
    kSecurityAt = kBackupAt + kBackupSize,
    kSoundROMAt = kSecurityAt + kSecuritySize,
    kSampleAt = kSoundROMAt + kSoundROMSize,
    kDSBROMAt = kSampleAt + kSampleSize,
    kMPEGAt = kDSBROMAt + kDSBROMSize,
    kDriveROMAt = kMPEGAt + kMPEGSize,
    kNetBufferAt = kDriveROMAt + kDriveROMSize,
    kNetRAMAt = kNetBufferAt + kNetBufferSize,
    kPoolSize = kNetRAMAt + kNetRAMSize,
  };

  // An active-low input: clears bit `bit` while `value` is set.
  inline void Press(UINT8 &port, unsigned value, int bit)
  {
    port &= UINT8(~(value << bit));
  }

  inline UINT16 Word16(const UINT8 *p)
  {
    UINT16 v;
    std::memcpy(&v, p, 2);
    return v;
  }

  inline UINT32 Word32(const UINT8 *p)
  {
    UINT32 v;
    std::memcpy(&v, p, 4);
    return v;
  }

  inline void Store16(UINT8 *p, UINT16 v)
  {
    std::memcpy(p, &v, 2);
  }

  inline void Store32(UINT8 *p, UINT32 v)
  {
    std::memcpy(p, &v, 4);
  }

  // Four byte-wide registers read as one big-endian word.
  template <typename F>
  UINT32 BigEndian32(F byteAt)
  {
    return (UINT32(byteAt(0)) << 24) | (UINT32(byteAt(1)) << 16) | (UINT32(byteAt(2)) << 8) | UINT32(byteAt(3));
  }

  // ...and written as one.
  template <typename F>
  void SplitBigEndian32(UINT32 data, F writeByte)
  {
    writeByte(0, UINT8(data >> 24));
    writeByte(1, UINT8(data >> 16));
    writeByte(2, UINT8(data >> 8));
    writeByte(3, UINT8(data));
  }

  unsigned PowerPCClock(const Game &game, Util::Config::Node &config)
  {
    unsigned mhz = config["PowerPCFrequency"].ValueAsDefault<unsigned>(0);
    if (mhz == 0)
      mhz = (game.stepping == "1.0") ? 66 : (game.stepping == "1.5") ? 100 : 166;
    return mhz * 1000000;
  }

  // Writes a memory region to a file (-dump-memory), as big-endian words.
  void DumpRegion(const char *what, const char *file, UINT8 *data, size_t size, bool words32, bool words16)
  {
    FILE *fp = std::fopen(file, "wb");
    if (fp == nullptr)
    {
      ErrorLog("Unable to open '%s' for writing.", file);
      return;
    }
    if (words32)
      Util::FlipEndian32(data, size);
    else if (words16)
      Util::FlipEndian16(data, size);
    std::fwrite(data, 1, size, fp);
    std::fclose(fp);
    printf("Wrote %s to '%s'\n", what, file);
  }
}

/******************************************************************************
 I/O board
******************************************************************************/

UINT8 CModel3::ReadButtons(void)
{
  const CInputs &in = *m_inputs;
  UINT8 port = 0xFF;
  bool bank1 = (m_ioBank & 1) != 0;
  if (!bank1)
  {
    Press(port, in.coin[0]->value, 0);
    Press(port, in.coin[1]->value, 1);
    Press(port, in.test[0]->value, 2);
    Press(port, in.service[0]->value, 3);
    Press(port, in.start[0]->value, 4);
    Press(port, in.start[1]->value, 5);
    if (m_game.inputs & Game::INPUT_SKI)
    {
      Press(port, in.skiSelect3->value, 4);
      Press(port, in.skiPollLeft->value, 5);
      Press(port, in.skiSelect1->value, 6);
      Press(port, in.skiSelect2->value, 7);
    }
  }
  else
  {
    Press(port, in.service[1]->value, 6);
    Press(port, in.test[1]->value, 7);
    port = UINT8((port & 0xDF) | (m_eeprom.Read() << 5));
  }
  return port;
}

UINT8 CModel3::ReadGamePort1(void)
{
  const CInputs &in = *m_inputs;
  const uint32_t has = m_game.inputs;
  UINT8 port = 0xFF;

  if (has & Game::INPUT_SKI)
    Press(port, in.skiPollRight->value, 0);
  if (has & Game::INPUT_JOYSTICK1)
  {
    Press(port, in.down[0]->value, 4);
    Press(port, in.up[0]->value, 5);
    Press(port, in.right[0]->value, 6);
    Press(port, in.left[0]->value, 7);
  }
  if (has & Game::INPUT_FIGHTING)
  {
    Press(port, in.punch[0]->value, 0);
    Press(port, in.kick[0]->value, 1);
    Press(port, in.guard[0]->value, 2);
    Press(port, in.escape[0]->value, 3);
  }
  if (has & Game::INPUT_SPIKEOUT)
  {
    Press(port, in.beat->value, 0);
    Press(port, in.charge->value, 1);
    Press(port, in.shift->value, 2);
    Press(port, in.jump->value, 3);
  }
  if (has & Game::INPUT_SOCCER)
  {
    Press(port, in.longPass[0]->value, 0);
    Press(port, in.shoot[0]->value, 1);
    Press(port, in.shortPass[0]->value, 2);
  }
  if (has & Game::INPUT_VR4)
  {
    for (int i = 0; i < 4; i++)
      Press(port, in.vr[i]->value, i);
  }
  if (has & Game::INPUT_VIEWCHANGE)
    Press(port, in.viewChange->value, (has & Game::INPUT_HARLEY) ? 1 : 0);   // Harley: bit 1
  if (has & Game::INPUT_SHIFT4)
  {
    // Gears 1-4 as combinations of bits 4-6
    static const UINT8 kGear[5] = { 0x00, 0x50, 0x60, 0x10, 0x20 };
    unsigned gear = in.gearShift4->value;
    if (gear < 5)
      port &= UINT8(~kGear[gear]);
  }
  if (has & Game::INPUT_SHIFTUPDOWN)
  {
    bool harley = (has & Game::INPUT_HARLEY) != 0;
    if (in.gearShiftUp->value)
      port &= UINT8(~(harley ? 0x20 : 0x50));
    else if (in.gearShiftDown->value)
      port &= UINT8(~(harley ? 0x10 : 0x60));
  }
  if (has & Game::INPUT_HANDBRAKE)
    Press(port, in.handBrake->value, 1);
  if (has & Game::INPUT_HARLEY)
    Press(port, in.musicSelect->value, 0);
  if (has & Game::INPUT_GUN1)
    Press(port, in.trigger[0]->value, 0);
  if (has & Game::INPUT_ANALOG_JOYSTICK)
  {
    Press(port, in.analogJoyEvent1->value, 0);
    Press(port, in.analogJoyEvent2->value, 1);
    Press(port, in.analogJoyTrigger2->value, 4);
    Press(port, in.analogJoyTrigger1->value, 5);
  }
  if (has & Game::INPUT_TWIN_JOYSTICKS)
  {
    // Left stick and its buttons
    Press(port, in.twinJoyShot1->value, 0);
    Press(port, in.twinJoyTurbo1->value, 1);
    Press(port, in.twinJoyDown1->value, 4);
    Press(port, in.twinJoyUp1->value, 5);
    Press(port, in.twinJoyRight1->value, 6);
    Press(port, in.twinJoyLeft1->value, 7);
    // Moves that use both sticks (left stick's part); one of each group.
    if (in.twinJoyTurnLeft->value)          port &= ~0x10;  // 1 down, 2 up
    else if (in.twinJoyTurnRight->value)    port &= ~0x20;  // 1 up, 2 down
    else if (in.twinJoyForward->value)      port &= ~0x20;  // both up
    else if (in.twinJoyReverse->value)      port &= ~0x10;  // both down
    if (in.twinJoyStrafeLeft->value)        port &= ~0x80;  // both left
    else if (in.twinJoyStrafeRight->value)  port &= ~0x40;  // both right
    else if (in.twinJoyJump->value)         port &= ~0x80;  // 1 left, 2 right
    else if (in.twinJoyCrouch->value)       port &= ~0x40;  // 1 right, 2 left
  }
  if (has & Game::INPUT_ANALOG_GUN1)
  {
    Press(port, in.analogTriggerLeft[0]->value, 0);
    Press(port, in.analogTriggerRight[0]->value, 1);
  }
  if (has & Game::INPUT_MAGTRUCK)
    Press(port, in.magicalPedal1->value, 0);
  if (has & Game::INPUT_FISHING)
  {
    if (m_game.name == "getbassur")
    {
      Press(port, in.fishingCast->value, 0);
      Press(port, in.fishingSelect->value, 1);
    }
    else
    {
      // Get Bass: these two are active high.
      Press(port, !in.fishingCast->value, 4);
      Press(port, !in.fishingSelect->value, 5);
    }
  }
  return port;
}

UINT8 CModel3::ReadGamePort2(void)
{
  const CInputs &in = *m_inputs;
  const uint32_t has = m_game.inputs;
  UINT8 port = 0xFF;

  // The drive board answers here (a billboard has nothing to say).
  if (m_driveBoard->IsAttached() && m_driveBoard->GetType() != Game::DRIVE_BOARD_BILLBOARD)
    port = m_driveBoard->Read();

  if (has & Game::INPUT_JOYSTICK2)
  {
    Press(port, in.down[1]->value, 4);
    Press(port, in.up[1]->value, 5);
    Press(port, in.right[1]->value, 6);
    Press(port, in.left[1]->value, 7);
  }
  if (has & Game::INPUT_FIGHTING)
  {
    Press(port, in.punch[1]->value, 0);
    Press(port, in.kick[1]->value, 1);
    Press(port, in.guard[1]->value, 2);
    Press(port, in.escape[1]->value, 3);
  }
  if (has & Game::INPUT_SOCCER)
  {
    Press(port, in.longPass[1]->value, 0);
    Press(port, in.shoot[1]->value, 1);
    Press(port, in.shortPass[1]->value, 2);
  }
  if (has & Game::INPUT_TWIN_JOYSTICKS)
  {
    // Right stick, and its part of the two-stick moves
    Press(port, in.twinJoyShot2->value, 0);
    Press(port, in.twinJoyTurbo2->value, 1);
    Press(port, in.twinJoyDown2->value, 4);
    Press(port, in.twinJoyUp2->value, 5);
    Press(port, in.twinJoyRight2->value, 6);
    Press(port, in.twinJoyLeft2->value, 7);
    if (in.twinJoyTurnLeft->value)          port &= ~0x20;
    else if (in.twinJoyTurnRight->value)    port &= ~0x10;
    else if (in.twinJoyForward->value)      port &= ~0x20;
    else if (in.twinJoyReverse->value)      port &= ~0x10;
    if (in.twinJoyStrafeLeft->value)        port &= ~0x80;
    else if (in.twinJoyStrafeRight->value)  port &= ~0x40;
    else if (in.twinJoyJump->value)         port &= ~0x40;
    else if (in.twinJoyCrouch->value)       port &= ~0x80;
  }
  if (has & Game::INPUT_GUN2)
    Press(port, in.trigger[1]->value, 0);
  if (has & Game::INPUT_ANALOG_GUN2)
  {
    Press(port, in.analogTriggerLeft[1]->value, 0);
    Press(port, in.analogTriggerRight[1]->value, 1);
  }
  if (has & Game::INPUT_MAGTRUCK)
    Press(port, in.magicalPedal2->value, 0);
  return port;
}

UINT8 CModel3::ReadADC(void)
{
  const CInputs &in = *m_inputs;
  const uint32_t has = m_game.inputs;
  UINT8 ch[8] = {};

  // Later control sets take the channels over.
  if (has & Game::INPUT_VEHICLE)
  {
    ch[0] = UINT8(in.steering->value);
    ch[1] = UINT8(in.accelerator->value);
    ch[2] = UINT8(in.brake->value);
    if (has & Game::INPUT_HARLEY)
      ch[3] = UINT8(in.rearBrake->value);
  }
  if (has & Game::INPUT_ANALOG_JOYSTICK)
  {
    ch[0] = UINT8(in.analogJoyY->value);
    ch[1] = UINT8(in.analogJoyX->value);
  }
  if (has & (Game::INPUT_ANALOG_GUN1 | Game::INPUT_ANALOG_GUN2))
  {
    if (m_game.name == "lostwsga" || m_game.name == "lostwsgo")
    {
      // The Lost World: X, then Y inverted, per player
      ch[0] = UINT8(in.analogGunX[0]->value);
      ch[1] = UINT8(255 - UINT8(in.analogGunY[0]->value));
      ch[2] = UINT8(in.analogGunX[1]->value);
      ch[3] = UINT8(255 - UINT8(in.analogGunY[1]->value));
    }
    else
    {
      ch[0] = UINT8(in.analogGunX[0]->value);
      ch[1] = UINT8(in.analogGunX[1]->value);
      ch[2] = UINT8(in.analogGunY[0]->value);
      ch[3] = UINT8(in.analogGunY[1]->value);
    }
  }
  if (has & Game::INPUT_SKI)
  {
    ch[0] = UINT8(in.skiY->value);
    ch[1] = UINT8(in.skiX->value);
  }
  if (has & Game::INPUT_MAGTRUCK)
  {
    ch[0] = UINT8(in.magicalLever1->value);
    ch[1] = UINT8(in.magicalLever2->value);
  }
  if (has & Game::INPUT_FISHING)
  {
    ch[0] = UINT8(in.fishingRodY->value);
    ch[1] = UINT8(in.fishingRodX->value);
    ch[2] = UINT8(in.fishingTension->value);
    ch[3] = UINT8(in.fishingReel->value);
    ch[4] = UINT8(in.fishingStickY->value);
    ch[5] = UINT8(in.fishingStickX->value);
  }

  // Each read moves on to the next channel.
  return ch[(m_adcChannel++) & 7];
}

UINT8 CModel3::ReadIO(unsigned reg)
{
  switch (reg & 0x3F)
  {
  case 0x00: return m_ioBank;
  case 0x04: return ReadButtons();
  case 0x08: return ReadGamePort1();
  case 0x0C: return ReadGamePort2();
  case 0x10: return m_ioOut[0];
  case 0x14: return m_ioOut[1];
  case 0x18:
    // Unknown status. The Bass Fishing games report an I/O error after a
    // while unless it reads 01.
    if (m_game.name == "bassdx" || m_game.name == "getbassdx" || m_game.name == "getbass")
      return 0x01;
    return 0x7F;
  case 0x2C: return m_serial1;
  case 0x30: return m_serial2;
  case 0x34:  // serial FIFO flags (light guns)
    return (m_game.inputs & (Game::INPUT_GUN1 | Game::INPUT_GUN2)) ? 0x0C : 0x00;
  case 0x3C: return ReadADC();
  default:   return 0xFF;
  }
}

void CModel3::GunCommand(UINT8 command)
{
  if (command == 0x00)          // select gun register
  {
    m_gunReg = m_serial2;
    return;
  }
  if (command != 0x87)          // read gun register
  {
    DebugLog("I/O board: unknown serial command %02X\n", command);
    return;
  }
  m_serial1 = 0;
  m_serial2 = 0;
  if ((m_game.inputs & (Game::INPUT_GUN1 | Game::INPUT_GUN2)) == 0)
    return;
  const CInputs &in = *m_inputs;
  if (m_gunReg < 8)
  {
    // Registers 0-7: per player (4 each) Y then X, each as low 8 bits
    // and high 2 bits.
    unsigned player = m_gunReg >> 2;
    unsigned pos = (m_gunReg & 2) ? in.gunX[player]->value : in.gunY[player]->value;
    m_serial2 = UINT8((m_gunReg & 1) ? ((pos >> 8) & 3) : (pos & 0xFF));
  }
  else if (m_gunReg == 8)       // off screen: bit 0 player 1, bit 1 player 2
    m_serial2 = UINT8((in.trigger[1]->offscreenValue << 1) | in.trigger[0]->offscreenValue);
  else
    DebugLog("I/O board: unknown gun register %X\n", m_gunReg);
}

void CModel3::WriteIO(unsigned reg, UINT8 data)
{
  switch (reg & 0x3F)
  {
  case 0x00:  // bank select; bits 5-7 are the EEPROM's lines
    m_eeprom.Write((data >> 6) & 1, (data >> 7) & 1, (data >> 5) & 1);
    m_ioBank = data;
    break;
  case 0x10:  // drive board
    if (m_driveBoard->IsAttached())
      m_driveBoard->Write(data);
    if (m_outputs != nullptr)
      m_outputs->SetValue(OutputRawDrive, data);
    m_ioOut[0] = data;
    break;
  case 0x14:  // lamps
    if (m_outputs != nullptr)
    {
      m_outputs->SetValue(OutputLampStart, !!(data & 0x04));
      m_outputs->SetValue(OutputLampView1, !!(data & 0x08));
      m_outputs->SetValue(OutputLampView2, !!(data & 0x10));
      m_outputs->SetValue(OutputLampView3, !!(data & 0x20));
      m_outputs->SetValue(OutputLampView4, !!(data & 0x40));
      m_outputs->SetValue(OutputLampLeader, !!(data & 0x80));
      m_outputs->SetValue(OutputRawLamps, data);
    }
    m_ioOut[1] = data;
    break;
  case 0x24: GunCommand(data); break;
  case 0x28: m_serial2 = data; break;
  case 0x3C: m_adcChannel = data & 7; break;
  default:   break;
  }
}

/******************************************************************************
 Security board
******************************************************************************/

uint16_t CModel3::ReadCryptRAM(uint32_t wordAddr)
{
  // The decryption device sees the RAM as 16-bit words, one per 32 bits.
  if (wordAddr >= 0x8000)
    return 0;
  return uint16_t(Word32(&m_securityRAM[wordAddr * 4]) >> 16);
}

UINT32 CModel3::ReadSecurity(unsigned reg)
{
  if (reg == 0x00)      // status
    return 0;
  if (reg == 0x1C)      // decrypted data, in the high half
  {
    if (m_securityFirstRead)
    {
      m_securityFirstRead = false;
      return 0xFFFF0000;
    }
    uint8_t *unused;
    return UINT32(m_crypto.Decrypt(&unused)) << 16;
  }
  DebugLog("Security board: read of register %X\n", reg);
  return 0xFFFFFFFF;
}

void CModel3::WriteSecurity(unsigned reg, UINT32 data)
{
  if (reg == 0x10 || reg == 0x14)       // restart the stream
  {
    m_crypto.SetAddressLow(0);
    m_crypto.SetAddressHigh(0);
    m_securityFirstRead = true;
  }
  else if (reg == 0x18)                 // sub-key, byte swapped
  {
    uint16_t key = uint16_t(data >> 16);
    m_crypto.SetSubKey(uint16_t((key >> 8) | (key << 8)));
  }
  else
    DebugLog("Security board: write of %08X to register %X\n", data, reg);
}

/******************************************************************************
 PCI device 16 (the board itself)
******************************************************************************/

UINT32 CModel3::ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
  if (bits == 8 || bits == 16)
    return 0;
  if (device == 16 && reg == 0)     // vendor and device ID (Daytona USA 2 checks it)
    return 0x182711DB;
  return 0;
}

void CModel3::WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data)
{
  DebugLog("Model 3: PCI write of %08X to device %u register %02X\n", data, device, reg);
}

/******************************************************************************
 System registers
******************************************************************************/

void CModel3::SelectCROMBank(unsigned value)
{
  // Active low: 0xFF selects CROM00.
  m_cromBankReg = value;
  unsigned bank = (~value) & 0xF;
  m_cromBank = &m_crom[8 * kMB + bank * 8 * kMB];
}

UINT8 CModel3::ReadSysReg(unsigned reg) const
{
  switch (reg & 0x3F)
  {
  case 0x08: return UINT8(m_cromBankReg);
  case 0x10: return UINT8(m_jtag.Read() << 5);  // TDO
  case 0x14: return m_irq.ReadIRQEnable();
  case 0x18: return m_irq.ReadIRQState();
  default:   return 0xFF;
  }
}

void CModel3::WriteSysReg(unsigned reg, UINT8 data)
{
  switch (reg & 0x3F)
  {
  case 0x08:
    SelectCROMBank(data);
    break;
  case 0x0C:  // JTAG: bit 7 TRST, 6 TCK, 5 TDI, 2 TMS
    m_jtag.Write((data >> 6) & 1, (data >> 2) & 1, (data >> 5) & 1, (data >> 7) & 1);
    break;
  case 0x14:
    m_irq.WriteIRQEnable(data);
    break;
  case 0x18:  // acknowledge
    m_irq.Deassert(data);
    break;
  default:    // 0D-0F, 1C: LEDs and the like
    break;
  }
}

/******************************************************************************
 Address space
******************************************************************************/

bool CModel3::SCSIAtC0(void) const
{
  return m_stepping <= 0x15 && m_scsi.GetBaseAddress() == 0xC0;
}

UINT8 CModel3::Read8(UINT32 addr)
{
  if (addr < kRAMSize)
    return m_ram[addr ^ 3];

  const unsigned area = addr >> 24;
  const unsigned sub = (addr >> 16) & 0xFF;
  switch (area)
  {
  case 0xFF:
    return (addr < 0xFF800000) ? m_cromBank[(addr & 0x7FFFFF) ^ 3] : m_crom[(addr & 0x7FFFFF) ^ 3];

  case 0xC2:
    return m_gpu.ReadDMARegister8(addr & 0xFF);

  case 0xF0:
  case 0xFE:
    if (sub == 0x04)
      return ReadIO(addr & 0x3F);
    if (sub == 0x08)      // MIDI data port 0 (0 keeps Magical Truck Adventure in Japan), control port 83
      return ((addr & 0xF) == 4) ? 0x83 : 0x00;
    if (sub == 0x10)
      return ReadSysReg(addr & 0x3F);
    if (sub == 0x14)      // RTC; byte 1 passes the battery test
    {
      if ((addr & 3) == 0)
        return m_rtc.ReadRegister((addr >> 2) & 0xF);
      return ((addr & 3) == 1) ? 0x03 : 0x00;
    }
    break;

  case 0xF1:
    if (addr < 0xF1120000)
      return m_tileGen.ReadRAM8(addr & 0x1FFFFF);
    break;

  case 0xC0:
    if (m_runNetBoard)
    {
      switch ((addr & 0x3FFFF) >> 16)
      {
      case 0:
        return m_netBoard->ReadCommRAM8((addr & 0xFFFF) ^ 2);
      case 1:
        if (addr > 0xC00101FF)
        {
          printf("R8 ATTENTION OUT OF RANGE\n");
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Info", "Out of Range", NULL);
        }
        return UINT8(m_netBoard->ReadIORegister((addr & 0x1FF) / 2));
      default:
        return m_netRAM[(addr & 0x1FFFF) / 2];
      }
    }
    if (!SCSIAtC0())
      break;
    return m_scsi.ReadRegister(addr & 0xFF);

  case 0xC1:
  case 0xF9:
    return m_scsi.ReadRegister(addr & 0xFF);

  default:
    printf("CMODEL3 : unknown R8 : %x\n", area);
    break;
  }

  DebugLog("PC=%08X\tread8 : %08X\n", ppc_get_pc(), addr);
  return 0xFF;
}

UINT16 CModel3::Read16(UINT32 addr)
{
  if (addr & 1)
    return UINT16((Read8(addr) << 8) | Read8(addr + 1));
  if (addr < kRAMSize)
    return Word16(&m_ram[addr ^ 2]);

  const unsigned area = addr >> 24;
  const unsigned sub = (addr >> 16) & 0xFF;
  switch (area)
  {
  case 0xFF:
    return Word16((addr < 0xFF800000) ? &m_cromBank[(addr & 0x7FFFFF) ^ 2] : &m_crom[(addr & 0x7FFFFF) ^ 2]);

  case 0xF0:
  case 0xFE:
    if (sub == 0x0C || sub == 0x0D)
      return Word16(&m_backupRAM[(addr & 0x1FFFF) ^ 2]);
    if (sub == 0xC0 && m_pciBridge.GetModel() == 0x105)
      return UINT16(m_pciBridge.ReadPCIConfigData(16, addr & 3));
    if (sub >= 0xE0 && sub <= 0xEF && m_pciBridge.GetModel() == 0x106)
      return UINT16(m_pciBridge.ReadPCIConfigData(16, addr & 3));
    break;

  case 0xF1:
    if (addr < 0xF1120000)
      return FLIPENDIAN16(m_tileGen.ReadRAM16(addr & 0x1FFFFF));
    break;

  case 0xC0:  // net board shared RAM (Spikeout)
    if (((addr & 0x3FFFF) >> 16) == 0)
      return FLIPENDIAN16(m_netBoard->ReadCommRAM16((addr & 0xFFFF) ^ 2));
    printf("CMODEL3 : unknown R16 : %x (C0)\n", addr);
    break;

  default:
    printf("CMODEL3 : unknown R16 : %x (%x)\n", addr, area);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Info", "CMODEL3 : Unknown R16", NULL);
    break;
  }

  DebugLog("PC=%08X\tread16: %08X\n", ppc_get_pc(), addr);
  return 0xFFFF;
}

UINT32 CModel3::Read32(UINT32 addr)
{
  if (addr & 3)
    return (UINT32(Read16(addr)) << 16) | Read16(addr + 2);
  if (addr < kRAMSize)
    return Word32(&m_ram[addr]);

  const unsigned area = addr >> 24;
  const unsigned sub = (addr >> 16) & 0xFF;
  switch (area)
  {
  case 0xFF:
    return Word32((addr < 0xFF800000) ? &m_cromBank[addr & 0x7FFFFF] : &m_crom[addr & 0x7FFFFF]);

  case 0x84:
    return FLIPENDIAN32(m_gpu.ReadRegister(addr & 0x3F));

  case 0xC2:
    return FLIPENDIAN32(m_gpu.ReadDMARegister32(addr & 0xFF));

  case 0xF0:
  case 0xFE:
    switch (sub)
    {
    case 0x04:
      return BigEndian32([&](unsigned i) { return ReadIO((addr & 0x3F) + i); });
    case 0x0C:
    case 0x0D:
      return Word32(&m_backupRAM[addr & 0x1FFFF]);
    case 0x10:
      return BigEndian32([&](unsigned i) { return ReadSysReg((addr & 0x3F) + i); });
    case 0x14:
      return (UINT32(m_rtc.ReadRegister((addr >> 2) & 0xF)) << 24) | 0x00030000;
    case 0x18:
    case 0x19:
      return Word32(&m_securityRAM[addr & 0x1FFFF]);
    case 0x1A:
      return ReadSecurity(addr & 0x3F);
    case 0xC0:
      if (m_pciBridge.GetModel() == 0x105)
        return m_pciBridge.ReadPCIConfigData(32, 0);
      break;
    default:
      if (sub >= 0xE0 && sub <= 0xEF && m_pciBridge.GetModel() == 0x106)
        return m_pciBridge.ReadPCIConfigData(32, 0);
      break;
    }
    break;

  case 0xF1:
    if (addr < 0xF1120000)
      return FLIPENDIAN32(m_tileGen.ReadRAM32(addr & 0x1FFFFF));
    if (addr >= 0xF1180000 && addr < 0xF1180100)
      return FLIPENDIAN32(m_tileGen.ReadRegister(addr & 0xFF));
    break;

  case 0xC0:
    if (m_runNetBoard)
    {
      switch ((addr & 0x3FFFF) >> 16)
      {
      case 0:
      {
        UINT32 v = FLIPENDIAN32(m_netBoard->ReadCommRAM32(addr & 0xFFFF));
        return (v << 16) | (v >> 16);
      }
      case 1:
        if (addr > 0xC00101FF)
          printf("R32 ATTENTION OUT OF RANGE\n");
        return FLIPENDIAN32(UINT32(m_netBoard->ReadIORegister((addr & 0x1FF) / 2)));
      default:
        return FLIPENDIAN32(Word32(&m_netRAM[(addr & 0x1FFFF) / 2]) & 0x0000FFFF);
      }
    }
    if (!SCSIAtC0())
      break;
    // fall through
  case 0xC1:
  case 0xF9:
    return BigEndian32([&](unsigned i) { return m_scsi.ReadRegister((addr + i) & 0xFF); });

  default:
    printf("CMODEL3 : unknown R32 : %x\n", area);
    break;
  }

  DebugLog("PC=%08X\tread32: %08X\n", ppc_get_pc(), addr);
  return 0xFFFFFFFF;
}

UINT64 CModel3::Read64(UINT32 addr)
{
  UINT64 hi = Read32(addr);
  return (hi << 32) | Read32(addr + 4);
}

void CModel3::Write8(UINT32 addr, UINT8 data)
{
  if (addr < kRAMSize)
  {
    m_ram[addr ^ 3] = data;
    return;
  }

  const unsigned area = addr >> 24;
  const unsigned sub = (addr >> 16) & 0xFF;
  switch (area)
  {
  case 0xC2:
    m_gpu.WriteDMARegister8(addr & 0xFF, data);
    return;

  case 0xF0:
  case 0xFE:
    if (sub == 0x04)
      WriteIO(addr & 0x3F, data);
    else if (sub == 0x08)
    {
      if ((addr & 0xF) == 0)          // MIDI data
      {
        m_soundBoard.WriteMIDIPort(data);
        m_irq.Deassert(0x40);
      }
      else if ((addr & 0xF) == 4)     // MIDI control; bit 5 enables the MIDI IRQ
      {
        m_midiControl = data;
        if ((data & 0x20) == 0)
          m_irq.Deassert(0x40);
      }
    }
    else if (sub == 0x0C || sub == 0x0D)
      m_backupRAM[(addr & 0x1FFFF) ^ 3] = data;
    else if (sub == 0x10)
      WriteSysReg(addr & 0x3F, data);
    else if (sub == 0x14 && (addr & 3) == 0)
      m_rtc.WriteRegister((addr >> 2) & 0xF, data);
    return;

  case 0xF1:
    if (addr < 0xF1120000)
      m_tileGen.WriteRAM8(addr & 0x1FFFFF, data);
    return;

  case 0xF8:
    if (m_pciBridge.GetModel() == 0x105)
      m_pciBridge.WriteRegister(addr & 0xFF, data);
    return;

  case 0xC0:
    if (m_runNetBoard)
    {
      switch ((addr & 0x3FFFF) >> 16)
      {
      case 0:
        m_netBoard->WriteCommRAM8((addr & 0xFFFF) ^ 2, data);
        break;
      case 1:
        if (addr > 0xC00101FF)
          printf("W8 ATTENTION OUT OF RANGE\n");
        m_netBoard->WriteIORegister((addr & 0x1FF) / 2, data);
        break;
      default:
        m_netRAM[(addr & 0x1FFFF) / 2] = data;
        break;
      }
      return;
    }
    if (SCSIAtC0())
      m_scsi.WriteRegister(addr & 0xFF, data);
    return;

  case 0xC1:
  case 0xF9:
    m_scsi.WriteRegister(addr & 0xFF, data);
    return;

  default:
    DebugLog("PC=%08X\twrite8 : %08X=%02X\n", ppc_get_pc(), addr, data);
    return;
  }
}

void CModel3::Write16(UINT32 addr, UINT16 data)
{
  if (addr & 1)
  {
    Write8(addr, UINT8(data >> 8));
    Write8(addr + 1, UINT8(data));
    return;
  }
  if (addr < kRAMSize)
  {
    Store16(&m_ram[addr ^ 2], data);
    return;
  }

  const unsigned area = addr >> 24;
  const unsigned sub = (addr >> 16) & 0xFF;
  switch (area)
  {
  case 0xF0:
  case 0xFE:
    if (sub == 0x0C || sub == 0x0D)
      Store16(&m_backupRAM[(addr & 0x1FFFF) ^ 2], data);
    else if (sub == 0xC0 && m_pciBridge.GetModel() == 0x105)
      m_pciBridge.WritePCIConfigData(16, addr & 2, data);
    return;

  case 0xF1:
    if (addr < 0xF1120000)
      m_tileGen.WriteRAM16(addr & 0x1FFFFF, FLIPENDIAN16(data));
    return;

  case 0xF8:
    if (m_pciBridge.GetModel() == 0x105)  // high byte first, as the PowerPC would
    {
      m_pciBridge.WriteRegister((addr & 0xFF) + 0, UINT8(data >> 8));
      m_pciBridge.WriteRegister((addr & 0xFF) + 1, UINT8(data));
    }
    return;

  case 0xC0:  // net board shared RAM (Ski Champ)
    if (((addr & 0x3FFFF) >> 16) == 0)
      m_netBoard->WriteCommRAM16((addr & 0xFFFF) ^ 2, FLIPENDIAN16(data));
    return;

  default:
    DebugLog("PC=%08X\twrite16: %08X=%04X\n", ppc_get_pc(), addr, data);
    return;
  }
}

void CModel3::Write32(UINT32 addr, UINT32 data)
{
  if (addr & 3)
  {
    Write16(addr, UINT16(data >> 16));
    Write16(addr + 2, UINT16(data));
    return;
  }
  if (addr < kRAMSize)
  {
    Store32(&m_ram[addr], data);
    return;
  }

  const unsigned area = addr >> 24;
  const unsigned sub = (addr >> 16) & 0xFF;
  switch (area)
  {
  // Real3D (little endian)
  case 0x88: m_gpu.Flush(); return;
  case 0x8C: m_gpu.WriteLowCullingRAM(addr & 0x3FFFFF, FLIPENDIAN32(data)); return;
  case 0x8E: m_gpu.WriteHighCullingRAM(addr & 0xFFFFF, FLIPENDIAN32(data)); return;
  case 0x90: m_gpu.WriteTexturePort(addr & 0xFF, FLIPENDIAN32(data)); return;
  case 0x94: m_gpu.WriteTextureFIFO(FLIPENDIAN32(data)); return;
  case 0x98: m_gpu.WritePolygonRAM(addr & 0x3FFFFF, FLIPENDIAN32(data)); return;
  case 0x9C: m_gpu.WriteConfigurationRegister(addr, FLIPENDIAN32(data)); return;
  case 0xC2: m_gpu.WriteDMARegister32(addr & 0xFF, FLIPENDIAN32(data)); return;

  case 0xF0:
  case 0xFE:
    switch (sub)
    {
    case 0x04:
      SplitBigEndian32(data, [&](unsigned i, UINT8 b) { WriteIO((addr & 0x3F) + i, b); });
      break;
    case 0x0C:
    case 0x0D:
      Store32(&m_backupRAM[addr & 0x1FFFF], data);
      break;
    case 0x10:
      SplitBigEndian32(data, [&](unsigned i, UINT8 b) { WriteSysReg((addr & 0x3F) + i, b); });
      break;
    case 0x14:
      m_rtc.WriteRegister((addr >> 2) & 0xF, UINT8(data));
      break;
    case 0x18:
    case 0x19:
      Store32(&m_securityRAM[addr & 0x1FFFF], data);
      break;
    case 0x1A:
      WriteSecurity(addr & 0x3F, data);
      break;
    case 0x80:  // MPC105 config address (F0800CF8)
      if (m_pciBridge.GetModel() == 0x105)
        m_pciBridge.WritePCIConfigAddress(data);
      break;
    default:
      if (sub >= 0xC0 && sub <= 0xEF)
      {
        const unsigned model = m_pciBridge.GetModel();
        if (model == 0x105 && addr >= 0xF0C00CF8 && addr < 0xF0C00D00)
          m_pciBridge.WritePCIConfigData(32, 0, data);
        else if (model == 0x106 && addr >= 0xFEC00000 && addr < 0xFEE00000)
          m_pciBridge.WritePCIConfigAddress(data);
        else if (model == 0x106 && addr >= 0xFEE00000 && addr < 0xFEF00000)
          m_pciBridge.WritePCIConfigData(32, 0, data);
      }
      break;
    }
    return;

  case 0xF1:
    if (addr < 0xF1120000)
    {
      m_tileGen.WriteRAM32(addr & 0x1FFFFF, FLIPENDIAN32(data));
      return;
    }
    if (addr >= 0xF1180000 && addr < 0xF1180100)
    {
      m_tileGen.WriteRegister(addr & 0xFF, FLIPENDIAN32(data));
      if (addr == 0xF118000C)
        m_gpu.TilegenDrawFrame(FLIPENDIAN32(data));
      return;
    }
    break;

  case 0xF8:
    if (m_pciBridge.GetModel() == 0x105)
      SplitBigEndian32(data, [&](unsigned i, UINT8 b) { m_pciBridge.WriteRegister((addr & 0xFF) + i, b); });
    return;

  case 0xC0:
    if (m_runNetBoard)
    {
      switch ((addr & 0x3FFFF) >> 16)
      {
      case 0:
      {
        UINT32 v = FLIPENDIAN32(data);
        m_netBoard->WriteCommRAM32(addr & 0xFFFF, (v << 16) | (v >> 16));
        break;
      }
      case 1:
        if (addr > 0xC00101FF)
          printf("W32 ATTENTION OUT OF RANGE\n");
        m_netBoard->WriteIORegister((addr & 0x1FF) / 2, FLIPENDIAN16(UINT16(data >> 16)));
        break;
      default:
        Store16(&m_netRAM[(addr & 0x1FFFF) / 2], FLIPENDIAN16(UINT16(data >> 16)));
        break;
      }
      return;
    }
    if (!SCSIAtC0())
      return;
    // fall through
  case 0xC1:
  case 0xF9:
    SplitBigEndian32(data, [&](unsigned i, UINT8 b) { m_scsi.WriteRegister((addr & 0xFF) + i, b); });
    return;

  default:
    break;
  }

  if (m_runNetBoard)
    printf("CMODEL3 : unknown W32 : %x (%x) data=%d\n", addr, area, data);
  DebugLog("PC=%08X\twrite32: %08X=%08X\n", ppc_get_pc(), addr, data);
}

void CModel3::Write64(UINT32 addr, UINT64 data)
{
  Write32(addr, UINT32(data >> 32));
  Write32(addr + 4, UINT32(data));
}

/******************************************************************************
 Save states and NVRAM
******************************************************************************/

void CModel3::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("Model 3", __FILE__);
  SaveState->Write(&m_ioBank, sizeof(m_ioBank));
  SaveState->Write(&m_serial1, sizeof(m_serial1));
  SaveState->Write(&m_serial2, sizeof(m_serial2));
  SaveState->Write(&m_gunReg, sizeof(m_gunReg));
  SaveState->Write(&m_adcChannel, sizeof(m_adcChannel));
  SaveState->Write(&m_cromBankReg, sizeof(m_cromBankReg));
  SaveState->Write(&m_securityPtr, sizeof(m_securityPtr));
  SaveState->Write(m_ram, kRAMSize);
  SaveState->Write(m_backupRAM, kBackupSize);
  SaveState->Write(m_securityRAM, kSecuritySize);
  SaveState->Write(&m_midiControl, sizeof(m_midiControl));
  int32_t firstRead = m_securityFirstRead ? 1 : 0;
  SaveState->Write(&firstRead, sizeof(firstRead));

  ppc_save_state(SaveState);
  m_irq.SaveState(SaveState);
  m_pciBridge.SaveState(SaveState);
  m_scsi.SaveState(SaveState);
  m_eeprom.SaveState(SaveState);
  m_tileGen.SaveState(SaveState);
  m_gpu.SaveState(SaveState);
  m_soundBoard.SaveState(SaveState);   // with the DSB
  m_driveBoard->SaveState(SaveState);
  m_crypto.SaveState(SaveState);
  m_jtag.SaveState(SaveState);
}

void CModel3::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("Model 3") != Result::OKAY)
  {
    ErrorLog("Unable to load Model 3 core state. Save state file is corrupt.");
    return;
  }
  SaveState->Read(&m_ioBank, sizeof(m_ioBank));
  SaveState->Read(&m_serial1, sizeof(m_serial1));
  SaveState->Read(&m_serial2, sizeof(m_serial2));
  SaveState->Read(&m_gunReg, sizeof(m_gunReg));
  SaveState->Read(&m_adcChannel, sizeof(m_adcChannel));
  SaveState->Read(&m_cromBankReg, sizeof(m_cromBankReg));
  SelectCROMBank(m_cromBankReg);
  SaveState->Read(&m_securityPtr, sizeof(m_securityPtr));
  SaveState->Read(m_ram, kRAMSize);
  SaveState->Read(m_backupRAM, kBackupSize);
  SaveState->Read(m_securityRAM, kSecuritySize);
  SaveState->Read(&m_midiControl, sizeof(m_midiControl));
  int32_t firstRead = 0;
  SaveState->Read(&firstRead, sizeof(firstRead));
  m_securityFirstRead = (firstRead != 0);

  m_gpu.LoadState(SaveState);
  m_tileGen.LoadState(SaveState);
  m_eeprom.LoadState(SaveState);
  m_scsi.LoadState(SaveState);
  m_pciBridge.LoadState(SaveState);
  m_irq.LoadState(SaveState);
  ppc_load_state(SaveState);
  m_soundBoard.LoadState(SaveState);
  m_driveBoard->LoadState(SaveState);
  m_crypto.LoadState(SaveState);
  m_jtag.LoadState(SaveState);
}

void CModel3::SaveNVRAM(CBlockFile *NVRAM)
{
  m_eeprom.SaveState(NVRAM);
  NVRAM->NewBlock("Backup RAM", __FILE__);
  NVRAM->Write(m_backupRAM, kBackupSize);
}

void CModel3::LoadNVRAM(CBlockFile *NVRAM)
{
  m_eeprom.LoadState(NVRAM);
  if (NVRAM->FindBlock("Backup RAM") != Result::OKAY)
  {
    ErrorLog("Unable to load Model 3 backup RAM. NVRAM file is corrupt.");
    return;
  }
  NVRAM->Read(m_backupRAM, kBackupSize);
}

void CModel3::ClearNVRAM(void)
{
  std::memset(m_backupRAM, 0, kBackupSize);
  m_eeprom.Clear();
}

/******************************************************************************
 Frames
******************************************************************************/

void CModel3::RunFrame(void)
{
  UINT32 start = CThread::GetTicks();

  if (!m_multiThreaded)
  {
    // Everything in turn on this thread.
    RunMainBoardFrame();
    SyncGPUs();
    RenderFrame();
    RunSoundBoardFrame();
    if (m_driveBoard->IsAttached())
      RunDriveBoardFrame();
    if (m_netBoard->IsRunning())
      RunNetBoardFrame();
  }
  else
  {
    if (!StartThreads())
    {
      ThreadError("CModel3::RunFrame");
      return;
    }

    // Let the board threads do a frame each: the PowerPC (when the GPU
    // runs on its own thread) and the drive board. The sound board's
    // thread is paced by the audio callback instead.
    if ((m_gpuMultiThreaded && !m_mainGo->Post()) ||
        (m_driveBoard->IsAttached() && !m_driveGo->Post()))
    {
      ThreadError("CModel3::RunFrame");
      return;
    }
    if (!m_gpuMultiThreaded)
    {
      RunMainBoardFrame();
      SyncGPUs();
    }
    RenderFrame();

    if (!m_notifyLock->Lock())
    {
      ThreadError("CModel3::RunFrame");
      return;
    }
    while ((m_gpuMultiThreaded && !m_mainDone) || (m_driveBoard->IsAttached() && !m_driveDone))
    {
      if (!m_notifyCond->Wait(m_notifyLock))
      {
        ThreadError("CModel3::RunFrame");
        return;
      }
    }
    m_mainDone = false;
    m_soundDone = false;
    m_driveDone = false;
    if (!m_notifyLock->Unlock())
    {
      ThreadError("CModel3::RunFrame");
      return;
    }

    // The PowerPC thread waits now, so its GPU state can be copied.
    if (m_gpuMultiThreaded)
      SyncGPUs();
    if (m_netBoard->IsRunning() && m_config["SimulateNet"].ValueAs<bool>())
      RunNetBoardFrame();
  }

  m_timings.frameTicks = CThread::GetTicks() - start;
  m_timings.frameId++;
}

void CModel3::RunMainBoardFrame(void)
{
  UINT32 start = CThread::GetTicks();

  // 57.524160 Hz, 424 lines: 384 shown, 40 of blanking and sync (as on
  // System 24).
  unsigned clock = PowerPCClock(m_game, m_config);
  unsigned frameCycles = (unsigned)((float)clock / 57.524160f);
  unsigned lineCycles = frameCycles / 424;
  unsigned vblankCycles = lineCycles * 40;

  // Scale the PowerPC's timer to the emulated clock so it counts at the
  // right rate (the Virtua Striker 2 games time themselves by it).
  ppc_set_timer_ratio(ppc_get_bus_freq_multipler() * 2 * clock / ppc_get_cycles_per_sec());

  if (m_gpusReady)
  {
    m_tileGen.BeginVBlank();
    m_gpu.BeginVBlank();

    // Run until the game acknowledges IRQ 2 (VBlank), so MIDI interrupts
    // pending before the next one aren't missed (Ski Champ).
    while ((m_irq.ReadIRQEnable() & 0x02) && (m_irq.ReadIRQState() & 0x02) && vblankCycles > 1000)
    {
      ppc_execute(1000);
      vblankCycles -= 1000;
    }

    // With bit 5 of the MIDI control port set, the sound board interrupts
    // periodically for the next MIDI command (up to 129 per frame).
    for (int n = 0; (m_midiControl & 0x20) && (m_irq.ReadIRQEnable() & 0x40); n++)
    {
      m_irq.Assert(0x40);
      ppc_execute(1000);
      vblankCycles -= 1000;
      if (n == 128)
        break;
    }

    ppc_execute(vblankCycles);
    m_irq.Assert(0x0D);
    m_gpu.EndVBlank();
    m_tileGen.EndVBlank();
  }

  // The display. The Real3D's ping-pong buffers flip at the line in tile
  // generator register 08; IRQ 2 comes at the start of the last line
  // (The Lost World locks up otherwise).
  auto flipLine = m_tileGen.ReadRegister(0x08);
  for (unsigned line = 0; line < 384; line++)
  {
    if (line == flipLine)
      m_gpu.FlipPingPongBit();
    if (line == 383)
      m_irq.Assert(0x02);
    m_tileGen.DrawLine(line);
    ppc_execute(lineCycles);
  }

  m_timings.ppcTicks = CThread::GetTicks() - start;
}

void CModel3::SyncGPUs(void)
{
  UINT32 start = CThread::GetTicks();
  m_timings.syncSize = m_gpu.SyncSnapshots() + m_tileGen.SyncSnapshots();
  m_gpusReady = true;
  m_timings.syncTicks = CThread::GetTicks() - start;
}

void CModel3::RenderFrame(void)
{
  UINT32 start = CThread::GetTicks();
  if (BeginFrameVideo() && m_gpusReady)
  {
    // 2D layers below the 3D, the 3D, then the layers above it
    m_tileGen.BeginFrame();
    m_gpu.BeginFrame();
    m_tileGen.PreRenderFrame();
    m_tileGen.RenderFrameBottom();
    m_gpu.RenderFrame();
    m_tileGen.RenderFrameTop();
    m_gpu.EndFrame();
    m_tileGen.EndFrame();
    m_superAA->Draw();
  }
  EndFrameVideo();
  m_timings.renderTicks = CThread::GetTicks() - start;
}

bool CModel3::RunSoundBoardFrame(void)
{
  UINT32 start = CThread::GetTicks();
  bool full = m_soundBoard.RunFrame();
  m_timings.sndTicks = CThread::GetTicks() - start;
  return full;
}

void CModel3::RunDriveBoardFrame(void)
{
  UINT32 start = CThread::GetTicks();
  m_driveBoard->RunFrame();
  m_timings.drvTicks = CThread::GetTicks() - start;
}

void CModel3::RunNetBoardFrame(void)
{
  m_netBoard->RunFrame();
}

void CModel3::DumpTimings(void)
{
  const FrameTimings &t = m_timings;
  printf("PPC:%3ums%c render:%3ums%c sync:%4uK%c%3ums%c snd:%3ums%c drv:%3ums%c frame:%3ums%c\n",
    t.ppcTicks, (t.ppcTicks > t.renderTicks ? '!' : ','),
    t.renderTicks, (t.renderTicks > t.ppcTicks ? '!' : ','),
    t.syncSize / 1024, (t.syncSize / 1024 > 128 ? '!' : ','),
    t.syncTicks, (t.syncTicks > 1 ? '!' : ','),
    t.sndTicks, (t.sndTicks > 10 ? '!' : ','),
    t.drvTicks, (t.drvTicks > 10 ? '!' : ','),
    t.frameTicks, (t.frameTicks > 16 ? '!' : ' '));
}

FrameTimings CModel3::GetTimings(void)
{
  return m_timings;
}

/******************************************************************************
 Threads

 With MultiThreaded set, the sound board runs on its own thread, woken by
 the audio callback when the output needs more samples, and the drive
 board on another, one frame per frame. With GPUMultiThreaded the PowerPC
 too gets a thread, which runs a frame while this one renders the last.
******************************************************************************/

bool CModel3::ThreadError(const char *where)
{
  ErrorLog("Threading error in %s: %s\nSwitching back to single-threaded mode.\n", where, CThread::GetLastError());
  m_multiThreaded = false;
  return false;
}

bool CModel3::StartThreads(void)
{
  if (m_threadsStarted)
    return true;

  bool ok = true;
  if (m_gpuMultiThreaded)
    ok = ok && (m_mainGo = CThread::CreateSemaphore(0)) != nullptr;
  ok = ok && (m_soundWakeLock = CThread::CreateMutex()) != nullptr;
  ok = ok && (m_soundWakeCond = CThread::CreateCondVar()) != nullptr;
  if (m_driveBoard->IsAttached())
    ok = ok && (m_driveGo = CThread::CreateSemaphore(0)) != nullptr;
  ok = ok && (m_notifyLock = CThread::CreateMutex()) != nullptr;
  ok = ok && (m_notifyCond = CThread::CreateCondVar()) != nullptr;

  m_pause = false;
  m_stop = false;

  if (ok && m_gpuMultiThreaded)
    ok = (m_mainThread = CThread::CreateThread("MainBoard", MainBoardThreadEntry, this)) != nullptr;
  if (ok)
    ok = (m_soundThread = CThread::CreateThread("SoundBoardNoSync", SoundBoardThreadEntry, this)) != nullptr;
  if (ok && m_driveBoard->IsAttached())
    ok = (m_driveThread = CThread::CreateThread("DriveBoard", DriveBoardThreadEntry, this)) != nullptr;

  if (!ok)
  {
    ErrorLog("Unable to create threads and/or synchronization objects: %s\nSwitching back to single-threaded mode.\n", CThread::GetLastError());
    DeleteThreadObjects();
    m_multiThreaded = false;
    return false;
  }

  SetAudioCallback(AudioCallback, this);
  m_threadsStarted = true;
  return true;
}

// Waits (with m_notifyLock held) until no board thread is mid-frame.
bool CModel3::WaitForThreads(void)
{
  while (m_mainRunning || m_soundRunning || m_driveRunning)
  {
    if (!m_notifyCond->Wait(m_notifyLock))
      return false;
  }
  return true;
}

bool CModel3::PauseThreads(void)
{
  if (!m_threadsStarted)
    return true;
  if (!m_notifyLock->Lock())
    return ThreadError("CModel3::PauseThreads");
  m_pause = true;
  if (!WaitForThreads() || !m_notifyLock->Unlock())
    return ThreadError("CModel3::PauseThreads");
  return true;
}

bool CModel3::ResumeThreads(void)
{
  if (!m_threadsStarted)
    return true;
  if (!m_notifyLock->Lock())
    return ThreadError("CModel3::ResumeThreads");
  m_pause = false;
  if (!m_notifyLock->Unlock())
    return ThreadError("CModel3::ResumeThreads");
  return true;
}

bool CModel3::StopThreads(void)
{
  if (!m_threadsStarted)
    return true;

  SetAudioCallback(NULL, NULL);

  // Pause them, then tell them to exit and wake each in turn.
  if (!m_notifyLock->Lock())
    return ThreadError("CModel3::StopThreads");
  m_pause = true;
  if (!WaitForThreads())
    return ThreadError("CModel3::StopThreads");
  m_stop = true;
  if (!m_notifyLock->Unlock())
    return ThreadError("CModel3::StopThreads");

  if (m_mainThread != nullptr && m_mainGo->Post())
    m_mainThread->Wait();
  if (m_soundThread != nullptr && WakeSoundBoardThread())
    m_soundThread->Wait();
  if (m_driveThread != nullptr && m_driveGo->Post())
    m_driveThread->Wait();

  DeleteThreadObjects();
  m_threadsStarted = false;
  return true;
}

void CModel3::DeleteThreadObjects(void)
{
  delete m_mainThread;
  delete m_soundThread;
  delete m_driveThread;
  delete m_mainGo;
  delete m_driveGo;
  delete m_soundWakeLock;
  delete m_soundWakeCond;
  delete m_notifyLock;
  delete m_notifyCond;
  m_mainThread = m_soundThread = m_driveThread = nullptr;
  m_mainGo = m_driveGo = nullptr;
  m_soundWakeLock = m_notifyLock = nullptr;
  m_soundWakeCond = m_notifyCond = nullptr;
}

// A board thread's wait for its next frame: 1 to run it (and *running is
// set), 0 to exit, -1 on error.
int CModel3::WaitForWork(CSemaphore *go, bool *running)
{
  for (;;)
  {
    if (!go->Wait() || !m_notifyLock->Lock())
      return -1;
    int result = m_stop ? 0 : !m_pause ? 1 : 2;
    if (result == 1)
      *running = true;
    if (!m_notifyLock->Unlock())
      return -1;
    if (result != 2)
      return result;
  }
}

bool CModel3::FinishWork(bool *running, bool *done)
{
  if (!m_notifyLock->Lock())
    return false;
  *running = false;
  *done = true;
  return m_notifyCond->SignalAll() && m_notifyLock->Unlock();
}

int CModel3::MainBoardThread(void)
{
  for (;;)
  {
    int w = WaitForWork(m_mainGo, &m_mainRunning);
    if (w == 0)
      return 0;
    if (w < 0)
      break;
    RunMainBoardFrame();
    if (!FinishWork(&m_mainRunning, &m_mainDone))
      break;
  }
  ThreadError("the main board thread");
  return 1;
}

int CModel3::DriveBoardThread(void)
{
  for (;;)
  {
    int w = WaitForWork(m_driveGo, &m_driveRunning);
    if (w == 0)
      return 0;
    if (w < 0)
      break;
    RunDriveBoardFrame();
    if (!FinishWork(&m_driveRunning, &m_driveDone))
      break;
  }
  ThreadError("the drive board thread");
  return 1;
}

int CModel3::SoundBoardThread(void)
{
  for (;;)
  {
    // Wait for the audio callback's wake-up; run unless paused.
    bool run = false;
    while (!run)
    {
      if (!m_soundWakeLock->Lock())
        goto Error;
      while (!m_soundWake)
      {
        if (!m_soundWakeCond->Wait(m_soundWakeLock))
          goto Error;
      }
      m_soundWake = false;
      if (!m_notifyLock->Lock())
        goto Error;
      bool exit = m_stop;
      run = !exit && !m_pause;
      if (run)
        m_soundRunning = true;
      if (!m_notifyLock->Unlock() || !m_soundWakeLock->Unlock())
        goto Error;
      if (exit)
        return 0;
    }

    // Fill the audio buffer (or stop early on a pause).
    for (;;)
    {
      if (!m_notifyLock->Lock())
        goto Error;
      bool paused = m_pause;
      if (!m_notifyLock->Unlock())
        goto Error;
      if (paused || RunSoundBoardFrame())
        break;
    }

    if (!FinishWork(&m_soundRunning, &m_soundDone))
      goto Error;
  }

Error:
  ThreadError("the sound board thread");
  return 1;
}

bool CModel3::WakeSoundBoardThread(void)
{
  // Only a thread that isn't already running needs waking.
  if (!m_soundWakeLock->Lock() || !m_notifyLock->Lock())
    return ThreadError("WakeSoundBoardThread");
  bool wake = !m_soundRunning;
  if (!m_notifyLock->Unlock())
    return ThreadError("WakeSoundBoardThread");
  if (wake)
  {
    m_soundWake = true;
    if (!m_soundWakeCond->Signal())
      return ThreadError("WakeSoundBoardThread");
  }
  if (!m_soundWakeLock->Unlock())
    return ThreadError("WakeSoundBoardThread");
  return wake;
}

int CModel3::MainBoardThreadEntry(void *self)
{
  return static_cast<CModel3 *>(self)->MainBoardThread();
}

int CModel3::SoundBoardThreadEntry(void *self)
{
  return static_cast<CModel3 *>(self)->SoundBoardThread();
}

int CModel3::DriveBoardThreadEntry(void *self)
{
  return static_cast<CModel3 *>(self)->DriveBoardThread();
}

void CModel3::AudioCallback(void *self)
{
  static_cast<CModel3 *>(self)->WakeSoundBoardThread();
}

/******************************************************************************
 Set-up
******************************************************************************/

void CModel3::Reset(void)
{
  std::memset(m_ram, 0, kRAMSize);    // backup RAM is kept
  SelectCROMBank(0xFF);
  m_securityPtr = 0;
  m_securityFirstRead = true;
  m_ioBank = 0;
  m_serial1 = 0;
  m_serial2 = 0;
  m_adcChannel = 0;
  m_midiControl = 0;

  ppc_reset();
  m_irq.Reset();
  m_pciBridge.Reset();
  m_pciBus.Reset();
  m_scsi.Reset();
  m_rtc.Reset();
  m_eeprom.Reset();
  m_tileGen.Reset();
  m_gpu.Reset();
  m_soundBoard.Reset();
  m_jtag.Reset();
  if (m_driveBoard->IsAttached())
    m_driveBoard->Reset();
  m_crypto.Reset();
  m_netBoard->Reset();

  m_gpusReady = false;
  std::memset(&m_timings, 0, sizeof(m_timings));
  DebugLog("Model 3 reset\n");
}

const Game &CModel3::GetGame() const
{
  return m_game;
}

Result CModel3::LoadGame(const Game &game, const ROMSet &rom_set)
{
  m_game = Game();

  // ROMs into the memory map, mirrored where a game's are smaller:
  //   VROM 64 MB: up to 32 MB is repeated in the upper half.
  //   Banked CROM 128 MB: up to 64 MB is repeated in the upper half.
  //   Fixed CROM 8 MB: a smaller one sits at the top, with the start of
  //     the banked CROM below it.
  //   Samples 16 MB: up to 8 MB is repeated.
  // (The lower VROM copy is limited to 3,200,000 bytes, as Supermodel
  // always did.)
  ROM vrom = rom_set.get_rom("vrom");
  if (vrom.size <= 32 * kMB)
  {
    vrom.CopyTo(m_vrom, 32 * 100000);
    vrom.CopyTo(m_vrom + 32 * kMB, 32 * kMB);
  }
  else
    vrom.CopyTo(m_vrom, 64 * kMB);

  UINT8 *banked = m_crom + 8 * kMB;
  ROM bankedROM = rom_set.get_rom("banked_crom");
  if (bankedROM.size <= 64 * kMB)
  {
    bankedROM.CopyTo(banked, 64 * kMB);
    bankedROM.CopyTo(banked + 64 * kMB, 64 * kMB);
  }
  else
    bankedROM.CopyTo(banked, 128 * kMB);
  ROM fixed = rom_set.get_rom("crom");
  fixed.CopyTo(m_crom + 8 * kMB - fixed.size, fixed.size);
  if (fixed.size < 8 * kMB)
    bankedROM.CopyTo(m_crom, 8 * kMB - fixed.size);

  ROM samples = rom_set.get_rom("sound_samples");
  if (samples.size <= 8 * kMB)
  {
    samples.CopyTo(m_sampleROM, 8 * kMB);
    samples.CopyTo(m_sampleROM + 8 * kMB, 8 * kMB);
  }
  else
    samples.CopyTo(m_sampleROM, 16 * kMB);
  rom_set.get_rom("sound_program").CopyTo(m_soundROM, kSoundROMSize);
  rom_set.get_rom("mpeg_program").CopyTo(m_dsbROM, kDSBROMSize);
  rom_set.get_rom("mpeg_music").CopyTo(m_mpegROM, kMPEGSize);
  rom_set.get_rom("driveboard_program").CopyTo(m_driveROM, kDriveROMSize);

  // PowerPC code as little-endian 32-bit words, 68K as 16-bit words
  Util::FlipEndian32(m_crom, kCROMSize);
  Util::FlipEndian16(m_soundROM, kSoundROMSize);
  Util::FlipEndian16(m_sampleROM, kSampleSize);

  // CPU and PCI bridge by stepping
  PPC_CONFIG cpu;
  cpu.bus_frequency = BUS_FREQUENCY_66MHZ;
  if (game.stepping == "2.0" || game.stepping == "2.1")
  {
    cpu.pvr = PPC_MODEL_603R;               // 166 MHz
    cpu.bus_frequency_multiplier = 0x25;    // 2.5x
    m_pciBridge.SetModel(0x106);
  }
  else if (game.stepping == "1.5")
  {
    cpu.pvr = PPC_MODEL_603E;               // 100 MHz
    cpu.bus_frequency_multiplier = 0x15;
    m_pciBridge.SetModel(0x105);
  }
  else if (game.stepping == "1.0")
  {
    cpu.pvr = PPC_MODEL_603R;               // 66 MHz
    cpu.bus_frequency_multiplier = 0x10;
    m_pciBridge.SetModel(0x105);
  }
  else
  {
    ErrorLog("Cannot configure Model 3 because game uses unrecognized stepping (%s).", game.stepping.c_str());
    return Result::FAIL;
  }
  ppc_init(&cpu);
  ppc_attach_bus(this);
  m_fetch[0] = { 0x00000000, 0x007FFFFF, (UINT32 *) m_ram };
  m_fetch[1] = { 0xFF800000, 0xFFFFFFFF, (UINT32 *) m_crom };
  m_fetch[2] = { 0, 0, nullptr };
  ppc_set_fetch(m_fetch);

  m_stepping = ((game.stepping[0] - '0') << 4) | (game.stepping[2] - '0');
  m_gpu.SetStepping(m_stepping);
  m_jtag.SetStepping(m_stepping);

  // Digital Sound Board (MPEG music)
  if (rom_set.get_rom("mpeg_program").size)
  {
    if (game.mpeg_board == "DSB1")
    {
      m_dsb = new(std::nothrow) CDSB1(m_config);
      if (m_dsb == nullptr)
        return ErrorLog("Insufficient memory for Digital Sound Board object.");
    }
    else if (game.mpeg_board == "DSB2")
    {
      Util::FlipEndian16(m_dsbROM, kDSBROMSize);    // a 68K program
      m_dsb = new(std::nothrow) CDSB2(m_config);
      if (m_dsb == nullptr)
        return ErrorLog("Insufficient memory for Digital Sound Board object.");
    }
    else if (game.mpeg_board.empty())
      ErrorLog("No MPEG board type defined in game XML for MPEG ROMs.");
    else
      ErrorLog("Unknown MPEG board type '%s'. Only 'DSB1' and 'DSB2' are supported.", game.mpeg_board.c_str());
    if (m_dsb != nullptr && m_dsb->Init(m_dsbROM, m_mpegROM) != Result::OKAY)
      return Result::FAIL;
  }
  m_soundBoard.AttachDSB(m_dsb);

  // Drive board: one that needs a ROM only when the set has one. Ski
  // Champ's is simulated. Otherwise a stand-in that isn't attached.
  bool driveROM = rom_set.get_rom("driveboard_program").size != 0;
  Result driveInit;
  switch (game.driveboard_type)
  {
  case Game::DRIVE_BOARD_WHEEL:
  case Game::DRIVE_BOARD_JOYSTICK:
  case Game::DRIVE_BOARD_BILLBOARD:
    if (!driveROM)
      goto NoDriveBoard;
    if (game.driveboard_type == Game::DRIVE_BOARD_WHEEL)
      m_driveBoard = new CWheelBoard(m_config);
    else if (game.driveboard_type == Game::DRIVE_BOARD_JOYSTICK)
      m_driveBoard = new CJoyBoard(m_config);
    else
      m_driveBoard = new CBillBoard(m_config);
    driveInit = m_driveBoard->Init(m_driveROM);
    break;
  case Game::DRIVE_BOARD_SKI:
    m_driveBoard = new CSkiBoard(m_config);
    driveInit = m_driveBoard->Init(m_driveROM);
    break;
  default:
  NoDriveBoard:
    m_driveBoard = new CDriveBoard(m_config);
    driveInit = m_driveBoard->Init();
    break;
  }
  if (driveInit != Result::OKAY)
    return Result::FAIL;

  m_crypto.Init(game.encryption_key, std::bind(&CModel3::ReadCryptRAM, this, std::placeholders::_1));

  // What is being emulated
  std::set<std::string> extras;
  if (m_dsb != nullptr)
    extras.insert(Util::Format() << "Digital Sound Board (Type " << game.mpeg_board << ")");
  if (m_driveBoard->IsAttached())
    extras.insert(m_driveBoard->GetType() == Game::DRIVE_BOARD_BILLBOARD ? "Billboard" : "Drive Board");
  if (game.encryption_key)
    extras.insert("Security Board");
  if (game.netboard_present)
    extras.insert("Net Board");
  std::cout << "    Title:          " << game.title;
  if (!game.version.empty())
    std::cout << " (" << game.version << ")";
  std::cout << std::endl;
  std::cout << "    ROM Set:        " << game.name << std::endl;
  std::cout << "    Developer:      " << game.manufacturer << std::endl;
  std::cout << "    Year:           " << game.year << std::endl;
  std::cout << "    Stepping:       " << game.stepping << std::endl;
  if (!extras.empty())
    std::cout << "    Extra Hardware: " << Util::Format(", ").Join(extras) << std::endl;
  std::cout << std::endl;

  m_game = game;
  m_netBoard->GetGame(m_game);
  if (m_netBoard->Init(m_netRAM, m_netBuffer) != Result::OKAY)
    return Result::FAIL;
  m_runNetBoard = m_game.stepping != "1.0" && m_netBoard->IsAttached();
  return Result::OKAY;
}

void CModel3::AttachRenderers(CRender2D *Render2DPtr, IRender3D *Render3DPtr, SuperAA *superAA)
{
  m_tileGen.AttachRenderer(Render2DPtr);
  m_gpu.AttachRenderer(Render3DPtr);
  m_superAA = superAA;
}

void CModel3::AttachInputs(CInputs *InputsPtr)
{
  m_inputs = InputsPtr;
  if (m_driveBoard->IsAttached())
    m_driveBoard->AttachInputs(m_inputs, m_game.inputs);
}

void CModel3::AttachOutputs(COutputs *OutputsPtr)
{
  m_outputs = OutputsPtr;
  m_outputs->SetGame(m_game);
  m_outputs->Attached();
  if (m_driveBoard->IsAttached())
    m_driveBoard->AttachOutputs(m_outputs);
}

Result CModel3::Init(void)
{
  constexpr float poolMB = float(kPoolSize) / float(kMB);
  m_pool = new(std::nothrow) UINT8[kPoolSize];
  if (m_pool == nullptr)
    return ErrorLog("Insufficient memory for Model 3 object (needs %1.1f MB).", poolMB);
  std::memset(m_pool, 0, kPoolSize);
  m_ram = m_pool + kRAMAt;
  m_crom = m_pool + kCROMAt;
  m_vrom = m_pool + kVROMAt;
  m_backupRAM = m_pool + kBackupAt;
  m_securityRAM = m_pool + kSecurityAt;
  m_soundROM = m_pool + kSoundROMAt;
  m_sampleROM = m_pool + kSampleAt;
  m_dsbROM = m_pool + kDSBROMAt;
  m_mpegROM = m_pool + kMPEGAt;
  m_driveROM = m_pool + kDriveROMAt;
  m_netBuffer = m_pool + kNetBufferAt;
  m_netRAM = m_pool + kNetRAMAt;
  SelectCROMBank(0xFF);

  // (The PowerPC, the DSB and the security board are set up by LoadGame.)
  m_irq.Init();
  m_pciBridge.Init();
  m_pciBus.Init();
  m_scsi.Init(this, &m_irq, 0x100);   // SCSI and Real3D DMA: non-maskable, outside the 8 IRQ bits
  m_rtc.Init();
  m_eeprom.Init();
  if (m_tileGen.Init(&m_irq) != Result::OKAY)
    return Result::FAIL;
  if (m_gpu.Init(m_vrom, this, &m_irq, 0x100) != Result::OKAY)
    return Result::FAIL;
  if (m_soundBoard.Init(m_soundROM, m_sampleROM) != Result::OKAY)
    return Result::FAIL;

  m_pciBridge.AttachPCIBus(&m_pciBus);
  m_pciBus.AttachDevice(13, &m_gpu);
  m_pciBus.AttachDevice(14, &m_scsi);
  m_pciBus.AttachDevice(16, this);

  if (m_config["SimulateNet"].ValueAs<bool>())
    m_netBoard = new CSimNetBoard(m_config);
  else
    m_netBoard = new CNetBoard(m_config);

  DebugLog("Initialized Model 3 (allocated %1.1f MB)\n", poolMB);
  return Result::OKAY;
}

CSoundBoard *CModel3::GetSoundBoard(void)
{
  return &m_soundBoard;
}

CDriveBoard *CModel3::GetDriveBoard(void)
{
  return m_driveBoard;
}

INetBoard *CModel3::GetNetBoard(void)
{
  return m_netBoard;
}

CModel3::CModel3(Util::Config::Node &config)
  : m_config(config),
    m_multiThreaded(config["MultiThreaded"].ValueAs<bool>()),
    m_gpuMultiThreaded(config["GPUMultiThreaded"].ValueAs<bool>()),
    m_tileGen(config),
    m_gpu(config),
    m_soundBoard(config),
    m_jtag(m_gpu)
{
  std::memset(m_fetch, 0, sizeof(m_fetch));
  std::memset(&m_timings, 0, sizeof(m_timings));
}

CModel3::~CModel3(void)
{
  if (m_config["DumpMemory"].ValueAsDefault<bool>(false))
  {
    DumpRegion("RAM", "ram.bin", m_ram, kRAMSize, true, false);
    DumpRegion("VROM", "vrom.bin", m_vrom, kVROMSize, true, false);
    DumpRegion("CROM", "crom.bin", m_crom, 8 * kMB, true, false);
    DumpRegion("banked CROM", "banked_crom.bin", m_crom + 8 * kMB, 0x7000000, true, false);
    DumpRegion("sound ROM", "sound_rom.bin", m_soundROM, kSoundROMSize, false, true);
    DumpRegion("sample ROM", "sample_rom.bin", m_sampleROM, 8 * kMB, false, true);
  }
  StopThreads();
  delete[] m_pool;
  delete m_dsb;
  delete m_driveBoard;
  delete m_netBoard;
}
