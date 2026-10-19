// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Collects the problems found while a graphic pack's patch files are parsed or applied.
// The first problem also writes a heading line to the log; showStageErrorMessageBox()
// turns everything collected into one dialog for the user.
class PatchErrorHandler
{
public:
	enum class STAGE
	{
		PARSER,
		APPLY,
	};

	void setCurrentGraphicPack(class GraphicPack2* gp) { m_gp = gp; }
	void setStage(STAGE s) { m_stage = s; }

	// patchGroup may be null and lineNumber may be negative when that detail is not known
	void printError(class PatchGroup* patchGroup, sint32 lineNumber, std::string_view errorMsg);
	void showStageErrorMessageBox();

	bool hasError() const { return m_anyErrorTriggered; }

	class GraphicPack2* m_gp{};
	bool m_anyErrorTriggered{};
	STAGE m_stage{ STAGE::PARSER };
	std::vector<std::string> errorMessages; // only filled while patch logging is on
};
