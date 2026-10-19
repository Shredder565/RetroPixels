// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Filesystem/WUHB/WUHBReader.h"
#include "Cafe/Filesystem/fsc.h"

// Read-only access to the RomFS of a .wuhb bundle.
namespace
{
	void CopyName(FSCDirEntry* dirEntry, const std::string& name)
	{
		const size_t len = std::min(name.size(), sizeof(dirEntry->path) - 1);
		std::memset(dirEntry->path, 0, sizeof(dirEntry->path));
		std::memcpy(dirEntry->path, name.data(), len);
	}

	class BundleFile : public FSCVirtualFile
	{
	  public:
		BundleFile(WUHBReader* reader, uint32 entryOffset, sint32 type) : m_reader(reader), m_entry(entryOffset), m_type(type)
		{
			cemu_assert(entryOffset != ROMFS_ENTRY_EMPTY);
			if (type == FSC_TYPE_DIRECTORY)
			{
				const romfs_direntry_t folder = reader->GetDirEntry(entryOffset);
				m_nextFolder = folder.dirListHead;
				m_nextFile = folder.fileListHead;
			}
		}

		sint32 fscGetType() override
		{
			return m_type;
		}

		uint64 fscQueryValueU64(uint32 id) override
		{
			if (m_type == FSC_TYPE_FILE && id == FSC_QUERY_SIZE)
				return m_reader->GetFileSize(m_entry);
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
			cemu_assert_error(); // bundles are read-only
			return 0;
		}

		uint32 fscReadData(void* buffer, uint32 size) override
		{
			if (m_type != FSC_TYPE_FILE)
				return 0;
			const uint64 bytesRead = m_reader->ReadFromFile(m_entry, m_position, size, buffer);
			m_position += bytesRead;
			return (uint32)bytesRead;
		}

		void fscSetSeek(uint64 seek) override
		{
			m_position = seek;
		}

		uint64 fscGetSeek() override
		{
			return m_type == FSC_TYPE_FILE ? m_position : 0;
		}

		void fscSetFileLength(uint64 endOffset) override
		{
			cemu_assert_error();
		}

		// Each call advances the folder list by one entry and, if that entry has
		// no name, the file list by one entry. Folders are listed before files.
		bool fscDirNext(FSCDirEntry* dirEntry) override
		{
			if (m_nextFolder != ROMFS_ENTRY_EMPTY)
			{
				const romfs_direntry_t folder = m_reader->GetDirEntry(m_nextFolder);
				m_nextFolder = folder.listNext;
				if (folder.name_size != 0)
				{
					dirEntry->isDirectory = true;
					dirEntry->isFile = false;
					dirEntry->fileSize = 0;
					CopyName(dirEntry, folder.name);
					return true;
				}
			}
			if (m_nextFile != ROMFS_ENTRY_EMPTY)
			{
				const romfs_fentry_t file = m_reader->GetFileEntry(m_nextFile);
				m_nextFile = file.listNext;
				if (file.name_size != 0)
				{
					dirEntry->isDirectory = false;
					dirEntry->isFile = true;
					dirEntry->fileSize = (uint32)file.size;
					CopyName(dirEntry, file.name);
					return true;
				}
			}
			return false;
		}

	  private:
		WUHBReader* m_reader;
		uint32 m_entry;
		sint32 m_type;
		uint64 m_position{0};
		uint32 m_nextFolder{ROMFS_ENTRY_EMPTY};
		uint32 m_nextFile{ROMFS_ENTRY_EMPTY};
	};

	// with both flags set a folder of that name is preferred over a file
	class BundleDevice : public fscDeviceC
	{
	  public:
		FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
		{
			WUHBReader* reader = static_cast<WUHBReader*>(ctx);
			cemu_assert_debug(!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION));
			if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
			{
				const uint32 folder = reader->Lookup(path, false);
				if (folder != ROMFS_ENTRY_EMPTY)
				{
					*fscStatus = FSC_STATUS_OK;
					return new BundleFile(reader, folder, FSC_TYPE_DIRECTORY);
				}
			}
			if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
			{
				const uint32 file = reader->Lookup(path, true);
				if (file != ROMFS_ENTRY_EMPTY)
				{
					*fscStatus = FSC_STATUS_OK;
					return new BundleFile(reader, file, FSC_TYPE_FILE);
				}
			}
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return nullptr;
		}
	};

	BundleDevice& GetBundleDevice()
	{
		static BundleDevice s_device;
		return s_device;
	}
}

bool FSCDeviceWUHB_Mount(std::string_view mountPath, std::string_view destinationBaseDir, WUHBReader* wuhbReader, sint32 priority)
{
	return fsc_mount(mountPath, destinationBaseDir, &GetBundleDevice(), wuhbReader, priority) == FSC_STATUS_OK;
}
