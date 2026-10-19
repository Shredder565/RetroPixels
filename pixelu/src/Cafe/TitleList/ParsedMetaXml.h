// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <pugixml.hpp>
#include "config/CemuConfig.h"

// Values read from a title's meta/meta.xml (root element <menu>)
struct ParsedMetaXml
{
	uint32 m_version = 0;
	std::string m_product_code;
	std::string m_company_code;
	std::string m_content_platform;
	uint64 m_title_id = 0;
	CafeConsoleRegion m_region{};

	// indexed by CafeConsoleLanguage
	std::array<std::string, 12> m_long_name;
	std::array<std::string, 12> m_short_name;
	std::array<std::string, 12> m_publisher;

	uint32 m_olv_accesskey = 0;

	// names fall back to English when the requested language has none
	std::string GetShortName(CafeConsoleLanguage languageId) const { return LocalizedOrEnglish(m_short_name, languageId); }
	std::string GetLongName(CafeConsoleLanguage languageId) const { return LocalizedOrEnglish(m_long_name, languageId); }

	TitleId GetTitleId() const { return m_title_id; }
	uint16 GetTitleVersion() const { return (uint16)m_version; }
	CafeConsoleRegion GetRegion() const { return m_region; }
	std::string GetProductCode() const { return m_product_code; }
	std::string GetCompanyCode() const { return m_company_code; }
	uint32 GetOlvAccesskey() const { return m_olv_accesskey; }

	// Parses meta.xml. The buffer is used in place (and modified). Returns
	// nullptr if the document is not a <menu> or has no (non-zero) title_id.
	static ParsedMetaXml* Parse(uint8* xmlData, size_t xmlSize)
	{
		if (xmlSize == 0)
			return nullptr;
		pugi::xml_document doc;
		if (!doc.load_buffer_inplace(xmlData, xmlSize))
			return nullptr;
		pugi::xml_node menu = doc.child("menu");
		if (!menu)
			return nullptr;

		auto meta = std::make_unique<ParsedMetaXml>();
		for (pugi::xml_node node : menu.children())
		{
			const std::string_view tag = node.name();
			const pugi::xml_text text = node.text();
			if (tag == "title_version")
				meta->m_version = text.as_uint();
			else if (tag == "product_code")
				meta->m_product_code = text.as_string();
			else if (tag == "company_code")
				meta->m_company_code = text.as_string();
			else if (tag == "content_platform")
				meta->m_content_platform = text.as_string();
			else if (tag == "title_id")
				meta->m_title_id = ParseHex64(text.as_string());
			else if (tag == "region")
				meta->m_region = (CafeConsoleRegion)text.as_uint();
			else if (tag.starts_with("longname_"))
			{
				if (std::optional<size_t> lang = LanguageSlot(tag.substr(9)))
				{
					// long names may contain line breaks; show them on one line
					std::string name = text.as_string();
					for (char& c : name)
					{
						if (c == '\r' || c == '\n')
							c = ' ';
					}
					meta->m_long_name[*lang] = std::move(name);
				}
			}
			else if (tag.starts_with("shortname_"))
			{
				if (std::optional<size_t> lang = LanguageSlot(tag.substr(10)))
					meta->m_short_name[*lang] = text.as_string();
			}
			else if (tag.starts_with("publisher_"))
			{
				if (std::optional<size_t> lang = LanguageSlot(tag.substr(10)))
					meta->m_publisher[*lang] = text.as_string();
			}
			else if (tag.starts_with("olv_accesskey"))
				meta->m_olv_accesskey = text.as_uint(0xFFFFFFFF);
		}
		if (meta->m_title_id == 0)
			return nullptr;
		return meta.release();
	}

  private:
	static std::string LocalizedOrEnglish(const std::array<std::string, 12>& names, CafeConsoleLanguage languageId)
	{
		const std::string& localized = names[(size_t)languageId];
		return localized.empty() ? names[(size_t)CafeConsoleLanguage::EN] : localized;
	}

	// strtoull semantics (leading spaces, optional sign and 0x); text that is
	// not a number or overflows gives 0
	static uint64 ParseHex64(const char* str)
	{
		char* end = nullptr;
		errno = 0;
		const unsigned long long value = std::strtoull(str, &end, 16);
		if (end == str || errno == ERANGE)
			return 0;
		return value;
	}

	// language suffix of longname_/shortname_/publisher_ tags
	static std::optional<size_t> LanguageSlot(std::string_view suffix)
	{
		static constexpr std::pair<std::string_view, CafeConsoleLanguage> kLanguages[] = {
			{"ja", CafeConsoleLanguage::JA}, {"en", CafeConsoleLanguage::EN}, {"fr", CafeConsoleLanguage::FR},
			{"de", CafeConsoleLanguage::DE}, {"it", CafeConsoleLanguage::IT}, {"es", CafeConsoleLanguage::ES},
			{"zhs", CafeConsoleLanguage::ZH}, {"ko", CafeConsoleLanguage::KO}, {"nl", CafeConsoleLanguage::NL},
			{"pt", CafeConsoleLanguage::PT}, {"ru", CafeConsoleLanguage::RU}, {"zht", CafeConsoleLanguage::TW},
		};
		for (const auto& [code, language] : kLanguages)
		{
			if (suffix == code)
				return (size_t)language;
		}
		return std::nullopt;
	}
};
