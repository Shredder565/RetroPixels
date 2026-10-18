// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// snd_user / snduser2 sit on top of AX (snd_core). The MIX part lets a game
// describe a voice by musical controls - input level, pan, surround pan
// ("span"), fader, aux sends - in tenths of a decibel, and turns them into
// the per-bus, per-speaker volumes AX mixes with. Changes are collected and
// applied once per audio frame by MIXUpdateSettings, ramping from the old to
// the new volume over one frame to avoid clicks.
//
// Only the TV mix is computed; the GamePad and remote speaker mixes are
// tracked but not turned into AX volumes yet.
//
// SP* reads sound tables made by the SDK's sound packer. The AXFX effects
// (reverbs) are not emulated: their callbacks return silence on the wet
// channels, so games play dry.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/snd_user/snd_user.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/RPL/rpl.h"

namespace snd_user
{
	using namespace snd_core;

	// as passed by games to MIXInitDeviceControl (guest memory)
	struct MixControl
	{
		sint16be aux[AX_AUX_BUS_COUNT];
		sint16be pan;
		sint16be span;
		sint16be fader;
		sint16be lfe;
	};
	static_assert(sizeof(MixControl) == 0xE);

	namespace
	{
		constexpr uint32 kDrcCount = 2;
		constexpr uint32 kRmtCount = 4;
		constexpr uint32 kMaxSpeakers = AX_TV_CHANNEL_COUNT;

		// voice input flags
		constexpr uint32 kInputMute = 0x8;
		constexpr uint32 kInputDirty = 0x10000000;   // input level changed
		constexpr uint32 kInputRamping = 0x20000000; // a ramp to the new level was started
		// device flags (bits 0-2: aux bus N is sent before the fader)
		constexpr uint32 kAuxPreFaderMask = 0x7;
		constexpr uint32 kDeviceModeMask = 0xF;
		constexpr uint32 kMixDirty = 0x40000000;     // pan/fader/aux changed
		constexpr uint32 kMixRamping = 0x80000000;   // a ramp to new volumes was started
		constexpr uint32 kInitFlagsMask = kMixDirty | kAuxPreFaderMask;

		constexpr sint16 kSilentDb = -960; // -96.0 dB
		constexpr sint16 kCentre = 64;
		constexpr sint16 kFront = 127;
		constexpr sint32 kMonoFloorDb = -120;
		constexpr sint32 kSamplesPerFrameVe = 96; // input volume ramps over one 32 kHz frame

		// --- dB tables ----------------------------------------------------------
		// Generated offline from the formulas next to each table.

		// kDbToVolume[i] = 0x8000 * 10^((i - 904) / 200), for -90.4 dB .. +6.0 dB;
		// unity (0 dB) is stored as 0x7FFF
		const uint16 kDbToVolume[965] =
		{
			0x0, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
			0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
			0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
			0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x2, 0x2,
			0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2,
			0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2,
			0x2, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3,
			0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4,
			0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x5, 0x5, 0x5,
			0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x6, 0x6, 0x6,
			0x6, 0x6, 0x6, 0x6, 0x6, 0x6, 0x6, 0x6, 0x6, 0x6, 0x7, 0x7, 0x7, 0x7, 0x7, 0x7,
			0x7, 0x7, 0x7, 0x7, 0x7, 0x7, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,
			0x9, 0x9, 0x9, 0x9, 0x9, 0x9, 0x9, 0x9, 0x9, 0xA, 0xA, 0xA, 0xA, 0xA, 0xA, 0xA,
			0xA, 0xA, 0xB, 0xB, 0xB, 0xB, 0xB, 0xB, 0xB, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC,
			0xD, 0xD, 0xD, 0xD, 0xD, 0xD, 0xD, 0xE, 0xE, 0xE, 0xE, 0xE, 0xE, 0xF, 0xF, 0xF,
			0xF, 0xF, 0x10, 0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12,
			0x12, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16,
			0x16, 0x16, 0x17, 0x17, 0x17, 0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x1A, 0x1A, 0x1A, 0x1A,
			0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C, 0x1D, 0x1D, 0x1D, 0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x20, 0x20,
			0x20, 0x21, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x26,
			0x27, 0x27, 0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E,
			0x2F, 0x2F, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x35, 0x35, 0x36, 0x37, 0x37, 0x38,
			0x38, 0x39, 0x3A, 0x3A, 0x3B, 0x3C, 0x3D, 0x3D, 0x3E, 0x3F, 0x3F, 0x40, 0x41, 0x42, 0x42, 0x43,
			0x44, 0x45, 0x46, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51,
			0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61,
			0x62, 0x64, 0x65, 0x66, 0x67, 0x68, 0x6A, 0x6B, 0x6C, 0x6D, 0x6F, 0x70, 0x71, 0x72, 0x74, 0x75,
			0x76, 0x78, 0x79, 0x7B, 0x7C, 0x7E, 0x7F, 0x80, 0x82, 0x83, 0x85, 0x87, 0x88, 0x8A, 0x8B, 0x8D,
			0x8F, 0x90, 0x92, 0x94, 0x95, 0x97, 0x99, 0x9B, 0x9C, 0x9E, 0xA0, 0xA2, 0xA4, 0xA6, 0xA8, 0xAA,
			0xAB, 0xAD, 0xAF, 0xB2, 0xB4, 0xB6, 0xB8, 0xBA, 0xBC, 0xBE, 0xC0, 0xC3, 0xC5, 0xC7, 0xCA, 0xCC,
			0xCE, 0xD1, 0xD3, 0xD6, 0xD8, 0xDB, 0xDD, 0xE0, 0xE2, 0xE5, 0xE7, 0xEA, 0xED, 0xF0, 0xF2, 0xF5,
			0xF8, 0xFB, 0xFE, 0x101, 0x104, 0x107, 0x10A, 0x10D, 0x110, 0x113, 0x116, 0x11A, 0x11D, 0x120, 0x124, 0x127,
			0x12A, 0x12E, 0x131, 0x135, 0x138, 0x13C, 0x140, 0x143, 0x147, 0x14B, 0x14F, 0x153, 0x157, 0x15B, 0x15F, 0x163,
			0x167, 0x16B, 0x16F, 0x173, 0x178, 0x17C, 0x180, 0x185, 0x189, 0x18E, 0x193, 0x197, 0x19C, 0x1A1, 0x1A6, 0x1AB,
			0x1AF, 0x1B4, 0x1BA, 0x1BF, 0x1C4, 0x1C9, 0x1CE, 0x1D4, 0x1D9, 0x1DF, 0x1E4, 0x1EA, 0x1EF, 0x1F5, 0x1FB, 0x201,
			0x207, 0x20D, 0x213, 0x219, 0x21F, 0x226, 0x22C, 0x232, 0x239, 0x240, 0x246, 0x24D, 0x254, 0x25B, 0x262, 0x269,
			0x270, 0x277, 0x27E, 0x286, 0x28D, 0x295, 0x29D, 0x2A4, 0x2AC, 0x2B4, 0x2BC, 0x2C4, 0x2CC, 0x2D5, 0x2DD, 0x2E6,
			0x2EE, 0x2F7, 0x300, 0x309, 0x312, 0x31B, 0x324, 0x32D, 0x337, 0x340, 0x34A, 0x354, 0x35D, 0x367, 0x371, 0x37C,
			0x386, 0x390, 0x39B, 0x3A6, 0x3B1, 0x3BB, 0x3C7, 0x3D2, 0x3DD, 0x3E9, 0x3F4, 0x400, 0x40C, 0x418, 0x424, 0x430,
			0x43D, 0x449, 0x456, 0x463, 0x470, 0x47D, 0x48A, 0x498, 0x4A5, 0x4B3, 0x4C1, 0x4CF, 0x4DD, 0x4EC, 0x4FA, 0x509,
			0x518, 0x527, 0x536, 0x546, 0x555, 0x565, 0x575, 0x586, 0x596, 0x5A6, 0x5B7, 0x5C8, 0x5D9, 0x5EB, 0x5FC, 0x60E,
			0x620, 0x632, 0x644, 0x657, 0x66A, 0x67D, 0x690, 0x6A4, 0x6B7, 0x6CB, 0x6DF, 0x6F4, 0x708, 0x71D, 0x732, 0x748,
			0x75D, 0x773, 0x789, 0x79F, 0x7B6, 0x7CD, 0x7E4, 0x7FB, 0x813, 0x82B, 0x843, 0x85C, 0x874, 0x88E, 0x8A7, 0x8C1,
			0x8DA, 0x8F5, 0x90F, 0x92A, 0x945, 0x961, 0x97D, 0x999, 0x9B5, 0x9D2, 0x9EF, 0xA0D, 0xA2A, 0xA48, 0xA67, 0xA86,
			0xAA5, 0xAC5, 0xAE5, 0xB05, 0xB25, 0xB47, 0xB68, 0xB8A, 0xBAC, 0xBCF, 0xBF2, 0xC15, 0xC39, 0xC5D, 0xC82, 0xCA7,
			0xCCC, 0xCF2, 0xD19, 0xD3F, 0xD67, 0xD8E, 0xDB7, 0xDDF, 0xE08, 0xE32, 0xE5C, 0xE87, 0xEB2, 0xEDD, 0xF09, 0xF36,
			0xF63, 0xF91, 0xFBF, 0xFEE, 0x101D, 0x104D, 0x107D, 0x10AE, 0x10DF, 0x1111, 0x1144, 0x1177, 0x11AB, 0x11DF, 0x1214, 0x124A,
			0x1280, 0x12B7, 0x12EE, 0x1326, 0x135F, 0x1399, 0x13D3, 0x140D, 0x1449, 0x1485, 0x14C2, 0x14FF, 0x153E, 0x157D, 0x15BC, 0x15FD,
			0x163E, 0x1680, 0x16C3, 0x1706, 0x174A, 0x178F, 0x17D5, 0x181C, 0x1863, 0x18AC, 0x18F5, 0x193F, 0x198A, 0x19D5, 0x1A22, 0x1A6F,
			0x1ABE, 0x1B0D, 0x1B5D, 0x1BAE, 0x1C00, 0x1C53, 0x1CA7, 0x1CFC, 0x1D52, 0x1DA9, 0x1E01, 0x1E5A, 0x1EB4, 0x1F0F, 0x1F6B, 0x1FC8,
			0x2026, 0x2086, 0x20E6, 0x2148, 0x21AA, 0x220E, 0x2273, 0x22D9, 0x2341, 0x23A9, 0x2413, 0x247E, 0x24EA, 0x2557, 0x25C6, 0x2636,
			0x26A7, 0x271A, 0x278E, 0x2803, 0x287A, 0x28F2, 0x296B, 0x29E6, 0x2A62, 0x2AE0, 0x2B5F, 0x2BDF, 0x2C61, 0x2CE5, 0x2D6A, 0x2DF1,
			0x2E79, 0x2F03, 0x2F8E, 0x301B, 0x30AA, 0x313A, 0x31CC, 0x325F, 0x32F5, 0x338C, 0x3425, 0x34BF, 0x355B, 0x35FA, 0x369A, 0x373C,
			0x37DF, 0x3885, 0x392C, 0x39D6, 0x3A81, 0x3B2F, 0x3BDE, 0x3C90, 0x3D43, 0x3DF9, 0x3EB1, 0x3F6A, 0x4026, 0x40E5, 0x41A5, 0x4268,
			0x432C, 0x43F4, 0x44BD, 0x4589, 0x4657, 0x4727, 0x47FA, 0x48D0, 0x49A8, 0x4A82, 0x4B5F, 0x4C3E, 0x4D20, 0x4E05, 0x4EEC, 0x4FD6,
			0x50C3, 0x51B2, 0x52A4, 0x5399, 0x5491, 0x558C, 0x5689, 0x578A, 0x588D, 0x5994, 0x5A9D, 0x5BAA, 0x5CBA, 0x5DCD, 0x5EE3, 0x5FFC,
			0x6119, 0x6238, 0x635C, 0x6482, 0x65AC, 0x66D9, 0x680A, 0x693F, 0x6A77, 0x6BB2, 0x6CF2, 0x6E35, 0x6F7B, 0x70C6, 0x7214, 0x7366,
			0x74BC, 0x7616, 0x7774, 0x78D6, 0x7A3D, 0x7BA7, 0x7D16, 0x7E88, 0x7FFF, 0x817B, 0x82FB, 0x847F, 0x8608, 0x8795, 0x8927, 0x8ABE,
			0x8C59, 0x8DF9, 0x8F9E, 0x9148, 0x92F6, 0x94AA, 0x9663, 0x9820, 0x99E3, 0x9BAB, 0x9D79, 0x9F4C, 0xA124, 0xA302, 0xA4E5, 0xA6CE,
			0xA8BC, 0xAAB0, 0xACAA, 0xAEAA, 0xB0B0, 0xB2BC, 0xB4CE, 0xB6E5, 0xB904, 0xBB28, 0xBD53, 0xBF84, 0xC1BC, 0xC3FA, 0xC63F, 0xC88B,
			0xCADD, 0xCD37, 0xCF97, 0xD1FE, 0xD46D, 0xD6E3, 0xD960, 0xDBE4, 0xDE70, 0xE103, 0xE39E, 0xE641, 0xE8EB, 0xEB9E, 0xEE58, 0xF11B,
			0xF3E6, 0xF6B9, 0xF994, 0xFC78, 0xFF64,
		};

		// kPanDb[i] = round(100 * log10((127 - i) / 127)), the last entry is -90.4 dB
		const sint16 kPanDb[128] =
		{
			0, 0, -1, -1, -1, -2, -2, -2, -3, -3, -4, -4, -4, -5, -5, -5,
			-6, -6, -7, -7, -7, -8, -8, -9, -9, -10, -10, -10, -11, -11, -12, -12,
			-13, -13, -14, -14, -14, -15, -15, -16, -16, -17, -17, -18, -18, -19, -20, -20,
			-21, -21, -22, -22, -23, -23, -24, -25, -25, -26, -26, -27, -28, -28, -29, -30,
			-30, -31, -32, -33, -33, -34, -35, -36, -36, -37, -38, -39, -40, -40, -41, -42,
			-43, -44, -45, -46, -47, -48, -49, -50, -51, -52, -54, -55, -56, -57, -59, -60,
			-61, -63, -64, -66, -67, -69, -71, -72, -74, -76, -78, -80, -83, -85, -87, -90,
			-93, -96, -99, -102, -106, -110, -115, -120, -126, -133, -140, -150, -163, -180, -210, -904,
		};

		// kPanDbCos[i] = trunc(200 * log10(cos(i * pi / 254))), the last entry is -96 dB
		const sint16 kPanDbCos[128] =
		{
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1,
			-1, -1, -2, -2, -2, -2, -3, -3, -3, -4, -4, -4, -5, -5, -6, -6,
			-6, -7, -7, -8, -8, -9, -9, -10, -11, -11, -12, -12, -13, -14, -14, -15,
			-16, -17, -17, -18, -19, -20, -21, -21, -22, -23, -24, -25, -26, -27, -28, -29,
			-30, -31, -32, -34, -35, -36, -37, -38, -40, -41, -42, -44, -45, -47, -48, -50,
			-52, -53, -55, -57, -58, -60, -62, -64, -66, -68, -70, -73, -75, -77, -80, -82,
			-85, -88, -90, -93, -96, -100, -103, -106, -110, -114, -118, -122, -126, -131, -136, -141,
			-146, -152, -159, -166, -173, -181, -190, -201, -212, -225, -241, -261, -286, -321, -381, -960,
		};

		// kPanDbSin[i] = trunc(200 * log10(sin(pi / 4 + (i - 63.5) * pi / 730)))
		const sint16 kPanDbSin[128] =
		{
			-61, -61, -60, -59, -59, -58, -58, -57, -56, -56, -55, -55, -54, -53, -53, -52,
			-52, -51, -50, -50, -49, -49, -48, -48, -47, -47, -46, -46, -45, -45, -44, -44,
			-43, -43, -42, -42, -41, -41, -40, -40, -39, -39, -38, -38, -38, -37, -37, -36,
			-36, -35, -35, -35, -34, -34, -33, -33, -32, -32, -32, -31, -31, -31, -30, -30,
			-29, -29, -29, -28, -28, -28, -27, -27, -27, -26, -26, -26, -25, -25, -25, -24,
			-24, -24, -23, -23, -23, -22, -22, -22, -21, -21, -21, -20, -20, -20, -20, -19,
			-19, -19, -18, -18, -18, -18, -17, -17, -17, -17, -16, -16, -16, -16, -15, -15,
			-15, -15, -14, -14, -14, -14, -13, -13, -13, -13, -13, -12, -12, -12, -12, -11,
		};

		uint16 DbToVolume(sint32 db)
		{
			if (db <= -904)
				return 0;
			if (db >= 60)
				return 0xFF64;
			return kDbToVolume[db + 904];
		}

		sint32 PanIndex(sint32 v)
		{
			return std::clamp(v, 0, 127);
		}

		// --- per voice state ----------------------------------------------------

		struct Ramp
		{
			uint16 now;
			uint16 target;
		};

		struct DeviceMix
		{
			sint16 aux[AX_AUX_BUS_COUNT];
			sint16 pan;
			sint16 span;
			sint16 fader;
			sint16 lfe;
			sint16 speakerDb[kMaxSpeakers]; // attenuation from pan/span per speaker
			Ramp vol[AX_MAX_NUM_BUS][kMaxSpeakers];
			uint32 flags;
		};

		struct Voice
		{
			MEMPTR<AXVPB> vpb;
			uint32 inputFlags;
			sint16 inputLevel;
			Ramp inputVol;
			DeviceMix tv;
			DeviceMix drc[kDrcCount];
			DeviceMix rmt[kRmtCount];
		};

		struct LibState
		{
			bool initialized;
			MixSoundMode tvMode;
			MixSoundMode drcMode;
			MixSoundMode rmtMode;
			Voice voices[AX_MAX_VOICES];
		};
		LibState s_mix{};

		void ResetDevice(DeviceMix& d, bool resetLfe)
		{
			d.flags = 0;
			d.pan = kCentre;
			d.span = kFront;
			d.fader = 0;
			// the system library leaves the TV LFE level alone when resetting
			if (resetLfe)
				d.lfe = kSilentDb;
			for (auto& a : d.aux)
				a = kSilentDb;
			for (auto& bus : d.vol)
				for (auto& r : bus)
					r = {0, 0};
		}

		void ResetVoice(Voice& v)
		{
			v.inputFlags = kMixDirty | kInputDirty;
			v.inputLevel = 0;
			v.inputVol = {0, 0};
			ResetDevice(v.tv, false);
			for (auto& d : v.drc)
				ResetDevice(d, true);
			for (auto& d : v.rmt)
				ResetDevice(d, true);
		}

		DeviceMix* FindDevice(Voice& v, uint32 device, uint32 index)
		{
			if (device == AX_DEV_TV && index == 0)
				return &v.tv;
			if (device == AX_DEV_DRC && index < kDrcCount)
				return &v.drc[index];
			if (device == AX_DEV_RMT && index < kRmtCount)
				return &v.rmt[index];
			cemuLog_log(LogType::APIErrors, "snd_user: invalid device {} / index {}", device, index);
			return nullptr;
		}

		Voice* VoiceOf(AXVPB* vpb)
		{
			if (!vpb)
			{
				cemuLog_log(LogType::APIErrors, "snd_user: MIX function called without a voice");
				return nullptr;
			}
			const uint32 index = vpb->index;
			if (index >= (uint32)AX_MAX_VOICES)
			{
				cemuLog_log(LogType::APIErrors, "snd_user: voice {:08x} has invalid index {}", MEMPTR<AXVPB>(vpb).GetMPTR(), index);
				return nullptr;
			}
			return &s_mix.voices[index];
		}

		// Turns pan (0 left .. 64 centre .. 127 right) and span (0 rear .. 127
		// front) into a per-speaker attenuation in 0.1 dB.
		void ComputeSpeakerDb(DeviceMix& d, uint32 device)
		{
			if (device != AX_DEV_TV)
				return; // GamePad and remote panning are not modelled
			const sint32 p = PanIndex(d.pan);
			const sint32 s = PanIndex(d.span);
			const sint32 pr = 127 - p;
			const sint32 sr = 127 - s;
			sint16* out = d.speakerDb;
			switch (s_mix.tvMode)
			{
			case MixSoundMode::SurroundDPL2:
				out[0] = kPanDbCos[p];
				out[1] = kPanDbCos[pr];
				out[2] = kPanDbSin[pr];
				out[3] = kPanDbSin[p];
				out[4] = kPanDbCos[sr];
				out[5] = kPanDbCos[s];
				break;
			case MixSoundMode::Surround6CH:
			{
				const sint32 left = std::min(p * 2, 127);
				const sint32 right = pr * 2 >= 126 ? 127 : pr * 2;
				const sint32 centreDist = std::min(std::abs(p - kCentre) * 2, 127);
				out[0] = kPanDb[left] + kPanDb[sr];
				out[1] = kPanDb[right] + kPanDb[sr];
				out[2] = kPanDb[s] + kPanDb[p];
				out[3] = kPanDb[s] + kPanDb[pr];
				out[4] = kPanDb[centreDist] + kPanDb[sr];
				out[5] = 0;
				break;
			}
			default: // mono and stereo
				out[0] = kPanDb[p];
				out[1] = kPanDb[pr];
				out[2] = 0;
				out[3] = 0;
				out[4] = kPanDb[sr];
				out[5] = kPanDb[s];
				break;
			}
		}

		// fader applies to an aux send unless that bus is pre-fader
		sint32 SendLevel(const DeviceMix& d, uint32 bus)
		{
			const sint32 aux = d.aux[bus];
			return (d.flags & (1u << bus)) ? aux : aux + d.fader;
		}

		void ComputeTvTargets(DeviceMix& d)
		{
			const sint16* c = d.speakerDb;
			auto set = [&](uint32 bus, uint32 speaker, sint32 db) { d.vol[bus][speaker].target = DbToVolume(db); };
			auto clear = [&](uint32 bus, uint32 speaker) { d.vol[bus][speaker].target = 0; };
			switch (s_mix.tvMode)
			{
			case MixSoundMode::Mono:
			case MixSoundMode::Stereo:
			case MixSoundMode::Surround:
			{
				const bool mono = s_mix.tvMode == MixSoundMode::Mono;
				const sint32 front = std::max<sint32>(c[4], kMonoFloorDb);
				const sint32 left = mono ? 0 : c[0];
				const sint32 right = mono ? 0 : c[1];
				for (uint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
				{
					const sint32 level = bus == 0 ? d.fader : SendLevel(d, bus - 1);
					set(bus, 0, front + left + level);
					set(bus, 1, front + right + level);
					for (uint32 sp = 2; sp < kMaxSpeakers; sp++)
						clear(bus, sp);
				}
				break;
			}
			case MixSoundMode::SurroundDPL2:
			{
				// Dolby Pro Logic II matrix: L, R, Ls, Rs; aux C is not sent
				for (uint32 bus = 0; bus < 3; bus++)
				{
					const sint32 level = bus == 0 ? d.fader : SendLevel(d, bus - 1);
					set(bus, 0, level + c[4] + c[0]);
					set(bus, 1, level + c[4] + c[1]);
					set(bus, 2, level + c[5] + c[2]);
					set(bus, 3, level + c[5] + c[3]);
					clear(bus, 4);
					clear(bus, 5);
				}
				break;
			}
			case MixSoundMode::Surround6CH:
			{
				for (uint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
				{
					const sint32 level = bus == 0 ? d.fader : SendLevel(d, bus - 1);
					for (uint32 sp = 0; sp < 5; sp++)
						set(bus, sp, level + c[sp]);
					set(bus, 5, level + d.lfe);
				}
				break;
			}
			default:
				break;
			}
		}

		// Commits a finished ramp, recomputes volumes if something changed and
		// hands AX this frame's start volume and per-sample step.
		void ApplyTv(Voice& v)
		{
			DeviceMix& d = v.tv;
			bool changed = false;
			if (d.flags & kMixRamping)
			{
				for (auto& bus : d.vol)
					for (auto& r : bus)
						r.now = r.target;
				d.flags &= ~kMixRamping;
				changed = true;
			}
			if (d.flags & kMixDirty)
			{
				ComputeTvTargets(d);
				d.flags = (d.flags & ~kMixDirty) | kMixRamping;
				changed = true;
			}
			if (!changed)
				return;

			const sint32 samples = AXGetInputSamplesPerFrame();
			AXCHMIX2 mix[AX_TV_CHANNEL_COUNT][AX_MAX_NUM_BUS];
			for (uint32 bus = 0; bus < AX_MAX_NUM_BUS; bus++)
			{
				for (uint32 sp = 0; sp < AX_TV_CHANNEL_COUNT; sp++)
				{
					const Ramp& r = d.vol[bus][sp];
					mix[sp][bus].vol = r.now;
					mix[sp][bus].delta = (sint16)((sint16)(r.target - r.now) / samples);
				}
			}
			AXSetVoiceDeviceMix(v.vpb.GetPtr(), AX_DEV_TV, 0, (AXCHMIX_DEPR*)&mix[0][0]);
		}

		void SendInputRamp(Voice& v)
		{
			AXPBVE ve;
			ve.currentVolume = v.inputVol.now;
			ve.currentDelta = (sint16)(((sint32)v.inputVol.target - (sint32)v.inputVol.now) / kSamplesPerFrameVe);
			AXSetVoiceVe(v.vpb.GetPtr(), &ve);
		}

		// --- AXFX allocator hooks -------------------------------------------------

		MEMPTR<void> s_fxAlloc;
		MEMPTR<void> s_fxFree;

		void* FxDefaultAlloc(uint32 size)
		{
			return coreinit::_weak_MEMAllocFromDefaultHeap(size);
		}

		void FxDefaultFree(void* block)
		{
			coreinit::_weak_MEMFreeToDefaultHeap(block);
		}

		void InstallDefaultFxHooks()
		{
			if (s_fxAlloc)
				return;
			s_fxAlloc = MEMPTR<void>(makeCallableExport<&FxDefaultAlloc>());
			s_fxFree = MEMPTR<void>(makeCallableExport<&FxDefaultFree>());
		}

		void ResetLibrary()
		{
			s_mix = LibState{};
			s_fxAlloc = nullptr;
			s_fxFree = nullptr;
			InstallDefaultFxHooks();
		}
	}

	// --- MIX ------------------------------------------------------------------------

	void MIXInit()
	{
		if (s_mix.initialized)
			return;
		for (Voice& v : s_mix.voices)
			ResetVoice(v);
		s_mix.tvMode = MixSoundMode::Stereo;
		s_mix.drcMode = MixSoundMode::Stereo;
		s_mix.rmtMode = MixSoundMode::Mono;
		s_mix.initialized = true;
		InstallDefaultFxHooks();
	}

	void MIXSetSoundMode(MixSoundMode soundMode)
	{
		// the TV only does mono or stereo through this call
		if (soundMode == MixSoundMode::Surround || soundMode == MixSoundMode::SurroundDPL2)
			soundMode = MixSoundMode::Stereo;
		if (soundMode != MixSoundMode::Mono && soundMode != MixSoundMode::Stereo)
		{
			cemuLog_log(LogType::APIErrors, "MIXSetSoundMode: unsupported mode {}", (uint32)soundMode);
			soundMode = MixSoundMode::Stereo;
		}
		s_mix.tvMode = soundMode;
	}

	MixSoundMode MIXGetSoundMode()
	{
		return s_mix.tvMode;
	}

	void MIXSetDeviceSoundMode(uint32 device, MixSoundMode mode)
	{
		bool tvChanged = false, drcChanged = false;
		if (device == AX_DEV_TV)
		{
			if ((uint32)mode > (uint32)MixSoundMode::Surround6CH)
			{
				cemuLog_log(LogType::APIErrors, "MIXSetDeviceSoundMode: unsupported TV mode {}", (uint32)mode);
				mode = MixSoundMode::Stereo;
			}
			s_mix.tvMode = mode;
			tvChanged = true;
		}
		else if (device == AX_DEV_DRC)
		{
			if ((uint32)mode > (uint32)MixSoundMode::Surround)
			{
				cemuLog_log(LogType::APIErrors, "MIXSetDeviceSoundMode: unsupported GamePad mode {}", (uint32)mode);
				mode = MixSoundMode::Stereo;
			}
			s_mix.drcMode = mode;
			drcChanged = true;
		}
		else if (device == AX_DEV_RMT)
		{
			if (mode != MixSoundMode::Mono)
				cemuLog_log(LogType::APIErrors, "MIXSetDeviceSoundMode: remote speakers are mono");
			s_mix.rmtMode = MixSoundMode::Mono;
		}
		else
			cemuLog_log(LogType::APIErrors, "MIXSetDeviceSoundMode: invalid device {}", device);

		// every assigned voice has to recompute its speaker levels
		for (Voice& v : s_mix.voices)
		{
			if (!v.vpb)
				continue;
			AXVoiceBegin(v.vpb.GetPtr());
			if (tvChanged)
			{
				v.tv.flags |= kMixDirty;
				ComputeSpeakerDb(v.tv, AX_DEV_TV);
			}
			if (drcChanged)
			{
				for (DeviceMix& d : v.drc)
				{
					d.flags |= kMixDirty;
					ComputeSpeakerDb(d, AX_DEV_DRC);
				}
			}
			AXVoiceEnd(v.vpb.GetPtr());
		}
	}

	void MIXAssignChannel(AXVPB* voice)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		AXVoiceBegin(voice);
		ResetVoice(*v);
		v->vpb = voice;
		AXVoiceEnd(voice);
	}

	void MIXReleaseChannel(AXVPB* voice)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		AXVoiceBegin(voice);
		v->vpb = nullptr;
		AXVoiceEnd(voice);
	}

	void MIXInitInputControl(AXVPB* voice, uint16 input, uint32 mode)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		AXVoiceBegin(voice);
		v->inputFlags = (mode & kInputMute) | kInputDirty;
		v->inputLevel = (sint16)input;
		AXVoiceEnd(voice);
	}

	void MIXSetInput(AXVPB* voice, uint16 input)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		v->inputLevel = (sint16)input;
		v->inputFlags |= kInputDirty;
	}

	void MIXInitChannel(AXVPB* voice, uint32 flags, sint16 input, sint16 aux1, sint16 aux2, sint16 aux3, sint16 pan, sint16 span, sint16 fader)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		AXVoiceBegin(voice);
		MIXAssignChannel(voice);
		MIXInitInputControl(voice, (uint16)input, flags);
		DeviceMix& tv = v->tv;
		tv.aux[0] = aux1;
		tv.aux[1] = aux2;
		tv.aux[2] = aux3;
		tv.pan = pan;
		tv.span = span;
		tv.fader = fader;
		tv.flags = (flags & kInitFlagsMask) | kMixDirty;
		ComputeSpeakerDb(tv, AX_DEV_TV);
		ApplyTv(*v);
		AXVoiceEnd(voice);
	}

	void MIXDRCInitChannel(AXVPB* voice, uint32 flags, uint16 aux, uint16 pan, uint16 fader)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		AXVoiceBegin(voice);
		DeviceMix& d = v->drc[0];
		ResetDevice(d, true);
		d.aux[0] = (sint16)aux;
		d.pan = (sint16)pan;
		d.fader = (sint16)fader;
		d.flags = (flags & kInitFlagsMask) | kMixDirty;
		ComputeSpeakerDb(d, AX_DEV_DRC);
		AXVoiceEnd(voice);
	}

	void MIXInitDeviceControl(AXVPB* voice, uint32 device, uint32 deviceSubIndex, MixControl* control, uint32 mode)
	{
		Voice* v = VoiceOf(voice);
		if (!v || !control)
			return;
		DeviceMix* d = FindDevice(*v, device, deviceSubIndex);
		if (!d)
			return;
		AXVoiceBegin(voice);
		v->vpb = voice;
		ResetDevice(*d, device != AX_DEV_TV);
		// the LFE level is not taken from the control block
		d->pan = control->pan;
		d->span = control->span;
		d->fader = control->fader;
		for (uint32 i = 0; i < AX_AUX_BUS_COUNT; i++)
			d->aux[i] = control->aux[i];
		ComputeSpeakerDb(*d, device);
		d->flags = mode & kDeviceModeMask;
		if (device != AX_DEV_RMT)
			d->flags |= kMixDirty;
		if (device == AX_DEV_TV)
			ApplyTv(*v);
		AXVoiceEnd(voice);
	}

	// Shared by the MIXSetDevice* setters: changes one control value and marks
	// the device for recomputation on the next MIXUpdateSettings.
	template<typename TApply>
	void ChangeDeviceControl(AXVPB* voice, uint32 device, uint32 index, TApply&& apply)
	{
		Voice* v = VoiceOf(voice);
		if (!v)
			return;
		AXVoiceBegin(voice);
		if (DeviceMix* d = FindDevice(*v, device, index))
		{
			if (apply(*d))
				d->flags |= kMixDirty;
		}
		AXVoiceEnd(voice);
	}

	void MIXSetDeviceFader(AXVPB* vpb, uint32 device, uint32 deviceIndex, sint16 newFader)
	{
		ChangeDeviceControl(vpb, device, deviceIndex, [&](DeviceMix& d) {
			if (d.fader == newFader)
				return false;
			d.fader = newFader;
			return true;
		});
	}

	void MIXSetDevicePan(AXVPB* vpb, uint32 device, uint32 deviceIndex, sint16 newPan)
	{
		ChangeDeviceControl(vpb, device, deviceIndex, [&](DeviceMix& d) {
			if (d.pan == newPan)
				return false;
			d.pan = newPan;
			ComputeSpeakerDb(d, device);
			return true;
		});
	}

	void MIXSetDeviceSPan(AXVPB* vpb, uint32 device, uint32 deviceIndex, sint16 newSPan)
	{
		ChangeDeviceControl(vpb, device, deviceIndex, [&](DeviceMix& d) {
			if (d.span == newSPan)
				return false;
			d.span = newSPan;
			ComputeSpeakerDb(d, device);
			return true;
		});
	}

	void MIXSetDeviceLFE(AXVPB* vpb, uint32 device, uint32 deviceIndex, sint16 newLFE)
	{
		if (device != AX_DEV_TV)
		{
			cemuLog_log(LogType::APIErrors, "MIXSetDeviceLFE: only the TV has an LFE channel");
			return;
		}
		ChangeDeviceControl(vpb, device, 0, [&](DeviceMix& d) {
			if (d.lfe == newLFE)
				return false;
			d.lfe = newLFE;
			return true;
		});
	}

	void MIXSetDeviceAux(AXVPB* vpb, uint32 device, uint32 deviceIndex, uint32 aux, sint16 newAux)
	{
		if (aux >= (uint32)AX_AUX_BUS_COUNT)
		{
			cemuLog_log(LogType::APIErrors, "MIXSetDeviceAux: invalid aux bus {}", aux);
			return;
		}
		ChangeDeviceControl(vpb, device, deviceIndex, [&](DeviceMix& d) {
			if (d.aux[aux] == newAux)
				return false;
			d.aux[aux] = newAux;
			return true;
		});
	}

	void MIXUpdateSettings()
	{
		if (!s_mix.initialized)
			return;
		for (Voice& v : s_mix.voices)
		{
			if (!v.vpb)
				continue;
			AXVoiceBegin(v.vpb.GetPtr());
			bool rampDone = false;
			if (v.inputFlags & kInputRamping)
			{
				v.inputVol.now = v.inputVol.target;
				v.inputFlags &= ~kInputRamping;
				rampDone = true;
			}
			if (v.inputFlags & kInputDirty)
			{
				v.inputVol.target = (v.inputFlags & kInputMute) ? 0 : DbToVolume(v.inputLevel);
				v.inputFlags = (v.inputFlags & ~kInputDirty) | kInputRamping;
				SendInputRamp(v);
			}
			else if (rampDone)
				SendInputRamp(v); // settle on the reached volume
			ApplyTv(v);
			AXVoiceEnd(v.vpb.GetPtr());
		}
	}

	// --- SP sound tables -----------------------------------------------------------------

	struct SPSoundEntry
	{
		/* +0x00 */ uint32be type;
		/* +0x04 */ uint32be sampleRate;
		/* +0x08 */ uint32be loopAddress;
		/* +0x0C */ uint32be loopEndAddress;
		/* +0x10 */ uint32be endOffset;
		/* +0x14 */ uint32be currentOffset;
		/* +0x18 */ MEMPTR<void> adpcmEntry;
	};
	static_assert(sizeof(SPSoundEntry) == 0x1C);

	struct SPSoundTable
	{
		uint32be count;
		SPSoundEntry entries[1]; // 'count' entries follow
	};

	// Prepares the table and reports how many bytes of sample data it uses.
	void SPInitSoundTable(SPSoundTable* soundTable, uint8* samples, uint32be* endOffsetPtr)
	{
		if (!soundTable || !samples)
			return;
		uint32 usedBytes = 0;
		const uint32 count = soundTable->count;
		for (uint32 i = 0; i < count; i++)
		{
			SPSoundEntry& e = soundTable->entries[i];
			const uint32 type = e.type;
			if (type > 5)
				continue;
			// even types play once, odd types loop
			if ((type & 1) == 0)
			{
				e.loopAddress = (uint32)e.currentOffset;
				e.loopEndAddress = 0;
			}
			// offsets are in nibbles (ADPCM), 16-bit samples (PCM16) or bytes (PCM8)
			uint32 end = e.endOffset;
			if (type <= 1)
				end >>= 1;
			else if (type <= 3)
				end <<= 1;
			usedBytes = std::max(usedBytes, end);
		}
		if (endOffsetPtr)
			*endOffsetPtr = usedBytes;
	}

	SPSoundEntry* SPGetSoundEntry(SPSoundTable* soundTable, uint32 index)
	{
		if (!soundTable || index >= (uint32)soundTable->count)
			return nullptr;
		return &soundTable->entries[index];
	}

	// --- AXFX --------------------------------------------------------------------

	void AXFXSetHooks(void* allocFunc, void* freeFunc)
	{
		s_fxAlloc = allocFunc;
		s_fxFree = freeFunc;
	}

	void AXFXGetHooks(MEMPTR<void>* allocFuncOut, MEMPTR<void>* freeFuncOut)
	{
		*allocFuncOut = s_fxAlloc;
		*freeFuncOut = s_fxFree;
	}

	// what an aux callback gets: one buffer per channel
	struct AUXCBSAMPLEDATA
	{
		MEMPTR<sint32be> channelSamples[6];
	};

	void SilenceWetChannels(AUXCBSAMPLEDATA* data, uint32 sampleCount, uint32 channelCount)
	{
		static std::atomic_bool s_warned{false};
		if (!s_warned.exchange(true))
		{
			cemuLog_log(LogType::Force, "This title uses AXFX audio effects (reverb), which are not emulated; it plays without them.");
			cemuLog_log(LogType::Force, "To get the effects, put snd_user.rpl and snduser2.rpl from a Wii U into the cafeLibs folder.");
		}
		for (uint32 ch = 0; ch < channelCount; ch++)
		{
			sint32be* buffer = data->channelSamples[ch].GetPtr();
			if (!buffer)
				continue;
			for (uint32 i = 0; i < sampleCount; i++)
				buffer[i] = 0;
		}
	}

	struct AXFXReverbHiData
	{
		uint32be unused;
	};

	void AXFXReverbHiInit(AXFXReverbHiData* param)
	{
		cemuLog_log(LogType::Force, "AXFXReverbHiInit: effect not emulated");
	}

	void AXFXReverbHiSettings(AXFXReverbHiData* param)
	{
	}

	void AXFXReverbHiShutdown(AXFXReverbHiData* param)
	{
	}

	void AXFXReverbHiCallback(AUXCBSAMPLEDATA* auxSamples, AXFXReverbHiData* reverbHi)
	{
		SilenceWetChannels(auxSamples, AXGetInputSamplesPerFrame(), 3);
	}

	struct AXFXMultiChReverbData
	{
		uint32be unused;
	};

	void AXFXMultiChReverbInit(AXFXMultiChReverbData* param, int ukn2, int ukn3)
	{
		cemuLog_log(LogType::Force, "AXFXMultiChReverbInit: effect not emulated");
	}

	void AXFXMultiChReverbSettingsUpdate(AXFXMultiChReverbData* param)
	{
	}

	void AXFXMultiChReverbShutdown(AXFXMultiChReverbData* param)
	{
	}

	void AXFXMultiChReverbCallback(AUXCBSAMPLEDATA* auxSamples, AXFXMultiChReverbData* reverb, AXAUXCBCHANNELINFO* auxInfo)
	{
		SilenceWetChannels(auxSamples, auxInfo->numSamples, 6);
	}

	struct AXFXReverbStdData
	{
		uint32be unused;
	};

	uint32 AXFXReverbStdExpGetMemSize(AXFXReverbStdData* reverbParam)
	{
		return sizeof(AXFXReverbStdData) * 3; // the effect needs no work memory here
	}

	bool AXFXReverbStdExpInit(AXFXReverbStdData* reverbParam)
	{
		return true;
	}

	void AXFXReverbStdExpShutdown(AXFXReverbStdData* reverbParam)
	{
	}

	void AXFXReverbStdExpCallback(AUXCBSAMPLEDATA* auxSamples, AXFXReverbStdData* reverbData)
	{
		SilenceWetChannels(auxSamples, AXGetInputSamplesPerFrame(), 3);
	}

	// --- modules ---------------------------------------------------------------

	// snd_user and snduser2 export the same MIX and AXFX API
#define SNDUSER_REGISTER_SHARED(lib) \
		cafeExportRegister(lib, MIXInit, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetSoundMode, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXGetSoundMode, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXInitChannel, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXReleaseChannel, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXAssignChannel, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXDRCInitChannel, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetInput, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXUpdateSettings, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetDeviceSoundMode, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetDeviceFader, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetDevicePan, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetDeviceSPan, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetDeviceLFE, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXSetDeviceAux, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXInitDeviceControl, LogType::SoundAPI); \
		cafeExportRegister(lib, MIXInitInputControl, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXSetHooks, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXGetHooks, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXReverbHiInit, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXReverbHiSettings, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXReverbHiShutdown, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXReverbHiCallback, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXMultiChReverbInit, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXMultiChReverbSettingsUpdate, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXMultiChReverbShutdown, LogType::SoundAPI); \
		cafeExportRegister(lib, AXFXMultiChReverbCallback, LogType::SoundAPI)

	class SndUserModule : public COSModule
	{
	public:
		std::string_view GetName() override { return "snd_user"; }

		void RPLMapped() override
		{
			ResetLibrary();
			SNDUSER_REGISTER_SHARED("snd_user");
			cafeExportRegister("snd_user", SPInitSoundTable, LogType::SoundAPI);
			cafeExportRegister("snd_user", SPGetSoundEntry, LogType::SoundAPI);
		}
	};

	class SndUser2Module : public COSModule
	{
	public:
		std::string_view GetName() override { return "snduser2"; }

		void RPLMapped() override
		{
			ResetLibrary();
			SNDUSER_REGISTER_SHARED("snduser2");
			cafeExportRegister("snduser2", AXFXReverbStdExpGetMemSize, LogType::SoundAPI);
			cafeExportRegister("snduser2", AXFXReverbStdExpInit, LogType::SoundAPI);
			cafeExportRegister("snduser2", AXFXReverbStdExpShutdown, LogType::SoundAPI);
			cafeExportRegister("snduser2", AXFXReverbStdExpCallback, LogType::SoundAPI);
		}
	};

	SndUserModule s_sndUserModule;
	SndUser2Module s_sndUser2Module;

	COSModule* GetModuleSndUser1()
	{
		return &s_sndUserModule;
	}

	COSModule* GetModuleSndUser2()
	{
		return &s_sndUser2Module;
	}
}
