// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nsyskbd reads USB keyboards plugged into the console (four channels).
// No keyboard is emulated: every channel reports "disconnected", and reading
// a key gives an empty key event for that channel. Some Virtual Console
// titles (MSX) poll these calls every frame.

#include "Cafe/OS/common/OSCommon.h"
#include "nsyskbd.h"

namespace nsyskbd
{
	namespace
	{
		constexpr uint32 kChannelCount = 4;
		constexpr uint32 kStatusDisconnected = 1;
		constexpr uint32 kResultOk = 0;

		// Key event as the game sees it. Only the channel byte is meaningful
		// for an empty event; the rest is cleared.
#pragma pack(push, 1)
		struct KeyEvent
		{
			uint8be channel;
			uint8be hidCode;
			uint8be reserved[2];
			uint32be keyState;
			uint32be modifiers;
			uint16be utf16Char;
		};
#pragma pack(pop)
		static_assert(sizeof(KeyEvent) == 14);

		uint32 GetChannelStatus(uint32 channel, uint32be* statusOut)
		{
			*statusOut = kStatusDisconnected;
			return kResultOk;
		}

		uint32 GetKey(uint32 channel, KeyEvent* eventOut)
		{
			if (channel >= kChannelCount || eventOut == nullptr)
			{
				cemuLog_log(LogType::APIErrors, "KBDGetKey(): bad channel {} or null event pointer", channel);
				return kResultOk;
			}
			eventOut->channel = (uint8)channel;
			eventOut->hidCode = 0;
			eventOut->keyState = 0;
			eventOut->modifiers = 0;
			eventOut->utf16Char = 0;
			return kResultOk;
		}

		class KeyboardLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nsyskbd"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(GetChannelStatus, "nsyskbd", "KBDGetChannelStatus", LogType::Placeholder);
				cafeExportRegisterFunc(GetKey, "nsyskbd", "KBDGetKey", LogType::Placeholder);
			}
		};

		KeyboardLibrary g_keyboardLibrary;
	}

	COSModule* GetModule()
	{
		return &g_keyboardLibrary;
	}
}
