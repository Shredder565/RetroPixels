// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// BSD sockets with the Winsock names used across the code base.

#if BOOST_OS_WINDOWS

	#include <WinSock2.h>
	#include <ws2tcpip.h>

typedef int socklen_t;
	#define GETLASTERR WSAGetLastError()

#elif BOOST_OS_UNIX

	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netdb.h>
	#include <unistd.h>
	#include <errno.h>
	#include <fcntl.h>

	#define GETLASTERR errno

	#define SOCKET int
	#define INVALID_SOCKET -1
	#define SOCKET_ERROR -1
	#define closesocket close

	#define WSAEWOULDBLOCK EWOULDBLOCK
	#define WSAEINPROGRESS EINPROGRESS
	#define WSAENOTCONN ENOTCONN
	#define WSAESHUTDOWN ESHUTDOWN
	#define WSAECONNABORTED ECONNABORTED
	#define WSAHOST_NOT_FOUND EAI_NONAME

#endif
