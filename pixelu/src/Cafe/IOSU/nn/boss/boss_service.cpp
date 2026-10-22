// Original code for RetroPixels (PixelU). Not derived from Cemu.
// /dev/boss: SpotPass (BOSS) tasks and their downloaded data.
//
// On disk (mlc):
//   usr/boss/<tid hi>/<tid lo>/user/common/<persistent id>/<dir>/fad.db   file list of a storage
//   usr/boss/<tid hi>/<tid lo>/user/common/data/<dir>/<data id>          downloaded (decrypted) data
#include "Cafe/OS/libs/nn_common.h"
#include "util/helpers/helpers.h"

#include "Cafe/IOSU/iosu_types_common.h"
#include "Cafe/IOSU/nn/iosu_nn_service.h"

#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Cafe/Account/Account.h"

#include "boss_service.h"
#include "boss_common.h"

#include <pugixml.hpp>
#include <curl/curl.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "Cemu/ncrypto/ncrypto.h"
#include "Cemu/napi/napi_helper.h"
#include "IOSU/legacy/iosu_crypto.h"
#include "util/crypto/aes128.h"

namespace iosu::boss
{
	using namespace ::nn::boss;

	namespace
	{
		constexpr nnResult kSuccess = 0x200080;
		constexpr nnResult kNoAccount = 0xC0203880;
		constexpr nnResult kTaskNotFound = 0xA021F900;
		constexpr nnResult kStorageNotFound = 0xA025AA00;
		constexpr nnResult kEntryNotFound = 0xA021FB00;

		constexpr nnResult kBossOk = BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_BOSS, 0);
		constexpr nnResult kBossFatal = BUILD_NN_RESULT(NN_RESULT_LEVEL_FATAL, NN_RESULT_MODULE_NN_BOSS, 0);

		fs::path StorageDir(uint64 titleId, uint32 persistentId, const char* directory)
		{
			return ActiveSettings::GetMlcPath("usr/boss/{:08x}/{:08x}/user/common/{:08x}/{}", GetTitleIdHigh(titleId), GetTitleIdLow(titleId), persistentId, directory);
		}

		fs::path DataFile(uint64 titleId, const char* directory, uint32 dataId, bool temporary = false)
		{
			return ActiveSettings::GetMlcPath(temporary ? "usr/boss/{:08x}/{:08x}/user/common/data/{}/.{:08x}" : "usr/boss/{:08x}/{:08x}/user/common/data/{}/{:08x}",
				GetTitleIdHigh(titleId), GetTitleIdLow(titleId), directory, dataId);
		}

		// ---- fad.db: list of the data files of one storage --------------------------------

		class FileListDb
		{
		public:
			static constexpr uint32 kMaxEntries = 512;

			struct Entry
			{
				CafeString<32> fileName;
				uint32be fileId; // 0 marks the end of the list
				uint32 ukn24;
				uint32 flags; // 0x80000000 read, 0x40000000 delete protected, 0x20000000 new arrival (guessed)
				uint32 memo_2C;
				uint64be entryCreationTimestamp;
			};
			static_assert(sizeof(Entry) == 0x38);

			struct FileImage
			{
				uint8 header[8];
				Entry entries[kMaxEntries];
			};
			static_assert(sizeof(FileImage) == 28680);

			static bool StorageExists(const DirectoryName& directory, uint64 titleId, uint32 persistentId)
			{
				std::error_code ec;
				return fs::exists(StorageDir(titleId, persistentId, directory.name2.c_str()), ec);
			}

			// loads the list of a storage; the storage directory is created when missing
			nnResult Load(const TaskId& storage, uint64 titleId, uint32 persistentId)
			{
				m_loaded = false;
				memset(&m_image, 0, sizeof(m_image));
				fs::path dir = PrepareDir(storage, titleId, persistentId);
				m_path = dir / "fad.db";
				std::unique_ptr<FileStream> file(FileStream::openFile2(m_path));
				if (!file)
					return kStorageNotFound;
				file->readData(&m_image, sizeof(m_image));
				m_loaded = true;
				return kSuccess;
			}

			nnResult Create(const TaskId& storage, uint64 titleId, uint32 persistentId)
			{
				memset(&m_image, 0, sizeof(m_image));
				m_path = PrepareDir(storage, titleId, persistentId) / "fad.db";
				std::unique_ptr<FileStream> file(FileStream::createFile2(m_path));
				if (!file)
				{
					cemuLog_log(LogType::Force, "BOSS: cannot create {}", _pathToUtf8(m_path));
					return NN_RESULT_PLACEHOLDER_ERROR;
				}
				file->writeData(&m_image, sizeof(m_image));
				m_loaded = true;
				return kSuccess;
			}

			void Save()
			{
				if (!m_loaded)
					return;
				std::unique_ptr<FileStream> file(FileStream::createFile2(m_path));
				if (file)
					file->writeData(&m_image, sizeof(m_image));
				else
					cemuLog_log(LogType::Force, "BOSS: cannot write {}", _pathToUtf8(m_path));
			}

			void Reset()
			{
				m_loaded = false;
				memset(&m_image, 0, sizeof(m_image));
			}

			// removes the first entry with that name and closes the gap (in memory only)
			void Remove(const CafeString<32>& fileName)
			{
				cemu_assert_debug(m_loaded);
				for (uint32 i = 0; i < kMaxEntries; i++)
				{
					if (!(m_image.entries[i].fileName == fileName))
						continue;
					std::copy(m_image.entries + i + 1, m_image.entries + kMaxEntries, m_image.entries + i);
					memset(&m_image.entries[kMaxEntries - 1], 0, sizeof(Entry));
					return;
				}
			}

			nnResult Add(const CafeString<32>& fileName, uint32 fileId)
			{
				if (fileId == 0)
				{
					cemuLog_log(LogType::Force, "BOSS: data id 0 cannot be stored");
					return 0;
				}
				if (fileName.empty())
					return 0;
				Remove(fileName);
				cemu_assert_debug(m_loaded);
				for (Entry& e : m_image.entries)
				{
					if (e.fileId != 0)
						continue;
					e.fileName = fileName;
					e.fileId = fileId;
					e.ukn24 = 0;
					e.flags = 0;
					e.memo_2C = 0;
					e.entryCreationTimestamp = 0;
					return kSuccess;
				}
				cemuLog_log(LogType::Force, "BOSS: {} has no free entries", _pathToUtf8(m_path));
				return kEntryNotFound;
			}

			std::vector<DataName> Names() const
			{
				cemu_assert_debug(m_loaded);
				std::vector<DataName> names;
				for (const Entry& e : m_image.entries)
				{
					if (e.fileId == 0)
						break;
					DataName n;
					n.name = e.fileName;
					names.push_back(n);
				}
				return names;
			}

			bool Find(const DataName& name, Entry& out) const
			{
				if (name.name.empty())
					return false;
				cemu_assert_debug(m_loaded);
				for (const Entry& e : m_image.entries)
				{
					if (e.fileName == name.name)
					{
						out = e;
						return true;
					}
					if (e.fileId == 0)
						break;
				}
				return false;
			}

		private:
			static fs::path PrepareDir(const TaskId& storage, uint64 titleId, uint32 persistentId)
			{
				fs::path dir = StorageDir(titleId, persistentId, storage.id.c_str());
				std::error_code ec;
				if (!fs::exists(dir, ec))
					fs::create_directories(dir, ec);
				return dir;
			}

			FileImage m_image;
			bool m_loaded = false;
			fs::path m_path;
		};

		FileListDb g_fileList;

		// ---- the currently opened NsData file ---------------------------------------------

		class OpenedNsData
		{
		public:
			nnResult Open(const DirectoryName& directory, uint64 titleId, uint32 persistentId, const DataName& name)
			{
				m_open = false;
				m_path.clear();
				nnResult r = g_fileList.Load(TaskId(directory.name2.c_str()), titleId, persistentId);
				if (NN_RESULT_IS_FAILURE(r))
					return r;
				FileListDb::Entry entry;
				if (!g_fileList.Find(name, entry))
					return kEntryNotFound;
				m_path = DataFile(titleId, directory.name2.c_str(), entry.fileId);
				m_directory = directory;
				m_titleId = titleId;
				m_persistentId = persistentId;
				m_name = name;
				m_open = true;
				return kSuccess;
			}

			void Close()
			{
				m_open = false;
			}

			// deletes the data file; the list entry is only removed in memory, fad.db is not rewritten
			nnResult Delete()
			{
				cemu_assert_debug(m_open);
				std::error_code ec;
				if (!fs::remove(m_path, ec))
					cemuLog_log(LogType::Force, "BOSS: cannot delete {}", _pathToUtf8(m_path));
				if (NN_RESULT_IS_SUCCESS(g_fileList.Load(TaskId(m_directory.name2.c_str()), m_titleId, m_persistentId)))
					g_fileList.Remove(m_name.name);
				m_open = false;
				return kSuccess;
			}

			bool Exists() const
			{
				if (!m_open || m_path.empty())
					return false;
				std::error_code ec;
				if (fs::exists(m_path, ec))
					return true;
				cemuLog_log(LogType::Force, "BOSS: {} is listed but missing on disk. Use \"Clear SpotPass cache\" in the debug menu to reset the cache", _pathToUtf8(m_path));
				return false;
			}

			nnResult GetSize(uint32& size) const
			{
				cemu_assert_debug(m_open);
				if (!m_open)
					return NN_RESULT_PLACEHOLDER_ERROR;
				std::unique_ptr<FileStream> file(FileStream::openFile2(m_path));
				if (!file)
					return NN_RESULT_PLACEHOLDER_ERROR;
				size = (uint32)file->GetSize();
				return kSuccess;
			}

			bool Read(void* dst, uint32 size, uint32 offset, uint32& bytesRead) const
			{
				if (!m_open)
					return false;
				std::unique_ptr<FileStream> file(FileStream::openFile2(m_path));
				if (!file)
					return false;
				file->SetPosition(offset);
				bytesRead = (uint32)file->readData(dst, size);
				return true;
			}

		private:
			bool m_open = false;
			fs::path m_path;
			DirectoryName m_directory;
			uint64 m_titleId = 0;
			uint32 m_persistentId = 0;
			DataName m_name;
		};

		OpenedNsData g_nsData;

		const char* ConsoleLanguageCode()
		{
			switch (GetConfig().console_language)
			{
			case CafeConsoleLanguage::JA: return "ja";
			case CafeConsoleLanguage::EN: return "en";
			case CafeConsoleLanguage::FR: return "fr";
			case CafeConsoleLanguage::DE: return "de";
			case CafeConsoleLanguage::IT: return "it";
			case CafeConsoleLanguage::ES: return "es";
			case CafeConsoleLanguage::ZH: return "zh";
			case CafeConsoleLanguage::KO: return "ko";
			case CafeConsoleLanguage::NL: return "nl";
			case CafeConsoleLanguage::PT: return "pt";
			case CafeConsoleLanguage::RU: return "ru";
			case CafeConsoleLanguage::TW: return "tw";
			default: return "en";
			}
		}

		// ---- a registered task --------------------------------------------------------------

		struct TasksheetFile
		{
			std::string url;
			std::string fileName;
			uint32 dataId;
			uint32 fileType; // 0 app data, 1 message
			uint32 size;
		};

		class Task
		{
		public:
			Task(const TaskId& id, const TaskSettingCore& settings)
				: m_id(id), m_settings(settings), m_persistentId(iosuAct_getAccountIdOfCurrentAccount())
			{
			}

			nnResult Run()
			{
				std::lock_guard lock(m_mutex);
				cemu_assert_debug(m_state == TaskState::Initial || m_state == TaskState::Done);
				m_state = TaskState::Ready;
				m_turnState = TaskTurnState::Ready;
				return kSuccess;
			}

			TaskState GetState()
			{
				std::lock_guard lock(m_mutex);
				return m_state;
			}

			TaskTurnState GetTurnState()
			{
				std::lock_guard lock(m_mutex);
				return m_turnState;
			}

			sint32 GetHttpStatusCode() const { return m_httpStatus; }
			uint32 GetContentLength() const { return m_contentLength; }
			uint32 GetProcessedLength() const { return m_contentLength; } // progress is not tracked

			// executes a task in Ready state (called from the daemon thread)
			nnResult Execute()
			{
				{
					std::lock_guard lock(m_mutex);
					if (m_state != TaskState::Ready)
					{
						cemuLog_log(LogType::Force, "BOSS: task {} cannot run in state {}", m_id.id.c_str(), (uint32)m_state);
						return kBossFatal;
					}
					m_state = TaskState::Running;
					m_turnState = TaskTurnState::Running;
					m_contentLength = 0;
				}
				if (!ActiveSettings::IsOnlineEnabled())
				{
					Finish(false);
					return kBossOk;
				}
				const uint64 titleId = CafeSystem::GetForegroundTitleId();
				std::string url = BaseUrl();
				const char* country = NCrypto::GetCountryAsString(Account::GetCurrentAccount().GetCountry());
				const char* language = ConsoleLanguageCode();
				switch (m_settings.taskType)
				{
				case TaskType::NbdlTaskSetting:
					// the boss code and task id select the tasksheet
					cemu_assert_debug(m_settings.nbdl.fileName.empty());
					url.append(fmt::format("/1/{}/{}?c={}&l={}", m_settings.nbdl.bossCode.c_str(), m_id.id.c_str(), country, language));
					break;
				case TaskType::NbdlDataListTaskSetting:
					url.append(fmt::format("/1/{}/{}?c={}&l={}", m_settings.nbdl.bossCode.c_str(), m_id.id.c_str(), country, language));
					cemu_assert_unimplemented();
					cemuLog_logDebug(LogType::Force, "BOSS: task type {} is not supported", (uint32)m_settings.taskType.value());
					Finish(false, 0, 0);
					return kBossOk;
				case TaskType::PlayReportSetting:
					cemuLog_logDebug(LogType::Force, "BOSS: play report tasks are not supported");
					Finish(false, 0, 200);
					return kBossOk;
				default:
					cemuLog_logDebug(LogType::Force, "BOSS: task type {} is not supported", (uint32)m_settings.taskType.value());
					Finish(false, 0, 0);
					return kBossOk;
				}

				CurlRequestHelper request;
				request.initate(ActiveSettings::GetNetworkService(), url, CurlRequestHelper::SERVER_SSL_CONTEXT::CUSTOM);
				ConfigureCertificates(request);
				request.addHeaderField("X-BOSS-Digest:", ""); // digest is not computed
				request.addHeaderField("X-Boss-UniqueId", fmt::format("{:05x}", (titleId >> 8) & 0xFFFFF));
				request.addHeaderField("X-BOSS-TitleId", fmt::format("{:016x}", titleId));
				if (!m_settings.serviceToken.empty())
					request.addHeaderField("X-Nintendo-ServiceToken", m_settings.serviceToken.c_str());
				if (!request.submitRequest(false))
				{
					cemuLog_log(LogType::Force, "BOSS: request for task {} failed", m_id.id.c_str());
					Finish(false);
					return kBossFatal;
				}
				const sint32 status = request.GetHTTPStatusCode();
				m_httpStatus = status;
				m_contentLength = (uint32)request.getReceivedData().size();
				if (status != 200)
					cemuLog_logDebug(LogType::Force, "BOSS: HTTP {} for {}", status, url);
				if (status == 404)
				{
					Finish(false);
					return kBossFatal;
				}
				if (m_settings.taskType == TaskType::NbdlTaskSetting)
				{
					std::vector<TasksheetFile> files = ParseTasksheet(request.getReceivedData());
					cemuLog_log(LogType::Force, "SpotPass: task {} lists {} file(s)", m_id.id.c_str(), files.size());
					for (const TasksheetFile& f : files)
						Download(f);
				}
				Finish(true);
				return kBossOk;
			}

		private:
			void Finish(bool success)
			{
				std::lock_guard lock(m_mutex);
				m_state = TaskState::Done;
				m_turnState = success ? TaskTurnState::DoneSuccess : TaskTurnState::DoneError;
			}

			void Finish(bool success, uint32 contentLength, sint32 httpStatus)
			{
				std::lock_guard lock(m_mutex);
				m_state = TaskState::Done;
				m_turnState = success ? TaskTurnState::DoneSuccess : TaskTurnState::DoneError;
				m_contentLength = contentLength;
				m_httpStatus = httpStatus;
			}

			std::string BaseUrl() const
			{
				if (!m_settings.url.empty())
					return m_settings.url.c_str();
				switch (ActiveSettings::GetNetworkService())
				{
				case NetworkService::Pretendo: return PretendoURLs::BOSSURL;
				case NetworkService::Custom: return GetNetworkConfig().urls.BOSS.GetValue();
				case NetworkService::Nintendo:
				default: return NintendoURLs::BOSSURL;
				}
			}

			void ConfigureCertificates(CurlRequestHelper& request) const
			{
				request.ClearCaCertIds();
				request.ClearClientCertIds();
				for (uint8be cert : m_settings.internalCaCert)
				{
					if (cert != 0)
						request.AddCaCertId(cert);
				}
				request.AddCaCertId(105);
				// the client certificate is also added to the CA list; 3 is used when the task sets none
				request.AddCaCertId(m_settings.internalClientCert != 0 ? (uint8)m_settings.internalClientCert : 3);
				cemu_assert_debug(m_settings.caCert[0].empty() && m_settings.caCert[1].empty() && m_settings.caCert[2].empty());
				cemu_assert_debug(m_settings.clientCertName.empty());
			}

			std::vector<TasksheetFile> ParseTasksheet(std::span<uint8> xml) const
			{
				std::vector<TasksheetFile> files;
				pugi::xml_document doc;
				if (!doc.load_buffer(xml.data(), xml.size()))
					return files;
				for (pugi::xml_node sheet = doc.child("TaskSheet"); sheet; sheet = sheet.next_sibling("TaskSheet"))
				{
					const char* status = sheet.child("ServiceStatus").child_value();
					if (strcmp(status, "close") == 0)
					{
						cemuLog_log(LogType::Force, "SpotPass: service closed, tasksheet skipped");
						continue;
					}
					if (strcmp(status, "open") != 0)
						cemuLog_log(LogType::Force, "SpotPass: unknown ServiceStatus {}", status);
					if (pugi::xml_node tid = sheet.child("TitleId"))
					{
						uint64 sheetTitleId;
						TitleIdParser::ParseFromStr(tid.child_value(), sheetTitleId);
						if (sheetTitleId != CafeSystem::GetForegroundTitleId())
							cemuLog_log(LogType::Force, "SpotPass: tasksheet title {:016x} differs from the running title {:016x}", sheetTitleId, CafeSystem::GetForegroundTitleId());
					}
					else
						cemuLog_log(LogType::Force, "SpotPass: tasksheet has no TitleId");
					if (pugi::xml_node taskId = sheet.child("TaskId"))
					{
						if (strcmp(taskId.child_value(), m_id.id.c_str()) != 0)
							cemuLog_log(LogType::Force, "SpotPass: tasksheet task {} differs from {}", taskId.child_value(), m_id.id.c_str());
					}
					else
						cemuLog_log(LogType::Force, "SpotPass: tasksheet has no TaskId");
					pugi::xml_node list = sheet.child("Files");
					if (!list)
						continue;
					for (pugi::xml_node f = list.child("File"); f; f = f.next_sibling("File"))
					{
						pugi::xml_node name = f.child("Filename"), dataId = f.child("DataId"), fileUrl = f.child("Url"), size = f.child("Size"), type = f.child("Type");
						if (!name || !dataId || !fileUrl || !size || !type)
							continue;
						uint32 fileType = 0;
						if (strcmp(type.child_value(), "Message") == 0)
							fileType = 1;
						else if (strcmp(type.child_value(), "AppData") != 0)
							cemuLog_log(LogType::Force, "SpotPass: unknown file type {}", type.child_value());
						if (!f.child("AutoDelete").empty())
							cemuLog_log(LogType::Force, "SpotPass: AutoDelete ({}) is ignored", f.child_value("AutoDelete"));
						// Attributes, Notify, Conditions, SecInterval, LED and Played are ignored
						files.push_back({ fileUrl.child_value(), name.child_value(), (uint32)dataId.text().as_int(), fileType, (uint32)size.text().as_int() });
					}
				}
				return files;
			}

			// downloaded nbdl files: 0x20 byte plain header, then AES-128-CTR encrypted data
			// starting with a 0x20 byte HMAC-SHA256 of the payload
			struct NbdlHeader
			{
				uint32be magic; // 'boss'
				uint32be version; // 0x20001
				uint16be ukn08; // 1
				uint16be ukn0A; // 2
				uint8 nonce[0xC];
				uint32 padding18;
				uint32 padding1C;
				uint8 payloadHmac[0x20]; // encrypted
			};
			static_assert(sizeof(NbdlHeader) == 0x40);

			void Download(const TasksheetFile& f)
			{
				const uint64 titleId = CafeSystem::GetForegroundTitleId();
				const fs::path target = DataFile(titleId, m_id.id.c_str(), f.dataId);
				const uint32 sizeKb = (f.size + 1023) / 1024;
				std::error_code ec;
				if (fs::exists(target, ec))
				{
					// already downloaded earlier, only make sure it is listed
					cemuLog_log(LogType::Force, "\t- {} (DataId:{:08x} {}KB) (Skipping, already downloaded)", f.fileName, f.dataId, sizeKb);
					AddToFileList(f);
					return;
				}
				cemuLog_log(LogType::Force, "\t- {} (DataId:{:08x} {}KB)", f.fileName, f.dataId, sizeKb);
				CurlRequestHelper request;
				request.initate(ActiveSettings::GetNetworkService(), f.url, CurlRequestHelper::SERVER_SSL_CONTEXT::CUSTOM);
				ConfigureCertificates(request);
				if (!request.submitRequest())
				{
					cemuLog_log(LogType::Force, "SpotPass: download of {} failed", f.fileName);
					return;
				}
				std::vector<uint8> data = request.getReceivedData();
				if (data.size() < sizeof(NbdlHeader))
				{
					cemuLog_log(LogType::Force, "SpotPass: {} is too small ({} bytes)", f.fileName, data.size());
					return;
				}
				NbdlHeader header;
				memcpy(&header, data.data(), sizeof(header));
				if (header.magic != 'boss' || header.version != 0x20001 || header.ukn08 != 1 || header.ukn0A != 2)
				{
					cemuLog_log(LogType::Force, "SpotPass: {} has an invalid header", f.fileName);
					return;
				}
				const size_t fileSize = data.size();
				data.resize((fileSize + 0xF) & ~(size_t)0xF); // CTR is applied to whole blocks
				if (fileSize != f.size)
				{
					cemuLog_log(LogType::Force, "SpotPass: {} has {} bytes, the tasksheet says {}", f.fileName, fileSize, f.size);
					return;
				}
				if (f.fileName.find("..") != std::string::npos)
				{
					cemuLog_log(LogType::Force, "SpotPass: rejected file name {}", f.fileName);
					return;
				}
				static const uint8 kNbdlKey[16] = { 0x39,0x70,0x57,0x35,0x58,0x70,0x34,0x58,0x37,0x41,0x7a,0x30,0x71,0x5a,0x70,0x74 };
				uint8 counter[16] = {};
				memcpy(counter, header.nonce, sizeof(header.nonce));
				counter[15] = 1;
				constexpr size_t kEncryptedStart = offsetof(NbdlHeader, payloadHmac);
				AES128CTR_transform(data.data() + kEncryptedStart, data.size() - sizeof(NbdlHeader::payloadHmac), (uint8*)kNbdlKey, counter);
				uint8 expectedHmac[32];
				memcpy(expectedHmac, data.data() + kEncryptedStart, 32);
				const size_t payloadSize = fileSize - sizeof(NbdlHeader);

				// write to a temporary name, read back and verify before renaming
				const fs::path temp = DataFile(titleId, m_id.id.c_str(), f.dataId, true);
				if (!fs::exists(temp.parent_path(), ec))
					fs::create_directories(temp.parent_path(), ec);
				{
					std::unique_ptr<FileStream> out(FileStream::createFile2(temp));
					if (!out)
					{
						cemuLog_log(LogType::Force, "SpotPass: cannot create {}", _pathToUtf8(temp));
						return;
					}
					out->writeData(data.data() + sizeof(NbdlHeader), payloadSize);
				}
				std::optional<std::vector<uint8>> written = FileStream::LoadIntoMemory(temp);
				static const char kHmacKey[] = "uyr5I8pu4ycq2pOT3D53Bp0n7jK8eyjLO5U20ocUNdN5muwUcC4By881UXECeM08";
				uint8 actualHmac[32];
				unsigned int hmacLen = 32;
				HMAC(EVP_sha256(), kHmacKey, 64, written->data(), payloadSize, actualHmac, &hmacLen);
				if (memcmp(actualHmac, expectedHmac, 32) != 0)
				{
					cemuLog_log(LogType::Force, "SpotPass: hash mismatch for {}", _pathToUtf8(temp));
					fs::remove(temp, ec);
					return;
				}
				fs::rename(temp, target, ec);
				AddToFileList(f);
			}

			void AddToFileList(const TasksheetFile& f)
			{
				const uint64 titleId = CafeSystem::GetForegroundTitleId();
				nnResult r = g_fileList.Load(m_id, titleId, m_persistentId);
				if (r == kStorageNotFound)
					r = g_fileList.Create(m_id, titleId, m_persistentId);
				if (NN_RESULT_IS_FAILURE(r))
				{
					cemuLog_log(LogType::Force, "SpotPass: no file list for task {}: {:08x}", m_id.id.c_str(), r);
					return;
				}
				CafeString<32> name;
				name.assign(f.fileName);
				g_fileList.Add(name, f.dataId);
				g_fileList.Save();
			}

			TaskId m_id;
			TaskSettingCore m_settings;
			uint32 m_persistentId;
			std::recursive_mutex m_mutex;
			TaskState m_state = TaskState::Initial;
			TaskTurnState m_turnState = TaskTurnState::Ukn;
			sint32 m_httpStatus = 0;
			uint32 m_contentLength = 0;
		};

		// ---- task registry and the thread that runs tasks ---------------------------------

		class TaskRunner
		{
		public:
			void Start()
			{
				if (m_running.exchange(true))
					return;
				m_thread = std::thread(&TaskRunner::ThreadMain, this);
			}

			void Stop()
			{
				if (!m_running.exchange(false))
					return;
				m_thread.join();
			}

			// tasks are identified by their id only (the persistent id is not part of the key)
			void Register(const TaskSettingCore& settings)
			{
				std::lock_guard lock(m_mutex);
				if (m_tasks.count(settings.taskId))
				{
					cemuLog_logDebug(LogType::Force, "BOSS: task {} is already registered", settings.taskId.id.c_str());
					return;
				}
				m_tasks.emplace(settings.taskId, std::make_shared<Task>(settings.taskId, settings));
			}

			void Unregister(const TaskId& id)
			{
				std::lock_guard lock(m_mutex);
				m_tasks.erase(id);
			}

			bool IsRegistered(const TaskId& id)
			{
				std::lock_guard lock(m_mutex);
				return m_tasks.count(id) != 0;
			}

			std::shared_ptr<Task> Find(const TaskId& id)
			{
				std::lock_guard lock(m_mutex);
				auto it = m_tasks.find(id);
				return it != m_tasks.end() ? it->second : nullptr;
			}

			void RunNow(const TaskId& id)
			{
				std::shared_ptr<Task> task = Find(id);
				if (!task)
				{
					cemuLog_log(LogType::Force, "BOSS: run requested for unknown task {}", id.id.c_str());
					return;
				}
				cemuLog_log(LogType::Force, "BOSS: running task {}", id.id.c_str());
				task->Run();
			}

			// scheduling is not emulated: an immediate start runs a stopped or new task once
			void StartScheduling(const TaskId& id, bool immediately)
			{
				if (!immediately)
				{
					cemuLog_logDebug(LogType::Force, "BOSS: deferred scheduling of task {} is not supported", id.id.c_str());
					return;
				}
				std::shared_ptr<Task> task = Find(id);
				if (!task)
					return;
				TaskState state = task->GetState();
				if (state == TaskState::Stopped || state == TaskState::Initial)
					task->Run();
				else
					cemuLog_logDebug(LogType::Force, "BOSS: task {} is not stopped, scheduling ignored", id.id.c_str());
			}

		private:
			void ThreadMain()
			{
				CURL* curl = curl_easy_init();
				while (m_running)
				{
					if (std::shared_ptr<Task> task = NextReady())
					{
						task->Execute();
						cemu_assert_debug(task->GetState() != TaskState::Ready);
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
				curl_easy_cleanup(curl);
			}

			std::shared_ptr<Task> NextReady()
			{
				std::lock_guard lock(m_mutex);
				for (auto& [id, task] : m_tasks)
				{
					if (task->GetState() == TaskState::Ready)
						return task;
				}
				return nullptr;
			}

			std::thread m_thread;
			std::atomic_bool m_running{ false };
			std::mutex m_mutex;
			std::map<TaskId, std::shared_ptr<Task>> m_tasks;
		};

		TaskRunner g_tasks;

		// ---- /dev/boss ----------------------------------------------------------------------

		class BossService : public iosu::nn::IPCService
		{
		public:
			BossService() : iosu::nn::IPCService("/dev/boss") {}

			nnResult ServiceCall(IPCServiceCall& call) override
			{
				if (call.GetServiceId() != 0)
				{
					cemu_assert_suspicious();
					return kBossOk;
				}
				switch ((BossCommandId)call.GetCommandId())
				{
				case BossCommandId::TaskIsRegistered: return TaskIsRegistered(call);
				case BossCommandId::TaskRegisterA:
				case BossCommandId::TaskRegisterForImmediateRunA: return TaskRegister(call);
				case BossCommandId::TaskUnregister: return TaskUnregister(call);
				case BossCommandId::TaskRun: return TaskRun(call);
				case BossCommandId::TaskStartScheduling: return TaskStartScheduling(call);
				case BossCommandId::TaskStopScheduling: return TaskStopScheduling(call);
				case BossCommandId::TaskWaitA: return TaskWait(call);
				case BossCommandId::TaskGetTurnState: return TaskGetTurnState(call);
				case BossCommandId::TaskGetHttpStatusCodeA: return TaskGetHttpStatusCode(call);
				case BossCommandId::TaskGetContentLength: return TaskGetLength(call, false);
				case BossCommandId::TaskGetProcessedLength: return TaskGetLength(call, true);
				case BossCommandId::UknA7:
					cemuLog_logDebug(LogType::Force, "BOSS: command A5 ignored");
					return kSuccess;
				case BossCommandId::DeleteDataRelated:
					cemuLog_logDebug(LogType::Force, "BOSS: command A6 ignored");
					return kSuccess;
				case BossCommandId::StorageExist: return StorageExist(call);
				case BossCommandId::StorageGetDataList: return StorageGetDataList(call);
				case BossCommandId::NsDataExist: return NsDataExist(call);
				case BossCommandId::NsDataRead: return NsDataRead(call);
				case BossCommandId::NsDataGetSize: return NsDataGetSize(call);
				case BossCommandId::NsDataDeleteFile:
				case BossCommandId::NsDataDeleteFileWithHistory: return NsDataDelete(call);
				case BossCommandId::NsFinalize: return NsFinalize(call);
				case BossCommandId::TitleSetNewArrivalFlag: return TitleSetNewArrivalFlag(call);
				default:
					cemuLog_log(LogType::Force, "BOSS: unsupported command {:x}", call.GetCommandId());
					cemu_assert_unimplemented();
					return kBossOk;
				}
			}

		private:
			// 0 in a request means the current account; returns 0 when there is none
			static uint32 ReadAccount(IPCServiceCall& call)
			{
				uint32 id = call.ReadParameter<uint32be>();
				if (id != 0)
					return id;
				uint32 current;
				return iosu::act::GetPersistentId(iosu::act::ACT_SLOT_CURRENT, &current) ? current : 0;
			}

			// most task commands start with (persistent id, task id)
			static bool ReadTaskTarget(IPCServiceCall& call, TaskId& id)
			{
				if (!ReadAccount(call))
					return false;
				id = call.ReadParameter<TaskId>();
				return true;
			}

			nnResult TaskIsRegistered(IPCServiceCall& call)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				call.WriteResponse<uint8be>(g_tasks.IsRegistered(id) ? 1 : 0);
				return kSuccess;
			}

			nnResult TaskRegister(IPCServiceCall& call)
			{
				uint32 account = ReadAccount(call);
				if (!account)
					return kNoAccount;
				TaskId id = call.ReadParameter<TaskId>();
				TaskSettingCore settings = call.ReadUnalignedInputBufferInfo().ReadType<TaskSettingCore>();
				if (settings.persistentId == 0)
					settings.persistentId = account;
				settings.taskId = id;
				g_tasks.Register(settings);
				return kSuccess;
			}

			nnResult TaskUnregister(IPCServiceCall& call)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				g_tasks.Unregister(id);
				return kSuccess;
			}

			nnResult TaskRun(IPCServiceCall& call)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				call.ReadParameter<uint8be>(); // foreground run flag, not used
				g_tasks.RunNow(id);
				return kSuccess;
			}

			nnResult TaskStartScheduling(IPCServiceCall& call)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				uint8 immediately = call.ReadParameter<uint8be>();
				g_tasks.StartScheduling(id, immediately != 0);
				return kSuccess;
			}

			nnResult TaskStopScheduling(IPCServiceCall& call)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				return kSuccess; // nothing is scheduled
			}

			nnResult TaskWait(IPCServiceCall& call)
			{
				static_assert(sizeof(betype<TaskWaitState>) == 4);
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				TaskWaitState waitFor = call.ReadParameter<betype<TaskWaitState>>();
				uint32 timeoutSeconds = call.ReadParameter<uint32be>();
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
				std::shared_ptr<Task> task = g_tasks.Find(id);
				cemu_assert_debug(task != nullptr);
				if (!task)
					return kTaskNotFound;
				// polls the task; the response byte is 1 when the state was reached, 0 on timeout
				for (;;)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (waitFor != TaskWaitState::Done)
					{
						cemuLog_log(LogType::Force, "BOSS: unknown wait state {}", (uint32)waitFor);
						call.WriteResponse<uint8be>(0);
						return kSuccess;
					}
					if (task->GetState() == TaskState::Done)
					{
						call.WriteResponse<uint8be>(1);
						return kSuccess;
					}
					if (timeoutSeconds != 0 && std::chrono::steady_clock::now() >= deadline)
					{
						call.WriteResponse<uint8be>(0);
						return kSuccess;
					}
				}
			}

			// the query commands reply with an execution counter (always 1) followed by the value
			nnResult TaskGetTurnState(IPCServiceCall& call)
			{
				static_assert(sizeof(betype<TaskTurnState>) == 4);
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				std::shared_ptr<Task> task = g_tasks.Find(id);
				call.WriteResponse<uint32be>(1);
				call.WriteResponse<betype<TaskTurnState>>(task ? task->GetTurnState() : TaskTurnState::Ukn);
				return task ? kSuccess : kTaskNotFound;
			}

			nnResult TaskGetHttpStatusCode(IPCServiceCall& call)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				std::shared_ptr<Task> task = g_tasks.Find(id);
				call.WriteResponse<sint32be>(1);
				call.WriteResponse<sint32be>(task ? task->GetHttpStatusCode() : 0);
				return task ? kSuccess : kTaskNotFound;
			}

			nnResult TaskGetLength(IPCServiceCall& call, bool processed)
			{
				TaskId id;
				if (!ReadTaskTarget(call, id))
					return kNoAccount;
				std::shared_ptr<Task> task = g_tasks.Find(id);
				uint32 length = task ? (processed ? task->GetProcessedLength() : task->GetContentLength()) : 0;
				call.WriteResponse<uint32be>(1);
				call.WriteResponse<uint64be>(length);
				return task ? kSuccess : kTaskNotFound;
			}

			nnResult StorageExist(IPCServiceCall& call)
			{
				static_assert(sizeof(DirectoryName) == 8);
				static_assert(sizeof(betype<StorageKind>) == 4);
				uint32 account = ReadAccount(call);
				DirectoryName directory = call.ReadParameter<DirectoryName>();
				StorageKind kind = call.ReadParameter<betype<StorageKind>>();
				if (!account)
					return kNoAccount;
				bool exists = true;
				if (kind == StorageKind::StorageNbdl)
					exists = FileListDb::StorageExists(directory, CafeSystem::GetForegroundTitleId(), account); // only the storage directory is checked
				else
				{
					cemuLog_logDebug(LogType::Force, "BOSS: storage kind {} not handled", (uint32)kind);
					cemu_assert_unimplemented();
				}
				call.WriteResponse<uint8be>(exists ? 1 : 0);
				return kSuccess;
			}

			nnResult StorageGetDataList(IPCServiceCall& call)
			{
				uint32 account = ReadAccount(call);
				if (!account)
					return kNoAccount;
				DirectoryName directory = call.ReadParameter<DirectoryName>();
				uint32 ukn = call.ReadParameter<uint32be>();
				cemu_assert_debug(ukn == 0);
				IPCServiceCall::UnalignedBuffer output = call.ReadUnalignedOutputBufferInfo();
				uint32 start = call.ReadParameter<uint32be>();
				cemu_assert_debug(start == 0); // offsets are not supported
				nnResult r = g_fileList.Load(TaskId(directory.name2.c_str()), CafeSystem::GetForegroundTitleId(), account);
				if (NN_RESULT_IS_FAILURE(r))
					return r;
				std::vector<DataName> names = g_fileList.Names();
				names.resize(std::min(names.size(), output.GetSize() / sizeof(DataName)));
				output.WriteData(names.data(), names.size() * sizeof(DataName));
				call.WriteResponse<uint32be>((uint32)names.size());
				call.WriteResponse<uint8be>(1);
				return kSuccess;
			}

			// NsData commands start with (persistent id, directory, storage kind, file name)
			struct NsDataTarget
			{
				uint32 account;
				DirectoryName directory;
				DataName name;
			};

			static bool ReadNsDataTarget(IPCServiceCall& call, NsDataTarget& t)
			{
				t.account = ReadAccount(call);
				if (!t.account)
					return false;
				t.directory = call.ReadParameter<DirectoryName>();
				call.ReadParameter<uint32be>(); // storage kind (unused)
				t.name = call.ReadParameter<DataName>();
				return true;
			}

			static nnResult OpenNsData(const NsDataTarget& t)
			{
				return g_nsData.Open(t.directory, CafeSystem::GetForegroundTitleId(), t.account, t.name);
			}

			nnResult NsDataExist(IPCServiceCall& call)
			{
				NsDataTarget t;
				if (!ReadNsDataTarget(call, t))
					return kNoAccount;
				nnResult r = OpenNsData(t);
				if (NN_RESULT_IS_FAILURE(r))
					return r;
				call.WriteResponse<uint8be>(g_nsData.Exists() ? 1 : 0);
				return kSuccess;
			}

			nnResult NsDataRead(IPCServiceCall& call)
			{
				NsDataTarget t;
				if (!ReadNsDataTarget(call, t))
					return kNoAccount;
				IPCServiceCall::UnalignedBuffer output = call.ReadUnalignedOutputBufferInfo();
				uint64 offset = call.ReadParameter<uint64be>();
				nnResult r = OpenNsData(t);
				if (NN_RESULT_IS_FAILURE(r))
				{
					call.WriteResponse<uint64be>(0);
					return r;
				}
				// the whole output buffer is written, also past the end of the file
				std::vector<uint8> buffer(output.GetSize());
				uint32 bytesRead = 0;
				g_nsData.Read(buffer.data(), (uint32)buffer.size(), (uint32)offset, bytesRead);
				output.WriteData(buffer.data(), buffer.size());
				call.WriteResponse<uint64be>(bytesRead);
				return kSuccess;
			}

			nnResult NsDataGetSize(IPCServiceCall& call)
			{
				NsDataTarget t;
				if (!ReadNsDataTarget(call, t))
					return kNoAccount;
				nnResult r = OpenNsData(t);
				if (NN_RESULT_IS_FAILURE(r))
					return r;
				uint32 size = 0;
				r = g_nsData.GetSize(size);
				call.WriteResponse<uint64be>(size);
				return r;
			}

			nnResult NsDataDelete(IPCServiceCall& call)
			{
				NsDataTarget t;
				if (!ReadNsDataTarget(call, t))
					return kNoAccount;
				nnResult r = OpenNsData(t);
				if (NN_RESULT_IS_FAILURE(r))
					return r;
				return g_nsData.Delete(); // the history variant is handled the same way
			}

			nnResult NsFinalize(IPCServiceCall& call)
			{
				NsDataTarget t;
				if (!ReadNsDataTarget(call, t))
					return kNoAccount;
				g_nsData.Close();
				return kSuccess;
			}

			nnResult TitleSetNewArrivalFlag(IPCServiceCall& call)
			{
				if (!ReadAccount(call))
					return kNoAccount;
				call.ReadParameter<uint8be>();
				cemuLog_logDebug(LogType::Force, "BOSS: TitleSetNewArrivalFlag ignored");
				return kSuccess;
			}
		};

		BossService g_service;

		// note: the task thread keeps running after TitleStop, as before
		class BossModule : public ::IOSUModule
		{
			void TitleStart() override
			{
				g_service.Start();
				g_tasks.Start();
			}
			void TitleStop() override
			{
				g_service.Stop();
				g_fileList.Reset();
				g_nsData.Close();
			}
		};

		BossModule g_module;
	}

	IOSUModule* GetModule()
	{
		return &g_module;
	}
}
