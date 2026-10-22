// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// FSC: the emulator's virtual file system. Devices (host folders, disc images,
// .wua/.wuhb archives, graphic pack redirections) are mounted at virtual
// paths with a priority; lookups try the highest priority first and
// directory listings merge every mounted layer.

struct FSCVirtualFile;

// node types
#define FSC_TYPE_INVALID				(0)
#define FSC_TYPE_FILE					(1)
#define FSC_TYPE_DIRECTORY				(2)

// ids for FSCVirtualFile::fscQueryValueU64
#define FSC_QUERY_SIZE					(1) // file size (0 for directories)
#define FSC_QUERY_WRITEABLE				(2) // non-zero for writable files

enum class FSC_ACCESS_FLAG : uint8
{
	NONE = 0,

	READ_PERMISSION = (1 << 0),
	WRITE_PERMISSION = (1 << 1),

	// how a file is opened (files only)
	FILE_ALLOW_CREATE = (1 << 2),  // create the file when it does not exist
	FILE_ALWAYS_CREATE = (1 << 3), // always start with an empty file

	// what may be opened; at least one of these must be set
	OPEN_DIR = (1 << 4),
	OPEN_FILE = (1 << 5),

	IS_APPEND = (1 << 6) // writes go to the end of the file
};
DEFINE_ENUM_FLAG_OPERATORS(FSC_ACCESS_FLAG);

// status codes; there is no separate "not a file"/"not a directory" code,
// callers open with one type flag at a time to tell them apart
#define FSC_STATUS_UNDEFINED			(-1)
#define FSC_STATUS_OK					(0)
#define FSC_STATUS_INVALID_PATH			(1)
#define FSC_STATUS_FILE_NOT_FOUND		(2)
#define FSC_STATUS_ALREADY_EXISTS		(3)

#define FSC_MAX_DIR_NAME_LENGTH			(256)
#define FSC_MAX_DEVICE_PATH_LENGTH		((std::max)(260,FSA_PATH_SIZE_MAX))

struct FSCDirEntry
{
	char path[FSC_MAX_DIR_NAME_LENGTH]; // entry name (not a full path)
	bool isDirectory;
	bool isFile;
	uint32 fileSize;

	std::string_view GetPath() { return {path, strnlen(path, sizeof(path))}; }
};

// result of a base-class operation that the device or file does not implement
template<typename T>
T fscUnsupported(T result)
{
	cemu_assert_unimplemented();
	return result;
}

inline void fscUnsupported()
{
	cemu_assert_unimplemented();
}

// A mountable device. Paths passed in are relative to the device's target
// path; ctx is the pointer given to fsc_mount.
class fscDeviceC
{
  public:
	virtual FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) { return fscUnsupported<FSCVirtualFile*>(nullptr); }
	virtual bool fscDeviceCreateDir(std::string_view path, void* ctx, sint32* fscStatus) { return fscUnsupported(false); }
	virtual bool fscDeviceRemoveFileOrDir(std::string_view path, void* ctx, sint32* fscStatus) { return fscUnsupported(false); }
	virtual bool fscDeviceRename(std::string_view srcPath, std::string_view dstPath, void* ctx, sint32* fscStatus) { return fscUnsupported(false); }
};

// An open file or directory. Devices override what they support.
struct FSCVirtualFile
{
	// buffered directory listing, for implementations that collect entries up front
	struct FSCDirIteratorState
	{
		sint32 index;
		std::vector<FSCDirEntry> dirEntries;
	};

	FSCVirtualFile() {}

	virtual ~FSCVirtualFile() { delete dirIterator; }

	virtual sint32 fscGetType() { return fscUnsupported<sint32>(0); }
	virtual uint64 fscQueryValueU64(uint32 id) { return fscUnsupported<uint64>(0); }
	virtual uint32 fscWriteData(void* buffer, uint32 size) { return fscUnsupported<uint32>(0); }
	virtual uint32 fscReadData(void* buffer, uint32 size) { return fscUnsupported<uint32>(0); }
	virtual void fscSetSeek(uint64 seek) { fscUnsupported(); }
	virtual uint64 fscGetSeek() { return fscUnsupported<uint64>(0); }
	virtual void fscSetFileLength(uint64 endOffset) { fscUnsupported(); }
	virtual bool fscDirNext(FSCDirEntry* dirEntry) { return fscUnsupported(false); }
	virtual bool fscRewindDir() { return fscUnsupported(false); }

	FSCDirIteratorState* dirIterator{}; // owned, freed with the file

	bool m_isAppend{false}; // set by fsc_open from FSC_ACCESS_FLAG::IS_APPEND
};

// mount priorities, higher ones are searched first
#define FSC_PRIORITY_BASE				(0)
#define FSC_PRIORITY_AOC				(1)
#define FSC_PRIORITY_PATCH				(2)
#define FSC_PRIORITY_REDIRECT			(3)
#define FSC_PRIORITY_MAX				(3)

#define FSC_PRIORITY_COUNT				(4)

void fsc_init();
sint32 fsc_mount(std::string_view mountPath, std::string_view targetPath, fscDeviceC* fscDevice, void* ctx, sint32 priority=0);
bool fsc_unmount(std::string_view mountPath, sint32 priority);
void fsc_unmountAll();

FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32 maxPriority=FSC_PRIORITY_MAX);
FSCVirtualFile* fsc_openDirIterator(const char* path, sint32* fscStatus);
bool fsc_createDir(const char* path, sint32* fscStatus);
bool fsc_rename(const char* srcPath, const char* dstPath, sint32* fscStatus);
bool fsc_remove(const char* path, sint32* fscStatus);
bool fsc_nextDir(FSCVirtualFile* fscFile, FSCDirEntry* dirEntry);
void fsc_close(FSCVirtualFile* fscFile);
uint32 fsc_getFileSize(FSCVirtualFile* fscFile);
uint32 fsc_getFileSeek(FSCVirtualFile* fscFile);
void fsc_setFileSeek(FSCVirtualFile* fscFile, uint32 newSeek);
void fsc_setFileLength(FSCVirtualFile* fscFile, uint32 newEndOffset);
bool fsc_isDirectory(FSCVirtualFile* fscFile);
bool fsc_isFile(FSCVirtualFile* fscFile);
bool fsc_isWritable(FSCVirtualFile* fscFile);
uint32 fsc_readFile(FSCVirtualFile* fscFile, void* buffer, uint32 size);
uint32 fsc_writeFile(FSCVirtualFile* fscFile, void* buffer, uint32 size);

// whole-file helpers
uint8* fsc_extractFile(const char* path, uint32* fileSize, sint32 maxPriority = FSC_PRIORITY_MAX); // malloc'd buffer
std::optional<std::vector<uint8>> fsc_extractFile(const char* path, sint32 maxPriority = FSC_PRIORITY_MAX);
bool fsc_doesFileExist(const char* path, sint32 maxPriority = FSC_PRIORITY_MAX);
bool fsc_doesDirectoryExist(const char* path, sint32 maxPriority = FSC_PRIORITY_MAX);

// disc image / NUS content (FSTVolume)
bool FSCDeviceWUD_Mount(std::string_view mountPath, std::string_view destinationBaseDir, class FSTVolume* mountedVolume, sint32 priority);

// .wua archive
bool FSCDeviceWUA_Mount(std::string_view mountPath, std::string_view destinationBaseDir, class ZArchiveReader* archive, sint32 priority);

// .wuhb bundle
bool FSCDeviceWUHB_Mount(std::string_view mountPath, std::string_view destinationBaseDir, class WUHBReader* wuhbReader, sint32 priority);

// host folder
bool FSCDeviceHostFS_Mount(std::string_view mountPath, std::string_view hostTargetPath, sint32 priority);

// single-file redirections (graphic pack content replacements)
void fscDeviceRedirect_map();
void fscDeviceRedirect_add(std::string_view virtualSourcePath, size_t fileSize, const fs::path& targetFilePath, sint32 priority);
