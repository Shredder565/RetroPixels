// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cemu/Tools/DownloadManager/DownloadManager.h"

#include "Cafe/Account/Account.h"
#include "util/crypto/md5.h"
#include "Cafe/TitleList/TitleId.h"
#include "Common/FileStream.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "config/ActiveSettings.h"
#include "util/ThreadPool/ThreadPool.h"
#include "util/helpers/enum_array.hpp"

#include "Cafe/Filesystem/FST/FST.h"
#include "Cafe/TitleList/TitleList.h"

#include <cinttypes>
#include <charconv>
#include <curl/curl.h>
#include <pugixml.hpp>

#include "WindowSystem.h"

#include "Cemu/napi/napi.h"
#include "util/helpers/Serializer.h"

namespace
{
	// NUP cache (mlc01/usr/save/system/nim/nup/cemu_cache.dat): IDBE icons plus, per account,
	// "<account>/token_info" and "<account>/eticket_cache"
	FileCache* s_nupCache = nullptr;

	std::mutex s_iconMutex;
	std::unordered_map<uint64, NAPI::IDBEIconDataV0*> s_icons;

	std::string TokenCacheName(const std::string& account) { return fmt::format("{}/token_info", account); }
	std::string TicketCacheName(const std::string& account) { return fmt::format("{}/eticket_cache", account); }
	std::string IconCacheName(uint64 titleId) { return fmt::format("idbe/{0:016x}", titleId); }

	// IAS account id and device token, so later logins can skip the IAS round trip
	struct TokenRecord : public SerializerHelper
	{
		std::string accountId;
		std::string deviceToken;

	protected:
		bool serializeImpl(MemStreamWriter& w) override
		{
			w.writeBE<uint8>(0); // format version
			w.writeBE<std::string>(accountId);
			w.writeBE<std::string>(deviceToken);
			return true;
		}

		bool deserializeImpl(MemStreamReader& r) override
		{
			if (r.readBE<uint8>() != 0)
				return false;
			accountId = r.readBE<std::string>();
			deviceToken = r.readBE<std::string>();
			return !r.hasError();
		}
	};

	constexpr uint8 kTicketCacheVersion = 1;

	bool IsListedType(TitleIdParser::TITLE_TYPE type, bool withUpdates, bool withDlc)
	{
		using T = TitleIdParser::TITLE_TYPE;
		switch (type)
		{
		case T::BASE_TITLE:
		case T::BASE_TITLE_DEMO:
		case T::SYSTEM_OVERLAY_TITLE:
			return true;
		case T::BASE_TITLE_UPDATE:
			return withUpdates;
		case T::AOC:
			return withDlc;
		default:
			return false;
		}
	}

	// eShop, Friend List and Internet browser of each region
	std::array<uint64, 3> SystemTitlesFor(CafeConsoleRegion region, bool& known)
	{
		known = true;
		switch (region)
		{
		case CafeConsoleRegion::EUR:
			return {0x000500301001420A, 0x000500301001520A, 0x000500301001220A};
		case CafeConsoleRegion::USA:
			return {0x000500301001410A, 0x000500301001510A, 0x000500301001210A};
		case CafeConsoleRegion::JPN:
			return {0x000500301001400A, 0x000500301001500A, 0x000500301001200A};
		default:
			known = false;
			return {};
		}
	}

	bool IsFriendListTitle(uint64 titleId)
	{
		return titleId == 0x000500301001500A || titleId == 0x000500301001510A || titleId == 0x000500301001520A;
	}

	// "<translated text> 3/10"
	std::string CountedMessage(std::string msg, size_t current, size_t total)
	{
		msg.append(fmt::format(" {0}/{1}", current, total));
		return msg;
	}

	bool WriteWholeFile(const fs::path& path, const std::vector<uint8>& data)
	{
		std::unique_ptr<FileStream> file(FileStream::createFile2(path));
		return file && (size_t)file->writeData(data.data(), (sint32)data.size()) == data.size();
	}
}

/* ---------------------------------------------------------------- update version list */

void DownloadManager::FetchVersionList()
{
	if (m_haveVersionList)
		return;
	NAPI::AuthInfo auth = MakeAuth(false);
	auto listVersion = NAPI::TAG_GetVersionListVersion(auth);
	if (!listVersion.isValid)
		return;
	auto list = NAPI::TAG_GetVersionList(auth, listVersion.fqdnURL, listVersion.version);
	if (!list.isValid)
		return;
	m_latestVersions = list.titleVersionList;
	m_haveVersionList = true;
}

bool DownloadManager::LatestVersion(uint64 titleId, uint16& version) const
{
	auto it = m_latestVersions.find(titleId);
	if (it == m_latestVersions.end())
		return false;
	version = it->second;
	return true;
}

/* ---------------------------------------------------------------- download list */

DownloadManager::ListEntry DownloadManager::DescribeTitle(uint64 titleId) const
{
	ListEntry e{};
	e.titleId = titleId;
	uint16 version = 0;
	e.installed = CafeTitleList::HasTitle(titleId, version);
	e.installedVersion = e.installed ? version : 0;
	// for DLC the installed content version is only visible in the installed title.tik
	// todo - this reads a file on the UI thread
	if (TitleIdParser(titleId).GetType() == TitleIdParser::TITLE_TYPE::AOC)
	{
		const auto tikPath = ActiveSettings::GetMlcPath(L"usr/title/{:08x}/{:08x}/code/title.tik", (uint32)(titleId >> 32), (uint32)(titleId & 0xFFFFFFFF));
		std::unique_ptr<FileStream> file(FileStream::openFile2(tikPath));
		if (file)
		{
			std::vector<uint8> tik;
			file->extract(tik);
			NCrypto::ETicketParser parser;
			if (parser.parse(tik.data(), tik.size()))
				e.installedTicketVersion = parser.GetTicketVersion();
		}
	}
	e.hasLatestVersion = LatestVersion(titleId, e.latestVersion);
	if (!e.hasLatestVersion)
		e.latestVersion = 0;
	return e;
}

// Everything the account can download: installed games, titles with a cached ticket (games,
// DLC, system titles) and the separate update of each of those games, if the version list
// knows one.
std::vector<DownloadManager::ListEntry> DownloadManager::BuildDownloadList()
{
	std::set<uint64> owned;
	for (TitleId titleId : CafeTitleList::GetAllTitleIds())
	{
		if (IsListedType(TitleIdParser(titleId).GetType(), false, false))
			owned.insert(titleId);
	}
	for (const CachedTicket& ticket : m_tickets)
	{
		if (IsListedType(TitleIdParser(ticket.titleId).GetType(), false, true))
			owned.insert(ticket.titleId);
	}

	std::set<uint64> listed;
	for (uint64 titleId : owned)
	{
		listed.insert(titleId);
		TitleIdParser parser(titleId);
		if (!parser.CanHaveSeparateUpdateTitleId())
			continue;
		const uint64 updateId = parser.GetSeparateUpdateTitleId();
		uint16 unused;
		if (LatestVersion(updateId, unused))
			listed.insert(updateId);
	}

	std::vector<ListEntry> list;
	list.reserve(listed.size());
	for (uint64 titleId : listed)
		list.push_back(DescribeTitle(titleId));
	return list;
}

void DownloadManager::ReportTitles()
{
	if (!m_cbAddDownloadableTitle)
		return;
	using STATUS = DlMgrTitleReport::STATUS;
	for (const ListEntry& e : BuildDownloadList())
	{
		const TitleIdParser::TITLE_TYPE type = TitleIdParser(e.titleId).GetType();
		if (!IsListedType(type, true, true))
		{
			cemu_assert_debug(false);
			continue;
		}
		// installed titles are only listed when their ticket is known
		CachedTicket* ticket = TicketFor(e.titleId);
		if (e.installed && !ticket)
			continue;
		const bool newerDlcContent = e.installed && type == TitleIdParser::TITLE_TYPE::AOC && ticket && ticket->ticketVersion > e.installedTicketVersion;

		STATUS status = STATUS::INSTALLABLE;
		if (e.installed && e.installedVersion >= e.latestVersion && !newerDlcContent)
			status = STATUS::INSTALLED;
		else if (newerDlcContent)
			status = STATUS::INSTALLABLE_UPDATE;
		else if (HasPartialDownload(e.titleId, e.latestVersion))
			status = STATUS::INSTALLABLE_UNFINISHED;

		DlMgrTitleReport report(status, e.titleId, e.latestVersion, getNameFromCachedIDBE(e.titleId), 0, 0, false);
		m_cbAddDownloadableTitle(report);
	}
}

/* ---------------------------------------------------------------- login */

NetworkService DownloadManager::Service() const
{
	return NetworkService::Nintendo;
}

NAPI::AuthInfo DownloadManager::MakeAuth(bool withIasToken) const
{
	NAPI::AuthInfo auth;
	auth.serviceOverwrite = Service();
	auth.accountId = m_login.accountName;
	auth.passwordHash = m_login.passwordHash;
	auth.deviceId = m_login.deviceId;
	auth.serial = m_login.serial;
	auth.country = m_login.country;
	auth.region = m_login.region;
	auth.deviceCertBase64 = m_login.deviceCertBase64;
	if (withIasToken)
	{
		cemu_assert_debug(!m_iasToken.accountId.empty());
		auth.IASToken.accountId = m_iasToken.accountId;
		auth.IASToken.deviceToken = m_iasToken.deviceToken;
	}
	return auth;
}

bool DownloadManager::RequestIasToken()
{
	NAPI::AuthInfo auth = MakeAuth(false);
	auto challenge = NAPI::IAS_GetChallenge(auth);
	if (challenge.apiError != NAPI_RESULT::SUCCESS)
		return false;
	auto registration = NAPI::IAS_GetRegistrationInfo_QueryInfo(auth, challenge.challenge);
	if (registration.apiError != NAPI_RESULT::SUCCESS)
		return false;
	m_iasToken.accountId = registration.accountId;
	m_iasToken.deviceToken = registration.deviceToken;

	TokenRecord record;
	record.accountId = registration.accountId;
	record.deviceToken = registration.deviceToken;
	std::vector<uint8> blob;
	if (!record.serialize(blob))
		return false;
	s_nupCache->AddFileAsync({TokenCacheName(m_login.cacheName)}, blob.data(), blob.size());
	return true;
}

bool DownloadManager::CheckAccountStatus()
{
	NAPI::AuthInfo auth = MakeAuth(true);
	NAPI::NAPI_ECSGetAccountStatus_Result result = NAPI::ECS_GetAccountStatus(auth);
	if (result.apiError != NAPI_RESULT::SUCCESS)
	{
		cemuLog_log(LogType::Force, "ECS - Failed to query account status (error: {0} {1})", result.apiError, result.serviceError);
		return false;
	}
	if (result.accountStatus == NAPI::NAPI_ECSGetAccountStatus_Result::AccountStatus::UNREGISTERED)
	{
		cemuLog_log(LogType::Force, "ECS - Account is not registered");
		return false;
	}
	return true;
}

void DownloadManager::RunLogin()
{
	m_loginState.store(LoginState::Working);
	m_iasToken.accountId.clear();
	m_iasToken.deviceToken.clear();
	setStatusMessage(_tr("Logging in..."), DLMGR_STATUS_CODE::CONNECTING);

	// IAS token from the cache, else ask the server
	if (s_nupCache)
	{
		std::vector<uint8> blob;
		TokenRecord record;
		if (s_nupCache->GetFile({TokenCacheName(m_login.cacheName)}, blob) && record.deserialize(blob))
		{
			m_iasToken.accountId = record.accountId;
			m_iasToken.deviceToken = record.deviceToken;
		}
	}
	if ((m_iasToken.accountId.empty() || m_iasToken.deviceToken.empty()) && !RequestIasToken())
	{
		cemuLog_log(LogType::Force, "Failed to request IAS token");
		cemu_assert_debug(false);
		m_loginState.store(LoginState::Failed);
		setStatusMessage(_tr("Login failed. Outdated or incomplete online files?"), DLMGR_STATUS_CODE::FAILED);
		return;
	}
	if (!CheckAccountStatus())
	{
		m_loginState.store(LoginState::Failed);
		setStatusMessage(_tr("Failed to query account status"), DLMGR_STATUS_CODE::FAILED);
		return;
	}

	setStatusMessage(_tr("Updating ticket cache"), DLMGR_STATUS_CODE::CONNECTING);
	LoadTickets();
	if (!SyncTickets())
	{
		m_loginState.store(LoginState::Failed);
		setStatusMessage(_tr("Failed to request tickets"), DLMGR_STATUS_CODE::FAILED);
		return;
	}
	FindPartialDownloads();
	ReportTitles();
	m_loginState.store(LoginState::Succeeded);
}

void DownloadManager::connect(
	std::string_view nnidAccountName,
	const std::array<uint8, 32>& passwordHash,
	CafeConsoleRegion region,
	std::string_view country,
	uint32 deviceId,
	std::string_view serial,
	std::string_view deviceCertBase64)
{
	StartWorker();
	m_login.accountName = nnidAccountName;
	m_login.cacheName = nnidAccountName.empty() ? std::string("DefaultName") : std::string(nnidAccountName);
	m_login.passwordHash = passwordHash;
	m_login.region = region;
	m_login.country = country;
	m_login.deviceCertBase64 = deviceCertBase64;
	m_login.deviceId = deviceId;
	m_login.serial = serial;
	m_loginState.store(LoginState::Requested);
	m_jobs.push([this]() { RunLogin(); });
}

bool DownloadManager::IsConnected() const
{
	return m_loginState.load() != LoginState::Idle;
}

/* ---------------------------------------------------------------- ticket cache */

void DownloadManager::AddTicket(CachedTicket::Origin origin, uint64 ticketId, uint32 ticketVersion, const std::vector<uint8>& data, const std::vector<std::vector<uint8>>& certs)
{
	CachedTicket t{origin, ticketId, ticketVersion, (uint64)-1, data, certs};
	NCrypto::ETicketParser parser;
	if (parser.parse(t.data.data(), t.data.size()))
	{
		cemu_assert_debug(!parser.IsPersonalized()); // only depersonalized tickets are kept
		cemu_assert_debug(ticketId == parser.GetTicketId());
		t.titleId = parser.GetTitleId();
		t.ticketVersion = parser.GetTicketVersion();
	}
	m_tickets.emplace_back(std::move(t));
}

DownloadManager::CachedTicket* DownloadManager::TicketById(uint64 ticketId)
{
	auto it = std::find_if(m_tickets.begin(), m_tickets.end(), [ticketId](const CachedTicket& t) { return t.ticketId == ticketId; });
	return it == m_tickets.end() ? nullptr : &*it;
}

DownloadManager::CachedTicket* DownloadManager::TicketFor(uint64 titleId)
{
	auto it = std::find_if(m_tickets.begin(), m_tickets.end(), [titleId](const CachedTicket& t) { return t.titleId == titleId; });
	return it == m_tickets.end() ? nullptr : &*it;
}

DownloadManager::CachedTicket* DownloadManager::TicketFor(uint64 titleId, uint16 version)
{
	auto it = std::find_if(m_tickets.begin(), m_tickets.end(), [titleId, version](const CachedTicket& t) { return t.titleId == titleId && t.ticketVersion == version; });
	return it == m_tickets.end() ? nullptr : &*it;
}

bool DownloadManager::HasPartialDownload(uint64 titleId, uint16 version) const
{
	return std::find(m_partialDownloads.begin(), m_partialDownloads.end(), std::make_pair(titleId, version)) != m_partialDownloads.end();
}

// cache layout: u8 version (1), u32 count, then per ticket: u8 origin, u64 ticket id,
// u32 ticket version, ticket bytes, u8 certificate count, certificates
void DownloadManager::LoadTickets()
{
	m_tickets.clear();
	std::vector<uint8> blob;
	if (!s_nupCache->GetFile({TicketCacheName(m_login.cacheName)}, blob))
		return;
	MemStreamReader r(blob.data(), blob.size());
	if (r.readBE<uint8>() != kTicketCacheVersion)
		return;
	const uint32 count = r.readBE<uint32>();
	for (uint32 n = 0; n < count; n++)
	{
		const uint8 origin = r.hasError() ? 0xFF : r.readBE<uint8>();
		if (origin != (uint8)CachedTicket::Origin::Account && origin != (uint8)CachedTicket::Origin::Public)
		{
			m_tickets.clear();
			return;
		}
		const uint64 ticketId = r.readBE<uint64>();
		const uint32 ticketVersion = r.readBE<uint32>();
		std::vector<uint8> data = r.readPODVector<uint8>();
		std::vector<std::vector<uint8>> certs;
		const uint8 certCount = r.readBE<uint8>();
		for (uint8 c = 0; c < certCount; c++)
			certs.push_back(r.readPODVector<uint8>());
		if (r.hasError())
		{
			m_tickets.clear();
			return;
		}
		AddTicket((CachedTicket::Origin)origin, ticketId, ticketVersion, data, certs);
	}
}

void DownloadManager::SaveTickets()
{
	MemStreamWriter w(32 * 1024);
	w.writeBE<uint8>(kTicketCacheVersion);
	w.writeBE<uint32>((uint32)m_tickets.size());
	for (const CachedTicket& t : m_tickets)
	{
		w.writeBE<uint8>((uint8)t.origin);
		w.writeBE<uint64>(t.ticketId);
		w.writeBE<uint32>(t.ticketVersion);
		w.writePODVector(t.data);
		w.writeBE<uint8>((uint8)t.certs.size());
		for (const auto& cert : t.certs)
			w.writePODVector(cert);
	}
	auto blob = w.getResult();
	s_nupCache->AddFileAsync({TicketCacheName(m_login.cacheName)}, blob.data(), blob.size());
}

// tickets of everything the account bought
bool DownloadManager::SyncAccountTickets()
{
	NAPI::AuthInfo auth = MakeAuth(true);
	NAPI::NAPI_ECSAccountListETicketIds_Result list = NAPI::ECS_AccountListETicketIds(auth);
	if (!list.isValid())
		return false;

	size_t done = 0;
	for (const auto& tiv : list.tivs)
	{
		setStatusMessage(CountedMessage(_tr("Downloading account ticket"), ++done, list.tivs.size()), DLMGR_STATUS_CODE::CONNECTING);
		if (CachedTicket* cached = TicketById(tiv.ticketId))
		{
			if (cached->ticketVersion == tiv.ticketVersion)
				continue;
			// the account got a newer version of this ticket
			m_tickets.erase(m_tickets.begin() + (cached - m_tickets.data()));
		}
		auto result = NAPI::ECS_AccountGetETickets(auth, tiv.ticketId);
		if (!result.isValid())
		{
			cemuLog_log(LogType::Force, "SyncTicketCache: Account ETicket invalid");
			continue;
		}
		NCrypto::ETicketParser parser;
		if (!parser.parse(result.eTickets.data(), result.eTickets.size()))
			continue;
		cemu_assert_debug(parser.GetTicketId() == (uint64)tiv.ticketId);
		cemu_assert_debug(parser.GetTicketVersion() == tiv.ticketVersion);
		if (!parser.IsPersonalized())
		{
			cemuLog_log(LogType::Force, "DownloadManager: Unexpected result. ECS ticket not personalized");
			continue;
		}
		// account tickets are bound to the console; store them depersonalized
		NCrypto::ECCPrivKey deviceKey = NCrypto::ECCPrivKey::getDeviceCertPrivateKey();
		if (!NCrypto::ETicketParser::Depersonalize(result.eTickets.data(), result.eTickets.size(), m_login.deviceId, deviceKey))
		{
			cemuLog_log(LogType::Force, "DownloadManager: Failed to depersonalize ticket");
			continue;
		}
		cemu_assert(parser.parse(result.eTickets.data(), result.eTickets.size()));
		AddTicket(CachedTicket::Origin::Account, tiv.ticketId, tiv.ticketVersion, result.eTickets, result.certs);
	}
	return true;
}

// common tickets of the system titles that can be downloaded
void DownloadManager::SyncSystemTickets()
{
	setStatusMessage(_tr("Downloading system tickets..."), DLMGR_STATUS_CODE::CONNECTING);
	NAPI::AuthInfo auth = MakeAuth(true);
	bool known;
	const std::array<uint64, 3> titles = SystemTitlesFor(m_login.region, known);
	if (!known)
		return;
	for (uint64 titleId : titles)
	{
		// todo - the version to ask for is unknown (system titles seem to use hashes), so any cached ticket will do
		if (TicketFor(titleId))
			continue;
		auto result = NAPI::NUS_GetSystemCommonETicket(auth, titleId);
		if (!result.isValid())
			continue;
		NCrypto::ETicketParser parser;
		if (!parser.parse(result.eTicket.data(), result.eTicket.size()) || parser.GetTitleId() != titleId || parser.IsPersonalized())
			continue;
		AddTicket(CachedTicket::Origin::Public, parser.GetTicketId(), parser.GetTicketVersion(), result.eTicket, result.certs);
	}
}

// common tickets for the latest update of every listed game
bool DownloadManager::SyncUpdateTickets()
{
	setStatusMessage(_tr("Retrieving update information..."), DLMGR_STATUS_CODE::CONNECTING);
	FetchVersionList();
	if (!m_haveVersionList)
		return false;
	std::vector<ListEntry> updates;
	for (const ListEntry& e : BuildDownloadList())
	{
		if (TitleIdParser(e.titleId).GetType() == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE)
			updates.push_back(e);
	}
	for (size_t i = 0; i < updates.size(); i++)
	{
		const ListEntry& e = updates[i];
		setStatusMessage(CountedMessage(_tr("Downloading ticket"), i, updates.size()), DLMGR_STATUS_CODE::CONNECTING);
		if (!e.hasLatestVersion || TicketFor(e.titleId, e.latestVersion))
			continue;
		auto cetk = NAPI::CCS_GetCETK(Service(), e.titleId, e.latestVersion);
		if (!cetk.isValid)
			continue;
		NCrypto::ETicketParser parser;
		if (!parser.parse(cetk.cetkData.data(), cetk.cetkData.size()) || parser.GetTitleId() != e.titleId)
			continue;
		const uint16 ticketVersion = parser.GetTicketVersion();
		if (ticketVersion != e.latestVersion)
		{
			cemuLog_log(LogType::Force, "Ticket for title update has a mismatching version");
			continue;
		}
		AddTicket(CachedTicket::Origin::Public, parser.GetTicketId(), ticketVersion, cetk.cetkData, {});
	}
	return true;
}

bool DownloadManager::SyncTickets()
{
	if (!SyncAccountTickets())
		return false;
	SyncSystemTickets();
	SyncUpdateTickets();
	SaveTickets();
	// names for everything that could be downloaded (only fetched when not cached on disk)
	for (size_t i = 0; i < m_tickets.size(); i++)
	{
		setStatusMessage(CountedMessage(_tr("Downloading meta data"), i + 1, m_tickets.size()), DLMGR_STATUS_CODE::CONNECTING);
		prepareIDBE(m_tickets[i].titleId);
	}
	setStatusMessage(_tr("Connected. Right click entries in the list to start downloading"), DLMGR_STATUS_CODE::CONNECTED);
	return true;
}

// download folders left behind by an earlier session
void DownloadManager::FindPartialDownloads()
{
	const fs::path packages = ActiveSettings::GetMlcPath("usr/packages/title/");
	if (!fs::exists(packages))
		return;
	for (const auto& dirEntry : fs::directory_iterator(packages))
	{
		const std::string folderName = dirEntry.path().filename().generic_string();
		uint64 titleId;
		uint32 version;
		if (sscanf(folderName.c_str(), "cemu_%" SCNx64 "_v%u", &titleId, &version) != 2)
			continue;
		std::unique_lock<std::recursive_mutex> lock(m_mutex);
		if (!m_tickets.empty())
			m_partialDownloads.emplace_back(titleId, (uint16)version);
	}
}

/* ---------------------------------------------------------------- downloads */

DownloadManager::Download::Download(uint64 titleId, uint16 version, const std::vector<uint8>& ticketData)
	: titleId(titleId), version(version), ticket(ticketData)
{
	NCrypto::ETicketParser parser;
	cemu_assert(parser.parse(ticket.data(), ticket.size()));
	parser.GetTitleKey(titleKey);
}

DownloadManager::Download::~Download()
{
	delete tmd;
}

DownloadManager::Download::Content::Content(const NCrypto::TMDParser::ContentEntry& entry)
	: index(entry.index), contentId(entry.contentId), size(entry.size), flags(entry.contentFlags)
{
	memcpy(hash, entry.hash32, sizeof(hash));
	const bool hashedLayout = HAS_FLAG(flags, NCrypto::TMDParser::TMDContentFlags::FLAG_HASHED_CONTENT);
	const uint64 block = hashedLayout ? 0x10000 : 0x10; // e.g. a 0x8001 byte non-hashed content arrives as 0x8010 bytes
	paddedSize = (size + block - 1) / block * block;
}

void DownloadManager::initiateDownload(uint64 titleId, uint16 version)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	if (Download* existing = FindDownload(titleId, version))
	{
		// resume or retry
		existing->paused = false;
		if (existing->failed)
		{
			existing->failed = false;
			Report(existing);
		}
		StartNextIfIdle();
		return;
	}
	// any ticket of the title will do, the TMD decides what gets downloaded
	CachedTicket* ticket = TicketFor(titleId);
	if (!ticket)
		return;
	Download* d = new Download(titleId, version, ticket->data);
	m_downloads.push_back(d);
	Report(d);
	StartNextIfIdle();
}

void DownloadManager::pauseDownload(uint64 titleId, uint16 version)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	Download* d = FindDownload(titleId, version);
	if (!d || !d->active)
		return;
	d->paused = true;
	d->active = false;
	Report(d);
	StartNextIfIdle();
}

DownloadManager::Download* DownloadManager::FindDownload(uint64 titleId, uint16 version)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	for (Download* d : m_downloads)
	{
		if (d->titleId == titleId && d->version == version)
			return d;
	}
	return nullptr;
}

fs::path DownloadManager::DownloadFolder(const Download* d) const
{
	return ActiveSettings::GetMlcPath(fmt::format("usr/packages/title/cemu_{:016x}_v{}/", d->titleId, d->version));
}

fs::path DownloadManager::InstallFolder(const Download* d) const
{
	const char* root = TitleIdParser(d->titleId).IsSystemTitle() ? "sys/title/" : "usr/title/";
	return ActiveSettings::GetMlcPath(fmt::format("{}{:08x}/{:08x}/", root, (uint32)(d->titleId >> 32), (uint32)d->titleId));
}

// Moves the active download along: fetch the TMD, then check -> download -> verify every
// content file (at most two tasks per step at a time), then install. Called whenever a
// download becomes active and whenever one of its tasks finishes.
void DownloadManager::Advance(Download* d)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	if (!d->active || d->paused || d->failed)
		return;
	if (!d->tmd)
	{
		if (!d->fetchingTmd)
		{
			d->fetchingTmd = true;
			ThreadPool::FireAndForget(&DownloadManager::TaskFetchTmd, this, d);
		}
		return;
	}

	using Step = Download::Content::Step;
	struct StepCount
	{
		uint32 waiting{}; // files at this step
		uint32 busy{}; // of which a task is working on
	};
	enum_array<Step, StepCount> counts;
	for (auto& [index, content] : d->contents)
	{
		counts[content.step].waiting++;
		if (content.busy)
			counts[content.step].busy++;
	}
	auto nextIdle = [d](Step step) -> Download::Content* {
		for (auto& [index, content] : d->contents)
		{
			if (content.step == step && !content.busy)
				return &content;
		}
		return nullptr;
	};
	constexpr uint32 kParallelTasks = 2;
	auto enterPhase = [&](Download::Phase phase, uint32 progressMax) {
		d->phase = phase;
		d->progress = 0;
		d->progressMax = progressMax;
		Report(d);
	};
	const uint32 fileCount = (uint32)d->contents.size();

	// files left by an earlier attempt
	if (d->phase == Download::Phase::New)
		enterPhase(Download::Phase::Checking, fileCount);
	while (counts[Step::Check].waiting > 0 && counts[Step::Check].busy < kParallelTasks)
	{
		Download::Content* c = nextIdle(Step::Check);
		if (!c)
			break;
		c->busy = true;
		counts[Step::Check].busy++;
		ThreadPool::FireAndForget(&DownloadManager::TaskVerifyContent, this, d, c->index);
	}
	if (counts[Step::Check].waiting > 0)
		return;

	// missing or broken files
	while (counts[Step::Download].waiting > 0 && counts[Step::Download].busy < kParallelTasks)
	{
		if (d->phase == Download::Phase::Checking || d->phase == Download::Phase::Verifying)
			enterPhase(Download::Phase::Downloading, 0);
		Download::Content* c = nextIdle(Step::Download);
		if (!c)
			break;
		c->busy = true;
		counts[Step::Download].busy++;
		ThreadPool::FireAndForget(&DownloadManager::TaskFetchContent, this, d, c->index);
	}
	if (counts[Step::Download].waiting > 0)
		return;

	// freshly downloaded files (a failed check sends the file back to the download step)
	if (d->phase != Download::Phase::Verifying)
		enterPhase(Download::Phase::Verifying, fileCount);
	while (counts[Step::Verify].waiting > 0 && counts[Step::Verify].busy < kParallelTasks)
	{
		Download::Content* c = nextIdle(Step::Verify);
		if (!c)
			break;
		c->busy = true;
		counts[Step::Verify].busy++;
		ThreadPool::FireAndForget(&DownloadManager::TaskVerifyContent, this, d, c->index);
	}
	if (counts[Step::Verify].waiting > 0)
		return;

	// everything verified
	if (!d->installing)
	{
		if (d->phase != Download::Phase::Installing)
			enterPhase(Download::Phase::Installing, 0);
		d->installing = true;
		ThreadPool::FireAndForget(&DownloadManager::TaskInstall, this, d);
	}
}

// only one download is worked on at a time; start the first waiting one if none is active
void DownloadManager::StartNextIfIdle()
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	for (Download* d : m_downloads)
	{
		if (d->active)
			return;
	}
	for (Download* d : m_downloads)
	{
		if (!d->failed && !d->paused && d->phase != Download::Phase::Installed)
		{
			d->active = true;
			Advance(d);
			Report(d);
			return;
		}
	}
}

// the first error of a download is the one that is shown
void DownloadManager::Fail(Download* d, std::string message)
{
	d->active = false;
	if (d->failed)
		return;
	d->failed = true;
	d->error = std::move(message);
	Report(d);
}

void DownloadManager::Report(Download* d)
{
	if (!m_cbAddDownloadableTitle)
		return;
	using STATUS = DlMgrTitleReport::STATUS;
	using Phase = Download::Phase;
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	STATUS status = STATUS::INITIALIZING;
	if (d->failed)
		status = STATUS::HAS_ERROR;
	else if (d->phase == Phase::Installed)
		status = STATUS::INSTALLED;
	else if (!d->active)
		status = d->tmd ? STATUS::PAUSED : STATUS::QUEUED;
	else if (d->tmd)
	{
		switch (d->phase)
		{
		case Phase::Checking: status = STATUS::CHECKING; break;
		case Phase::Verifying: status = STATUS::VERIFYING; break;
		case Phase::Installing: status = STATUS::INSTALLING; break;
		default: status = STATUS::DOWNLOADING; break;
		}
	}
	DlMgrTitleReport report(status, d->titleId, d->version, getNameFromCachedIDBE(d->titleId), d->progress, d->progressMax, d->paused);
	if (d->failed)
		report.errorMsg = d->error;
	lock.unlock();
	m_cbAddDownloadableTitle(report);
}

void DownloadManager::SetProgress(Download* d, uint32 progress)
{
	// todo - rate limit, this reports on every change
	d->progress = progress;
	Report(d);
}

// download progress in per mille of all content bytes
void DownloadManager::UpdateDownloadProgress(Download* d)
{
	if (d->phase != Download::Phase::Downloading)
		return;
	uint64 total = 0;
	uint64 received = 0;
	for (auto& [index, c] : d->contents)
	{
		total += c.paddedSize;
		const bool complete = c.step == Download::Content::Step::Verify || c.step == Download::Content::Step::Install;
		received += complete ? c.paddedSize : c.bytesReceived;
	}
	const uint32 perMille = (uint32)(received * 1000ull / total);
	if (d->progress == perMille)
		return;
	d->progress = perMille;
	d->progressMax = 1000;
	SetProgress(d, d->progress);
}

void DownloadManager::TaskFetchTmd(Download* d)
{
	NAPI::AuthInfo auth = MakeAuth(true);
	NAPI::NAPI_CCSGetTMD_Result result;
	// DLC: always the latest TMD (neither the ticket nor the version list tells the version)
	if (TitleIdParser(d->titleId).GetType() == TitleIdParser::TITLE_TYPE::AOC)
		result = NAPI::CCS_GetTMD(auth, d->titleId);
	else
		result = NAPI::CCS_GetTMD(auth, d->titleId, d->version);
	if (!result.isValid)
		result = NAPI::CCS_GetTMD(auth, d->titleId); // fall back to the latest one

	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	if (!result.isValid)
	{
		Fail(d, _tr("TMD download failed"));
		d->fetchingTmd = false;
		return;
	}
	lock.unlock();
	NCrypto::TMDParser parser;
	if (!parser.parse(result.tmdData.data(), result.tmdData.size()))
	{
		lock.lock();
		Fail(d, _tr("Invalid TMD"));
		d->fetchingTmd = false;
		return;
	}

	lock.lock();
	d->tmdData = result.tmdData;
	d->tmd = new NCrypto::TMDParser(parser);
	d->fetchingTmd = false;
	d->contents.clear();
	for (const auto& entry : d->tmd->GetContentList())
	{
		if (!d->contents.try_emplace(entry.index, entry).second)
			cemu_assert_debug(false); // index listed twice
	}
	fs::create_directories(DownloadFolder(d));
	Report(d);
	Advance(d);
}

// streams one .app file from the content server into the download folder
void DownloadManager::TaskFetchContent(Download* d, uint16 index)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	auto it = d->contents.find(index);
	cemu_assert(it != d->contents.end());
	Download::Content* content = &it->second;
	const uint32 contentId = content->contentId;
	content->bytesReceived = 0;
	const fs::path folder = DownloadFolder(d);
	lock.unlock();

	struct Receiver
	{
		DownloadManager* manager;
		Download* download;
		Download::Content* content;
		std::unique_ptr<FileStream> file;
		std::vector<uint8> pending;

		// data is collected and written in pieces of at least 128 KiB
		static bool OnData(void* userData, const void* data, size_t length, bool isLast)
		{
			Receiver& self = *(Receiver*)userData;
			const uint8* bytes = (const uint8*)data;
			self.pending.insert(self.pending.end(), bytes, bytes + length);
			const bool flush = self.pending.size() >= 128 * 1024 || (isLast && !self.pending.empty());
			if (!flush)
				return true;
			const size_t count = self.pending.size();
			if ((uint32)self.file->writeData(self.pending.data(), (sint32)count) != (uint32)count)
			{
				self.manager->Fail(self.download, _tr("Cannot write file. Disk full?"));
				return false;
			}
			self.pending.clear();
			std::unique_lock<std::recursive_mutex> progressLock(self.manager->m_mutex);
			self.content->bytesReceived += count;
			self.manager->UpdateDownloadProgress(self.download);
			return true;
		}
	};
	Receiver receiver{this, d, content, std::unique_ptr<FileStream>(FileStream::createFile2(folder / fmt::format("{:08x}.app", contentId))), {}};
	if (!receiver.file)
	{
		Fail(d, _tr("Cannot create file"));
		return;
	}
	const bool received = NAPI::CCS_GetContentFile(Service(), d->titleId, contentId, &Receiver::OnData, &receiver);
	receiver.file.reset();
	if (!received)
	{
		Fail(d, _tr("Download failed"));
		return;
	}
	lock.lock();
	content->Finish(Download::Content::Step::Verify);
	lock.unlock();
	Advance(d);
}

// Checks a content file against the TMD hash. Good files wait for the install step, missing
// or bad ones go (back) to the download step.
void DownloadManager::TaskVerifyContent(Download* d, uint16 index)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	auto it = d->contents.find(index);
	cemu_assert(it != d->contents.end());
	Download::Content& content = it->second;
	const uint16 contentIndex = content.index;
	const uint32 contentId = content.contentId;
	const uint64 size = content.size;
	const uint64 paddedSize = content.paddedSize;
	const auto flags = content.flags;
	uint8 expectedHash[32];
	memcpy(expectedHash, content.hash, sizeof(expectedHash));
	const fs::path folder = DownloadFolder(d);
	lock.unlock();
	NCrypto::AesKey key = d->titleKey;

	bool valid = false;
	std::unique_ptr<FileStream> file(FileStream::openFile2(folder / fmt::format("{:08x}.app", contentId)));
	if (file)
	{
		const bool sha1 = HAS_FLAG(flags, NCrypto::TMDParser::TMDContentFlags::FLAG_SHA1);
		if (HAS_FLAG(flags, NCrypto::TMDParser::TMDContentFlags::FLAG_HASHED_CONTENT))
			valid = FSTVerifier::VerifyHashedContentFile(file.get(), &key, contentIndex, size, paddedSize, sha1, expectedHash);
		else
			valid = FSTVerifier::VerifyContentFile(file.get(), &key, contentIndex, size, paddedSize, sha1, expectedHash);
	}

	lock.lock();
	if (valid)
	{
		content.Finish(Download::Content::Step::Install);
		SetProgress(d, d->progress + 1);
	}
	else
		content.Finish(Download::Content::Step::Download);
	lock.unlock();
	Advance(d);
}

// Copies one folder of the package's file system into the install folder. Files and folders
// with the link flag are not installed; whatever an older install left under their name is
// removed.
bool DownloadManager::ExtractFolder(Download* d, FSTVolume* volume, const std::string& sourcePath, const fs::path& destination)
{
	std::error_code ec;
	fs::create_directories(destination, ec); // errors are platform specific (existing folders), ignore them
	cemu_assert_debug(sourcePath.back() == '/');
	FSTDirectoryIterator dirIt;
	if (!volume->OpenDirectoryIterator(sourcePath, dirIt))
	{
		std::unique_lock<std::recursive_mutex> lock(m_mutex);
		Fail(d, "Internal error");
		return false;
	}
	if (volume->HasLinkFlag(dirIt.GetDirHandle()))
	{
		cemu_assert_suspicious();
		return true;
	}
	FSTFileHandle node;
	while (volume->Next(dirIt, node))
	{
		const std::string_view name = volume->GetName(node);
		const bool unsafeName = name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos;
		if (unsafeName)
			continue;
		const std::string nodeSource = sourcePath + std::string(name);
		fs::path nodeDestination = destination;
		nodeDestination.append(name.begin(), name.end());

		if (volume->IsDirectory(node))
		{
			if (volume->HasLinkFlag(node))
				fs::remove_all(nodeDestination, ec);
			else
				ExtractFolder(d, volume, nodeSource + "/", nodeDestination); // a failure is already recorded in d
			continue;
		}
		if (!volume->IsFile(node))
		{
			cemu_assert_debug(false); // neither file nor folder
			continue;
		}
		if (volume->HasLinkFlag(node))
			fs::remove_all(nodeDestination, ec);
		else
		{
			std::unique_ptr<FileStream> out(FileStream::createFile2(nodeDestination));
			if (!out)
			{
				Fail(d, "Failed to create file");
				return false;
			}
			std::vector<uint8> chunk(64 * 1024);
			const uint32 fileSize = volume->GetFileSize(node);
			for (uint32 offset = 0; offset < fileSize;)
			{
				const uint32 count = std::min<uint32>(fileSize - offset, (uint32)chunk.size());
				if (volume->ReadFile(node, offset, count, chunk.data()) != count)
				{
					Fail(d, "Failed to extract data");
					return false;
				}
				if ((uint32)out->writeData(chunk.data(), (sint32)count) != count)
				{
					Fail(d, "Failed to write to file. Disk full?");
					return false;
				}
				offset += count;
			}
		}
		std::unique_lock<std::recursive_mutex> lock(m_mutex);
		SetProgress(d, d->progress + 1);
	}
	return true;
}

// Writes title.tmd and title.tik into the download folder, extracts code/, content/ and meta/
// into the title folder, then deletes the download folder.
void DownloadManager::TaskInstall(Download* d)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	const fs::path folder = DownloadFolder(d);
	const fs::path target = InstallFolder(d);
	lock.unlock();

	auto abort = [&](const char* message) {
		lock.lock();
		Fail(d, message);
		d->installing = false;
	};

	if (!WriteWholeFile(folder / "title.tmd", d->tmdData))
		return abort("Failed to write title.tmd");
	if (!WriteWholeFile(folder / "title.tik", d->ticket))
		return abort("Failed to write title.tik");
	// DLC tickets go next to the title (a real console keeps them on the SLC, which is not emulated)
	if (TitleIdParser(d->titleId).GetType() == TitleIdParser::TITLE_TYPE::AOC)
	{
		std::error_code ec;
		fs::create_directories(target, ec);
		fs::create_directories(target / "code/", ec);
		if (!WriteWholeFile(target / "code/title.tik", d->ticket))
			return abort("Failed to install title.tik");
	}

	std::unique_ptr<FSTVolume> volume(FSTVolume::OpenFromContentFolder(folder));
	if (!volume)
		return abort("Failed to extract content");
	d->progressMax = volume->GetFileCount();
	d->progress = 0;
	if (!ExtractFolder(d, volume.get(), "code/", target / "code"))
		return abort("Failed to extract code folder");
	if (!ExtractFolder(d, volume.get(), "content/", target / "content"))
		return abort("Failed to extract content folder");
	if (!ExtractFolder(d, volume.get(), "meta/", target / "meta"))
		return abort("Failed to extract meta folder");
	volume.reset();

	std::error_code ec;
	fs::remove_all(folder, ec);

	lock.lock();
	d->phase = Download::Phase::Installed;
	d->installing = false;
	d->active = false;
	CafeTitleList::AddTitleFromPath(target);
	Report(d);
	StartNextIfIdle();
	WindowSystem::RefreshGameList();
}

/* ---------------------------------------------------------------- title names (IDBE) */

// Loads the icon data of a title into memory: from the NUP cache, else from the server
// (and then stores it in the cache). Blocks while doing so.
void DownloadManager::prepareIDBE(uint64 titleId)
{
	auto remember = [titleId](const NAPI::IDBEIconDataV0& icon) {
		auto* copy = new NAPI::IDBEIconDataV0(icon);
		std::lock_guard<std::mutex> lock(s_iconMutex);
		if (!s_icons.try_emplace(titleId, copy).second)
			delete copy;
	};
	{
		std::lock_guard<std::mutex> lock(s_iconMutex);
		if (s_icons.find(titleId) != s_icons.end())
			return;
	}
	std::vector<uint8> cached;
	if (s_nupCache->GetFile({IconCacheName(titleId)}, cached) && cached.size() == sizeof(NAPI::IDBEIconDataV0))
		return remember(*(const NAPI::IDBEIconDataV0*)cached.data());
	std::optional<NAPI::IDBEIconDataV0> icon = NAPI::IDBE_Request(Service(), titleId);
	if (!icon)
		return;
	s_nupCache->AddFileAsync({IconCacheName(titleId)}, (const uint8*)&*icon, sizeof(NAPI::IDBEIconDataV0));
	remember(*icon);
}

std::string DownloadManager::getNameFromCachedIDBE(uint64 titleId)
{
	// Friend List has no IDBE
	if (IsFriendListTitle(titleId))
		return "Friend List";
	std::lock_guard<std::mutex> lock(s_iconMutex);
	auto it = s_icons.find(titleId);
	if (it != s_icons.end())
		return it->second->GetLanguageStrings(GetConfig().console_language).GetGameNameUTF8();
	return fmt::format("Title {0:016x}", titleId);
}

/* ---------------------------------------------------------------- callbacks and status */

void DownloadManager::registerCallbacks(
	void (*cbUpdateConnectStatus)(std::string statusText, DLMGR_STATUS_CODE statusCode),
	void (*cbAddDownloadableTitle)(const DlMgrTitleReport& titleInfo),
	void (*cbRemoveDownloadableTitle)(uint64 titleId, uint16 version))
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	m_cbUpdateConnectStatus = cbUpdateConnectStatus;
	m_cbAddDownloadableTitle = cbAddDownloadableTitle;
	m_cbRemoveDownloadableTitle = cbRemoveDownloadableTitle;
	if (!m_cbUpdateConnectStatus && !m_cbAddDownloadableTitle)
		return;
	// bring the new listener up to date
	if (m_cbUpdateConnectStatus)
		m_cbUpdateConnectStatus(m_statusMessage, m_statusCode);
	ReportTitles();
	for (Download* d : m_downloads)
		Report(d);
}

void DownloadManager::setStatusMessage(std::string_view msg, DLMGR_STATUS_CODE statusCode)
{
	m_statusMessage = msg;
	m_statusCode = statusCode;
	if (m_cbUpdateConnectStatus)
		m_cbUpdateConnectStatus(m_statusMessage, statusCode);
}

bool DownloadManager::hasActiveDownloads()
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	return std::any_of(m_downloads.begin(), m_downloads.end(), [](const Download* d) { return d->active && !d->failed && d->phase != Download::Phase::Installed; });
}

void DownloadManager::reset()
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	// the records are not freed: tasks that are still running may hold them
	m_downloads.clear();
	m_statusCode = DLMGR_STATUS_CODE::UNINITIALIZED;
	m_statusMessage.clear();
}

/* ---------------------------------------------------------------- manager thread */

void DownloadManager::WorkerLoop()
{
	for (;;)
	{
		std::function<void()> job = m_jobs.pop();
		job();
	}
}

// first call: opens the NUP cache and starts the manager thread
void DownloadManager::StartWorker()
{
	bool expected = false;
	if (!m_workerStarted.compare_exchange_strong(expected, true))
		return;
	fs::path cacheFile = ActiveSettings::GetMlcPath("usr/save/system/nim/nup/");
	fs::create_directories(cacheFile);
	cacheFile /= "cemu_cache.dat";
	s_nupCache = FileCache::Open(cacheFile, true);
	std::thread(&DownloadManager::WorkerLoop, this).detach();
}
