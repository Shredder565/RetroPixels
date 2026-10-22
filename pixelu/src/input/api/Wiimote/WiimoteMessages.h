// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Report ids, registers and decoded state of a real Wii remote, as documented on
// wiibrew.org (Wiimote, Extension Controllers and Motion Plus pages).

// reports the remote sends
enum InputReportId : uint8
{
	kNone = 0,

	kStatus = 0x20,      // battery, LEDs, extension plugged in
	kRead = 0x21,        // answer to a memory read
	kAcknowledge = 0x22, // result of an output report

	// data reports: B = buttons, A = accelerometer, I = IR camera, E = extension bytes
	kDataCore = 0x30,         // BB
	kDataCoreAcc = 0x31,      // BB AAA
	kDataCoreExt8 = 0x32,     // BB E*8
	kDataCoreAccIR = 0x33,    // BB AAA I*12
	kDataCoreExt19 = 0x34,    // BB E*19
	kDataCoreAccExt = 0x35,   // BB AAA E*16
	kDataCoreIRExt = 0x36,    // BB I*10 E*9
	kDataCoreAccIRExt = 0x37, // BB AAA I*10 E*6
	kDataExt = 0x3d,          // E*21
};

// addresses in the remote's EEPROM / register space (24 bit)
enum RegisterAddress : uint32
{
	kRegisterCalibration = 0x16,  // accelerometer calibration in EEPROM
	kRegisterCalibration2 = 0x20, // its backup copy

	kRegisterIR = 0x4b00030,
	kRegisterIRSensitivity1 = 0x4b00000,
	kRegisterIRSensitivity2 = 0x4b0001a,
	kRegisterIRMode = 0x4b00033,

	kRegisterExtensionEncrypted = 0x4a40040,

	// writing 0x55 / 0x00 here starts an extension unencrypted
	kRegisterExtension1 = 0x4a400f0,
	kRegisterExtension2 = 0x4a400fb,
	kRegisterExtensionType = 0x4a400fa,        // 6 byte id
	kRegisterExtensionCalibration = 0x4a40020, // 16 bytes

	kRegisterMotionPlusDetect = 0x4a600fa,
	kRegisterMotionPlusInit = 0x4a600f0,
	kRegisterMotionPlusEnable = 0x4a600fe,
};

// 6 byte extension ids, read big endian
enum ExtensionType : uint64
{
	kExtensionNunchuck = 0x0000A4200000,
	kExtensionClassic = 0x0000A4200101,
	kExtensionClassicPro = 0x0100A4200101,
	kExtensionDrawsome = 0xFF00A4200013,
	kExtensionGuitar = 0x0000A4200103,
	kExtensionDrums = 0x0100A4200103,
	kExtensionBalanceBoard = 0x2A2C,

	kExtensionMotionPlusInactive = 0xa4200005,
	kExtensionMotionPlus = 0xa6200005,

	kExtensionPartialyInserted = 0xffffffffffff,
};

enum MemoryType : uint8
{
	kEEPROMMemory = 0,
	kRegisterMemory = 0x4,
};

// flags byte of a status report
enum StatusBitmask : uint8
{
	kBatteryEmpty = 0x1,
	kExtensionConnected = 0x2,
	kSpeakerEnabled = 0x4,
	kIREnabled = 0x8,
	kLed1 = 0x10,
	kLed2 = 0x20,
	kLed3 = 0x40,
	kLed4 = 0x80
};

// reports we send; bit 0 of the first payload byte always carries the rumble state
enum OutputReportId : uint8
{
	kLED = 0x11,
	kType = 0x12, // data reporting mode
	kIR = 0x13,
	kSpeakerState = 0x14,
	kStatusRequest = 0x15,
	kWriteMemory = 0x16,
	kReadMemory = 0x17,
	kSpeakerData = 0x18,
	kSpeakerMute = 0x19,
	kIR2 = 0x1A,
};

enum IRMode : uint8
{
	kIRDisabled,
	kBasicIR = 1,    // 10 bytes for 4 dots
	kExtendedIR = 3, // 12 bytes for 4 dots, with size
	kFullIR = 5,
};

// button ids of the remote: bit numbers of its two button bytes (read little endian)
enum WiimoteButtons
{
	kWiimoteButton_Left = 0,
	kWiimoteButton_Right = 1,
	kWiimoteButton_Down = 2,
	kWiimoteButton_Up = 3,
	kWiimoteButton_Plus = 4,

	kWiimoteButton_Two = 8,
	kWiimoteButton_One = 9,
	kWiimoteButton_B = 10,
	kWiimoteButton_A = 11,
	kWiimoteButton_Minus = 12,

	kWiimoteButton_Home = 15,

	// the Nunchuk's, our own numbering
	kWiimoteButton_C = 16,
	kWiimoteButton_Z = 17,

	// Classic Controller buttons are kHighestWiimote + ClassicButtons
	kHighestWiimote = 20,
};

// bit numbers in the Classic Controller's button word
enum ClassicButtons
{
	kClassicButton_R = 1,
	kClassicButton_Plus = 2,
	kClassicButton_Home = 3,
	kClassicButton_Minus = 4,
	kClassicButton_L = 5,
	kClassicButton_Down = 6,
	kClassicButton_Right = 7,
	kClassicButton_Up = 8,
	kClassicButton_Left = 9,
	kClassicButton_ZR = 10,
	kClassicButton_X = 11,
	kClassicButton_A = 12,
	kClassicButton_Y = 13,
	kClassicButton_B = 14,
	kClassicButton_ZL = 15,
};

// 10 bit accelerometer values at rest and at 1 g
struct Calibration
{
	glm::vec<3, uint16> zero{ 0x200, 0x200, 0x200 };
	glm::vec<3, uint16> gravity{ 0x240, 0x240, 0x240 };
};

struct IRDot
{
	bool visible = false;
	glm::vec2 pos;            // 0..1, x mirrored so it follows the pointer
	glm::vec<2, uint16> raw;  // 0..1023 / 0..767
	uint32 size;
};

struct NunchuchCalibration : Calibration
{
	glm::vec<2, uint8> min{};
	glm::vec<2, uint8> center{ 0x7f, 0x7f };
	glm::vec<2, uint8> max{ 0xff, 0xff };
};

struct MotionPlusData
{
	Calibration calibration{};

	glm::vec3 orientation; // yaw, roll, pitch
	bool slow_roll = false;
	bool slow_pitch = false;
	bool slow_yaw = false;
	bool extension_connected = false;
};

struct NunchuckData
{
	glm::vec3 acceleration{}, prev_acceleration{};
	NunchuchCalibration calibration{};

	bool c = false;
	bool z = false;

	glm::vec2 axis{};
	glm::vec<2, uint8> raw_axis{};

	MotionSample motion_sample{};
};

struct ClassicData
{
	glm::vec2 left_axis{};
	glm::vec<2, uint8> left_raw_axis{};

	glm::vec2 right_axis{};
	glm::vec<2, uint8> right_raw_axis{};

	glm::vec2 trigger{};
	glm::vec<2, uint8> raw_trigger{};
	uint16 buttons = 0;
};
