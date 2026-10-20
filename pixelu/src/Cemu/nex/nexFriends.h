// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Data structures and session object of the Wii U friend service (NEX protocol 0x66).

#ifndef FFL_SIZE
#define FFL_SIZE	0x60
#endif

class nexGameKey : public nexType
{
public:
	nexGameKey() = default;
	nexGameKey(uint64 titleId, uint16 ukn) : titleId(titleId), ukn(ukn) {}
	nexGameKey(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeU64(titleId);
		pb->writeU16(ukn);
	}

	void readData(nexPacketBuffer* pb) override
	{
		titleId = pb->readU64();
		ukn = pb->readU16();
	}

public:
	uint64 titleId{};
	uint16 ukn{}; // title version
};

class nexMiiV2 : public nexType
{
public:
	nexMiiV2() = default;

	nexMiiV2(const char* miiNickname, uint8* miiData)
	{
		strncpy(this->miiNickname, miiNickname, sizeof(this->miiNickname) - 1);
		this->miiNickname[sizeof(this->miiNickname) - 1] = '\0';
		memcpy(this->miiData, miiData, FFL_SIZE);
	}

	nexMiiV2(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeString(miiNickname);
		pb->writeU8(0);
		pb->writeU8(0);
		pb->writeBuffer(miiData, FFL_SIZE);
		pb->writeU64(0);
	}

	void readData(nexPacketBuffer* pb) override
	{
		pb->readString(miiNickname, sizeof(miiNickname));
		pb->readU8();
		pb->readU8();
		memset(miiData, 0, sizeof(miiData));
		pb->readBuffer(miiData, FFL_SIZE);
		pb->readU64();
	}

public:
	uint8 miiData[FFL_SIZE]{};
	char miiNickname[128]{};
};

class nexPresenceV2 : public nexType
{
public:
	nexPresenceV2() = default;
	nexPresenceV2(int) {} // empty presence
	nexPresenceV2(nexPacketBuffer* pb) { readData(pb); }

	const char* getMetaName() override
	{
		return "NintendoPresenceV2";
	}

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeU32(1);
		pb->writeU8(isOnline ? 1 : 0);
		gameKey.writeData(pb);
		pb->writeU8(1);
		pb->writeString("");
		pb->writeU32(joinFlagMask);
		pb->writeU8(joinAvailability);
		pb->writeU32(gameId);
		pb->writeU32(gameMode);
		pb->writeU32(hostPid);
		pb->writeU32(groupId);
		pb->writeBuffer(appSpecificData, sizeof(appSpecificData));
		pb->writeU8(3);
		pb->writeU8(1);
		pb->writeU8(3);
	}

	void readData(nexPacketBuffer* pb) override
	{
		ukn0 = pb->readU32();
		isOnline = pb->readU8();
		gameKey.readData(pb);
		ukn3 = pb->readU8();
		msg = nexReadBoundedString(pb, 1024);
		joinFlagMask = pb->readU32();
		joinAvailability = pb->readU8();
		gameId = pb->readU32();
		gameMode = pb->readU32();
		hostPid = pb->readU32();
		groupId = pb->readU32();
		pb->readBuffer(appSpecificData, sizeof(appSpecificData));
		ukn11 = pb->readU8();
		ukn12 = pb->readU8();
		ukn13 = pb->readU8();
	}

public:
	uint32 ukn0{}; // presence kind; 0 when offline
	uint8 isOnline{};
	nexGameKey gameKey;
	uint8 ukn3{};
	std::string msg;
	uint32 joinFlagMask{};
	uint8 joinAvailability{};
	uint32 gameId{};
	uint32 gameMode{};
	uint32 hostPid{};
	uint32 groupId{};
	uint8 appSpecificData[0x14]{};
	uint8 ukn11{};
	uint8 ukn12{};
	uint8 ukn13{};
};

class nexPrincipalBasicInfo : public nexType
{
public:
	nexPrincipalBasicInfo() = default;

	nexPrincipalBasicInfo(uint32 principalId, char* nnid, const nexMiiV2& mii)
		: principalId(principalId), mii(mii)
	{
		strncpy(this->nnid, nnid, sizeof(this->nnid) - 1);
		this->nnid[sizeof(this->nnid) - 1] = '\0';
	}

	nexPrincipalBasicInfo(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeU32(principalId);
		pb->writeString(nnid);
		mii.writeData(pb);
		pb->writeU8(2);
	}

	void readData(nexPacketBuffer* pb) override
	{
		principalId = pb->readU32();
		pb->readString(nnid, sizeof(nnid));
		mii.readData(pb);
		regionGuessed = pb->readU8();
	}

public:
	uint32 principalId{};
	char nnid[32]{};
	nexMiiV2 mii;
	uint8 regionGuessed{};
};

class nexNNAInfo : public nexType
{
public:
	nexNNAInfo() = default;

	nexNNAInfo(uint8 countryCode, uint8 countrySubCode, const nexPrincipalBasicInfo& principalBasicInfo)
		: countryCode(countryCode), countrySubCode(countrySubCode), principalInfo(principalBasicInfo) {}

	nexNNAInfo(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		principalInfo.writeData(pb);
		pb->writeU8(countryCode);
		pb->writeU8(countrySubCode);
	}

	void readData(nexPacketBuffer* pb) override
	{
		principalInfo.readData(pb);
		countryCode = pb->readU8();
		countrySubCode = pb->readU8();
	}

public:
	uint8 countryCode{};
	uint8 countrySubCode{};
	nexPrincipalBasicInfo principalInfo;
};

class nexPrincipalPreference : public nexType
{
public:
	nexPrincipalPreference() = default;

	nexPrincipalPreference(uint8 showOnline, uint8 showGame, uint8 blockFriendRequests)
		: showOnline(showOnline), showGame(showGame), blockFriendRequests(blockFriendRequests) {}

	nexPrincipalPreference(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeU8(showOnline);
		pb->writeU8(showGame);
		pb->writeU8(blockFriendRequests);
	}

	void readData(nexPacketBuffer* pb) override
	{
		showOnline = pb->readU8();
		showGame = pb->readU8();
		blockFriendRequests = pb->readU8();
	}

public:
	uint8 showOnline{};
	uint8 showGame{};
	uint8 blockFriendRequests{};
};

class nexComment : public nexType
{
public:
	nexComment() = default;
	nexComment(uint8) { cemu_assert_unimplemented(); }
	nexComment(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeU8(ukn0);
		pb->writeString(commentString.c_str());
		pb->writeU64(ukn1);
	}

	void readData(nexPacketBuffer* pb) override
	{
		ukn0 = pb->readU8();
		commentString = nexReadBoundedString(pb, 1024);
		ukn1 = pb->readU64();
	}

public:
	uint8 ukn0{}; // language
	std::string commentString;
	uint64 ukn1{}; // last change time
};

class nexFriendRequestMessage : public nexType
{
public:
	nexFriendRequestMessage() = default;
	nexFriendRequestMessage(uint8) { cemu_assert_unimplemented(); }
	nexFriendRequestMessage(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		cemu_assert_unimplemented();
	}

	void readData(nexPacketBuffer* pb) override
	{
		messageId = pb->readU64();
		isMarkedAsReceived = pb->readU8();
		ukn2 = pb->readU8();
		commentStr = nexReadBoundedString(pb, 1024);
		ukn4 = pb->readU8();
		ukn5Str = nexReadBoundedString(pb, 1024);
		gameKey.readData(pb);
		ukn7 = pb->readU64();
		expireTimestamp = pb->readU64();
	}

public:
	uint64 messageId{};
	uint8 isMarkedAsReceived{};
	uint8 ukn2{};
	std::string commentStr;
	uint8 ukn4{};
	std::string ukn5Str;
	nexGameKey gameKey;
	uint64 ukn7{};
	uint64 expireTimestamp{};
};

class nexFriendRequest : public nexType
{
public:
	nexFriendRequest() = default;
	nexFriendRequest(uint64, uint16) { cemu_assert_unimplemented(); }
	nexFriendRequest(nexPacketBuffer* pb) { readData(pb); }

	const char* getMetaName() override
	{
		return "FriendRequest";
	}

	void writeData(nexPacketBuffer* pb) const override
	{
		cemu_assert_unimplemented();
	}

	void readData(nexPacketBuffer* pb) override
	{
		principalInfo.readData(pb);
		message.readData(pb);
		ukn = pb->readU64();
	}

public:
	nexPrincipalBasicInfo principalInfo;
	nexFriendRequestMessage message;
	uint64 ukn{};
};

class nexFriend : public nexType
{
public:
	nexFriend() = default;
	nexFriend(nexPacketBuffer* pb) { readData(pb); }

	const char* getMetaName() override
	{
		return "FriendInfo";
	}

	void writeData(nexPacketBuffer* pb) const override
	{
		cemu_assert_unimplemented();
	}

	void readData(nexPacketBuffer* pb) override
	{
		nnaInfo.readData(pb);
		presence.readData(pb);
		comment.readData(pb);
		friendsSinceTimestamp = pb->readU64();
		lastOnlineTimestamp = pb->readU64();
		ukn6 = pb->readU64();
	}

public:
	nexNNAInfo nnaInfo;
	nexPresenceV2 presence;
	nexComment comment;
	uint64 friendsSinceTimestamp{};
	uint64 lastOnlineTimestamp{};
	uint64 ukn6{};
};

class nexBlacklisted : public nexType
{
public:
	nexBlacklisted() = default;
	nexBlacklisted(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		cemu_assert_unimplemented();
	}

	void readData(nexPacketBuffer* pb) override
	{
		basicInfo.readData(pb);
		gameKey.readData(pb);
		ukn = pb->readU64();
	}

public:
	nexPrincipalBasicInfo basicInfo;
	nexGameKey gameKey;
	uint64 ukn{};
};

class nexPersistentNotification : public nexType
{
public:
	nexPersistentNotification() = default;
	nexPersistentNotification(nexPacketBuffer* pb) { readData(pb); }

	void writeData(nexPacketBuffer* pb) const override
	{
		cemu_assert_unimplemented();
	}

	void readData(nexPacketBuffer* pb) override
	{
		messageId = pb->readU64();
		pid1 = pb->readU32();
		pid2 = pb->readU32();
		type = pb->readU32();
		pb->readStdString(msg);
	}

public:
	uint64 messageId{};
	uint32 pid1{};
	uint32 pid2{};
	uint32 type{};
	std::string msg;
};

class NexFriends
{
public:
	using RpcErrorCode = int;

	static const int ERR_NONE = 0;
	static const int ERR_RPC_FAILED = 1;
	static const int ERR_UNEXPECTED_RESULT = 2;
	static const int ERR_NOT_CONNECTED = 3;

	// values are visible to games through the fpd notification mask
	enum NOTIFICATION_TYPE
	{
		NOTIFICATION_TYPE_ONLINE = 0,
		NOTIFICATION_TYPE_FRIEND_LOGIN = 4,
		NOTIFICATION_TYPE_FRIEND_LOGOFF = 5,
		NOTIFICATION_TYPE_FRIEND_PRESENCE_CHANGE = 6,
		NOTIFICATION_TYPE_ADDED_FRIEND = 9,
		NOTIFICATION_TYPE_REMOVED_FRIEND = 10,
		NOTIFICATION_TYPE_ADDED_OUTGOING_REQUEST = 11,
		NOTIFICATION_TYPE_REMOVED_OUTGOING_REQUEST = 12,
		NOTIFICATION_TYPE_ADDED_INCOMING_REQUEST = 17,
		NOTIFICATION_TYPE_REMOVED_INCOMING_REQUEST = 18
	};

public:
	NexFriends(uint32 authServerIp, uint16 authServerPort, const char* accessKey, uint32 pid, const char* nexPassword, const char* nexToken, const char* nnid, uint8* miiData, const uint16* miiNickname, uint8 countryCode, nexPresenceV2& myPresence);
	~NexFriends();

	std::string getAccountNameByPid(uint32 principalId);
	int getPendingFriendRequestCount();

	bool requestGetAllInformation();
	bool addProvisionalFriendByPidGuessed(uint32 principalId);

	// answered from the local copy of the friend lists
	bool requestGetAllInformation(std::function<void(uint32)> cb);
	void getFriendPIDs(uint32* pidList, uint32* pidCount, sint32 offset, sint32 count, bool includeFriendRequests);
	void getFriendRequestPIDs(uint32* pidList, uint32* pidCount, sint32 offset, sint32 count, bool includeIncoming, bool includeOutgoing);
	bool getFriendByPID(nexFriend& friendData, uint32 pid);
	bool getFriendRequestByPID(nexFriendRequest& friendRequestData, bool* isIncoming, uint32 searchedPid);
	bool getFriendRequestByMessageId(nexFriendRequest& friendRequestData, bool* isIncoming, uint64 messageId);
	bool isOnline();
	void getMyPreference(nexPrincipalPreference& preference);
	void getMyComment(nexComment& comment);

	// sent to the server, cb runs once the server answered
	bool addProvisionalFriend(char* name, std::function<void(RpcErrorCode)> cb);
	void addFriendRequest(uint32 pid, const char* comment, std::function<void(RpcErrorCode)> cb);
	void requestPrincipleBaseInfoByPID(uint32* pidList, sint32 count, const std::function<void(RpcErrorCode result, std::span<nexPrincipalBasicInfo> basicInfo)>& cb);
	void removeFriend(uint32 pid, std::function<void(RpcErrorCode)> cb);
	void cancelOutgoingProvisionalFriendRequest(uint32 pid, std::function<void(RpcErrorCode)> cb);
	void markFriendRequestsAsReceived(uint64* messageIdList, sint32 count, std::function<void(RpcErrorCode)> cb);
	void acceptFriendRequest(uint64 messageId, std::function<void(RpcErrorCode)> cb);
	void deleteFriendRequest(uint64 messageId, std::function<void(RpcErrorCode)> cb);
	bool updatePreferencesAsync(const nexPrincipalPreference newPreferences, std::function<void(RpcErrorCode)> cb);
	bool updateCommentAsync(const nexComment newComment, std::function<void(RpcErrorCode)> cb);
	void updateMyPresence(nexPresenceV2& myPresence);

	void setNotificationHandler(void(*notificationHandler)(NOTIFICATION_TYPE notificationType, uint32 pid));

	void update();

	void processServerNotification(uint32 notificationType, uint32 pid, nexPacketBuffer* notificationData);

private:
	struct Lists
	{
		std::vector<nexFriend> friends;
		std::vector<nexFriendRequest> outgoing;
		std::vector<nexFriendRequest> incoming;
	};

	void LoginThread();
	void StartLogin();
	bool IsConnectedToServer() const;
	void SendGetAllInformation(const nexPresenceV2& presence, std::function<void(uint32)> cb);
	void OnAllInformation(nexServiceResponse_t* response, const std::function<void(uint32)>& cb);
	void Notify(NOTIFICATION_TYPE notificationType, uint32 pid);
	void NotifyListChanges(const Lists& before);
	void CallFriendMethod(uint32 methodId, nexPacketBuffer& params, std::function<void(RpcErrorCode)> cb, bool refreshAfterSuccess);

	void OnFriendOffline(uint32 pid);
	void OnPresenceChange(uint32 pid, const nexPresenceV2& presence);
	void OnRemoved(uint32 pid);
	void OnIncomingRequest(uint32 pid, const nexFriendRequest& friendRequest);
	void OnFriendAdded(uint32 pid, const nexFriend& frd);

private:
	void(*m_notificationHandler)(NOTIFICATION_TYPE notificationType, uint32 pid){nullptr};
	bool m_connected{false};
	bool m_hasData{false}; // set once the first full information response arrived
	bool m_firstInfoRequest{false};
	nexService* m_service{nullptr};

	// identity
	uint8 m_miiData[FFL_SIZE];
	std::string m_miiNickname;
	char m_nnid[96];
	uint32 m_pid;
	uint8 m_countryCode;

	// login state
	std::recursive_mutex m_loginMutex;
	bool m_loginRunning{false};
	uint32 m_lastLoginTime{0};
	uint32 m_failedLogins{0};
	uint32 m_successfulLogins{0};
	uint32 m_authIp;
	uint16 m_authPort;
	std::string m_accessKey;
	std::string m_nexPassword;
	std::string m_nexToken;

	// our own state as known to the server
	nexPresenceV2 m_myPresence;
	nexPrincipalPreference m_myPreference;
	nexComment m_myComment;

	std::recursive_mutex m_listMutex;
	Lists m_lists;
};
