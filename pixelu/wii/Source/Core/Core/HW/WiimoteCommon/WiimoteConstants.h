// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Report IDs and other numbers of the Wii Remote's Bluetooth HID protocol.
// The values are those listed on WiiBrew's "Wiimote" page.

namespace WiimoteCommon
{
// Largest HID packet the remote exchanges, counting the leading HID header byte
// (0xa1/0xa2) as well as the report ID.
constexpr u8 MAX_PAYLOAD = 23;

// Reports sent by the remote to the host.
enum class InputReportID : u8
{
  // Pseudo-mode: data reporting is switched off. The remote never sends this ID.
  ReportDisabled = 0x00,

  Status = 0x20,
  ReadDataReply = 0x21,
  Ack = 0x22,

  // Data reporting modes (selected with output report 0x12).
  ReportCore = 0x30,
  ReportCoreAccel = 0x31,
  ReportCoreExt8 = 0x32,
  ReportCoreAccelIR12 = 0x33,
  ReportCoreExt19 = 0x34,
  ReportCoreAccelExt16 = 0x35,
  ReportCoreIR10Ext9 = 0x36,
  ReportCoreAccelIR10Ext6 = 0x37,
  ReportExt21 = 0x3d,
  ReportInterleave1 = 0x3e,
  ReportInterleave2 = 0x3f,
};

// Reports sent by the host to the remote.
enum class OutputReportID : u8
{
  Rumble = 0x10,
  LED = 0x11,
  ReportMode = 0x12,
  IRLogicEnable = 0x13,
  SpeakerEnable = 0x14,
  RequestStatus = 0x15,
  WriteData = 0x16,
  ReadData = 0x17,
  SpeakerData = 0x18,
  SpeakerMute = 0x19,
  IRLogicEnable2 = 0x1a,
};

// How the camera objects are packed into a data report.
enum class IRReportFormat : u8
{
  None,
  // 10 bytes, two objects per 5 bytes (reports 0x36 and 0x37).
  Basic,
  // 12 bytes, 3 bytes per object (report 0x33).
  Extended,
  // 36 bytes, 9 bytes per object, split over the interleaved reports 0x3e (first half)...
  Full1,
  // ...and 0x3f (second half).
  Full2,
};

// Player LED bits of output report 0x11.
enum class LED : u8
{
  None = 0x00,
  LED_1 = 0x10,
  LED_2 = 0x20,
  LED_3 = 0x40,
  LED_4 = 0x80
};

// Address space selector of the read/write memory reports (0x16 and 0x17).
enum class AddressSpace : u8
{
  // The 0x1700 byte user area of the internal EEPROM. The EEPROM itself sits on the i2c bus
  // at slave 0x50 (offset 0x70), but direct i2c access to it is refused.
  EEPROM = 0x00,
  // The i2c bus of the remote (speaker, camera, extension port).
  I2CBus = 0x01,
  // Also reaches the i2c bus. Real remotes accept it; games don't use it.
  I2CBusAlt = 0x02,
};

// Error field of the acknowledge (0x22) and read reply (0x21) reports.
enum class ErrorCode : u8
{
  Success = 0,
  // A read or write was attempted while a read was still in progress.
  Busy = 4,
  // The address space byte was not one of the values above.
  InvalidSpace = 6,
  // Nothing answered at the i2c slave address.
  Nack = 7,
  // Out-of-range EEPROM access, or an attempt to reach the EEPROM over i2c.
  InvalidAddress = 8,
};

}  // namespace WiimoteCommon
