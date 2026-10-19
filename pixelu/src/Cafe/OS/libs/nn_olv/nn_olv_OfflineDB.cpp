// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_OfflineDB.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include <zlib.h>
#include <random>
#include <zarchive/zarchivereader.h>

namespace nn::olv
{
	namespace
	{
		std::mutex s_archiveLock;
		bool s_archiveOpened = false; // tried to open (it may be missing)
		std::unique_ptr<ZArchiveReader> s_archive;

		void OpenArchiveOnce()
		{
			std::scoped_lock lock(s_archiveLock);
			if (s_archiveOpened)
				return;
			s_archive.reset(ZArchiveReader::OpenFromFile(ActiveSettings::GetUserDataPath("resources/miiverse/OfflineDB.zar")));
			if (!s_archive)
				cemuLog_log(LogType::Force, "nn_olv: the offline Miiverse archive is not installed, no posts will be shown");
			s_archiveOpened = true;
		}

		std::optional<uint64> ArchiveFileSize(const std::string& path)
		{
			if (!s_archive)
				return std::nullopt;
			ZArchiveNodeHandle node = s_archive->LookUp(path);
			if (!s_archive->IsFile(node))
				return std::nullopt;
			return s_archive->GetFileSize(node);
		}

		bool ReadArchiveFile(const std::string& path, std::vector<uint8>& data)
		{
			data.clear();
			if (!s_archive)
				return false;
			ZArchiveNodeHandle node = s_archive->LookUp(path);
			if (!s_archive->IsFile(node))
				return false;
			data.resize(s_archive->GetFileSize(node));
			s_archive->ReadFromFile(node, 0, data.size(), data.data());
			return true;
		}

		// which bundled post list belongs to the running title
		const char* PostListFor(uint64 titleId)
		{
			switch (titleId)
			{
			case 0x0005000010143400: // The Legend of Zelda: The Wind Waker HD (JP, US, EU)
			case 0x0005000010143500:
			case 0x0005000010143600:
				return "PostList_WindWakerHD.xml";
			default:
				return nullptr;
			}
		}

		// paintings are stored as a 1 bit 320x120 mask; games expect a compressed 32-bit TGA
		void AttachPainting(DownloadedPostData& post)
		{
			constexpr uint32 kWidth = 320, kHeight = 120;
			std::vector<uint8> mask;
			if (!ReadArchiveFile(fmt::format("memo/{}", (const char*)post.base.postId), mask))
				return;
			if (mask.size() != kWidth * kHeight / 8)
				return;
			const uint8 header[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				(uint8)(kWidth & 0xFF), (uint8)(kWidth >> 8), (uint8)(kHeight & 0xFF), (uint8)(kHeight >> 8), 32, 8 };
			std::vector<uint8> tga(sizeof(header) + kWidth * kHeight * 4);
			memcpy(tga.data(), header, sizeof(header));
			uint8* pixel = tga.data() + sizeof(header);
			for (uint32 i = 0; i < kWidth * kHeight; i++, pixel += 4)
			{
				const uint8 shade = ((mask[i / 8] >> (i % 8)) & 1) ? 0xFF : 0x00;
				pixel[0] = shade;
				pixel[1] = shade;
				pixel[2] = shade;
				pixel[3] = 0xFF;
			}
			uLongf packed = sizeof(post.base.memo);
			if (compress((uint8*)post.base.memo, &packed, tga.data(), tga.size()) != Z_OK)
				return;
			post.base.memoSize = (uint32)packed;
			post.base.Set(PostFlag::BodyMemo);
		}

		void AttachExternalImage(DownloadedPostData& post)
		{
			const std::string path = fmt::format("image/{}.jpg", (const char*)post.base.postId);
			const std::optional<uint64> size = ArchiveFileSize(path);
			if (!size || path.size() >= sizeof(post.base.externalImageUrl))
				return;
			memcpy(post.base.externalImageUrl, path.c_str(), path.size() + 1);
			post.base.externalImageSize = (uint32)*size;
			post.base.Set(PostFlag::ExternalImage);
		}

		// pick up to maxCount random posts of the title's list
		uint32 CollectPosts(DownloadedPostData* posts, uint32 maxCount)
		{
			const char* listName = PostListFor(CafeSystem::GetForegroundTitleId());
			if (!listName)
				return 0;
			std::vector<uint8> xml;
			if (!ReadArchiveFile(listName, xml))
				return 0;
			pugi::xml_document doc;
			if (!doc.load_buffer(xml.data(), xml.size()))
				return 0;
			std::vector<pugi::xml_node> pool;
			for (pugi::xml_node n : doc.child("posts").children("post"))
				pool.push_back(n);

			std::mt19937 rng(std::random_device{}());
			uint32 count = 0;
			while (!pool.empty() && count < maxCount)
			{
				const size_t pick = std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng);
				const pugi::xml_node node = pool[pick];
				pool[pick] = pool.back();
				pool.pop_back();

				DownloadedPostData& post = posts[count];
				memset(&post, 0, sizeof(post));
				if (!ReadPostXml(post, node))
					continue;
				AttachPainting(post);
				AttachExternalImage(post);
				count++;
			}
			return count;
		}
	}

	void OfflineDB_Shutdown()
	{
		std::scoped_lock lock(s_archiveLock);
		s_archive.reset();
		s_archiveOpened = false;
	}

	nnResult OfflineDB_ListPosts(DownloadedTopicData* topic, DownloadedPostData* posts, uint32be* postCountOut, uint32 maxCount, DownloadPostDataListParam* param)
	{
		OpenArchiveOnce();
		memset(topic, 0, sizeof(*topic));
		topic->communityId = param->communityId;
		*postCountOut = 0;
		if (param->Has(DownloadPostDataListParam::kSelfOnly))
			return kOlvSuccess; // the archive holds no posts of the user

		memset(posts, 0, sizeof(DownloadedPostData) * maxCount);
		return RunBlockingOnHost([&]() -> nnResult {
			*postCountOut = CollectPosts(posts, maxCount);
			return kOlvSuccess;
		});
	}

	nnResult OfflineDB_ReadExternalImage(DownloadedDataBase* post, void* out, uint32be* outSize, uint32 maxSize)
	{
		OpenArchiveOnce();
		return RunBlockingOnHost([&]() -> nnResult {
			if (!post->Has(PostFlag::ExternalImage))
				return kOlvMissingData;
			std::vector<uint8> jpeg;
			if (!ReadArchiveFile(fmt::format("image/{}.jpg", (const char*)post->postId), jpeg))
				return kOlvRequestFailed;
			if (jpeg.size() > maxSize)
				return kOlvNotEnoughSize;
			memcpy(out, jpeg.data(), jpeg.size());
			*outSize = (uint32)jpeg.size();
			return kOlvSuccess;
		});
	}
}
