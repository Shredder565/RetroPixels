// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "prudp.h"
#include "nex.h"
#include "nexThread.h"

#include "util/crypto/md5.h"

#if BOOST_OS_WINDOWS
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#endif

// RPC message layout (little endian):
//   call:   u32 size, u8 protocol|0x80, u32 callId, u32 methodId, parameters
//   answer: u32 size, u8 protocol, u8 ok=1, u32 callId, u32 methodId|0x8000, results
//           u32 size, u8 protocol, u8 ok=0, u32 errorCode, u32 callId
// where size counts the bytes after the size field.

namespace
{
	constexpr uint32 kCallTimeoutMs = 10000;
	constexpr size_t kMaxParamSize = 1024 * 7;

	uint32 g_nextCallId = 1; // shared by all services

	uint32 LoadU32(const uint8* p)
	{
		uint32 v;
		memcpy(&v, p, 4);
		return v;
	}

	bool IsServerCall(const uint8* msg, sint32 length)
	{
		return length >= 5 && (msg[4] & 0x80) != 0;
	}

	// returns the number of bytes used, 0 if the message is malformed
	sint32 ParseAnswer(uint8* msg, sint32 length, nexServiceResponse_t& out)
	{
		if (length < 4)
			return 0;
		const uint32 bodySize = LoadU32(msg);
		const sint32 available = length - 4;
		if (bodySize > (uint32)available || bodySize < 6)
			return 0;
		out.protocolId = msg[4] & 0x7F;
		if (msg[5] == 0)
		{
			if (available < 10)
				return 0;
			out.isSuccessful = false;
			out.errorCode = LoadU32(msg + 6);
			out.callId = LoadU32(msg + 10);
			out.methodId = 0xFFFFFFFF; // matches any method of that call id
		}
		else
		{
			if (bodySize < 10)
				return 0;
			out.isSuccessful = true;
			out.errorCode = 0;
			out.callId = LoadU32(msg + 6);
			out.methodId = LoadU32(msg + 10) & 0x7FFF;
			out.data = nexPacketBuffer(msg + 14, (sint32)bodySize - 10, false);
		}
		return (sint32)bodySize + 4;
	}

	sint32 ParseServerCall(uint8* msg, sint32 length, nexServiceRequest_t& out)
	{
		if (length < 4)
			return 0;
		const uint32 totalSize = LoadU32(msg) + 4;
		if (totalSize > (uint32)length || totalSize < 13)
			return 0;
		out.protocolId = msg[4] & 0x7F;
		out.callId = LoadU32(msg + 5);
		out.methodId = LoadU32(msg + 9);
		out.data = nexPacketBuffer(msg + 13, (sint32)totalSize - 13, false);
		return (sint32)totalSize;
	}
}

// --- nexService ---

nexService::nexService() = default;

nexService::nexService(prudpClient* con) : nexService()
{
	if (!con->IsConnected())
		cemu_assert_suspicious();
	m_connection = con;
	m_receiveBuffer.resize(4096);
}

nexService::nexService(uint32 ip, uint16 port, const char* accessKey) : nexService()
{
	m_connection = new prudpClient(ip, port, accessKey);
	m_receiveBuffer.resize(4096);
}

nexService::~nexService()
{
	// calls still waiting for an answer are reported as timed out
	for (SentCall& sent : m_sent)
		FailCall(sent.call, ERR_TIMEOUT);
	delete m_connection;
}

void nexService::destroy()
{
	if (nexThread_isCurrentThread() || !m_ownedByNexThread)
	{
		delete this;
		return;
	}
	m_destroyRequested = true;
}

bool nexService::isMarkedForDestruction()
{
	return m_destroyRequested;
}

void nexService::Enqueue(PendingCall&& call)
{
	std::lock_guard lock(m_queueMutex);
	m_queue.emplace_back(std::move(call));
}

void nexService::callMethod(uint8 protocolId, uint32 methodId, nexPacketBuffer* parameter, void(*nexServiceResponse)(nexService* nex, nexServiceResponse_t* serviceResponse), void* custom, bool callHandlerIfError)
{
	PendingCall call{};
	call.protocolId = protocolId;
	call.methodId = methodId;
	call.params.assign(parameter->getDataPtr(), parameter->getDataPtr() + parameter->getWriteIndex());
	call.handlerWantsErrors = callHandlerIfError;
	call.legacyHandler = nexServiceResponse;
	call.legacyCustom = custom;
	Enqueue(std::move(call));
}

void nexService::callMethod(uint8 protocolId, uint32 methodId, nexPacketBuffer* parameter, std::function<void(nexServiceResponse_t*)> cb, bool callHandlerIfError)
{
	PendingCall call{};
	call.protocolId = protocolId;
	call.methodId = methodId;
	call.params.assign(parameter->getDataPtr(), parameter->getDataPtr() + parameter->getWriteIndex());
	call.handlerWantsErrors = callHandlerIfError;
	call.handler = std::move(cb);
	Enqueue(std::move(call));
}

void nexService::Deliver(PendingCall& call, nexServiceResponse_t* response)
{
	response->custom = call.legacyCustom;
	if (call.legacyHandler)
		call.legacyHandler(this, response);
	else
		call.handler(response);
}

void nexService::FailCall(PendingCall& call, uint32 errorCode)
{
	nexServiceResponse_t response{};
	response.isSuccessful = false;
	response.errorCode = errorCode;
	Deliver(call, &response);
}

void nexService::SendPending(PendingCall& call)
{
	const uint32 callId = g_nextCallId++;
	if (m_connection->GetConnectionState() != prudpClient::ConnectionState::Connected)
	{
		FailCall(call, ERR_NO_CONNECTION);
		return;
	}
	if (call.params.size() >= kMaxParamSize)
	{
		cemuLog_log(LogType::Force, "NEX: RPC parameters too large ({} bytes)", call.params.size());
		FailCall(call, ERR_NO_CONNECTION);
		return;
	}
	std::vector<uint8> msg(13 + call.params.size());
	const uint32 bodySize = (uint32)msg.size() - 4;
	memcpy(msg.data() + 0, &bodySize, 4);
	msg[4] = call.protocolId | PROTOCOL_BIT_REQUEST;
	memcpy(msg.data() + 5, &callId, 4);
	memcpy(msg.data() + 9, &call.methodId, 4);
	if (!call.params.empty())
		memcpy(msg.data() + 13, call.params.data(), call.params.size());
	m_connection->SendDatagram(msg.data(), (sint32)msg.size(), true);
	m_sent.push_back({ std::move(call), callId, prudpGetMSTimestamp() });
}

void nexService::update()
{
	if (m_state == STATE_CONNECTED)
	{
		PollConnected();
		std::vector<PendingCall> work;
		{
			std::lock_guard lock(m_queueMutex);
			work.swap(m_queue);
		}
		for (PendingCall& call : work)
			SendPending(call);
	}
	else if (m_state == STATE_CONNECTING)
	{
		PollHandshake();
	}
	ExpireCalls();
}

void nexService::ExpireCalls()
{
	const uint32 now = prudpGetMSTimestamp();
	for (size_t i = 0; i < m_sent.size();)
	{
		if ((uint32)(now - m_sent[i].sentAt) < kCallTimeoutMs)
		{
			i++;
			continue;
		}
		SentCall expired = std::move(m_sent[i]);
		m_sent.erase(m_sent.begin() + i);
		FailCall(expired.call, ERR_TIMEOUT);
	}
}

prudpClient* nexService::getPRUDPConnection()
{
	return m_connection;
}

sint32 nexService::getState()
{
	return m_state;
}

void nexService::registerForAsyncProcessing()
{
	if (m_ownedByNexThread)
		return;
	nexThread_registerService(this);
	m_ownedByNexThread = true;
}

void nexService::PollHandshake()
{
	m_connection->Update();
	if (m_connection->IsConnected() && m_state == STATE_CONNECTING)
		m_state = STATE_CONNECTED;
	if (m_connection->GetConnectionState() == prudpClient::ConnectionState::Disconnected)
		m_state = STATE_DISCONNECTED;
}

void nexService::registerProtocolRequestHandler(uint8 protocol, void(*processRequest)(nexServiceRequest_t* request), void* custom)
{
	m_handlers.push_back({ protocol, processRequest, custom });
}

void nexService::sendRequestResponse(nexServiceRequest_t* request, uint32 errorCode, uint8* data, sint32 length)
{
	cemu_assert_debug(length == 0); // answers with result data are not needed yet
	uint8 storage[32];
	nexPacketBuffer answer(storage, sizeof(storage), true);
	answer.writeU32(0); // size, patched below
	answer.writeU8(request->protocolId & 0x7F);
	if (errorCode == 0)
	{
		answer.writeU8(1);
		answer.writeU32(request->callId);
		answer.writeU32(request->methodId);
	}
	else
	{
		answer.writeU8(0);
		answer.writeU32(errorCode);
		answer.writeU32(request->callId);
	}
	const uint32 bodySize = (uint32)answer.getWriteIndex() - 4;
	memcpy(storage, &bodySize, 4);
	if (request->nex->m_connection)
		request->nex->m_connection->SendDatagram(storage, answer.getWriteIndex(), true);
}

void nexService::PollConnected()
{
	if (m_connection->GetConnectionState() == prudpClient::ConnectionState::Disconnected)
	{
		m_state = STATE_DISCONNECTED;
		return;
	}
	m_connection->Update();
	// one message per update
	const sint32 length = m_connection->ReceiveDatagram(m_receiveBuffer);
	if (length > 0)
		HandleIncoming(length);
}

void nexService::HandleIncoming(sint32 length)
{
	uint8* msg = m_receiveBuffer.data();
	if (IsServerCall(msg, length))
	{
		nexServiceRequest_t request{};
		if (ParseServerCall(msg, length, request) == 0)
			return;
		request.nex = this;
		for (const ServerHandler& h : m_handlers)
		{
			if (h.protocolId != request.protocolId)
				continue;
			request.custom = h.custom;
			h.handler(&request);
			break;
		}
		return;
	}
	nexServiceResponse_t response{};
	if (ParseAnswer(msg, length, response) == 0)
		return;
	response.nex = this;
	for (size_t i = 0; i < m_sent.size(); i++)
	{
		SentCall& sent = m_sent[i];
		if (sent.callId != response.callId || sent.call.protocolId != response.protocolId)
			continue;
		if (response.methodId != sent.call.methodId && response.methodId != 0xFFFFFFFF)
			continue;
		SentCall answered = std::move(sent);
		m_sent.erase(m_sent.begin() + i);
		if (response.isSuccessful || answered.call.handlerWantsErrors)
			Deliver(answered.call, &response);
		break;
	}
}

// --- login ---

namespace
{
	// value of "name=value" inside a station url such as
	// prudps:/address=1.2.3.4;port=60000;CID=1;PID=2;sid=1;stream=10;type=2
	bool StationUrlValue(const char* url, const char* name, char* out, sint32 outSize)
	{
		const size_t nameLen = strlen(name);
		for (const char* hit = strstr(url, name); hit; hit = strstr(hit + 1, name))
		{
			if (hit[nameLen] != '=')
				continue;
			const char* value = hit + nameLen + 1;
			out[outSize - 1] = '\0';
			for (sint32 i = 0; i < outSize - 1; i++)
			{
				if (value[i] == ';' || value[i] == '\0')
				{
					out[i] = '\0';
					break;
				}
				out[i] = value[i];
			}
			return true;
		}
		return false;
	}

	void ParseStationUrl(const char* url, prudpStationUrl* station)
	{
		memset(station, 0, sizeof(prudpStationUrl));
		char value[128];
		if (StationUrlValue(url, "address", value, sizeof(value)))
			inet_pton(AF_INET, value, &station->ip);
		struct { const char* name; sint32 prudpStationUrl::* field; } const intFields[] = {
			{ "CID", &prudpStationUrl::cid }, { "PID", &prudpStationUrl::pid }, { "sid", &prudpStationUrl::sid },
			{ "stream", &prudpStationUrl::stream }, { "type", &prudpStationUrl::type },
		};
		if (StationUrlValue(url, "port", value, sizeof(value)))
			station->port = (uint16)atoi(value);
		for (const auto& f : intFields)
			if (StationUrlValue(url, f.name, value, sizeof(value)))
				station->*(f.field) = atoi(value);
	}

	struct AuthProgress
	{
		uint32 pid;
		prudpStationUrl secureServer;
		std::vector<uint8> ticket;
		bool failed;
		bool finished;
	};

	void OnTicketAnswer(nexService* nex, nexServiceResponse_t* response)
	{
		AuthProgress* auth = (AuthProgress*)response->custom;
		if (!response->isSuccessful)
		{
			cemuLog_log(LogType::Force, "NEX: RPC error while requesting auth ticket with error code 0x{:08x}", response->errorCode);
			auth->failed = true;
			return;
		}
		const uint32 result = response->data.readU32();
		if (result & 0x80000000)
		{
			cemuLog_log(LogType::Force, "NEX: Failed to request auth ticket with error code 0x{:08x}", result);
			auth->failed = true;
		}
		auth->ticket.resize(4096);
		auth->ticket.resize(response->data.readBuffer(auth->ticket.data(), (sint32)auth->ticket.size()));
		if (response->data.hasReadOutOfBounds())
		{
			auth->failed = true;
			cemuLog_log(LogType::Force, "NEX: Out of bounds error while reading auth ticket");
			return;
		}
		auth->finished = true;
	}

	void OnLoginAnswer(nexService* nex, nexServiceResponse_t* response)
	{
		AuthProgress* auth = (AuthProgress*)response->custom;
		if (!response->isSuccessful)
		{
			auth->failed = true;
			cemuLog_log(LogType::Force, "NEX: RPC error in login response 0x{:08x}", response->errorCode);
			return;
		}
		nexPacketBuffer& pb = response->data;
		const uint32 result = pb.readU32();
		if (result & 0x80000000)
		{
			auth->failed = true;
			cemuLog_log(LogType::Force, "NEX: Error 0x{:08x} in login response (returnCode 0x{:08x})", response->errorCode, result);
			return;
		}
		pb.readU32(); // our pid
		uint8 unusedTicket[1024];
		pb.readBuffer(unusedTicket, sizeof(unusedTicket));
		// connection data: the secure server url, a list of special protocols
		// (expected to be empty), a special url and the server build name
		char url[1024];
		pb.readString(url, sizeof(url));
		ParseStationUrl(url, &auth->secureServer);
		if (pb.readU32() != 0)
			cemuLog_log(LogType::Force, "NEX: Login response lists special protocols, ignoring them");
		char unused[256];
		pb.readString(unused, 32);
		pb.readString(unused, sizeof(unused));
		if (pb.hasReadOutOfBounds())
		{
			auth->failed = true;
			cemuLog_log(LogType::Force, "NEX: Read out of bounds");
			return;
		}
		// next step: ticket for the secure server
		uint8 storage[1024];
		nexPacketBuffer params(storage, sizeof(storage), true);
		params.writeU32(auth->pid);
		params.writeU32(auth->secureServer.pid);
		nex->callMethod(NEX_PROTOCOL_AUTHENTICATION, 3, &params, OnTicketAnswer, auth, true);
	}

	struct RegisterProgress
	{
		bool done;
		bool ok;
	};

	void OnRegisterAnswer(nexService* nex, nexServiceResponse_t* response)
	{
		RegisterProgress* reg = (RegisterProgress*)response->custom;
		reg->done = true;
		reg->ok = false;
		const uint32 result = response->data.readU32();
		if (!response->isSuccessful || response->data.hasReadOutOfBounds())
		{
			cemuLog_log(LogType::Force, "NEX: RPC error in secure register");
			return;
		}
		if (result & 0x80000000)
		{
			cemuLog_log(LogType::Force, "NEX: Secure register failed with error code 0x{:08x}", result);
			return;
		}
		reg->ok = true; // the rest of the answer is not needed
	}

	// key that protects the ticket: md5 of the password, hashed again 65000 + (pid % 1024) - 1 times
	void DeriveTicketKey(const char* password, uint32 pid, uint8 key[16])
	{
		MD5_CTX ctx;
		MD5_Init(&ctx);
		MD5_Update(&ctx, password, (unsigned long)strlen(password));
		MD5_Final(key, &ctx);
		const uint32 rounds = 65000 + (pid % 1024) - 1;
		for (uint32 i = 0; i < rounds; i++)
		{
			MD5_Init(&ctx);
			MD5_Update(&ctx, key, 16);
			MD5_Final(key, &ctx);
		}
	}

	nexService* ConnectSecure(prudpAuthServerInfo* info, const char* accessKey, const char* nexToken)
	{
		prudpClient* connection = new prudpClient(info->server.ip, info->server.port, accessKey, info);
		while (true)
		{
			connection->Update();
			if (connection->IsConnected())
				break;
			if (connection->GetConnectionState() == prudpClient::ConnectionState::Disconnected)
			{
				cemuLog_log(LogType::Force, "NEX: Secure login connection time-out");
				delete connection;
				return nullptr;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		nexService* nex = new nexService(connection);
		// RegisterEx: our station url and the login token
		uint8 storage[4096];
		nexPacketBuffer params(storage, sizeof(storage), true);
		const std::string myUrl = fmt::format("prudp:/port={};natf=0;natm=0;pmp=0;sid=15;type=2;upnp=0", (uint32)connection->GetSourcePort());
		params.writeU32(1);
		params.writeString(myUrl.c_str());
		params.writeCustomType(nexNintendoLoginData(nexToken));
		RegisterProgress reg{};
		nex->callMethod(NEX_PROTOCOL_SECURE, 4, &params, OnRegisterAnswer, &reg, true);
		while (!reg.done)
		{
			nex->update();
			if (reg.done)
				break;
			if (nex->getState() == nexService::STATE_DISCONNECTED)
			{
				cemuLog_log(LogType::Force, "NEX: Connection error while registering");
				break;
			}
		}
		if (!reg.ok)
		{
			cemuLog_log(LogType::Force, "NEX: Failed to register to secure server");
			nex->destroy();
			return nullptr;
		}
		return nex;
	}
}

nexService* nex_establishSecureConnection(uint32 authServerIp, uint16 authServerPort, const char* accessKey, uint32 pid, const char* nexPassword, const char* nexToken)
{
	// step 1: log into the authentication server and get a ticket for the secure server
	nexService* authServer = new nexService(authServerIp, authServerPort, accessKey);
	while (authServer->getState() == nexService::STATE_CONNECTING)
	{
		authServer->update();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (authServer->getState() != nexService::STATE_CONNECTED)
	{
		authServer->destroy();
		cemuLog_log(LogType::Force, "NEX: Failed to connect to the NEX server");
		return nullptr;
	}
	uint8 storage[1024];
	nexPacketBuffer params(storage, sizeof(storage), true);
	params.writeString(fmt::format("{}", pid).c_str());
	AuthProgress auth{};
	auth.pid = pid;
	authServer->callMethod(NEX_PROTOCOL_AUTHENTICATION, 1, &params, OnLoginAnswer, &auth, true);
	while (!auth.failed && !auth.finished && authServer->getState() == nexService::STATE_CONNECTED)
		authServer->update();
	const bool authOk = !auth.failed && authServer->getState() == nexService::STATE_CONNECTED;
	authServer->destroy();
	if (!authOk)
	{
		cemuLog_log(LogType::Force, "NEX: Error during authentication");
		return nullptr;
	}

	// step 2: check and decrypt the ticket. Layout: encrypted part, then hmac-md5 of it
	uint8 ticketKey[16];
	DeriveTicketKey(nexPassword, pid, ticketKey);
	if (auth.ticket.size() < 16)
	{
		cemuLog_log(LogType::Force, "NEX: Kerberos ticket too short");
		return nullptr;
	}
	const sint32 sealedSize = (sint32)auth.ticket.size() - 16;
	uint8 mac[16];
	hmacMD5(ticketKey, 16, auth.ticket.data(), sealedSize, mac);
	if (memcmp(mac, auth.ticket.data() + sealedSize, 16) != 0)
	{
		cemuLog_log(LogType::Force, "NEX: Kerberos ticket hmac invalid");
		return nullptr;
	}
	RC4Ctx rc4;
	RC4_initCtx(&rc4, ticketKey, 16);
	RC4_transform(&rc4, auth.ticket.data(), sealedSize, auth.ticket.data());
	// decrypted: session key (16 bytes for the friend server), u32, buffer with the ticket for the secure server
	auto info = std::make_unique<prudpAuthServerInfo>();
	nexPacketBuffer ticket(auth.ticket.data(), sealedSize, false);
	uint8 sessionKey[16]{};
	ticket.readData(sessionKey, 16);
	ticket.readU32();
	info->secureTicketLength = ticket.readBuffer(info->secureTicket, sizeof(info->secureTicket));
	if (ticket.hasReadOutOfBounds())
	{
		cemuLog_log(LogType::Force, "NEX: Parse error");
		return nullptr;
	}
	memcpy(info->kerberosKey, ticketKey, 16);
	memcpy(info->secureKey, sessionKey, 16);
	info->server = auth.secureServer;
	info->userPid = pid;

	// step 3: connect to the secure server with the ticket
	return ConnectSecure(info.get(), accessKey, nexToken);
}
