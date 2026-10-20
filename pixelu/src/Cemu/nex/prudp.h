// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "nexTypes.h"
#include "Common/socket.h"

// PRUDP version 0: the reliable UDP transport used by NEX servers.

#define RC4_N 256

// RC4 stream cipher state
struct RC4Ctx
{
	unsigned char S[RC4_N];
	int i;
	int j;
};

void RC4_initCtx(RC4Ctx* rc4Ctx, const char* key);
void RC4_initCtx(RC4Ctx* rc4Ctx, unsigned char* key, int keyLen);
void RC4_transform(RC4Ctx* rc4Ctx, unsigned char* input, int len, unsigned char* output);

struct prudpStreamSettings
{
	uint8 checksumBase; // sum of the access key characters
	uint8 accessKeyDigest[16]; // md5 of the access key, used to sign data packets
	RC4Ctx rc4Client; // encrypts what we send
	RC4Ctx rc4Server; // decrypts what we receive
};

struct prudpStationUrl
{
	uint32 ip;
	uint16 port;
	sint32 cid;
	sint32 pid;
	sint32 sid;
	sint32 stream;
	sint32 type;
};

struct prudpAuthServerInfo
{
	uint32 userPid;
	uint8 secureKey[16];
	uint8 kerberosKey[16];
	uint8 secureTicket[1024];
	sint32 secureTicketLength;
	prudpStationUrl server;
};

// a packet we send
class prudpPacket
{
public:
	static const int PACKET_RAW_SIZE_MAX = 500;

	static const int TYPE_SYN = 0;
	static const int TYPE_CON = 1;
	static const int TYPE_DATA = 2;
	static const int TYPE_DISCONNECT = 3;
	static const int TYPE_PING = 4;

	static const int STREAM_TYPE_SECURE = 0xA;

	static const int FLAG_ACK = 0x1;
	static const int FLAG_RELIABLE = 0x2;
	static const int FLAG_NEED_ACK = 0x4;
	static const int FLAG_HAS_SIZE = 0x8;

	// size of the first packet in a datagram, 0 if it is truncated
	static sint32 calculateSizeFromPacketData(uint8* data, sint32 length);

	prudpPacket(prudpStreamSettings* streamSettings, uint8 src, uint8 dst, uint8 type, uint16 flags, uint8 sessionId, uint16 sequenceId, uint32 packetSignature);
	bool requiresAck();
	void setData(uint8* data, sint32 length);
	void setFragmentIndex(uint8 fragmentIndex);
	// serialises the packet, encrypting a data payload on first use; returns the size
	// or a value above maxLength if it does not fit
	sint32 buildData(uint8* output, sint32 maxLength);
	uint8 GetType() const { return m_type; }
	uint16 GetSequenceId() const { return m_sequenceId; }

private:
	uint32 Signature() const;

	prudpStreamSettings* m_settings;
	uint8 m_src;
	uint8 m_dst;
	uint8 m_type;
	uint16 m_flags;
	uint8 m_sessionId;
	uint16 m_sequenceId;
	uint32 m_signature;
	uint8 m_fragmentIndex{0};
	bool m_payloadEncrypted{false};
	std::vector<uint8> m_payload;
};

// a packet we received
class prudpIncomingPacket
{
public:
	prudpIncomingPacket(prudpStreamSettings* streamSettings, uint8* data, sint32 length);

	bool hasError();

	uint8 calculateChecksum(uint8* data, sint32 length);
	void decrypt();

public:
	uint8 src{};
	uint8 dst{};
	uint16 flags{};
	uint8 type{};
	uint8 sessionId{};
	uint32 packetSignature{};
	uint16 sequenceId{};
	uint8 fragmentIndex{};
	bool hasData = false;
	std::vector<uint8> packetData;

private:
	bool m_invalid{false};
	prudpStreamSettings* m_settings{nullptr};
};

class prudpClient
{
public:
	enum class ConnectionState : uint8
	{
		Connecting,
		Connected,
		Disconnected
	};

	prudpClient(uint32 dstIp, uint16 dstPort, const char* key);
	prudpClient(uint32 dstIp, uint16 dstPort, const char* key, prudpAuthServerInfo* authInfo);
	~prudpClient();

	bool IsConnected() const { return m_state == ConnectionState::Connected; }
	ConnectionState GetConnectionState() const { return m_state; }
	uint16 GetSourcePort() const { return m_localPort; }

	// processes received packets, retransmissions and pings
	bool Update();

	// next complete message in order, -1 if none is ready
	sint32 ReceiveDatagram(std::vector<uint8>& outputBuffer);
	void SendDatagram(uint8* input, sint32 length, bool reliable = true);

private:
	struct Unacked
	{
		std::unique_ptr<prudpPacket> packet;
		uint32 lastSent;
		sint32 resends;
	};

	bool OpenSocket();
	void Transmit(prudpPacket& packet);
	void SendHandshake();
	std::vector<uint8> BuildConnectPayload();
	void OnPacket(std::unique_ptr<prudpIncomingPacket> packet);
	void OnPing(const prudpIncomingPacket& packet);
	void OnSynAck(const prudpIncomingPacket& packet);
	void OnConAck(const prudpIncomingPacket& packet);
	void OnData(std::unique_ptr<prudpIncomingPacket> packet);
	void CheckRetransmits(uint32 now);
	bool CheckHandshakeTimeout(uint32 now);
	bool CheckPing(uint32 now);
	void SendPing();

private:
	SOCKET m_socket{};
	uint16 m_localPort{0};
	uint32 m_serverIp;
	uint16 m_serverPort;
	uint8 m_localVPort;
	uint8 m_serverVPort;
	prudpStreamSettings m_settings{};

	ConnectionState m_state{ConnectionState::Connecting};
	bool m_gotSynAck{false};
	bool m_gotConAck{false};
	uint32 m_lastHandshakeSent{0};
	uint8 m_handshakeSends{0};
	uint32 m_serverSignature{0};
	uint32 m_clientSignature{0};
	uint8 m_clientSession{0};
	uint8 m_serverSession{0};

	uint16 m_nextSendSeq{2}; // 1 is taken by the CON packet
	uint16 m_nextRecvSeq{0};
	std::vector<Unacked> m_unacked;
	std::vector<std::unique_ptr<prudpIncomingPacket>> m_recvQueue; // sorted by sequence id

	uint16 m_pingSeq{0};
	uint8 m_pingsUnanswered{0};
	uint32 m_lastPingSent{0};

	bool m_secure{false};
	prudpAuthServerInfo m_auth{};
};

uint32 prudpGetMSTimestamp();
