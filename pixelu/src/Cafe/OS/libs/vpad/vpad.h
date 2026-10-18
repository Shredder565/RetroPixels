// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// vpad: the Wii U GamePad (buttons, sticks, motion, touch screen, rumble).
// VPADStatus is the guest-memory sample layout; input/emulated/VPADController
// fills it, including the touch screen from mouse or touch input on the
// emulator window.

#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/RPL/COSModule.h"

namespace vpad
{
	void load();
	// arms the sampling callback tick (coreinit init)
	void start();
	COSModule* GetModule();
}

#define VPAD_MAX_CONTROLLERS (2)

// button auto-repeat as handed to the emulated controllers
struct BtnRepeat
{
	sint32 delay, pulse;
};

// VPADTPData::validity
enum VPADTouchValidity
{
	kTpValid = 0,
	kTpInvalidX = 1,
	kTpInvalidY = 2,
	kTpInvalid = kTpInvalidX | kTpInvalidY,
};

// VPADTPData::touch
enum VPADTouchState
{
	kTpTouchOff = 0,
	kTpTouchOn = 1,
};

// orientation as three axis vectors
struct VPADDir
{
	beVec3D_t x;
	beVec3D_t y;
	beVec3D_t z;

	VPADDir() = default;
	VPADDir(const beVec3D_t& vx, const beVec3D_t& vy, const beVec3D_t& vz) : x(vx), y(vy), z(vz) {}
};
static_assert(sizeof(VPADDir) == 0x24);

// one touch screen sample; x/y are raw panel units (0..4095)
struct VPADTPData_t
{
	uint16be x;
	uint16be y;
	uint16be touch;
	uint16be validity;
};
static_assert(sizeof(VPADTPData_t) == 8);

struct VPADStatus
{
	/* +0x00 */ uint32be hold;
	/* +0x04 */ uint32be trig;
	/* +0x08 */ uint32be release;
	/* +0x0C */ beVec2D_t leftStick;
	/* +0x14 */ beVec2D_t rightStick;
	/* +0x1C */ beVec3D_t acc;
	/* +0x28 */ float32be accMagnitude;
	/* +0x2C */ float32be accAcceleration;
	/* +0x30 */ beVec2D_t accXY;
	/* +0x38 */ beVec3D_t gyroChange;
	/* +0x44 */ beVec3D_t gyroOrientation;
	/* +0x50 */ sint8 vpadErr;
	/* +0x51 */ uint8 padding1[1];
	/* +0x52 */ VPADTPData_t tpData;
	/* +0x5A */ VPADTPData_t tpProcessed1;
	/* +0x62 */ VPADTPData_t tpProcessed2;
	/* +0x6A */ uint8 padding2[2];
	/* +0x6C */ VPADDir dir;
	/* +0x90 */ uint8 headphoneStatus;
	/* +0x91 */ uint8 padding3[3];
	/* +0x94 */ beVec3D_t magnet;
	/* +0xA0 */ uint8 slideVolume;
	/* +0xA1 */ uint8 batteryLevel;
	/* +0xA2 */ uint8 micStatus;
	/* +0xA3 */ uint8 slideVolume2;
	/* +0xA4 */ uint8 padding4[8];
};
using VPADStatus_t = VPADStatus;
static_assert(sizeof(VPADStatus) == 0xAC);
static_assert(offsetof(VPADStatus, tpData) == 0x52);
static_assert(offsetof(VPADStatus, dir) == 0x6C);
static_assert(offsetof(VPADStatus, slideVolume) == 0xA0);
