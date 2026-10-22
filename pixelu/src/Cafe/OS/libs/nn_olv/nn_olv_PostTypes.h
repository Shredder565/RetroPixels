// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "nn_olv_Common.h"

namespace nn::olv
{
	namespace PostFlag
	{
		constexpr uint32 BodyText = 0x01;
		constexpr uint32 BodyMemo = 0x02;       // handwritten painting
		constexpr uint32 ExternalImage = 0x04;
		constexpr uint32 ExternalBinary = 0x08;
		constexpr uint32 Mii = 0x10;
		constexpr uint32 ExternalUrl = 0x20;
		constexpr uint32 AppData = 0x40;
		constexpr uint32 EmpathyAdded = 0x80;   // the user gave this post a "Yeah"
		constexpr uint32 Autopost = 0x100;
		constexpr uint32 Spoiler = 0x200;
		constexpr uint32 NotAutopost = 0x400;   // is_autopost was present and 0
	}

	namespace TopicFlag
	{
		constexpr uint32 Recommended = 0x01;
		constexpr uint32 Title = 0x02;
		constexpr uint32 Icon = 0x04;
	}

	// nn::olv::DownloadedDataBase (0xC008 bytes)
	struct DownloadedDataBase
	{
		/* +0x0000 */ uint32be flags;
		/* +0x0004 */ uint32be userPid;
		/* +0x0008 */ uint8be postId[32]; // NUL terminated, at most 22 characters
		/* +0x0028 */ uint64be postDate;
		/* +0x0030 */ sint8be feeling;
		/* +0x0031 */ uint8be reserved031[3];
		/* +0x0034 */ uint32be regionId;
		/* +0x0038 */ uint8be platformId;
		/* +0x0039 */ uint8be languageId;
		/* +0x003A */ uint8be countryId;
		/* +0x003B */ uint8be reserved03B;
		/* +0x003C */ uint16be bodyText[256];
		/* +0x023C */ uint32be bodyTextLength;
		/* +0x0240 */ uint8be memo[0xA000]; // zlib-compressed TGA
		/* +0xA240 */ uint32be memoSize;
		/* +0xA244 */ uint16be topicTag[152];
		/* +0xA374 */ uint8be appData[1024];
		/* +0xA774 */ uint32be appDataSize;
		/* +0xA778 */ uint8be externalBinaryUrl[256];
		/* +0xA878 */ uint32be externalBinarySize;
		/* +0xA87C */ uint8be externalImageUrl[256];
		/* +0xA97C */ uint32be externalImageSize;
		/* +0xA980 */ uint8be externalUrl[256];
		/* +0xAA80 */ uint8be mii[96]; // FFLStoreData
		/* +0xAAE0 */ uint16be miiNickname[16];
		/* +0xAB00 */ uint8be reservedAB00[0x1500];
		/* +0xC000 */ uint32be reservedC000;
		/* +0xC004 */ uint32be reservedC004;

		bool Has(uint32 flag) const { return ((uint32)flags & flag) == flag; }
		void Set(uint32 flag) { flags = (uint32)flags | flag; }
	};
	static_assert(sizeof(DownloadedDataBase) == 0xC008);
	static_assert(offsetof(DownloadedDataBase, memoSize) == 0xA240);
	static_assert(offsetof(DownloadedDataBase, appDataSize) == 0xA774);
	static_assert(offsetof(DownloadedDataBase, mii) == 0xAA80);

	// nn::olv::DownloadedPostData (0xC208 bytes)
	struct DownloadedPostData
	{
		/* +0x0000 */ DownloadedDataBase base;
		/* +0xC008 */ uint32be communityId;
		/* +0xC00C */ uint32be empathyCount;
		/* +0xC010 */ uint32be commentCount;
		/* +0xC014 */ uint32be reservedC014[125];
	};
	static_assert(sizeof(DownloadedPostData) == 0xC208);

	// nn::olv::DownloadedTopicData (0x1000 bytes)
	struct DownloadedTopicData
	{
		/* +0x000 */ uint32be flags;
		/* +0x004 */ uint32be communityId;
		/* +0x008 */ uint32be reserved008[1022];

		bool Has(uint32 flag) const { return ((uint32)flags & flag) == flag; }
		void Set(uint32 flag) { flags = (uint32)flags | flag; }
	};
	static_assert(sizeof(DownloadedTopicData) == 0x1000);

	namespace hidden
	{
		// nn::olv::hidden::DownloadedSystemPostData (0xC408 bytes)
		struct DownloadedSystemPostData
		{
			/* +0x0000 */ DownloadedPostData post;
			/* +0xC208 */ uint64be titleId;
			/* +0xC210 */ uint32be reservedC210[126];
		};
		static_assert(sizeof(DownloadedSystemPostData) == 0xC408);

		// nn::olv::hidden::DownloadedSystemTopicData (0x13000 bytes)
		struct DownloadedSystemTopicData
		{
			/* +0x00000 */ DownloadedTopicData topic;
			/* +0x01000 */ uint64be titleId;
			/* +0x01008 */ uint16be title[128];
			/* +0x01108 */ uint8be reserved1108[256];
			/* +0x01208 */ uint8be icon[0x1002C]; // zlib-compressed TGA
			/* +0x11234 */ uint32be iconSize;
			/* +0x11238 */ uint64be titleIds[32];
			/* +0x11338 */ uint32be titleIdCount;
			/* +0x1133C */ uint32be reserved1133C[1841];
		};
		static_assert(sizeof(DownloadedSystemTopicData) == 0x13000);
		static_assert(offsetof(DownloadedSystemTopicData, titleIdCount) == 0x11338);

		// nn::olv::hidden::DownloadedSystemTopicDataList (0xC1000 bytes)
		struct DownloadedSystemTopicDataList
		{
			static constexpr uint32 kMaxTopics = 10;
			static constexpr uint32 kMaxPostsPerTopic = 300;

			struct Topic
			{
				/* +0x00000 */ DownloadedSystemTopicData data;
				/* +0x13000 */ uint32be postCount;
				/* +0x13004 */ MEMPTR<DownloadedSystemPostData> posts[kMaxPostsPerTopic];
				/* +0x134B4 */ uint32be reserved134B4;
			};
			static_assert(sizeof(Topic) == 0x134B8);
			static_assert(offsetof(Topic, postCount) == 0x13000);

			/* +0x00000 */ uint32be topicCount;
			/* +0x00004 */ uint32be reserved004;
			/* +0x00008 */ Topic topics[kMaxTopics];
			/* +0xC0F38 */ uint32be reservedC0F38[50];
		};
		static_assert(sizeof(DownloadedSystemTopicDataList) == 0xC1000);
	}

	// nn::olv::DownloadPostDataListParam (0x1000 bytes)
	struct DownloadPostDataListParam
	{
		static constexpr size_t kMaxSearchPids = 12;
		static constexpr size_t kMaxSearchKeys = 5;
		static constexpr size_t kMaxPostIds = 20;

		static constexpr uint32 kFriendsOnly = 0x01;
		static constexpr uint32 kFollowingsOnly = 0x02;
		static constexpr uint32 kSelfOnly = 0x04;
		static constexpr uint32 kOnlyText = 0x08;
		static constexpr uint32 kOnlyMemo = 0x10;
		static constexpr uint32 kWithMii = 0x40;
		static constexpr uint32 kWithEmpathy = 0x80;

		/* +0x000 */ uint32be flags;
		/* +0x004 */ uint32be communityId;
		/* +0x008 */ uint32be searchPids[kMaxSearchPids];
		/* +0x038 */ uint8 languageId;
		/* +0x039 */ uint8 hasLanguageId;
		/* +0x03A */ uint8 reserved03A[2];
		/* +0x03C */ uint32be postDataMaxNum;
		/* +0x040 */ uint16be searchKeys[kMaxSearchKeys][152];
		/* +0x630 */ char postIds[kMaxPostIds][32];
		/* +0x8B0 */ uint64be postDate;
		/* +0x8B8 */ uint64be titleId; // system (Wii U Menu) searches only
		/* +0x8C0 */ uint32be bodyTextMaxLength;
		/* +0x8C4 */ uint8 reserved8C4[0x73C];

		bool Has(uint32 flag) const { return ((uint32)flags & flag) != 0; }
	};
	static_assert(sizeof(DownloadPostDataListParam) == 0x1000);
	static_assert(offsetof(DownloadPostDataListParam, postIds) == 0x630);
	static_assert(offsetof(DownloadPostDataListParam, bodyTextMaxLength) == 0x8C0);

	// fill a post from a <post> element of an Olive reply or the offline archive
	bool ReadPostXml(DownloadedPostData& post, const pugi::xml_node& node);
}
