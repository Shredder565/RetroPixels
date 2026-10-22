// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "config/ActiveSettings.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"
#include "Common/FileStream.h"

FSCVirtualFile_Host::FSCVirtualFile_Host(sint32 type) : m_type(type) {}

FSCVirtualFile_Host::~FSCVirtualFile_Host() = default;

sint32 FSCVirtualFile_Host::fscGetType()
{
	return m_type;
}

uint32 FSCVirtualFile_Host::fscDeviceHostFSFile_getFileSize()
{
	if (m_type != FSC_TYPE_FILE)
		return 0;
	if (m_size > 0xFFFFFFFFull)
		cemu_assert_suspicious(); // the Wii U file API cannot describe files of 4GiB or more
	return (uint32)m_size;
}

uint64 FSCVirtualFile_Host::fscQueryValueU64(uint32 id)
{
	switch (id)
	{
	case FSC_QUERY_SIZE:
		return fscDeviceHostFSFile_getFileSize();
	case FSC_QUERY_WRITEABLE:
		if (m_type == FSC_TYPE_FILE)
			return m_writable ? 1 : 0;
		break;
	default:
		break;
	}
	cemu_assert_unimplemented();
	return 0;
}

uint32 FSCVirtualFile_Host::fscWriteData(void* buffer, uint32 size)
{
	if (m_type != FSC_TYPE_FILE)
		return 0;
	if (size >= 0x80000000u)
	{
		cemu_assert_suspicious();
		return 0;
	}
	const sint32 written = m_stream->writeData(buffer, (sint32)size);
	m_position += (uint64)(sint64)written;
	m_size = std::max(m_size, m_position);
	return (uint32)written;
}

uint32 FSCVirtualFile_Host::fscReadData(void* buffer, uint32 size)
{
	if (m_type != FSC_TYPE_FILE)
		return 0;
	if (size >= 0x80000000u)
	{
		cemu_assert_suspicious();
		return 0;
	}
	const uint64 available = m_position < m_size ? m_size - m_position : 0;
	const uint32 toRead = (uint32)std::min<uint64>({available, (uint64)size, 0x7FFFFFFFull});
	const uint32 bytesRead = m_stream->readData(buffer, toRead);
	m_position += bytesRead;
	return bytesRead;
}

void FSCVirtualFile_Host::fscSetSeek(uint64 seek)
{
	if (m_type != FSC_TYPE_FILE)
		return;
	cemu_assert_debug(seek <= m_size);
	m_position = seek;
	m_stream->SetPosition(seek);
}

uint64 FSCVirtualFile_Host::fscGetSeek()
{
	return m_type == FSC_TYPE_FILE ? m_position : 0;
}

void FSCVirtualFile_Host::fscSetFileLength(uint64 endOffset)
{
	if (m_type != FSC_TYPE_FILE)
		return;
	m_stream->SetPosition(endOffset);
	const bool resized = m_stream->SetEndOfFile();
	m_size = endOffset;
	m_position = std::min(m_position, endOffset);
	m_stream->SetPosition(m_position);
	if (!resized)
		cemuLog_log(LogType::Force, "HostFS: could not change the file length to 0x{:x}", endOffset);
}

bool FSCVirtualFile_Host::fscDirNext(FSCDirEntry* dirEntry)
{
	if (m_type != FSC_TYPE_DIRECTORY)
		return false;
	std::error_code ec;
	if (!m_listing)
	{
		m_listing.emplace(m_folder, ec);
		if (ec)
		{
			cemuLog_log(LogType::Force, "HostFS: cannot list {}", _pathToUtf8(m_folder));
			m_listing.emplace(); // end iterator, the folder reads as empty
			return false;
		}
	}
	if (*m_listing == fs::directory_iterator())
		return false;
	const fs::directory_entry& entry = **m_listing;
	const std::string name = entry.path().filename().generic_string();
	const size_t len = std::min(name.size(), sizeof(dirEntry->path) - 1);
	std::memcpy(dirEntry->path, name.data(), len);
	std::memset(dirEntry->path + len, 0, sizeof(dirEntry->path) - len);
	dirEntry->isDirectory = entry.is_directory(ec);
	dirEntry->isFile = !dirEntry->isDirectory;
	dirEntry->fileSize = 0;
	if (dirEntry->isFile)
	{
		const uintmax_t fileSize = entry.file_size(ec);
		dirEntry->fileSize = ec ? 0 : (uint32)fileSize;
	}
	m_listing->increment(ec);
	if (ec)
		m_listing.emplace();
	return true;
}

FSCVirtualFile* FSCVirtualFile_Host::OpenFile(const fs::path& path, FSC_ACCESS_FLAG accessFlags, sint32& fscStatus)
{
	cemu_assert_debug(HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) || HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR));
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
	{
		const bool write = HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION);
		FileStream* stream = nullptr;
		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE) && !HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALLOW_CREATE))
			stream = FileStream::createFile2(path);
		else
		{
			stream = FileStream::openFile2(path, write);
			if (!stream && HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALLOW_CREATE))
				stream = FileStream::createFile2(path);
		}
		const bool wantedCreate = HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALLOW_CREATE) || HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE);
		if (!stream && wantedCreate)
			cemuLog_log(LogType::Force, "HostFS: cannot create {}", _pathToUtf8(path));
		if (stream)
		{
			FSCVirtualFile_Host* file = new FSCVirtualFile_Host(FSC_TYPE_FILE);
			file->m_stream.reset(stream);
			file->m_writable = write;
			file->m_size = stream->GetSize();
			fscStatus = FSC_STATUS_OK;
			return file;
		}
	}
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
	{
		std::error_code ec;
		if (fs::is_directory(path, ec))
		{
			FSCVirtualFile_Host* folder = new FSCVirtualFile_Host(FSC_TYPE_DIRECTORY);
			folder->m_folder = path;
			fscStatus = FSC_STATUS_OK;
			return folder;
		}
	}
	fscStatus = FSC_STATUS_FILE_NOT_FOUND;
	return nullptr;
}

namespace
{
	class HostFolderDevice : public fscDeviceC
	{
	  public:
		FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
		{
			return FSCVirtualFile_Host::OpenFile(_utf8ToPath(path), accessFlags, *fscStatus);
		}

		// an existing entry is reported as ALREADY_EXISTS; otherwise this succeeds
		// even if the folder could not be created (only logged)
		bool fscDeviceCreateDir(std::string_view path, void* ctx, sint32* fscStatus) override
		{
			const fs::path folder = _utf8ToPath(path);
			std::error_code ec;
			if (fs::exists(folder, ec))
			{
				if (!fs::is_directory(folder, ec))
					cemuLog_log(LogType::Force, "HostFS: {} exists and is not a folder", path);
				*fscStatus = FSC_STATUS_ALREADY_EXISTS;
				return false;
			}
			if (!fs::create_directories(folder, ec))
				cemuLog_log(LogType::Force, "HostFS: cannot create folder {}", path);
			*fscStatus = FSC_STATUS_OK;
			return true;
		}

		// a failed removal is reported through the status only
		bool fscDeviceRemoveFileOrDir(std::string_view path, void* ctx, sint32* fscStatus) override
		{
			const fs::path target = _utf8ToPath(path);
			std::error_code ec;
			if (!fs::exists(target, ec))
			{
				*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
				return false;
			}
			*fscStatus = FSC_STATUS_OK;
			if (!fs::remove(target, ec))
			{
				cemuLog_log(LogType::Force, "HostFS: cannot remove {}", path);
				*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			}
			return true;
		}

		bool fscDeviceRename(std::string_view srcPath, std::string_view dstPath, void* ctx, sint32* fscStatus) override
		{
			const fs::path source = _utf8ToPath(srcPath);
			std::error_code ec;
			if (!fs::exists(source, ec))
			{
				*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
				return false;
			}
			*fscStatus = FSC_STATUS_OK;
			fs::rename(source, _utf8ToPath(dstPath), ec);
			return true;
		}
	};

	HostFolderDevice& GetHostFolderDevice()
	{
		static HostFolderDevice s_device;
		return s_device;
	}
}

bool FSCDeviceHostFS_Mount(std::string_view mountPath, std::string_view hostTargetPath, sint32 priority)
{
	return fsc_mount(mountPath, hostTargetPath, &GetHostFolderDevice(), nullptr, priority) == FSC_STATUS_OK;
}
