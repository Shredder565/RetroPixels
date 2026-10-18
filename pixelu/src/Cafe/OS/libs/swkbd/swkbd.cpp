// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// swkbd is the console's on-screen keyboard. PixelU replaces it with its own
// keyboard drawn with imgui over the game (swkbd_render, called by the
// renderer every frame): a text box with the typed text and a clickable key
// layout. The host keyboard types into it too (swkbd_keyInput, fed by the
// main window and the GamePad window while swkbd_hasKeyboardInputHook()).
//
// Two ways a game uses it:
//  - input form (SwkbdAppearInputForm): the game reads the result with
//    SwkbdGetInputFormString once SwkbdIsDecideOkButton says Enter was hit;
//  - keyboard only (SwkbdAppearKeyboard): the game owns the text buffer (set
//    with a ReceiverArg) and is told about every change through its event
//    receiver.
// In both modes every change is also written into the receiver's buffer.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "util/helpers/helpers.h"
#include "resource/IconsFontAwesome5.h"
#include "swkbd.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"

namespace
{
	constexpr sint32 kTextCapacity = 4096; // UTF-16 units, terminator included

	// nn::swkbd::State
	constexpr uint32 kStateHidden = 0;
	constexpr uint32 kStateVisible = 2;

	// ---- structures shared with the game ------------------------------------

	struct EventReceiverVTable
	{
		uint32 unk00;
		uint32 unk04;
		uint32 unk08;
		MEMPTR<void> onDirtyString; // void (IEventReceiver*, const DirtyInfo&)
	};

	struct EventReceiver
	{
		MEMPTR<EventReceiverVTable> vtable;
	};

	// nn::swkbd::ReceiverArg
	struct ReceiverArg
	{
		MEMPTR<EventReceiver> receiver;
		MEMPTR<uint16be> stringBuf;
		sint32be stringBufSize; // in UTF-16 units, terminator included
		sint32be fixedCharLimit;
		sint32be cursorPos;
		sint32be selectFrom;
	};
	static_assert(sizeof(ReceiverArg) == 0x18);

	// nn::swkbd::KeyboardArg (only the receiver part is used)
	struct KeyboardArg
	{
		uint8 config[0xA8];
		ReceiverArg receiver;
	};
	static_assert(offsetof(KeyboardArg, receiver) == 0xA8);

	// nn::swkbd::AppearArg (only the input form part is used)
	struct AppearArg
	{
		uint8 keyboardConfig[0xC0];
		/* +0xC0 */ uint32be inputFormType;
		/* +0xC4 */ uint32be cursorIndex;
		/* +0xC8 */ MEMPTR<uint16be> initialText;
		/* +0xCC */ MEMPTR<uint16be> hintText;
		/* +0xD0 */ uint32be maxTextLength;
	};
	static_assert(offsetof(AppearArg, cursorIndex) == 0xC4);
	static_assert(offsetof(AppearArg, maxTextLength) == 0xD0);

	// nn::swkbd::DirtyInfo
	struct DirtyRange
	{
		uint32be from;
		uint32be to;
	};

	// nn::swkbd::DrawStringInfo
	struct DrawStringInfo
	{
		uint32be values[6];
		uint8 flag;
	};

	// learning dictionary blob ("NJDC") initialised by SwkbdInitLearnDic
	constexpr uint32 kDicSize = 0xA460;
	constexpr uint32 kDicMagic = 0x4E4A4443;
	constexpr uint32 kDicHeaderSize = 0x48;
	constexpr uint32 kDicEntries = 1000;
	constexpr uint32 kDicEntrySize = 0x20;

	struct LearnDic
	{
		/* +0x00 */ uint32be magic;
		/* +0x04 */ uint32be version;
		/* +0x08 */ uint32be format;
		/* +0x0C */ uint32be storageSize;
		/* +0x10 */ uint32be metadataSize;
		/* +0x14 */ uint32be maxWordLength;
		/* +0x18 */ uint32be maxReadingLength;
		/* +0x1C */ uint32be indexEnd;
		/* +0x20 */ uint32be entryBase;
		/* +0x24 */ uint32be unk24;
		/* +0x28 */ uint32be entryCount;
		/* +0x2C */ uint32be entrySize;
		/* +0x30 */ uint32be unk30[3];
		/* +0x3C */ uint32be index1;
		/* +0x40 */ uint32be index2;
		/* +0x44 */ uint32be entriesEnd;
		/* +0x48 */ uint8 data[kDicSize - kDicHeaderSize - 4];
		/* end-4 */ uint32be endMagic;
	};
	static_assert(sizeof(LearnDic) == kDicSize);

	// ---- keyboard state ------------------------------------------------------

	struct Keyboard
	{
		Keyboard()
		{
			// the renderer reads the text while other threads type: never reallocate
			text.reserve(kTextCapacity);
		}

		std::u16string text;
		bool shown = false;
		bool entered = false;      // Enter was hit since the keyboard appeared
		bool receiverOnly = false; // opened with SwkbdAppearKeyboard
		sint32 maxLength = kTextCapacity - 1;
		ReceiverArg receiver{};
		// drawing
		bool shift = false;
		bool enterHeld = false;
		bool backHeld = false;
	};

	std::unique_ptr<Keyboard> g_kb;
	sint32 g_pendingFontWork = 0;
	sint32 g_pendingPredictWork = 0;

	SysAllocator<uint16be, kTextCapacity> g_textForGame;
	SysAllocator<DirtyRange> g_dirtyRange;

	// Text limit for typing: the receiver's buffer in keyboard-only mode.
	sint32 TypingLimit(const Keyboard& kb)
	{
		if (!kb.receiverOnly)
			return kb.maxLength;
		const sint32 bufSize = kb.receiver.stringBufSize;
		return bufSize > 1 ? bufSize - 1 : 0;
	}

	// Mirrors the text into the game's buffer and tells its event receiver.
	void PublishText(Keyboard& kb)
	{
		const sint32 bufSize = kb.receiver.stringBufSize;
		if (bufSize > 1 && kb.receiver.stringBuf)
		{
			uint16be* out = kb.receiver.stringBuf.GetPtr();
			const sint32 n = std::min<sint32>(bufSize - 1, (sint32)kb.text.size());
			for (sint32 i = 0; i < n; i++)
				out[i] = (uint16)kb.text[i];
			out[n] = 0;
		}
		EventReceiver* receiver = kb.receiver.receiver.GetPtr();
		if (!receiver || !receiver->vtable)
			return;
		const MPTR callback = receiver->vtable->onDirtyString.GetMPTR();
		if (!callback)
			return;
		g_dirtyRange->from = 0;
		g_dirtyRange->to = 0;
		coreinitAsyncCallback_add(callback, 2, memory_getVirtualOffsetFromPointer(receiver), g_dirtyRange.GetMPTR());
	}

	void ShowKeyboard(bool receiverOnly)
	{
		g_kb->text.clear();
		g_kb->shown = true;
		g_kb->entered = false;
		g_kb->receiverOnly = receiverOnly;
	}

	std::string TextAsUtf8(const std::u16string& text)
	{
		std::wstring wide(text.begin(), text.end());
		return boost::nowide::narrow(wide);
	}

	// ---- exports -------------------------------------------------------------

	void SwkbdCreate(uint8* workMemory, uint32 regionType, uint32 unk, coreinit::FSClient_t* fsClient)
	{
		if (!g_kb)
			g_kb = std::make_unique<Keyboard>();
		// the real library takes a while to load its fonts here; Monster Hunter 3
		// Ultimate softlocks if this returns at once
		coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK);
		g_pendingFontWork = 3;
		g_pendingPredictWork = 3;
	}

	uint32 SwkbdGetStateKeyboard()
	{
		return g_kb && g_kb->shown ? kStateVisible : kStateHidden;
	}

	uint32 SwkbdGetStateInputForm()
	{
		return SwkbdGetStateKeyboard();
	}

	uint32 SwkbdSetReceiver(ReceiverArg* arg)
	{
		if (g_kb)
			g_kb->receiver = *arg;
		return 0;
	}

	uint32 SwkbdAppearInputForm(AppearArg* arg)
	{
		if (!g_kb)
			return 0;
		ShowKeyboard(false);
		const sint32 requested = (sint32)(uint32)arg->maxTextLength;
		g_kb->maxLength = requested <= 0 ? kTextCapacity - 1 : std::min(requested, kTextCapacity - 1);
		if (const uint16be* initial = arg->initialText.GetPtr())
		{
			for (sint32 i = 0; i < g_kb->maxLength && initial[i] != 0; i++)
				g_kb->text.push_back((char16_t)(uint16)initial[i]);
		}
		return 1;
	}

	uint32 SwkbdAppearKeyboard(KeyboardArg* arg)
	{
		if (!g_kb)
			return 0;
		ShowKeyboard(true);
		g_kb->receiver = arg->receiver;
		return 1;
	}

	uint32 SwkbdDisappearInputForm()
	{
		if (g_kb)
			g_kb->shown = false;
		return 1;
	}

	uint32 SwkbdDisappearKeyboard()
	{
		return SwkbdDisappearInputForm();
	}

	// Pointer to a null-terminated big-endian copy of the typed text.
	uint16be* SwkbdGetInputFormString()
	{
		uint16be* out = g_textForGame.GetPtr();
		const size_t n = g_kb ? std::min<size_t>(g_kb->text.size(), kTextCapacity - 1) : 0;
		for (size_t i = 0; i < n; i++)
			out[i] = (uint16)g_kb->text[i];
		out[n] = 0;
		return out;
	}

	// Enter always accepts the input.
	uint32 SwkbdIsDecideOkButton(uint8* unused)
	{
		return g_kb && g_kb->entered ? 1 : 0;
	}

	uint32 SwkbdGetDrawStringInfo(DrawStringInfo* info)
	{
		for (auto& v : info->values)
			v = 0xFFFFFFFF;
		info->flag = 0;
		return 0;
	}

	// Writes an empty dictionary in the layout the system keyboard uses.
	uint32 SwkbdInitLearnDic(LearnDic* dic)
	{
		if (!g_kb || !dic)
			return 0;
		const uint32 index1 = kDicHeaderSize;
		const uint32 index2 = index1 + (kDicEntries + 1) * 2;
		const uint32 indexEnd = index2 + (kDicEntries + 1) * 2;
		memset(dic, 0, sizeof(LearnDic));
		dic->magic = kDicMagic;
		dic->version = 0x30000;
		dic->format = 0x80020000;
		dic->storageSize = kDicSize - kDicHeaderSize;
		dic->metadataSize = 0x2C;
		dic->maxWordLength = 0x6E;
		dic->maxReadingLength = 0x6E;
		dic->indexEnd = indexEnd;
		dic->entryCount = kDicEntries;
		dic->entrySize = kDicEntrySize;
		dic->index1 = index1;
		dic->index2 = index2;
		dic->entriesEnd = indexEnd + kDicEntries * kDicEntrySize;
		dic->endMagic = kDicMagic;
		return 1;
	}

	// The game runs these on worker threads after SwkbdCreate; a few rounds of
	// pretend work (5 ms each) keep its loading sequence happy.
	bool SwkbdIsNeedCalcSubThreadFont()
	{
		return g_pendingFontWork > 0;
	}

	bool SwkbdIsNeedCalcSubThreadPredict()
	{
		return g_pendingPredictWork > 0;
	}

	void DoPendingWork(sint32& counter)
	{
		if (counter <= 0)
			return;
		counter--;
		coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK / 200);
	}

	void SwkbdCalcSubThreadFont()
	{
		DoPendingWork(g_pendingFontWork);
	}

	void SwkbdCalcSubThreadPredict()
	{
		DoPendingWork(g_pendingPredictWork);
	}

	void SwkbdCalc(void* controllerInfo)
	{
	}

	// ---- drawing -------------------------------------------------------------

	enum class KeyKind
	{
		Char,
		Backspace,
		Shift,
		Enter,
		EndOfRow,
	};

	struct Key
	{
		KeyKind kind;
		char ch; // for KeyKind::Char
	};

	constexpr Key C(char c) { return { KeyKind::Char, c }; }
	constexpr Key kBack{ KeyKind::Backspace, 0 };
	constexpr Key kShift{ KeyKind::Shift, 0 };
	constexpr Key kEnter{ KeyKind::Enter, 0 };
	constexpr Key kRowEnd{ KeyKind::EndOfRow, 0 };

	constexpr Key kLowerLayout[] = {
		C('1'), C('2'), C('3'), C('4'), C('5'), C('6'), C('7'), C('8'), C('9'), C('0'), C('-'), kBack, kRowEnd,
		C('q'), C('w'), C('e'), C('r'), C('t'), C('y'), C('u'), C('i'), C('o'), C('p'), C('/'), kRowEnd,
		C('a'), C('s'), C('d'), C('f'), C('g'), C('h'), C('j'), C('k'), C('l'), C(':'), C('\''), kRowEnd,
		C('z'), C('x'), C('c'), C('v'), C('b'), C('n'), C('m'), C(','), C('.'), C('?'), C('!'), kRowEnd,
		kShift, C(' '), kEnter,
	};

	constexpr Key kUpperLayout[] = {
		C('#'), C('['), C(']'), C('$'), C('%'), C('^'), C('&'), C('*'), C('('), C(')'), C('_'), kBack, kRowEnd,
		C('Q'), C('W'), C('E'), C('R'), C('T'), C('Y'), C('U'), C('I'), C('O'), C('P'), C('@'), kRowEnd,
		C('A'), C('S'), C('D'), C('F'), C('G'), C('H'), C('J'), C('K'), C('L'), C(';'), C('"'), kRowEnd,
		C('Z'), C('X'), C('C'), C('V'), C('B'), C('N'), C('M'), C('<'), C('>'), C('+'), C('='), kRowEnd,
		kShift, C(' '), kEnter,
	};

	const char* KeyLabel(const Key& key, char (&buffer)[2])
	{
		switch (key.kind)
		{
		case KeyKind::Backspace: return _utf8WrapperPtr(ICON_FA_ARROW_CIRCLE_LEFT);
		case KeyKind::Shift: return _utf8WrapperPtr(ICON_FA_ARROW_UP);
		case KeyKind::Enter: return _utf8WrapperPtr(ICON_FA_CHECK);
		default:
			buffer[0] = key.ch;
			buffer[1] = '\0';
			return buffer;
		}
	}

	void DrawKeys(Keyboard& kb, float keyWidth)
	{
		const auto& layout = kb.shift ? kUpperLayout : kLowerLayout;
		for (const Key& key : layout)
		{
			if (key.kind == KeyKind::EndOfRow)
			{
				ImGui::NewLine();
				continue;
			}
			char buffer[2];
			const char* label = KeyLabel(key, buffer);
			const bool isSpace = key.kind == KeyKind::Char && key.ch == ' ';
			if (ImGui::Button(label, { isSpace ? 537.0f : keyWidth + 5, 0 }))
			{
				switch (key.kind)
				{
				case KeyKind::Backspace: swkbd_keyInput(8); break;
				case KeyKind::Enter: swkbd_keyInput(13); break;
				case KeyKind::Shift: kb.shift = !kb.shift; break;
				default: swkbd_keyInput((uint8)key.ch); break;
				}
			}
			ImGui::SameLine();
		}
		ImGui::NewLine();
	}

	// Calls action once when a held button goes down.
	void OnPress(bool down, bool& held, uint32 keyCode)
	{
		if (down && !held)
			swkbd_keyInput(keyCode);
		held = down;
	}
}

void swkbd_render(bool mainWindow)
{
	if (!g_kb || !g_kb->shown)
		return;
	Keyboard& kb = *g_kb;

	ImGuiIO& io = ImGui::GetIO();
	ImFont* bigFont = ImGui_GetFont(48.0f);
	ImFont* keyFont = ImGui_GetFont(24.0f);
	if (!bigFont || !keyFont)
		return;

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);

	// dim the whole game picture
	ImGui::SetNextWindowPos({ 0, 0 }, ImGuiCond_Always);
	ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImGui::Begin("Background overlay", nullptr, flags | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::End();
	ImGui::PopStyleVar(2);

	// text box, wide enough for the longest allowed text
	ImVec2 anchor = { io.DisplaySize.x / 2.0f, io.DisplaySize.y / 3.0f };
	const ImVec2 centered = { 0.5f, 0.5f };
	const float keyWidth = bigFont->GetCharAdvance('W');
	const sint32 chars = std::max(4, std::max(kb.maxLength, (sint32)kb.receiver.stringBufSize));
	ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, centered);
	ImGui::SetNextWindowSize({ std::min(io.DisplaySize.x * 0.9f, keyWidth * chars + 90), 0 }, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.9f);
	ImGui::PushFont(bigFont);
	if (ImGui::Begin("Keyboard Input", nullptr, flags))
	{
		ImGui::Text("%s", _utf8WrapperPtr(ICON_FA_KEYBOARD));
		ImGui::SameLine(70);

		// blinking cursor, 500 ms on / 500 ms off
		static auto s_blinkSince = tick_cached();
		static bool s_cursorOn = false;
		const auto now = tick_cached();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_blinkSince).count() >= 500)
		{
			s_cursorOn = !s_cursorOn;
			s_blinkSince = now;
		}
		std::string shown = TextAsUtf8(kb.text);
		if (s_cursorOn)
			shown += "|";
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted(shown.c_str(), shown.c_str() + shown.size());
		ImGui::PopTextWrapPos();

		anchor.y += ImGui::GetWindowSize().y + 100.0f;
	}
	ImGui::End();
	ImGui::PopFont();

	// clickable keys below it
	ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, centered);
	ImGui::SetNextWindowBgAlpha(0.9f);
	ImGui::PushFont(keyFont);
	if (ImGui::Begin(fmt::format("Software keyboard##SoftwareKeyboard{}", mainWindow).c_str(), nullptr, flags))
		DrawKeys(kb, keyWidth);
	ImGui::End();

	// controller: cancel = backspace, input = enter
	OnPress(io.NavInputs[ImGuiNavInput_Cancel] > 0, kb.backHeld, 8);
	OnPress(io.NavInputs[ImGuiNavInput_Input] > 0, kb.enterHeld, 13);

	ImGui::PopFont();
	ImGui::PopStyleColor();
}

bool swkbd_hasKeyboardInputHook()
{
	return g_kb && g_kb->shown;
}

void swkbd_keyInput(uint32 keyCode)
{
	if (!g_kb)
		return;
	Keyboard& kb = *g_kb;
	if (keyCode == 8 || keyCode == 127) // backspace, delete
	{
		if (!kb.text.empty())
			kb.text.pop_back();
		PublishText(kb);
		return;
	}
	if (keyCode == 13) // enter
	{
		kb.entered = true;
		return;
	}
	if (keyCode < 32 || keyCode > 0xFFFF) // other control keys, and what UTF-16 can't hold in one unit
		return;
	if ((sint32)kb.text.size() >= TypingLimit(kb))
		return;
	kb.text.push_back((char16_t)keyCode);
	PublishText(kb);
}

namespace swkbd
{
	namespace
	{
		class SwkbdLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "swkbd"; }

			void RPLMapped() override
			{
				constexpr auto lt = LogType::SWKBD;
				cafeExportRegisterFunc(SwkbdCreate, "swkbd", "SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient", lt);
				cafeExportRegisterFunc(SwkbdCalc, "swkbd", "SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo", lt);
				cafeExportRegisterFunc(SwkbdGetStateKeyboard, "swkbd", "SwkbdGetStateKeyboard__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdGetStateInputForm, "swkbd", "SwkbdGetStateInputForm__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdSetReceiver, "swkbd", "SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg", lt);
				cafeExportRegisterFunc(SwkbdAppearInputForm, "swkbd", "SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg", lt);
				cafeExportRegisterFunc(SwkbdDisappearInputForm, "swkbd", "SwkbdDisappearInputForm__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdDisappearKeyboard, "swkbd", "SwkbdDisappearKeyboard__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdAppearKeyboard, "swkbd", "SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg", lt);
				cafeExportRegisterFunc(SwkbdGetInputFormString, "swkbd", "SwkbdGetInputFormString__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdIsDecideOkButton, "swkbd", "SwkbdIsDecideOkButton__3RplFPb", lt);
				cafeExportRegisterFunc(SwkbdInitLearnDic, "swkbd", "SwkbdInitLearnDic__3RplFPv", lt);
				cafeExportRegisterFunc(SwkbdGetDrawStringInfo, "swkbd", "SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo", lt);
				cafeExportRegisterFunc(SwkbdCalcSubThreadFont, "swkbd", "SwkbdCalcSubThreadFont__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdCalcSubThreadPredict, "swkbd", "SwkbdCalcSubThreadPredict__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdIsNeedCalcSubThreadFont, "swkbd", "SwkbdIsNeedCalcSubThreadFont__3RplFv", lt);
				cafeExportRegisterFunc(SwkbdIsNeedCalcSubThreadPredict, "swkbd", "SwkbdIsNeedCalcSubThreadPredict__3RplFv", lt);
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
			{
				if (reason == coreinit::RplEntryReason::Loaded)
					g_kb.reset();
			}
		};

		SwkbdLibrary g_swkbdLibrary;
	}

	COSModule* GetModule()
	{
		return &g_swkbdLibrary;
	}
}
