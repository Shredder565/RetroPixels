// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "prudp.h"
#include "util/crypto/md5.h"

#include <bitset>
#include <random>

#include <boost/random/uniform_int.hpp>

// Packet layout (PRUDP v0, little endian):
//   0  u8  source vport        1  u8  destination vport
//   2  u16 type | flags << 4   4  u8  session id
//   5  u32 signature           9  u16 sequence id
//   11 type specific fields: SYN u32 connection signature (0), CON u32 signature
//      plus optional login data, DATA u8 fragment index and payload, PING nothing
//   last byte: checksum
// With FLAG_HAS_SIZE a u16 payload size follows the type specific fields.

// --- RC4 ---

void RC4_initCtx(RC4Ctx* rc4Ctx, unsigned char* key, int keyLen)
{
	unsigned char* S = rc4Ctx->S;
	for (int n = 0; n < RC4_N; n++)
		S[n] = (unsigned char)n;
	unsigned char k = 0;
	for (int n = 0; n < RC4_N; n++)
	{
		k = (unsigned char)(k + S[n] + key[n % keyLen]);
		std::swap(S[n], S[k]);
	}
	rc4Ctx->i = 0;
	rc4Ctx->j = 0;
}

void RC4_initCtx(RC4Ctx* rc4Ctx, const char* key)
{
	RC4_initCtx(rc4Ctx, (unsigned char*)key, (int)strlen(key));
}

void RC4_transform(RC4Ctx* rc4Ctx, unsigned char* input, int len, unsigned char* output)
{
	unsigned char* S = rc4Ctx->S;
	unsigned char a = (unsigned char)rc4Ctx->i;
	unsigned char b = (unsigned char)rc4Ctx->j;
	for (int n = 0; n < len; n++)
	{
		a++;
		b = (unsigned char)(b + S[a]);
		std::swap(S[a], S[b]);
		output[n] = input[n] ^ S[(unsigned char)(S[a] + S[b])];
	}
	rc4Ctx->i = a;
	rc4Ctx->j = b;
}

// --- shared helpers ---

uint32 prudpGetMSTimestamp()
{
	return GetTickCount();
}

namespace
{
	constexpr uint32 kAckTimeoutMs = 2300;
	constexpr sint32 kMaxResends = 7;
	constexpr uint32 kHandshakeTimeoutMs = 1200;
	constexpr uint8 kMaxHandshakeSends = 5;
	constexpr uint32 kPingIntervalMs = 20000;
	constexpr uint32 kPingRetryMs = 1500;
	constexpr uint8 kMaxUnansweredPings = 10;
	constexpr sint32 kHeaderSize = 0xB;
	constexpr uint16 kLocalPortBase = 40000;
	constexpr int kLocalPortRange = 10000;

	std::mt19937_64 s_random(prudpGetMSTimestamp());
	boost::random::uniform_int_distribution<int> s_byteDist(0, 0xFF);
	boost::random::uniform_int_distribution<int> s_portDist(0, kLocalPortRange);
	std::bitset<kLocalPortRange + 1> s_portsInUse;

	uint8 RandomU8()
	{
		return (uint8)s_byteDist(s_random);
	}

	uint32 RandomU32()
	{
		uint32 v = 0;
		for (int n = 0; n < 4; n++)
			v = (v << 8) | RandomU8();
		return v;
	}

	uint16 ReservePort()
	{
		while (true)
		{
			const int slot = s_portDist(s_random);
			if (s_portsInUse.test(slot))
				continue;
			s_portsInUse.set(slot);
			return (uint16)(kLocalPortBase + slot);
		}
	}

	void ReleasePort(uint16 port)
	{
		cemu_assert_debug(port >= kLocalPortBase && s_portsInUse.test(port - kLocalPortBase));
		s_portsInUse.reset(port - kLocalPortBase);
	}

	template<typename T>
	T LoadLE(const uint8* p)
	{
		T v;
		memcpy(&v, p, sizeof(T));
		return v;
	}

	template<typename T>
	void StoreLE(uint8* p, T v)
	{
		memcpy(p, &v, sizeof(T));
	}

	// the base plus every byte, except that whole 32 bit words are first
	// summed as words and the four bytes of that sum are added
	uint8 Checksum(uint8 base, const uint8* data, sint32 length)
	{
		const sint32 wordBytes = length & ~3;
		uint32 wordSum = 0;
		for (sint32 n = 0; n < wordBytes; n += 4)
			wordSum += LoadLE<uint32>(data + n);
		uint8 sum = base;
		for (sint32 n = wordBytes; n < length; n++)
			sum += data[n];
		for (int shift = 0; shift < 32; shift += 8)
			sum += (uint8)(wordSum >> shift);
		return sum;
	}

	constexpr uint8 VPort(uint8 streamType, uint8 port)
	{
		return (uint8)((streamType << 4) | port);
	}
}

// --- outgoing packets ---

sint32 prudpPacket::calculateSizeFromPacketData(uint8* data, sint32 length)
{
	if (length < kHeaderSize)
		return 0;
	const uint16 typeAndFlags = LoadLE<uint16>(data + 2);
	if (((typeAndFlags >> 4) & FLAG_HAS_SIZE) == 0)
		return length; // no size field, the packet fills the datagram
	// offset of the size field
	sint32 sizeOffset;
	switch (typeAndFlags & 0xF)
	{
	case TYPE_SYN:
	case TYPE_CON:
		sizeOffset = kHeaderSize + 4;
		break;
	case TYPE_DATA:
		sizeOffset = kHeaderSize + 1;
		break;
	case TYPE_PING:
		sizeOffset = kHeaderSize;
		break;
	default:
		return length; // size field position unknown for this type
	}
	if (length < sizeOffset + 2)
		return 0;
	const sint32 total = sizeOffset + 2 + LoadLE<uint16>(data + sizeOffset) + 1;
	return total <= length ? total : 0;
}

prudpPacket::prudpPacket(prudpStreamSettings* streamSettings, uint8 src, uint8 dst, uint8 type, uint16 flags, uint8 sessionId, uint16 sequenceId, uint32 packetSignature)
	: m_settings(streamSettings), m_src(src), m_dst(dst), m_type(type), m_flags(flags), m_sessionId(sessionId), m_sequenceId(sequenceId), m_signature(packetSignature)
{
}

bool prudpPacket::requiresAck()
{
	return (m_flags & FLAG_NEED_ACK) != 0;
}

void prudpPacket::setData(uint8* data, sint32 length)
{
	m_payload.assign(data, data + length);
}

void prudpPacket::setFragmentIndex(uint8 fragmentIndex)
{
	m_fragmentIndex = fragmentIndex;
}

uint32 prudpPacket::Signature() const
{
	switch (m_type)
	{
	case TYPE_CON:
	case TYPE_PING:
		return m_signature;
	case TYPE_DATA:
	{
		if (m_payload.empty())
			return 0x12345678;
		// first four bytes of hmac-md5(md5(access key), encrypted payload)
		uint8 mac[16];
		hmacMD5(m_settings->accessKeyDigest, 16, m_payload.data(), (int)m_payload.size(), mac);
		return LoadLE<uint32>(mac);
	}
	default:
		return 0;
	}
}

sint32 prudpPacket::buildData(uint8* output, sint32 maxLength)
{
	sint32 bodySize = 0;
	if (m_type == TYPE_SYN)
		bodySize = 4;
	else if (m_type == TYPE_CON)
		bodySize = (sint32)m_payload.size();
	else if (m_type == TYPE_DATA)
		bodySize = 1 + (sint32)m_payload.size();
	const sint32 total = kHeaderSize + bodySize + 1;
	if (total > maxLength)
		return total;
	// data payloads are encrypted once, resends reuse the result
	if (m_type == TYPE_DATA && !m_payload.empty() && !m_payloadEncrypted)
	{
		RC4_transform(&m_settings->rc4Client, m_payload.data(), (int)m_payload.size(), m_payload.data());
		m_payloadEncrypted = true;
	}
	output[0] = m_src;
	output[1] = m_dst;
	StoreLE<uint16>(output + 2, (uint16)((m_flags << 4) | m_type));
	output[4] = m_sessionId;
	StoreLE<uint32>(output + 5, Signature());
	StoreLE<uint16>(output + 9, m_sequenceId);
	uint8* body = output + kHeaderSize;
	if (m_type == TYPE_SYN)
	{
		StoreLE<uint32>(body, 0);
	}
	else if (m_type == TYPE_CON)
	{
		if (!m_payload.empty())
			memcpy(body, m_payload.data(), m_payload.size());
	}
	else if (m_type == TYPE_DATA)
	{
		body[0] = m_fragmentIndex;
		if (!m_payload.empty())
			memcpy(body + 1, m_payload.data(), m_payload.size());
	}
	else if (m_type != TYPE_PING)
	{
		cemu_assert_suspicious();
	}
	output[total - 1] = Checksum(m_settings->checksumBase, output, total - 1);
	return total;
}

// --- incoming packets ---

prudpIncomingPacket::prudpIncomingPacket(prudpStreamSettings* streamSettings, uint8* data, sint32 length)
{
	if (length < kHeaderSize + 1)
	{
		m_invalid = true;
		return;
	}
	m_settings = streamSettings;
	const sint32 end = length - 1; // without the checksum
	if (calculateChecksum(data, end) != data[end])
	{
		m_invalid = true;
		return;
	}
	src = data[0];
	dst = data[1];
	const uint16 typeAndFlags = LoadLE<uint16>(data + 2);
	type = typeAndFlags & 0xF;
	flags = (typeAndFlags >> 4) & 0xFFF;
	sessionId = data[4];
	packetSignature = LoadLE<uint32>(data + 5);
	sequenceId = LoadLE<uint16>(data + 9);
	sint32 pos = kHeaderSize;
	if (type == prudpPacket::TYPE_SYN)
	{
		// the server's connection signature; a size field is ignored
		if (end - pos < 4)
		{
			m_invalid = true;
			return;
		}
		packetData.assign(data + pos, data + pos + 4);
		hasData = true;
	}
	else if (type == prudpPacket::TYPE_DATA)
	{
		const bool sized = (flags & prudpPacket::FLAG_HAS_SIZE) != 0;
		if (end - pos < (sized ? 3 : 1))
		{
			m_invalid = true;
			return;
		}
		fragmentIndex = data[pos];
		pos += sized ? 3 : 1; // the size field always matches since it was used to split the datagram
		if (pos < end)
			packetData.assign(data + pos, data + end);
	}
	else if (type != prudpPacket::TYPE_CON && type != prudpPacket::TYPE_PING && type != prudpPacket::TYPE_DISCONNECT)
	{
		cemu_assert_suspicious();
	}
	// CON, PING and DISCONNECT carry nothing we need
}

bool prudpIncomingPacket::hasError()
{
	return m_invalid;
}

uint8 prudpIncomingPacket::calculateChecksum(uint8* data, sint32 length)
{
	return Checksum(m_settings->checksumBase, data, length);
}

void prudpIncomingPacket::decrypt()
{
	if (!packetData.empty())
		RC4_transform(&m_settings->rc4Server, packetData.data(), (int)packetData.size(), packetData.data());
}

// --- connection ---

prudpClient::prudpClient(uint32 dstIp, uint16 dstPort, const char* key)
	: m_serverIp(dstIp), m_serverPort(dstPort)
{
	if (!OpenSocket())
	{
		cemuLog_log(LogType::Force, "PRUDP: Failed to bind UDP socket");
		m_state = ConnectionState::Disconnected;
		return;
	}
	m_localVPort = VPort(prudpPacket::STREAM_TYPE_SECURE, 0xF);
	m_serverVPort = VPort(prudpPacket::STREAM_TYPE_SECURE, 0x1);
	uint8 keySum = 0;
	for (const char* c = key; *c; c++)
		keySum += (uint8)*c;
	m_settings.checksumBase = keySum;
	MD5_CTX md5;
	MD5_Init(&md5);
	MD5_Update(&md5, key, (int)strlen(key));
	MD5_Final(m_settings.accessKeyDigest, &md5);
	RC4_initCtx(&m_settings.rc4Client, "CD&ML");
	RC4_initCtx(&m_settings.rc4Server, "CD&ML");
	SendHandshake();
	m_nextRecvSeq = 1;
}

prudpClient::prudpClient(uint32 dstIp, uint16 dstPort, const char* key, prudpAuthServerInfo* authInfo)
	: prudpClient(dstIp, dstPort, key)
{
	// the secure server uses the session key from the ticket for both directions
	RC4_initCtx(&m_settings.rc4Client, authInfo->secureKey, 16);
	RC4_initCtx(&m_settings.rc4Server, authInfo->secureKey, 16);
	m_secure = true;
	m_auth = *authInfo;
}

prudpClient::~prudpClient()
{
	if (m_localPort == 0)
		return;
	ReleasePort(m_localPort);
	closesocket(m_socket);
}

bool prudpClient::OpenSocket()
{
	for (int attempt = 0; attempt < 5; attempt++)
	{
		const uint16 port = ReservePort();
		SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = INADDR_ANY;
		local.sin_port = htons(port);
		if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
		{
			ReleasePort(port);
			closesocket(s);
			continue;
		}
#if BOOST_OS_WINDOWS
		u_long nonBlocking = 1;
		ioctlsocket(s, FIONBIO, &nonBlocking);
#else
		fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
		m_socket = s;
		m_localPort = port;
		return true;
	}
	return false;
}

void prudpClient::Transmit(prudpPacket& packet)
{
	uint8 raw[prudpPacket::PACKET_RAW_SIZE_MAX];
	const sint32 size = packet.buildData(raw, sizeof(raw));
	if (size > (sint32)sizeof(raw))
	{
		cemuLog_log(LogType::Force, "PRUDP: Packet exceeds maximum size");
		return;
	}
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(m_serverPort);
	to.sin_addr.s_addr = m_serverIp;
	sendto(m_socket, (const char*)raw, size, 0, (const sockaddr*)&to, sizeof(to));
}

std::vector<uint8> prudpClient::BuildConnectPayload()
{
	std::vector<uint8> payload(4);
	StoreLE<uint32>(payload.data(), m_clientSignature);
	if (!m_secure)
		return payload;
	// secure servers also want the ticket and a proof that we know the session key:
	// rc4(key, pid, cid, nonce) followed by hmac-md5(key, encrypted part)
	uint8 storage[512];
	nexPacketBuffer pb(storage, sizeof(storage), true);
	pb.writeU32(m_clientSignature);
	pb.writeBuffer(m_auth.secureTicket, m_auth.secureTicketLength);
	uint8 proof[12 + 16];
	StoreLE<uint32>(proof + 0, m_auth.userPid);
	StoreLE<uint32>(proof + 4, (uint32)m_auth.server.cid);
	StoreLE<uint32>(proof + 8, RandomU32());
	RC4Ctx rc4;
	RC4_initCtx(&rc4, m_auth.secureKey, 16);
	RC4_transform(&rc4, proof, 12, proof);
	hmacMD5(m_auth.secureKey, 16, proof, 12, proof + 12);
	pb.writeBuffer(proof, sizeof(proof));
	payload.assign(storage, storage + pb.getWriteIndex());
	return payload;
}

// sends SYN until it is acknowledged, then CON
void prudpClient::SendHandshake()
{
	if (!m_gotSynAck)
	{
		prudpPacket syn(&m_settings, m_localVPort, m_serverVPort, prudpPacket::TYPE_SYN, prudpPacket::FLAG_NEED_ACK, 0, 0, 0);
		Transmit(syn);
	}
	else
	{
		prudpPacket con(&m_settings, m_localVPort, m_serverVPort, prudpPacket::TYPE_CON, prudpPacket::FLAG_NEED_ACK | prudpPacket::FLAG_RELIABLE, m_clientSession, 1, m_serverSignature);
		std::vector<uint8> payload = BuildConnectPayload();
		con.setData(payload.data(), (sint32)payload.size());
		Transmit(con);
	}
	m_lastHandshakeSent = prudpGetMSTimestamp();
	m_handshakeSends++;
}

void prudpClient::OnPing(const prudpIncomingPacket& packet)
{
	if (packet.flags & prudpPacket::FLAG_ACK)
	{
		if (packet.flags & prudpPacket::FLAG_NEED_ACK)
			cemuLog_log(LogType::PRUDP, "[PRUDP] Received unexpected ping packet with both ACK and NEED_ACK set");
		if (m_pingsUnanswered == 0)
			cemuLog_log(LogType::PRUDP, "[PRUDP] Received ping packet ACK which we dont need");
		else if (packet.sequenceId != m_pingSeq)
			cemuLog_log(LogType::PRUDP, "[PRUDP] Received ping packet ACK with wrong sequenceId (expected: {}, received: {})", m_pingSeq, packet.sequenceId);
		else
		{
			cemuLog_log(LogType::PRUDP, "[PRUDP] Received ping packet ACK (unacknowledged count: {})", m_pingsUnanswered);
			m_pingsUnanswered = 0;
		}
		return;
	}
	if (packet.flags & prudpPacket::FLAG_NEED_ACK)
	{
		cemuLog_log(LogType::PRUDP, "[PRUDP] Received ping packet with NEED_ACK set. Sending ACK back");
		prudpPacket ack(&m_settings, m_localVPort, m_serverVPort, prudpPacket::TYPE_PING, prudpPacket::FLAG_ACK, m_clientSession, packet.sequenceId, 0);
		Transmit(ack);
	}
}

void prudpClient::OnSynAck(const prudpIncomingPacket& packet)
{
	if (!(packet.flags & prudpPacket::FLAG_ACK))
	{
		cemuLog_log(LogType::Force, "[PRUDP] Received SYN packet without ACK flag set");
		return;
	}
	if (m_gotSynAck || !packet.hasData || packet.packetData.size() != 4)
	{
		cemuLog_log(LogType::PRUDP, "[PRUDP] Received unexpected SYN packet");
		return;
	}
	m_gotSynAck = true;
	m_serverSignature = LoadLE<uint32>(packet.packetData.data());
	m_clientSession = RandomU8();
	m_clientSignature = RandomU32();
	m_handshakeSends = 0;
	SendHandshake();
}

void prudpClient::OnConAck(const prudpIncomingPacket& packet)
{
	if (!m_gotSynAck || m_gotConAck)
	{
		cemuLog_log(LogType::PRUDP, "[PRUDP] Received unexpected CON packet");
		return;
	}
	if (!(packet.flags & prudpPacket::FLAG_ACK))
	{
		cemuLog_log(LogType::Force, "[PRUDP] Received CON packet without ACK flag set");
		return;
	}
	m_gotConAck = true;
	m_handshakeSends = 0;
	m_lastPingSent = prudpGetMSTimestamp(); // the ping interval starts now
	m_serverSession = packet.sessionId;
	m_state = ConnectionState::Connected;
	cemuLog_log(LogType::PRUDP, "[PRUDP] Connection established. ClientSession {:02x} ServerSession {:02x}", m_clientSession, m_serverSession);
}

void prudpClient::OnData(std::unique_ptr<prudpIncomingPacket> packet)
{
	if (packet->flags & prudpPacket::FLAG_ACK)
	{
		// the server confirms one of our packets
		auto it = std::find_if(m_unacked.begin(), m_unacked.end(), [&](const Unacked& u) { return u.packet->GetSequenceId() == packet->sequenceId; });
		if (it != m_unacked.end())
			m_unacked.erase(it);
		if (!packet->packetData.empty())
			cemuLog_log(LogType::PRUDP, "[PRUDP] Received ACK data packet with payload");
		return;
	}
	if (packet->flags & prudpPacket::FLAG_NEED_ACK)
	{
		prudpPacket ack(&m_settings, m_localVPort, m_serverVPort, prudpPacket::TYPE_DATA, prudpPacket::FLAG_ACK, m_clientSession, packet->sequenceId, 0);
		Transmit(ack);
	}
	if (packet->packetData.empty())
		return;
	// sequence ids up to 0x4000 behind the next expected one are old duplicates
	const uint16 ahead = (uint16)(packet->sequenceId - m_nextRecvSeq);
	if (ahead >= 0xC000)
		return;
	for (const auto& queued : m_recvQueue)
	{
		if (queued->sequenceId == packet->sequenceId)
		{
			cemuLog_log(LogType::PRUDP, "Duplicate PRUDP packet received");
			return;
		}
	}
	// keep the queue ordered: insert before the first entry that comes later
	auto pos = m_recvQueue.begin();
	while (pos != m_recvQueue.end() && ((uint16)(packet->sequenceId - (*pos)->sequenceId) & 0x8000) == 0)
		++pos;
	m_recvQueue.insert(pos, std::move(packet));
}

void prudpClient::OnPacket(std::unique_ptr<prudpIncomingPacket> packet)
{
	switch (packet->type)
	{
	case prudpPacket::TYPE_PING:
		OnPing(*packet);
		break;
	case prudpPacket::TYPE_SYN:
		OnSynAck(*packet);
		break;
	case prudpPacket::TYPE_CON:
		OnConAck(*packet);
		break;
	case prudpPacket::TYPE_DATA:
		OnData(std::move(packet));
		break;
	case prudpPacket::TYPE_DISCONNECT:
		m_state = ConnectionState::Disconnected;
		break;
	default:
		cemuLog_log(LogType::PRUDP, "[PRUDP] Received unknown packet type");
		break;
	}
}

void prudpClient::CheckRetransmits(uint32 now)
{
	for (Unacked& u : m_unacked)
	{
		if ((uint32)(now - u.lastSent) < kAckTimeoutMs)
			continue;
		if (u.resends >= kMaxResends)
			m_state = ConnectionState::Disconnected; // the server stopped answering
		Transmit(*u.packet);
		u.lastSent = now;
		u.resends++;
	}
}

// returns false once the handshake gave up
bool prudpClient::CheckHandshakeTimeout(uint32 now)
{
	if ((uint32)(now - m_lastHandshakeSent) < kHandshakeTimeoutMs)
		return true;
	if (m_handshakeSends >= kMaxHandshakeSends)
	{
		m_state = ConnectionState::Disconnected;
		cemuLog_log(LogType::PRUDP, "PRUDP: Failed to connect");
		return false;
	}
	SendHandshake();
	return true;
}

void prudpClient::SendPing()
{
	prudpPacket ping(&m_settings, m_localVPort, m_serverVPort, prudpPacket::TYPE_PING, prudpPacket::FLAG_NEED_ACK, m_clientSession, m_pingSeq, m_serverSignature);
	Transmit(ping);
	m_pingsUnanswered++;
}

// Only one ping is in flight: a new one (with the next sequence id) is started
// every 20 seconds, an unanswered one is repeated every 1.5 seconds.
bool prudpClient::CheckPing(uint32 now)
{
	const uint32 sinceLast = now - m_lastPingSent;
	if (m_pingsUnanswered == 0)
	{
		if (sinceLast < kPingIntervalMs)
			return true;
		cemuLog_log(LogType::PRUDP, "[PRUDP] Sending new ping packet with sequenceId {}", m_pingSeq + 1);
		m_pingSeq++;
	}
	else
	{
		if (sinceLast < kPingRetryMs)
			return true;
		cemuLog_log(LogType::PRUDP, "[PRUDP] Resending ping packet (no ack received)");
		if (m_pingsUnanswered >= kMaxUnansweredPings)
		{
			m_state = ConnectionState::Disconnected;
			cemuLog_log(LogType::PRUDP, "PRUDP: Connection did not receive a ping response in a while. Assuming disconnect");
			return false;
		}
	}
	SendPing();
	m_lastPingSent = now;
	return true;
}

bool prudpClient::Update()
{
	if (m_state == ConnectionState::Disconnected)
		return false;
	const uint32 now = prudpGetMSTimestamp();
	uint8 datagram[4096];
	while (true)
	{
		sockaddr from{};
		socklen_t fromLen = sizeof(from);
		const sint32 received = recvfrom(m_socket, (char*)datagram, sizeof(datagram), 0, &from, &fromLen);
		if (received < 0)
			break;
		// a datagram can hold several packets
		sint32 pos = 0;
		while (pos < received)
		{
			const sint32 size = prudpPacket::calculateSizeFromPacketData(datagram + pos, received - pos);
			if (size <= 0 || pos + size > received)
			{
				cemuLog_log(LogType::Force, "[PRUDP] Invalid packet length");
				break;
			}
			auto packet = std::make_unique<prudpIncomingPacket>(&m_settings, datagram + pos, size);
			pos += size;
			if (packet->hasError())
			{
				cemuLog_log(LogType::Force, "[PRUDP] Packet error");
				break;
			}
			if (packet->type != prudpPacket::TYPE_CON && packet->sessionId != m_serverSession)
			{
				cemuLog_log(LogType::PRUDP, "[PRUDP] Invalid session id");
				continue;
			}
			OnPacket(std::move(packet));
		}
	}
	CheckRetransmits(now);
	if (m_state == ConnectionState::Connecting)
		CheckHandshakeTimeout(now);
	else if (m_state == ConnectionState::Connected)
		CheckPing(now);
	return false;
}

void prudpClient::SendDatagram(uint8* input, sint32 length, bool reliable)
{
	cemu_assert_debug(reliable); // unreliable packets would need their own sequence ids
	cemu_assert_debug(m_gotSynAck && m_gotConAck);
	if (length > prudpPacket::PACKET_RAW_SIZE_MAX - 13)
	{
		cemuLog_logOnce(LogType::Force, "PRUDP: Datagram too long. Fragmentation not implemented yet");
		return;
	}
	const uint16 flags = prudpPacket::FLAG_NEED_ACK | (reliable ? prudpPacket::FLAG_RELIABLE : 0);
	std::unique_ptr<prudpPacket> packet(new prudpPacket(&m_settings, m_localVPort, m_serverVPort, prudpPacket::TYPE_DATA, flags, m_clientSession, m_nextSendSeq, 0));
	if (reliable)
		m_nextSendSeq++;
	packet->setFragmentIndex(0);
	packet->setData(input, length);
	// every data packet asks for an ack, keep it until the ack arrives
	const uint32 now = prudpGetMSTimestamp();
	m_unacked.push_back({ std::move(packet), now, 0 });
	Transmit(*m_unacked.back().packet);
}

sint32 prudpClient::ReceiveDatagram(std::vector<uint8>& outputBuffer)
{
	outputBuffer.clear();
	if (m_recvQueue.empty() || m_recvQueue.front()->sequenceId != m_nextRecvSeq)
		return -1;
	// a message is either one packet with fragment index 0, or packets with
	// fragment indices 1, 2, ... and a final 0, with consecutive sequence ids
	size_t count = 0;
	if (m_recvQueue.front()->fragmentIndex == 0)
		count = 1;
	else
	{
		if (m_recvQueue.front()->fragmentIndex != 1)
			return -1;
		for (size_t n = 1; n < m_recvQueue.size(); n++)
		{
			if (m_recvQueue[n]->sequenceId != (uint16)(m_nextRecvSeq + n))
				return -1;
			if (m_recvQueue[n]->fragmentIndex == 0)
			{
				count = n + 1;
				break;
			}
		}
		if (count == 0)
			return -1;
	}
	// release memory left over from an unusually large message
	constexpr size_t kKeepCapacity = 32 * 1024;
	if (outputBuffer.capacity() > kKeepCapacity)
		std::vector<uint8>().swap(outputBuffer);
	for (size_t n = 0; n < count; n++)
	{
		prudpIncomingPacket& p = *m_recvQueue[n];
		p.decrypt();
		outputBuffer.insert(outputBuffer.end(), p.packetData.begin(), p.packetData.end());
	}
	m_recvQueue.erase(m_recvQueue.begin(), m_recvQueue.begin() + count);
	m_nextRecvSeq = (uint16)(m_nextRecvSeq + count);
	return (sint32)outputBuffer.size();
}
