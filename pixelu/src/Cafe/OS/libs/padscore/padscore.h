// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// padscore: Wii Remote, Nunchuk, Classic and Pro Controller input (WPAD is the
// raw controller layer, KPAD the processed layer most games use).
//
// The structures below are the guest-memory layouts the console's padscore
// library uses; input/emulated/WPADController fills them.

#include "util/math/vector3.h"
#include "Cafe/OS/RPL/COSModule.h"

namespace padscore
{
	// arms the periodic connect/extension/sampling callback tick (coreinit init)
	void start();
	COSModule* GetModule();
}

constexpr int kWPADMaxControllers = 4; // Wii Remotes
constexpr int kKPADMaxControllers = 7; // Wii Remotes plus Pro Controllers

#define WPAD_ERR_NONE				0
#define WPAD_ERR_NO_CONTROLLER		-1
#define WPAD_ERR_BUSY				-2
#define WPAD_ERR_INVALID			-4

#define WPAD_PRESS_UNITS			4 // balance board sensors
#define WPAD_DPD_MAX_OBJECTS		4 // IR points seen by the pointer camera

#define WPAD_FMT_CORE				0
#define WPAD_FMT_CORE_ACC			1
#define WPAD_FMT_CORE_ACC_DPD		2

// big-endian float vectors as games see them
struct beVec2D_t
{
	float32be x;
	float32be y;
};
static_assert(sizeof(beVec2D_t) == 0x8);

struct beVec3D_t
{
	float32be x;
	float32be y;
	float32be z;

	beVec3D_t() = default;
	beVec3D_t(const Vector3<float>& v) : x(v.x), y(v.y), z(v.z) {}
	beVec3D_t(const float32be& vx, const float32be& vy, const float32be& vz) : x(vx), y(vy), z(vz) {}
};
static_assert(sizeof(beVec3D_t) == 0xC);
using beVec3D = beVec3D_t;

// --- KPAD -------------------------------------------------------------------

// extension part of a KPAD sample; which member is valid depends on devType
union KPADEXStatus_t
{
	struct // Nunchuk
	{
		/* +0x00 */ beVec2D_t stick;
		/* +0x08 */ beVec3D_t acc;
		/* +0x14 */ float32be accValue;
		/* +0x18 */ float32be accSpeed;
	} fs;
	struct // Classic Controller
	{
		/* +0x00 */ uint32be hold;
		/* +0x04 */ uint32be trig;
		/* +0x08 */ uint32be release;
		/* +0x0C */ beVec2D_t lstick;
		/* +0x14 */ beVec2D_t rstick;
		/* +0x1C */ float32be ltrigger;
		/* +0x20 */ float32be rtrigger;
	} cl;
	struct // Pro Controller
	{
		/* +0x00 */ uint32be hold;
		/* +0x04 */ uint32be trig;
		/* +0x08 */ uint32be release;
		/* +0x0C */ beVec2D_t lstick;
		/* +0x14 */ beVec2D_t rstick;
		/* +0x1C */ uint32be charge;
		/* +0x20 */ uint32be cable;
	} uc;
	struct // buttons only
	{
		/* +0x00 */ uint32be hold;
		/* +0x04 */ uint32be trig;
		/* +0x08 */ uint32be release;
	} cm;
	uint8 _size[0x50];
};
static_assert(sizeof(KPADEXStatus_t) == 0x50);

// MotionPlus attitude (three axis vectors)
struct KPADMPDir_t
{
	beVec3D_t X;
	beVec3D_t Y;
	beVec3D_t Z;
};
static_assert(sizeof(KPADMPDir_t) == 0x24);

struct KPADMPStatus_t
{
	/* +0x00 */ beVec3D_t mpls;  // angular speed
	/* +0x0C */ beVec3D_t angle; // integrated rotation
	/* +0x18 */ KPADMPDir_t dir;
};
static_assert(sizeof(KPADMPStatus_t) == 0x3C);

struct KPADStatus_t
{
	/* +0x00 */ uint32be hold;
	/* +0x04 */ uint32be trig;
	/* +0x08 */ uint32be release;
	/* +0x0C */ beVec3D_t acc;
	/* +0x18 */ float32be acc_value;
	/* +0x1C */ float32be acc_speed;
	/* +0x20 */ beVec2D_t pos;     // pointer position
	/* +0x28 */ beVec2D_t vec;
	/* +0x30 */ float32be speed;
	/* +0x34 */ beVec2D_t horizon;
	/* +0x3C */ beVec2D_t horiVec;
	/* +0x44 */ float32be horiSpeed;
	/* +0x48 */ float32be dist;
	/* +0x4C */ float32be distVec;
	/* +0x50 */ float32be distSpeed;
	/* +0x54 */ beVec2D_t accVertical;
	/* +0x5C */ uint8 devType;
	/* +0x5D */ sint8 wpadErr;
	/* +0x5E */ sint8 dpd_valid_fg;
	/* +0x5F */ uint8 data_format;
	/* +0x60 */ KPADEXStatus_t ex_status;
	/* +0xB0 */ KPADMPStatus_t mpls;
	/* +0xEC */ uint8 _unusedEC[4];
};
static_assert(sizeof(KPADStatus_t) == 0xF0);
static_assert(offsetof(KPADStatus_t, ex_status) == 0x60);
static_assert(offsetof(KPADStatus_t, mpls) == 0xB0);

// --- WPAD raw samples ---------------------------------------------------------

struct DPDObject_t
{
	uint16be x;
	uint16be y;
	uint16be size;
	uint8 traceId;
	uint8 _pad;
};
static_assert(sizeof(DPDObject_t) == 0x8);

// core part shared by every format
struct WPADStatus_t
{
	/* +0x00 */ uint16be button;
	/* +0x02 */ uint16be accX;
	/* +0x04 */ uint16be accY;
	/* +0x06 */ uint16be accZ;
	/* +0x08 */ DPDObject_t obj[WPAD_DPD_MAX_OBJECTS];
	/* +0x28 */ uint8 dev;
	/* +0x29 */ sint8 err;
};
static_assert(sizeof(WPADStatus_t) == 0x2A);

struct WPADFSStatus_t : WPADStatus_t // Nunchuk
{
	/* +0x2A */ uint16be fsAccX;
	/* +0x2C */ uint16be fsAccY;
	/* +0x2E */ uint16be fsAccZ;
	/* +0x30 */ sint8 fsStickX;
	/* +0x31 */ sint8 fsStickY;
};
static_assert(sizeof(WPADFSStatus_t) == 0x32);

struct WPADCLStatus_t : WPADStatus_t // Classic Controller
{
	/* +0x2A */ uint16be clButton;
	/* +0x2C */ uint16be clLStickX;
	/* +0x2E */ uint16be clLStickY;
	/* +0x30 */ uint16be clRStickX;
	/* +0x32 */ uint16be clRStickY;
	/* +0x34 */ uint8 clTriggerL;
	/* +0x35 */ uint8 clTriggerR;
};
static_assert(sizeof(WPADCLStatus_t) == 0x36);

struct WPADUCStatus_t : WPADStatus_t // Pro Controller
{
	/* +0x2A */ uint8 _pad2A[2];
	/* +0x2C */ uint32be ucButton;
	/* +0x30 */ uint16be ucLStickX;
	/* +0x32 */ uint16be ucLStickY;
	/* +0x34 */ uint16be ucRStickX;
	/* +0x36 */ uint16be ucRStickY;
	/* +0x38 */ uint32be charge;
	/* +0x3C */ uint32be cable;
};
static_assert(sizeof(WPADUCStatus_t) == 0x40);

struct WPADTRStatus_t : WPADStatus_t // train controller
{
	/* +0x2A */ uint16be trButton;
	/* +0x2C */ uint8 brake;
	/* +0x2D */ uint8 mascon;
};
static_assert(sizeof(WPADTRStatus_t) == 0x2E);

struct WPADBLStatus_t : WPADStatus_t // Balance Board
{
	/* +0x2A */ uint16be press[WPAD_PRESS_UNITS];
	/* +0x32 */ sint8 temp;
	/* +0x33 */ uint8 battery;
};
static_assert(sizeof(WPADBLStatus_t) == 0x34);

struct WPADMPStatus_t : WPADStatus_t // MotionPlus with optional extension
{
	union
	{
		struct
		{
			uint16be fsAccX;
			uint16be fsAccY;
			uint16be fsAccZ;
			sint8 fsStickX;
			sint8 fsStickY;
		} fs;
		struct
		{
			uint16be clButton;
			uint16be clLStickX;
			uint16be clLStickY;
			uint16be clRStickX;
			uint16be clRStickY;
			uint8 clTriggerL;
			uint8 clTriggerR;
		} cl;
	} status;           // +0x2A
	uint8 stat;          // +0x36
	uint8 _ukn37;
	uint16be pitch;      // +0x38
	uint16be yaw;
	uint16be roll;
};
static_assert(sizeof(WPADMPStatus_t) == 0x3E);

// one entry of the ring buffer passed to KPADInitEx
struct KPADUnifiedWpadStatus_t
{
	union
	{
		WPADStatus_t core;
		WPADFSStatus_t fs;
		WPADCLStatus_t cl;
		WPADTRStatus_t tr;
		WPADBLStatus_t bl;
		WPADMPStatus_t mp;
	} u;
	/* +0x3E */ uint8 fmt;
	/* +0x3F */ uint8 _pad3F;
	/* +0x40 */ uint32 _ukn40;
};
static_assert(sizeof(KPADUnifiedWpadStatus_t) == 0x44);
using KPADUnifiedWpadStatus = KPADUnifiedWpadStatus_t;
