// Original code for RetroPixels (PixelU). Not derived from Cemu.
// nsysnet: the Wii U socket library (BSD-style sockets, DNS) and NSSL, the
// SSL context library nlibcurl uses for HTTPS.
#pragma once
#include <set>
#include <vector>
#include "Cafe/OS/RPL/COSModule.h"

#if BOOST_OS_WINDOWS
#include <WinSock2.h>
#else
#include <sys/socket.h>
#define SOCKET int
#define closesocket close
#endif

namespace nsysnet
{
	// a guest fd_set: bit n stands for socket handle n
	struct GuestFdSet
	{
		uint32be mask;
	};

	void FdSetClear(GuestFdSet* set);
	void FdSetAdd(GuestFdSet* set, sint32 handle);

	// host sockets opened by our nlibcurl get guest handles so that games can
	// select() on them (curl_multi_fdset)
	sint32 GuestHandleOfHostSocket(SOCKET s);  // -1 if none
	sint32 AdoptHostSocket(SOCKET s);          // new handle, -1 if the table is full
	void ForgetHostSocket(SOCKET s);           // drop the handle, the socket is closed by its owner

	// an NSSL context: which certificates an HTTPS connection trusts and presents
	struct NsslContext
	{
		bool destroyed = false;
		uint32 sslVersion = 0;
		uint32 clientCertId = 0;              // 0 = none
		std::set<uint32> serverCertIds;       // console-internal CA certificates
		std::vector<std::vector<uint8>> serverCertsExternal; // CA certificates supplied by the game
	};

	// nullptr for unknown handles; destroyed contexts are still returned
	const NsslContext* GetNsslContext(uint32 handle);

	COSModule* GetModule();
}
