// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nsysnet.rpl gives games BSD-style sockets. Each guest socket handle (1..32)
// maps to a host socket. Addresses are IPv4 only; a guest sockaddr is the
// usual 16-byte layout with a big-endian family, and the port/address bytes
// are already in network order, so they are copied as they are.
//
// Errors go to the calling thread's errno (__gh_set_errno), read back with
// socketlasterr(). Blocking receives and sends are emulated by polling a
// non-blocking host socket and sleeping the guest thread in between, so a
// waiting game thread never blocks the emulated CPU core.
//
// The NSSL part only records which certificates a context uses; nlibcurl
// applies them when it opens an HTTPS connection.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_GHS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Common/socket.h"
#include "nsysnet.h"

#include <deque>

#if BOOST_OS_UNIX
#include <netinet/tcp.h>
#endif

#if BOOST_OS_WINDOWS
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace nsysnet
{
	namespace
	{
		// ---- Wii U constants ----------------------------------------------------

		constexpr sint32 kAfInet = 2;
		constexpr sint32 kSockStream = 1;
		constexpr sint32 kSockDgram = 2;
		constexpr sint32 kProtoIp = 0;
		constexpr sint32 kProtoTcp = 6;
		constexpr sint32 kProtoUdp = 17;

		constexpr sint32 kSolSocket = -1;
		// SOL_SOCKET options
		constexpr sint32 kSoReuseAddr = 0x0004;
		constexpr sint32 kSoKeepAlive = 0x0008;
		constexpr sint32 kSoWinScale = 0x0400;
		constexpr sint32 kSoSndBuf = 0x1001;
		constexpr sint32 kSoRcvBuf = 0x1002;
		constexpr sint32 kSoLastError = 0x1007;
		constexpr sint32 kSoType = 0x1008;
		constexpr sint32 kSoNbio = 0x1014;
		constexpr sint32 kSoBio = 0x1015;
		constexpr sint32 kSoNonBlock = 0x1016;
		// IPPROTO_TCP options
		constexpr sint32 kTcpNoDelay = 0x2004;
		// IPPROTO_IP options
		constexpr sint32 kIpTos = 3;
		constexpr sint32 kIpTtl = 4;
		constexpr sint32 kIpMulticastIf = 9;
		constexpr sint32 kIpDropMembership = 13;

		constexpr sint32 kMsgPeek = 0x02;
		constexpr sint32 kMsgDontWait = 0x20;
		constexpr sint32 kMsgReadTtl = 0x40; // recvfrom_ex: report the packet's TTL

		// errno values
		constexpr sint32 kErrNone = 0;
		constexpr sint32 kErrWouldBlock = 6;
		constexpr sint32 kErrConnReset = 8;
		constexpr sint32 kErrNotConn = 9;
		constexpr sint32 kErrInvalid = 11;
		constexpr sint32 kErrShutdown = 15;
		constexpr sint32 kErrInProgress = 22;
		constexpr sint32 kErrAfNoSupport = 33;
		constexpr sint32 kErrLibNotReady = 0x2B;
		constexpr sint32 kErrUnmapped = 99999;

		constexpr sint32 kMaxSockets = 32;

		// ---- guest structures -----------------------------------------------

		struct GuestSockAddr
		{
			uint16be family;
			uint8 data[14]; // port + IPv4 address, network order
		};
		static_assert(sizeof(GuestSockAddr) == 16);

		struct GuestInAddr
		{
			uint32be addr;
		};

		struct GuestTimeVal
		{
			uint32be sec;
			uint32be usec;
		};

		struct GuestAddrInfo
		{
			sint32be flags;
			sint32be family;
			sint32be sockType;
			sint32be protocol;
			uint32be addrLen;
			MEMPTR<char> canonName;
			MEMPTR<GuestSockAddr> addr;
			MEMPTR<GuestAddrInfo> next;
		};
		static_assert(sizeof(GuestAddrInfo) == 0x20);

		struct GuestHostEnt
		{
			MEMPTR<char> name;
			MEMPTR<MEMPTR<char>> aliases;
			sint32be addrType;
			sint32be length;
			MEMPTR<MEMPTR<GuestInAddr>> addrList;
		};

		struct SendToMultiBuffer
		{
			MEMPTR<uint8> data;
			uint32be dataLen;
			MEMPTR<uint32be> sendLens;
			uint32be sendLensCount;
			MEMPTR<GuestSockAddr> destinations;
			uint32be destinationCount;
			MEMPTR<uint32be> results;
			uint32be resultCount;
		};

		// ---- socket table ---------------------------------------------------

		struct Socket
		{
			SOCKET host;
			sint32 family;
			sint32 type;
			sint32 protocol;
			bool nonBlocking = false;
			bool recvShut = false;
			bool sendShut = false;
		};

		std::mutex g_tableMutex;
		std::array<std::optional<Socket>, kMaxSockets> g_sockets;
		bool g_libReady = false;

		Socket* Lookup(sint32 handle)
		{
			std::lock_guard lock(g_tableMutex);
			if (handle < 1 || handle > kMaxSockets || !g_sockets[handle - 1])
				return nullptr;
			return &*g_sockets[handle - 1];
		}

		sint32 Insert(const Socket& s)
		{
			std::lock_guard lock(g_tableMutex);
			for (sint32 i = 0; i < kMaxSockets; i++)
			{
				if (!g_sockets[i])
				{
					g_sockets[i] = s;
					return i + 1;
				}
			}
			return -1;
		}

		void Remove(sint32 handle)
		{
			std::lock_guard lock(g_tableMutex);
			if (handle >= 1 && handle <= kMaxSockets)
				g_sockets[handle - 1].reset();
		}

		// ---- errors -------------------------------------------------------------

		void SetErrno(sint32 e)
		{
			coreinit::__gh_set_errno(e);
		}

		enum class ErrorContext
		{
			Generic,
			Connect, // a pending non-blocking connect reports "in progress"
			Accept,  // success is any non-negative result
		};

		// sets errno from a host call's result and returns 0 / the result on
		// success, -1 on failure
		sint32 ReportResult(sint32 result, sint32 hostError, ErrorContext ctx = ErrorContext::Generic)
		{
			const bool ok = ctx == ErrorContext::Accept ? result >= 0 : result == 0;
			if (ok)
			{
				SetErrno(kErrNone);
				return result;
			}
			switch (hostError)
			{
			case 0:
				SetErrno(kErrNone);
				break;
			case WSAEWOULDBLOCK:
				SetErrno(ctx == ErrorContext::Connect ? kErrInProgress : kErrWouldBlock);
				break;
#if WSAEINPROGRESS != WSAEWOULDBLOCK
			case WSAEINPROGRESS:
				SetErrno(kErrInProgress);
				break;
#endif
			case WSAECONNABORTED:
				cemuLog_logDebug(LogType::Socket, "nsysnet: connection aborted");
				break; // errno left as it was
			case WSAESHUTDOWN:
				SetErrno(kErrShutdown);
				break;
			case WSAENOTCONN:
				SetErrno(kErrNotConn);
				break;
			default:
				cemuLog_logDebug(LogType::Force, "nsysnet: unmapped host socket error {}", hostError);
				SetErrno(kErrUnmapped);
				break;
			}
			return -1;
		}

		// ---- host helpers -------------------------------------------------------

		void SetHostNonBlocking(SOCKET s, bool nonBlocking)
		{
#if BOOST_OS_WINDOWS
			u_long mode = nonBlocking ? 1 : 0;
			ioctlsocket(s, FIONBIO, &mode);
#else
			int flags = fcntl(s, F_GETFL);
			flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
			fcntl(s, F_SETFL, flags);
#endif
		}

		void ToHostAddr(const GuestSockAddr* in, sockaddr& out)
		{
			memset(&out, 0, sizeof(out));
#if BOOST_OS_MACOS
			out.sa_len = sizeof(sockaddr);
#endif
			out.sa_family = (decltype(out.sa_family))(uint16)in->family;
			memcpy(out.sa_data, in->data, sizeof(in->data));
		}

		void ToGuestAddr(const sockaddr& in, GuestSockAddr* out)
		{
			out->family = (uint16)in.sa_family;
			memcpy(out->data, in.sa_data, sizeof(out->data));
		}

		bool HostHasData(SOCKET s, bool& hasException)
		{
			fd_set readSet, exceptSet;
			FD_ZERO(&readSet);
			FD_ZERO(&exceptSet);
			FD_SET(s, &readSet);
			FD_SET(s, &exceptSet);
			timeval now{0, 0};
			const int n = select((int)s + 1, &readSet, nullptr, &exceptSet, &now);
			hasException = n > 0 && FD_ISSET(s, &exceptSet);
			return n > 0 && FD_ISSET(s, &readSet);
		}

		void SleepTicks(uint64 ticks)
		{
			coreinit::OSSleepTicks(ticks);
		}

		uint32 ReadBE32(const void* p)
		{
			return _swapEndianU32(*(const uint32*)p);
		}

		bool ParseDottedQuad(const char* text, uint32& out)
		{
			sint32 a = -1, b = -1, c = -1, d = -1;
			if (sscanf(text, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
				return false;
			for (sint32 v : {a, b, c, d})
			{
				if (v < 0 || v > 255)
					return false;
			}
			out = ((uint32)a << 24) | ((uint32)b << 16) | ((uint32)c << 8) | (uint32)d;
			return true;
		}

		// ---- library ------------------------------------------------------------

		sint32 socket_lib_init()
		{
			g_libReady = true;
#if BOOST_OS_WINDOWS
			WSADATA wsa;
			WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
			return 0;
		}

		sint32 socket_lib_finish()
		{
			g_libReady = false;
#if BOOST_OS_WINDOWS
			WSACleanup();
#endif
			return 0;
		}

		sint32 socketlasterr()
		{
			return coreinit::__gh_get_errno();
		}

		// ---- sockets ------------------------------------------------------------

		sint32 socket(sint32 family, sint32 type, sint32 protocol)
		{
			if (!g_libReady)
			{
				SetErrno(kErrLibNotReady);
				return -1;
			}
			if (family != kAfInet || (type != kSockStream && type != kSockDgram) ||
				(protocol != kProtoIp && protocol != kProtoTcp && protocol != kProtoUdp))
			{
				cemuLog_logDebug(LogType::Force, "nsysnet: socket({}, {}, {}) is not supported", family, type, protocol);
				return -1;
			}
			// the Wii U uses the same numbers as the host APIs for these
			Socket s{};
			s.family = family;
			s.type = type;
			s.protocol = protocol;
			s.host = ::socket(family, type, protocol);
#if BOOST_OS_WINDOWS
			if (protocol == IPPROTO_UDP)
			{
				// don't report ICMP "port unreachable" as a receive error
				BOOL reportReset = FALSE;
				DWORD returned = 0;
				WSAIoctl(s.host, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
			}
#endif
			const sint32 handle = Insert(s);
			if (handle < 0)
			{
				cemuLog_log(LogType::Force, "nsysnet: out of socket handles");
				closesocket(s.host);
				SetErrno(kErrInvalid);
				return -1;
			}
			cemuLog_log(LogType::Socket, "nsysnet: socket handle {}", handle);
			return handle;
		}

		sint32 mw_socket(sint32 family, sint32 type, sint32 protocol)
		{
			return socket(family, type, protocol);
		}

		sint32 shutdown(sint32 handle, sint32 how)
		{
			Socket* s = Lookup(handle);
			sint32 r = 0;
			if (s)
			{
				r = ::shutdown(s->host, how);
				if (how == 0 || how == 2)
					s->recvShut = true;
				if (how == 1 || how == 2)
					s->sendShut = true;
			}
			SetErrno(kErrNone);
			return r;
		}

		sint32 socketclose(sint32 handle)
		{
			if (Socket* s = Lookup(handle))
			{
				closesocket(s->host);
				Remove(handle);
			}
			return 0;
		}

		sint32 HostSetIntOption(SOCKET s, int level, int name, uint32 value)
		{
			const int v = (int)value;
			return setsockopt(s, level, name, (const char*)&v, sizeof(v));
		}

		sint32 setsockopt(sint32 handle, sint32 level, sint32 optname, uint8* optval, sint32 optlen)
		{
			Socket* s = Lookup(handle);
			if (!s)
				return 0;
			const uint32 value = (optval && optlen >= 4) ? ReadBE32(optval) : 0;
			if (level == kSolSocket)
			{
				switch (optname)
				{
				case kSoReuseAddr:
					HostSetIntOption(s->host, SOL_SOCKET, SO_REUSEADDR, value);
					break;
				case kSoNbio:
				case kSoBio:
					// set the mode named by the option, the value is ignored
					s->nonBlocking = optname == kSoNbio;
					SetHostNonBlocking(s->host, s->nonBlocking);
					break;
				case kSoNonBlock:
					s->nonBlocking = value != 0;
					SetHostNonBlocking(s->host, s->nonBlocking);
					break;
				case kSoRcvBuf:
					HostSetIntOption(s->host, SOL_SOCKET, SO_RCVBUF, value);
					break;
				case kSoSndBuf:
					HostSetIntOption(s->host, SOL_SOCKET, SO_SNDBUF, value);
					break;
				case kSoKeepAlive:
				case kSoWinScale:
					break; // accepted, not applied
				default:
					cemuLog_logDebug(LogType::Force, "nsysnet: setsockopt(SOL_SOCKET, {:#x}) not supported", optname);
					break;
				}
			}
			else if (level == kProtoTcp)
			{
				if (optname == kTcpNoDelay)
					HostSetIntOption(s->host, IPPROTO_TCP, TCP_NODELAY, value);
				else
					cemuLog_logDebug(LogType::Force, "nsysnet: setsockopt(IPPROTO_TCP, {:#x}) not supported", optname);
			}
			else if (level == kProtoIp)
			{
				// TTL, TOS and multicast options are accepted but not applied
				if (!(optname == kIpTos || optname == kIpTtl || (optname >= kIpMulticastIf && optname <= kIpDropMembership)))
					cemuLog_logDebug(LogType::Force, "nsysnet: setsockopt(IPPROTO_IP, {:#x}) not supported", optname);
			}
			else
				cemuLog_logDebug(LogType::Force, "nsysnet: setsockopt level {:#x} not supported", level);
			return 0;
		}

		sint32 getsockopt(sint32 handle, sint32 level, sint32 optname, uint8* optval, uint32be* optlen)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			if (level != kSolSocket)
			{
				cemuLog_logDebug(LogType::Force, "nsysnet: getsockopt level {:#x} not supported", level);
				return 0;
			}
			auto answer = [&](uint32 v) {
				*(uint32be*)optval = v;
				*optlen = 4;
			};
			auto queryHost = [&](int name) -> sint32 {
				int v = 0;
				socklen_t len = sizeof(v);
				const sint32 r = ReportResult(::getsockopt(s->host, SOL_SOCKET, name, (char*)&v, &len), GETLASTERR);
				answer((uint32)v);
				return r;
			};
			switch (optname)
			{
			case kSoLastError:
				return queryHost(SO_ERROR); // pending error of a non-blocking connect
			case kSoRcvBuf:
				return queryHost(SO_RCVBUF);
			case kSoSndBuf:
				return queryHost(SO_SNDBUF);
			case kSoType:
				answer((uint32)s->type);
				return 0;
			case kSoNonBlock:
				answer(s->nonBlocking ? 1 : 0);
				return 0;
			default:
				cemuLog_logDebug(LogType::Force, "nsysnet: getsockopt(SOL_SOCKET, {:#x}) not supported", optname);
				return 0;
			}
		}

		sint32 bind(sint32 handle, GuestSockAddr* addr, sint32 len)
		{
			Socket* s = Lookup(handle);
			if (!s)
				return 0;
			sockaddr host;
			ToHostAddr(addr, host);
			const sint32 hr = ::bind(s->host, &host, sizeof(host));
			return ReportResult(hr, GETLASTERR);
		}

		sint32 listen(sint32 handle, sint32 backlog)
		{
			Socket* s = Lookup(handle);
			if (!s)
				return 0;
			return ::listen(s->host, backlog) == 0 ? 0 : -1;
		}

		sint32 accept(sint32 handle, GuestSockAddr* addr, uint32be* addrLen)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			if (*addrLen != 16)
			{
				cemuLog_log(LogType::Force, "nsysnet: accept() with an address size other than 16");
				return 0;
			}
			while (true)
			{
				sockaddr peer{};
				socklen_t peerLen = sizeof(peer);
				if (!s->nonBlocking)
					SetHostNonBlocking(s->host, true);
				const SOCKET client = ::accept(s->host, &peer, &peerLen);
				const sint32 err = GETLASTERR;
				if (!s->nonBlocking)
					SetHostNonBlocking(s->host, false);
				if (client != INVALID_SOCKET)
				{
					ToGuestAddr(peer, addr);
					const sint32 h = AdoptHostSocket(client);
					SetErrno(kErrNone);
					return h;
				}
				if (s->nonBlocking || err != WSAEWOULDBLOCK)
				{
					ToGuestAddr(peer, addr);
					return ReportResult(-1, err, ErrorContext::Accept);
				}
				// blocking accept: wait for a client without holding the CPU
				SleepTicks(ESPRESSO_TIMER_CLOCK / 1000);
			}
		}

		sint32 connect(sint32 handle, GuestSockAddr* addr, sint32 len)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			sockaddr host;
			ToHostAddr(addr, host);
			cemuLog_log(LogType::Socket, "nsysnet: connect to {}.{}.{}.{}:{}", addr->data[2], addr->data[3], addr->data[4], addr->data[5], (addr->data[0] << 8) | addr->data[1]);
			const sint32 hr = ::connect(s->host, &host, sizeof(host));
			return ReportResult(hr, GETLASTERR, ErrorContext::Connect);
		}

		sint32 send(sint32 handle, uint8* data, sint32 len, sint32 flags)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			if (flags & ~kMsgDontWait)
				cemuLog_logDebug(LogType::Force, "nsysnet: send() flags {:#x} not supported", flags);
			const sint32 hr = (sint32)::send(s->host, (const char*)data, len, 0);
			ReportResult(hr <= 0 ? -1 : 0, GETLASTERR);
			return hr;
		}

		sint32 recv(sint32 handle, uint8* buffer, sint32 len, sint32 flags)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			const int hostFlags = (flags & kMsgPeek) ? MSG_PEEK : 0;
			if (flags & ~(kMsgDontWait | kMsgPeek))
				cemuLog_logDebug(LogType::Force, "nsysnet: recv() flags {:#x} not supported", flags);
			const bool dontWait = s->nonBlocking || (flags & kMsgDontWait);

			if (dontWait && !s->nonBlocking)
				SetHostNonBlocking(s->host, true);
			if (!dontWait)
			{
				// wait until a byte, the end of the stream or an error is there
				SetHostNonBlocking(s->host, true);
				while (true)
				{
					char probe;
					const sint32 n = (sint32)::recv(s->host, &probe, 1, MSG_PEEK);
					if (n >= 0 || GETLASTERR != WSAEWOULDBLOCK)
						break;
					SleepTicks(coreinit::EspressoTime::GetTimerClock() / 5000); // 0.2 ms
				}
				SetHostNonBlocking(s->host, false);
			}
			const sint32 hr = (sint32)::recv(s->host, (char*)buffer, len, hostFlags);
			ReportResult(hr <= 0 ? -1 : 0, GETLASTERR);
			if (dontWait && !s->nonBlocking)
				SetHostNonBlocking(s->host, false);
			return hr;
		}

		// shared by recvfrom and recvfrom_ex: poll until a datagram is there
		sint32 ReceiveFrom(Socket* s, uint8* buffer, sint32 len, int hostFlags, bool dontWait, uint64 pollTicks, sockaddr& from, socklen_t& fromLen)
		{
			while (true)
			{
				if (s->recvShut)
				{
					SetErrno(kErrShutdown);
					return -1;
				}
				bool exception = false;
				if (HostHasData(s->host, exception))
				{
					const sint32 r = (sint32)::recvfrom(s->host, (char*)buffer, len, hostFlags, &from, &fromLen);
					SetErrno(kErrNone);
					return r;
				}
				if (exception)
					cemuLog_logDebug(LogType::Force, "nsysnet: exception pending on socket");
				if (dontWait)
				{
					SetErrno(kErrWouldBlock);
					return -1;
				}
				SleepTicks(pollTicks);
				PPCCore_switchToScheduler();
			}
		}

		sint32 recvfrom(sint32 handle, uint8* buffer, sint32 len, sint32 flags, GuestSockAddr* fromAddr, uint32be* fromLen)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			const int hostFlags = (flags & kMsgPeek) ? MSG_PEEK : 0;
			const bool dontWait = s->nonBlocking || (flags & kMsgDontWait);
			sockaddr from{};
			socklen_t hostLen = sizeof(from);
			const sint32 r = ReceiveFrom(s, buffer, len, hostFlags, dontWait, ESPRESSO_CORE_CLOCK / 1000, from, hostLen);
			if (r >= 0 && fromAddr && fromLen)
			{
				*fromLen = (uint32)hostLen;
				ToGuestAddr(from, fromAddr);
			}
			return r;
		}

		sint32 recvfrom_ex(sint32 handle, uint8* buffer, sint32 len, sint32 flags, GuestSockAddr* fromAddr, uint32be* fromLen, uint8* extra, sint32 extraLen)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			const int hostFlags = (flags & kMsgPeek) ? MSG_PEEK : 0;
			if ((flags & kMsgReadTtl) && extra && extraLen >= 1)
				extra[0] = 5; // the TTL is not known; report a typical value
			const bool dontWait = s->nonBlocking || (flags & kMsgDontWait);
			sockaddr from{};
			socklen_t hostLen = fromLen ? (socklen_t)(uint32)*fromLen : (socklen_t)sizeof(from);
			const sint32 r = ReceiveFrom(s, buffer, len, hostFlags, dontWait, ESPRESSO_CORE_CLOCK / 100, from, hostLen);
			if (r >= 0)
			{
				if (fromLen)
					*fromLen = (uint32)hostLen;
				if (fromAddr)
					ToGuestAddr(from, fromAddr);
			}
			return r;
		}

		sint32 sendto(sint32 handle, uint8* data, sint32 len, sint32 flags, GuestSockAddr* toAddr, sint32 toLen)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			const bool dontWait = s->nonBlocking || (flags & kMsgDontWait);
			sockaddr to;
			ToHostAddr(toAddr, to);
			SetHostNonBlocking(s->host, true);
			sint32 r;
			sint32 err;
			while (true)
			{
				r = (sint32)::sendto(s->host, (const char*)data, len, 0, &to, toLen);
				err = GETLASTERR;
				if (dontWait || r >= 0 || err != WSAEWOULDBLOCK)
					break;
				SleepTicks(ESPRESSO_CORE_CLOCK / 100);
				PPCCore_switchToScheduler();
			}
			SetHostNonBlocking(s->host, s->nonBlocking);
			ReportResult(r <= 0 ? -1 : 0, err);
			return r;
		}

		sint32 sendto_multi(sint32 handle, uint8* data, sint32 len, uint32 flags, GuestSockAddr* destinations, uint32 count)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			for (uint32 i = 0; i < count; i++)
			{
				sockaddr to;
				ToHostAddr(destinations + i, to);
				if ((sint32)::sendto(s->host, (const char*)data, len, 0, &to, sizeof(to)) < len)
					cemuLog_logDebug(LogType::Force, "nsysnet: sendto_multi could not send to destination {}", i);
			}
			SetErrno(kErrNone);
			return len;
		}

		sint32 sendto_multi_ex(sint32 handle, uint32 flags, SendToMultiBuffer* buf, uint32 count)
		{
			Socket* s = Lookup(handle);
			if (!s)
			{
				SetErrno(kErrInvalid);
				return -1;
			}
			if (buf->sendLensCount < count || buf->destinationCount < count || buf->resultCount < count)
				cemuLog_logDebug(LogType::Force, "nsysnet: sendto_multi_ex arrays are smaller than the count");
			const uint8* data = buf->data.GetPtr();
			sint32 total = 0;
			for (uint32 i = 0; i < count; i++)
			{
				const uint32 n = buf->sendLens[i];
				sockaddr to;
				ToHostAddr(buf->destinations.GetPtr() + i, to);
				const sint32 r = (sint32)::sendto(s->host, (const char*)data, (int)n, 0, &to, sizeof(to));
				buf->results[i] = r >= (sint32)n ? (uint32)r : 0;
				if (r > 0)
					total += r;
				data += n;
			}
			return total;
		}

		// ---- select ---------------------------------------------------------

		// guest set -> host set; returns the highest host socket seen
		void ToHostSet(const GuestFdSet* in, sint32 nfds, fd_set& out, int& maxHost)
		{
			FD_ZERO(&out);
			if (!in)
				return;
			const uint32 mask = in->mask;
			for (sint32 h = 0; h < nfds && h < 32; h++)
			{
				if (!((mask >> h) & 1))
					continue;
				if (Socket* s = Lookup(h))
				{
					FD_SET(s->host, &out);
					maxHost = std::max(maxHost, (int)s->host);
				}
			}
		}

		void ToGuestSet(GuestFdSet* out, fd_set& in)
		{
			if (!out)
				return;
			uint32 mask = 0;
			std::lock_guard lock(g_tableMutex);
			for (sint32 i = 0; i < kMaxSockets; i++)
			{
				const sint32 handle = i + 1;
				if (g_sockets[i] && handle < 32 && FD_ISSET(g_sockets[i]->host, &in))
					mask |= 1u << handle;
			}
			out->mask = mask;
		}

		sint32 select(sint32 nfds, GuestFdSet* readSet, GuestFdSet* writeSet, GuestFdSet* exceptSet, GuestTimeVal* timeout)
		{
			auto empty = [](GuestFdSet* s) { return !s || s->mask == 0; };
			auto clearAll = [&]() {
				for (GuestFdSet* s : {readSet, writeSet, exceptSet})
				{
					if (s)
						s->mask = 0;
				}
			};
			// nothing to wait for: answer right away (Lost Reavers relies on getting 0 here)
			if (empty(readSet) && empty(writeSet) && empty(exceptSet))
				return 0;
			if (!timeout)
				return 0;

			const uint64 timeoutMs = (uint64)timeout->sec * 1000 + timeout->usec / 1000;
			const uint32 start = GetTickCount();
			while (true)
			{
				fd_set r, w, e;
				int maxHost = -1;
				ToHostSet(readSet, nfds, r, maxHost);
				ToHostSet(writeSet, nfds, w, maxHost);
				ToHostSet(exceptSet, nfds, e, maxHost);
				timeval poll{0, 0};
				const sint32 n = ::select(maxHost + 1, readSet ? &r : nullptr, writeSet ? &w : nullptr, exceptSet ? &e : nullptr, &poll);
				if (n < 0)
				{
					ReportResult(n, GETLASTERR);
					clearAll();
					return n;
				}
				if (n > 0)
				{
					ToGuestSet(readSet, r);
					ToGuestSet(writeSet, w);
					ToGuestSet(exceptSet, e);
					return n;
				}
				if (GetTickCount() - start >= timeoutMs)
				{
					SetErrno(kErrNone);
					clearAll();
					return 0;
				}
				// 0.3 ms, short enough for games that select in a tight loop
				SleepTicks(coreinit::EspressoTime::ConvertNsToTimerTicks(300000));
			}
		}

		// ---- names and addresses ------------------------------------------------

		sint32 getsockname(sint32 handle, GuestSockAddr* addr, uint32be* len)
		{
			Socket* s = Lookup(handle);
			if (!s)
				return 0;
			sockaddr host{};
			socklen_t hostLen = sizeof(host);
			if (::getsockname(s->host, &host, &hostLen) == 0)
				ToGuestAddr(host, addr);
			return 0;
		}

		sint32 getpeername(sint32 handle, GuestSockAddr* addr, uint32be* len)
		{
			Socket* s = Lookup(handle);
			if (!s)
				return -1;
			sockaddr host{};
			socklen_t hostLen = sizeof(host);
			const sint32 r = ReportResult(::getpeername(s->host, &host, &hostLen), GETLASTERR);
			ToGuestAddr(host, addr);
			*len = 16;
			return r;
		}

		sint32 inet_aton(const char* text, GuestInAddr* out)
		{
			uint32 ip;
			if (!ParseDottedQuad(text, ip))
				return 0;
			out->addr = ip;
			return 1;
		}

		sint32 inet_pton(sint32 af, const char* text, GuestInAddr* out)
		{
			if (af != kAfInet)
			{
				cemuLog_log(LogType::Force, "nsysnet: inet_pton only supports AF_INET");
				return 0;
			}
			uint32 ip;
			if (!ParseDottedQuad(text, ip))
			{
				SetErrno(kErrAfNoSupport);
				return 0;
			}
			out->addr = ip;
			return 1;
		}

		const char* inet_ntop(sint32 af, const void* src, char* dst, uint32 size)
		{
			if (af != kAfInet)
			{
				SetErrno(kErrAfNoSupport);
				return nullptr;
			}
			const uint8* b = (const uint8*)src;
			const std::string text = fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
			if (text.size() + 1 > size)
			{
				SetErrno(kErrAfNoSupport);
				return nullptr;
			}
			memcpy(dst, text.c_str(), text.size() + 1);
			return dst;
		}

		SysAllocator<char, 64> g_ntoaBuffer;

		char* inet_ntoa(GuestInAddr* in)
		{
			const uint8* b = (const uint8*)in;
			const std::string text = fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
			strcpy(g_ntoaBuffer.GetPtr(), text.c_str());
			return g_ntoaBuffer.GetPtr();
		}

		// the guest's values are already in network order
		// glibc turns these four names into __bswap macros when optimising, drop them here
#ifdef htons
#undef htons
#endif
#ifdef htonl
#undef htonl
#endif
#ifdef ntohs
#undef ntohs
#endif
#ifdef ntohl
#undef ntohl
#endif
		uint32 htons(uint32 v) { return v; }
		uint32 htonl(uint32 v) { return v; }
		uint32 ntohs(uint32 v) { return v; }
		uint32 ntohl(uint32 v) { return v; }

		template<typename T>
		T* AllocSys(uint32 size)
		{
			MPTR p = coreinit_allocFromSysArea(size, 4);
			memset(memory_getPointerFromVirtualOffset(p), 0, size);
			return (T*)memory_getPointerFromVirtualOffset(p);
		}

		char* AllocSysString(const char* s)
		{
			char* out = AllocSys<char>((uint32)strlen(s) + 1);
			strcpy(out, s);
			return out;
		}

		GuestHostEnt* gethostbyname(const char* name)
		{
			hostent* he = ::gethostbyname(name);
			if (!he)
				return nullptr;
			// every lookup gets its own copy in the system area
			auto* ent = AllocSys<GuestHostEnt>(sizeof(GuestHostEnt));
			auto* list = AllocSys<MEMPTR<GuestInAddr>>(sizeof(MEMPTR<GuestInAddr>) * 2);
			auto* addr = AllocSys<GuestInAddr>(sizeof(GuestInAddr));
			ent->name = AllocSysString(he->h_name ? he->h_name : "");
			ent->aliases = nullptr;
			ent->addrType = he->h_addrtype;
			ent->length = he->h_length;
			memcpy(addr, he->h_addr_list[0], sizeof(GuestInAddr)); // network order already
			list[0] = addr;
			list[1] = nullptr;
			ent->addrList = list;
			return ent;
		}

		constexpr sint32 kHostEntMaxAddrs = 31;
		SysAllocator<GuestHostEnt> g_hostEnt;
		SysAllocator<char, 256> g_hostEntName;
		SysAllocator<MEMPTR<GuestInAddr>, kHostEntMaxAddrs + 1> g_hostEntList;
		SysAllocator<GuestInAddr, kHostEntMaxAddrs> g_hostEntAddrs;

		GuestHostEnt* gethostbyaddr(const char* addr, sint32 len, sint32 type)
		{
			hostent* he = ::gethostbyaddr(addr, len, type);
			if (!he)
			{
				cemuLog_log(LogType::Socket, "nsysnet: gethostbyaddr failed");
				return nullptr;
			}
			GuestHostEnt* ent = g_hostEnt.GetPtr();
			char* name = g_hostEntName.GetPtr();
			if (he->h_name && strlen(he->h_name) < 255)
				strcpy(name, he->h_name);
			else
				name[0] = '\0';
			ent->name = name;
			ent->aliases = nullptr;
			ent->addrType = kAfInet;
			ent->length = sizeof(GuestInAddr);
			MEMPTR<GuestInAddr>* list = g_hostEntList.GetPtr();
			sint32 i = 0;
			for (; i < kHostEntMaxAddrs && he->h_addr_list[i]; i++)
			{
				memcpy(g_hostEntAddrs.GetPtr() + i, he->h_addr_list[i], sizeof(GuestInAddr));
				list[i] = g_hostEntAddrs.GetPtr() + i;
			}
			list[i] = nullptr;
			ent->addrList = list;
			return ent;
		}

		sint32 getaddrinfo(const char* node, const char* service, GuestAddrInfo* hints, MEMPTR<GuestAddrInfo>* results)
		{
			addrinfo hint{};
			if (hints)
			{
				hint.ai_family = hints->family;
				hint.ai_socktype = hints->sockType;
				hint.ai_flags = hints->flags;
				hint.ai_protocol = hints->protocol;
			}
			addrinfo* found = nullptr;
			const sint32 hr = ::getaddrinfo(node, service, &hint, &found);
			if (hr != 0)
			{
				cemuLog_log(LogType::Socket, "nsysnet: getaddrinfo({}) failed with {}", node ? node : "", hr);
				return hr == WSAHOST_NOT_FOUND ? kErrConnReset : 1;
			}
			// IPv4 entries only, all in one system-area block
			std::vector<const addrinfo*> v4;
			for (const addrinfo* a = found; a; a = a->ai_next)
			{
				if (a->ai_addrlen == 16)
					v4.push_back(a);
			}
			const uint32 count = (uint32)v4.size();
			if (count == 0)
				cemuLog_log(LogType::Force, "nsysnet: getaddrinfo({}) found no IPv4 address", node ? node : "");
			auto* entries = AllocSys<GuestAddrInfo>((sizeof(GuestAddrInfo) + sizeof(GuestSockAddr)) * std::max<uint32>(count, 1));
			auto* addrs = (GuestSockAddr*)(entries + count);
			for (uint32 i = 0; i < count; i++)
			{
				const addrinfo* a = v4[i];
				GuestAddrInfo& e = entries[i];
				e.flags = a->ai_flags;
				e.family = a->ai_family;
				e.sockType = a->ai_socktype;
				e.protocol = a->ai_protocol;
				e.addrLen = (uint32)a->ai_addrlen;
				e.canonName = nullptr;
				ToGuestAddr(*a->ai_addr, addrs + i);
				e.addr = addrs + i;
				e.next = (i + 1 < count) ? &entries[i + 1] : nullptr;
			}
			freeaddrinfo(found);
			*results = entries;
			return 0;
		}

		// ---- NSSL ---------------------------------------------------------------

		constexpr uint32 kNsslOk = 0;
		constexpr uint32 kNsslInvalidContext = 0xFFD7FFFF;

		std::mutex g_nsslMutex;
		std::deque<NsslContext> g_nssl; // handle = index; deque keeps references stable

		NsslContext* LiveContext(uint32 handle)
		{
			if (handle >= g_nssl.size() || g_nssl[handle].destroyed)
				return nullptr;
			return &g_nssl[handle];
		}

		uint32 NSSLCreateContext(uint32 version)
		{
			std::lock_guard lock(g_nsslMutex);
			NsslContext ctx;
			ctx.sslVersion = version;
			g_nssl.push_back(std::move(ctx));
			return (uint32)g_nssl.size() - 1;
		}

		uint32 NSSLDestroyContext(uint32 handle)
		{
			std::lock_guard lock(g_nsslMutex);
			NsslContext* ctx = LiveContext(handle);
			if (!ctx)
				return kNsslInvalidContext;
			ctx->destroyed = true;
			return kNsslOk;
		}

		uint32 NSSLSetClientPKI(uint32 handle, uint32 certId)
		{
			std::lock_guard lock(g_nsslMutex);
			NsslContext* ctx = LiveContext(handle);
			if (!ctx)
				return kNsslInvalidContext;
			ctx->clientCertId = certId;
			return kNsslOk;
		}

		uint32 NSSLAddServerPKI(uint32 handle, uint32 certId)
		{
			std::lock_guard lock(g_nsslMutex);
			NsslContext* ctx = LiveContext(handle);
			if (!ctx)
				return kNsslInvalidContext;
			ctx->serverCertIds.insert(certId);
			return kNsslOk;
		}

		uint32 NSSLAddServerPKIExternal(uint32 handle, uint8* cert, sint32 certLen, sint32 certType)
		{
			std::lock_guard lock(g_nsslMutex);
			NsslContext* ctx = LiveContext(handle);
			if (!ctx)
				return kNsslInvalidContext;
			ctx->serverCertsExternal.emplace_back(cert, cert + std::max(certLen, 0));
			return kNsslOk;
		}

		uint32 NSSLAddServerPKIGroups(uint32 handle, uint32 groups, sint32be* validCount, sint32be* invalidCount)
		{
			std::lock_guard lock(g_nsslMutex);
			NsslContext* ctx = LiveContext(handle);
			if (!ctx)
				return kNsslInvalidContext;
			if (groups & 1) // Nintendo CAs
			{
				for (uint32 id = 100; id <= 105; id++)
					ctx->serverCertIds.insert(id);
			}
			if (groups & 2) // commercial CAs
			{
				for (uint32 id = 1001; id <= 1033; id++)
					ctx->serverCertIds.insert(id);
			}
			if (validCount)
				*validCount = (sint32)ctx->serverCertIds.size();
			if (invalidCount)
				*invalidCount = 0;
			return kNsslOk;
		}

		uint32 NSSLExportInternalServerCertificate(uint32 certId, uint8* out, uint32be* outSize, uint32be* certType)
		{
			sint32 size = 0;
			const uint8* data = iosuCrypto_getCertificateDataById(certId, &size);
			if (!data)
			{
				cemuLog_log(LogType::Force, "nsysnet: unknown internal certificate {}", certId);
				size = 0;
			}
			if (out && data)
				memcpy(out, data, size);
			*outSize = (uint32)size;
			*certType = 0;
			return kNsslOk;
		}

		uint32 NSSLExportInternalClientCertificate(uint32 certId, uint8* out, uint32be* outSize, uint32be* certType, uint8* keyOut, uint32be* keySize, uint32be* keyType)
		{
			NSSLExportInternalServerCertificate(certId, out, outSize, certType);
			sint32 size = 0;
			const uint8* key = iosuCrypto_getCertificatePrivateKeyById(certId, &size);
			if (!key)
			{
				cemuLog_log(LogType::Force, "nsysnet: no private key for certificate {}", certId);
				size = 0;
			}
			if (keyOut && key)
				memcpy(keyOut, key, size);
			*keySize = (uint32)size;
			*keyType = 0;
			return kNsslOk;
		}
	}

	// ---- interface for nlibcurl ---------------------------------------------

	void FdSetClear(GuestFdSet* set)
	{
		set->mask = 0;
	}

	void FdSetAdd(GuestFdSet* set, sint32 handle)
	{
		if (handle >= 0 && handle < 32)
			set->mask = set->mask | (1u << handle);
	}

	sint32 GuestHandleOfHostSocket(SOCKET s)
	{
		std::lock_guard lock(g_tableMutex);
		for (sint32 i = 0; i < kMaxSockets; i++)
		{
			if (g_sockets[i] && g_sockets[i]->host == s)
				return i + 1;
		}
		return -1;
	}

	sint32 AdoptHostSocket(SOCKET hostSocket)
	{
		Socket s{};
		s.host = hostSocket;
		s.family = kAfInet;
		int type = 0;
		socklen_t len = sizeof(type);
		if (::getsockopt(hostSocket, SOL_SOCKET, SO_TYPE, (char*)&type, &len) == 0)
			s.type = type;
		s.protocol = s.type == kSockDgram ? kProtoUdp : kProtoTcp;
		const sint32 handle = Insert(s);
		if (handle < 0)
			cemuLog_log(LogType::Force, "nsysnet: out of socket handles");
		return handle;
	}

	void ForgetHostSocket(SOCKET s)
	{
		std::lock_guard lock(g_tableMutex);
		for (auto& entry : g_sockets)
		{
			if (entry && entry->host == s)
			{
				entry.reset();
				return;
			}
		}
	}

	const NsslContext* GetNsslContext(uint32 handle)
	{
		std::lock_guard lock(g_nsslMutex);
		if (handle >= g_nssl.size())
			return nullptr;
		if (g_nssl[handle].destroyed)
			cemuLog_logDebug(LogType::Force, "nsysnet: NSSL context {} used after it was destroyed", handle);
		return &g_nssl[handle];
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nsysnet";
		}

		void RPLMapped() override
		{
			cafeExportRegister("nsysnet", socket_lib_init, LogType::Socket);
			cafeExportRegister("nsysnet", socket_lib_finish, LogType::Socket);
			cafeExportRegister("nsysnet", socketlasterr, LogType::Socket);

			cafeExportRegister("nsysnet", socket, LogType::Socket);
			cafeExportRegister("nsysnet", mw_socket, LogType::Socket);
			cafeExportRegister("nsysnet", shutdown, LogType::Socket);
			cafeExportRegister("nsysnet", socketclose, LogType::Socket);
			cafeExportRegister("nsysnet", setsockopt, LogType::Socket);
			cafeExportRegister("nsysnet", getsockopt, LogType::Socket);
			cafeExportRegister("nsysnet", bind, LogType::Socket);
			cafeExportRegister("nsysnet", listen, LogType::Socket);
			cafeExportRegister("nsysnet", accept, LogType::Socket);
			cafeExportRegister("nsysnet", connect, LogType::Socket);
			cafeExportRegister("nsysnet", send, LogType::Socket);
			cafeExportRegister("nsysnet", recv, LogType::Socket);
			cafeExportRegister("nsysnet", recvfrom, LogType::Socket);
			cafeExportRegister("nsysnet", recvfrom_ex, LogType::Socket);
			cafeExportRegister("nsysnet", sendto, LogType::Socket);
			cafeExportRegister("nsysnet", sendto_multi, LogType::Socket);
			cafeExportRegister("nsysnet", sendto_multi_ex, LogType::Socket);
			cafeExportRegister("nsysnet", select, LogType::Socket);
			cafeExportRegister("nsysnet", getsockname, LogType::Socket);
			cafeExportRegister("nsysnet", getpeername, LogType::Socket);

			cafeExportRegister("nsysnet", inet_aton, LogType::Socket);
			cafeExportRegister("nsysnet", inet_pton, LogType::Socket);
			cafeExportRegister("nsysnet", inet_ntop, LogType::Socket);
			cafeExportRegister("nsysnet", inet_ntoa, LogType::Socket);
			cafeExportRegister("nsysnet", htons, LogType::Socket);
			cafeExportRegister("nsysnet", htonl, LogType::Socket);
			cafeExportRegister("nsysnet", ntohs, LogType::Socket);
			cafeExportRegister("nsysnet", ntohl, LogType::Socket);
			cafeExportRegister("nsysnet", gethostbyname, LogType::Socket);
			cafeExportRegister("nsysnet", gethostbyaddr, LogType::Socket);
			cafeExportRegister("nsysnet", getaddrinfo, LogType::Socket);

			cafeExportRegister("nsysnet", NSSLCreateContext, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLDestroyContext, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLSetClientPKI, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLAddServerPKI, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLAddServerPKIExternal, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLAddServerPKIGroups, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLExportInternalServerCertificate, LogType::Socket);
			cafeExportRegister("nsysnet", NSSLExportInternalClientCertificate, LogType::Socket);
		}
	} s_nsysnetModule;

	COSModule* GetModule()
	{
		return &s_nsysnetModule;
	}
}
