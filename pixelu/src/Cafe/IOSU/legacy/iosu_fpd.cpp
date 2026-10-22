// Original code for RetroPixels (PixelU). Not derived from Cemu.
// /dev/fpd: the friend service used by nn_fp. Friend data comes from a NexFriends session
// that is created on the first login request while online mode is enabled.
#include "iosu_act.h"
#include "iosu_fpd.h"
#include "Cemu/nex/nex.h"
#include "Cemu/nex/nexFriends.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Cemu/napi/napi.h"
#include "util/helpers/StringHelpers.h"
#include "Cafe/IOSU/iosu_types_common.h"
#include "Cafe/IOSU/nn/iosu_nn_service.h"
#include "Common/socket.h"
#include "Common/CafeString.h"

// on-screen friend notifications, drawn by LatteOverlay (text, duration in ms)
std::mutex g_friend_notification_mutex;
std::vector< std::pair<std::string, int> > g_friend_notifications;

namespace iosu
{
	namespace fpd
	{
		namespace
		{
			constexpr nnResult kOk = 0;
			constexpr nnResult kBadParam = BUILD_NN_RESULT(NN_RESULT_LEVEL_LVL6, NN_RESULT_MODULE_NN_FP, 0x680);
			constexpr nnResult kFailed = BUILD_NN_RESULT(NN_RESULT_LEVEL_FATAL, NN_RESULT_MODULE_NN_FP, 0);

			// ---- notification log ------------------------------------------------------------
			// Notifications from the friend session are kept for ten seconds. Every client reads
			// them in order, remembering the serial of the last one it has seen.

			struct LoggedNotification
			{
				std::chrono::steady_clock::time_point when;
				uint64 serial;
				NexFriends::NOTIFICATION_TYPE type;
				uint32 pid;
			};

			class NotificationLog
			{
			public:
				void Add(NexFriends::NOTIFICATION_TYPE type, uint32 pid)
				{
					std::lock_guard lock(m_mutex);
					m_entries.push_back({ std::chrono::steady_clock::now(), m_nextSerial++, type, pid });
				}

				void Prune()
				{
					std::lock_guard lock(m_mutex);
					auto limit = std::chrono::steady_clock::now() - std::chrono::seconds(10);
					std::erase_if(m_entries, [limit](const LoggedNotification& n) { return n.when < limit; });
				}

				// first entry newer than lastSeen; advances lastSeen past it
				std::optional<LoggedNotification> Next(uint64& lastSeen)
				{
					std::lock_guard lock(m_mutex);
					for (const LoggedNotification& n : m_entries)
					{
						if (n.serial > lastSeen)
						{
							lastSeen = n.serial;
							return n;
						}
					}
					return std::nullopt;
				}

			private:
				std::mutex m_mutex;
				std::vector<LoggedNotification> m_entries;
				uint64 m_nextSerial = 1;
			};

			NotificationLog g_notificationLog;

			// ---- friend session state --------------------------------------------------------

			NexFriends* g_session = nullptr;
			std::mutex g_sessionMutex;
			std::atomic_bool g_sessionStarted{ false };
			nexPresenceV2 g_myPresence;

			void ShowOverlayMessage(NexFriends::NOTIFICATION_TYPE type, uint32 pid)
			{
				cemuLog_logDebug(LogType::Force, "FPD: notification {:02x} pid {:08x}", type, pid);
				if (!GetConfig().notification.friends)
					return;
				std::lock_guard lock(g_friend_notification_mutex);
				if (type == NexFriends::NOTIFICATION_TYPE_ONLINE)
				{
					g_friend_notifications.emplace_back("Connected to friend service", 5000);
					if (g_session && g_session->getPendingFriendRequestCount() > 0)
						g_friend_notifications.emplace_back(fmt::format("You have {} pending friend request(s)", g_session->getPendingFriendRequestCount()), 5000);
					return;
				}
				const char* text = nullptr;
				switch (type)
				{
				case NexFriends::NOTIFICATION_TYPE_FRIEND_LOGIN: text = "{} is now online"; break;
				case NexFriends::NOTIFICATION_TYPE_FRIEND_LOGOFF: text = "{} is now offline"; break;
				case NexFriends::NOTIFICATION_TYPE_ADDED_FRIEND: text = "{} has been added to your friend list"; break;
				case NexFriends::NOTIFICATION_TYPE_REMOVED_FRIEND: text = "{} has been removed from your friend list"; break;
				case NexFriends::NOTIFICATION_TYPE_ADDED_INCOMING_REQUEST: text = "{} wants to add you to his friend list"; break;
				default: break;
				}
				if (!text)
					return;
				std::string who = fmt::format("{:#x}", pid);
				if (g_session)
				{
					std::string name = g_session->getAccountNameByPid(pid);
					if (!name.empty())
						who = name;
				}
				g_friend_notifications.emplace_back(fmt::format(fmt::runtime(text), who), 5000);
			}

			void OnSessionNotification(NexFriends::NOTIFICATION_TYPE type, uint32 pid)
			{
				ShowOverlayMessage(type, pid);
				g_notificationLog.Add(type, pid);
			}

			// ---- conversions -----------------------------------------------------------------

			// UTF-8 -> zero terminated big endian UTF-16, truncated to fit 'capacity' units
			void Utf8ToBeWide(const char* text, uint16be* out, size_t capacity)
			{
				std::vector<uint16be> wide = StringHelpers::FromUtf8(text);
				size_t n = std::min(wide.size(), capacity - 1);
				for (size_t i = 0; i < n; i++)
					out[i] = wide[i];
				out[n] = 0;
			}

			// packed timestamp: second:6 minute:6 hour:5 day:5 month:4 year:rest; 0 maps to 1970-01-01
			void UnpackTimestamp(uint64 ts, FPDDate& date)
			{
				if (ts == 0)
				{
					date.year = 1970;
					date.month = 1;
					date.day = 1;
					date.hour = 0;
					date.minute = 0;
					date.second = 0;
					return;
				}
				date.second = (uint8)(ts & 0x3F);
				date.minute = (uint8)((ts >> 6) & 0x3F);
				date.hour = (uint8)((ts >> 12) & 0x1F);
				date.day = (uint8)((ts >> 17) & 0x1F);
				date.month = (uint8)((ts >> 22) & 0xF);
				date.year = (uint16)(ts >> 26);
			}

			// NNIDs are at most 16 characters; longer input is cut instead of overflowing the field
			void CopyNnid(char (&dst)[17], const char* src)
			{
				size_t n = strnlen(src, 16);
				memcpy(dst, src, n);
				dst[n] = '\0';
			}

			void SetDate(FPDDate& d, uint16 year, uint8 month, uint8 day, uint8 hour, uint8 minute, uint8 second)
			{
				d.year = year;
				d.month = month;
				d.day = day;
				d.hour = hour;
				d.minute = minute;
				d.second = second;
			}

			void PresenceToGameMode(const nexPresenceV2& p, GameMode& gm)
			{
				memset(&gm, 0, sizeof(GameMode));
				gm.joinFlagMask = p.joinFlagMask;
				gm.matchmakeType = p.joinAvailability;
				gm.joinGameId = p.gameId;
				gm.joinGameMode = p.gameMode;
				gm.hostPid = p.hostPid;
				gm.groupId = p.groupId;
				memcpy(gm.appSpecificData, p.appSpecificData, sizeof(gm.appSpecificData));
			}

			// note: resets every other presence field (online flag, game key) as well
			void GameModeToPresence(const GameMode& gm, nexPresenceV2& p)
			{
				p = {};
				p.joinFlagMask = gm.joinFlagMask;
				p.joinAvailability = (uint8)(uint32)gm.matchmakeType;
				p.gameId = gm.joinGameId;
				p.gameMode = gm.joinGameMode;
				p.hostPid = gm.hostPid;
				p.groupId = gm.groupId;
				memcpy(p.appSpecificData, gm.appSpecificData, sizeof(gm.appSpecificData));
			}

			void FillFromFriend(const nexFriend& f, FriendData& out)
			{
				memset(&out, 0, sizeof(FriendData));
				const auto& who = f.nnaInfo.principalInfo;
				out.type = 1;
				out.pid = who.principalId;
				memcpy(out.mii, who.mii.miiData, FFL_SIZE);
				CopyNnid(out.nnid, who.nnid);
				Utf8ToBeWide(who.mii.miiNickname, out.screenname, std::size(out.screenname));
				auto& extra = out.friendExtraData;
				extra.isOnline = f.presence.isOnline != 0 ? 1 : 0;
				extra.gameKey.titleId = f.presence.gameKey.titleId;
				extra.gameKey.ukn08 = f.presence.gameKey.ukn;
				PresenceToGameMode(f.presence, extra.gameMode);
				extra.gameModeDescription.assignFromUTF8(f.presence.msg);
				extra.comment.commentString.assignFromUTF8(f.comment.commentString);
				// placeholder dates, the friend list only needs them to be valid
				SetDate(out.uknDate, 2018, 1, 1, 1, 1, 1);
				SetDate(extra.approvalTime, 2018, 1, 1, 1, 1, 1);
				UnpackTimestamp(f.lastOnlineTimestamp, extra.lastOnline);
			}

			void FillFromRequest(const nexFriendRequest& r, FriendData& out)
			{
				memset(&out, 0, sizeof(FriendData));
				const auto& who = r.principalInfo;
				out.type = 0;
				out.pid = who.principalId;
				memcpy(out.mii, who.mii.miiData, FFL_SIZE);
				CopyNnid(out.nnid, who.nnid);
				Utf8ToBeWide(who.mii.miiNickname, out.screenname, std::size(out.screenname));
				auto& extra = out.requestExtraData;
				Utf8ToBeWide(r.message.commentStr.c_str(), extra.comment, std::size(extra.comment));
				extra.messageId = r.message.messageId;
				SetDate(out.uknDate, 2018, 4, 20, 12, 1, 1);
				SetDate(extra.uknData0, 2018, 4, 24, 1, 1, 1);
				UnpackTimestamp(r.message.expireTimestamp, extra.uknData1); // "expires in"
			}

			void FillRequestEntry(const nexFriendRequest& r, FriendRequest& out)
			{
				memset(&out, 0, sizeof(FriendRequest));
				const auto& who = r.principalInfo;
				out.pid = who.principalId;
				strncpy((char*)out.nnid, who.nnid, sizeof(out.nnid));
				out.nnid[sizeof(out.nnid) - 1] = '\0';
				memcpy(out.miiData, who.mii.miiData, sizeof(out.miiData));
				Utf8ToBeWide(r.message.commentStr.c_str(), out.message, std::size(out.message));
				Utf8ToBeWide(who.mii.miiNickname, out.screenname, std::size(out.screenname));
				out.isMarkedAsReceived = 1;
				out.ukn98 = _swapEndianU64(r.message.messageId);
				UnpackTimestamp(0, out.uknDate);
				UnpackTimestamp(0, out.uknDate2);
				UnpackTimestamp(r.message.expireTimestamp, out.expireDate);
			}

			void FillBasicInfo(const nexPrincipalBasicInfo& src, FriendBasicInfo& out)
			{
				memset(&out, 0, sizeof(FriendBasicInfo));
				out.pid = src.principalId;
				CopyNnid(out.nnid, src.nnid);
				Utf8ToBeWide(src.mii.miiNickname, out.screenname, std::size(out.screenname));
				memcpy(out.miiData, src.mii.miiData, FFL_SIZE);
				SetDate(out.uknDate90, 0, 1, 1, 1, 1, 1);
			}

			struct MyProfile
			{
				uint8be country;
				uint8be area;
				uint16be unused;
			};
			static_assert(sizeof(MyProfile) == 4);

			struct MyPresence
			{
				uint8be ukn[0x130];
			};
			static_assert(sizeof(MyPresence) == 0x130);

			// ---- ioctlv vector access --------------------------------------------------------

			bool SizeMatches(const IPCIoctlVector& v, uint64 expected)
			{
				if ((uint64)(uint32)v.size == expected)
					return true;
				cemuLog_log(LogType::Force, "FPD: IPC buffer has incorrect size");
				return false;
			}

			// reads a single value; the vector must be exactly sizeof(T)
			template<typename T>
			bool ReadArg(const IPCIoctlVector& v, T& out)
			{
				if (!SizeMatches(v, sizeof(T)))
					return false;
				out = *(T*)v.basePhys.GetPtr();
				return true;
			}

			// typed view of an array argument; the vector must hold exactly 'count' elements
			template<typename T>
			T* ArrayArg(const IPCIoctlVector& v, uint64 count)
			{
				if (!SizeMatches(v, sizeof(T) * count))
					return nullptr;
				return (T*)v.basePhys.GetPtr();
			}

			template<typename T>
			nnResult WriteResult(IPCIoctlVector& v, const T& value)
			{
				if (v.size != sizeof(T))
					return kBadParam;
				*(T*)v.basePhys.GetPtr() = value;
				return kOk;
			}

			nnResult AsyncResult(NexFriends::RpcErrorCode r)
			{
				return r == NexFriends::ERR_NONE ? kOk : kFailed;
			}
		}

		class FriendService : public iosu::nn::IPCSimpleService
		{
			struct PendingNotificationRead
			{
				IPCCommandBody* cmd;
				uint32 capacity;
				FPDNotification* entries;
				uint32be* countOut;
			};

			struct Client
			{
				bool loggedIn = false;
				uint32 notificationMask = 0;
				uint64 lastNotification = 0;
				std::vector<PendingNotificationRead> pendingReads;
			};

			std::vector<IPCCommandBody*> m_pendingLogins;
			std::vector<Client*> m_clients;

			// the request currently being handled; 'in' and 'out' as seen by nn_fp
			IPCIoctlVector* in = nullptr;
			uint32 numIn = 0;
			IPCIoctlVector* out = nullptr;
			uint32 numOut = 0;

			bool Shape(uint32 wantIn, uint32 wantOut) const { return numIn == wantIn && numOut == wantOut; }

		public:
			FriendService() : iosu::nn::IPCSimpleService("/dev/fpd") {}

			std::string GetThreadName() override { return "IOSUModule::FPD"; }

			void StartService() override
			{
				cemu_assert_debug(m_pendingLogins.empty());
			}

			void StopService() override
			{
				m_pendingLogins.clear();
				for (Client* c : m_clients)
					delete c;
				m_clients.clear();
			}

			void* CreateClientObject() override
			{
				Client* c = new Client();
				m_clients.push_back(c);
				return c;
			}

			void DestroyClientObject(void* clientObject) override
			{
				Client* c = (Client*)clientObject;
				std::erase(m_clients, c);
				delete c;
			}

			void TimerUpdate() override
			{
				std::lock_guard lock(g_sessionMutex);
				if (!g_session)
					return;
				g_session->update();
				while (!m_pendingLogins.empty() && g_session->isOnline())
				{
					ServiceCallAsyncRespond(m_pendingLogins.front(), kOk);
					m_pendingLogins.erase(m_pendingLogins.begin());
				}
				g_notificationLog.Prune();
				for (Client* c : m_clients)
					DeliverNotifications(c);
			}

			uint32 ServiceCall(void* clientObject, uint32 requestId, IPCIoctlVector* vecIn, uint32 numVecIn, IPCIoctlVector* vecOut, uint32 numVecOut) override
			{
				// nn_fp sends its vectors in the opposite order of the generic service layout
				in = vecOut;
				numIn = numVecOut;
				out = vecIn;
				numOut = numVecIn;
				Client& c = *(Client*)clientObject;
				switch ((FPD_REQUEST_ID)requestId)
				{
				case FPD_REQUEST_ID::SetNotificationMask: return SetNotificationMask(c);
				case FPD_REQUEST_ID::GetNotificationAsync: return GetNotificationAsync(c);
				case FPD_REQUEST_ID::SetLedEventMask:
					cemuLog_logDebug(LogType::Force, "FPD: SetLedEventMask ignored");
					return kOk;
				case FPD_REQUEST_ID::LoginAsync: return LoginAsync(c);
				case FPD_REQUEST_ID::HasLoggedIn: return Shape(0, 1) ? WriteResult<uint32be>(*out, c.loggedIn ? 1 : 0) : kBadParam;
				case FPD_REQUEST_ID::IsOnline: return Shape(0, 1) ? WriteResult<uint32be>(*out, (g_session && g_session->isOnline()) ? 1 : 0) : kBadParam;
				case FPD_REQUEST_ID::GetMyPrincipalId: return GetMyPrincipalId();
				case FPD_REQUEST_ID::GetMyAccountId: return GetMyAccountId();
				case FPD_REQUEST_ID::GetMyScreenName: return GetMyScreenName();
				case FPD_REQUEST_ID::GetMyMii: return GetMyMii();
				case FPD_REQUEST_ID::GetMyProfile: return GetMyProfile();
				case FPD_REQUEST_ID::GetMyPresence: return GetMyPresence();
				case FPD_REQUEST_ID::GetMyComment: return GetMyComment();
				case FPD_REQUEST_ID::GetMyPreference: return GetMyPreference();
				case FPD_REQUEST_ID::GetMyPlayingGame: return GetMyPlayingGame();
				case FPD_REQUEST_ID::GetFriendAccountId: return GetFriendAccountId();
				case FPD_REQUEST_ID::GetFriendScreenName: return GetFriendScreenName();
				case FPD_REQUEST_ID::GetFriendMii: return GetFriendMii();
				case FPD_REQUEST_ID::GetFriendPresence: return GetFriendPresence();
				case FPD_REQUEST_ID::GetFriendRelationship: return GetFriendRelationship();
				case FPD_REQUEST_ID::GetFriendList: return GetPidList(false, false);
				case FPD_REQUEST_ID::GetFriendListAll: return GetPidList(false, true);
				case FPD_REQUEST_ID::GetFriendRequestList: return GetPidList(true, false);
				case FPD_REQUEST_ID::GetFriendRequestListEx: return GetFriendRequestListEx();
				case FPD_REQUEST_ID::GetBlackList: return GetBlackList();
				case FPD_REQUEST_ID::GetFriendListEx: return GetFriendListEx();
				case FPD_REQUEST_ID::UpdateCommentAsync: return UpdateCommentAsync();
				case FPD_REQUEST_ID::UpdatePreferenceAsync: return UpdatePreferenceAsync();
				case FPD_REQUEST_ID::AddFriendRequestByPlayRecordAsync: return AddFriendRequestAsync();
				case FPD_REQUEST_ID::AcceptFriendRequestAsync: return HandleRequestByMessageId(RequestAction::Accept);
				case FPD_REQUEST_ID::DeleteFriendRequestAsync: return HandleRequestByMessageId(RequestAction::Reject);
				case FPD_REQUEST_ID::CancelFriendRequestAsync: return HandleRequestByMessageId(RequestAction::Retract);
				case FPD_REQUEST_ID::MarkFriendRequestsAsReceivedAsync: return MarkFriendRequestsAsReceivedAsync();
				case FPD_REQUEST_ID::RemoveFriendAsync: return RemoveFriendAsync();
				case FPD_REQUEST_ID::DeleteFriendFlagsAsync: return DeleteFriendFlagsAsync();
				case FPD_REQUEST_ID::GetBasicInfoAsync: return GetBasicInfoAsync();
				case FPD_REQUEST_ID::CheckSettingStatusAsync: return CheckSettingStatusAsync();
				case FPD_REQUEST_ID::IsPreferenceValid: return IsPreferenceValid();
				case FPD_REQUEST_ID::GetRequestBlockSettingAsync: return GetRequestBlockSettingAsync();
				case FPD_REQUEST_ID::AddFriendAsyncByPid: return AddFriendAsyncByPid();
				case FPD_REQUEST_ID::UpdateGameModeVariation1:
				case FPD_REQUEST_ID::UpdateGameModeVariation2: return UpdateGameMode();
				default:
					cemuLog_log(LogType::Force, "FPD: unsupported request {}", requestId);
					return BUILD_NN_RESULT(NN_RESULT_LEVEL_FATAL, NN_RESULT_MODULE_NN_FP, 0);
				}
			}

			void StartFriendSession()
			{
				bool notYetStarted = false;
				if (!g_sessionStarted.compare_exchange_strong(notYetStarted, true))
					return;
				cemu_assert(!g_session);
				NAPI::AuthInfo auth;
				NAPI::NAPI_MakeAuthInfoFromCurrentAccount(auth);
				// the friend server token belongs to the friend list title
				NAPI::ACTGetNexTokenResult token = NAPI::ACT_GetNexToken_WithCache(auth, 0x0005001010001C00, 0x0000, 0x00003200);
				if (!token.isValid())
				{
					cemuLog_logDebug(LogType::Force, "FPD: no nex token for the friend server");
					g_myPresence.isOnline = 0;
					return;
				}
				const uint8 slot = iosu::act::getCurrentAccountSlot();
				uint32 myPid;
				iosu::act::getPrincipalId(slot, &myPid);
				char accountId[256] = {};
				iosu::act::getAccountId(slot, accountId);
				FFLData_t mii;
				iosu::act::getMii(slot, &mii);
				uint16 screenName[ACT_NICKNAME_SIZE] = {};
				iosu::act::getScreenname(slot, screenName);
				uint32 country = 0;
				iosu::act::getCountryIndex(slot, &country);

				g_myPresence.isOnline = 1;
				const uint64 titleId = CafeSystem::GetForegroundTitleId();
				// only regular applications are shown to others
				bool showTitle = GetTitleIdHigh(titleId) == 0x00050000;
				g_myPresence.gameKey.titleId = showTitle ? titleId : 0;
				g_myPresence.gameKey.ukn = showTitle ? CafeSystem::GetForegroundTitleVersion() : 0;

				addrinfo hints = {};
				hints.ai_family = AF_INET;
				addrinfo* resolved;
				if (getaddrinfo(token.nexToken.host, nullptr, &hints, &resolved) != 0)
				{
					cemuLog_log(LogType::Force, "FPD: cannot resolve friend server {}", token.nexToken.host);
					return;
				}
				char numericHost[NI_MAXHOST];
				getnameinfo(resolved->ai_addr, resolved->ai_addrlen, numericHost, sizeof(numericHost), nullptr, 0, NI_NUMERICHOST);
				cemuLog_log(LogType::Force, "FPD: friend server {} is {}", token.nexToken.host, numericHost);
				const uint32 serverIp = ((sockaddr_in*)resolved->ai_addr)->sin_addr.s_addr;
				freeaddrinfo(resolved);

				std::lock_guard lock(g_sessionMutex);
				g_session = new NexFriends(serverIp, token.nexToken.port, "ridfebb9", myPid, token.nexToken.nexPassword, token.nexToken.token, accountId, (uint8*)&mii, screenName, (uint8)country, g_myPresence);
				g_session->setNotificationHandler(OnSessionNotification);
				cemuLog_log(LogType::Force, "FPD: friend session created");
			}

			void StopFriendSession()
			{
				std::lock_guard lock(g_sessionMutex);
				bool running = true;
				if (!g_sessionStarted.compare_exchange_strong(running, false))
					return;
				delete g_session;
				g_session = nullptr;
			}

		private:
			void DeliverNotifications(Client* c)
			{
				if (c->pendingReads.empty())
					return;
				if (c->pendingReads.size() > 1)
					cemuLog_log(LogType::Force, "FPD: only one outstanding notification read is supported");
				PendingNotificationRead& read = c->pendingReads.front();
				uint32 n = 0;
				while (n < read.capacity)
				{
					std::optional<LoggedNotification> next = g_notificationLog.Next(c->lastNotification);
					if (!next)
						break;
					if ((c->notificationMask & (1u << (uint32)next->type)) == 0)
						continue; // masked out, but still counts as seen
					read.entries[n].type = (uint32)next->type;
					read.entries[n].pid = next->pid;
					n++;
				}
				if (n == 0)
					return;
				*read.countOut = n;
				ServiceCallAsyncRespond(read.cmd, kOk);
				c->pendingReads.erase(c->pendingReads.begin());
			}

			// ---- notifications and login -----------------------------------------------------

			nnResult SetNotificationMask(Client& c)
			{
				if (!Shape(1, 0))
					return kBadParam;
				uint32be mask;
				if (!ReadArg(in[0], mask))
					return kBadParam;
				c.notificationMask = mask;
				return kOk;
			}

			nnResult GetNotificationAsync(Client& c)
			{
				if (!Shape(0, 2))
					return kBadParam;
				if ((out[0].size % sizeof(FPDNotification)) != 0 || out[0].size < sizeof(FPDNotification))
				{
					cemuLog_log(LogType::Force, "FPD: GetNotificationAsync output has an unexpected size");
					return kBadParam;
				}
				// the response is deferred before the count vector is validated, so a bad count
				// vector leaves the request unanswered (kept as is)
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				uint32be* countOut = ArrayArg<uint32be>(out[1], 1);
				if (!countOut)
					return kBadParam;
				uint32 capacity = out[0].size / sizeof(FPDNotification);
				FPDNotification* entries = ArrayArg<FPDNotification>(out[0], capacity);
				if (!entries)
					return kBadParam;
				c.pendingReads.push_back({ cmd, capacity, entries, countOut });
				DeliverNotifications(&c);
				return kOk;
			}

			nnResult LoginAsync(Client& c)
			{
				if (!Shape(0, 0))
					return kBadParam;
				// offline: report success right away, some titles (Splatoon) hang on an error here
				if (!ActiveSettings::IsOnlineEnabled())
					return kOk;
				StartFriendSession();
				c.loggedIn = true;
				m_pendingLogins.push_back(ServiceCallDelayCurrentResponse());
				return kOk;
			}

			// ---- own account -----------------------------------------------------------------

			nnResult GetMyPrincipalId()
			{
				if (!Shape(0, 1))
					return kBadParam;
				uint32 pid = 0;
				iosu::act::getPrincipalId(iosu::act::getCurrentAccountSlot(), &pid);
				return WriteResult<uint32be>(*out, pid);
			}

			nnResult GetMyAccountId()
			{
				if (!Shape(0, 1))
					return kBadParam;
				std::string id = iosu::act::getAccountId2(iosu::act::getCurrentAccountSlot());
				if (out->size != ACT_ACCOUNTID_LENGTH || id.size() > ACT_ACCOUNTID_LENGTH - 1 || id.empty())
				{
					cemuLog_log(LogType::Force, "FPD: GetMyAccountId failed (size {} id length {})", (uint32)out->size, id.size());
					return kBadParam;
				}
				char* dst = (char*)out->basePhys.GetPtr();
				memset(dst, 0, ACT_ACCOUNTID_LENGTH);
				memcpy(dst, id.data(), id.size());
				return kOk;
			}

			nnResult GetMyScreenName()
			{
				if (!Shape(0, 1))
					return kBadParam;
				if (out->size != ACT_NICKNAME_SIZE * sizeof(uint16be))
					return kBadParam;
				uint16 name[ACT_NICKNAME_SIZE] = {};
				if (!iosu::act::getScreenname(iosu::act::getCurrentAccountSlot(), name))
					return kBadParam;
				uint16be* dst = (uint16be*)out->basePhys.GetPtr();
				for (sint32 i = 0; i < ACT_NICKNAME_SIZE; i++)
					dst[i] = name[i];
				return kOk;
			}

			nnResult GetMyMii()
			{
				if (!Shape(0, 1))
					return kBadParam;
				if (out->size != FFL_SIZE)
					return kBadParam;
				if (!iosu::act::getMii(iosu::act::getCurrentAccountSlot(), (FFLData_t*)out->basePhys.GetPtr()))
					return kBadParam;
				return kOk;
			}

			nnResult GetMyProfile()
			{
				if (!Shape(0, 1))
					return kBadParam;
				cemuLog_log(LogType::Force, "FPD: GetMyProfile returns an empty profile");
				return WriteResult<MyProfile>(*out, MyProfile{});
			}

			nnResult GetMyPresence()
			{
				if (!Shape(0, 1))
					return kBadParam;
				cemuLog_log(LogType::Force, "FPD: GetMyPresence returns an empty presence");
				return WriteResult<MyPresence>(*out, MyPresence{});
			}

			nnResult GetMyComment()
			{
				if (!Shape(0, 1))
					return kBadParam;
				Comment comment{};
				if (g_session)
				{
					if (out->size != sizeof(Comment))
						return kBadParam;
					nexComment mine;
					g_session->getMyComment(mine);
					comment.commentString.assignFromUTF8(mine.commentString);
				}
				// without a session the (empty) comment is written without checking the size
				memcpy(out->basePhys.GetPtr(), &comment, sizeof(Comment));
				return kOk;
			}

			nnResult GetMyPreference()
			{
				if (!Shape(0, 1))
					return kBadParam;
				FPDPreference pref{};
				if (g_session)
				{
					nexPrincipalPreference mine;
					g_session->getMyPreference(mine);
					pref.showOnline = mine.showOnline;
					pref.showGame = mine.showGame;
					pref.blockFriendRequests = mine.blockFriendRequests;
				}
				return WriteResult<FPDPreference>(*out, pref);
			}

			nnResult GetMyPlayingGame()
			{
				if (!Shape(0, 1))
					return kBadParam;
				GameKey key{};
				const uint64 titleId = CafeSystem::GetForegroundTitleId();
				if (GetTitleIdHigh(titleId) == 0x00050000)
				{
					key.titleId = titleId;
					key.ukn08 = CafeSystem::GetForegroundTitleVersion();
				}
				return WriteResult<GameKey>(*out, key);
			}

			// ---- per friend queries (pid list in, one entry per pid out) ---------------------

			// common argument layout: in[0] pid list, in[1] count, out[0] one T per pid
			template<typename T>
			bool PerPidArgs(uint32& count, uint32be*& pids, T*& results)
			{
				uint32be c;
				if (!ReadArg(in[1], c))
					return false;
				count = c;
				pids = ArrayArg<uint32be>(in[0], count);
				if (!pids)
					return false;
				results = ArrayArg<T>(out[0], count);
				return results != nullptr;
			}

			// looks a pid up among friends first, then among friend requests
			template<typename TFriend, typename TRequest>
			bool VisitPerson(uint32 pid, TFriend onFriend, TRequest onRequest)
			{
				nexFriend f;
				if (g_session->getFriendByPID(f, pid))
				{
					onFriend(f);
					return true;
				}
				nexFriendRequest r;
				bool incoming = false;
				if (g_session->getFriendRequestByPID(r, &incoming, pid))
				{
					onRequest(r, incoming);
					return true;
				}
				return false;
			}

			nnResult GetFriendAccountId()
			{
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pids;
				CafeString<ACT_ACCOUNTID_LENGTH>* ids;
				if (!PerPidArgs(count, pids, ids))
					return kBadParam;
				memset(ids, 0, sizeof(*ids) * count);
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					auto& dst = ids[i];
					bool found = VisitPerson(pids[i],
						[&](const nexFriend& f) { dst.assign(f.nnaInfo.principalInfo.nnid); },
						[&](const nexFriendRequest& r, bool) { dst.assign(r.principalInfo.nnid); });
					if (!found)
						cemuLog_log(LogType::Force, "FPD: GetFriendAccountId: pid {} unknown", (uint32)pids[i]);
				}
				return kOk;
			}

			nnResult GetFriendScreenName()
			{
				using Name = CafeWideString<ACT_NICKNAME_SIZE>;
				static_assert(sizeof(Name) == 11 * 2);
				if (!Shape(3, 2))
					return kBadParam;
				uint32be countBE;
				if (!ReadArg(in[1], countBE))
					return kBadParam;
				uint32 count = countBE;
				uint32be* pids = ArrayArg<uint32be>(in[0], count);
				if (!pids)
					return kBadParam;
				uint8be replaceNonAscii;
				if (!ReadArg(in[2], replaceNonAscii))
					return kBadParam;
				Name* names = ArrayArg<Name>(out[0], count);
				if (!names)
					return kBadParam;
				uint8be* languages = nullptr; // optional
				if (out[1].size > 0)
				{
					languages = ArrayArg<uint8be>(out[1], count);
					if (!languages)
						return kBadParam;
				}
				memset(names, 0, sizeof(Name) * count);
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					if (languages)
						languages[i] = 0;
					bool found = VisitPerson(pids[i],
						[&](const nexFriend& f) {
							names[i].assignFromUTF8(f.nnaInfo.principalInfo.mii.miiNickname);
							if (languages)
								languages[i] = f.nnaInfo.principalInfo.regionGuessed;
						},
						[&](const nexFriendRequest& r, bool) {
							names[i].assignFromUTF8(r.principalInfo.mii.miiNickname);
							if (languages)
								languages[i] = r.principalInfo.regionGuessed;
						});
					if (!found)
						cemuLog_log(LogType::Force, "FPD: GetFriendScreenName: pid {} unknown", (uint32)pids[i]);
				}
				return kOk;
			}

			nnResult GetFriendMii()
			{
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pids;
				FFLData_t* miis;
				if (!PerPidArgs(count, pids, miis))
					return kBadParam;
				memset(miis, 0, sizeof(FFLData_t) * count);
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					VisitPerson(pids[i],
						[&](const nexFriend& f) { memcpy(miis + i, f.nnaInfo.principalInfo.mii.miiData, FFL_SIZE); },
						[&](const nexFriendRequest& r, bool) { memcpy(miis + i, r.principalInfo.mii.miiData, FFL_SIZE); });
				}
				return kOk;
			}

			nnResult GetFriendPresence()
			{
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pids;
				FriendPresence* presences;
				if (!PerPidArgs(count, pids, presences))
					return kBadParam;
				memset(presences, 0, sizeof(FriendPresence) * count);
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					nexFriend f;
					if (!g_session->getFriendByPID(f, pids[i]))
					{
						cemuLog_log(LogType::Force, "FPD: GetFriendPresence: pid {} unknown", (uint32)pids[i]);
						continue;
					}
					FriendPresence& p = presences[i];
					p.isOnline = f.presence.isOnline ? 1 : 0;
					p.isValid = 1;
					// region fields are not provided by the session
					p.gameMode.joinFlagMask = f.presence.joinFlagMask;
					p.gameMode.matchmakeType = f.presence.joinAvailability;
					p.gameMode.joinGameId = f.presence.gameId;
					p.gameMode.joinGameMode = f.presence.gameMode;
					p.gameMode.hostPid = f.presence.hostPid;
					p.gameMode.groupId = f.presence.groupId;
					memcpy(p.gameMode.appSpecificData, f.presence.appSpecificData, sizeof(p.gameMode.appSpecificData));
				}
				return kOk;
			}

			nnResult GetFriendRelationship()
			{
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pids;
				uint8be* relations;
				if (!PerPidArgs(count, pids, relations))
					return kBadParam;
				for (uint32 i = 0; i < count; i++)
					relations[i] = RELATIONSHIP_INVALID;
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					VisitPerson(pids[i],
						[&](const nexFriend&) { relations[i] = RELATIONSHIP_FRIEND; },
						[&](const nexFriendRequest&, bool incoming) { relations[i] = incoming ? RELATIONSHIP_FRIENDREQUEST_IN : RELATIONSHIP_FRIENDREQUEST_OUT; });
				}
				return kOk;
			}

			// ---- lists -----------------------------------------------------------------------

			// GetFriendList / GetFriendListAll / GetFriendRequestList: in[0] start, in[1] max, out[0] pids, out[1] count
			nnResult GetPidList(bool requests, bool includeRequests)
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 2))
					return kBadParam;
				uint32be start, max;
				if (!ReadArg(in[0], start) || !ReadArg(in[1], max))
					return kBadParam;
				if ((uint64)(uint32)max * sizeof(FriendPID) != out[0].size || out[0].basePhys.IsNull())
				{
					cemuLog_log(LogType::Force, "FPD: pid list buffer has the wrong size");
					return kBadParam;
				}
				if (!g_session)
					return WriteResult<uint32be>(out[1], 0);
				std::vector<FriendPID> pids(std::min<size_t>(max, 500));
				uint32 found = 0;
				if (requests)
					g_session->getFriendRequestPIDs(pids.data(), &found, start, (sint32)pids.size(), true, false);
				else
					g_session->getFriendPIDs(pids.data(), &found, start, (sint32)pids.size(), includeRequests);
				betype<FriendPID>* dst = (betype<FriendPID>*)out[0].basePhys.GetPtr();
				for (uint32 i = 0; i < found; i++)
					dst[i] = pids[i];
				return WriteResult<uint32be>(out[1], found);
			}

			nnResult GetFriendRequestListEx()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pids;
				FriendRequest* entries;
				if (!PerPidArgs(count, pids, entries))
					return kBadParam;
				memset(entries, 0, sizeof(FriendRequest) * count);
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					nexFriendRequest r;
					bool incoming = false;
					if (!g_session->getFriendRequestByPID(r, &incoming, pids[i]))
					{
						cemuLog_log(LogType::Force, "FPD: GetFriendRequestListEx: pid {} has no request", (uint32)pids[i]);
						return kFailed;
					}
					FillRequestEntry(r, entries[i]);
				}
				return kOk;
			}

			nnResult GetBlackList()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 2))
					return kBadParam;
				uint32be start, max;
				if (!ReadArg(in[0], start) || !ReadArg(in[1], max))
					return kBadParam;
				if ((uint64)(uint32)max * sizeof(FriendPID) != out[0].size)
					return kBadParam;
				// blocked users are not tracked; the list is always empty
				cemuLog_logDebug(LogType::Force, "FPD: GetBlackList returns an empty list");
				return WriteResult<uint32be>(out[1], 0);
			}

			nnResult GetFriendListEx()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pids;
				FriendData* entries;
				if (!PerPidArgs(count, pids, entries))
					return kBadParam;
				memset(entries, 0, sizeof(FriendData) * count);
				if (!g_session)
					return kOk;
				for (uint32 i = 0; i < count; i++)
				{
					bool found = VisitPerson(pids[i],
						[&](const nexFriend& f) { FillFromFriend(f, entries[i]); },
						[&](const nexFriendRequest& r, bool) { FillFromRequest(r, entries[i]); });
					if (!found)
						cemuLog_logDebug(LogType::Force, "FPD: GetFriendListEx: pid {} unknown", (uint32)pids[i]);
				}
				return kOk;
			}

			nnResult GetBasicInfoAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 1))
					return kBadParam;
				uint32 count;
				uint32be* pidsBE;
				FriendBasicInfo* infos;
				if (!PerPidArgs(count, pidsBE, infos))
					return kBadParam;
				if (!g_session)
				{
					memset(infos, 0, sizeof(FriendBasicInfo) * count);
					return kOk;
				}
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				std::vector<uint32> pids(pidsBE, pidsBE + count);
				g_session->requestPrincipleBaseInfoByPID(pids.data(), count, [cmd, infos, count](NexFriends::RpcErrorCode r, std::span<nexPrincipalBasicInfo> result) {
					if (r != NexFriends::ERR_NONE)
					{
						ServiceCallAsyncRespond(cmd, kFailed);
						return;
					}
					cemu_assert_debug(result.size() == count);
					for (uint32 i = 0; i < count; i++)
						FillBasicInfo(result[i], infos[i]);
					ServiceCallAsyncRespond(cmd, kOk);
				});
				return kOk;
			}

			// ---- changes (answered when the server replies) ----------------------------------

			// validates a zero terminated UTF-16 string argument
			const uint16be* MessageArg(const IPCIoctlVector& v, uint32& length)
			{
				length = v.size / sizeof(uint16be);
				const uint16be* text = ArrayArg<uint16be>(v, length);
				if (!text)
					return nullptr;
				if (length == 0 || text[length - 1] != 0)
				{
					cemuLog_log(LogType::Force, "FPD: message argument is not zero terminated");
					return nullptr;
				}
				return text;
			}

			std::function<void(NexFriends::RpcErrorCode)> Responder(IPCCommandBody* cmd)
			{
				return [cmd](NexFriends::RpcErrorCode r) { ServiceCallAsyncRespond(cmd, AsyncResult(r)); };
			}

			nnResult UpdateCommentAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(1, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint32 length;
				const uint16be* text = MessageArg(in[0], length);
				if (!text)
					return kBadParam;
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				nexComment comment;
				comment.ukn0 = 0;
				comment.commentString = StringHelpers::ToUtf8(text, length - 1);
				comment.ukn1 = 0;
				g_session->updateCommentAsync(comment, Responder(cmd));
				return kOk;
			}

			nnResult UpdatePreferenceAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(1, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				FPDPreference* pref = ArrayArg<FPDPreference>(in[0], 1);
				if (!pref)
					return kBadParam;
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				nexPrincipalPreference p(pref->showOnline != 0 ? 1 : 0, pref->showGame != 0 ? 1 : 0, pref->blockFriendRequests != 0 ? 1 : 0);
				g_session->updatePreferencesAsync(p, Responder(cmd));
				return kOk;
			}

			nnResult AddFriendRequestAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				RecentPlayRecordEx* record = ArrayArg<RecentPlayRecordEx>(in[0], 1);
				if (!record)
					return kBadParam;
				uint32 length;
				const uint16be* text = MessageArg(in[1], length);
				if (!text)
					return kBadParam;
				std::string message = StringHelpers::ToUtf8(text, length - 1);
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				g_session->addFriendRequest(record->pid, message.data(), Responder(cmd));
				return kOk;
			}

			enum class RequestAction
			{
				Accept,  // incoming request -> friend
				Reject,  // incoming request -> deleted
				Retract, // outgoing request -> removed
			};

			nnResult HandleRequestByMessageId(RequestAction action)
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(1, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint64be messageIdBE;
				if (!ReadArg(in[0], messageIdBE))
					return kBadParam;
				const uint64 messageId = messageIdBE;
				nexFriendRequest request;
				bool incoming;
				if (!g_session->getFriendRequestByMessageId(request, &incoming, messageId))
					return kFailed;
				const bool wantIncoming = action != RequestAction::Retract;
				if (incoming != wantIncoming)
				{
					cemuLog_log(LogType::Force, "FPD: friend request {} has the wrong direction for this action", messageId);
					return kFailed;
				}
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				switch (action)
				{
				case RequestAction::Accept: g_session->acceptFriendRequest(messageId, Responder(cmd)); break;
				case RequestAction::Reject: g_session->deleteFriendRequest(messageId, Responder(cmd)); break;
				case RequestAction::Retract: g_session->removeFriend(request.principalInfo.principalId, Responder(cmd)); break;
				}
				return kOk;
			}

			nnResult MarkFriendRequestsAsReceivedAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(2, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint32be countBE;
				if (!ReadArg(in[1], countBE))
					return kBadParam;
				uint32 count = countBE;
				uint64be* idsBE = ArrayArg<uint64be>(in[0], count);
				if (!idsBE)
					return kBadParam;
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				std::vector<uint64> ids(idsBE, idsBE + count);
				g_session->markFriendRequestsAsReceived(ids.data(), (sint32)ids.size(), Responder(cmd));
				return kOk;
			}

			nnResult RemoveFriendAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(1, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint32be pid;
				if (!ReadArg(in[0], pid))
					return kBadParam;
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				g_session->removeFriend(pid, Responder(cmd));
				return kOk;
			}

			nnResult DeleteFriendFlagsAsync()
			{
				std::lock_guard lock(g_sessionMutex);
				if (!Shape(3, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint32be pid;
				if (!ReadArg(in[0], pid))
					return kBadParam;
				cemuLog_logDebug(LogType::Force, "FPD: DeleteFriendFlagsAsync ignored");
				return kOk;
			}

			nnResult CheckSettingStatusAsync()
			{
				if (!Shape(0, 1) || out[0].size != sizeof(uint8be))
					return kBadParam;
				if (!g_session)
					return kFailed;
				// answered immediately with status 1 (meaning unknown)
				IPCCommandBody* cmd = ServiceCallDelayCurrentResponse();
				ServiceCallAsyncRespond(cmd, WriteResult<uint8be>(out[0], 1));
				cemuLog_log(LogType::Force, "FPD: CheckSettingStatusAsync returns a fixed status");
				return kOk;
			}

			nnResult IsPreferenceValid()
			{
				if (!Shape(0, 1) || !g_session)
					return 0;
				// preferences get valid defaults when the session is created; 0 would start the first time setup
				return WriteResult<uint32be>(*out, 1);
			}

			nnResult GetRequestBlockSettingAsync()
			{
				if (!Shape(2, 1))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint32 count;
				uint32be* pids;
				uint8be* settings;
				if (!PerPidArgs(count, pids, settings))
					return kBadParam;
				// nobody is reported as blocking requests (0); 1 would make the friend list app add a provisional friend
				for (uint32 i = 0; i < count; i++)
					settings[i] = 0;
				return kOk;
			}

			nnResult AddFriendAsyncByPid()
			{
				if (!Shape(1, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				uint32be pid;
				if (!ReadArg(in[0], pid))
					return kBadParam;
				cemuLog_log(LogType::Force, "FPD: AddFriendAsyncByPid ignored");
				return kOk;
			}

			nnResult UpdateGameMode()
			{
				if (!Shape(2, 0))
					return kBadParam;
				if (!g_session)
					return kFailed;
				GameMode* mode = ArrayArg<GameMode>(in[0], 1);
				if (!mode)
					return kBadParam;
				uint32 length = in[1].size / sizeof(uint16be);
				if (length == 0 || (in[1].size % sizeof(uint16be)) != 0)
					return kBadParam;
				// the game mode message is accepted but not forwarded
				GameModeToPresence(*mode, g_myPresence);
				g_session->updateMyPresence(g_myPresence);
				return kOk;
			}
		};

		namespace
		{
			FriendService g_service;

			class FpdModule : public ::IOSUModule
			{
				void TitleStart() override
				{
					g_service.Start();
					g_service.SetTimerUpdate(1000);
				}
				void TitleStop() override
				{
					g_service.StopFriendSession();
					g_service.Stop();
				}
			};

			FpdModule g_module;
		}

		IOSUModule* GetModule()
		{
			return &g_module;
		}
	}
}
