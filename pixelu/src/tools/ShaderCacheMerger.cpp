// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Stand-alone console tool (not part of the normal build): copies the entries
// of cache files placed in shaderCache/transferable/merge/ into the cache file
// of the same name in shaderCache/transferable/, then deletes the merged copy.
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/Core/LatteShaderCache.h"

#include <cinttypes>
#include <regex>
#include <thread>

namespace
{
	constexpr const char* kCacheFolder = "shaderCache/transferable/";
	constexpr const char* kMergeFolder = "shaderCache/transferable/merge/";

	// what: "shaders" or "pipelines", used in the messages
	void MergeCacheFile(const std::string& fileName, const char* what)
	{
		// file names start with the title id in hex
		uint64 titleId = 0;
		if (sscanf(fileName.c_str(), "%" SCNx64, &titleId) != 1)
			return;
		const fs::path target = fs::path(kCacheFolder) / fileName;
		const fs::path source = fs::path(kMergeFolder) / fileName;
		std::error_code ec;
		if (!fs::exists(target, ec) || !fs::exists(source, ec))
			return;

		std::unique_ptr<FileCache> into(FileCache::Open(target));
		std::unique_ptr<FileCache> from(FileCache::Open(source));
		if (!into || !from)
		{
			printf("Failed to open cache files for %s\n", fileName.c_str());
			return;
		}

		printf("Merging %s %016" PRIx64 "...", what, titleId);
		uint32 added = 0;
		const sint32 count = from->GetFileCount();
		for (sint32 index = 0; index < count; index++)
		{
			uint64 nameA = 0, nameB = 0;
			std::vector<uint8> data;
			if (!from->GetFileByIndex(index, &nameA, &nameB, data))
				continue;
			if (into->HasFile({nameA, nameB}))
				continue; // the target's own entry wins
			into->AddFile({nameA, nameB}, data.data(), (sint32)data.size());
			added++;
		}
		printf(" -> added %u, now %d %s\n", added, (sint32)into->GetFileCount(), what);

		into.reset();
		from.reset();
		fs::remove(source, ec);
	}

	void MergeMatching(const std::regex& pattern, const char* what)
	{
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::directory_iterator(kCacheFolder, ec))
		{
			if (entry.is_directory(ec))
				continue;
			const std::string name = entry.path().filename().generic_string();
			if (std::regex_match(name, pattern))
				MergeCacheFile(name, what);
		}
	}
}

void MergeShaderCacheFile(std::string fileName)
{
	MergeCacheFile(fileName, "shaders");
}

void MergePipelineCacheFile(std::string fileName)
{
	MergeCacheFile(fileName, "pipelines");
}

void MergeShaderAndPipelineCacheFiles()
{
	printf("Looking for shader caches to merge...\n");
	MergeMatching(std::regex("^[0-9a-fA-F]{16}_shaders\\.bin$"), "shaders");
	printf("\nLooking for pipeline caches to merge...\n");
	MergeMatching(std::regex("^[0-9a-fA-F]{16}_vkpipeline\\.bin$"), "pipelines");
}

void ToolShaderCacheMerger()
{
	printf("Shader and pipeline cache merger\n");
	printf("Entries of the cache files in %s are added to the files of the same name in %s\n\n", kMergeFolder, kCacheFolder);
	MergeShaderAndPipelineCacheFiles();
	printf("done!\n");
	// keep the console window open
	for (;;)
		std::this_thread::sleep_for(std::chrono::seconds(1));
}
