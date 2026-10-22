// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// erreula is the system's error viewer: a game asks it to show an error code
// or a message with one or two buttons, keeps calling ErrEulaCalc every frame
// and polls which button the user picked. PixelU draws the dialog with imgui
// over the game picture (render(), called by the renderer every frame) and
// mimics the fade in / fade out timing of the real viewer.
//
// Embedded mode (--rp-embed, RetroPixels): when online play isn't available,
// error code dialogs about online services (codes 102-xxxx account server,
// 103-xxxx internet connection, 115-xxxx Miiverse, 118-xxxx online play) are
// answered with OK on the user's behalf and never drawn, so the game carries
// on instead of stopping on a dialog that can't help. Nothing else changes.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "util/helpers/helpers.h"
#include "erreula.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"

namespace nn::erreula
{
	namespace
	{
		// ErrEula::ErrorType
		enum class DialogKind : uint32
		{
			Code = 0,          // error code, OK button
			Text = 1,          // message, OK button
			TextOneButton = 2, // message, one custom button
			TextTwoButton = 3, // message, two custom buttons
		};

		// ErrEula::State
		enum class ViewerState : uint32
		{
			Hidden = 0,
			Appearing = 1,
			Visible = 2,
			Disappearing = 3,
		};

		// ErrEula::ResultType
		enum class ResultKind : uint32
		{
			None = 0,
			Finish = 1,
			Next = 2,
			Jump = 3,
			Password = 4,
		};

		constexpr uint64 kFadeMs = 80;

		// ErrEula::AppearArg as the game passes it
		struct AppearArg
		{
			betype<DialogKind> kind;
			uint32be screenType;
			uint32be controllerType;
			uint32be holdType;
			uint32be errorCode; // XXXYYYY, shown as XXX-YYYY
			uint32be framerate;
			MEMPTR<uint16be> text;
			MEMPTR<uint16be> button1Text;
			MEMPTR<uint16be> button2Text;
			MEMPTR<uint16be> title;
			uint8 pad[3];
			bool drawCursor;
		};
		static_assert(sizeof(AppearArg) == 0x2C);

		struct HomeNixSignArg
		{
			uint32be framerate;
		};
		static_assert(sizeof(HomeNixSignArg) == 4);

		struct ControllerInfo
		{
			MEMPTR<void> vpad;
			MEMPTR<void> kpad[4];
		};
		static_assert(sizeof(ControllerInfo) == 0x14);

		enum class Button : sint32
		{
			None = -1,
			Left = 0,
			Right = 1,
		};

		// One error viewer, alive between ErrEulaCreate and ErrEulaDestroy.
		class Viewer
		{
		public:
			Viewer()
			{
				SetState(ViewerState::Hidden);
			}

			void Appear(bool answerForUser)
			{
				m_answerForUser = answerForUser;
				m_chosen = Button::None;
				m_result = -1;
				m_resultLeft = 0;
				m_resultRight = 1;
				SetState(ViewerState::Appearing);
			}

			void Disappear()
			{
				if (m_state == ViewerState::Visible)
					SetState(ViewerState::Disappearing);
			}

			// Fades finish on their own once kFadeMs have passed.
			void Update()
			{
				if (!IsFading() || ElapsedTicks() <= FadeTicks())
					return;
				if (m_state == ViewerState::Appearing)
				{
					SetState(ViewerState::Visible);
					if (m_answerForUser && m_chosen == Button::None)
						Choose(Button::Left); // OK
				}
				else
				{
					SetState(ViewerState::Hidden);
				}
			}

			void Choose(Button button)
			{
				if (m_chosen != Button::None)
					return;
				m_chosen = button;
				m_result = button == Button::Left ? m_resultLeft : m_resultRight;
			}

			bool AnyChosen() const { return m_chosen != Button::None; }
			// These two report "true" when the named button was NOT the one
			// picked; that is what games have always seen from PixelU here.
			bool LeftQuery() const { return m_chosen != Button::Left; }
			bool RightQuery() const { return m_chosen != Button::Right; }

			ViewerState State() const { return m_state; }
			bool AnsweredForUser() const { return m_answerForUser; }
			sint32 Result() const { return m_result; }

			ResultKind ResultType() const
			{
				if (m_result == -1)
					return ResultKind::None;
				if (m_result < 10)
					return ResultKind::Finish;
				if (m_result >= 9999)
					return ResultKind::Next;
				if (m_result == 40)
					return ResultKind::Password;
				return ResultKind::Jump;
			}

			// 0..1 opacity while fading, 1 otherwise.
			float Opacity() const
			{
				if (!IsFading())
					return 1.0f;
				const float t = (float)ElapsedTicks() / (float)FadeTicks();
				return m_state == ViewerState::Appearing ? std::min(1.0f, t) : std::max(0.0f, 1.0f - t);
			}

		private:
			bool IsFading() const
			{
				return m_state == ViewerState::Appearing || m_state == ViewerState::Disappearing;
			}

			static uint32 FadeTicks()
			{
				return (uint32)coreinit::EspressoTime::ConvertMsToTimerTicks(kFadeMs);
			}

			uint32 ElapsedTicks() const
			{
				return (uint32)coreinit::OSGetTime() - m_stateSince;
			}

			void SetState(ViewerState state)
			{
				m_state = state;
				m_stateSince = (uint32)coreinit::OSGetTime();
			}

			ViewerState m_state = ViewerState::Hidden;
			uint32 m_stateSince = 0;
			bool m_answerForUser = false;
			Button m_chosen = Button::None;
			sint32 m_result = -1;
			sint32 m_resultLeft = 0;
			sint32 m_resultRight = 0;
		};

		struct Globals
		{
			SysAllocator<coreinit::OSMutex> mutex;
			uint32 region = 0;
			uint32 language = 0;
			MEMPTR<coreinit::FSClient_t> fsClient;
			std::unique_ptr<Viewer> viewer;
			AppearArg dialog{}; // the dialog being shown
			bool homeNixSign = false;
		};
		Globals g_erreula;

		void ResetGlobals()
		{
			g_erreula.viewer.reset();
			g_erreula.region = 0;
			g_erreula.language = 0;
			g_erreula.fsClient = nullptr;
			g_erreula.dialog = AppearArg{};
			g_erreula.homeNixSign = false;
		}

		std::string Utf8FromGame(const uint16be* text)
		{
			std::wstring wide;
			for (; *text != 0; text++)
				wide.push_back((wchar_t)(uint16)*text);
			return boost::nowide::narrow(wide);
		}

		std::string GameTextOr(const MEMPTR<uint16be>& text, const char* fallback)
		{
			return text ? Utf8FromGame(text.GetPtr()) : std::string(fallback);
		}

		// Online-service error codes that are answered for the user in embedded
		// mode when online play isn't available (see top of file).
		bool AnswerForUser(const AppearArg& arg)
		{
			if (!LaunchSettings::RPEmbedEnabled())
				return false;
			const DialogKind kind = arg.kind;
			if (kind != DialogKind::Code && kind != DialogKind::Text)
				return false; // only plain OK dialogs
			switch ((uint32)arg.errorCode / 10000)
			{
			case 102: // Nintendo Network ID / account server
			case 103: // internet connection
			case 115: // Miiverse
			case 118: // online play
				break;
			default:
				return false;
			}
			return !ActiveSettings::IsOnlineEnabled();
		}

		// ---- exports -------------------------------------------------------------

		void ErrEulaCreate(void* workMemory, uint32 region, uint32 language, coreinit::FSClient_t* fsClient)
		{
			coreinit::OSLockMutex(&g_erreula.mutex);
			g_erreula.region = region;
			g_erreula.language = language;
			g_erreula.fsClient = fsClient;
			g_erreula.viewer = std::make_unique<Viewer>();
			coreinit::OSUnlockMutex(&g_erreula.mutex);
		}

		void ErrEulaDestroy()
		{
			g_erreula.viewer.reset();
		}

		bool ErrEulaIsDecideSelectButtonError()
		{
			return g_erreula.viewer && g_erreula.viewer->AnyChosen();
		}

		bool ErrEulaIsDecideSelectLeftButtonError()
		{
			return g_erreula.viewer && g_erreula.viewer->LeftQuery();
		}

		bool ErrEulaIsDecideSelectRightButtonError()
		{
			return g_erreula.viewer && g_erreula.viewer->RightQuery();
		}

		sint32 ErrEulaGetResultCode()
		{
			return g_erreula.viewer ? g_erreula.viewer->Result() : -1;
		}

		ResultKind ErrEulaGetResultType()
		{
			return g_erreula.viewer ? g_erreula.viewer->ResultType() : ResultKind::None;
		}

		void ErrEulaAppearError(AppearArg* arg)
		{
			g_erreula.dialog = *arg;
			const bool answer = AnswerForUser(*arg);
			if (answer)
			{
				const uint32 code = arg->errorCode;
				cemuLog_log(LogType::Force, "ErrEula: Error-Code {:03}-{:04} (online, and online mode isn't available) answered with OK", code / 10000, code % 10000);
			}
			if (g_erreula.viewer)
				g_erreula.viewer->Appear(answer);
		}

		void ErrEulaDisappearError()
		{
			if (g_erreula.viewer)
				g_erreula.viewer->Disappear();
		}

		ViewerState ErrEulaGetStateErrorViewer()
		{
			return g_erreula.viewer ? g_erreula.viewer->State() : ViewerState::Hidden;
		}

		void ErrEulaCalc(ControllerInfo* controllers)
		{
			if (g_erreula.viewer)
				g_erreula.viewer->Update();
		}

		// The "HOME button not available" sign is only remembered, not drawn.
		uint32 ErrEulaAppearHomeNixSign(HomeNixSignArg* arg)
		{
			g_erreula.homeNixSign = true;
			return 0;
		}

		uint32 ErrEulaDisappearHomeNixSign()
		{
			g_erreula.homeNixSign = false;
			return 0;
		}

		uint32 ErrEulaIsAppearHomeNixSign()
		{
			return g_erreula.homeNixSign ? 1 : 0;
		}

		uint32 ErrEulaChangeLang(uint32 language)
		{
			g_erreula.language = language;
			return 0;
		}

		// ---- drawing -------------------------------------------------------------

		float ButtonWidth(const std::string& label)
		{
			return std::max(100.0f, ImGui::CalcTextSize(label.c_str()).x + 10.0f);
		}

		// Draws the dialog body and returns the button the user clicked this frame.
		Button DrawDialogBody(const AppearArg& dialog, std::string text)
		{
			const float centerX = ImGui::GetWindowSize().x / 2.0f;
			const DialogKind kind = dialog.kind;
			if (kind == DialogKind::Text || kind == DialogKind::TextOneButton || kind == DialogKind::TextTwoButton)
				text += GameTextOr(dialog.text, "Unknown Error");
			ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
			ImGui::Spacing();

			if (kind == DialogKind::TextOneButton)
			{
				const std::string label = GameTextOr(dialog.button1Text, "Yes");
				const float width = ButtonWidth(label);
				ImGui::SetCursorPosX(centerX - width / 2.0f);
				return ImGui::Button(label.c_str(), { width, 0 }) ? Button::Left : Button::None;
			}
			if (kind == DialogKind::TextTwoButton)
			{
				const std::string left = GameTextOr(dialog.button1Text, "Yes");
				const std::string right = GameTextOr(dialog.button2Text, "No");
				const float leftWidth = ButtonWidth(left);
				const float rightWidth = ButtonWidth(right);
				ImGui::SetCursorPosX(centerX - leftWidth / 2.0f - rightWidth / 2.0f - 10);
				const bool leftClicked = ImGui::Button(left.c_str(), { leftWidth, 0 });
				ImGui::SameLine();
				const bool rightClicked = ImGui::Button(right.c_str(), { rightWidth, 0 });
				// left wins if both register in one frame
				return leftClicked ? Button::Left : (rightClicked ? Button::Right : Button::None);
			}
			// error code, plain text and unknown kinds: a single OK button
			ImGui::SetCursorPosX(centerX - 50);
			return ImGui::Button("OK", { 100, 0 }) ? Button::Left : Button::None;
		}
	}

	void render(bool mainWindow)
	{
		Viewer* viewer = g_erreula.viewer.get();
		if (!viewer || viewer->State() == ViewerState::Hidden)
			return;
		if (viewer->AnsweredForUser())
			return; // answered in embedded mode, never shown

		const AppearArg& dialog = g_erreula.dialog;
		std::string text;
		if (const uint32 code = dialog.errorCode; code != 0)
			text = fmt::format("Error-Code: {:03}-{:04}\n", code / 10000, code % 10000);

		ImFont* font = ImGui_GetFont(32.0f);
		if (!font)
			return;

		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
		const ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos({ io.DisplaySize.x / 2.0f, io.DisplaySize.y / 2.0f }, ImGuiCond_Always, { 0.5f, 0.5f });
		ImGui::PushFont(font);

		// imgui needs a non-empty window title
		std::string title = GameTextOr(dialog.title, "");
		if (title.empty())
			title = "ErrEula";

		const float opacity = viewer->Opacity();
		ImGuiStyle& style = ImGui::GetStyle();
		const float savedAlpha = style.Alpha;
		style.Alpha = opacity;
		ImGui::SetNextWindowBgAlpha(0.9f * opacity);
		if (ImGui::Begin(title.c_str(), nullptr, flags))
		{
			const Button clicked = DrawDialogBody(dialog, std::move(text));
			if (clicked != Button::None && !viewer->AnyChosen())
				viewer->Choose(clicked);
		}
		ImGui::End();
		ImGui::PopFont();
		style.Alpha = savedAlpha;
	}

	namespace
	{
		class ErreulaLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "erreula"; }

			void RPLMapped() override
			{
				constexpr auto lt = LogType::Placeholder;
				cafeExportRegisterFunc(ErrEulaCreate, "erreula", "ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient", lt);
				cafeExportRegisterFunc(ErrEulaDestroy, "erreula", "ErrEulaDestroy__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaIsDecideSelectButtonError, "erreula", "ErrEulaIsDecideSelectButtonError__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaIsDecideSelectLeftButtonError, "erreula", "ErrEulaIsDecideSelectLeftButtonError__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaIsDecideSelectRightButtonError, "erreula", "ErrEulaIsDecideSelectRightButtonError__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaGetResultCode, "erreula", "ErrEulaGetResultCode__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaGetResultType, "erreula", "ErrEulaGetResultType__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaAppearError, "erreula", "ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg", lt);
				cafeExportRegisterFunc(ErrEulaDisappearError, "erreula", "ErrEulaDisappearError__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaGetStateErrorViewer, "erreula", "ErrEulaGetStateErrorViewer__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaCalc, "erreula", "ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo", lt);
				cafeExportRegisterFunc(ErrEulaAppearHomeNixSign, "erreula", "ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg", lt);
				cafeExportRegisterFunc(ErrEulaChangeLang, "erreula", "ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType", lt);
				cafeExportRegisterFunc(ErrEulaIsAppearHomeNixSign, "erreula", "ErrEulaIsAppearHomeNixSign__3RplFv", lt);
				cafeExportRegisterFunc(ErrEulaDisappearHomeNixSign, "erreula", "ErrEulaDisappearHomeNixSign__3RplFv", lt);
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
			{
				if (reason == coreinit::RplEntryReason::Loaded)
				{
					g_erreula.viewer.reset();
					coreinit::OSInitMutexEx(&g_erreula.mutex, nullptr);
				}
				else if (reason == coreinit::RplEntryReason::Unloaded)
				{
					ResetGlobals();
				}
			}
		};

		ErreulaLibrary g_erreulaLibrary;
	}

	COSModule* GetModule()
	{
		return &g_erreulaLibrary;
	}
}
