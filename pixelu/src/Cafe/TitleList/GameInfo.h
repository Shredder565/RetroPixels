// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "TitleInfo.h"

// A game as the user sees it: the base title plus the best update and DLC found
class GameInfo2
{
  public:
	~GameInfo2()
	{
		m_base.UnmountAll();
		m_update.UnmountAll();
		for (TitleInfo& aoc : m_aoc)
			aoc.UnmountAll();
	}

	bool IsValid() const { return m_base.IsValid(); } // a game needs at least its base title
	bool IsSystemDataTitle() const { return m_base.IsSystemDataTitle(); }

	void SetBase(const TitleInfo& titleInfo)
	{
		if (Supersedes(titleInfo, m_base))
			m_base = titleInfo;
	}

	void SetUpdate(const TitleInfo& titleInfo)
	{
		if (Supersedes(titleInfo, m_update))
			m_update = titleInfo;
	}

	bool HasUpdate() const { return m_update.IsValid(); }

	// one entry per DLC title id, keeping the preferred copy
	void AddAOC(const TitleInfo& titleInfo)
	{
		const TitleId titleId = titleInfo.GetAppTitleId();
		auto existing = std::find_if(m_aoc.begin(), m_aoc.end(), [titleId](const TitleInfo& t) { return t.GetAppTitleId() == titleId; });
		if (existing != m_aoc.end())
		{
			if (!Supersedes(titleInfo, *existing))
				return;
			m_aoc.erase(existing);
		}
		m_aoc.push_back(titleInfo);
	}

	bool HasAOC() const { return !m_aoc.empty(); }

	TitleInfo& GetBase() { return m_base; }
	TitleInfo& GetUpdate() { return m_update; }
	std::span<TitleInfo> GetAOC() { return m_aoc; }

	TitleId GetBaseTitleId()
	{
		cemu_assert_debug(m_base.IsValid());
		return m_base.GetAppTitleId();
	}

	std::string GetTitleName()
	{
		cemu_assert_debug(m_base.IsValid());
		return m_base.GetMetaTitleName();
	}

	// version, SDK and region come from the update when there is one
	uint16 GetVersion() const { return Effective().GetAppTitleVersion(); }
	uint32 GetSDKVersion() const { return Effective().GetAppSDKVersion(); }
	CafeConsoleRegion GetRegion() const { return Effective().GetMetaRegion(); }

	uint16 GetAOCVersion() const { return m_aoc.empty() ? 0 : m_aoc.front().GetAppTitleVersion(); }

	fs::path GetSaveFolder();

  private:
	const TitleInfo& Effective() const { return m_update.IsValid() ? m_update : m_base; }

	// Should 'candidate' replace 'current'? Any valid title beats an invalid one,
	// otherwise the higher version wins. On equal versions a NUS copy does not
	// replace a title in another format (NUS support came later and existing
	// setups should keep using what they used before).
	static bool Supersedes(const TitleInfo& candidate, const TitleInfo& current)
	{
		if (!current.IsValid())
			return true;
		const uint16 candidateVersion = candidate.GetAppTitleVersion();
		const uint16 currentVersion = current.GetAppTitleVersion();
		if (candidateVersion != currentVersion)
			return candidateVersion > currentVersion;
		const bool candidateIsNus = candidate.GetFormat() == TitleInfo::TitleDataFormat::NUS;
		const bool currentIsNus = current.GetFormat() == TitleInfo::TitleDataFormat::NUS;
		return !(candidateIsNus && !currentIsNus);
	}

	TitleInfo m_base;
	TitleInfo m_update;
	std::vector<TitleInfo> m_aoc;
};
