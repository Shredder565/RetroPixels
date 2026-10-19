// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SaveInfo.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"

SaveInfo::SaveInfo(TitleId titleId)
	: m_titleId(titleId), m_path(GetSavePath(titleId))
{
	std::error_code ec;
	m_exists = fs::is_directory(m_path, ec);
}

fs::path SaveInfo::GetSavePath(TitleId titleId)
{
	return ActiveSettings::GetMlcPath(fmt::format("usr/save/{:08x}/{:08x}", titleId >> 32, titleId & 0xFFFFFFFF));
}

bool SaveInfo::ParseMetaData()
{
	if (!m_metaLoaded)
	{
		m_metaLoaded = true;
		if (std::optional<std::vector<uint8>> xml = FileStream::LoadIntoMemory(m_path / "meta/meta.xml"))
			m_meta = ParsedMetaXml::Parse(xml->data(), xml->size());
	}
	return m_meta != nullptr;
}
