// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/app.h>

class MainWindow;
class wxTimer;
class wxTimerEvent;

// The wxWidgets application: start-up (paths, settings, language, mlc folder), the main window,
// global keyboard tracking and shutdown.
class CemuApp : public wxApp
{
public:
	bool OnInit() override;
	int OnExit() override;

	void OnAssertFailure(const wxChar* file, int line, const wxChar* func, const wxChar* cond, const wxChar* msg) override;
	int FilterEvent(wxEvent& event) override;

	// English followed by every language a translation catalog was found for
	std::vector<const wxLanguageInfo*> GetLanguages() const;

	// mlc folder checks and set-up; the *OrFail variants tell the user and quit when the folder is unusable
	static bool CheckMLCPath(const fs::path& mlc);
	static bool CreateDefaultMLCFiles(const fs::path& mlc);
	static void CreateDefaultCemuFiles();

	static void InitializeNewMLCOrFail(fs::path mlc);
	static void InitializeExistingMLCOrFail(fs::path mlc);

	// close the main window and start executablePath once PixelU has shut down
	void RequestRestart(fs::path executablePath);
private:
	void LocalizeUI(wxLanguage languageToUse);

	void DeterminePaths(std::set<fs::path>& failedWriteAccess);

	void ActivateApp(wxActivateEvent& event);
	static std::vector<const wxLanguageInfo*> GetAvailableTranslationLanguages(wxTranslations* translationsMgr);

	MainWindow* m_mainFrame = nullptr;
#if BOOST_OS_MACOS
	// SDL wants its events pumped on the main thread on macOS
	void OnSDLEventPumpTimer(wxTimerEvent& event);
	wxTimer* m_sdlEventPumpTimer = nullptr;
#endif

	wxLocale m_locale;
	std::vector<const wxLanguageInfo*> m_availableTranslations;
	std::optional<fs::path> m_restartExecutable;
};

wxDECLARE_APP(CemuApp);
