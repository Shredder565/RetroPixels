// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <zip.h>

// Read-only access to a zip file through libzip. Entry names are UTF-8.
// Entries whose names could escape an extraction folder (absolute paths,
// drive letters, backslashes, "." or ".." components) and directory entries
// are not reported.
class ZipArchive
{
public:
	struct ZipFileEntry
	{
		std::string_view fullPath;
		std::string_view filename; // last path component
	};

	ZipArchive(const fs::path& path)
	{
		m_zipPath = _pathToUtf8(path);
		int openError = 0;
		m_zip = zip_open(m_zipPath.c_str(), ZIP_RDONLY, &openError);
		if (!m_zip)
			cemuLog_log(LogType::Force, "Could not open zip archive \"{}\" (libzip error {})", m_zipPath, openError);
	}

	~ZipArchive()
	{
		if (m_zip)
			zip_close(m_zip);
	}

	ZipArchive(const ZipArchive&) = delete;
	ZipArchive& operator=(const ZipArchive&) = delete;

	bool IsValid()
	{
		return m_zip != nullptr;
	}

	size_t GetNumEntries()
	{
		cemu_assert(m_zip);
		const zip_int64_t n = zip_get_num_entries(m_zip, 0);
		return n > 0 ? (size_t)n : 0;
	}

	// false for invalid, unsafe or directory entries
	bool GetFileEntryByIndex(size_t index, ZipFileEntry& entry)
	{
		zip_stat_t info{};
		if (zip_stat_index(m_zip, (zip_uint64_t)index, 0, &info) != 0)
		{
			cemuLog_log(LogType::Force, "Zip archive {}: entry {} is unreadable", m_zipPath, index);
			return false;
		}
		if ((info.valid & ZIP_STAT_NAME) == 0 || info.name == nullptr)
			return false;
		const std::string_view name = info.name;
		if (!IsSafeFileName(name))
			return false;
		entry = {};
		entry.fullPath = name;
		const size_t slash = name.find_last_of('/');
		entry.filename = slash == std::string_view::npos ? name : name.substr(slash + 1);
		return true;
	}

	bool ExtractIntoMemoryByIndex(size_t index, std::vector<uint8>& data)
	{
		zip_file_t* file = zip_fopen_index(m_zip, (zip_uint64_t)index, 0);
		if (!file)
		{
			cemuLog_log(LogType::Force, "Zip archive {}: cannot open entry {}", m_zipPath, index);
			cemu_assert_suspicious();
			return false;
		}
		data.clear();
		uint8 chunk[4096];
		bool ok = true;
		for (;;)
		{
			const zip_int64_t got = zip_fread(file, chunk, sizeof(chunk));
			if (got == 0)
				break;
			if (got < 0)
			{
				cemuLog_log(LogType::Force, "Zip archive {}: read error in entry {}", m_zipPath, index);
				cemu_assert_suspicious();
				ok = false;
				break;
			}
			data.insert(data.end(), chunk, chunk + got);
		}
		zip_fclose(file);
		return ok;
	}

private:
	static bool IsSafeFileName(std::string_view name)
	{
		if (name.empty())
			return false;
		if (name.find('\\') != std::string_view::npos) // Windows separator, also catches UNC paths
			return false;
		if (name.front() == '/') // absolute
			return false;
		if (name.size() >= 2 && name[1] == ':' && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
			return false; // drive letter
		if (name.back() == '/') // directory entry
			return false;
		size_t start = 0;
		while (true)
		{
			size_t end = name.find('/', start);
			if (end == std::string_view::npos)
				end = name.size();
			const std::string_view part = name.substr(start, end - start);
			if (part == "." || part == "..")
				return false;
			if (end == name.size())
				break;
			start = end + 1;
		}
		return true;
	}

	zip_t* m_zip{};
	std::string m_zipPath;
};
