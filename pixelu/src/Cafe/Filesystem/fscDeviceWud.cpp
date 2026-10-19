// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/FST/FST.h"

// Read-only access to the files of an FSTVolume (disc images and NUS content).
namespace
{
	class VolumeFile : public FSCVirtualFile
	{
	  public:
		VolumeFile(FSTVolume* volume, const FSTFileHandle& file) : m_volume(volume), m_type(FSC_TYPE_FILE), m_file(file) {}
		VolumeFile(FSTVolume* volume, const FSTDirectoryIterator& listing) : m_volume(volume), m_type(FSC_TYPE_DIRECTORY), m_listing(listing) {}

		sint32 fscGetType() override
		{
			return m_type;
		}

		uint64 fscQueryValueU64(uint32 id) override
		{
			if (m_type == FSC_TYPE_FILE && id == FSC_QUERY_SIZE)
				return m_volume->GetFileSize(m_file);
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
			cemu_assert_error(); // volumes are read-only
			return 0;
		}

		uint32 fscReadData(void* buffer, uint32 size) override
		{
			if (m_type != FSC_TYPE_FILE)
				return 0;
			cemu_assert(size < 0x80000000u);
			const uint32 fileSize = m_volume->GetFileSize(m_file);
			const uint32 toRead = m_position < fileSize ? std::min(fileSize - m_position, size) : 0;
			const uint32 bytesRead = m_volume->ReadFile(m_file, m_position, toRead, buffer);
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
			FSTFileHandle entry;
			if (!m_volume->Next(m_listing, entry))
				return false;
			dirEntry->isDirectory = m_volume->IsDirectory(entry);
			dirEntry->isFile = !dirEntry->isDirectory;
			dirEntry->fileSize = dirEntry->isFile ? m_volume->GetFileSize(entry) : 0;
			const std::string_view name = m_volume->GetName(entry);
			const size_t len = std::min(name.size(), sizeof(dirEntry->path) - 1);
			std::memset(dirEntry->path, 0, sizeof(dirEntry->path));
			std::memcpy(dirEntry->path, name.data(), len);
			return true;
		}

	  private:
		FSTVolume* m_volume;
		sint32 m_type;
		FSTFileHandle m_file{};
		uint32 m_position{0};
		FSTDirectoryIterator m_listing{};
	};

	// entries with the link flag are not opened (they are listed though)
	class VolumeDevice : public fscDeviceC
	{
	  public:
		FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
		{
			FSTVolume* volume = static_cast<FSTVolume*>(ctx);
			cemu_assert_debug(!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION));
			if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
			{
				FSTFileHandle file;
				if (volume->OpenFile(path, file, true) && !volume->HasLinkFlag(file))
				{
					*fscStatus = FSC_STATUS_OK;
					return new VolumeFile(volume, file);
				}
			}
			if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
			{
				FSTDirectoryIterator listing;
				if (volume->OpenDirectoryIterator(path, listing) && !volume->HasLinkFlag(listing.GetDirHandle()))
				{
					*fscStatus = FSC_STATUS_OK;
					return new VolumeFile(volume, listing);
				}
			}
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return nullptr;
		}
	};

	VolumeDevice& GetVolumeDevice()
	{
		static VolumeDevice s_device;
		return s_device;
	}
}

bool FSCDeviceWUD_Mount(std::string_view mountPath, std::string_view destinationBaseDir, FSTVolume* mountedVolume, sint32 priority)
{
	return fsc_mount(mountPath, destinationBaseDir, &GetVolumeDevice(), mountedVolume, priority) == FSC_STATUS_OK;
}
