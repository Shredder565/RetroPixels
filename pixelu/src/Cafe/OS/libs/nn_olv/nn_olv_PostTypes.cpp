// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_PostTypes.h"
#include "nn_olv_OfflineDB.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/helpers/helpers.h"
#include <zlib.h>

namespace nn::olv
{
	using hidden::DownloadedSystemPostData;
	using hidden::DownloadedSystemTopicData;
	using hidden::DownloadedSystemTopicDataList;

	// --- XML -> guest structs --------------------------------------------------------

	namespace
	{
		bool Fail(const char* what)
		{
			cemuLog_log(LogType::Force, "nn_olv: post XML: {}", what);
			return false;
		}

		template<typename T>
		T Number(const pugi::xml_node& node)
		{
			return ConvertString<T>(node.child_value());
		}

		bool ReadPostBaseXml(DownloadedDataBase& b, const pugi::xml_node& node)
		{
			if (pugi::xml_node n = node.child("body"))
			{
				b.bodyTextLength = Utf8ToWideTerminated(b.bodyText, std::size(b.bodyText), n.child_value());
				if (b.bodyTextLength > 0)
					b.Set(PostFlag::BodyText);
			}
			if (pugi::xml_node n = node.child("topic_tag"))
				Utf8ToWideTerminated(b.topicTag, std::size(b.topicTag), n.child_value());
			if (pugi::xml_node n = node.child("feeling_id"))
			{
				const sint8 feeling = Number<sint8>(n);
				b.feeling = feeling;
				if (feeling < 0 || feeling >= 5)
					return Fail("feeling_id out of range");
			}
			if (pugi::xml_node n = node.child("id"))
			{
				const std::string_view id = n.child_value();
				if (id.size() > 22)
					return Fail("post id too long");
				memcpy(b.postId, id.data(), id.size());
				b.postId[id.size()] = 0;
			}
			if (pugi::xml_node n = node.child("is_autopost"))
			{
				const uint8 autopost = (uint8)Number<sint8>(n);
				if (autopost == 1)
					b.Set(PostFlag::Autopost);
				else if (autopost == 0)
					b.Set(PostFlag::NotAutopost);
				else
					return Fail("is_autopost is neither 0 nor 1");
			}
			if (pugi::xml_node n = node.child("empathy_added"); n && Number<sint32>(n) > 0)
				b.Set(PostFlag::EmpathyAdded);
			if (pugi::xml_node n = node.child("is_spoiler"); n && Number<sint32>(n) > 0)
				b.Set(PostFlag::Spoiler);
			if (pugi::xml_node n = node.child("mii"))
			{
				const std::vector<uint8> mii = NCrypto::base64Decode(n.child_value());
				if (mii.size() != sizeof(b.mii))
					return Fail("mii data has the wrong size");
				memcpy(b.mii, mii.data(), mii.size());
				b.Set(PostFlag::Mii);
			}
			if (pugi::xml_node n = node.child("pid"))
				b.userPid = Number<uint32>(n);
			if (pugi::xml_node n = node.child("screen_name"))
				Utf8ToWideTerminated(b.miiNickname, std::size(b.miiNickname), n.child_value());
			if (pugi::xml_node n = node.child("region_id"))
				b.regionId = Number<uint32>(n);
			if (pugi::xml_node n = node.child("platform_id"))
				b.platformId = Number<uint8>(n);
			if (pugi::xml_node n = node.child("language_id"))
				b.languageId = Number<uint8>(n);
			if (pugi::xml_node n = node.child("country_id"))
				b.countryId = Number<uint8>(n);
			if (pugi::xml_node painting = node.child("painting"))
			{
				if (pugi::xml_node content = painting.child("content"))
				{
					const std::vector<uint8> memo = NCrypto::base64Decode(content.child_value());
					if (memo.size() > sizeof(b.memo))
						return Fail("painting is too large");
					memcpy(b.memo, memo.data(), memo.size());
					b.Set(PostFlag::BodyMemo);
				}
				if (pugi::xml_node size = painting.child("size"))
					b.memoSize = Number<uint32>(size);
			}
			if (pugi::xml_node n = node.child("app_data"))
			{
				const std::vector<uint8> data = NCrypto::base64Decode(n.child_value());
				if (data.size() > sizeof(b.appData))
					return Fail("app data is too large");
				memcpy(b.appData, data.data(), data.size());
				b.appDataSize = (uint32)data.size();
				b.Set(PostFlag::AppData);
			}
			return true;
		}

		bool ReadSystemPostXml(DownloadedSystemPostData& post, const pugi::xml_node& node)
		{
			if (pugi::xml_node n = node.child("title_id"))
				post.titleId = Number<uint64>(n);
			return ReadPostXml(post.post, node);
		}

		void ReadSystemTopicXml(DownloadedSystemTopicData& topic, const pugi::xml_node& node)
		{
			if (pugi::xml_node n = node.child("community_id"))
				topic.topic.communityId = Number<uint32>(n);
			if (pugi::xml_node n = node.child("name"))
			{
				Utf8ToWideTerminated(topic.title, std::size(topic.title), n.child_value());
				topic.topic.Set(TopicFlag::Title);
			}
			if (pugi::xml_node n = node.child("is_recommended"); n && Number<uint32>(n) != 0)
				topic.topic.Set(TopicFlag::Recommended);
			if (pugi::xml_node n = node.child("title_id"))
				topic.titleId = Number<uint64>(n);
			// <title_ids> lists are not seen in replies so far and are ignored
			if (pugi::xml_node n = node.child("icon"))
			{
				const std::vector<uint8> icon = NCrypto::base64Decode(n.child_value());
				if (icon.size() > sizeof(topic.icon))
				{
					Fail("topic icon is too large");
					return;
				}
				topic.iconSize = (uint32)icon.size();
				memcpy(topic.icon, icon.data(), icon.size());
				topic.topic.Set(TopicFlag::Icon);
			}
		}
	}

	bool ReadPostXml(DownloadedPostData& post, const pugi::xml_node& node)
	{
		if (pugi::xml_node n = node.child("community_id"))
			post.communityId = Number<uint32>(n);
		if (pugi::xml_node n = node.child("empathy_count"))
			post.empathyCount = Number<uint32>(n);
		if (pugi::xml_node n = node.child("reply_count"))
			post.commentCount = Number<uint32>(n);
		return ReadPostBaseXml(post.base, node);
	}

	namespace
	{
		// --- DownloadedDataBase ---------------------------------------------------------

		bool Base_TestFlags(DownloadedDataBase* b, uint32 flag) { return b->Has(flag); }
		uint32 Base_GetUserPid(DownloadedDataBase* b) { return b->userPid; }
		uint64 Base_GetPostDate(DownloadedDataBase* b) { return b->postDate; }
		uint32 Base_GetRegionId(DownloadedDataBase* b) { return b->regionId; }
		uint32 Base_GetPlatformId(DownloadedDataBase* b) { return b->platformId; }
		uint32 Base_GetLanguageId(DownloadedDataBase* b) { return b->languageId; }
		uint32 Base_GetCountryId(DownloadedDataBase* b) { return b->countryId; }
		uint16be* Base_GetTopicTag(DownloadedDataBase* b) { return b->topicTag; }
		uint8be* Base_GetPostId(DownloadedDataBase* b) { return b->postId; }
		uint32 Base_GetAppDataSize(DownloadedDataBase* b) { return b->appDataSize; }

		uint32 Base_GetFeeling(DownloadedDataBase* b)
		{
			const sint8 feeling = b->feeling;
			return feeling >= 6 ? 0 : (uint32)(sint32)feeling;
		}

		uint8be* Base_GetExternalUrl(DownloadedDataBase* b)
		{
			return b->Has(PostFlag::ExternalUrl) ? b->externalUrl : nullptr;
		}

		nnResult Base_CopyMiiData(DownloadedDataBase* b, void* out)
		{
			if (!b->Has(PostFlag::Mii))
				return kOlvMissingData;
			if (!out)
				return kOlvInvalidPointer;
			memcpy(out, b->mii, sizeof(b->mii));
			return kOlvSuccess;
		}

		uint8be* Base_GetMiiData(DownloadedDataBase* b)
		{
			return b->Has(PostFlag::Mii) ? b->mii : nullptr;
		}

		uint16be* Base_GetMiiNickname(DownloadedDataBase* b)
		{
			return b->miiNickname[0] != 0 ? b->miiNickname : nullptr;
		}

		nnResult Base_GetBodyText(DownloadedDataBase* b, uint16be* out, uint32 maxChars)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (maxChars == 0)
				return kOlvNotEnoughSize;
			if (!b->Has(PostFlag::BodyText))
				return kOlvMissingData;
			memset(out, 0, maxChars * sizeof(uint16be));
			WideCopyPadded(out, b->bodyText, std::min<uint32>(maxChars, b->bodyTextLength));
			return kOlvSuccess;
		}

		nnResult Base_GetBodyMemo(DownloadedDataBase* b, uint8be* out, uint32be* outSize, uint32 maxSize)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (maxSize < 0x2582C) // 320x120x4 plus TGA header and footer
				return kOlvNotEnoughSize;
			if (!b->Has(PostFlag::BodyMemo))
				return kOlvMissingData;
			uLongf size = maxSize;
			if (uncompress((uint8*)out, &size, (const uint8*)b->memo, std::min<uint32>(b->memoSize, sizeof(b->memo))) != Z_OK)
			{
				cemuLog_log(LogType::Force, "nn_olv: inflating a post painting failed");
				return kOlvBadTextField;
			}
			if (outSize)
				*outSize = (uint32)size;
			return kOlvSuccess;
		}

		nnResult Base_GetAppData(DownloadedDataBase* b, uint8be* out, uint32be* outSize, uint32 maxSize)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (!b->Has(PostFlag::AppData))
				return kOlvMissingData;
			const uint32 n = std::min<uint32>(maxSize, b->appDataSize);
			memcpy(out, b->appData, n);
			if (outSize)
				*outSize = n;
			return kOlvSuccess;
		}

		nnResult Base_DownloadExternalImageData(DownloadedDataBase* b, void* out, uint32be* outSize, uint32 maxSize)
		{
			if (g_olv.useOfflineDB)
				return OfflineDB_ReadExternalImage(b, out, outSize, maxSize);
			if (!g_olv.online)
				return kOlvOfflineMode;
			if (!b->Has(PostFlag::ExternalImage))
				return kOlvMissingData;
			// fetching attached images from a live server is not supported yet
			cemuLog_logDebug(LogType::Force, "nn_olv: DownloadExternalImageData from a server is not supported");
			return kOlvRequestFailed;
		}

		uint32 Base_GetExternalImageDataSize(DownloadedDataBase* b)
		{
			return b->Has(PostFlag::ExternalImage) ? (uint32)b->externalImageSize : 0;
		}

		// --- posts and topics ---------------------------------------------------------------

		uint32 Post_GetCommunityId(DownloadedPostData* p) { return p->communityId; }
		uint32 Post_GetEmpathyCount(DownloadedPostData* p) { return p->empathyCount; }
		uint32 Post_GetCommentCount(DownloadedPostData* p) { return p->commentCount; }
		uint8be* Post_GetPostId(DownloadedPostData* p) { return p->base.postId; }

		uint64 SystemPost_GetTitleId(DownloadedSystemPostData* p) { return p->titleId; }

		uint32 Topic_GetCommunityId(DownloadedTopicData* t) { return t->communityId; }

		bool SystemTopic_TestFlags(DownloadedSystemTopicData* t, uint32 flag) { return t->topic.Has(flag); }
		uint64 SystemTopic_GetTitleId(DownloadedSystemTopicData* t) { return t->titleId; }
		uint32 SystemTopic_GetTitleIdNum(DownloadedSystemTopicData* t) { return t->titleIdCount; }

		nnResult SystemTopic_GetTitleText(DownloadedSystemTopicData* t, uint16be* out, uint32 maxChars)
		{
			if (!t->topic.Has(TopicFlag::Title))
				return kOlvMissingData;
			if (!out)
				return kOlvInvalidPointer;
			memset(out, 0, maxChars * sizeof(uint16be));
			WideCopyPadded(out, t->title, std::min<uint32>(maxChars, std::size(t->title)));
			return kOlvSuccess;
		}

		nnResult SystemTopic_GetTitleIconData(DownloadedSystemTopicData* t, void* out, uint32be* outSize, uint32 maxSize)
		{
			if (!t->topic.Has(TopicFlag::Icon))
				return kOlvMissingData;
			if (!out)
				return kOlvInvalidPointer;
			if (maxSize < sizeof(t->icon))
				return kOlvNotEnoughSize;
			uLongf size = maxSize;
			if (uncompress((uint8*)out, &size, (const uint8*)t->icon, std::min<uint32>(t->iconSize, sizeof(t->icon))) != Z_OK)
			{
				cemuLog_log(LogType::Force, "nn_olv: inflating a topic icon failed");
				return kOlvBadTextField;
			}
			if (outSize)
				*outSize = (uint32)size;
			return kOlvSuccess;
		}

		uint32 TopicList_GetTopicNum(DownloadedSystemTopicDataList* l) { return l->topicCount; }

		uint32 TopicList_GetPostNum(DownloadedSystemTopicDataList* l, uint32 topic)
		{
			return topic < DownloadedSystemTopicDataList::kMaxTopics ? (uint32)l->topics[topic].postCount : 0;
		}

		DownloadedSystemTopicData* TopicList_GetTopic(DownloadedSystemTopicDataList* l, uint32 topic)
		{
			return topic < DownloadedSystemTopicDataList::kMaxTopics ? &l->topics[topic].data : nullptr;
		}

		DownloadedSystemPostData* TopicList_GetPost(DownloadedSystemTopicDataList* l, uint32 topic, uint32 post)
		{
			if (topic >= DownloadedSystemTopicDataList::kMaxTopics || post >= DownloadedSystemTopicDataList::kMaxPostsPerTopic)
				return nullptr;
			return l->topics[topic].posts[post].GetPtr();
		}

		// parse the reply of a Wii U Menu topic query into a topic list and a flat post array;
		// returns 0 (also when a limit stops it early) or -1 for unreadable XML
		uint32 GetSystemTopicDataListFromRawData(DownloadedSystemTopicDataList* list, DownloadedSystemPostData* posts, uint32be* postCountOut, uint32 postCapacity, void* xml, uint32 xmlSize)
		{
			std::vector<uint8> text((const uint8*)xml, (const uint8*)xml + xmlSize);
			pugi::xml_document doc;
			if (!doc.load_buffer_inplace(text.data(), text.size(), pugi::parse_default, pugi::encoding_utf8))
				return 0xFFFFFFFF;

			memset(list, 0, sizeof(*list));
			uint32 postCount = 0;
			for (pugi::xml_node topicNode : doc.child("result").child("topics").children("topic"))
			{
				if (list->topicCount >= DownloadedSystemTopicDataList::kMaxTopics)
				{
					Fail("more than 10 topics");
					return 0;
				}
				DownloadedSystemTopicDataList::Topic& topic = list->topics[(uint32)list->topicCount];
				memset(&topic, 0, sizeof(topic));
				ReadSystemTopicXml(topic.data, topicNode);
				list->topicCount = list->topicCount + 1;

				for (pugi::xml_node person : topicNode.child("people").children("person"))
				{
					for (pugi::xml_node postNode : person.child("posts").children("post"))
					{
						if (postCount >= postCapacity)
						{
							Fail("more posts than the caller has room for");
							return 0;
						}
						DownloadedSystemPostData& post = posts[postCount];
						memset(&post, 0, sizeof(post));
						if (!ReadSystemPostXml(post, postNode))
							return 0;
						postCount++;
						if (topic.postCount >= DownloadedSystemTopicDataList::kMaxPostsPerTopic)
						{
							Fail("more than 300 posts in one topic");
							return 0;
						}
						topic.posts[(uint32)topic.postCount] = &post;
						topic.postCount = topic.postCount + 1;
					}
				}
			}
			*postCountOut = postCount;
			return 0;
		}

		// --- DownloadPostDataListParam ----------------------------------------------------

		using ListParam = DownloadPostDataListParam;

		ListParam* ListParam_Construct(ListParam* p)
		{
			memset(p, 0, sizeof(*p));
			return p;
		}

		nnResult ListParam_SetFlags(ListParam* p, uint32 flags)
		{
			p->flags = flags;
			return kOlvSuccess;
		}

		nnResult ListParam_SetLanguageId(ListParam* p, uint8 languageId)
		{
			p->languageId = languageId;
			p->hasLanguageId = 1;
			return kOlvSuccess;
		}

		nnResult ListParam_SetCommunityId(ListParam* p, uint32 communityId)
		{
			p->communityId = communityId;
			return kOlvSuccess;
		}

		nnResult ListParam_SetSearchKeyAt(ListParam* p, const uint16be* key, uint8 slot)
		{
			if (slot >= ListParam::kMaxSearchKeys)
				return kOlvInvalidParameter;
			memset(p->searchKeys[slot], 0, sizeof(p->searchKeys[slot]));
			if (!key)
				return kOlvSuccess;
			if (WideLength(key, std::size(p->searchKeys[slot])) > 50)
			{
				cemuLog_log(LogType::Force, "nn_olv: search key longer than 50 characters");
				return kOlvInvalidParameter;
			}
			WideAssignTruncated(p->searchKeys[slot], std::size(p->searchKeys[slot]), key);
			return kOlvSuccess;
		}

		nnResult ListParam_SetSearchKey(ListParam* p, const uint16be* key)
		{
			if (!key)
				return kOlvInvalidParameter;
			return ListParam_SetSearchKeyAt(p, key, 0);
		}

		nnResult ListParam_SetSearchPid(ListParam* p, uint32 pid)
		{
			if (p->Has(ListParam::kFriendsOnly | ListParam::kFollowingsOnly | ListParam::kSelfOnly))
				return kOlvInvalidParameter;
			p->searchPids[0] = pid;
			return kOlvSuccess;
		}

		nnResult ListParam_SetPostId(ListParam* p, const char* postId, uint32 slot)
		{
			if (slot >= ListParam::kMaxPostIds)
				return kOlvInvalidParameter;
			memset(p->postIds[slot], 0, sizeof(p->postIds[slot]));
			if (!postId)
				return kOlvInvalidPointer;
			const size_t len = strlen(postId);
			if (len > 22)
			{
				cemuLog_log(LogType::Force, "nn_olv: post id longer than 22 characters");
				return kOlvInvalidParameter;
			}
			memcpy(p->postIds[slot], postId, len + 1);
			return kOlvSuccess;
		}

		nnResult ListParam_SetPostDate(ListParam* p, uint64 date)
		{
			p->postDate = date;
			return kOlvSuccess;
		}

		nnResult ListParam_SetPostDataMaxNum(ListParam* p, uint32 count)
		{
			if (count == 0)
				return kOlvInvalidParameter;
			p->postDataMaxNum = count;
			return kOlvSuccess;
		}

		nnResult ListParam_SetBodyTextMaxLength(ListParam* p, uint32 length)
		{
			if (length >= 256)
				return kOlvInvalidParameter;
			p->bodyTextMaxLength = length;
			return kOlvSuccess;
		}

		nnResult ListParam_GetRawDataUrl(ListParam* p, char* out, uint32 outSize)
		{
			if (!g_olv.online)
				return kOlvOfflineMode;

			// the generic filters (language, limit, text/memo type) are not sent yet
			std::string query;
			bool anyPostId = false;
			for (const auto& id : p->postIds)
				anyPostId |= id[0] != '\0';
			if (anyPostId)
			{
				if (p->Has(ListParam::kWithMii))
					query += "&with_mii=1";
				if (p->Has(ListParam::kWithEmpathy))
					query += "&with_empathy_added=1";
				if (p->bodyTextMaxLength != 0)
					query += fmt::format("&max_body_length={}", (uint32)p->bodyTextMaxLength);
			}
			if (p->titleId != 0)
				query += fmt::format("&title_id={}", (uint64)p->titleId);
			if (p->Has(ListParam::kFriendsOnly))
				query += "&by=friend";
			if (p->Has(ListParam::kFollowingsOnly))
				query += "&by=followings";
			if (p->Has(ListParam::kSelfOnly))
				query += "&by=self";
			if (!query.empty())
				query[0] = '?';

			const std::string url = fmt::format("{}/v1/posts.search{}", g_DiscoveryResults.apiEndpoint, query);
			if (url.size() + 1 > outSize)
				return kOlvNotEnoughSize;
			memcpy(out, url.c_str(), url.size() + 1);
			return kOlvSuccess;
		}

		nnResult DownloadPostDataList(DownloadedTopicData* topic, DownloadedPostData* posts, uint32be* postCountOut, uint32 maxCount, ListParam* param)
		{
			if (g_olv.useOfflineDB)
				return OfflineDB_ListPosts(topic, posts, postCountOut, maxCount, param);
			memset(topic, 0, sizeof(*topic));
			topic->communityId = param->communityId;
			*postCountOut = 0;
			char url[2048];
			if (NN_RESULT_IS_FAILURE(ListParam_GetRawDataUrl(param, url, sizeof(url))))
				return kOlvInvalidParameter;
			// downloading posts from a live server is not implemented; report an empty list
			return kOlvSuccess;
		}
	}

	void RegisterPostExports()
	{
		cafeExportRegisterFunc(GetSystemTopicDataListFromRawData, "nn_olv", "GetSystemTopicDataListFromRawData__Q3_2nn3olv6hiddenFPQ4_2nn3olv6hidden29DownloadedSystemTopicDataListPQ4_2nn3olv6hidden24DownloadedSystemPostDataPUiUiPCUcT4", LogType::NN_OLV);

		cafeExportRegisterFunc(Base_TestFlags, "nn_olv", "TestFlags__Q3_2nn3olv18DownloadedDataBaseCFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetUserPid, "nn_olv", "GetUserPid__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetPostDate, "nn_olv", "GetPostDate__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetFeeling, "nn_olv", "GetFeeling__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetRegionId, "nn_olv", "GetRegionId__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetPlatformId, "nn_olv", "GetPlatformId__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetLanguageId, "nn_olv", "GetLanguageId__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetCountryId, "nn_olv", "GetCountryId__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetExternalUrl, "nn_olv", "GetExternalUrl__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_CopyMiiData, "nn_olv", "GetMiiData__Q3_2nn3olv18DownloadedDataBaseCFP12FFLStoreData", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetMiiNickname, "nn_olv", "GetMiiNickname__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetBodyText, "nn_olv", "GetBodyText__Q3_2nn3olv18DownloadedDataBaseCFPwUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetBodyMemo, "nn_olv", "GetBodyMemo__Q3_2nn3olv18DownloadedDataBaseCFPUcPUiUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetTopicTag, "nn_olv", "GetTopicTag__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetAppData, "nn_olv", "GetAppData__Q3_2nn3olv18DownloadedDataBaseCFPUcPUiUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetAppDataSize, "nn_olv", "GetAppDataSize__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetPostId, "nn_olv", "GetPostId__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetMiiData, "nn_olv", "GetMiiData__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_DownloadExternalImageData, "nn_olv", "DownloadExternalImageData__Q3_2nn3olv18DownloadedDataBaseCFPvPUiUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Base_GetExternalImageDataSize, "nn_olv", "GetExternalImageDataSize__Q3_2nn3olv18DownloadedDataBaseCFv", LogType::NN_OLV);

		cafeExportRegisterFunc(Post_GetCommunityId, "nn_olv", "GetCommunityId__Q3_2nn3olv18DownloadedPostDataCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Post_GetEmpathyCount, "nn_olv", "GetEmpathyCount__Q3_2nn3olv18DownloadedPostDataCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Post_GetCommentCount, "nn_olv", "GetCommentCount__Q3_2nn3olv18DownloadedPostDataCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Post_GetPostId, "nn_olv", "GetPostId__Q3_2nn3olv18DownloadedPostDataCFv", LogType::NN_OLV);

		cafeExportRegisterFunc(SystemPost_GetTitleId, "nn_olv", "GetTitleId__Q4_2nn3olv6hidden24DownloadedSystemPostDataCFv", LogType::NN_OLV);

		cafeExportRegisterFunc(Topic_GetCommunityId, "nn_olv", "GetCommunityId__Q3_2nn3olv19DownloadedTopicDataCFv", LogType::NN_OLV);

		cafeExportRegisterFunc(SystemTopic_TestFlags, "nn_olv", "TestFlags__Q4_2nn3olv6hidden25DownloadedSystemTopicDataCFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(SystemTopic_GetTitleId, "nn_olv", "GetTitleId__Q4_2nn3olv6hidden25DownloadedSystemTopicDataCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(SystemTopic_GetTitleIdNum, "nn_olv", "GetTitleIdNum__Q4_2nn3olv6hidden25DownloadedSystemTopicDataCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(SystemTopic_GetTitleText, "nn_olv", "GetTitleText__Q4_2nn3olv6hidden25DownloadedSystemTopicDataCFPwUi", LogType::NN_OLV);
		cafeExportRegisterFunc(SystemTopic_GetTitleIconData, "nn_olv", "GetTitleIconData__Q4_2nn3olv6hidden25DownloadedSystemTopicDataCFPUcPUiUi", LogType::NN_OLV);

		cafeExportRegisterFunc(TopicList_GetTopicNum, "nn_olv", "GetDownloadedSystemTopicDataNum__Q4_2nn3olv6hidden29DownloadedSystemTopicDataListCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(TopicList_GetPostNum, "nn_olv", "GetDownloadedSystemPostDataNum__Q4_2nn3olv6hidden29DownloadedSystemTopicDataListCFi", LogType::NN_OLV);
		cafeExportRegisterFunc(TopicList_GetTopic, "nn_olv", "GetDownloadedSystemTopicData__Q4_2nn3olv6hidden29DownloadedSystemTopicDataListCFi", LogType::NN_OLV);
		cafeExportRegisterFunc(TopicList_GetPost, "nn_olv", "GetDownloadedSystemPostData__Q4_2nn3olv6hidden29DownloadedSystemTopicDataListCFiT1", LogType::NN_OLV);

		cafeExportRegisterFunc(ListParam_Construct, "nn_olv", "__ct__Q3_2nn3olv25DownloadPostDataListParamFv", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetFlags, "nn_olv", "SetFlags__Q3_2nn3olv25DownloadPostDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetLanguageId, "nn_olv", "SetLanguageId__Q3_2nn3olv25DownloadPostDataListParamFUc", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetCommunityId, "nn_olv", "SetCommunityId__Q3_2nn3olv25DownloadPostDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetSearchKeyAt, "nn_olv", "SetSearchKey__Q3_2nn3olv25DownloadPostDataListParamFPCwUc", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetSearchKey, "nn_olv", "SetSearchKey__Q3_2nn3olv25DownloadPostDataListParamFPCw", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetSearchPid, "nn_olv", "SetSearchPid__Q3_2nn3olv25DownloadPostDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetPostId, "nn_olv", "SetPostId__Q3_2nn3olv25DownloadPostDataListParamFPCcUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetPostDate, "nn_olv", "SetPostDate__Q3_2nn3olv25DownloadPostDataListParamFL", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetPostDataMaxNum, "nn_olv", "SetPostDataMaxNum__Q3_2nn3olv25DownloadPostDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetBodyTextMaxLength, "nn_olv", "SetBodyTextMaxLength__Q3_2nn3olv25DownloadPostDataListParamFUi", LogType::NN_OLV);

		cafeExportRegisterFunc(ListParam_GetRawDataUrl, "nn_olv", "GetRawDataUrl__Q3_2nn3olv25DownloadPostDataListParamCFPcUi", LogType::NN_OLV);
		cafeExportRegisterFunc(DownloadPostDataList, "nn_olv", "DownloadPostDataList__Q2_2nn3olvFPQ3_2nn3olv19DownloadedTopicDataPQ3_2nn3olv18DownloadedPostDataPUiUiPCQ3_2nn3olv25DownloadPostDataListParam", LogType::NN_OLV);
	}
}
