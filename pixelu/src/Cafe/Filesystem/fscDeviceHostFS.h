// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/Filesystem/fsc.h"

class FileStream;

// A file or folder on the host file system. Also used by the redirect device.
class FSCVirtualFile_Host : public FSCVirtualFile
{
  public:
	// opens 'path' according to the access flags; fscStatus is set to OK or FILE_NOT_FOUND
	static FSCVirtualFile* OpenFile(const fs::path& path, FSC_ACCESS_FLAG accessFlags, sint32& fscStatus);
	~FSCVirtualFile_Host() override;

	sint32 fscGetType() override;

	uint32 fscDeviceHostFSFile_getFileSize(); // 0 for folders

	uint64 fscQueryValueU64(uint32 id) override;
	uint32 fscWriteData(void* buffer, uint32 size) override;
	uint32 fscReadData(void* buffer, uint32 size) override;
	void fscSetSeek(uint64 seek) override;
	uint64 fscGetSeek() override;
	void fscSetFileLength(uint64 endOffset) override;
	bool fscDirNext(FSCDirEntry* dirEntry) override;

  private:
	explicit FSCVirtualFile_Host(sint32 type);

	sint32 m_type; // FSC_TYPE_FILE or FSC_TYPE_DIRECTORY
	// file
	std::unique_ptr<FileStream> m_stream;
	uint64 m_position{0};
	uint64 m_size{0};
	bool m_writable{false};
	// folder, listed lazily
	fs::path m_folder;
	std::optional<fs::directory_iterator> m_listing;
};
