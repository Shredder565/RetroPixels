// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// 64-bit Wii U title id: <platform:16><category:8><unique id...>
// e.g. 00050000-1010ED00 (base game), 0005000E-1010ED00 (its update)
using TitleId = uint64;

static_assert(sizeof(TitleId) == 8);

class TitleIdParser
{
  public:
	// value of the category byte (bits 32-39)
	enum class TITLE_TYPE
	{
		UNKNOWN = 0xFF,
		BASE_TITLE = 0x00,        // disc and eShop games
		BASE_TITLE_DEMO = 0x02,
		BASE_TITLE_UPDATE = 0x0E,
		HOMEBREW = 0x0F,
		AOC = 0x0C,               // DLC
		SYSTEM_TITLE = 0x10,
		SYSTEM_DATA = 0x1B,
		SYSTEM_OVERLAY_TITLE = 0x30,
	};

	TitleIdParser(uint64 titleId) : m_titleId(titleId) {}

	// system titles live in sys/title instead of usr/title
	bool IsSystemTitle() const { return (CategoryByte() & 0x10) != 0; }

	bool IsBaseTitleUpdate() const { return GetType() == TITLE_TYPE::BASE_TITLE_UPDATE; }

	TITLE_TYPE GetType() const
	{
		const uint8 category = CategoryByte();
		switch (static_cast<TITLE_TYPE>(category))
		{
		case TITLE_TYPE::BASE_TITLE:
		case TITLE_TYPE::BASE_TITLE_DEMO:
		case TITLE_TYPE::BASE_TITLE_UPDATE:
		case TITLE_TYPE::HOMEBREW:
		case TITLE_TYPE::AOC:
		case TITLE_TYPE::SYSTEM_TITLE:
		case TITLE_TYPE::SYSTEM_DATA:
		case TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
			return static_cast<TITLE_TYPE>(category);
		default:
			break;
		}
		cemuLog_log(LogType::Force, "Title id {:016x} has an unknown category", m_titleId);
		return TITLE_TYPE::UNKNOWN;
	}

	bool IsPlatformCafe() const { return (m_titleId >> 48) == 0x0005; }

	// only base titles have a separate update title (same id, category 0E)
	bool CanHaveSeparateUpdateTitleId() const { return GetType() == TITLE_TYPE::BASE_TITLE; }

	TitleId GetSeparateUpdateTitleId() const
	{
		cemu_assert_debug(CanHaveSeparateUpdateTitleId());
		return WithCategory(TITLE_TYPE::BASE_TITLE_UPDATE);
	}

	// maps an update title id to its base title id, any other id is returned unchanged
	static TitleId MakeBaseTitleId(TitleId titleId)
	{
		TitleIdParser parsed(titleId);
		if (parsed.GetType() == TITLE_TYPE::BASE_TITLE_UPDATE)
			return parsed.WithCategory(TITLE_TYPE::BASE_TITLE);
		return titleId;
	}

	// parses the first 16 characters as hex digits (anything after is ignored)
	static bool ParseFromStr(std::string_view text, TitleId& titleIdOut)
	{
		if (text.size() < 16)
			return false;
		TitleId value = 0;
		for (size_t i = 0; i < 16; i++)
		{
			const char c = text[i];
			uint32 digit;
			if (c >= '0' && c <= '9')
				digit = c - '0';
			else if (c >= 'a' && c <= 'f')
				digit = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				digit = c - 'A' + 10;
			else
				return false;
			value = (value << 4) | digit;
		}
		titleIdOut = value;
		return true;
	}

  private:
	uint8 CategoryByte() const { return (uint8)(m_titleId >> 32); }

	TitleId WithCategory(TITLE_TYPE category) const
	{
		return (m_titleId & ~(0xFFull << 32)) | ((uint64)category << 32);
	}

	TitleId m_titleId;
};
