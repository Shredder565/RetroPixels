// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "TitleId.h"
#include "ParsedMetaXml.h"

// A title's save folder in the mlc (usr/save/<id high>/<id low>)
class SaveInfo
{
  public:
	SaveInfo() {}
	SaveInfo(TitleId titleId);

	bool IsValid() const { return m_exists; } // the save folder exists
	TitleId GetTitleId() const { return m_titleId; }
	fs::path GetPath() const { return m_path; }

	// reads meta/meta.xml of the save folder (once)
	bool ParseMetaData();
	ParsedMetaXml* GetMetaInfo() { return m_meta; }

  private:
	static fs::path GetSavePath(TitleId titleId);

	TitleId m_titleId{};
	fs::path m_path;
	bool m_exists{false};
	bool m_metaLoaded{false};
	// shared between copies and never freed, copies handed out by CafeSaveList may outlive the list entry
	ParsedMetaXml* m_meta{nullptr};
};
