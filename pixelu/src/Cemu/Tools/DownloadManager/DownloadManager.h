// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Download manager for titles owned by the logged in NNID: keeps a cache of eShop tickets,
// lists what can be downloaded (games, updates, DLC, a few system titles) and downloads,
// verifies and installs NUS packages into the MLC.
#pragma once
#include "util/helpers/Semaphore.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/TitleList/TitleId.h"
#include "util/helpers/ConcurrentQueue.h"
#include "config/NetworkSettings.h"

namespace NAPI
{
	struct IDBEIconDataV0;
	struct AuthInfo;
}

namespace NCrypto
{
	class TMDParser;
}

// One row of the download list as sent to the UI
struct DlMgrTitleReport
{
	enum class STATUS
	{
		// not downloading
		INSTALLABLE,
		INSTALLABLE_UNFINISHED, // a download folder from an earlier session exists
		INSTALLABLE_UPDATE, // DLC: the account owns newer content than what is installed
		// downloads
		QUEUED,
		PAUSED,
		INITIALIZING, // fetching the TMD
		CHECKING, // looking at files left by an earlier attempt
		DOWNLOADING,
		VERIFYING,
		INSTALLING,
		INSTALLED,
		HAS_ERROR
	};

	DlMgrTitleReport(STATUS status, uint64 titleId, uint16 version, std::string name, uint32 progress, uint32 progressMax, bool isPaused)
		: titleId(titleId), version(version), name(std::move(name)), status(status), progress(progress), progressMax(progressMax), isPaused(isPaused)
	{
	}

	uint64 titleId;
	uint16 version;
	std::string name; // UTF-8
	STATUS status;
	uint32 progress;
	uint32 progressMax;
	bool isPaused;
	std::string errorMsg;
};

enum class DLMGR_STATUS_CODE
{
	UNINITIALIZED,
	CONNECTING,
	FAILED,
	CONNECTED
};

class DownloadManager
{
public:
	static DownloadManager* GetInstance(bool createIfNotExist = true)
	{
		static DownloadManager* s_manager = nullptr;
		if (!s_manager && createIfNotExist)
			s_manager = new DownloadManager();
		return s_manager;
	}

	// Logs in on the manager thread. Progress and the result arrive through the status callback.
	void connect(
		std::string_view nnidAccountName,
		const std::array<uint8, 32>& passwordHash,
		CafeConsoleRegion region,
		std::string_view country,
		uint32 deviceId,
		std::string_view serial,
		std::string_view deviceCertBase64);

	bool IsConnected() const; // true once connect() was called, whatever the outcome

	// title names come from the icon data (IDBE), cached in memory and in the NUP file cache
	void prepareIDBE(uint64 titleId);
	std::string getNameFromCachedIDBE(uint64 titleId);

	// start, resume or retry; version is the one from the title report
	void initiateDownload(uint64 titleId, uint16 version);
	void pauseDownload(uint64 titleId, uint16 version);

	void setUserData(void* ptr) { m_userData = ptr; }
	void* getUserData() const { return m_userData; }

	// Setting a status or title callback also replays the current status message, the title
	// list and the state of every download to it.
	void registerCallbacks(
		void (*cbUpdateConnectStatus)(std::string statusText, DLMGR_STATUS_CODE statusCode),
		void (*cbAddDownloadableTitle)(const DlMgrTitleReport& titleInfo),
		void (*cbRemoveDownloadableTitle)(uint64 titleId, uint16 version));

	void setStatusMessage(std::string_view msg, DLMGR_STATUS_CODE statusCode);

	bool hasActiveDownloads();

	// forgets the download list and status (before connecting again)
	void reset();

private:
	/* login */
	enum class LoginState
	{
		Idle,
		Requested,
		Working,
		Succeeded,
		Failed,
	};

	struct Credentials
	{
		std::string cacheName; // prefix of this account's entries in the NUP file cache
		std::string accountName;
		std::array<uint8, 32> passwordHash;
		std::string deviceCertBase64;
		CafeConsoleRegion region;
		std::string country;
		uint32 deviceId; // without the Wii U platform bits
		std::string serial;
	};
	Credentials m_login{};

	struct
	{
		std::string accountId;
		std::string deviceToken;
	} m_iasToken{};

	std::atomic<LoginState> m_loginState{LoginState::Idle};

	void RunLogin();
	bool RequestIasToken();
	bool CheckAccountStatus();
	NetworkService Service() const;
	NAPI::AuthInfo MakeAuth(bool withIasToken) const;

	/* tickets */
	struct CachedTicket
	{
		// stored in the ticket cache file, keep the values
		enum class Origin : uint8
		{
			Account = 0, // personalized ticket of a purchase, stored depersonalized
			Public = 1, // common ticket (system titles, updates)
		};

		Origin origin;
		uint64 ticketId;
		uint32 ticketVersion;
		uint64 titleId; // from the ticket itself, all ones if it does not parse
		std::vector<uint8> data;
		std::vector<std::vector<uint8>> certs;
	};
	std::vector<CachedTicket> m_tickets;
	std::vector<std::pair<uint64, uint16>> m_partialDownloads; // download folders found on disk

	void AddTicket(CachedTicket::Origin origin, uint64 ticketId, uint32 ticketVersion, const std::vector<uint8>& data, const std::vector<std::vector<uint8>>& certs);
	CachedTicket* TicketById(uint64 ticketId);
	CachedTicket* TicketFor(uint64 titleId);
	CachedTicket* TicketFor(uint64 titleId, uint16 version);
	bool HasPartialDownload(uint64 titleId, uint16 version) const;

	void LoadTickets();
	void SaveTickets();
	bool SyncAccountTickets();
	void SyncSystemTickets();
	bool SyncUpdateTickets();
	bool SyncTickets();
	void FindPartialDownloads();
	void ReportTitles();

	/* update version list (TAGAYA) */
	bool m_haveVersionList{};
	std::unordered_map<uint64, uint16> m_latestVersions;
	void FetchVersionList();
	bool LatestVersion(uint64 titleId, uint16& version) const;

	/* download list */
	struct ListEntry
	{
		uint64 titleId;
		bool installed;
		uint16 installedVersion;
		uint16 installedTicketVersion; // DLC only: version of the installed title.tik
		bool hasLatestVersion; // listed in the update version list
		uint16 latestVersion;
	};
	ListEntry DescribeTitle(uint64 titleId) const;
	std::vector<ListEntry> BuildDownloadList(); // sorted by title id

	/* downloads */
	struct Download
	{
		Download(uint64 titleId, uint16 version, const std::vector<uint8>& ticket);
		~Download();

		enum class Phase
		{
			New,
			Checking,
			Downloading,
			Verifying,
			Installing,
			Installed
		};

		struct Content
		{
			enum class Step
			{
				Check, // a file from an earlier attempt may be there
				Download,
				Verify,
				Install, // verified, waiting for the install step
				ENUM_COUNT,
			};

			Content(const NCrypto::TMDParser::ContentEntry& entry);
			void Finish(Step next)
			{
				step = next;
				busy = false;
			}

			uint16 index;
			uint32 contentId;
			uint64 size;
			uint64 paddedSize; // what the server sends: 64 KiB blocks for hashed content, else 16 bytes
			NCrypto::TMDParser::TMDContentFlags flags;
			uint8 hash[32];
			Step step{Step::Check};
			bool busy{false}; // a task is working on the current step
			uint64 bytesReceived{};
		};

		uint64 titleId;
		uint16 version;
		std::vector<uint8> ticket;
		NCrypto::AesKey titleKey;

		Phase phase{Phase::New};
		bool active{}; // the one download being worked on
		bool paused{};
		bool fetchingTmd{};
		std::vector<uint8> tmdData;
		NCrypto::TMDParser* tmd{};
		std::unordered_map<uint16, Content> contents; // by content index
		uint32 progress{}; // downloading: per mille; checking, verifying, installing: files done
		uint32 progressMax{};
		bool installing{};
		bool failed{};
		std::string error;
	};

	std::recursive_mutex m_mutex;
	std::vector<Download*> m_downloads;

	Download* FindDownload(uint64 titleId, uint16 version);
	fs::path DownloadFolder(const Download* d) const;
	fs::path InstallFolder(const Download* d) const;
	void Advance(Download* d);
	void StartNextIfIdle();
	void Fail(Download* d, std::string message);
	void Report(Download* d);
	void SetProgress(Download* d, uint32 progress);
	void UpdateDownloadProgress(Download* d);

	// tasks, run on the thread pool
	void TaskFetchTmd(Download* d);
	void TaskFetchContent(Download* d, uint16 index);
	void TaskVerifyContent(Download* d, uint16 index);
	void TaskInstall(Download* d);
	bool ExtractFolder(Download* d, class FSTVolume* volume, const std::string& sourcePath, const fs::path& destination);

	/* callbacks */
	void (*m_cbUpdateConnectStatus)(std::string statusText, DLMGR_STATUS_CODE statusCode){nullptr};
	void (*m_cbAddDownloadableTitle)(const DlMgrTitleReport& titleInfo){nullptr};
	void (*m_cbRemoveDownloadableTitle)(uint64 titleId, uint16 version){nullptr};
	void* m_userData{};
	std::string m_statusMessage;
	DLMGR_STATUS_CODE m_statusCode{DLMGR_STATUS_CODE::UNINITIALIZED};

	/* manager thread */
	std::atomic_bool m_workerStarted{false};
	ConcurrentQueue<std::function<void()>> m_jobs;
	void StartWorker();
	void WorkerLoop();
};
