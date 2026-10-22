// Original code for RetroPixels (PixelU). Not derived from Cemu.
// ACP (application config) service: the private /dev/acp_main request used by nn_acp,
// plus the save directory helpers that nn_acp and nn_save call directly.
#include "iosu_ioctl.h"
#include "iosu_acp.h"
#include "Cafe/OS/libs/nn_common.h"
#include "util/tinyxml2/tinyxml2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/nn_save/nn_save.h"
#include "util/helpers/helpers.h"
#include "Cafe/OS/libs/nn_acp/nn_acp.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/IOSU/iosu_types_common.h"
#include "Cafe/IOSU/nn/iosu_nn_service.h"
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"

#include <inttypes.h>

namespace iosu
{
	namespace
	{
		constexpr const char* kMlcVolume = "/vol/storage_mlc01/";

		// ---- meta.xml reader -----------------------------------------------------------

		enum class MetaField
		{
			Dec32, // decimal, stored as uint32be
			Hex32, // hexadecimal, stored as uint32be
			Hex64, // hexadecimal, stored byte swapped in a uint64
			Text,  // zero terminated string, truncated to the field size
		};

		struct MetaFieldDesc
		{
			const char* tag;
			MetaField kind;
			size_t offset;
			size_t size;
		};

#define META_FIELD(_kind, _member) { #_member, MetaField::_kind, offsetof(acpMetaXml_t, _member), sizeof(acpMetaXml_t::_member) }

		const MetaFieldDesc kMetaFields[] =
		{
			META_FIELD(Hex64, title_id), META_FIELD(Hex64, boss_id), META_FIELD(Hex64, os_version),
			META_FIELD(Hex64, app_size), META_FIELD(Hex64, common_save_size), META_FIELD(Hex64, account_save_size),
			META_FIELD(Hex64, common_boss_size), META_FIELD(Hex64, account_boss_size), META_FIELD(Hex64, join_game_mode_mask),
			META_FIELD(Dec32, version),
			META_FIELD(Text, product_code), META_FIELD(Text, content_platform), META_FIELD(Text, company_code), META_FIELD(Text, mastering_date),
			META_FIELD(Dec32, logo_type), META_FIELD(Dec32, app_launch_type), META_FIELD(Dec32, invisible_flag),
			META_FIELD(Dec32, no_managed_flag), META_FIELD(Dec32, no_event_log), META_FIELD(Dec32, no_icon_database),
			META_FIELD(Dec32, launching_flag), META_FIELD(Dec32, install_flag), META_FIELD(Dec32, closing_msg),
			META_FIELD(Dec32, title_version), META_FIELD(Hex32, group_id), META_FIELD(Dec32, save_no_rollback),
			META_FIELD(Dec32, bg_daemon_enable), META_FIELD(Hex32, join_game_id), META_FIELD(Dec32, olv_accesskey),
			META_FIELD(Dec32, wood_tin), META_FIELD(Dec32, e_manual), META_FIELD(Dec32, e_manual_version),
			META_FIELD(Hex32, region),
			META_FIELD(Dec32, pc_cero), META_FIELD(Dec32, pc_esrb), META_FIELD(Dec32, pc_bbfc), META_FIELD(Dec32, pc_usk),
			META_FIELD(Dec32, pc_pegi_gen), META_FIELD(Dec32, pc_pegi_fin), META_FIELD(Dec32, pc_pegi_prt), META_FIELD(Dec32, pc_pegi_bbfc),
			META_FIELD(Dec32, pc_cob), META_FIELD(Dec32, pc_grb), META_FIELD(Dec32, pc_cgsrr), META_FIELD(Dec32, pc_oflc),
			META_FIELD(Dec32, pc_reserved0), META_FIELD(Dec32, pc_reserved1), META_FIELD(Dec32, pc_reserved2), META_FIELD(Dec32, pc_reserved3),
			META_FIELD(Dec32, ext_dev_nunchaku), META_FIELD(Dec32, ext_dev_classic), META_FIELD(Dec32, ext_dev_urcc),
			META_FIELD(Dec32, ext_dev_board), META_FIELD(Dec32, ext_dev_usb_keyboard), META_FIELD(Dec32, ext_dev_etc),
			META_FIELD(Text, ext_dev_etc_name),
			META_FIELD(Dec32, eula_version), META_FIELD(Dec32, drc_use), META_FIELD(Dec32, network_use),
			META_FIELD(Dec32, online_account_use), META_FIELD(Dec32, direct_boot),
		};
#undef META_FIELD

		// the long name, short name and publisher strings exist once per language
		const char* const kMetaLanguages[12] = { "ja", "en", "fr", "de", "it", "es", "zhs", "ko", "nl", "pt", "ru", "zht" };

		void ReadNumber(tinyxml2::XMLElement* menu, const char* tag, MetaField kind, void* dst)
		{
			tinyxml2::XMLElement* el = menu->FirstChildElement(tag);
			if (!el)
				return;
			const char* text = el->GetText();
			if (!text)
				return;
			if (kind == MetaField::Hex64)
			{
				uint64 v;
				if (sscanf(text, "%" SCNx64, &v) == 1)
					*(uint64*)dst = _swapEndianU64(v);
				return;
			}
			uint32 v;
			if (sscanf(text, kind == MetaField::Hex32 ? "%x" : "%u", &v) == 1)
				*(uint32be*)dst = v;
		}

		void ReadText(tinyxml2::XMLElement* menu, const char* tag, char* dst, size_t capacity)
		{
			tinyxml2::XMLElement* el = menu->FirstChildElement(tag);
			if (!el)
				return;
			const char* text = el->GetText();
			if (!text)
				return;
			size_t n = std::min(strlen(text), capacity - 1);
			memcpy(dst, text, n);
			dst[n] = '\0';
		}

		void ParseMetaXml(const uint8* xmlData, uint32 xmlSize, acpMetaXml_t* out)
		{
			memset(out, 0, sizeof(acpMetaXml_t));
			tinyxml2::XMLDocument doc;
			doc.Parse((const char*)xmlData, xmlSize);
			tinyxml2::XMLElement* menu = doc.FirstChildElement("menu");
			if (!menu)
				return;
			uint8* base = (uint8*)out;
			for (const MetaFieldDesc& f : kMetaFields)
			{
				if (f.kind == MetaField::Text)
					ReadText(menu, f.tag, (char*)(base + f.offset), f.size);
				else
					ReadNumber(menu, f.tag, f.kind, base + f.offset);
			}
			struct { const char* prefix; char* first; size_t stride; } localized[] =
			{
				{ "longname_", out->longname_ja, sizeof(out->longname_ja) },
				{ "shortname_", out->shortname_ja, sizeof(out->shortname_ja) },
				{ "publisher_", out->publisher_ja, sizeof(out->publisher_ja) },
			};
			for (auto& group : localized)
			{
				for (size_t lang = 0; lang < 12; lang++)
				{
					std::string tag = std::string(group.prefix) + kMetaLanguages[lang];
					ReadText(menu, tag.c_str(), group.first + lang * group.stride, group.stride);
				}
			}
			for (sint32 i = 0; i < 8; i++)
			{
				std::string tag = fmt::format("reserved_flag{}", i);
				ReadNumber(menu, tag.c_str(), MetaField::Dec32, &out->reserved_flag[i]);
			}
			for (sint32 i = 0; i < 32; i++)
			{
				std::string tag = fmt::format("add_on_unique_id{}", i);
				ReadNumber(menu, tag.c_str(), MetaField::Dec32, &out->add_on_unique_id[i]);
			}
		}

		bool ReadMetaXmlFile(const char* path, acpMetaXml_t* out)
		{
			uint32 size = 0;
			uint8* data = fsc_extractFile(path, &size);
			if (!data)
				return false;
			ParseMetaXml(data, size, out);
			free(data);
			return true;
		}

		// ---- helpers ---------------------------------------------------------------------

		bool IsPersistentIdName(const char* name)
		{
			if (strlen(name) != 8)
				return false;
			for (sint32 i = 0; i < 8; i++)
			{
				if (!isxdigit((unsigned char)name[i]))
					return false;
			}
			return true;
		}

		uint32 HexNameToU32(const char* name)
		{
			uint32 v = 0;
			sscanf(name, "%x", &v);
			return v;
		}

		std::string SaveRoot(uint32 high, uint32 low)
		{
			return fmt::format("{}usr/save/{:08x}/{:08x}/", kMlcVolume, high, low);
		}

		std::string TitleRoot(uint64 titleId)
		{
			// system titles live under sys/, everything else under usr/
			const char* area = ((titleId >> 32) & 0x10) ? "sys" : "usr";
			return fmt::format("{}{}/title/{:08x}/{:08x}/", kMlcVolume, area, GetTitleIdHigh(titleId), GetTitleIdLow(titleId));
		}

		uint64 NowSeconds()
		{
			return coreinit::OSGetTime() / ESPRESSO_TIMER_CLOCK;
		}

		// ---- request handlers ------------------------------------------------------------

		sint32 ListSaveTitles(uint32 device, acpTitleId_t* list, sint32 maxCount, uint32be* countOut)
		{
			if (device != 3)
				cemu_assert_unimplemented();
			sint32 found = 0;
			sint32 status = 0;
			std::string root = fmt::format("{}usr/save/", kMlcVolume);
			FSCVirtualFile* highDir = fsc_openDirIterator(root.c_str(), &status);
			if (highDir)
			{
				FSCDirEntry highEntry;
				while (fsc_nextDir(highDir, &highEntry))
				{
					if (!IsPersistentIdName(highEntry.path))
						continue;
					uint32 high = HexNameToU32(highEntry.path);
					std::string highPath = fmt::format("{}{:08x}/", root, high);
					FSCVirtualFile* lowDir = fsc_openDirIterator(highPath.c_str(), &status);
					if (!lowDir)
						continue;
					FSCDirEntry lowEntry;
					while (fsc_nextDir(lowDir, &lowEntry))
					{
						if (!IsPersistentIdName(lowEntry.path))
							continue;
						uint32 low = HexNameToU32(lowEntry.path);
						std::string metaPath = SaveRoot(high, low) + "meta/meta.xml";
						if (!fsc_doesFileExist(metaPath.c_str()))
						{
							cemuLog_logDebug(LogType::Force, "ACP: save {:08x}-{:08x} has no meta.xml", high, low);
							continue;
						}
						if (found < maxCount)
						{
							list[found].titleIdHigh = high;
							list[found].titleIdLow = low;
							found++;
						}
					}
					fsc_close(lowDir);
				}
				fsc_close(highDir);
			}
			*countOut = found;
			return 0;
		}

		sint32 ReadSaveMetaXml(uint64 titleId, acpMetaXml_t* out, sint32 device)
		{
			if (device != 3)
				assert_dbg();
			std::string path = SaveRoot(GetTitleIdHigh(titleId), GetTitleIdLow(titleId)) + "meta/meta.xml";
			if (!ReadMetaXmlFile(path.c_str(), out))
			{
				cemuLog_log(LogType::Force, "ACP: save meta file \"{}\" not found", path);
				memset(out, 0, sizeof(acpMetaXml_t));
			}
			return 0;
		}

		void LoadIntoBuffer(const std::string& path, uint8* dst, uint32 capacity)
		{
			uint32 size = 0;
			uint8* data = fsc_extractFile(path.c_str(), &size);
			if (!data)
			{
				cemuLog_log(LogType::Force, "ACP: could not read \"{}\"", path);
				return;
			}
			memcpy(dst, data, std::min(size, capacity));
			free(data);
		}

		sint32 ReadTitleMetaData(uint64 titleId, acpMetaData_t* out)
		{
			memset(out, 0, sizeof(acpMetaData_t));
			std::string metaDir = TitleRoot(titleId) + "meta/";
			LoadIntoBuffer(metaDir + "bootMovie.h264", out->bootMovie, sizeof(out->bootMovie));
			LoadIntoBuffer(metaDir + "bootLogoTex.tga", out->bootLogoTex, sizeof(out->bootLogoTex));
			return 0;
		}

		sint32 ReadTitleMetaXml(uint64 titleId, acpMetaXml_t* out)
		{
			memset(out, 0, sizeof(acpMetaXml_t));
			std::string path = TitleRoot(titleId) + "meta/meta.xml";
			if (!ReadMetaXmlFile(path.c_str(), out))
				cemuLog_log(LogType::Force, "ACP: title meta file \"{}\" not found", path);
			return 0;
		}

		void FillSaveDirEntry(acpSaveDirInfo_t& e, uint64 titleId, uint32 persistentId, uint64 size)
		{
			memset(&e, 0, sizeof(acpSaveDirInfo_t));
			uint32 high = GetTitleIdHigh(titleId);
			uint32 low = GetTitleIdLow(titleId);
			e.ukn00 = high;
			e.ukn04 = low;
			e.persistentId = persistentId;
			e.ukn0C = 0;
			e.sizeA = _swapEndianU64(0);
			e.sizeB = _swapEndianU64(size);
			e.time = _swapEndianU64(NowSeconds());
			std::string metaPath = SaveRoot(high, low) + "meta/";
			strncpy(e.path, metaPath.c_str(), sizeof(e.path) - 1);
		}

		sint32 ListSaveDirs(uint64 titleId, uint32 device, acpSaveDirInfo_t* out, sint32 maxCount, uint32be* countOut)
		{
			if (device != 3)
				cemu_assert_unimplemented();
			sint32 found = 0;
			sint32 status = 0;
			std::string userDir = SaveRoot(GetTitleIdHigh(titleId), GetTitleIdLow(titleId)) + "user/";
			// the common save comes first, reported with persistent id 0
			std::string commonDir = userDir + "common/";
			if (fsc_doesDirectoryExist(commonDir.c_str()) && found < maxCount)
			{
				uint64 size = 0;
				if (FSCVirtualFile* dir = fsc_open(commonDir.c_str(), FSC_ACCESS_FLAG::OPEN_DIR, &status))
				{
					size = fsc_getFileSize(dir);
					fsc_close(dir);
				}
				FillSaveDirEntry(out[found++], titleId, 0, size);
			}
			// then one entry per account directory
			FSCVirtualFile* it = fsc_openDirIterator(userDir.c_str(), &status);
			if (!it)
			{
				cemuLog_log(LogType::Force, "ACP: cannot list \"{}\"", userDir);
			}
			else
			{
				FSCDirEntry entry;
				while (fsc_nextDir(it, &entry))
				{
					if (!entry.isDirectory || !IsPersistentIdName(entry.path))
						continue;
					if (found < maxCount)
						FillSaveDirEntry(out[found++], titleId, HexNameToU32(entry.path), 0);
				}
				fsc_close(it);
			}
			*countOut = found;
			return 0;
		}

		void HandlePrivateRequest(iosuAcpCemuRequest_t* req)
		{
			uint32be count = 0;
			switch (req->requestCode)
			{
			case IOSU_ACP_GET_SAVE_DATA_TITLE_ID_LIST:
				req->returnCode = ListSaveTitles(req->type, (acpTitleId_t*)req->ptr.GetPtr(), req->maxCount, &count);
				req->resultU32.u32 = count;
				break;
			case IOSU_ACP_GET_TITLE_SAVE_META_XML:
				req->returnCode = ReadSaveMetaXml(req->titleId, (acpMetaXml_t*)req->ptr.GetPtr(), req->type);
				break;
			case IOSU_ACP_GET_TITLE_SAVE_DIR:
				req->returnCode = ListSaveDirs(req->titleId, req->type, (acpSaveDirInfo_t*)req->ptr.GetPtr(), req->maxCount, &count);
				req->resultU32.u32 = count;
				break;
			case IOSU_ACP_GET_TITLE_META_DATA:
				req->returnCode = ReadTitleMetaData(req->titleId, (acpMetaData_t*)req->ptr.GetPtr());
				break;
			case IOSU_ACP_GET_TITLE_META_XML:
				req->returnCode = ReadTitleMetaXml(req->titleId, (acpMetaXml_t*)req->ptr.GetPtr());
				break;
			case IOSU_ACP_CREATE_SAVE_DIR_EX:
				req->returnCode = acp::ACPCreateSaveDirEx(req->accountSlot, req->titleId);
				break;
			default:
				cemu_assert_unimplemented();
				break;
			}
		}

		void RequestLoop()
		{
			SetThreadName("iosuAcp_thread");
			for (;;)
			{
				ioQueueEntry_t* entry = iosuIoctl_getNextWithWait(IOS_DEVICE_ACP_MAIN);
				if (entry->request == IOSU_ACP_REQUEST_CEMU)
					HandlePrivateRequest((iosuAcpCemuRequest_t*)entry->bufferVectors[0].buffer.GetPtr());
				else
					cemu_assert_unimplemented();
				iosuIoctl_completeRequest(entry, 0);
			}
		}

		bool g_requestThreadStarted = false;
	}

	void iosuAcp_init()
	{
		if (g_requestThreadStarted)
			return;
		std::thread(RequestLoop).detach();
		g_requestThreadStarted = true;
	}

	namespace acp
	{
		namespace
		{
			void CopyTitleMetaFile(const std::string& titleStorage, const char* fileName, uint64 saveTitleId)
			{
				sint32 status;
				std::string src = titleStorage + "/meta/" + fileName;
				FSCVirtualFile* file = fsc_open(src.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
				if (!file)
					return;
				sint32 size = (sint32)fsc_getFileSize(file);
				std::unique_ptr<uint8[]> buffer = std::make_unique<uint8[]>(size);
				fsc_readFile(file, buffer.get(), size);
				fsc_close(file);
				fs::path dst = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/meta/{}", GetTitleIdHigh(saveTitleId), GetTitleIdLow(saveTitleId), fileName);
				std::ofstream out(dst, std::ios::out | std::ios::binary);
				out.write((const char*)buffer.get(), size);
			}

			sint32 MakeSaveDirectories(uint32 persistentId, uint64 titleId)
			{
				uint32 high = GetTitleIdHigh(titleId) & ~0xCu;
				uint32 low = GetTitleIdLow(titleId);
				uint32 account = persistentId == 0 ? 0x80000001 : persistentId;
				// the exact spelling (including the missing trailing slashes) is kept on purpose
				const std::string dirs[] =
				{
					fmt::format("{}usr/boss/", kMlcVolume),
					fmt::format("{}usr/boss/{:08x}/", kMlcVolume, high),
					fmt::format("{}usr/boss/{:08x}/{:08x}/", kMlcVolume, high, low),
					fmt::format("{}usr/boss/{:08x}/{:08x}/user/", kMlcVolume, high, low),
					fmt::format("{}usr/boss/{:08x}/{:08x}/user/common", kMlcVolume, high, low),
					fmt::format("{}usr/boss/{:08x}/{:08x}/user/{:08x}/", kMlcVolume, high, low, account),
					fmt::format("{}usr/save/{:08x}/", kMlcVolume, high),
					fmt::format("{}usr/save/{:08x}/{:08x}/", kMlcVolume, high, low),
					fmt::format("{}usr/save/{:08x}/{:08x}/meta/", kMlcVolume, high, low),
					fmt::format("{}usr/save/{:08x}/{:08x}/user/", kMlcVolume, high, low),
					fmt::format("{}usr/save/{:08x}/{:08x}/user/common", kMlcVolume, high, low),
					fmt::format("{}usr/save/{:08x}/{:08x}/user/{:08x}", kMlcVolume, high, low, account),
				};
				sint32 status = FSC_STATUS_FILE_NOT_FOUND;
				for (const std::string& d : dirs)
					fsc_createDir(d.c_str(), &status);
				CreateSaveMetaFiles(persistentId, titleId);
				return 0;
			}
		}

		nnResult ACPUpdateSaveTimeStamp(uint32 persistentId, uint64 titleId, ACPDeviceType deviceType)
		{
			if (deviceType == ACPDeviceType::UnknownType)
				return (nnResult)0xA030FB80;

			// only an existing, non-empty saveinfo.xml is updated
			const fs::path file = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/meta/saveinfo.xml", GetTitleIdHigh(titleId), GetTitleIdLow(titleId));
			std::optional<std::vector<uint8>> content = FileStream::LoadIntoMemory(file);
			if (!content || content->empty())
				return NN_RESULT_SUCCESS;

			tinyxml2::XMLDocument doc;
			tinyxml2::XMLError err = doc.Parse((const char*)content->data(), content->size());
			if (err != tinyxml2::XML_SUCCESS && err != tinyxml2::XML_ERROR_EMPTY_DOCUMENT)
				return NN_RESULT_SUCCESS;

			tinyxml2::XMLNode* first = doc.FirstChild();
			if (!first || !first->ToDeclaration())
				doc.InsertFirstChild(doc.NewDeclaration());

			tinyxml2::XMLElement* info = doc.FirstChildElement("info");
			if (!info)
				info = doc.InsertEndChild(doc.NewElement("info"))->ToElement();

			const std::string idText = fmt::format("{:08x}", persistentId);
			char stamp[32];
			snprintf(stamp, sizeof(stamp), "%" PRIx64, NowSeconds());

			tinyxml2::XMLElement* account = info->FirstChildElement("account");
			while (account && !account->Attribute("persistentId", idText.c_str()))
				account = account->NextSiblingElement("account");

			if (account)
			{
				tinyxml2::XMLElement* ts = account->FirstChildElement("timestamp");
				if (ts)
					ts->SetText(stamp);
				else
					account->InsertFirstChild(doc.NewElement("timestamp")); // added without a value
			}
			else
			{
				account = doc.NewElement("account");
				account->SetAttribute("persistentId", idText.c_str());
				tinyxml2::XMLElement* ts = doc.NewElement("timestamp");
				ts->SetText(stamp);
				account->InsertFirstChild(ts);
				info->InsertFirstChild(account);
			}

			tinyxml2::XMLPrinter printer;
			doc.Print(&printer);
			if (FileStream* out = FileStream::createFile2(file))
			{
				out->writeString(printer.CStr());
				delete out;
			}
			return NN_RESULT_SUCCESS;
		}

		void CreateSaveMetaFiles(uint32 persistentId, uint64 titleId)
		{
			// the files always come from the running title
			std::string storage = CafeSystem::GetMlcStoragePath(CafeSystem::GetForegroundTitleId());
			CopyTitleMetaFile(storage, "meta.xml", titleId);
			CopyTitleMetaFile(storage, "iconTex.tga", titleId);
			ACPUpdateSaveTimeStamp(persistentId, titleId, ACPDeviceType::InternalDeviceType);
		}

		nnResult ACPCreateSaveDir(uint32 persistentId, ACPDeviceType type)
		{
			return MakeSaveDirectories(persistentId, CafeSystem::GetForegroundTitleId());
		}

		sint32 ACPCreateSaveDirEx(uint8 accountSlot, uint64 titleId)
		{
			cemu_assert_debug(accountSlot >= 1 && accountSlot <= 13);
			uint32 persistentId = 0;
			bool ok = iosu::act::GetPersistentId(accountSlot, &persistentId);
			cemu_assert_debug(ok);
			return MakeSaveDirectories(persistentId, titleId);
		}

		nnResult ACPGetOlvAccesskey(uint32be* accessKey)
		{
			*accessKey = CafeSystem::GetForegroundTitleOlvAccesskey();
			return 0;
		}

		namespace
		{
			// IPC endpoint for the real nn_acp.rpl; no commands are implemented yet
			class MainService : public iosu::nn::IPCService
			{
			public:
				MainService() : iosu::nn::IPCService("/dev/acp_main") {}

				nnResult ServiceCall(IPCServiceCall& call) override
				{
					cemuLog_log(LogType::Force, "ACP: unsupported call on /dev/acp_main");
					cemu_assert_unimplemented();
					return BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_ACP, 0);
				}
			};

			MainService g_mainService;

			class AcpModule : public ::IOSUModule
			{
				void TitleStart() override { g_mainService.Start(); }
				void TitleStop() override { g_mainService.Stop(); }
			};

			AcpModule g_module;
		}

		IOSUModule* GetModule()
		{
			return &g_module;
		}
	}
}
