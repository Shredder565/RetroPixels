// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/Filesystem/fsc.h"
#include <zarchive/zarchivereader.h>

// Read-only access to a .wua (ZArchive) file.
namespace
{
	class ArchiveFile : public FSCVirtualFile
	{
	  public:
		ArchiveFile(ZArchiveReader* archive, ZArchiveNodeHandle node, sint32 type) : m_archive(archive), m_node(node), m_type(type) {}

		sint32 fscGetType() override
		{
			return m_type;
		}

		uint64 fscQueryValueU64(uint32 id) override
		{
			if (m_type == FSC_TYPE_FILE && id == FSC_QUERY_SIZE)
				return FileSize();
			if (m_type == FSC_TYPE_FILE && id == FSC_QUERY_WRITEABLE)
				return 0;
			if (m_type == FSC_TYPE_FILE)
				cemu_assert_error();
			else
				cemu_assert_unimplemented();
			return 0;
		}

		uint32 fscWriteData(void* buffer, uint32 size) override
		{
			cemu_assert_error(); // archives are read-only
			return 0;
		}

		uint32 fscReadData(void* buffer, uint32 size) override
		{
			if (m_type != FSC_TYPE_FILE)
				return 0;
			cemu_assert(size < 0x80000000u);
			const uint32 fileSize = FileSize();
			const uint32 toRead = m_position < fileSize ? std::min(fileSize - m_position, size) : 0;
			const uint32 bytesRead = (uint32)m_archive->ReadFromFile(m_node, m_position, toRead, buffer);
			m_position += bytesRead;
			return bytesRead;
		}

		void fscSetSeek(uint64 seek) override
		{
			if (m_type != FSC_TYPE_FILE)
				return;
			cemu_assert_debug(seek <= 0xFFFFFFFFull);
			m_position = (uint32)seek;
		}

		uint64 fscGetSeek() override
		{
			return m_type == FSC_TYPE_FILE ? m_position : 0;
		}

		bool fscDirNext(FSCDirEntry* dirEntry) override
		{
			if (m_type != FSC_TYPE_DIRECTORY)
				return false;
			ZArchiveReader::DirEntry entry;
			if (!m_archive->GetDirEntry(m_node, m_nextEntry, entry))
				return false;
			m_nextEntry++;
			if (entry.isDirectory)
			{
				dirEntry->isDirectory = true;
				dirEntry->isFile = false;
				dirEntry->fileSize = 0;
			}
			else if (entry.isFile)
			{
				dirEntry->isDirectory = false;
				dirEntry->isFile = true;
				dirEntry->fileSize = (uint32)entry.size;
			}
			else
				cemu_assert_suspicious();
			const size_t len = std::min(entry.name.size(), sizeof(dirEntry->path) - 1);
			std::memset(dirEntry->path, 0, sizeof(dirEntry->path));
			std::memcpy(dirEntry->path, entry.name.data(), len);
			return true;
		}

	  private:
		uint32 FileSize()
		{
			return (uint32)m_archive->GetFileSize(m_node);
		}

		ZArchiveReader* m_archive;
		ZArchiveNodeHandle m_node;
		sint32 m_type;
		uint32 m_position{0};  // files
		uint32 m_nextEntry{0}; // folders
	};

	// a path that exists with the other type than requested is "not found"
	class ArchiveDevice : public fscDeviceC
	{
	  public:
		FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
		{
			ZArchiveReader* archive = static_cast<ZArchiveReader*>(ctx);
			cemu_assert_debug(!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION));
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			const ZArchiveNodeHandle node = archive->LookUp(path, true, true);
			if (node == ZARCHIVE_INVALID_NODE)
				return nullptr;
			sint32 type;
			if (archive->IsFile(node))
			{
				if (!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
					return nullptr;
				type = FSC_TYPE_FILE;
			}
			else if (archive->IsDirectory(node))
			{
				if (!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
					return nullptr;
				type = FSC_TYPE_DIRECTORY;
			}
			else
			{
				cemu_assert_suspicious();
				return nullptr;
			}
			*fscStatus = FSC_STATUS_OK;
			return new ArchiveFile(archive, node, type);
		}
	};

	ArchiveDevice& GetArchiveDevice()
	{
		static ArchiveDevice s_device;
		return s_device;
	}
}

bool FSCDeviceWUA_Mount(std::string_view mountPath, std::string_view destinationBaseDir, ZArchiveReader* archive, sint32 priority)
{
	return fsc_mount(mountPath, destinationBaseDir, &GetArchiveDevice(), archive, priority) == FSC_STATUS_OK;
}
