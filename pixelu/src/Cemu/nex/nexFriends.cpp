// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "prudp.h"
#include "nex.h"
#include "nexFriends.h"
#include "Cafe/CafeSystem.h"
#include "util/helpers/StringHelpers.h"

namespace
{
	// methods of the friend protocol
	enum : uint32
	{
		kMethodGetAllInformation = 1,
		kMethodAddFriendByName = 3,
		kMethodRemoveFriend = 4,
		kMethodAddFriendRequest = 5,
		kMethodAcceptFriendRequest = 7,
		kMethodDeleteFriendRequest = 8,
		kMethodMarkFriendRequestsAsReceived = 10,
		kMethodUpdatePresence = 13,
		kMethodUpdateComment = 15,
		kMethodUpdatePreference = 16,
		kMethodGetBasicInfo = 17,
	};

	// the server pushes events through protocol 0x64
	constexpr uint8 kProtocolNotification = 0x64;

	// event kinds sent by the server
	enum : uint32
	{
		kEventFriendOffline = 0x0A, // coming online is reported as a presence change
		kEventPresenceChanged = 0x18,
		kEventRemoved = 0x1A, // friend removed, or incoming request withdrawn
		kEventRequestReceived = 0x1B,
		kEventFriendAdded = 0x1E, // the other side accepted our request
		kEventCommentChanged = 0x21,
	};

	constexpr uint64 kBirthdayJan1st1990 = 0x1f18420000;

	// a reconnect is attempted 8 seconds after a failure for the first three
	// failures, after that one more minute for every failure, up to 10 failures
	bool ReconnectDelay(uint32 failed, uint32 succeeded, uint32& delayMs)
	{
		if (failed < 3)
		{
			delayMs = 8 * 1000;
			return true;
		}
		if (succeeded == 0 || failed >= 10)
			return false;
		delayMs = (failed - 2) * 60 * 1000;
		return true;
	}

	template<typename T, typename TGetPid>
	auto FindByPid(std::vector<T>& list, uint32 pid, TGetPid getPid)
	{
		return std::find_if(list.begin(), list.end(), [&](const T& e) { return getPid(e) == pid; });
	}

	uint32 PidOf(const nexFriend& f) { return f.nnaInfo.principalInfo.principalId; }
	uint32 PidOf(const nexFriendRequest& r) { return r.principalInfo.principalId; }

	template<typename T>
	bool ContainsPid(const std::vector<T>& list, uint32 pid)
	{
		for (const T& e : list)
			if (PidOf(e) == pid)
				return true;
		return false;
	}

	template<typename T>
	void AppendUniquePids(std::vector<uint32>& out, const std::vector<T>& list)
	{
		for (const T& e : list)
		{
			const uint32 pid = PidOf(e);
			if (std::find(out.begin(), out.end(), pid) == out.end())
				out.push_back(pid);
		}
	}

	void CopyPidRange(const std::vector<uint32>& all, uint32* pidList, uint32* pidCount, sint32 offset, sint32 count)
	{
		sint32 n = (sint32)all.size() - offset;
		n = std::clamp(n, 0, std::max(count, 0));
		if (pidList)
			for (sint32 i = 0; i < n; i++)
				pidList[i] = all[offset + i];
		*pidCount = (uint32)n;
	}

	// reads up to count entries, stopping early once the data ran out
	template<typename T>
	void ReadList(nexPacketBuffer& pb, std::vector<T>& list, uint32 count)
	{
		list.clear();
		for (uint32 i = 0; i < count; i++)
		{
			list.emplace_back().readData(&pb);
			if (pb.hasReadOutOfBounds())
				break;
		}
	}

	template<typename T>
	void SkipList(nexPacketBuffer& pb, uint32 count)
	{
		for (uint32 i = 0; i < count && !pb.hasReadOutOfBounds(); i++)
			T skipped(&pb);
	}

	void OnNotificationRequest(nexServiceRequest_t* request)
	{
		NexFriends* session = (NexFriends*)request->custom;
		if (request->methodId != 1 && request->methodId != 2)
		{
			request->nex->sendRequestResponse(request, 0x80000000, nullptr, 0);
			return;
		}
		const uint32 eventType = request->data.readU32();
		const uint32 pid = request->data.readU32();
		if (!request->data.hasReadOutOfBounds())
		{
			cemuLog_logDebug(LogType::Force, "NN_NOTIFICATION methodId {} type {:02x} pid {:08x}", request->methodId, eventType, pid);
			session->processServerNotification(eventType, pid, &request->data);
		}
		request->nex->sendRequestResponse(request, 0, nullptr, 0);
	}
}

NexFriends::NexFriends(uint32 authServerIp, uint16 authServerPort, const char* accessKey, uint32 pid, const char* nexPassword, const char* nexToken, const char* nnid, uint8* miiData, const uint16* miiNickname, uint8 countryCode, nexPresenceV2& myPresence)
	: m_pid(pid), m_countryCode(countryCode), m_authIp(authServerIp), m_authPort(authServerPort),
	  m_accessKey(accessKey), m_nexPassword(nexPassword), m_nexToken(nexToken), m_myPresence(myPresence)
{
	memcpy(m_miiData, miiData, FFL_SIZE);
	strncpy(m_nnid, nnid, sizeof(m_nnid) - 1);
	m_nnid[sizeof(m_nnid) - 1] = '\0';
	m_miiNickname = StringHelpers::ToUtf8FromLE(miiNickname, ACT_NICKNAME_LENGTH);
	// pretend the last attempt was an hour ago so the first update() logs in right away
	m_lastLoginTime = prudpGetMSTimestamp() - 60 * 60 * 1000;
}

NexFriends::~NexFriends()
{
	if (m_service)
		m_service->destroy();
}

bool NexFriends::IsConnectedToServer() const
{
	return m_service && m_service->getState() == nexService::STATE_CONNECTED;
}

// --- login ---

void NexFriends::LoginThread()
{
	nexService* service = nex_establishSecureConnection(m_authIp, m_authPort, m_accessKey.c_str(), m_pid, m_nexPassword.c_str(), m_nexToken.c_str());
	if (!service)
	{
		m_failedLogins++;
		m_loginRunning = false;
		return;
	}
	m_service = service;
	m_service->registerProtocolRequestHandler(kProtocolNotification, OnNotificationRequest, this);
	m_service->registerForAsyncProcessing();
	m_connected = true;
	m_firstInfoRequest = true;
	m_loginRunning = false;
	m_successfulLogins++;
	requestGetAllInformation();
}

void NexFriends::StartLogin()
{
	if (m_connected || m_loginRunning)
		return;
	std::unique_lock loginLock(m_loginMutex);
	m_lastLoginTime = prudpGetMSTimestamp();
	m_loginRunning = true;
	{
		std::unique_lock listLock(m_listMutex);
		m_lists = {};
		m_hasData = false;
	}
	cemuLog_logDebug(LogType::Force, "Attempt to log into friend server...");
	std::thread(&NexFriends::LoginThread, this).detach();
}

void NexFriends::update()
{
	std::unique_lock loginLock(m_loginMutex);
	if (m_connected)
	{
		if (IsConnectedToServer())
			return;
		cemuLog_log(LogType::Force, "NEX: Lost friend server session");
		if (m_service)
		{
			m_service->destroy();
			m_service = nullptr;
		}
		m_connected = false;
		return;
	}
	if (m_loginRunning)
		return;
	uint32 delayMs;
	if (!ReconnectDelay(m_failedLogins, m_successfulLogins, delayMs))
		return;
	if ((uint32)(prudpGetMSTimestamp() - m_lastLoginTime) < delayMs)
		return;
	cemuLog_log(LogType::Force, "NEX: Attempt async friend service login");
	StartLogin();
}

bool NexFriends::isOnline()
{
	return m_connected && m_hasData;
}

// --- full state refresh ---

void NexFriends::SendGetAllInformation(const nexPresenceV2& presence, std::function<void(uint32)> cb)
{
	// failed calls are reported while the lost session is torn down, a refresh is pointless then
	if (!m_service)
		return;
	uint8 storage[1024];
	nexPacketBuffer params(storage, sizeof(storage), true);
	nexMiiV2 mii(m_miiNickname.c_str(), m_miiData);
	nexNNAInfo(m_countryCode, 0, nexPrincipalBasicInfo(m_pid, m_nnid, mii)).writeData(&params);
	presence.writeData(&params);
	params.writeU64(kBirthdayJan1st1990);
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, kMethodGetAllInformation, &params,
		[this, cb](nexServiceResponse_t* response) { OnAllInformation(response, cb); }, true);
}

bool NexFriends::requestGetAllInformation()
{
	SendGetAllInformation(m_myPresence, nullptr);
	return true;
}

bool NexFriends::requestGetAllInformation(std::function<void(uint32)> cb)
{
	SendGetAllInformation(nexPresenceV2(0), std::move(cb));
	return true;
}

void NexFriends::OnAllInformation(nexServiceResponse_t* response, const std::function<void(uint32)>& cb)
{
	if (!response->isSuccessful)
	{
		if (cb)
			cb(ERR_RPC_FAILED);
		return;
	}
	nexPacketBuffer& pb = response->data;
	m_myPreference = nexPrincipalPreference(&pb);
	m_myComment = nexComment(&pb);
	if (pb.hasReadOutOfBounds())
		return;

	std::unique_lock listLock(m_listMutex);
	const Lists before = m_lists;
	ReadList(pb, m_lists.friends, pb.readU32());
	const uint32 outgoingCount = pb.readU32();
	if (pb.hasReadOutOfBounds())
		return;
	ReadList(pb, m_lists.outgoing, outgoingCount);
	const uint32 incomingCount = pb.readU32();
	if (pb.hasReadOutOfBounds())
		return;
	ReadList(pb, m_lists.incoming, incomingCount);
	if (pb.hasReadOutOfBounds())
		return;
	// the block list and the persistent notifications are not used
	SkipList<nexBlacklisted>(pb, pb.readU32());
	pb.readU8();
	SkipList<nexPersistentNotification>(pb, pb.readU32());
	if (pb.readU8() != 0)
	{
		// the account never chose its friend settings
		cemuLog_log(LogType::Force, "NEX: First time login into friend account, setting up default preferences");
		updatePreferencesAsync(nexPrincipalPreference(1, 1, 0), [](RpcErrorCode) {});
	}
	if (m_firstInfoRequest)
		Notify(NOTIFICATION_TYPE_ONLINE, m_pid);
	else
		NotifyListChanges(before);
	m_firstInfoRequest = false;
	m_hasData = true;
	if (cb)
		cb(ERR_NONE);
}

// --- notifications ---

void NexFriends::setNotificationHandler(void(*notificationHandler)(NOTIFICATION_TYPE notificationType, uint32 pid))
{
	m_notificationHandler = notificationHandler;
}

void NexFriends::Notify(NOTIFICATION_TYPE notificationType, uint32 pid)
{
	if (m_notificationHandler)
		m_notificationHandler(notificationType, pid);
}

void NexFriends::NotifyListChanges(const Lists& before)
{
	auto diff = [this](const auto& now, const auto& old, NOTIFICATION_TYPE added, NOTIFICATION_TYPE removed) {
		for (const auto& e : now)
			if (!ContainsPid(old, PidOf(e)))
				Notify(added, PidOf(e));
		for (const auto& e : old)
			if (!ContainsPid(now, PidOf(e)))
				Notify(removed, PidOf(e));
	};
	diff(m_lists.friends, before.friends, NOTIFICATION_TYPE_ADDED_FRIEND, NOTIFICATION_TYPE_REMOVED_FRIEND);
	diff(m_lists.incoming, before.incoming, NOTIFICATION_TYPE_ADDED_INCOMING_REQUEST, NOTIFICATION_TYPE_REMOVED_INCOMING_REQUEST);
	diff(m_lists.outgoing, before.outgoing, NOTIFICATION_TYPE_ADDED_OUTGOING_REQUEST, NOTIFICATION_TYPE_REMOVED_OUTGOING_REQUEST);
}

void NexFriends::OnFriendOffline(uint32 pid)
{
	std::unique_lock listLock(m_listMutex);
	auto it = FindByPid(m_lists.friends, pid, [](const nexFriend& f) { return PidOf(f); });
	if (it == m_lists.friends.end())
		return;
	it->presence.isOnline = 0;
	Notify(NOTIFICATION_TYPE_FRIEND_LOGOFF, pid);
}

void NexFriends::OnPresenceChange(uint32 pid, const nexPresenceV2& presence)
{
	std::unique_lock listLock(m_listMutex);
	auto it = FindByPid(m_lists.friends, pid, [](const nexFriend& f) { return PidOf(f); });
	if (it == m_lists.friends.end())
		return;
	const bool onlineToggled = it->presence.isOnline != presence.isOnline;
	it->presence = presence;
	if (onlineToggled)
		Notify(presence.isOnline != 0 ? NOTIFICATION_TYPE_FRIEND_LOGIN : NOTIFICATION_TYPE_FRIEND_LOGOFF, pid);
	Notify(NOTIFICATION_TYPE_FRIEND_PRESENCE_CHANGE, pid);
}

void NexFriends::OnRemoved(uint32 pid)
{
	std::unique_lock listLock(m_listMutex);
	auto f = FindByPid(m_lists.friends, pid, [](const nexFriend& e) { return PidOf(e); });
	if (f != m_lists.friends.end())
	{
		m_lists.friends.erase(f);
		Notify(NOTIFICATION_TYPE_REMOVED_FRIEND, pid);
		return;
	}
	auto r = FindByPid(m_lists.incoming, pid, [](const nexFriendRequest& e) { return PidOf(e); });
	if (r != m_lists.incoming.end())
	{
		m_lists.incoming.erase(r);
		Notify(NOTIFICATION_TYPE_REMOVED_INCOMING_REQUEST, pid);
	}
}

void NexFriends::OnIncomingRequest(uint32 pid, const nexFriendRequest& friendRequest)
{
	std::unique_lock listLock(m_listMutex);
	if (ContainsPid(m_lists.incoming, pid))
		return;
	m_lists.incoming.push_back(friendRequest);
	Notify(NOTIFICATION_TYPE_ADDED_INCOMING_REQUEST, pid);
}

void NexFriends::OnFriendAdded(uint32 pid, const nexFriend& frd)
{
	std::unique_lock listLock(m_listMutex);
	auto in = FindByPid(m_lists.incoming, pid, [](const nexFriendRequest& e) { return PidOf(e); });
	if (in != m_lists.incoming.end())
	{
		m_lists.incoming.erase(in);
		Notify(NOTIFICATION_TYPE_REMOVED_INCOMING_REQUEST, pid);
	}
	auto out = FindByPid(m_lists.outgoing, pid, [](const nexFriendRequest& e) { return PidOf(e); });
	if (out != m_lists.outgoing.end())
	{
		m_lists.outgoing.erase(out);
		Notify(NOTIFICATION_TYPE_REMOVED_OUTGOING_REQUEST, pid);
	}
	// a second incoming request from the same pid suppresses the add
	if (ContainsPid(m_lists.incoming, pid))
		return;
	m_lists.friends.push_back(frd);
	Notify(NOTIFICATION_TYPE_ADDED_FRIEND, pid);
}

void NexFriends::processServerNotification(uint32 notificationType, uint32 pid, nexPacketBuffer* notificationData)
{
	nexNotificationEventGeneral general;
	switch (notificationType)
	{
	case kEventPresenceChanged:
	{
		nexPresenceV2 presence;
		if (notificationData->readPlaceholderType(presence))
			OnPresenceChange(pid, presence);
		break;
	}
	case kEventCommentChanged:
		notificationData->readPlaceholderType(general); // not tracked
		break;
	case kEventFriendOffline:
		if (notificationData->readPlaceholderType(general))
			OnFriendOffline(pid);
		break;
	case kEventRemoved:
		if (notificationData->readPlaceholderType(general))
			OnRemoved(pid);
		break;
	case kEventRequestReceived:
	{
		nexFriendRequest request;
		if (notificationData->readPlaceholderType(request))
			OnIncomingRequest(pid, request);
		break;
	}
	case kEventFriendAdded:
	{
		nexFriend frd;
		if (notificationData->readPlaceholderType(frd))
			OnFriendAdded(pid, frd);
		break;
	}
	default:
		cemuLog_logDebug(LogType::Force, "Unsupported friend server notification type 0x{:02x}", notificationType);
		break;
	}
}

// --- local queries ---

void NexFriends::getFriendPIDs(uint32* pidList, uint32* pidCount, sint32 offset, sint32 count, bool includeFriendRequests)
{
	if (offset < 0 || count < 0)
	{
		*pidCount = 0;
		return;
	}
	std::vector<uint32> pids;
	std::unique_lock listLock(m_listMutex);
	AppendUniquePids(pids, m_lists.friends);
	if (includeFriendRequests)
		AppendUniquePids(pids, m_lists.outgoing); // only the ones we sent
	CopyPidRange(pids, pidList, pidCount, offset, count);
}

void NexFriends::getFriendRequestPIDs(uint32* pidList, uint32* pidCount, sint32 offset, sint32 count, bool includeIncoming, bool includeOutgoing)
{
	if (offset < 0 || count < 0)
	{
		*pidCount = 0;
		return;
	}
	std::vector<uint32> pids;
	std::unique_lock listLock(m_listMutex);
	if (includeIncoming)
		AppendUniquePids(pids, m_lists.incoming);
	if (includeOutgoing)
		AppendUniquePids(pids, m_lists.outgoing);
	CopyPidRange(pids, pidList, pidCount, offset, count);
}

std::string NexFriends::getAccountNameByPid(uint32 principalId)
{
	if (principalId == m_pid)
		return m_nnid;
	nexFriend f;
	if (getFriendByPID(f, principalId))
		return f.nnaInfo.principalInfo.nnid;
	nexFriendRequest r;
	if (getFriendRequestByPID(r, nullptr, principalId))
		return r.principalInfo.nnid;
	return {};
}

int NexFriends::getPendingFriendRequestCount()
{
	std::unique_lock listLock(m_listMutex);
	return (int)m_lists.incoming.size();
}

bool NexFriends::getFriendByPID(nexFriend& friendData, uint32 searchedPid)
{
	std::unique_lock listLock(m_listMutex);
	for (const nexFriend& f : m_lists.friends)
	{
		if (PidOf(f) == searchedPid)
		{
			friendData = f;
			return true;
		}
	}
	return false;
}

bool NexFriends::getFriendRequestByPID(nexFriendRequest& friendRequestData, bool* isIncoming, uint32 searchedPid)
{
	std::unique_lock listLock(m_listMutex);
	for (bool incoming : { true, false })
	{
		for (const nexFriendRequest& r : incoming ? m_lists.incoming : m_lists.outgoing)
		{
			if (PidOf(r) != searchedPid)
				continue;
			friendRequestData = r;
			if (isIncoming)
				*isIncoming = incoming;
			return true;
		}
	}
	return false;
}

bool NexFriends::getFriendRequestByMessageId(nexFriendRequest& friendRequestData, bool* isIncoming, uint64 messageId)
{
	std::unique_lock listLock(m_listMutex);
	for (bool incoming : { true, false })
	{
		for (const nexFriendRequest& r : incoming ? m_lists.incoming : m_lists.outgoing)
		{
			if (r.message.messageId == 0 || r.message.messageId != messageId)
				continue;
			friendRequestData = r;
			*isIncoming = incoming;
			return true;
		}
	}
	return false;
}

void NexFriends::getMyPreference(nexPrincipalPreference& preference)
{
	preference = m_myPreference;
}

void NexFriends::getMyComment(nexComment& comment)
{
	comment = m_myComment;
}

// --- requests to the server ---

void NexFriends::CallFriendMethod(uint32 methodId, nexPacketBuffer& params, std::function<void(RpcErrorCode)> cb, bool refreshAfterSuccess)
{
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, methodId, &params, [this, cb, refreshAfterSuccess](nexServiceResponse_t* response) {
		if (!response->isSuccessful)
		{
			cb(ERR_RPC_FAILED);
			return;
		}
		cb(ERR_NONE);
		if (refreshAfterSuccess)
			requestGetAllInformation(); // the refresh reports the list changes
	}, true);
}

bool NexFriends::updatePreferencesAsync(const nexPrincipalPreference newPreferences, std::function<void(RpcErrorCode)> cb)
{
	if (!m_service)
	{
		cb(ERR_NOT_CONNECTED);
		return false;
	}
	uint8 storage[1024];
	nexPacketBuffer params(storage, sizeof(storage), true);
	newPreferences.writeData(&params);
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, kMethodUpdatePreference, &params, [this, cb, newPreferences](nexServiceResponse_t* response) {
		if (!response->isSuccessful)
		{
			cb(ERR_RPC_FAILED);
			return;
		}
		m_myPreference = newPreferences;
		cb(ERR_NONE);
	}, true);
	return true;
}

bool NexFriends::updateCommentAsync(const nexComment newComment, std::function<void(RpcErrorCode)> cb)
{
	if (!m_service)
	{
		cb(ERR_NOT_CONNECTED);
		return false;
	}
	uint8 storage[1024];
	nexPacketBuffer params(storage, sizeof(storage), true);
	newComment.writeData(&params);
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, kMethodUpdateComment, &params, [this, cb, newComment](nexServiceResponse_t* response) {
		if (!response->isSuccessful)
		{
			cb(ERR_RPC_FAILED);
			return;
		}
		m_myComment = newComment;
		cb(ERR_NONE);
	}, true);
	return true;
}

bool NexFriends::addProvisionalFriendByPidGuessed(uint32 principalId)
{
	cemu_assert_unimplemented();
	return true;
}

bool NexFriends::addProvisionalFriend(char* name, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return false;
	}
	uint8 storage[128];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeString(name);
	CallFriendMethod(kMethodAddFriendByName, params, std::move(cb), false);
	return true;
}

void NexFriends::addFriendRequest(uint32 pid, const char* comment, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return;
	}
	uint8 storage[2048];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU32(pid);
	params.writeU8(0);
	params.writeString(comment);
	params.writeU8(0);
	params.writeString("");
	nexGameKey(0, 0).writeData(&params);
	params.writeU64(0);
	// unlike the other calls this one refreshes the lists when it failed
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, kMethodAddFriendRequest, &params, [this, cb](nexServiceResponse_t* response) {
		if (response->isSuccessful)
		{
			cb(ERR_NONE);
			return;
		}
		cb(ERR_RPC_FAILED);
		requestGetAllInformation();
	}, true);
}

void NexFriends::requestPrincipleBaseInfoByPID(uint32* pidList, sint32 count, const std::function<void(RpcErrorCode result, std::span<nexPrincipalBasicInfo> basicInfo)>& cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED, {});
		return;
	}
	uint8 storage[512];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU32((uint32)count);
	for (sint32 i = 0; i < count; i++)
		params.writeU32(pidList[i]);
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, kMethodGetBasicInfo, &params, [cb, count](nexServiceResponse_t* response) {
		if (!response->isSuccessful)
		{
			cb(ERR_RPC_FAILED, {});
			return;
		}
		if (response->data.readU32() != (uint32)count)
		{
			cb(ERR_UNEXPECTED_RESULT, {});
			return;
		}
		std::vector<nexPrincipalBasicInfo> infos((size_t)count);
		for (auto& info : infos)
			info.readData(&response->data);
		if (response->data.hasReadOutOfBounds())
		{
			cb(ERR_UNEXPECTED_RESULT, {});
			return;
		}
		cb(ERR_NONE, infos);
	}, true);
}

void NexFriends::removeFriend(uint32 pid, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return;
	}
	uint8 storage[512];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU32(pid);
	CallFriendMethod(kMethodRemoveFriend, params, std::move(cb), true);
}

void NexFriends::cancelOutgoingProvisionalFriendRequest(uint32 pid, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return;
	}
	uint8 storage[512];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU32(pid);
	CallFriendMethod(kMethodRemoveFriend, params, std::move(cb), false);
}

void NexFriends::acceptFriendRequest(uint64 messageId, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return;
	}
	uint8 storage[128];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU64(messageId);
	CallFriendMethod(kMethodAcceptFriendRequest, params, std::move(cb), false);
}

void NexFriends::deleteFriendRequest(uint64 messageId, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return;
	}
	uint8 storage[128];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU64(messageId);
	CallFriendMethod(kMethodDeleteFriendRequest, params, std::move(cb), true);
}

void NexFriends::markFriendRequestsAsReceived(uint64* messageIdList, sint32 count, std::function<void(RpcErrorCode)> cb)
{
	if (!IsConnectedToServer())
	{
		cb(ERR_NOT_CONNECTED);
		return;
	}
	uint8 storage[1024];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeU32((uint32)count);
	for (sint32 i = 0; i < count; i++)
		params.writeU64(messageIdList[i]);
	CallFriendMethod(kMethodMarkFriendRequestsAsReceived, params, std::move(cb), false);
}

void NexFriends::updateMyPresence(nexPresenceV2& myPresence)
{
	// the stored copy keeps the caller's game key, the caller's object and
	// the server get the running title (or nothing for system titles)
	m_myPresence = myPresence;
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	if (GetTitleIdHigh(titleId) == 0x00050000)
		myPresence.gameKey = nexGameKey(titleId, CafeSystem::GetForegroundTitleVersion());
	else
		myPresence.gameKey = nexGameKey(0, 0);
	if (!IsConnectedToServer())
		return;
	uint8 storage[1024];
	nexPacketBuffer params(storage, sizeof(storage), true);
	myPresence.writeData(&params);
	m_service->callMethod(NEX_PROTOCOL_FRIENDS_WIIU, kMethodUpdatePresence, &params, [](nexServiceResponse_t*) {}, false);
}
