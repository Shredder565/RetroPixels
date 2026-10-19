// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/FST/fstUtil.h"

namespace
{
	// One name of a mount tree. A node with a device is a mount point; nodes
	// without one only exist to reach deeper mount points (and show up as
	// folders in directory listings).
	struct MountNode
	{
		std::string name;
		MountNode* parent{nullptr};
		std::vector<std::unique_ptr<MountNode>> children;
		fscDeviceC* device{nullptr};
		void* ctx{nullptr};
		std::string target; // device path of this mount point, ends with a separator unless empty

		MountNode* FindChild(std::string_view childName) const
		{
			for (const auto& child : children)
			{
				if (FSCPath::MatchNodeName(child->name, childName))
					return child.get();
			}
			return nullptr;
		}
	};

	// one tree per priority
	std::array<std::unique_ptr<MountNode>, FSC_PRIORITY_COUNT> s_trees;
	std::recursive_mutex s_lock;

	MountNode& Tree(sint32 priority)
	{
		cemu_assert(priority >= 0 && priority < FSC_PRIORITY_COUNT);
		auto& root = s_trees[priority];
		if (!root)
			root = std::make_unique<MountNode>();
		return *root;
	}

	// walks the tree as far as the path matches; returns the deepest node and how many names it covers
	std::pair<MountNode*, size_t> WalkTree(const FSCPath& path, sint32 priority)
	{
		MountNode* node = &Tree(priority);
		size_t depth = 0;
		while (depth < path.GetNodeCount())
		{
			MountNode* child = node->FindChild(path.GetNodeName(depth));
			if (!child)
				break;
			node = child;
			depth++;
		}
		return {node, depth};
	}

	// the node for exactly this path, if the tree has one
	MountNode* FindExact(std::string_view path, sint32 priority)
	{
		const FSCPath parsed(path);
		auto [node, depth] = WalkTree(parsed, priority);
		return depth == parsed.GetNodeCount() ? node : nullptr;
	}

	// Finds the mount point responsible for a path at one priority: the deepest
	// mounted node along the path. The rest of the path is appended to that
	// mount's target path.
	bool ResolveDevice(std::string_view path, sint32 priority, std::string& devicePathOut, fscDeviceC*& deviceOut, void*& ctxOut)
	{
		std::lock_guard lock(s_lock);
		const FSCPath parsed(path);
		auto [node, depth] = WalkTree(parsed, priority);
		while (node && !node->device)
		{
			node = node->parent;
			depth--;
		}
		if (!node)
			return false;
		devicePathOut = node->target;
		for (size_t i = depth; i < parsed.GetNodeCount(); i++)
		{
			if (i != depth)
				devicePathOut.push_back('/');
			devicePathOut.append(parsed.GetNodeName(i));
		}
		deviceOut = node->device;
		ctxOut = node->ctx;
		return true;
	}

	// Directory handle returned by fsc_open: merges the listings of the same
	// folder from every priority, plus the names of mount points below it.
	class MergedDirectory : public FSCVirtualFile
	{
	  public:
		MergedDirectory(std::string_view virtualPath, std::vector<FSCVirtualFile*> layers)
			: m_virtualPath(virtualPath), m_layers(std::move(layers))
		{
		}

		~MergedDirectory() override
		{
			for (FSCVirtualFile* layer : m_layers)
				delete layer;
		}

		sint32 fscGetType() override
		{
			return FSC_TYPE_DIRECTORY;
		}

		bool fscDirNext(FSCDirEntry* dirEntry) override
		{
			if (!m_collected)
				Collect();
			if (m_position >= m_entries.size())
				return false;
			*dirEntry = m_entries[m_position++];
			return true;
		}

		bool fscRewindDir() override
		{
			m_position = 0;
			return true;
		}

	  private:
		void Collect()
		{
			std::lock_guard lock(s_lock);
			m_collected = true;
			FSCDirEntry entry;
			for (FSCVirtualFile* layer : m_layers)
			{
				while (layer->fscDirNext(&entry))
					AddUnique(entry);
			}
			for (sint32 priority = FSC_PRIORITY_COUNT - 1; priority >= 0; priority--)
			{
				MountNode* node = FindExact(m_virtualPath, priority);
				if (!node)
					continue;
				for (const auto& child : node->children)
				{
					FSCDirEntry folder{};
					folder.isDirectory = true;
					const size_t len = std::min(child->name.size(), sizeof(folder.path) - 1);
					std::memcpy(folder.path, child->name.data(), len);
					AddUnique(folder);
				}
			}
		}

		// the first entry of a name wins (higher priorities are listed first)
		void AddUnique(const FSCDirEntry& entry)
		{
			const std::string_view name(entry.path, strnlen(entry.path, sizeof(entry.path)));
			for (const FSCDirEntry& existing : m_entries)
			{
				if (FSCPath::MatchNodeName(std::string_view(existing.path, strnlen(existing.path, sizeof(existing.path))), name))
					return;
			}
			m_entries.push_back(entry);
		}

		std::string m_virtualPath;
		std::vector<FSCVirtualFile*> m_layers; // owned
		bool m_collected{false};
		std::vector<FSCDirEntry> m_entries;
		size_t m_position{0};
	};

	template<typename TFunc>
	auto WithLock(TFunc&& fn)
	{
		std::lock_guard lock(s_lock);
		return fn();
	}
}

void fsc_init()
{
	fsc_unmountAll();
}

// Mounting the same path twice at the same priority fails, as does mounting
// at a path that already exists in the tree only because of a deeper mount.
// The empty path (root) can always be (re)assigned.
sint32 fsc_mount(std::string_view mountPath, std::string_view targetPath, fscDeviceC* fscDevice, void* ctx, sint32 priority)
{
	cemu_assert(fscDevice);
	std::string target(targetPath);
	if (!target.empty() && target.back() != '/' && target.back() != '\\')
		target.push_back('/');
	const FSCPath parsed(mountPath);

	std::lock_guard lock(s_lock);
	auto [node, depth] = WalkTree(parsed, priority);
	if (depth == parsed.GetNodeCount() && depth != 0)
	{
		cemuLog_log(LogType::Force, "FSC: cannot mount {}, the path is already in use", mountPath);
		return FSC_STATUS_INVALID_PATH;
	}
	for (; depth < parsed.GetNodeCount(); depth++)
	{
		auto child = std::make_unique<MountNode>();
		child->name = parsed.GetNodeName(depth);
		child->parent = node;
		MountNode* next = child.get();
		node->children.push_back(std::move(child));
		node = next;
	}
	node->device = fscDevice;
	node->ctx = ctx;
	node->target = std::move(target);
	return FSC_STATUS_OK;
}

bool fsc_unmount(std::string_view mountPath, sint32 priority)
{
	std::lock_guard lock(s_lock);
	MountNode* node = FindExact(mountPath, priority);
	if (!node || !node->device)
		return false;
	node->device = nullptr;
	node->ctx = nullptr;
	node->target.clear();
	// drop folders that no longer lead to any mount point
	while (node->parent && node->children.empty() && !node->device)
	{
		MountNode* parent = node->parent;
		std::erase_if(parent->children, [node](const std::unique_ptr<MountNode>& c) { return c.get() == node; });
		node = parent;
	}
	return true;
}

void fsc_unmountAll()
{
	std::lock_guard lock(s_lock);
	for (auto& root : s_trees)
		root = std::make_unique<MountNode>();
}

// Files: the first priority (from maxPriority down) whose device has the file
// wins. Directories: every priority's copy is merged; a folder also exists if
// mount points lie below it.
FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32 maxPriority)
{
	cemu_assert_debug(HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) || HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR));
	*fscStatus = FSC_STATUS_UNDEFINED;
	std::lock_guard lock(s_lock);
	std::vector<FSCVirtualFile*> folders;
	for (sint32 priority = maxPriority; priority >= 0; priority--)
	{
		std::string devicePath;
		fscDeviceC* device;
		void* ctx;
		if (!ResolveDevice(path, priority, devicePath, device, ctx))
			continue;
		FSCVirtualFile* opened = device->fscDeviceOpenByPath(devicePath, accessFlags, ctx, fscStatus);
		if (!opened)
			continue;
		if (opened->fscGetType() != FSC_TYPE_DIRECTORY)
		{
			for (FSCVirtualFile* folder : folders)
				delete folder;
			opened->m_isAppend = HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::IS_APPEND);
			return opened;
		}
		folders.push_back(opened);
	}
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
	{
		bool exists = !folders.empty();
		for (sint32 priority = 0; priority < FSC_PRIORITY_COUNT && !exists; priority++)
			exists = FindExact(path, priority) != nullptr;
		if (exists)
		{
			*fscStatus = FSC_STATUS_OK;
			return new MergedDirectory(path, std::move(folders));
		}
	}
	for (FSCVirtualFile* folder : folders)
		delete folder;
	*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
	return nullptr;
}

FSCVirtualFile* fsc_openDirIterator(const char* path, sint32* fscStatus)
{
	return fsc_open(path, FSC_ACCESS_FLAG::OPEN_DIR, fscStatus);
}

bool fsc_nextDir(FSCVirtualFile* fscFile, FSCDirEntry* dirEntry)
{
	std::lock_guard lock(s_lock);
	if (fscFile->fscGetType() != FSC_TYPE_DIRECTORY)
	{
		cemu_assert_suspicious();
		return false;
	}
	return fscFile->fscDirNext(dirEntry);
}

// creating, renaming and removing only look at the base priority
bool fsc_createDir(const char* path, sint32* fscStatus)
{
	*fscStatus = FSC_STATUS_UNDEFINED;
	std::lock_guard lock(s_lock);
	std::string devicePath;
	fscDeviceC* device;
	void* ctx;
	if (!ResolveDevice(path, FSC_PRIORITY_BASE, devicePath, device, ctx))
		return false;
	return device->fscDeviceCreateDir(devicePath, ctx, fscStatus);
}

bool fsc_rename(const char* srcPath, const char* dstPath, sint32* fscStatus)
{
	*fscStatus = FSC_STATUS_UNDEFINED;
	std::lock_guard lock(s_lock);
	std::string srcDevicePath, dstDevicePath;
	fscDeviceC *srcDevice, *dstDevice;
	void *srcCtx, *dstCtx;
	if (!ResolveDevice(srcPath, FSC_PRIORITY_BASE, srcDevicePath, srcDevice, srcCtx) || !ResolveDevice(dstPath, FSC_PRIORITY_BASE, dstDevicePath, dstDevice, dstCtx))
		return false;
	if (srcDevice != dstDevice)
		return false; // no moving between devices
	return srcDevice->fscDeviceRename(srcDevicePath, dstDevicePath, srcCtx, fscStatus);
}

bool fsc_remove(const char* path, sint32* fscStatus)
{
	*fscStatus = FSC_STATUS_UNDEFINED;
	std::lock_guard lock(s_lock);
	std::string devicePath;
	fscDeviceC* device;
	void* ctx;
	if (!ResolveDevice(path, FSC_PRIORITY_BASE, devicePath, device, ctx))
		return false;
	return device->fscDeviceRemoveFileOrDir(devicePath, ctx, fscStatus);
}

void fsc_close(FSCVirtualFile* fscFile)
{
	std::lock_guard lock(s_lock);
	delete fscFile;
}

uint32 fsc_getFileSize(FSCVirtualFile* fscFile)
{
	return (uint32)fscFile->fscQueryValueU64(FSC_QUERY_SIZE);
}

uint32 fsc_getFileSeek(FSCVirtualFile* fscFile)
{
	return (uint32)fscFile->fscGetSeek();
}

// read-only files cannot be seeked past their end
void fsc_setFileSeek(FSCVirtualFile* fscFile, uint32 newSeek)
{
	std::lock_guard lock(s_lock);
	if (!fsc_isWritable(fscFile))
		newSeek = std::min(newSeek, fsc_getFileSize(fscFile));
	fscFile->fscSetSeek(newSeek);
}

void fsc_setFileLength(FSCVirtualFile* fscFile, uint32 newEndOffset)
{
	std::lock_guard lock(s_lock);
	if (!fsc_isWritable(fscFile))
	{
		cemuLog_log(LogType::Force, "FSC: cannot change the length of a read-only file");
		return;
	}
	fscFile->fscSetFileLength(newEndOffset);
}

bool fsc_isDirectory(FSCVirtualFile* fscFile)
{
	return fscFile->fscGetType() == FSC_TYPE_DIRECTORY;
}

bool fsc_isFile(FSCVirtualFile* fscFile)
{
	return fscFile->fscGetType() == FSC_TYPE_FILE;
}

bool fsc_isWritable(FSCVirtualFile* fscFile)
{
	return fscFile->fscQueryValueU64(FSC_QUERY_WRITEABLE) != 0;
}

uint32 fsc_readFile(FSCVirtualFile* fscFile, void* buffer, uint32 size)
{
	return WithLock([&] { return fscFile->fscReadData(buffer, size); });
}

uint32 fsc_writeFile(FSCVirtualFile* fscFile, void* buffer, uint32 size)
{
	std::lock_guard lock(s_lock);
	if (!fsc_isWritable(fscFile))
		return 0;
	if (fscFile->m_isAppend)
		fsc_setFileSeek(fscFile, fsc_getFileSize(fscFile));
	return fscFile->fscWriteData(buffer, size);
}

uint8* fsc_extractFile(const char* path, uint32* fileSize, sint32 maxPriority)
{
	std::lock_guard lock(s_lock);
	sint32 status;
	*fileSize = 0;
	FSCVirtualFile* file = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status, maxPriority);
	if (!file)
		return nullptr;
	const uint32 size = fsc_getFileSize(file);
	uint8* data = (uint8*)malloc(size);
	const bool complete = fsc_readFile(file, data, size) == size;
	fsc_close(file);
	if (!complete)
	{
		free(data);
		return nullptr;
	}
	*fileSize = size;
	return data;
}

std::optional<std::vector<uint8>> fsc_extractFile(const char* path, sint32 maxPriority)
{
	constexpr uint32 kChunkSize = 32 * 1024 * 1024;
	std::lock_guard lock(s_lock);
	sint32 status;
	FSCVirtualFile* file = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status, maxPriority);
	if (!file)
		return std::nullopt;
	std::vector<uint8> data(fsc_getFileSize(file));
	for (size_t offset = 0; offset < data.size();)
	{
		const uint32 chunk = (uint32)std::min<size_t>(data.size() - offset, kChunkSize);
		if (fsc_readFile(file, data.data() + offset, chunk) != chunk)
		{
			fsc_close(file);
			return std::nullopt;
		}
		offset += chunk;
	}
	fsc_close(file);
	return data;
}

bool fsc_doesFileExist(const char* path, sint32 maxPriority)
{
	std::lock_guard lock(s_lock);
	sint32 status;
	FSCVirtualFile* file = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE, &status, maxPriority);
	if (!file)
		return false;
	fsc_close(file);
	return true;
}

bool fsc_doesDirectoryExist(const char* path, sint32 maxPriority)
{
	std::lock_guard lock(s_lock);
	sint32 status;
	FSCVirtualFile* dir = fsc_open(path, FSC_ACCESS_FLAG::OPEN_DIR, &status, maxPriority);
	if (!dir)
		return false;
	fsc_close(dir);
	return true;
}
